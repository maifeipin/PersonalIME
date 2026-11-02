#include <windows.h>
#include <sddl.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include "../common/ime_guids.h"
#include "../common/ipc_protocol.h"

// Retrieve the exact Current User SID string at runtime
static std::wstring GetCurrentUserSidString() {
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

struct ContextState {
    std::wstring buffer;
    std::wstring prevBuffer; // for rollback support
};

// Handles a single client pipe connection with multi-context isolation
void HandleClient(HANDLE hPipe) {
    std::map<std::pair<uint64_t, uint64_t>, ContextState> contextStates;
    DWORD bytesRead = 0;
    DWORD bytesWritten = 0;

    std::wcout << L"[ime-service] Client connected to pipe." << std::endl;

    while (true) {
        IpcKeyRequest req{};
        BOOL success = ReadFile(hPipe, &req, sizeof(req), &bytesRead, nullptr);
        if (!success || bytesRead != sizeof(req)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || bytesRead == 0) {
                std::wcout << L"[ime-service] Client disconnected." << std::endl;
            } else {
                std::wcout << L"[ime-service] Pipe read error or invalid length (" << bytesRead << L" bytes): " << err << std::endl;
            }
            break;
        }

        auto contextKey = std::make_pair(req.session_id, req.context_id);
        auto& state = contextStates[contextKey];

        IpcKeyResponse resp{};
        resp.msg_type = req.msg_type;
        resp.session_id = req.session_id;
        resp.context_id = req.context_id;

        if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Ping)) {
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Reset) ||
                 req.msg_type == static_cast<uint32_t>(IpcMsgType::ContextLost)) {
            state.buffer.clear();
            state.prevBuffer.clear();
            resp.handled = 1;
            resp.has_composition = 1;
            resp.composition[0] = L'\0';
            std::wcout << L"[ime-service] Reset state for session " << req.session_id << L", context " << req.context_id << std::endl;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Rollback)) {
            // Edit failed on host side: restore previous buffer
            state.buffer = state.prevBuffer;
            resp.handled = 1;
            resp.has_composition = 1;
            wcsncpy_s(resp.composition, state.buffer.c_str(), _TRUNCATE);
            if (state.buffer.empty()) {
                resp.candidate_count = 0;
            } else {
                resp.candidate_count = 2;
                wcsncpy_s(resp.candidates[0], L"1. 测试", _TRUNCATE);
                wcsncpy_s(resp.candidates[1], L"2. 验证", _TRUNCATE);
            }
            std::wcout << L"[ime-service] Rollback applied for session " << req.session_id << L" -> restored buffer: " << state.buffer << std::endl;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::TestKeyDown)) {
            uint32_t vk = req.vk_code;
            if ((vk >= 'A' && vk <= 'Z') || 
                (!state.buffer.empty() && (vk == VK_SPACE || (vk >= '1' && vk <= '9') || vk == VK_BACK || vk == VK_ESCAPE))) {
                resp.handled = 1;
            } else {
                resp.handled = 0;
            }
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::KeyDown)) {
            uint32_t vk = req.vk_code;
            state.prevBuffer = state.buffer;

            if (vk >= 'A' && vk <= 'Z') {
                wchar_t ch = static_cast<wchar_t>(L'a' + (vk - 'A'));
                state.buffer.push_back(ch);
                resp.handled = 1;
                resp.has_composition = 1;
                wcsncpy_s(resp.composition, state.buffer.c_str(), _TRUNCATE);

                resp.candidate_count = 2;
                wcsncpy_s(resp.candidates[0], L"1. 测试", _TRUNCATE);
                wcsncpy_s(resp.candidates[1], L"2. 验证", _TRUNCATE);

                std::wcout << L"[ime-service] [Session " << req.session_id << L"] Key: " << ch 
                           << L" -> Composition: " << state.buffer << std::endl;
            }
            else if (!state.buffer.empty() && (vk == VK_SPACE || vk == '1')) {
                // Commit first candidate
                resp.handled = 1;
                resp.has_commit = 1;
                wcsncpy_s(resp.commit_text, L"测试", _TRUNCATE);
                state.buffer.clear();
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                std::wcout << L"[ime-service] [Session " << req.session_id << L"] Commit: 测试" << std::endl;
            }
            else if (!state.buffer.empty() && vk == '2') {
                // Commit second candidate
                resp.handled = 1;
                resp.has_commit = 1;
                wcsncpy_s(resp.commit_text, L"验证", _TRUNCATE);
                state.buffer.clear();
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                std::wcout << L"[ime-service] [Session " << req.session_id << L"] Commit: 验证" << std::endl;
            }
            else if (!state.buffer.empty() && vk == VK_BACK) {
                state.buffer.pop_back();
                resp.handled = 1;
                resp.has_composition = 1;
                wcsncpy_s(resp.composition, state.buffer.c_str(), _TRUNCATE);
                if (state.buffer.empty()) {
                    resp.candidate_count = 0;
                } else {
                    resp.candidate_count = 2;
                    wcsncpy_s(resp.candidates[0], L"1. 测试", _TRUNCATE);
                    wcsncpy_s(resp.candidates[1], L"2. 验证", _TRUNCATE);
                }
                std::wcout << L"[ime-service] [Session " << req.session_id << L"] Backspace -> Composition: " << state.buffer << std::endl;
            }
            else if (!state.buffer.empty() && vk == VK_ESCAPE) {
                state.buffer.clear();
                resp.handled = 1;
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.candidate_count = 0;
                std::wcout << L"[ime-service] [Session " << req.session_id << L"] Cancel composition." << std::endl;
            }
            else {
                resp.handled = 0;
            }
        }

        WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
        FlushFileBuffers(hPipe);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wcout << L"========================================\n";
    std::wcout << L"  Personal IME Background Service Prototype\n";
    std::wcout << L"========================================\n";
    std::wcout << L"[ime-service] Pipe Name: " << IME_PIPE_NAME << L"\n";

    // Strictly scoped security descriptor using actual Current User SID at runtime:
    std::wstring userSid = GetCurrentUserSidString();
    if (userSid.empty()) {
        std::wcout << L"[ime-service] Warning: Failed to query user SID, defaulting to Interactive Users (IU)\n";
        userSid = L"IU";
    }

    std::wstring sddl = L"D:(A;;GA;;;" + userSid + L")(A;;GA;;;AC)(A;;GA;;;BA)(A;;GA;;;SY)";
    std::wcout << L"[ime-service] Configured SDDL: " << sddl << L"\n";

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &pSD,
            nullptr)) {
        std::wcout << L"[ime-service] Failed to create security descriptor! Error: "
                   << GetLastError() << L"\n";
        return 1;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    std::wcout << L"[ime-service] Listening for TSF Client connections (PIPE_REJECT_REMOTE_CLIENTS enabled)...\n";

    while (true) {
        // Enforce PIPE_REJECT_REMOTE_CLIENTS to block all non-local connections
        HANDLE hPipe = CreateNamedPipeW(
            IME_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(IpcKeyResponse) * 2,
            sizeof(IpcKeyRequest) * 2,
            50,
            &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::wcout << L"[ime-service] CreateNamedPipe failed: " << GetLastError() << L"\n";
            break;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            std::thread(HandleClient, hPipe).detach();
        } else {
            CloseHandle(hPipe);
        }
    }

    if (pSD) {
        LocalFree(pSD);
    }

    return 0;
}
