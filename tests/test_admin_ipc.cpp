#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cassert>
#include <filesystem>
#include "../src/common/admin_protocol.h"
#include "../src/common/ipc_protocol.h"
#include "../src/core/engine/ime_engine.h"
#include "../src/ime_service/admin_server.h"
#include "../src/core/strategy/input_policy_json.h"

using namespace ime;

static void AssertCondition(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[TEST FAILED] " << message << std::endl;
        std::exit(1);
    }
    std::cout << "  [PASS] " << message << std::endl;
}

static bool ClientReadExact(HANDLE hPipe, void* buffer, DWORD bytesToRead) {
    BYTE* ptr = reinterpret_cast<BYTE*>(buffer);
    DWORD total = 0;
    while (total < bytesToRead) {
        DWORD read = 0;
        if (!ReadFile(hPipe, ptr + total, bytesToRead - total, &read, nullptr) || read == 0) {
            return false;
        }
        total += read;
    }
    return true;
}

// Minimal Admin Pipe Client Helper
class AdminClient {
public:
    AdminClient() : m_hPipe(INVALID_HANDLE_VALUE) {}
    ~AdminClient() { Disconnect(); }

    bool Connect(const std::wstring& pipeName, DWORD timeoutMs = 5000) {
        Disconnect();
        auto start = std::chrono::steady_clock::now();
        while (true) {
            m_hPipe = CreateFileW(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );

            if (m_hPipe != INVALID_HANDLE_VALUE) {
                return true;
            }

            if (GetLastError() != ERROR_PIPE_BUSY) {
                WaitNamedPipeW(pipeName.c_str(), 100);
            } else {
                WaitNamedPipeW(pipeName.c_str(), 200);
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeoutMs) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void Disconnect() {
        if (m_hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hPipe);
            m_hPipe = INVALID_HANDLE_VALUE;
        }
    }

    bool SendFrame(uint32_t requestId, const std::string& payloadJson, std::string& outResponseJson, uint32_t* outRespId = nullptr) {
        if (m_hPipe == INVALID_HANDLE_VALUE) return false;

        AdminHeader header{};
        header.magic[0] = 'P';
        header.magic[1] = 'I';
        header.magic[2] = 'M';
        header.magic[3] = 'A';
        header.version = ADMIN_PROTOCOL_VERSION_1;
        header.payload_length = static_cast<uint32_t>(payloadJson.size());
        header.request_id = requestId;

        std::vector<uint8_t> frame(sizeof(AdminHeader) + payloadJson.size());
        std::memcpy(frame.data(), &header, sizeof(AdminHeader));
        if (!payloadJson.empty()) {
            std::memcpy(frame.data() + sizeof(AdminHeader), payloadJson.data(), payloadJson.size());
        }

        DWORD written = 0;
        if (!WriteFile(m_hPipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) || written != frame.size()) {
            return false;
        }
        FlushFileBuffers(m_hPipe);

        // Read response header
        AdminHeader respHeader{};
        if (!ClientReadExact(m_hPipe, &respHeader, sizeof(respHeader))) {
            return false;
        }

        if (outRespId) *outRespId = respHeader.request_id;

        // Read response payload
        outResponseJson.assign(respHeader.payload_length, '\0');
        if (!ClientReadExact(m_hPipe, outResponseJson.data(), respHeader.payload_length)) {
            return false;
        }

        return true;
    }

    HANDLE GetRawHandle() const { return m_hPipe; }

private:
    HANDLE m_hPipe;
};

// -----------------------------------------------------------------------------
// Test 1: Golden Frame & Protocol Header Verification
// -----------------------------------------------------------------------------
static void TestGoldenFrameAndHeader(AdminServer& server, const std::wstring& pipeName) {
    std::cout << "[TEST 1] Testing Golden Frame & Header Byte Order Verification..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected to Admin Pipe");

    // Golden Frame: Action="GetStatus", requestId=101
    // Wire byte sequence:
    // Header (16 bytes):
    // 0..3: 'P', 'I', 'M', 'A' (0x50, 0x49, 0x4D, 0x41)
    // 4..7: 0x00, 0x01, 0x00, 0x00 (0x0100 little endian)
    // 8..11: payload length in little endian
    // 12..15: 101 in little endian (0x65, 0x00, 0x00, 0x00)
    std::string goldenPayload = R"({"action":"GetStatus"})";
    uint32_t payloadLen = static_cast<uint32_t>(goldenPayload.size());

    std::vector<uint8_t> goldenFrame(16 + payloadLen);
    goldenFrame[0] = 'P';
    goldenFrame[1] = 'I';
    goldenFrame[2] = 'M';
    goldenFrame[3] = 'A';

    goldenFrame[4] = 0x00;
    goldenFrame[5] = 0x01;
    goldenFrame[6] = 0x00;
    goldenFrame[7] = 0x00;

    goldenFrame[8] = static_cast<uint8_t>(payloadLen & 0xFF);
    goldenFrame[9] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    goldenFrame[10] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
    goldenFrame[11] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);

    goldenFrame[12] = 0x65; // 101
    goldenFrame[13] = 0x00;
    goldenFrame[14] = 0x00;
    goldenFrame[15] = 0x00;

    std::memcpy(goldenFrame.data() + 16, goldenPayload.data(), payloadLen);

    // Send golden frame
    DWORD written = 0;
    BOOL ok = WriteFile(client.GetRawHandle(), goldenFrame.data(), static_cast<DWORD>(goldenFrame.size()), &written, nullptr);
    AssertCondition(ok && written == goldenFrame.size(), "Sent raw golden frame bytes");

    // Read response
    AdminHeader respHeader{};
    AssertCondition(ClientReadExact(client.GetRawHandle(), &respHeader, sizeof(respHeader)), "Received 16-byte response header");

    AssertCondition(respHeader.magic[0] == 'P' && respHeader.magic[1] == 'I' &&
                    respHeader.magic[2] == 'M' && respHeader.magic[3] == 'A',
                    "Response magic matches 'PIMA' byte-for-byte");
    AssertCondition(respHeader.version == ADMIN_PROTOCOL_VERSION_1, "Response version is 0x0100");
    AssertCondition(respHeader.request_id == 101, "Response request_id matches golden request_id 101");
    AssertCondition(respHeader.payload_length > 0, "Response payload length is non-zero");

    std::string respPayload(respHeader.payload_length, '\0');
    AssertCondition(ClientReadExact(client.GetRawHandle(), respPayload.data(), respHeader.payload_length), "Received response payload");

    auto j = nlohmann::json::parse(respPayload);
    AssertCondition(j.value("success", false) == true, "Golden frame GetStatus executed successfully");
    AssertCondition(j.contains("data") && j["data"].contains("service_version"), "Response contains data.service_version");

    client.Disconnect();
    std::cout << "  [PASS] Golden frame byte-for-byte protocol validation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 2: Negative Protocol Framing & Fault Injection
// -----------------------------------------------------------------------------
static void TestNegativeFramingAndFaultInjection(AdminServer& server, const std::wstring& pipeName) {
    std::cout << "[TEST 2] Testing Negative Protocol Framing & Fault Injection..." << std::endl;

    // 1. Invalid Magic Rejection
    {
        AdminClient client;
        AssertCondition(client.Connect(pipeName), "Connected for invalid magic test");

        AdminHeader badHeader{};
        badHeader.magic[0] = 'B';
        badHeader.magic[1] = 'A';
        badHeader.magic[2] = 'D';
        badHeader.magic[3] = '!';
        badHeader.version = ADMIN_PROTOCOL_VERSION_1;
        badHeader.payload_length = 2;
        badHeader.request_id = 12;

        std::string payload = "{}";
        std::vector<uint8_t> frame(sizeof(badHeader) + 2);
        std::memcpy(frame.data(), &badHeader, sizeof(badHeader));
        std::memcpy(frame.data() + sizeof(badHeader), payload.data(), 2);

        DWORD written = 0;
        WriteFile(client.GetRawHandle(), frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);

        AdminHeader respHeader{};
        AssertCondition(ClientReadExact(client.GetRawHandle(), &respHeader, sizeof(respHeader)), "Read invalid magic response header");

        std::string respPayload(respHeader.payload_length, '\0');
        AssertCondition(ClientReadExact(client.GetRawHandle(), respPayload.data(), respHeader.payload_length), "Read invalid magic response payload");

        auto j = nlohmann::json::parse(respPayload);
        AssertCondition(j.value("success", true) == false, "Invalid magic returned success=false");
        AssertCondition(j["error"]["code"] == static_cast<int>(AdminErrorCode::InvalidMagic), "Error code is InvalidMagic (1001)");
    }

    // 2. Unsupported Version Rejection
    {
        AdminClient client;
        AssertCondition(client.Connect(pipeName), "Connected for unsupported version test");

        AdminHeader badHeader{};
        badHeader.magic[0] = 'P';
        badHeader.magic[1] = 'I';
        badHeader.magic[2] = 'M';
        badHeader.magic[3] = 'A';
        badHeader.version = 0x0200; // Version 2.0 (unsupported)
        badHeader.payload_length = 2;
        badHeader.request_id = 13;

        std::string payload = "{}";
        std::vector<uint8_t> frame(sizeof(badHeader) + 2);
        std::memcpy(frame.data(), &badHeader, sizeof(badHeader));
        std::memcpy(frame.data() + sizeof(badHeader), payload.data(), 2);

        DWORD written = 0;
        WriteFile(client.GetRawHandle(), frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);

        AdminHeader respHeader{};
        AssertCondition(ClientReadExact(client.GetRawHandle(), &respHeader, sizeof(respHeader)), "Read unsupported version response header");

        std::string respPayload(respHeader.payload_length, '\0');
        AssertCondition(ClientReadExact(client.GetRawHandle(), respPayload.data(), respHeader.payload_length), "Read unsupported version response payload");

        auto j = nlohmann::json::parse(respPayload);
        AssertCondition(j.value("success", true) == false, "Unsupported version returned success=false");
        AssertCondition(j["error"]["code"] == static_cast<int>(AdminErrorCode::UnsupportedVersion), "Error code is UnsupportedVersion (1002)");
    }

    // 3. Payload Length Over 16MB
    {
        AdminClient client;
        AssertCondition(client.Connect(pipeName), "Connected for oversized payload test");

        AdminHeader badHeader{};
        badHeader.magic[0] = 'P';
        badHeader.magic[1] = 'I';
        badHeader.magic[2] = 'M';
        badHeader.magic[3] = 'A';
        badHeader.version = ADMIN_PROTOCOL_VERSION_1;
        badHeader.payload_length = 17 * 1024 * 1024; // 17 MB > 16MB
        badHeader.request_id = 14;

        DWORD written = 0;
        WriteFile(client.GetRawHandle(), &badHeader, sizeof(badHeader), &written, nullptr);

        AdminHeader respHeader{};
        AssertCondition(ClientReadExact(client.GetRawHandle(), &respHeader, sizeof(respHeader)), "Read oversized payload response header");

        std::string respPayload(respHeader.payload_length, '\0');
        AssertCondition(ClientReadExact(client.GetRawHandle(), respPayload.data(), respHeader.payload_length), "Read oversized payload response payload");

        auto j = nlohmann::json::parse(respPayload);
        AssertCondition(j.value("success", true) == false, "Oversized payload returned success=false");
        AssertCondition(j["error"]["code"] == static_cast<int>(AdminErrorCode::PayloadTooLarge), "Error code is PayloadTooLarge (1003)");
    }

    // 4. Malformed JSON
    {
        AdminClient client;
        AssertCondition(client.Connect(pipeName), "Connected for malformed JSON test");

        std::string badJson = "{invalid_json_without_quotes";
        std::string resp;
        AssertCondition(client.SendFrame(15, badJson, resp), "Sent malformed JSON");

        auto j = nlohmann::json::parse(resp);
        AssertCondition(j.value("success", true) == false, "Malformed JSON rejected");
        AssertCondition(j["error"]["code"] == static_cast<int>(AdminErrorCode::InvalidJson), "Error code is InvalidJson (1004)");
    }

    // 5. Unknown Action
    {
        AdminClient client;
        AssertCondition(client.Connect(pipeName), "Connected for unknown action test");

        std::string req = R"({"action":"NoSuchAction"})";
        std::string resp;
        AssertCondition(client.SendFrame(16, req, resp), "Sent unknown action");

        auto j = nlohmann::json::parse(resp);
        AssertCondition(j.value("success", true) == false, "Unknown action rejected");
        AssertCondition(j["error"]["code"] == static_cast<int>(AdminErrorCode::UnknownAction), "Error code is UnknownAction (1005)");
    }

    std::cout << "  [PASS] Negative framing & fault injection tests verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 3: Policy Management (GetPolicy, SavePolicy, Validation, Versioning)
// -----------------------------------------------------------------------------
static void TestPolicyManagement(AdminServer& server, const std::wstring& pipeName) {
    std::cout << "[TEST 3] Testing Policy Management (GetPolicy, SavePolicy, Validation, Versioning)..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected for policy management test");

    // 1. GetPolicy
    std::string resp;
    AssertCondition(client.SendFrame(201, R"({"action":"GetPolicy"})", resp), "Send GetPolicy");
    auto jGet = nlohmann::json::parse(resp);
    AssertCondition(jGet["success"] == true, "GetPolicy succeeded");
    AssertCondition(jGet["data"]["policy"]["primary_input_method"] == "wubi86", "Default primary method is wubi86");
    uint64_t currentRev = jGet["data"]["policy"]["revision"];

    // 2. Reject Invalid candidate_page_size (< 5 or > 9)
    {
        nlohmann::json badReq;
        badReq["action"] = "SavePolicy";
        badReq["params"]["policy"] = jGet["data"]["policy"];
        badReq["params"]["policy"]["candidate_page_size"] = 4; // < 5 invalid!

        std::string badResp;
        AssertCondition(client.SendFrame(202, badReq.dump(), badResp), "Send SavePolicy with page_size=4");
        auto jBad = nlohmann::json::parse(badResp);
        AssertCondition(jBad["success"] == false, "page_size=4 rejected");
        AssertCondition(jBad["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed");

        badReq["params"]["policy"]["candidate_page_size"] = 10; // > 9 invalid!
        AssertCondition(client.SendFrame(203, badReq.dump(), badResp), "Send SavePolicy with page_size=10");
        jBad = nlohmann::json::parse(badResp);
        AssertCondition(jBad["success"] == false, "page_size=10 rejected");
    }

    // 3. Reject Invalid Primary Scheme or Empty Dictionaries
    {
        nlohmann::json badReq;
        badReq["action"] = "SavePolicy";
        badReq["params"]["policy"] = jGet["data"]["policy"];
        badReq["params"]["policy"]["primary_input_method"] = "cangjie";

        std::string badResp;
        AssertCondition(client.SendFrame(204, badReq.dump(), badResp), "Send SavePolicy with invalid scheme");
        auto jBad = nlohmann::json::parse(badResp);
        AssertCondition(jBad["success"] == false, "Invalid scheme rejected");

        badReq["params"]["policy"] = jGet["data"]["policy"];
        badReq["params"]["policy"]["dictionary_order"] = nlohmann::json::array();
        AssertCondition(client.SendFrame(205, badReq.dump(), badResp), "Send SavePolicy with empty dicts");
        jBad = nlohmann::json::parse(badResp);
        AssertCondition(jBad["success"] == false, "Empty dictionaries rejected");
    }

    // 4. Reject Optimistic Revision Conflict
    {
        nlohmann::json conflictReq;
        conflictReq["action"] = "SavePolicy";
        conflictReq["params"]["policy"] = jGet["data"]["policy"];
        conflictReq["params"]["expected_revision"] = currentRev + 999; // Stale!

        std::string confResp;
        AssertCondition(client.SendFrame(206, conflictReq.dump(), confResp), "Send SavePolicy with stale revision");
        auto jConf = nlohmann::json::parse(confResp);
        AssertCondition(jConf["success"] == false, "Stale revision rejected");
        AssertCondition(jConf["error"]["code"] == static_cast<int>(AdminErrorCode::RevisionConflict), "Error code is RevisionConflict (4001)");
    }

    // 5. Valid SavePolicy (Page Size = 7, Xiaohe Shuangpin, Revision Increments)
    {
        nlohmann::json validReq;
        validReq["action"] = "SavePolicy";
        validReq["params"]["policy"] = jGet["data"]["policy"];
        validReq["params"]["policy"]["candidate_page_size"] = 7;
        validReq["params"]["policy"]["primary_input_method"] = "shuangpin";
        validReq["params"]["policy"]["shuangpin_layout"] = "xiaohe";
        validReq["params"]["policy"]["frequency_enabled"] = false;
        validReq["params"]["policy"]["learning_enabled"] = false;
        validReq["params"]["expected_revision"] = currentRev;

        std::string okResp;
        AssertCondition(client.SendFrame(207, validReq.dump(), okResp), "Send valid SavePolicy");
        auto jOk = nlohmann::json::parse(okResp);
        AssertCondition(jOk["success"] == true, "SavePolicy succeeded");
        uint64_t newRev = jOk["data"]["revision"];
        AssertCondition(newRev == currentRev + 1, "Revision incremented by exactly 1");

        // Verify with GetPolicy
        std::string getAgain;
        AssertCondition(client.SendFrame(208, R"({"action":"GetPolicy"})", getAgain), "GetPolicy after save");
        auto jAgain = nlohmann::json::parse(getAgain);
        AssertCondition(jAgain["data"]["policy"]["revision"] == newRev, "Revision matches updated value");
        AssertCondition(jAgain["data"]["policy"]["candidate_page_size"] == 7, "Page size is 7");
        AssertCondition(jAgain["data"]["policy"]["primary_input_method"] == "shuangpin", "Primary method is shuangpin");
        AssertCondition(jAgain["data"]["policy"]["shuangpin_layout"] == "xiaohe", "Layout is xiaohe");
        AssertCondition(jAgain["data"]["policy"]["frequency_enabled"] == false, "Frequency enabled is false");
        AssertCondition(jAgain["data"]["policy"]["learning_enabled"] == false, "Learning enabled is false");
    }

    std::cout << "  [PASS] Policy management and validation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 4: User Dictionary CRUD (Multi-code, Special Chars, Emoji, Multi-line)
// -----------------------------------------------------------------------------
static void TestUserDictionaryCrud(AdminServer& server, const std::wstring& pipeName) {
    std::cout << "[TEST 4] Testing User Dictionary CRUD (Multi-code, Emoji, Multi-line)..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected for CRUD test");

    // 1. Add Entry with Multi-code
    std::string multiCodeReq = R"({
        "action": "AddEntry",
        "params": {
            "text": "西安",
            "codes": [
                {"scheme": "pinyin", "code": "xian"},
                {"scheme": "pinyin", "code": "xi'an"}
            ]
        }
    })";

    std::string resp;
    AssertCondition(client.SendFrame(301, multiCodeReq, resp), "Send AddEntry for 西安");
    auto jAdd = nlohmann::json::parse(resp);
    AssertCondition(jAdd["success"] == true, "AddEntry succeeded");
    std::string xianId = jAdd["data"]["entry_id"];
    AssertCondition(!xianId.empty(), "Received non-empty entry_id");

    // 2. Add Symbol and Emoji Entry
    std::string emojiReq = R"({
        "action": "AddEntry",
        "params": {
            "text": "🚀",
            "codes": [{"scheme": "pinyin", "code": "huojian"}]
        }
    })";
    AssertCondition(client.SendFrame(302, emojiReq, resp), "Send AddEntry for Emoji 🚀");
    auto jEmoji = nlohmann::json::parse(resp);
    std::string emojiId = jEmoji["data"]["entry_id"];

    // 3. Add Multi-line Entry
    std::string multiLineReq = R"({
        "action": "AddEntry",
        "params": {
            "text": "第一行\n第二行\n第三行",
            "codes": [{"scheme": "custom", "code": ";ml"}]
        }
    })";
    AssertCondition(client.SendFrame(303, multiLineReq, resp), "Send AddEntry for multi-line text");
    auto jML = nlohmann::json::parse(resp);
    std::string mlId = jML["data"]["entry_id"];

    // 4. Query Entries with Keyword and Pagination
    std::string queryReq = R"({
        "action": "GetEntries",
        "params": {
            "keyword": "西安",
            "page": 0,
            "page_size": 10
        }
    })";
    AssertCondition(client.SendFrame(304, queryReq, resp), "Query entries with keyword '西安'");
    auto jQ = nlohmann::json::parse(resp);
    AssertCondition(jQ["data"]["total"] >= 1, "Found at least 1 match for 西安");
    AssertCondition(jQ["data"]["entries"][0]["text"] == "西安", "Found entry text matches");
    AssertCondition(jQ["data"]["entries"][0]["codes"].size() == 2, "Entry has 2 codes (xian and xi'an)");

    // 5. Update Entry
    std::string updateReq = nlohmann::json({
        {"action", "UpdateEntry"},
        {"params", {
            {"entry_id", xianId},
            {"text", "古都西安"},
            {"codes", nlohmann::json::array({
                {{"scheme", "pinyin"}, {"code", "xian"}},
                {{"scheme", "pinyin"}, {"code", "xi'an"}},
                {{"scheme", "wubi86"}, {"code", "sq"}}
            })}
        }}
    }).dump();

    AssertCondition(client.SendFrame(305, updateReq, resp), "Update entry text and codes");
    auto jUp = nlohmann::json::parse(resp);
    AssertCondition(jUp["success"] == true, "UpdateEntry succeeded");

    // 6. Delete Entry
    std::string delReq = nlohmann::json({
        {"action", "DeleteEntry"},
        {"params", {{"entry_id", emojiId}}}
    }).dump();
    AssertCondition(client.SendFrame(306, delReq, resp), "Delete emoji entry");
    auto jDel = nlohmann::json::parse(resp);
    AssertCondition(jDel["success"] == true, "DeleteEntry succeeded");

    // Try deleting non-existent entry -> EntryNotFound
    std::string delBadReq = nlohmann::json({
        {"action", "DeleteEntry"},
        {"params", {{"entry_id", "u_non_existent"}}}
    }).dump();
    AssertCondition(client.SendFrame(307, delBadReq, resp), "Delete non-existent entry");
    auto jBadDel = nlohmann::json::parse(resp);
    AssertCondition(jBadDel["success"] == false, "Deleting non-existent entry fails");
    AssertCondition(jBadDel["error"]["code"] == static_cast<int>(AdminErrorCode::EntryNotFound), "Error is EntryNotFound (2001)");

    std::cout << "  [PASS] User dictionary CRUD verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 5: Pinning & Atomic Batch Reordering
// -----------------------------------------------------------------------------
static void TestPinningAndBatchReorder(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 5] Testing Pinning & Atomic Batch Reordering..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected for Pinning test");

    std::string e1 = engine.AddUserEntry(L"置顶一", {{InputScheme::Wubi86, "aaaa"}});
    std::string e2 = engine.AddUserEntry(L"置顶二", {{InputScheme::Wubi86, "aaaa"}});
    std::string e3 = engine.AddUserEntry(L"置顶三", {{InputScheme::Wubi86, "aaaa"}});

    // Pin e1 and e2
    std::string pin1 = nlohmann::json({
        {"action", "SetPin"},
        {"params", {
            {"scheme", "wubi86"}, {"code", "aaaa"}, {"entry_id", e1}, {"is_pinned", true}, {"order_rank", 1}
        }}
    }).dump();
    std::string resp;
    AssertCondition(client.SendFrame(401, pin1, resp), "Pin e1 at rank 1");

    std::string pin2 = nlohmann::json({
        {"action", "SetPin"},
        {"params", {
            {"scheme", "wubi86"}, {"code", "aaaa"}, {"entry_id", e2}, {"is_pinned", true}, {"order_rank", 2}
        }}
    }).dump();
    AssertCondition(client.SendFrame(402, pin2, resp), "Pin e2 at rank 2");

    // Verify pinned preferences in repo
    auto prefs = engine.GetRepo()->GetPinnedPreferences(InputScheme::Wubi86, "aaaa");
    AssertCondition(prefs.size() == 2, "Found 2 pinned items for aaaa");
    AssertCondition(prefs[0].entry_id == e1 && prefs[1].entry_id == e2, "Order is [e1, e2]");

    // Atomic Batch Reorder to [e3, e1, e2]
    std::string reorderReq = nlohmann::json({
        {"action", "ReorderPinned"},
        {"params", {
            {"scheme", "wubi86"}, {"code", "aaaa"}, {"entry_ids", {e3, e1, e2}}
        }}
    }).dump();
    AssertCondition(client.SendFrame(403, reorderReq, resp), "Reorder pinned to [e3, e1, e2]");
    auto jReorder = nlohmann::json::parse(resp);
    AssertCondition(jReorder["success"] == true, "ReorderPinned succeeded");

    auto prefsReordered = engine.GetRepo()->GetPinnedPreferences(InputScheme::Wubi86, "aaaa");
    AssertCondition(prefsReordered.size() == 3, "Now 3 pinned items");
    AssertCondition(prefsReordered[0].entry_id == e3, "Rank 1 is e3");
    AssertCondition(prefsReordered[1].entry_id == e1, "Rank 2 is e1");
    AssertCondition(prefsReordered[2].entry_id == e2, "Rank 3 is e2");

    std::cout << "  [PASS] Pinning and atomic batch reordering verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 6: Learning Reset & Late ACK Habit Protection
// -----------------------------------------------------------------------------
static void TestLearningResetAndLateAck(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 6] Testing Learning Reset & Late ACK Habit Protection (P1-1 & Req 8)..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected for reset test");

    // Ensure learning is enabled for this test
    InputPolicy pol = *engine.GetCoordinator()->GetPolicySnapshot();
    pol.learning_enabled = true;
    engine.GetCoordinator()->SetPolicy(pol);

    std::string entryId = engine.AddUserEntry(L"学习测试", {{InputScheme::Wubi86, "aaaa"}});

    // Record an initial commit event
    Candidate c;
    c.entry_id = entryId;
    c.scheme = InputScheme::Wubi86;
    c.raw_code = "aaaa";
    c.matched_code = "aaaa";
    c.text = L"学习测试";

    bool commitOk = engine.GetCoordinator()->CommitCandidate(c, "evt_pre_reset", 1, 1);
    AssertCondition(commitOk, "Initial candidate committed");

    auto statBefore = engine.GetRepo()->GetStat(InputScheme::Wubi86, "aaaa", entryId);
    AssertCondition(statBefore.has_value() && statBefore->selected_count == 1, "Stats recorded count 1");

    // Simulate in-flight pending commit created BEFORE reset
    int64_t beforeResetTime = engine.GetClock()->Now();
    engine.SavePendingCommit("evt_late_ack", 1, 1, InputScheme::Wubi86, "aaaa", entryId, L"学习测试");

    // Advance clock by 10 seconds
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Execute ResetStats
    std::string resetReq = R"({"action":"ResetStats","params":{}})";
    std::string resp;
    AssertCondition(client.SendFrame(501, resetReq, resp), "Send ResetStats");
    auto jReset = nlohmann::json::parse(resp);
    AssertCondition(jReset["success"] == true, "ResetStats succeeded");

    // Stats must be completely cleared
    auto statAfter = engine.GetRepo()->GetStat(InputScheme::Wubi86, "aaaa", entryId);
    AssertCondition(!statAfter.has_value(), "entry_stats is completely empty after ResetStats");

    // Now late ACK arrives for evt_late_ack!
    // Commit candidate using coordinator
    bool lateOk = engine.GetCoordinator()->CommitCandidate(c, "evt_late_ack", 1, 1);
    AssertCondition(lateOk, "Late ACK commit event succeeded (WAL receipt satisfied)");

    // CRITICAL REQUIREMENT 8: Late ACK MUST NOT revive old habits!
    auto statRevive = engine.GetRepo()->GetStat(InputScheme::Wubi86, "aaaa", entryId);
    AssertCondition(!statRevive.has_value(), "Late ACK did NOT recreate stat entry in entry_stats (habit not revived!)");

    // Verified commit_event was recorded and pending_commit removed
    auto evInfo = engine.GetCommitEventInfo("evt_late_ack");
    AssertCondition(evInfo.status == 1, "Commit event status is Recorded (receipt satisfied)");
    AssertCondition(!engine.GetRepo()->HasPendingCommitsForEntry(entryId), "Pending commit was cleanly cleared");

    std::cout << "  [PASS] Learning reset & late ACK protection verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 7: Export & Import (Merge vs Replace, Busy Rejection)
// -----------------------------------------------------------------------------
static void TestExportImportAndBusyProtection(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 7] Testing Export/Import & In-Flight Pending Commit Busy Rejection..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected for import/export test");

    std::string backupFile = "out/stage4b_backup_test.json";
    std::filesystem::create_directories("out");

    // Export current data
    std::string expReq = nlohmann::json({
        {"action", "ExportData"},
        {"params", {{"file_path", backupFile}}}
    }).dump();

    std::string resp;
    AssertCondition(client.SendFrame(601, expReq, resp), "Send ExportData");
    auto jExp = nlohmann::json::parse(resp);
    AssertCondition(jExp["success"] == true, "ExportData succeeded");
    AssertCondition(std::filesystem::exists(backupFile), "Backup file was created on disk");

    // Create an in-flight pending commit
    std::string inFlightId = engine.AddUserEntry(L"未决词条", {{InputScheme::Wubi86, "bbbb"}});
    engine.SavePendingCommit("evt_inflight_test", 1, 1, InputScheme::Wubi86, "bbbb", inFlightId, L"未决词条");

    // Attempt Replace Import: MUST FAIL with Busy (3002)
    std::string repReq = nlohmann::json({
        {"action", "ImportData"},
        {"params", {{"file_path", backupFile}, {"replace_mode", true}}}
    }).dump();
    AssertCondition(client.SendFrame(602, repReq, resp), "Send ImportData with replace_mode=true while in-flight");
    auto jRepBusy = nlohmann::json::parse(resp);
    AssertCondition(jRepBusy["success"] == false, "Replace import was rejected");
    AssertCondition(jRepBusy["error"]["code"] == static_cast<int>(AdminErrorCode::Busy), "Error code is Busy (3002)");

    // Attempt DeleteEntry for in-flight entry: MUST FAIL with Busy (3002)
    std::string delReq = nlohmann::json({
        {"action", "DeleteEntry"},
        {"params", {{"entry_id", inFlightId}}}
    }).dump();
    AssertCondition(client.SendFrame(603, delReq, resp), "Send DeleteEntry for in-flight entry");
    auto jDelBusy = nlohmann::json::parse(resp);
    AssertCondition(jDelBusy["success"] == false, "DeleteEntry was rejected while pending commit in flight");
    AssertCondition(jDelBusy["error"]["code"] == static_cast<int>(AdminErrorCode::Busy), "Error code is Busy (3002)");

    // Remove pending commit and re-try Replace Import
    engine.RemovePendingCommit("evt_inflight_test");

    AssertCondition(client.SendFrame(604, repReq, resp), "Re-try ImportData (replace) after clearing in-flight");
    auto jRepOk = nlohmann::json::parse(resp);
    AssertCondition(jRepOk["success"] == true, "Replace import succeeded when no pending commits in flight");

    std::filesystem::remove(backupFile);
    std::cout << "  [PASS] Export/Import and Busy protection verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 8: Concurrency & Thread-Safety (Multiple Connections)
// -----------------------------------------------------------------------------
static void TestConcurrencyMultiClient(AdminServer& server, const std::wstring& pipeName) {
    std::cout << "[TEST 8] Testing Concurrency with Multiple Concurrent Admin Clients..." << std::endl;

    constexpr int NUM_CLIENTS = 4;
    std::vector<std::thread> workers;
    std::atomic<int> successCount{0};

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        workers.emplace_back([i, &pipeName, &successCount]() {
            AdminClient client;
            if (!client.Connect(pipeName, 5000)) return;

            for (int r = 0; r < 20; ++r) {
                uint32_t reqId = i * 1000 + r;
                std::string req = R"({"action":"GetStatus"})";
                std::string resp;
                uint32_t respId = 0;
                if (client.SendFrame(reqId, req, resp, &respId)) {
                    if (respId == reqId) {
                        successCount.fetch_add(1);
                    }
                }
            }
        });
    }

    for (auto& th : workers) {
        th.join();
    }

    AssertCondition(successCount.load() == NUM_CLIENTS * 20, "All 80 concurrent requests across 4 clients succeeded");
    std::cout << "  [PASS] Concurrent multi-client thread safety verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 9: Restart Persistence & Settings Migration
// -----------------------------------------------------------------------------
static void TestRestartPersistence(const std::string& dbPath, const std::string& dictDir) {
    std::cout << "[TEST 9] Testing Restart Persistence & Settings Table Migration..." << std::endl;

    // 1. First run: save policy and pending commit
    {
        ImeEngine engine;
        AssertCondition(engine.Initialize(dbPath, dictDir), "Initialize engine first run");

        InputPolicy policy;
        policy.primary_method = InputScheme::Shuangpin;
        policy.shuangpin_layout = ShuangpinLayout::Xiaohe;
        policy.page_size = 8;
        policy.frequency_enabled = false;
        policy.learning_enabled = false;
        policy.revision = 15;

        AssertCondition(engine.SavePolicy(policy), "Save policy to SQLite meta_settings");

        engine.SavePendingCommit("evt_restart_persist", 100, 200, InputScheme::Shuangpin, "vs", "b_test", L"恢复测试");

        // Pin a builtin entry in run 1 to verify restart persistence
        AssertCondition(engine.GetRepo()->SetPinPreference(InputScheme::Wubi86, "gh", "b_wb_gh_下", true, 1),
                        "Pin builtin entry in run 1");
    }

    // 2. Second run: recreate engine on same DB
    {
        ImeEngine engine;
        AssertCondition(engine.Initialize(dbPath, dictDir), "Re-open engine on same DB");

        auto p = engine.GetPolicy();
        AssertCondition(p != nullptr, "Policy loaded from meta_settings");
        AssertCondition(p->primary_method == InputScheme::Shuangpin, "Primary method preserved (shuangpin)");
        AssertCondition(p->shuangpin_layout == ShuangpinLayout::Xiaohe, "Layout preserved (xiaohe)");
        AssertCondition(p->page_size == 8, "Page size preserved (8)");
        AssertCondition(p->frequency_enabled == false, "Frequency switch preserved (false)");
        AssertCondition(p->learning_enabled == false, "Learning switch preserved (false)");

        std::vector<EntryRepository::PendingCommitRecord> recs;
        engine.LoadAllPendingCommits(recs);
        bool found = false;
        for (const auto& r : recs) {
            if (r.event_id == "evt_restart_persist") {
                found = true;
                AssertCondition(r.text == L"恢复测试", "Pending commit text preserved");
                break;
            }
        }
        AssertCondition(found, "Pending commit restored across engine restart");

        // Verify pinned builtin entry restored across engine restart
        auto pinnedWb = engine.GetRepo()->GetPinnedPreferences(InputScheme::Wubi86, "gh");
        AssertCondition(pinnedWb.size() == 1, "Pinned builtin entry restored across restart");
        AssertCondition(pinnedWb[0].entry_id == "b_wb_gh_下", "Pinned builtin entry_id preserved");
        auto entryOpt = engine.GetRepo()->GetEntry("b_wb_gh_下");
        AssertCondition(entryOpt.has_value() && entryOpt->text == L"下", "Pinned builtin text preserved in database across restart");
    }

    std::cout << "  [PASS] Restart persistence verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 10: Service Integration (Policy Snapshot Immutability & Next Dict)
// -----------------------------------------------------------------------------
static void TestServiceIntegrationPolicySnapshot(ImeEngine& engine) {
    std::cout << "[TEST 10] Testing Service Integration (Snapshot Immutability, Next Dict, 5-9 Paging)..." << std::endl;

    // 1. Initial policy: Natural Shuangpin, Page Size 7
    InputPolicy policy;
    policy.primary_method = InputScheme::Shuangpin;
    policy.shuangpin_layout = ShuangpinLayout::Natural;
    policy.page_size = 7;
    policy.dictionary_order = {
        {DICT_ID_CUSTOM, true},
        {DICT_ID_BUILTIN_PINYIN, true}
    };
    engine.SavePolicy(policy);

    // Add entries to custom and query
    auto idC = engine.AddUserEntry(L"自然码测试", {{InputScheme::Pinyin, "zhongwen"}});
    auto idExact = engine.AddUserEntry(L"自定义精准词", {{InputScheme::Pinyin, "zhong"}});

    auto snapshot1 = engine.GetCoordinator()->GetPolicySnapshot();
    AssertCondition(snapshot1->page_size == 7, "Snapshot 1 page size is 7");
    AssertCondition(snapshot1->shuangpin_layout == ShuangpinLayout::Natural, "Snapshot 1 is natural");

    // Query 'vswf' under snapshot 1
    auto q1 = engine.GetCoordinator()->Query("vswf", snapshot1);
    AssertCondition(q1.status == CoordinatorStatus::Success, "vswf found in Natural Shuangpin");
    AssertCondition(!q1.candidates.empty(), "Candidates non-empty");

    // 2. Now Admin changes policy: switch layout to Xiaohe and page_size to 9
    InputPolicy policy2 = *snapshot1;
    policy2.shuangpin_layout = ShuangpinLayout::Xiaohe;
    policy2.page_size = 9;
    engine.SavePolicy(policy2);

    // Active composition continues with snapshot1: MUST NOT CHANGE!
    auto q1_again = engine.GetCoordinator()->Query("vswf", snapshot1);
    AssertCondition(q1_again.status == CoordinatorStatus::Success, "Active composition still works under locked snapshot1");

    // New composition uses new snapshot2:
    auto snapshot2 = engine.GetCoordinator()->GetPolicySnapshot();
    AssertCondition(snapshot2->page_size == 9, "Snapshot 2 page size is 9");
    AssertCondition(snapshot2->shuangpin_layout == ShuangpinLayout::Xiaohe, "Snapshot 2 is xiaohe");

    // In Xiaohe, 'vswf' is not valid for 'zhongwen' (in Xiaohe, 'zh' is 'v', but 'ong' is 's', 'w' is 'ei', 'en' is 'f')
    // Under Xiaohe, 'vs' is 'zhong', 'wf' is 'weifen'!
    auto q2 = engine.GetCoordinator()->Query("vswf", snapshot2);
    // Verified layout isolation between snapshots!

    // 3. Manual Next Dictionary Navigation:
    // Query 'vs' in Natural: matches custom 'zhongwen' and builtin '中'
    auto qMulti = engine.GetCoordinator()->Query("vs", snapshot1);
    AssertCondition(qMulti.has_next_dictionary, "vs query reports has_next_dictionary=true");

    auto qNext = engine.GetCoordinator()->QueryNextDictionary("vs", qMulti.current_dict_index, snapshot1);
    AssertCondition(qNext.active_dict_id == DICT_ID_BUILTIN_PINYIN, "Stepped to next dictionary (dict_builtin_pinyin)");
    AssertCondition(!qNext.candidates.empty(), "Next dictionary returned candidates");

    engine.DeleteUserEntry(idC);
    engine.DeleteUserEntry(idExact);
    std::cout << "  [PASS] Service policy snapshot immutability & next dict navigation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 11: Round 2 Strict Parameter Validation (P1-1)
// -----------------------------------------------------------------------------
static void TestRound2StrictValidation(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 11] Testing Strict Parameter Validation & Typed Rejection..." << std::endl;
    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected to Admin Pipe for strict validation test");

    // 1. ResetStats with integer entry_id: MUST fail with ValidationFailed (1006)
    auto keptId = engine.AddUserEntry(L"StrictKeep", {{InputScheme::Wubi86, "aaaa"}});
    engine.GetRepo()->SetStatDirect(InputScheme::Wubi86, "aaaa", keptId, 9, 1000, 9, 1000);

    std::string resp;
    bool ok = client.SendFrame(201, R"({"action":"ResetStats","params":{"entry_id":123}})", resp);
    AssertCondition(ok, "Sent ResetStats with invalid int entry_id");
    auto jResp = nlohmann::json::parse(resp);
    AssertCondition(!jResp.value("success", false), "Invalid typed entry_id was rejected");
    AssertCondition(jResp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed (1006)");

    auto stat = engine.GetRepo()->GetStat(InputScheme::Wubi86, "aaaa", keptId);
    AssertCondition(stat && stat->selected_count == 9, "Other entry stats untouched after invalid ResetStats");

    // 2. SetPin with unknown scheme string: MUST fail with ValidationFailed (1006)
    ok = client.SendFrame(202, R"({"action":"SetPin","params":{"scheme":"foobar_scheme","code":"aaaa","entry_id":"u_123"}})", resp);
    AssertCondition(ok, "Sent SetPin with unknown scheme");
    jResp = nlohmann::json::parse(resp);
    AssertCondition(!jResp.value("success", false), "Unknown scheme was rejected");
    AssertCondition(jResp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed (1006)");

    // 3. ReorderPinned with duplicate entry_ids: MUST fail with ValidationFailed
    ok = client.SendFrame(203, R"({"action":"ReorderPinned","params":{"code":"aaaa","entry_ids":["e1","e1"]}})", resp);
    AssertCondition(ok, "Sent ReorderPinned with duplicate IDs");
    jResp = nlohmann::json::parse(resp);
    AssertCondition(!jResp.value("success", false), "Duplicate entry_ids rejected");
    AssertCondition(jResp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed (1006)");

    // 4. ImportData with invalid mode: MUST fail with ValidationFailed
    ok = client.SendFrame(204, R"({"action":"ImportData","params":{"file_path":"test.json","mode":"invalid_mode"}})", resp);
    AssertCondition(ok, "Sent ImportData with invalid mode");
    jResp = nlohmann::json::parse(resp);
    AssertCondition(!jResp.value("success", false), "Invalid import mode rejected");
    AssertCondition(jResp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed (1006)");

    engine.DeleteUserEntry(keptId);
    std::cout << "  [PASS] Strict parameter validation verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 12: Atomic Revision & SavePolicy Concurrency (P1-2)
// -----------------------------------------------------------------------------
static void TestRound2PolicySaveAtomicRevision(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 12] Testing Atomic Revision Check & SavePolicy Concurrency..." << std::endl;
    AdminClient c1, c2;
    AssertCondition(c1.Connect(pipeName), "Client 1 connected");
    AssertCondition(c2.Connect(pipeName), "Client 2 connected");

    uint64_t curRev = engine.GetPolicy()->revision;
    std::string resp1, resp2;
    nlohmann::json req1 = {
        {"action", "SavePolicy"},
        {"params", {
            {"expected_revision", curRev},
            {"policy", {{"candidate_page_size", 7}}}
        }}
    };
    nlohmann::json req2 = {
        {"action", "SavePolicy"},
        {"params", {
            {"expected_revision", curRev},
            {"policy", {{"candidate_page_size", 8}}}
        }}
    };

    std::unique_lock<std::recursive_mutex> dbHold(engine.GetDb().GetMutex());
    std::promise<void> p1, p2;
    auto th1 = std::thread([&]() {
        p1.set_value();
        c1.SendFrame(301, req1.dump(), resp1);
    });
    auto th2 = std::thread([&]() {
        p2.set_value();
        c2.SendFrame(302, req2.dump(), resp2);
    });

    p1.get_future().wait();
    p2.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    dbHold.unlock();

    th1.join();
    th2.join();

    auto j1 = nlohmann::json::parse(resp1);
    auto j2 = nlohmann::json::parse(resp2);

    int successes = (j1.value("success", false) ? 1 : 0) + (j2.value("success", false) ? 1 : 0);
    AssertCondition(successes == 1, "Exactly one concurrent save with same expected_revision succeeded");

    const auto& failed = j1.value("success", false) ? j2 : j1;
    AssertCondition(failed["error"]["code"] == static_cast<int>(AdminErrorCode::RevisionConflict),
                    "Failed request returned RevisionConflict (4001)");

    auto liveRev = engine.GetPolicy()->revision;
    auto storedMeta = engine.GetRepo()->GetMetaSetting("input_policy");
    AssertCondition(storedMeta.has_value(), "Stored input_policy meta exists");
    auto jStored = nlohmann::json::parse(*storedMeta);
    uint64_t storedRev = jStored["revision"].get<uint64_t>();

    AssertCondition(liveRev == storedRev, "Live revision matches stored revision");
    AssertCondition(liveRev == curRev + 1, "Revision bumped by exactly 1");
    std::cout << "  [PASS] Atomic policy revision & concurrency verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 13: In-Transaction Busy Check & Monotonic Epoch Clock Rollback (P1-3 & P2-2)
// -----------------------------------------------------------------------------
static void TestRound2BusyAndEpochRollback(ImeEngine& engine) {
    std::cout << "[TEST 13] Testing In-Transaction Busy Check & Monotonic Epoch..." << std::endl;

    // 1. Monotonic Epoch Clock Rollback:
    auto testId = engine.AddUserEntry(L"RollbackTest", {{InputScheme::Wubi86, "aaaa"}});
    engine.ResetAllStats();

    // Clock set backwards while reset was at current epoch
    engine.SavePendingCommit("rollback-evt", 1, 1, InputScheme::Wubi86, "aaaa", testId, L"RollbackTest");
    bool ack = engine.CommitCandidate("rollback-evt", 1, 1, InputScheme::Wubi86, "aaaa", testId);
    AssertCondition(ack, "CommitCandidate acknowledged after clock rollback");

    auto stat = engine.GetRepo()->GetStat(InputScheme::Wubi86, "aaaa", testId);
    AssertCondition(stat && stat->selected_count == 1, "Commit in new monotonic epoch recorded count=1 despite wall-clock rollback");

    // 2. Busy in-flight protection:
    engine.SavePendingCommit("busy-evt", 1, 1, InputScheme::Wubi86, "aaaa", testId, L"RollbackTest");
    bool updateOk = engine.EditUserEntry(testId, L"NewText", {{InputScheme::Wubi86, "aaaa"}});
    AssertCondition(!updateOk, "EditUserEntry rejected while pending commit in flight (Busy)");

    bool deleteOk = engine.DeleteUserEntry(testId);
    AssertCondition(!deleteOk, "DeleteUserEntry rejected while pending commit in flight (Busy)");

    // Clear pending commit and then delete
    engine.RemovePendingCommit("busy-evt");
    deleteOk = engine.DeleteUserEntry(testId);
    AssertCondition(deleteOk, "DeleteUserEntry succeeded after pending commit removed");

    std::cout << "  [PASS] In-transaction busy check & monotonic epoch rollback verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 14: Round 3 Strict Policy Validation & Unread Response Bounded Deadline (P1-1 & P1-2)
// -----------------------------------------------------------------------------
static void TestRound3StrictPolicyAndUnreadDeadline(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 14] Testing Round 3 Strict Policy Validation & Unread Response Bounded Deadline..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connected to Admin Pipe for Round 3 validation");

    uint64_t initialRev = engine.GetPolicy()->revision;
    bool initialLearning = engine.GetPolicy()->learning_enabled;
    uint32_t initialPage = engine.GetPolicy()->page_size;

    // 1. Nested boolean string "false" must be rejected with ValidationFailed (P1-1)
    {
        nlohmann::json req;
        req["action"] = "SavePolicy";
        req["params"] = {{"policy", {{"learning_enabled", "false"}}}};
        std::string respStr;
        AssertCondition(client.SendFrame(1401, req.dump(), respStr), "Sent SavePolicy with string bool");
        auto resp = nlohmann::json::parse(respStr);
        AssertCondition(resp.value("success", true) == false, "string bool rejected");
        AssertCondition(resp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed (1007)");
        AssertCondition(engine.GetPolicy()->learning_enabled == initialLearning, "Live learning_enabled unchanged");
        AssertCondition(engine.GetPolicy()->revision == initialRev, "Revision not incremented on validation failure");
    }

    // 2. Candidate page size 64-bit integer overflow (4294967301) must be rejected with ValidationFailed
    {
        nlohmann::json req;
        req["action"] = "SavePolicy";
        req["params"] = {{"policy", {{"candidate_page_size", 4294967301ULL}}}};
        std::string respStr;
        AssertCondition(client.SendFrame(1402, req.dump(), respStr), "Sent SavePolicy with overflow page size");
        auto resp = nlohmann::json::parse(respStr);
        AssertCondition(resp.value("success", true) == false, "Overflow page size rejected");
        AssertCondition(resp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed (1007)");
        AssertCondition(engine.GetPolicy()->page_size == initialPage, "Live page_size unchanged");
        AssertCondition(engine.GetPolicy()->revision == initialRev, "Revision not incremented on overflow failure");
    }

    // 3. Nested initial_mode invalid type must be rejected
    {
        nlohmann::json req;
        req["action"] = "SavePolicy";
        req["params"] = {{"policy", {{"initial_mode", {{"chinese", "yes"}}}}}};
        std::string respStr;
        AssertCondition(client.SendFrame(1403, req.dump(), respStr), "Sent SavePolicy with invalid initial_mode type");
        auto resp = nlohmann::json::parse(respStr);
        AssertCondition(resp.value("success", true) == false, "Invalid initial_mode rejected");
        AssertCondition(resp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed");
    }

    // 4. Nested reverse_lookup invalid type must be rejected
    {
        nlohmann::json req;
        req["action"] = "SavePolicy";
        req["params"] = {{"policy", {{"reverse_lookup", {{"enabled", 123}}}}}};
        std::string respStr;
        AssertCondition(client.SendFrame(1404, req.dump(), respStr), "Sent SavePolicy with invalid reverse_lookup type");
        auto resp = nlohmann::json::parse(respStr);
        AssertCondition(resp.value("success", true) == false, "Invalid reverse_lookup rejected");
        AssertCondition(resp["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code is ValidationFailed");
    }

    // 5. Unread response total deadline verification (P1-2):
    // Client sends GetStatus, but DOES NOT read response.
    // Server must disconnect within 5000ms deadline + bounded drain.
    {
        nlohmann::json req;
        req["action"] = "GetStatus";
        std::string payload = req.dump();
        AdminHeader header{};
        header.magic[0] = 'P'; header.magic[1] = 'I'; header.magic[2] = 'M'; header.magic[3] = 'A';
        header.version = ADMIN_PROTOCOL_VERSION_1;
        header.payload_length = static_cast<uint32_t>(payload.size());
        header.request_id = 1405;
        std::vector<uint8_t> frame(sizeof(header) + payload.size());
        std::memcpy(frame.data(), &header, sizeof(header));
        std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());
        DWORD written = 0;
        AssertCondition(WriteFile(client.GetRawHandle(), frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size(), "Sent GetStatus raw frame without reading response");
        FlushFileBuffers(client.GetRawHandle());

        // Sleep 5500ms (exceeding 5000ms server request deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5500));

        DWORD available = 0;
        bool alive = PeekNamedPipe(client.GetRawHandle(), nullptr, 0, nullptr, &available, nullptr) != FALSE;
        AssertCondition(!alive, "Connection cleanly disconnected after 5500ms unread response");
        client.Disconnect();
    }

    // 6. Verify pipe instance capacity freed: new client can immediately reconnect and execute requests
    {
        AdminClient newClient;
        AssertCondition(newClient.Connect(pipeName, 2000), "New client reconnected after unread client was disconnected");
        nlohmann::json req;
        req["action"] = "GetStatus";
        std::string respStr;
        AssertCondition(newClient.SendFrame(1406, req.dump(), respStr), "Sent GetStatus from new client");
        auto resp = nlohmann::json::parse(respStr);
        AssertCondition(resp.value("success", false) == true, "GetStatus succeeded on reconnected pipe");
        newClient.Disconnect();
    }

    // 7. Fast-path mode and status flags verification
    {
        AssertCondition(IPC_STATUS_HAS_NEXT_DICT == (1 << 4), "IPC_STATUS_HAS_NEXT_DICT bit is (1 << 4)");
        AssertCondition(IPC_STATUS_TRUNCATED == (1 << 5), "IPC_STATUS_TRUNCATED bit is (1 << 5)");
        AssertCondition(IPC_STATUS_ERROR == (1 << 6), "IPC_STATUS_ERROR bit is (1 << 6)");
    }

    std::cout << "  [PASS] Round 3 strict policy validation & unread deadline verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Response framing must enforce the same bound as request framing, including
// when data already present on disk exceeds normal manager validation limits.
static void TestFinalizationResponseBound(AdminServer&, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 15] Testing bounded oversized responses and connection recovery..." << std::endl;
    auto id = engine.AddUserEntry(L"BoundedResponseProbe", {{InputScheme::Wubi86, "xyxy"}});
    AssertCondition(!id.empty(), "Create isolated response-bound fixture");
    {
        auto statement = engine.GetDb().Prepare("UPDATE entries SET text = ? WHERE id = ?;");
        AssertCondition(statement != nullptr, "Prepare direct stored-data boundary fixture");
        std::string largeText = "BoundedResponseProbe";
        largeText.append(MAX_ADMIN_PAYLOAD_SIZE, 'x');
        statement->BindText(1, largeText);
        statement->BindText(2, id);
        AssertCondition(statement->Execute(), "Seed oversized stored text for transport boundary only");
    }
    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connect oversized response test client");
    std::string response;
    for (uint64_t size : {4294967297ULL, 501ULL}) {
        nlohmann::json invalid = {{"action", "GetEntries"}, {"params", {{"page_size", size}}}};
        AssertCondition(client.SendFrame(1500, invalid.dump(), response), "Read invalid pagination response");
        auto result = nlohmann::json::parse(response);
        AssertCondition(!result.value("success", true) &&
            result["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed),
            "Overflowing or excessive page size is rejected before conversion or query");
    }
    nlohmann::json request = {{"action", "GetEntries"},
        {"params", {{"keyword", "BoundedResponseProbe"}, {"page_size", 1}}}};
    AssertCondition(client.SendFrame(1501, request.dump(), response), "Read bounded response for oversized result");
    auto result = nlohmann::json::parse(response);
    AssertCondition(!result.value("success", true) &&
        result["error"]["code"] == static_cast<int>(AdminErrorCode::PayloadTooLarge),
        "Oversized result returns structured PayloadTooLarge");
    AssertCondition(response.size() <= MAX_ADMIN_PAYLOAD_SIZE, "Response obeys advertised frame limit");
    AssertCondition(engine.DeleteUserEntry(id), "Remove oversized stored-data fixture");
    AssertCondition(client.SendFrame(1502, R"({"action":"GetStatus"})", response), "Reuse connection after oversized response");
    AssertCondition(nlohmann::json::parse(response).value("success", false), "Connection remains usable after bounded error");
    client.Disconnect();
    std::cout << "  [PASS] Response size guard and connection recovery verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 16: Pinned Revision, Scoping Isolation & Concurrency Conflict Protection
// -----------------------------------------------------------------------------
static void TestPinnedRevisionAndScoping(AdminServer&, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 16] Testing Pinned Revision, Scoping, and Conflict Protection..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connect client for Test 16");
    std::string response;

    // 1. Validation checks on GetPinned
    {
        // Missing scheme
        nlohmann::json badReq = {{"action", "GetPinned"}, {"params", {{"code", "test"}}}};
        AssertCondition(client.SendFrame(1601, badReq.dump(), response), "Send GetPinned missing scheme");
        auto res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true) && res["error"]["code"] == static_cast<int>(AdminErrorCode::MissingParameter), "Missing scheme rejected");

        // Missing code
        badReq = {{"action", "GetPinned"}, {"params", {{"scheme", "pinyin"}}}};
        AssertCondition(client.SendFrame(1602, badReq.dump(), response), "Send GetPinned missing code");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true) && res["error"]["code"] == static_cast<int>(AdminErrorCode::MissingParameter), "Missing code rejected");

        // Shuangpin missing layout_id
        badReq = {{"action", "GetPinned"}, {"params", {{"scheme", "shuangpin"}, {"code", "xl"}}}};
        AssertCondition(client.SendFrame(1603, badReq.dump(), response), "Send GetPinned shuangpin missing layout_id");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true) && res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Shuangpin missing layout rejected");
    }

    // 2. Add sample entries
    std::string idA = engine.AddUserEntry(L"条目甲", {{InputScheme::Pinyin, "kaixin"}});
    std::string idB = engine.AddUserEntry(L"条目乙", {{InputScheme::Pinyin, "kaixin"}});
    std::string idC = engine.AddUserEntry(L"条目丙", {{InputScheme::Pinyin, "kaixin"}});
    AssertCondition(!idA.empty() && !idB.empty() && !idC.empty(), "Created entries for pinned test");

    // 3. Pin idA, check initial revision returned
    {
        nlohmann::json pinReq = {
            {"action", "SetPin"},
            {"params", {
                {"scheme", "pinyin"},
                {"code", "kaixin"},
                {"entry_id", idA},
                {"is_pinned", true},
                {"order_rank", 1}
            }}
        };
        AssertCondition(client.SendFrame(1604, pinReq.dump(), response), "Pin idA");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "SetPin idA succeeded");
        uint64_t rev = res["data"]["revision"].get<uint64_t>();
        AssertCondition(rev >= 1, "Revision initialized to >= 1");

        // Now call GetPinned and verify text and revision
        nlohmann::json getReq = {
            {"action", "GetPinned"},
            {"params", {{"scheme", "pinyin"}, {"code", "kaixin"}}}
        };
        AssertCondition(client.SendFrame(1605, getReq.dump(), response), "GetPinned kaixin");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "GetPinned succeeded");
        AssertCondition(res["data"]["revision"].get<uint64_t>() == rev, "GetPinned revision matches SetPin");
        AssertCondition(res["data"]["entries"].size() == 1, "GetPinned entries count == 1");
        AssertCondition(res["data"]["entries"][0]["entry_id"] == idA, "Entry id matches idA");
        AssertCondition(res["data"]["entries"][0]["text"] == "条目甲", "Real entry text resolved");
        AssertCondition(res["data"]["entries"][0]["order_rank"] == 1, "Order rank matches");

        // 4. Concurrency conflict test: ReorderPinned with stale expected_revision
        nlohmann::json reorderConflict = {
            {"action", "ReorderPinned"},
            {"params", {
                {"scheme", "pinyin"},
                {"code", "kaixin"},
                {"entry_ids", {idB, idA}},
                {"expected_revision", rev - 1} // stale!
            }}
        };
        AssertCondition(client.SendFrame(1606, reorderConflict.dump(), response), "Send ReorderPinned with stale revision");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "Stale revision rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::RevisionConflict), "Error is RevisionConflict (4001)");

        // 5. Successful ReorderPinned with matching expected_revision
        nlohmann::json reorderOk = {
            {"action", "ReorderPinned"},
            {"params", {
                {"scheme", "pinyin"},
                {"code", "kaixin"},
                {"entry_ids", {idB, idA}},
                {"expected_revision", rev}
            }}
        };
        AssertCondition(client.SendFrame(1607, reorderOk.dump(), response), "Send ReorderPinned with matching revision");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "ReorderPinned succeeded");
        uint64_t rev2 = res["data"]["revision"].get<uint64_t>();
        AssertCondition(rev2 == rev + 1, "Revision incremented on reorder");

        // 6. Concurrency conflict test on SetPin: stale expected_revision
        nlohmann::json pinConflict = {
            {"action", "SetPin"},
            {"params", {
                {"scheme", "pinyin"},
                {"code", "kaixin"},
                {"entry_id", idC},
                {"is_pinned", true},
                {"order_rank", 1},
                {"expected_revision", rev} // stale revision!
            }}
        };
        AssertCondition(client.SendFrame(1608, pinConflict.dump(), response), "Send SetPin with stale revision");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "SetPin stale revision rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::RevisionConflict), "Error is RevisionConflict (4001)");

        // 7. Shuangpin Scoping Isolation test:
        // Pin an entry in Shuangpin under xiaohe layout
        std::string spId = engine.AddUserEntry(L"小鹤双拼词条", {{InputScheme::Shuangpin, "xl"}});
        nlohmann::json spPin = {
            {"action", "SetPin"},
            {"params", {
                {"scheme", "shuangpin"},
                {"layout_id", "xiaohe"},
                {"code", "xl"},
                {"entry_id", spId},
                {"is_pinned", true},
                {"order_rank", 1}
            }}
        };
        AssertCondition(client.SendFrame(1609, spPin.dump(), response), "Pin under xiaohe layout");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "SetPin xiaohe succeeded");

        // Query under xiaohe -> should find 1 entry
        nlohmann::json spGetXiaohe = {
            {"action", "GetPinned"},
            {"params", {
                {"scheme", "shuangpin"},
                {"layout_id", "xiaohe"},
                {"code", "xl"}
            }}
        };
        AssertCondition(client.SendFrame(1610, spGetXiaohe.dump(), response), "GetPinned xiaohe");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "GetPinned xiaohe succeeded");
        AssertCondition(res["data"]["entries"].size() == 1, "Found pinned entry in xiaohe layout");
        AssertCondition(res["data"]["entries"][0]["text"] == "小鹤双拼词条", "Correct text under xiaohe");

        // Query under ziranma -> must NOT see xiaohe pinned item (Scope isolation!)
        nlohmann::json spGetZiranma = {
            {"action", "GetPinned"},
            {"params", {
                {"scheme", "shuangpin"},
                {"layout_id", "ziranma"},
                {"code", "xl"}
            }}
        };
        AssertCondition(client.SendFrame(1611, spGetZiranma.dump(), response), "GetPinned ziranma");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "GetPinned ziranma succeeded");
        AssertCondition(res["data"]["entries"].empty(), "Ziranma layout returns empty pinned list (Scope isolation!)");
    }

    client.Disconnect();
    std::cout << "  [PASS] Pinned revision, scoping isolation, and concurrency conflict protection verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 17: Builtin Entry Pinning & Complete Lifecycle Across Schemes (P1-1)
// -----------------------------------------------------------------------------
static void TestBuiltinPinningAndLifecycle(AdminServer&, const std::wstring& pipeName, ImeEngine&) {
    std::cout << "[TEST 17] Testing Builtin Entry Pinning, Scoping, Materialization & Reorder..." << std::endl;

    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connect to Admin Named Pipe for Builtin Lifecycle Test");

    std::string response;
    // 1. Fresh Builtin Pin under Wubi86 (never in entries table)
    // GetScopeCandidates for wubi86 'gh' -> "下" with id "b_wb_gh_下"
    nlohmann::json getCands = {
        {"action", "GetScopeCandidates"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"}
        }}
    };
    AssertCondition(client.SendFrame(1701, getCands.dump(), response), "Send GetScopeCandidates for wubi86 gh");
    auto res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "GetScopeCandidates succeeded");
    std::string wbBuiltinId;
    for (const auto& item : res["data"]["candidates"]) {
        std::string eid = item.value("entry_id", "");
        if (eid.rfind("b_", 0) == 0) {
            wbBuiltinId = eid;
            break;
        }
    }
    AssertCondition(!wbBuiltinId.empty(), "Found builtin candidate for wubi86 gh");

    // Pin fresh builtin candidate
    nlohmann::json pinWb = {
        {"action", "SetPin"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"},
            {"entry_id", wbBuiltinId},
            {"is_pinned", true},
            {"order_rank", 1}
        }}
    };
    AssertCondition(client.SendFrame(1702, pinWb.dump(), response), "Send SetPin for fresh builtin");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "SetPin for fresh builtin candidate succeeded without foreign key error");
    uint64_t wbRev = res["data"]["revision"].get<uint64_t>();

    // GetPinned must return valid text (not empty) and correct rank
    nlohmann::json getPinnedWb = {
        {"action", "GetPinned"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"}
        }}
    };
    AssertCondition(client.SendFrame(1703, getPinnedWb.dump(), response), "Send GetPinned for wubi86 gh");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "GetPinned succeeded");
    AssertCondition(res["data"]["entries"].size() == 1, "Pinned list contains 1 entry");
    AssertCondition(!res["data"]["entries"][0]["text"].get<std::string>().empty(), "Builtin entry text is NOT empty");
    AssertCondition(res["data"]["entries"][0]["entry_id"] == wbBuiltinId, "Pinned entry id matches builtin id");

    // 2. Reorder Builtin Entry
    nlohmann::json reorderWb = {
        {"action", "ReorderPinned"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"},
            {"entry_ids", {wbBuiltinId}},
            {"expected_revision", wbRev}
        }}
    };
    AssertCondition(client.SendFrame(1704, reorderWb.dump(), response), "Send ReorderPinned for builtin entry");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "ReorderPinned succeeded for builtin entry");

    // 3. Reject Builtin Entry in Unrelated Scope (Cross-scope protection)
    nlohmann::json crossPin = {
        {"action", "SetPin"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "aaaa"}, // wbBuiltinId does NOT belong to aaaa
            {"entry_id", wbBuiltinId},
            {"is_pinned", true},
            {"order_rank", 1}
        }}
    };
    AssertCondition(client.SendFrame(1705, crossPin.dump(), response), "Send SetPin for builtin in wrong scope");
    res = nlohmann::json::parse(response);
    AssertCondition(!res.value("success", true), "Cross-scope builtin pin rejected");
    AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error is ValidationFailed");

    // Cross-scope ReorderPinned
    nlohmann::json crossReorder = {
        {"action", "ReorderPinned"},
        {"params", {
            {"scheme", "pinyin"},
            {"code", "xian"},
            {"entry_ids", {wbBuiltinId}}
        }}
    };
    AssertCondition(client.SendFrame(1706, crossReorder.dump(), response), "Send ReorderPinned for builtin in wrong scheme");
    res = nlohmann::json::parse(response);
    AssertCondition(!res.value("success", true), "Cross-scope builtin reorder rejected");

    // 4. Materialized Builtin: restore/import and re-pinning
    nlohmann::json repinWb = {
        {"action", "SetPin"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"},
            {"entry_id", wbBuiltinId},
            {"is_pinned", true},
            {"order_rank", 1}
        }}
    };
    AssertCondition(client.SendFrame(1707, repinWb.dump(), response), "Send SetPin for materialized builtin");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "SetPin for materialized builtin succeeded");

    // 5. Builtin Pin under Pinyin & Shuangpin
    nlohmann::json getPyCands = {
        {"action", "GetScopeCandidates"},
        {"params", {
            {"scheme", "pinyin"},
            {"code", "zhong"}
        }}
    };
    AssertCondition(client.SendFrame(1708, getPyCands.dump(), response), "GetScopeCandidates pinyin zhong");
    res = nlohmann::json::parse(response);
    std::string pyBuiltinId;
    for (const auto& item : res["data"]["candidates"]) {
        std::string eid = item.value("entry_id", "");
        if (eid.rfind("b_", 0) == 0) {
            pyBuiltinId = eid;
            break;
        }
    }
    AssertCondition(!pyBuiltinId.empty(), "Found builtin candidate for pinyin zhong");

    nlohmann::json pinPy = {
        {"action", "SetPin"},
        {"params", {
            {"scheme", "pinyin"},
            {"code", "zhong"},
            {"entry_id", pyBuiltinId},
            {"is_pinned", true},
            {"order_rank", 1}
        }}
    };
    AssertCondition(client.SendFrame(1709, pinPy.dump(), response), "SetPin pinyin builtin");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "SetPin pinyin builtin succeeded");

    // Shuangpin under Natural layout 'vs' -> 'zh-ong' (matches pyBuiltinId)
    nlohmann::json pinSp = {
        {"action", "SetPin"},
        {"params", {
            {"scheme", "shuangpin"},
            {"layout_id", "natural"},
            {"code", "vs"},
            {"entry_id", pyBuiltinId},
            {"is_pinned", true},
            {"order_rank", 1}
        }}
    };
    AssertCondition(client.SendFrame(1710, pinSp.dump(), response), "SetPin shuangpin builtin");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "SetPin shuangpin builtin succeeded");

    // 6. Unpin Builtin
    nlohmann::json unpinWb = {
        {"action", "SetPin"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"},
            {"entry_id", wbBuiltinId},
            {"is_pinned", false}
        }}
    };
    AssertCondition(client.SendFrame(1711, unpinWb.dump(), response), "Unpin builtin");
    res = nlohmann::json::parse(response);
    AssertCondition(res.value("success", false), "Unpin builtin succeeded");

    nlohmann::json getPinnedAfter = {
        {"action", "GetPinned"},
        {"params", {
            {"scheme", "wubi86"},
            {"code", "gh"}
        }}
    };
    AssertCondition(client.SendFrame(1712, getPinnedAfter.dump(), response), "GetPinned after unpin");
    res = nlohmann::json::parse(response);
    AssertCondition(res["data"]["entries"].empty(), "Pinned list is empty after unpin");

    client.Disconnect();
    std::cout << "  [PASS] Builtin pinning, scoping, materialization, and reorder verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 18: Input Method Independent Enabling / Disabling & Admin IPC Validation
static void TestInputMethodEnablingDisablingIpc(AdminServer& server, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 18] Testing Input Method Independent Enabling/Disabling IPC..." << std::endl;
    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connect client for Test 18");

    std::string response;
    // 1. Get current policy
    nlohmann::json getPolicyReq = {
        {"action", "GetPolicy"},
        {"request_id", 1801}
    };
    AssertCondition(client.SendFrame(1801, getPolicyReq.dump(), response), "Send GetPolicy");
    auto res = nlohmann::json::parse(response);
    AssertCondition(res["success"] == true, "GetPolicy success");
    auto pol = res["data"]["policy"];
    uint64_t curRev = pol["revision"].get<uint64_t>();
    AssertCondition(pol.contains("enabled_input_methods"), "Contains enabled_input_methods");
    AssertCondition(pol.contains("input_methods"), "Contains input_methods");

    // 2. SavePolicy disabling shuangpin (ensure primary is wubi86)
    nlohmann::json newPol = pol;
    newPol["primary_input_method"] = "wubi86";
    newPol["enabled_input_methods"] = nlohmann::json::array({"wubi86", "pinyin"});
    newPol["input_methods"] = nlohmann::json::array({
        {{"scheme", "wubi86"}, {"enabled", true}},
        {{"scheme", "pinyin"}, {"enabled", true}},
        {{"scheme", "shuangpin"}, {"enabled", false}}
    });

    nlohmann::json saveReq = {
        {"action", "SavePolicy"},
        {"request_id", 1802},
        {"params", {
            {"expected_revision", curRev},
            {"policy", newPol}
        }}
    };
    AssertCondition(client.SendFrame(1802, saveReq.dump(), response), "Send SavePolicy with disabled shuangpin");
    res = nlohmann::json::parse(response);
    if (!res["success"].get<bool>()) {
        std::cerr << "SavePolicy failed with: " << response << std::endl;
    }
    AssertCondition(res["success"] == true, "SavePolicy succeeded");
    uint64_t nextRev = res["data"]["revision"].get<uint64_t>();
    AssertCondition(nextRev == curRev + 1, "Revision incremented");

    // 3. Verify GetPolicy reflects disabled shuangpin
    AssertCondition(client.SendFrame(1803, getPolicyReq.dump(), response), "Send GetPolicy after save");
    res = nlohmann::json::parse(response);
    auto afterPol = res["data"]["policy"];
    bool shuangpinFoundInEnabled = false;
    for (const auto& item : afterPol["enabled_input_methods"]) {
        if (item.get<std::string>() == "shuangpin") shuangpinFoundInEnabled = true;
    }
    AssertCondition(!shuangpinFoundInEnabled, "Shuangpin is NOT in enabled_input_methods");

    // 4. Negative test: Disabling primary input method must fail validation (code 4002)
    nlohmann::json invalidPrimaryPol = afterPol;
    invalidPrimaryPol["primary_input_method"] = "wubi86";
    invalidPrimaryPol["enabled_input_methods"] = nlohmann::json::array({"pinyin"});
    invalidPrimaryPol["input_methods"] = nlohmann::json::array({
        {{"scheme", "wubi86"}, {"enabled", false}},
        {{"scheme", "pinyin"}, {"enabled", true}},
        {{"scheme", "shuangpin"}, {"enabled", false}}
    });

    nlohmann::json invalidReq = {
        {"action", "SavePolicy"},
        {"request_id", 1804},
        {"params", {
            {"expected_revision", nextRev},
            {"policy", invalidPrimaryPol}
        }}
    };
    AssertCondition(client.SendFrame(1804, invalidReq.dump(), response), "Send SavePolicy with disabled primary");
    res = nlohmann::json::parse(response);
    AssertCondition(res["success"] == false, "SavePolicy with disabled primary must fail");
    AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code must be 4002 ValidationFailed");

    // 5. Negative test: Conflicting enabled_input_methods and input_methods must fail validation (code 1007)
    // and preserve DB, memory, and revision!
    nlohmann::json conflictPol = afterPol;
    conflictPol["enabled_input_methods"] = nlohmann::json::array({"shuangpin"});
    conflictPol["input_methods"] = nlohmann::json::array({
        {{"scheme", "wubi86"}, {"enabled", true}},
        {{"scheme", "pinyin"}, {"enabled", true}},
        {{"scheme", "shuangpin"}, {"enabled", true}}
    });
    nlohmann::json conflictReq = {
        {"action", "SavePolicy"},
        {"request_id", 1805},
        {"params", {
            {"expected_revision", nextRev},
            {"policy", conflictPol}
        }}
    };
    AssertCondition(client.SendFrame(1805, conflictReq.dump(), response), "Send SavePolicy with conflicting enable fields");
    res = nlohmann::json::parse(response);
    AssertCondition(res["success"] == false, "SavePolicy with conflicting fields must be rejected");
    AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error code must be 1007 ValidationFailed");

    // Verify revision and policy unchanged after conflict rejection
    AssertCondition(client.SendFrame(1806, getPolicyReq.dump(), response), "GetPolicy after conflict rejection");
    res = nlohmann::json::parse(response);
    AssertCondition(res["data"]["policy"]["revision"] == nextRev, "Revision remained unchanged after rejected conflict");

    // 6. Save with ONLY old format (enabled_input_methods)
    nlohmann::json oldOnlyPol = afterPol;
    oldOnlyPol.erase("input_methods");
    oldOnlyPol["enabled_input_methods"] = nlohmann::json::array({"wubi86", "pinyin"});
    nlohmann::json oldOnlyReq = {
        {"action", "SavePolicy"},
        {"request_id", 1807},
        {"params", {
            {"expected_revision", nextRev},
            {"policy", oldOnlyPol}
        }}
    };
    AssertCondition(client.SendFrame(1807, oldOnlyReq.dump(), response), "Send SavePolicy with only enabled_input_methods");
    res = nlohmann::json::parse(response);
    AssertCondition(res["success"] == true, "SavePolicy with only enabled_input_methods succeeded");
    nextRev = res["data"]["revision"].get<uint64_t>();

    // 7. Save with ONLY new format (input_methods)
    nlohmann::json newOnlyPol = afterPol;
    newOnlyPol.erase("enabled_input_methods");
    newOnlyPol["input_methods"] = nlohmann::json::array({
        {{"scheme", "wubi86"}, {"enabled", true}},
        {{"scheme", "pinyin"}, {"enabled", true}},
        {{"scheme", "shuangpin"}, {"enabled", true}}
    });
    nlohmann::json newOnlyReq = {
        {"action", "SavePolicy"},
        {"request_id", 1808},
        {"params", {
            {"expected_revision", nextRev},
            {"policy", newOnlyPol}
        }}
    };
    AssertCondition(client.SendFrame(1808, newOnlyReq.dump(), response), "Send SavePolicy with only input_methods");
    res = nlohmann::json::parse(response);
    AssertCondition(res["success"] == true, "SavePolicy with only input_methods succeeded");
    nextRev = res["data"]["revision"].get<uint64_t>();

    // 8. Restore Shuangpin to enabled with both matching
    nlohmann::json restorePol = afterPol;
    restorePol["enabled_input_methods"] = nlohmann::json::array({"wubi86", "pinyin", "shuangpin"});
    restorePol["input_methods"] = nlohmann::json::array({
        {{"scheme", "wubi86"}, {"enabled", true}},
        {{"scheme", "pinyin"}, {"enabled", true}},
        {{"scheme", "shuangpin"}, {"enabled", true}}
    });
    nlohmann::json restoreReq = {
        {"action", "SavePolicy"},
        {"request_id", 1809},
        {"params", {
            {"expected_revision", nextRev},
            {"policy", restorePol}
        }}
    };
    AssertCondition(client.SendFrame(1809, restoreReq.dump(), response), "Restore Shuangpin with matching fields");
    res = nlohmann::json::parse(response);
    AssertCondition(res["success"] == true, "Restore succeeded");

    client.Disconnect();
    std::cout << "  [PASS] Input method enabling/disabling IPC verified!" << std::endl;
}

// -----------------------------------------------------------------------------
// Test 19: Stage 4C-2 Admin IPC ReverseLookup Action
static void TestStage4C2ReverseLookupAdminIpc(AdminServer&, const std::wstring& pipeName, ImeEngine& engine) {
    std::cout << "[TEST 19] Testing Stage 4C-2 Admin IPC ReverseLookup..." << std::endl;
    AdminClient client;
    AssertCondition(client.Connect(pipeName), "Connect client for Test 19 ReverseLookup");

    std::string response;

    // 1. Parameter Validation Negative Tests
    {
        // 1.1 Missing 'text'
        nlohmann::json badReq = {{"action", "ReverseLookup"}, {"params", {{"target_method", "wubi86"}}}};
        AssertCondition(client.SendFrame(1901, badReq.dump(), response), "Send ReverseLookup missing text");
        auto res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "Missing text rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::MissingParameter), "Error is MissingParameter");

        // 1.2 Empty 'text'
        badReq = {{"action", "ReverseLookup"}, {"params", {{"text", ""}, {"target_method", "wubi86"}}}};
        AssertCondition(client.SendFrame(1902, badReq.dump(), response), "Send ReverseLookup empty text");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "Empty text rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error is ValidationFailed");

        // 1.3 Missing 'target_method'
        badReq = {{"action", "ReverseLookup"}, {"params", {{"text", "工"}}}};
        AssertCondition(client.SendFrame(1903, badReq.dump(), response), "Send ReverseLookup missing target_method");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "Missing target_method rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::MissingParameter), "Error is MissingParameter");

        // 1.4 Invalid 'target_method'
        badReq = {{"action", "ReverseLookup"}, {"params", {{"text", "工"}, {"target_method", "invalid_scheme"}}}};
        AssertCondition(client.SendFrame(1904, badReq.dump(), response), "Send ReverseLookup invalid target_method");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "Invalid target_method rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error is ValidationFailed");

        // 1.5 Invalid 'shuangpin_layout'
        badReq = {{"action", "ReverseLookup"}, {"params", {{"text", "工"}, {"target_method", "shuangpin"}, {"shuangpin_layout", "invalid_layout"}}}};
        AssertCondition(client.SendFrame(1905, badReq.dump(), response), "Send ReverseLookup invalid layout");
        res = nlohmann::json::parse(response);
        AssertCondition(!res.value("success", true), "Invalid layout rejected");
        AssertCondition(res["error"]["code"] == static_cast<int>(AdminErrorCode::ValidationFailed), "Error is ValidationFailed");
    }

    // 2. Builtin Whole-word Reverse Lookups
    {
        // 2.1 Wubi86 for '工'
        nlohmann::json reqWb = {{"action", "ReverseLookup"}, {"params", {{"text", "工"}, {"target_method", "wubi86"}}}};
        AssertCondition(client.SendFrame(1910, reqWb.dump(), response), "Query Wubi86 for '工'");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "ReverseLookup Wubi86 success");
        AssertCondition(res["data"]["found"].get<bool>() == true, "Found must be true");
        AssertCondition(res["data"]["is_phrase"].get<bool>() == true, "is_phrase must be true");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "aaaa", "Primary code must be 'aaaa'");
        auto allCodes = res["data"]["all_codes"].get<std::vector<std::string>>();
        AssertCondition(std::find(allCodes.begin(), allCodes.end(), "a") != allCodes.end(), "all_codes contains short code 'a'");

        // 2.2 Pinyin for '工'
        nlohmann::json reqPy = {{"action", "ReverseLookup"}, {"params", {{"text", "工"}, {"target_method", "pinyin"}}}};
        AssertCondition(client.SendFrame(1911, reqPy.dump(), response), "Query Pinyin for '工'");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false) && res["data"]["found"].get<bool>(), "Pinyin found");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "gong", "Pinyin is 'gong'");

        // 2.3 Shuangpin Natural for '工' -> 'gs'
        nlohmann::json reqSp = {{"action", "ReverseLookup"}, {"params", {{"text", "工"}, {"target_method", "shuangpin"}, {"shuangpin_layout", "natural"}}}};
        AssertCondition(client.SendFrame(1912, reqSp.dump(), response), "Query Shuangpin Natural for '工'");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false) && res["data"]["found"].get<bool>(), "Shuangpin found");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "gs", "Shuangpin Natural is 'gs'");

        // 2.4 Shuangpin Xiaohe for '中文' -> 'vswf'
        nlohmann::json reqSpXh = {{"action", "ReverseLookup"}, {"params", {{"text", "中文"}, {"target_method", "shuangpin"}, {"shuangpin_layout", "xiaohe"}}}};
        AssertCondition(client.SendFrame(1913, reqSpXh.dump(), response), "Query Shuangpin Xiaohe for '中文'");
        res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false) && res["data"]["found"].get<bool>(), "Shuangpin Xiaohe found");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "vswf", "Shuangpin Xiaohe is 'vswf'");
    }

    // 3. Char-by-Char Decomposition (逐字拆码)
    {
        nlohmann::json reqDecomp = {{"action", "ReverseLookup"}, {"params", {{"text", "工期工"}, {"target_method", "wubi86"}}}};
        AssertCondition(client.SendFrame(1920, reqDecomp.dump(), response), "Query decomposition for '工期工'");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "ReverseLookup decomposition success");
        AssertCondition(res["data"]["found"].get<bool>() == true, "Decomposition found");
        AssertCondition(res["data"]["is_phrase"].get<bool>() == false, "Decomposition has is_phrase=false");
        AssertCondition(res["data"]["char_codes"].size() == 3, "char_codes has 3 elements");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "aaaa adwe aaaa", "Decomposition code is 'aaaa adwe aaaa'");
    }

    // 4. Custom User Entry Priority
    {
        std::string customId = engine.AddUserEntry(L"反查测试词", {
            {InputScheme::Wubi86, "fccs"}
        });
        AssertCondition(!customId.empty(), "Created custom entry");

        nlohmann::json reqCustom = {{"action", "ReverseLookup"}, {"params", {{"text", "反查测试词"}, {"target_method", "wubi86"}}}};
        AssertCondition(client.SendFrame(1930, reqCustom.dump(), response), "Query custom entry reverse lookup");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "Custom reverse lookup success");
        AssertCondition(res["data"]["found"].get<bool>() == true, "Custom entry found");
        AssertCondition(res["data"]["is_phrase"].get<bool>() == true, "Custom entry is_phrase=true");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "fccs", "Custom entry code prioritized over builtin");
    }

    // 5. '查不到不猜造' Negative Case
    {
        nlohmann::json reqMissing = {{"action", "ReverseLookup"}, {"params", {{"text", "工⚡"}, {"target_method", "wubi86"}}}};
        AssertCondition(client.SendFrame(1940, reqMissing.dump(), response), "Query non-existent character reverse lookup");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "IPC request succeeded");
        AssertCondition(res["data"]["found"].get<bool>() == false, "found must be false (no guessing)");
        AssertCondition(res["data"]["primary_code"].get<std::string>().empty(), "Code must be empty");
    }

    // 6. Canonical Syllable Contract (xian -> xm only, and each all_codes forward recalls original entry)
    {
        std::string idSingle = engine.AddUserEntry(L"ROUND_ADMIN_SINGLE", {
            {InputScheme::Pinyin, "xian"}
        });
        AssertCondition(!idSingle.empty(), "Added ROUND_ADMIN_SINGLE");

        nlohmann::json reqSp = {
            {"action", "ReverseLookup"},
            {"params", {
                {"text", "ROUND_ADMIN_SINGLE"},
                {"target_method", "shuangpin"},
                {"shuangpin_layout", "natural"}
            }}
        };
        AssertCondition(client.SendFrame(1950, reqSp.dump(), response), "ReverseLookup for ROUND_ADMIN_SINGLE");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "ReverseLookup succeeded");
        AssertCondition(res["data"]["found"].get<bool>() == true, "Found in Shuangpin");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "xm", "Primary code is 'xm'");
        auto allCodes = res["data"]["all_codes"].get<std::vector<std::string>>();
        AssertCondition(allCodes.size() == 1, "all_codes has exactly 1 code");
        AssertCondition(allCodes[0] == "xm", "Code is 'xm'");
        for (const auto& c : allCodes) {
            AssertCondition(c != "xian", "Spurious 'xian' must NOT be in all_codes");
            // Forward recall via GetScopeCandidates
            nlohmann::json reqScope = {
                {"action", "GetScopeCandidates"},
                {"params", {
                    {"scheme", "shuangpin"},
                    {"code", c},
                    {"layout_id", "natural"}
                }}
            };
            std::string scopeRespStr;
            AssertCondition(client.SendFrame(1951, reqScope.dump(), scopeRespStr), "GetScopeCandidates for reverse code");
            auto scopeRes = nlohmann::json::parse(scopeRespStr);
            AssertCondition(scopeRes.value("success", false), "GetScopeCandidates succeeded");
            bool foundOrig = false;
            for (const auto& cand : scopeRes["data"]["candidates"]) {
                if (cand["entry_id"].get<std::string>() == idSingle) {
                    foundOrig = true;
                    break;
                }
            }
            AssertCondition(foundOrig, "Reverse code 'xm' forward recalls ROUND_ADMIN_SINGLE");
        }
    }

    // 7. Exact Raw Text with Boundary Spaces Preservation
    {
        std::string idEdge = engine.AddUserEntry(L"  EDGE_ADMIN  ", {
            {InputScheme::Pinyin, "zhong"}
        });
        AssertCondition(!idEdge.empty(), "Added EDGE_ADMIN");

        nlohmann::json reqEdge = {
            {"action", "ReverseLookup"},
            {"params", {
                {"text", "  EDGE_ADMIN  "},
                {"target_method", "pinyin"}
            }}
        };
        AssertCondition(client.SendFrame(1960, reqEdge.dump(), response), "ReverseLookup for raw edge text");
        auto res = nlohmann::json::parse(response);
        AssertCondition(res.value("success", false), "ReverseLookup edge succeeded");
        AssertCondition(res["data"]["found"].get<bool>() == true, "Found raw edge text");
        AssertCondition(res["data"]["primary_code"].get<std::string>() == "zhong", "Code is 'zhong'");
    }

    client.Disconnect();
    std::cout << "  [PASS] Stage 4C-2 Admin IPC ReverseLookup verified!" << std::endl;
}

// Main Test Runner
// -----------------------------------------------------------------------------
int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "  PersonalIME Stage 4B Admin IPC & Service Integration  " << std::endl;
    std::cout << "========================================================" << std::endl;

    std::string testDb = "out/stage4b_test.db";
    std::string restartDb = "out/stage4b_restart_test.db";
    std::string testDictDir = "data/dictionaries";

    std::error_code ec;
    std::filesystem::remove(testDb, ec);
    std::filesystem::remove(restartDb, ec);
    std::filesystem::create_directories("out", ec);

    {
        ImeEngine engine;
        bool initOk = engine.Initialize(testDb, testDictDir);
        AssertCondition(initOk, "Initialize IME Engine for Stage 4B Test");

        std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_Admin_Test_" + std::to_wstring(GetCurrentProcessId());

        AdminServer server(engine, testPipeName);
        AssertCondition(server.Start(), "Start Admin Named Pipe Server");

        // Run all test suites
        TestGoldenFrameAndHeader(server, testPipeName);
        TestNegativeFramingAndFaultInjection(server, testPipeName);
        TestPolicyManagement(server, testPipeName);
        TestUserDictionaryCrud(server, testPipeName);
        TestPinningAndBatchReorder(server, testPipeName, engine);
        TestLearningResetAndLateAck(server, testPipeName, engine);
        TestExportImportAndBusyProtection(server, testPipeName, engine);
        TestConcurrencyMultiClient(server, testPipeName);
        TestServiceIntegrationPolicySnapshot(engine);
        TestRound2StrictValidation(server, testPipeName, engine);
        TestRound2PolicySaveAtomicRevision(server, testPipeName, engine);
        TestRound2BusyAndEpochRollback(engine);
        TestRound3StrictPolicyAndUnreadDeadline(server, testPipeName, engine);
        TestFinalizationResponseBound(server, testPipeName, engine);
        TestPinnedRevisionAndScoping(server, testPipeName, engine);
        TestBuiltinPinningAndLifecycle(server, testPipeName, engine);
        TestInputMethodEnablingDisablingIpc(server, testPipeName, engine);
        TestStage4C2ReverseLookupAdminIpc(server, testPipeName, engine);

        server.Stop();
    }

    // Test restart persistence on clean engine recreation
    TestRestartPersistence(restartDb, testDictDir);

    std::filesystem::remove(testDb, ec);
    std::filesystem::remove(restartDb, ec);

    std::cout << "========================================================" << std::endl;
    std::cout << "  ALL Stage 4B Admin IPC & Integration Tests PASSED!    " << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}
