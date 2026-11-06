#include "admin_server.h"
#include <sddl.h>
#include <iostream>
#include <algorithm>
#include <limits>
#include "../third_party/nlohmann/json.hpp"
#include "../core/strategy/input_policy_json.h"

namespace ime {

static std::wstring GetCurrentUserSid() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (len == 0) {
        CloseHandle(hToken);
        return L"";
    }
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(hToken, TokenUser, buf.data(), len, &len)) {
        CloseHandle(hToken);
        return L"";
    }
    CloseHandle(hToken);

    auto* pUser = reinterpret_cast<TOKEN_USER*>(buf.data());
    LPWSTR pSidStr = nullptr;
    if (ConvertSidToStringSidW(pUser->User.Sid, &pSidStr)) {
        std::wstring sid(pSidStr);
        LocalFree(pSidStr);
        return sid;
    }
    return L"";
}

AdminServer::AdminServer(ImeEngine& engine, std::wstring pipeName)
    : m_engine(engine)
    , m_pipeName(std::move(pipeName)) {
}

AdminServer::~AdminServer() {
    Stop();
}

bool AdminServer::Start() {
    if (m_running.load()) return true;

    std::wstring userSid = GetCurrentUserSid();
    if (userSid.empty()) {
        std::wcerr << L"[AdminServer] Failed to query user SID. Aborting to fail closed.\n";
        return false;
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) return false;

    m_running.store(true);
    std::promise<bool> readyPromise;
    auto readyFuture = readyPromise.get_future();
    m_listenerThread = std::thread(&AdminServer::ListenerThread, this, std::move(readyPromise));

    bool ready = false;
    try {
        ready = readyFuture.get();
    } catch (...) {
        ready = false;
    }

    if (!ready) {
        Stop();
        return false;
    }

    return true;
}

void AdminServer::Stop() {
    if (!m_running.exchange(false)) return;

    if (m_stopEvent) {
        SetEvent(m_stopEvent);
    }

    // Cancel active client pipe I/O and disconnect to wake up threads immediately (< 1500ms)
    {
        std::lock_guard<std::mutex> lock(m_pipesMutex);
        for (HANDLE h : m_activeClientPipes) {
            CancelIoEx(h, nullptr);
            DisconnectNamedPipe(h);
        }
    }

    if (m_listenerThread.joinable()) {
        m_listenerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        for (auto& w : m_clientWorkers) {
            if (w.thread.joinable()) {
                w.thread.join();
            }
        }
        m_clientWorkers.clear();
    }

    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

void AdminServer::CleanUpFinishedWorkersLocked() {
    for (auto it = m_clientWorkers.begin(); it != m_clientWorkers.end(); ) {
        if (it->done->load()) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = m_clientWorkers.erase(it);
        } else {
            ++it;
        }
    }
}

void AdminServer::ListenerThread(std::promise<bool> readyPromise) {
    std::wstring userSid = GetCurrentUserSid();
    if (userSid.empty()) {
        readyPromise.set_value(false);
        return; // Fail closed
    }

    // Strictly limit Admin Pipe to Current User SID, Builtin Admins (BA) and Local System (SY)
    // AppContainer (AC) is strictly excluded to block untrusted sandbox access
    std::wstring sddl = L"D:(A;;GA;;;" + userSid + L")(A;;GA;;;BA)(A;;GA;;;SY)";

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &pSD,
            nullptr)) {
        readyPromise.set_value(false);
        return;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    bool promiseDelivered = false;

    while (m_running.load()) {
        {
            std::lock_guard<std::mutex> lock(m_threadsMutex);
            CleanUpFinishedWorkersLocked();
        }

        HANDLE hPipe = CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            4, // Maximum 4 concurrent admin connections
            65536,
            65536,
            ADMIN_DEFAULT_TIMEOUT_MS,
            &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (!promiseDelivered) {
                readyPromise.set_value(false);
                promiseDelivered = true;
            }
            if (err == ERROR_PIPE_BUSY) {
                // All 4 pipe instances are currently connected. Wait briefly and retry.
                WaitForSingleObject(m_stopEvent, 50);
                continue;
            }
            break;
        }

        if (!promiseDelivered) {
            readyPromise.set_value(true);
            promiseDelivered = true;
        }

        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(hPipe, &ol);
        bool ioPending = false;
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                ioPending = true;
                HANDLE handles[2] = { m_stopEvent, ol.hEvent };
                while (m_running.load()) {
                    {
                        std::lock_guard<std::mutex> lock(m_threadsMutex);
                        CleanUpFinishedWorkersLocked();
                    }
                    DWORD waitRes = WaitForMultipleObjects(2, handles, FALSE, 50);
                    if (waitRes == WAIT_OBJECT_0) {
                        break;
                    } else if (waitRes == WAIT_OBJECT_0 + 1) {
                        DWORD dummy = 0;
                        if (GetOverlappedResult(hPipe, &ol, &dummy, FALSE)) {
                            connected = TRUE;
                        }
                        ioPending = false;
                        break;
                    } else if (waitRes == WAIT_TIMEOUT) {
                        continue;
                    } else {
                        break;
                    }
                }
                if (ioPending) {
                    CancelIoEx(hPipe, &ol);
                    DWORD dummy = 0;
                    GetOverlappedResult(hPipe, &ol, &dummy, TRUE);
                    ioPending = false;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(ol.hEvent);
                CloseHandle(hPipe);
                continue;
            } else {
                connected = TRUE;
            }
        }
        CloseHandle(ol.hEvent);

        if (!m_running.load()) {
            CloseHandle(hPipe);
            break;
        }

        auto doneFlag = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        CleanUpFinishedWorkersLocked();
        std::thread th([this, hPipe, doneFlag]() {
            this->ClientHandlerThread(hPipe);
            doneFlag->store(true);
        });
        m_clientWorkers.push_back({ std::move(th), doneFlag });
    }

    if (!promiseDelivered) {
        readyPromise.set_value(false);
    }

    if (pSD) {
        LocalFree(pSD);
    }
}

static void DrainOrDisconnect(HANDLE hPipe, DWORD timeoutMs, HANDLE hStopEvent) {
    if (hPipe == INVALID_HANDLE_VALUE) return;
    std::atomic<bool> flushed{false};
    std::thread flusher([hPipe, &flushed]() {
        FlushFileBuffers(hPipe);
        flushed.store(true);
    });

    ULONGLONG start = GetTickCount64();
    while (!flushed.load()) {
        if (GetTickCount64() - start >= timeoutMs ||
            (hStopEvent && WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0)) {
            // This helper owns the connection shutdown. Disconnect also wakes a
            // synchronous flush, rather than depending on overlapped cancellation.
            DisconnectNamedPipe(hPipe);
            break;
        }
        Sleep(5);
    }
    if (flusher.joinable()) {
        flusher.join();
    }
}

static std::string MakeErrorResponse(const std::string& action, uint32_t reqId, AdminErrorCode code, const std::string& message);

static bool SendFrame(HANDLE hPipe, uint32_t requestId, const std::string& payload, ULONGLONG deadlineTick = 0) {
    if (payload.size() > MAX_ADMIN_PAYLOAD_SIZE) {
        return SendFrame(hPipe, requestId,
            MakeErrorResponse("Unknown", requestId, AdminErrorCode::PayloadTooLarge,
                "Response exceeds 16MB; request a smaller result set"), deadlineTick);
    }
    AdminHeader header{};
    header.magic[0] = 'P';
    header.magic[1] = 'I';
    header.magic[2] = 'M';
    header.magic[3] = 'A';
    header.version = ADMIN_PROTOCOL_VERSION_1;
    header.payload_length = static_cast<uint32_t>(payload.size());
    header.request_id = requestId;

    std::vector<uint8_t> frame(sizeof(AdminHeader) + payload.size());
    std::memcpy(frame.data(), &header, sizeof(AdminHeader));
    if (!payload.empty()) {
        std::memcpy(frame.data() + sizeof(AdminHeader), payload.data(), payload.size());
    }

    if (deadlineTick == 0) {
        deadlineTick = GetTickCount64() + ADMIN_DEFAULT_TIMEOUT_MS;
    }

    DWORD totalWritten = 0;
    DWORD totalBytes = static_cast<DWORD>(frame.size());

    while (totalWritten < totalBytes) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadlineTick) {
            return false;
        }
        DWORD remainingMs = static_cast<DWORD>(deadlineTick - now);

        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return false;

        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, frame.data() + totalWritten, totalBytes - totalWritten, &written, &ol);
        if (!ok) {
            if (GetLastError() == ERROR_IO_PENDING) {
                DWORD waitRes = WaitForSingleObject(ol.hEvent, remainingMs);
                if (waitRes == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(hPipe, &ol, &written, FALSE);
                } else {
                    CancelIoEx(hPipe, &ol);
                    DWORD dummy = 0;
                    GetOverlappedResult(hPipe, &ol, &dummy, TRUE);
                    CloseHandle(ol.hEvent);
                    return false;
                }
            } else {
                CloseHandle(ol.hEvent);
                return false;
            }
        }
        CloseHandle(ol.hEvent);
        if (!ok || written == 0) return false;
        totalWritten += written;
    }

    return true;
}

static std::string MakeErrorResponse(const std::string& action, uint32_t reqId, AdminErrorCode code, const std::string& message) {
    nlohmann::json resp;
    resp["action"] = action;
    resp["request_id"] = reqId;
    resp["success"] = false;
    resp["error"] = {
        {"code", static_cast<int>(code)},
        {"message", message}
    };
    return resp.dump();
}

static std::string MakeSuccessResponse(const std::string& action, uint32_t reqId, const nlohmann::json& data) {
    nlohmann::json resp;
    resp["action"] = action;
    resp["request_id"] = reqId;
    resp["success"] = true;
    resp["data"] = data;
    return resp.dump();
}

static bool ReadExactWithDeadline(HANDLE hPipe, void* buffer, DWORD bytesToRead, ULONGLONG deadlineTick, HANDLE hStopEvent) {
    BYTE* ptr = reinterpret_cast<BYTE*>(buffer);
    DWORD total = 0;
    while (total < bytesToRead) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadlineTick) {
            return false;
        }
        DWORD remainingMs = static_cast<DWORD>(deadlineTick - now);

        OVERLAPPED ol{};
        ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return false;

        DWORD read = 0;
        BOOL ok = ReadFile(hPipe, ptr + total, bytesToRead - total, &read, &ol);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waitHandles[2] = { hStopEvent, ol.hEvent };
                DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, remainingMs);
                if (waitRes == WAIT_OBJECT_0) {
                    CancelIoEx(hPipe, &ol);
                    DWORD dummy = 0;
                    GetOverlappedResult(hPipe, &ol, &dummy, TRUE);
                    CloseHandle(ol.hEvent);
                    return false;
                } else if (waitRes == WAIT_OBJECT_0 + 1) {
                    if (!GetOverlappedResult(hPipe, &ol, &read, FALSE) || read == 0) {
                        CloseHandle(ol.hEvent);
                        return false;
                    }
                } else {
                    CancelIoEx(hPipe, &ol);
                    DWORD dummy = 0;
                    GetOverlappedResult(hPipe, &ol, &dummy, TRUE);
                    CloseHandle(ol.hEvent);
                    return false;
                }
            } else {
                CloseHandle(ol.hEvent);
                return false;
            }
        }
        CloseHandle(ol.hEvent);
        if (read == 0) return false;
        total += read;
    }
    return true;
}

void AdminServer::ClientHandlerThread(HANDLE hPipe) {
    {
        std::lock_guard<std::mutex> lock(m_pipesMutex);
        m_activeClientPipes.push_back(hPipe);
    }

    while (m_running.load()) {
        ULONGLONG frameDeadline = GetTickCount64() + ADMIN_DEFAULT_TIMEOUT_MS;
        AdminHeader header{};

        // Read 16-byte fixed header with absolute deadline
        if (!ReadExactWithDeadline(hPipe, &header, sizeof(header), frameDeadline, m_stopEvent)) {
            break; // Client disconnected, timed out, or stop event signaled
        }

        // Validate Magic
        if (header.magic[0] != 'P' || header.magic[1] != 'I' || header.magic[2] != 'M' || header.magic[3] != 'A') {
            SendFrame(hPipe, header.request_id, MakeErrorResponse("Unknown", header.request_id, AdminErrorCode::InvalidMagic, "Invalid protocol magic; expected 'PIMA'"), frameDeadline);
            break;
        }

        // Validate Protocol Version
        if (header.version != ADMIN_PROTOCOL_VERSION_1) {
            SendFrame(hPipe, header.request_id, MakeErrorResponse("Unknown", header.request_id, AdminErrorCode::UnsupportedVersion, "Unsupported protocol version; expected 0x0100"), frameDeadline);
            break;
        }

        // Validate Maximum Frame Size
        if (header.payload_length > MAX_ADMIN_PAYLOAD_SIZE) {
            SendFrame(hPipe, header.request_id, MakeErrorResponse("Unknown", header.request_id, AdminErrorCode::PayloadTooLarge, "Payload exceeds 16MB limit"), frameDeadline);
            break;
        }

        // Read JSON payload with the same frame deadline
        std::string payloadJson(header.payload_length, '\0');
        if (header.payload_length > 0) {
            if (!ReadExactWithDeadline(hPipe, payloadJson.data(), header.payload_length, frameDeadline, m_stopEvent)) {
                break;
            }
        }

        std::string respJson = HandleRequest(payloadJson, header.request_id);
        if (!SendFrame(hPipe, header.request_id, respJson, frameDeadline)) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_pipesMutex);
        auto it = std::find(m_activeClientPipes.begin(), m_activeClientPipes.end(), hPipe);
        if (it != m_activeClientPipes.end()) {
            m_activeClientPipes.erase(it);
        }
    }

    DrainOrDisconnect(hPipe, 200, m_stopEvent);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

static bool CheckStringField(const nlohmann::json& obj, const char* key, std::string& out, bool required, std::string& err) {
    if (!obj.contains(key)) {
        if (required) {
            err = std::string("Field '") + key + "' is required";
            return false;
        }
        out = "";
        return true;
    }
    const auto& v = obj[key];
    if (!v.is_string()) {
        err = std::string("Field '") + key + "' must be a string";
        return false;
    }
    out = v.get<std::string>();
    if (required && out.empty()) {
        err = std::string("Field '") + key + "' cannot be empty";
        return false;
    }
    return true;
}

static bool CheckIntField(const nlohmann::json& obj, const char* key, int& out, bool required, int def, std::string& err) {
    if (!obj.contains(key)) {
        if (required) {
            err = std::string("Field '") + key + "' is required";
            return false;
        }
        out = def;
        return true;
    }
    const auto& v = obj[key];
    if (!v.is_number_integer()) {
        err = std::string("Field '") + key + "' must be an integer";
        return false;
    }
    if (v.is_number_unsigned()) {
        auto value = v.get<uint64_t>();
        if (value > static_cast<uint64_t>((std::numeric_limits<int>::max)())) {
            err = std::string("Field '") + key + "' is outside the integer range";
            return false;
        }
        out = static_cast<int>(value);
    } else {
        auto value = v.get<int64_t>();
        if (value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)()) {
            err = std::string("Field '") + key + "' is outside the integer range";
            return false;
        }
        out = static_cast<int>(value);
    }
    return true;
}

static bool CheckBoolField(const nlohmann::json& obj, const char* key, bool& out, bool required, bool def, std::string& err) {
    if (!obj.contains(key)) {
        if (required) {
            err = std::string("Field '") + key + "' is required";
            return false;
        }
        out = def;
        return true;
    }
    const auto& v = obj[key];
    if (!v.is_boolean()) {
        err = std::string("Field '") + key + "' must be a boolean";
        return false;
    }
    out = v.get<bool>();
    return true;
}

std::string AdminServer::HandleRequest(const std::string& requestJson, uint32_t requestId) {
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(requestJson);
    } catch (const std::exception& e) {
        return MakeErrorResponse("Unknown", requestId, AdminErrorCode::InvalidJson, std::string("Malformed JSON payload: ") + e.what());
    }

    if (!req.is_object()) {
        return MakeErrorResponse("Unknown", requestId, AdminErrorCode::ValidationFailed, "Top-level JSON payload must be an object");
    }

    if (!req.contains("action") || !req["action"].is_string()) {
        return MakeErrorResponse("Unknown", requestId, AdminErrorCode::MissingParameter, "Field 'action' is required and must be a string");
    }

    std::string action = req["action"].get<std::string>();
    if (action.empty()) {
        return MakeErrorResponse("Unknown", requestId, AdminErrorCode::MissingParameter, "Field 'action' cannot be empty");
    }

    nlohmann::json params = nlohmann::json::object();
    if (req.contains("params")) {
        if (!req["params"].is_object()) {
            return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'params' must be an object");
        }
        params = req["params"];
    }

    try {
        // 1. GetStatus
        if (action == "GetStatus" || action == "status" || action == "get_status") {
            nlohmann::json data;
            data["service_version"] = "1.0.0";
            data["protocol_version"] = static_cast<int>(ADMIN_PROTOCOL_VERSION_1);
            auto p = m_engine.GetPolicy();
            data["active_policy_revision"] = p ? p->revision : 1;
            data["db_path"] = m_engine.GetDb().GetPath();
            data["user_entry_count"] = m_engine.GetRepo() ? m_engine.GetRepo()->GetAllEntries().size() : 0;
            data["wubi_entry_count"] = m_engine.GetBuiltinDict().WubiEntryCount();
            data["pinyin_entry_count"] = m_engine.GetBuiltinDict().PinyinEntryCount();
            data["has_pending_commits"] = m_engine.GetRepo() ? m_engine.GetRepo()->HasAnyPendingCommits() : false;

            {
                std::lock_guard<std::mutex> lock(m_pipesMutex);
                data["active_clients"] = m_activeClientPipes.size();
            }

            std::error_code ec;
            auto fsize = std::filesystem::file_size(m_engine.GetDb().GetPath(), ec);
            data["db_size_bytes"] = ec ? 0 : fsize;

            return MakeSuccessResponse(action, requestId, data);
        }

        // 2. GetPolicy
        if (action == "GetPolicy" || action == "get_policy" || action == "policy.get") {
            auto p = m_engine.GetPolicy();
            if (!p) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Policy coordinator unavailable");
            }
            nlohmann::json data;
            data["policy"] = InputPolicyToJson(*p);
            return MakeSuccessResponse(action, requestId, data);
        }

        // 3. SavePolicy
        if (action == "SavePolicy" || action == "save_policy" || action == "policy.set") {
            if (!params.contains("policy") || !params["policy"].is_object()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::MissingParameter, "Field 'policy' object is required");
            }

            std::optional<uint64_t> expectedRev;
            if (params.contains("expected_revision")) {
                if (!params["expected_revision"].is_number_integer() && !params["expected_revision"].is_number_unsigned()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'expected_revision' must be an integer");
                }
                expectedRev = params["expected_revision"].get<uint64_t>();
            }

            auto currentPolicy = m_engine.GetPolicy();
            InputPolicy newPolicy = currentPolicy ? *currentPolicy : InputPolicy{};
            std::string err;
            if (!InputPolicyFromJson(params["policy"], newPolicy, &err)) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
            }

            uint64_t allocatedRev = 0;
            bool isConflict = false;
            if (!m_engine.SavePolicy(newPolicy, expectedRev, &err, &allocatedRev, &isConflict)) {
                AdminErrorCode code = isConflict ? AdminErrorCode::RevisionConflict : AdminErrorCode::ValidationFailed;
                return MakeErrorResponse(action, requestId, code, err);
            }

            nlohmann::json data;
            data["revision"] = allocatedRev;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 4. GetEntries / QueryEntries
        if (action == "GetEntries" || action == "query_entries" || action == "get_entries") {
            if (!m_engine.GetRepo()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Repository unavailable");
            }

            std::string keyword, schemeFilter, err;
            if (!CheckStringField(params, "keyword", keyword, false, err) ||
                !CheckStringField(params, "scheme", schemeFilter, false, err)) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
            }

            int page = 0, pageSize = 50;
            if (!CheckIntField(params, "page", page, false, 0, err) ||
                !CheckIntField(params, "page_size", pageSize, false, 50, err)) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
            }
            if (page < 0 || pageSize < 1 || pageSize > 500) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed,
                    "page must be nonnegative and page_size must be between 1 and 500");
            }

            InputScheme reqScheme = InputScheme::Unknown;
            if (!schemeFilter.empty()) {
                reqScheme = StringToInputScheme(schemeFilter, InputScheme::Unknown);
                if (reqScheme == InputScheme::Unknown) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + schemeFilter);
                }
            }

            auto allEntries = m_engine.GetRepo()->GetAllEntries();
            std::vector<Entry> filtered;

            for (const auto& e : allEntries) {
                bool matches = true;
                if (!keyword.empty()) {
                    std::string textUtf8 = WideToUtf8(e.text);
                    bool hasCodeMatch = false;
                    for (const auto& c : e.codes) {
                        if (c.second.find(keyword) != std::string::npos) {
                            hasCodeMatch = true;
                            break;
                        }
                    }
                    if (textUtf8.find(keyword) == std::string::npos && !hasCodeMatch) {
                        matches = false;
                    }
                }

                if (matches && !schemeFilter.empty()) {
                    bool hasScheme = false;
                    for (const auto& c : e.codes) {
                        if (c.first == reqScheme) {
                            hasScheme = true;
                            break;
                        }
                    }
                    if (!hasScheme) matches = false;
                }

                if (matches) {
                    filtered.push_back(e);
                }
            }

            size_t total = filtered.size();
            size_t start = static_cast<size_t>(page) * pageSize;
            nlohmann::json entriesArr = nlohmann::json::array();

            if (start < total) {
                size_t count = (std::min)(static_cast<size_t>(pageSize), total - start);
                for (size_t i = 0; i < count; ++i) {
                    const auto& e = filtered[start + i];
                    nlohmann::json item;
                    item["id"] = e.id;
                    item["text"] = WideToUtf8(e.text);
                    item["source"] = static_cast<int>(e.source);
                    item["created_at"] = e.created_at;
                    item["updated_at"] = e.updated_at;

                    nlohmann::json codesArr = nlohmann::json::array();
                    for (const auto& c : e.codes) {
                        codesArr.push_back({
                            {"scheme", InputSchemeToString(c.first)},
                            {"code", c.second}
                        });
                    }
                    item["codes"] = codesArr;
                    entriesArr.push_back(item);
                }
            }

            nlohmann::json data;
            data["total"] = total;
            data["page"] = page;
            data["page_size"] = pageSize;
            data["entries"] = entriesArr;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 5. AddEntry
        if (action == "AddEntry" || action == "add_entry") {
            std::string text, err;
            if (!CheckStringField(params, "text", text, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("text") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }

            if (!params.contains("codes") || !params["codes"].is_array() || params["codes"].empty()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::MissingParameter, "Field 'codes' must be a non-empty array");
            }

            std::vector<std::pair<InputScheme, std::string>> parsedCodes;
            for (const auto& item : params["codes"]) {
                if (!item.is_object() || !item.contains("scheme") || !item["scheme"].is_string() ||
                    !item.contains("code") || !item["code"].is_string()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Each code entry must have string 'scheme' and 'code'");
                }
                std::string sStr = item["scheme"].get<std::string>();
                std::string cStr = item["code"].get<std::string>();
                if (cStr.empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Code string cannot be empty");
                }
                InputScheme sch = StringToInputScheme(sStr, InputScheme::Unknown);
                if (sch == InputScheme::Unknown) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + sStr);
                }
                parsedCodes.emplace_back(sch, cStr);
            }

            std::string id = m_engine.AddUserEntry(Utf8ToWide(text), parsedCodes);
            if (id.empty()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Failed to insert user entry");
            }

            nlohmann::json data;
            data["entry_id"] = id;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 6. UpdateEntry
        if (action == "UpdateEntry" || action == "update_entry") {
            std::string entryId, err;
            if (!CheckStringField(params, "entry_id", entryId, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("entry_id") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }

            if (m_engine.GetRepo() && m_engine.GetRepo()->HasPendingCommitsForEntry(entryId)) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::Busy, "Entry has unconfirmed pending commits in flight; cannot update");
            }

            auto existing = m_engine.GetUserEntry(entryId);
            if (!existing) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::EntryNotFound, "Entry not found: " + entryId);
            }

            std::string text = WideToUtf8(existing->text);
            if (params.contains("text")) {
                if (!CheckStringField(params, "text", text, true, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            std::vector<std::pair<InputScheme, std::string>> parsedCodes;
            if (params.contains("codes")) {
                if (!params["codes"].is_array()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'codes' must be an array");
                }
                for (const auto& item : params["codes"]) {
                    if (!item.is_object() || !item.contains("scheme") || !item["scheme"].is_string() ||
                        !item.contains("code") || !item["code"].is_string()) {
                        return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Each code entry must have string 'scheme' and 'code'");
                    }
                    std::string sStr = item["scheme"].get<std::string>();
                    std::string cStr = item["code"].get<std::string>();
                    if (cStr.empty()) {
                        return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Code string cannot be empty");
                    }
                    InputScheme sch = StringToInputScheme(sStr, InputScheme::Unknown);
                    if (sch == InputScheme::Unknown) {
                        return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + sStr);
                    }
                    parsedCodes.emplace_back(sch, cStr);
                }
            } else {
                parsedCodes = existing->codes;
            }

            bool ok = m_engine.EditUserEntry(entryId, Utf8ToWide(text), parsedCodes);
            if (!ok) {
                if (m_engine.GetRepo() && m_engine.GetRepo()->HasPendingCommitsForEntry(entryId)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::Busy, "Entry has unconfirmed pending commits in flight; cannot update");
                }
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Failed to update entry");
            }

            nlohmann::json data;
            data["success"] = true;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 7. DeleteEntry
        if (action == "DeleteEntry" || action == "delete_entry") {
            std::string entryId, err;
            if (!CheckStringField(params, "entry_id", entryId, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("entry_id") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }

            if (m_engine.GetRepo() && m_engine.GetRepo()->HasPendingCommitsForEntry(entryId)) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::Busy, "Entry has unconfirmed pending commits in flight; cannot delete");
            }

            auto existing = m_engine.GetUserEntry(entryId);
            if (!existing) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::EntryNotFound, "Entry not found: " + entryId);
            }

            bool ok = m_engine.DeleteUserEntry(entryId);
            if (!ok) {
                if (m_engine.GetRepo() && m_engine.GetRepo()->HasPendingCommitsForEntry(entryId)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::Busy, "Entry has unconfirmed pending commits in flight; cannot delete");
                }
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Failed to delete entry");
            }

            nlohmann::json data;
            data["success"] = true;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 8. GetPinned
        if (action == "GetPinned" || action == "get_pinned") {
            std::string schemeStr, code, layoutId, err;
            if (!CheckStringField(params, "scheme", schemeStr, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("scheme") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (!CheckStringField(params, "code", code, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("code") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (params.contains("layout_id")) {
                if (!CheckStringField(params, "layout_id", layoutId, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            InputScheme sch = StringToInputScheme(schemeStr, InputScheme::Unknown);
            if (sch == InputScheme::Unknown) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + schemeStr);
            }
            if (sch == InputScheme::Shuangpin) {
                if (layoutId.empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Missing layout_id for shuangpin scheme");
                }
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            } else if (!layoutId.empty()) {
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            }

            if (!m_engine.GetRepo()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Repository unavailable");
            }

            std::string scopedCode = InputCoordinator::BuildScopedCode(sch, layoutId, code);
            std::transform(scopedCode.begin(), scopedCode.end(), scopedCode.begin(), [](unsigned char c) { return std::tolower(c); });

            uint64_t rev = m_engine.GetRepo()->GetPinnedRevision(sch, scopedCode);
            auto prefs = m_engine.GetRepo()->GetPinnedPreferences(sch, scopedCode);

            nlohmann::json entriesArr = nlohmann::json::array();
            for (const auto& pref : prefs) {
                if (!pref.is_pinned) continue;
                nlohmann::json item;
                item["entry_id"] = pref.entry_id;
                item["order_rank"] = pref.order_rank;

                std::string textUtf8;
                auto userEntry = m_engine.GetRepo()->GetEntry(pref.entry_id);
                if (userEntry && !userEntry->text.empty()) {
                    textUtf8 = WideToUtf8(userEntry->text);
                } else {
                    auto textOpt = m_engine.GetBuiltinDict().GetTextById(pref.entry_id);
                    if (textOpt) {
                        textUtf8 = WideToUtf8(*textOpt);
                    }
                }
                item["text"] = textUtf8;
                entriesArr.push_back(item);
            }

            nlohmann::json data;
            data["scheme"] = InputSchemeToString(sch);
            data["code"] = code;
            data["layout_id"] = layoutId;
            data["revision"] = rev;
            data["entries"] = entriesArr;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 9. SetPin
        if (action == "SetPin" || action == "set_pin") {
            std::string schemeStr, code, entryId, layoutId, err;
            if (params.contains("scheme")) {
                if (!CheckStringField(params, "scheme", schemeStr, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }
            if (!CheckStringField(params, "code", code, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("code") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (!CheckStringField(params, "entry_id", entryId, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("entry_id") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (params.contains("layout_id")) {
                if (!CheckStringField(params, "layout_id", layoutId, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            bool isPinned = true;
            if (params.contains("is_pinned")) {
                if (!CheckBoolField(params, "is_pinned", isPinned, false, true, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            int rank = 1;
            if (params.contains("order_rank")) {
                if (!CheckIntField(params, "order_rank", rank, false, 1, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
                if (rank < 1) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'order_rank' must be at least 1");
                }
            }

            std::optional<uint64_t> expectedRev;
            if (params.contains("expected_revision")) {
                if (!params["expected_revision"].is_number_integer() && !params["expected_revision"].is_number_unsigned()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'expected_revision' must be an integer");
                }
                expectedRev = params["expected_revision"].get<uint64_t>();
            }

            InputScheme sch = InputScheme::Wubi86;
            if (!schemeStr.empty()) {
                sch = StringToInputScheme(schemeStr, InputScheme::Unknown);
                if (sch == InputScheme::Unknown) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + schemeStr);
                }
            }
            if (sch == InputScheme::Shuangpin) {
                if (layoutId.empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Missing layout_id for shuangpin scheme");
                }
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            } else if (!layoutId.empty()) {
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            }

            if (isPinned && m_engine.GetCoordinator()) {
                if (!m_engine.GetCoordinator()->IsEntryValidForScope(sch, layoutId, code, entryId)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed,
                        "Entry does not belong to target query scope: " + entryId);
                }
            }

            uint64_t outRev = 0;
            bool isConflict = false;
            bool ok = false;
            if (m_engine.GetCoordinator()) {
                if (isPinned) {
                    ok = m_engine.GetCoordinator()->SetPinPreference(sch, code, entryId, true, rank, layoutId, expectedRev, &outRev, &isConflict);
                } else {
                    ok = m_engine.GetCoordinator()->UnpinPreference(sch, code, entryId, layoutId, expectedRev, &outRev, &isConflict);
                }
            }

            if (!ok) {
                if (isConflict) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::RevisionConflict, "Pinned order revision conflict");
                }
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Failed to set pin preference");
            }

            nlohmann::json data;
            data["success"] = true;
            data["revision"] = outRev;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 10. ReorderPinned
        if (action == "ReorderPinned" || action == "reorder_pinned") {
            std::string schemeStr, code, layoutId, err;
            if (params.contains("scheme")) {
                if (!CheckStringField(params, "scheme", schemeStr, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }
            if (!CheckStringField(params, "code", code, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("code") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (params.contains("layout_id")) {
                if (!CheckStringField(params, "layout_id", layoutId, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            if (!params.contains("entry_ids") || !params["entry_ids"].is_array()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::MissingParameter, "Field 'entry_ids' must be an array");
            }

            InputScheme sch = InputScheme::Wubi86;
            if (!schemeStr.empty()) {
                sch = StringToInputScheme(schemeStr, InputScheme::Unknown);
                if (sch == InputScheme::Unknown) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + schemeStr);
                }
            }
            if (sch == InputScheme::Shuangpin) {
                if (layoutId.empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Missing layout_id for shuangpin scheme");
                }
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            } else if (!layoutId.empty()) {
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            }

            std::vector<std::string> entryIds;
            std::unordered_set<std::string> seenIds;
            for (const auto& item : params["entry_ids"]) {
                if (!item.is_string() || item.get<std::string>().empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Each entry_id in entry_ids must be a non-empty string");
                }
                std::string eid = item.get<std::string>();
                if (!seenIds.insert(eid).second) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Duplicate entry_id in entry_ids: " + eid);
                }
                if (m_engine.GetCoordinator() && !m_engine.GetCoordinator()->IsEntryValidForScope(sch, layoutId, code, eid)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed,
                        "Entry does not belong to target query scope: " + eid);
                }
                entryIds.push_back(eid);
            }

            std::optional<uint64_t> expectedRev;
            if (params.contains("expected_revision")) {
                if (!params["expected_revision"].is_number_integer() && !params["expected_revision"].is_number_unsigned()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'expected_revision' must be an integer");
                }
                expectedRev = params["expected_revision"].get<uint64_t>();
            }

            std::string scopedCode = InputCoordinator::BuildScopedCode(sch, layoutId, code);

            uint64_t outRev = 0;
            bool isConflict = false;
            bool ok = false;
            if (m_engine.GetRepo()) {
                ok = m_engine.GetRepo()->ReorderPinnedEntries(sch, scopedCode, entryIds, expectedRev, &outRev, &isConflict);
            }

            if (!ok) {
                if (isConflict) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::RevisionConflict, "Pinned order revision conflict");
                }
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Failed to reorder pinned entries");
            }

            nlohmann::json data;
            data["success"] = true;
            data["revision"] = outRev;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 11. GetScopeCandidates
        if (action == "GetScopeCandidates" || action == "get_scope_candidates") {
            std::string schemeStr, code, layoutId, keyword, err;
            if (!CheckStringField(params, "scheme", schemeStr, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("scheme") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (!CheckStringField(params, "code", code, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("code") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (params.contains("layout_id")) {
                if (!CheckStringField(params, "layout_id", layoutId, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }
            if (params.contains("keyword")) {
                if (!CheckStringField(params, "keyword", keyword, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            InputScheme sch = StringToInputScheme(schemeStr, InputScheme::Unknown);
            if (sch == InputScheme::Unknown) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown scheme: " + schemeStr);
            }
            if (sch == InputScheme::Shuangpin) {
                if (layoutId.empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Missing layout_id for shuangpin scheme");
                }
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            } else if (!layoutId.empty()) {
                ShuangpinLayout lay;
                if (!TryParseShuangpinLayout(layoutId, lay)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Unknown layout_id: " + layoutId);
                }
                layoutId = ShuangpinLayoutToString(lay);
            }

            if (!m_engine.GetCoordinator()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Coordinator unavailable");
            }

            auto cands = m_engine.GetCoordinator()->QueryScopedCandidates(sch, layoutId, code, keyword);
            nlohmann::json candArr = nlohmann::json::array();
            for (const auto& c : cands) {
                nlohmann::json item;
                item["entry_id"] = c.entry_id;
                item["text"] = WideToUtf8(c.text);
                item["matched_code"] = c.matched_code;
                item["raw_code"] = c.raw_code;
                item["dict_id"] = c.dict_id;
                candArr.push_back(item);
            }

            nlohmann::json data;
            data["candidates"] = candArr;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 10. ResetStats
        if (action == "ResetStats" || action == "reset_stats") {
            std::string entryId;
            if (params.contains("entry_id")) {
                if (!params["entry_id"].is_string()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'entry_id' must be a string");
                }
                entryId = params["entry_id"].get<std::string>();
            }

            bool ok = false;
            if (entryId.empty()) {
                ok = m_engine.ResetAllStats();
            } else {
                ok = m_engine.ResetStats(entryId);
            }

            if (!ok) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Failed to reset stats");
            }

            nlohmann::json data;
            data["success"] = true;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 11. ExportData
        if (action == "ExportData" || action == "export_data") {
            std::string filePath, err;
            if (!CheckStringField(params, "file_path", filePath, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("file_path") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }

            bool ok = m_engine.ExportData(filePath, &err);
            if (!ok) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Export failed: " + err);
            }

            nlohmann::json data;
            data["success"] = true;
            data["file_path"] = filePath;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 12. ImportData
        if (action == "ImportData" || action == "import_data") {
            std::string filePath, err;
            if (!CheckStringField(params, "file_path", filePath, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("file_path") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }

            bool replaceMode = false;
            if (params.contains("replace_mode")) {
                if (!CheckBoolField(params, "replace_mode", replaceMode, false, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
            }

            if (params.contains("mode")) {
                std::string modeStr;
                if (!CheckStringField(params, "mode", modeStr, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
                if (modeStr == "replace") {
                    replaceMode = true;
                } else if (modeStr == "merge") {
                    replaceMode = false;
                } else if (!modeStr.empty()) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Invalid import mode; expected 'merge' or 'replace'");
                }
            }

            if (replaceMode && m_engine.GetRepo() && m_engine.GetRepo()->HasAnyPendingCommits()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::Busy,
                    "Cannot replace database while unconfirmed pending commits are in flight");
            }

            bool ok = m_engine.ImportData(filePath, replaceMode, &err);
            if (!ok) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Import failed: " + err);
            }

            nlohmann::json data;
            data["success"] = true;
            return MakeSuccessResponse(action, requestId, data);
        }

        // 13. ReverseLookup (Stage 4C-2)
        if (action == "ReverseLookup" || action == "reverse_lookup" || action == "get_reverse_lookup") {
            std::string textStr, err;
            if (!CheckStringField(params, "text", textStr, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("text") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }
            if (textStr.empty()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'text' cannot be empty");
            }
            std::wstring textWide = Utf8ToWide(textStr);
            if (textWide.empty()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Field 'text' could not be decoded as valid UTF-8");
            }

            std::string targetMethodStr;
            if (!CheckStringField(params, "target_method", targetMethodStr, true, err)) {
                return MakeErrorResponse(action, requestId, params.contains("target_method") ? AdminErrorCode::ValidationFailed : AdminErrorCode::MissingParameter, err);
            }

            InputScheme targetScheme = InputScheme::Unknown;
            if (!TryParseInputScheme(targetMethodStr, targetScheme) ||
                (targetScheme != InputScheme::Wubi86 && targetScheme != InputScheme::Pinyin && targetScheme != InputScheme::Shuangpin)) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Invalid target_method for reverse lookup: " + targetMethodStr);
            }

            ShuangpinLayout spLayout = ShuangpinLayout::Natural;
            if (params.contains("shuangpin_layout")) {
                std::string spStr;
                if (!CheckStringField(params, "shuangpin_layout", spStr, false, err)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, err);
                }
                if (!TryParseShuangpinLayout(spStr, spLayout)) {
                    return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, "Invalid shuangpin_layout: " + spStr);
                }
            } else if (auto snap = m_engine.GetCoordinator() ? m_engine.GetCoordinator()->GetPolicySnapshot() : nullptr) {
                spLayout = snap->reverse_lookup.shuangpin_layout;
            }

            if (!m_engine.GetCoordinator()) {
                return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, "Engine coordinator unavailable");
            }

            auto rl = m_engine.GetCoordinator()->ReverseLookup(textWide, targetScheme, spLayout);

            nlohmann::json data;
            data["found"] = rl.found;
            data["text"] = textStr;
            data["target_method"] = InputSchemeToString(targetScheme);
            data["is_phrase"] = rl.is_phrase;
            data["code"] = rl.primary_code;
            data["primary_code"] = rl.primary_code;
            data["all_codes"] = rl.all_codes;
            data["char_codes"] = rl.char_codes;

            return MakeSuccessResponse(action, requestId, data);
        }

        return MakeErrorResponse(action, requestId, AdminErrorCode::UnknownAction, "Unknown action: " + action);
    } catch (const nlohmann::json::exception& e) {
        return MakeErrorResponse(action, requestId, AdminErrorCode::ValidationFailed, std::string("JSON parameter type error: ") + e.what());
    } catch (const std::exception& e) {
        return MakeErrorResponse(action, requestId, AdminErrorCode::StorageError, std::string("Internal error: ") + e.what());
    }
}

} // namespace ime

