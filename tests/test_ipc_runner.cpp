#include <windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <cwchar>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include "../src/common/ime_guids.h"
#include "../src/common/ipc_protocol.h"
#include "../src/tsf_client/ipc_client.h"

LONG g_cDllRef = 0;

static DWORD GetTestTimeoutMs(DWORD defaultMs = 600) {
    const char* env = getenv("PERSONALIME_TEST_TIMEOUT_MS");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) return static_cast<DWORD>(v);
    }
    return defaultMs;
}

#define CHECK_TRUE(cond, desc) \
    do { \
        if (!(cond)) { \
            std::wcerr << L"[TEST FAILED] " << desc << L" at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (false)

int RunMultiThreadedTest() {
    std::wcout << L"[TEST] Running concurrent multi-threaded IPC client test..." << std::endl;
    std::atomic<bool> threadFailed = false;

    auto threadFunc = [&threadFailed](uint64_t sessionId, uint64_t contextId, wchar_t keyChar, const wchar_t* expectedComp) {
        IpcClient client;
        for (int i = 0; i < 5; ++i) {
            IpcKeyRequest req{};
            req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
            req.session_id = sessionId;
            req.context_id = contextId;
            req.vk_code = keyChar;

            IpcKeyResponse resp{};
            if (!client.SendRequest(req, resp, GetTestTimeoutMs())) {
                std::wcerr << L"[MT FAIL] SendRequest failed for session " << sessionId << L", context " << contextId << std::endl;
                threadFailed = true;
                return;
            }
            if (resp.session_id != sessionId || resp.context_id != contextId) {
                std::wcerr << L"[MT FAIL] Session/Context mismatch: expected " << sessionId << L"/" << contextId 
                           << L", got " << resp.session_id << L"/" << resp.context_id << std::endl;
                threadFailed = true;
                return;
            }
            if (wcscmp(resp.composition, expectedComp) != 0) {
                std::wcerr << L"[MT FAIL] Composition mismatch: expected " << expectedComp 
                           << L", got " << resp.composition << std::endl;
                threadFailed = true;
                return;
            }

            // Commit
            req.vk_code = VK_SPACE;
            req.candidate_version = resp.candidate_version;
            if (!client.SendRequest(req, resp, GetTestTimeoutMs())) {
                std::wcerr << L"[MT FAIL] Commit SendRequest failed for session " << sessionId << std::endl;
                threadFailed = true;
                return;
            }
            if (resp.has_commit != 1 || wcslen(resp.composition) != 0) {
                std::wcerr << L"[MT FAIL] Commit response invalid for session " << sessionId << std::endl;
                threadFailed = true;
                return;
            }
        }
    };

    std::thread t1(threadFunc, 1001, 10, 'A', L"a");
    std::thread t2(threadFunc, 2002, 20, 'B', L"b");

    t1.join();
    t2.join();

    if (threadFailed.load()) {
        std::wcerr << L"[TEST FAILED] Concurrent multi-threaded test encountered error!" << std::endl;
        return 1;
    }
    std::wcout << L"[TEST PASS] Concurrent multi-threaded IPC test completed with zero collisions!" << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--sizes") == 0) {
        std::cout << "REQUEST_SIZE=" << sizeof(IpcKeyRequest) << std::endl;
        std::cout << "RESPONSE_SIZE=" << sizeof(IpcKeyResponse) << std::endl;
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--test-timeout-budget") == 0) {
        IpcClient client;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        req.session_id = 999;
        req.context_id = 999;
        IpcKeyResponse resp{};

        ULONGLONG t0 = GetTickCount64();
        bool ok = client.SendRequest(req, resp, 60); // 60ms budget
        ULONGLONG elapsed = GetTickCount64() - t0;

        std::cout << "TIMEOUT_ELAPSED_MS=" << elapsed << " OK=" << (ok ? 1 : 0) << std::endl;
        return ok ? 0 : 1;
    }

    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    std::wcout << L"[TEST] Connecting to PersonalIME IPC service..." << std::endl;

    IpcClient client;

    // 1. Send Ping
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
    req.session_id = 1;
    req.context_id = 101;
    IpcKeyResponse resp{};
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"Ping SendRequest failed or timed out");
    CHECK_TRUE(resp.handled == 1, L"Ping response handled must be 1");
    CHECK_TRUE(resp.msg_type == static_cast<uint32_t>(IpcMsgType::Ping), L"Ping msg_type mismatch");
    std::wcout << L"[TEST PASS] Ping OK." << std::endl;

    // 2. Send KeyDown 'A' in Context 101
    req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
    req.session_id = 1;
    req.context_id = 101;
    req.vk_code = 'A';
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"KeyDown 'A' SendRequest failed");
    CHECK_TRUE(resp.handled == 1, L"KeyDown 'A' handled must be 1");
    CHECK_TRUE(resp.has_composition == 1, L"KeyDown 'A' has_composition must be 1");
    CHECK_TRUE(wcscmp(resp.composition, L"a") == 0, L"Composition text must be 'a'");
    CHECK_TRUE(resp.candidate_count > 0, L"Candidate count must be > 0");
    uint32_t ctx101Ver = resp.candidate_version;
    std::wcout << L"[TEST PASS] Context 101 -> Key 'A' -> Composition: " << resp.composition << L", candidates: " << resp.candidate_count << std::endl;

    // 3. Multi-Context Isolation: Send KeyDown 'B' in Context 102
    req.context_id = 102;
    req.vk_code = 'B';
    req.candidate_version = 0;
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"KeyDown 'B' in Context 102 failed");
    CHECK_TRUE(wcscmp(resp.composition, L"b") == 0, L"Context 102 composition must be 'b'");
    std::wcout << L"[TEST PASS] Context 102 -> Key 'B' -> Composition: " << resp.composition << std::endl;

    // 4. Commit Context 101 with Space -> Must commit candidate and not affect Context 102
    req.context_id = 101;
    req.vk_code = VK_SPACE;
    req.candidate_version = ctx101Ver;
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"Commit Context 101 failed");
    CHECK_TRUE(resp.has_commit == 1, L"Commit flag must be 1");
    CHECK_TRUE(wcslen(resp.commit_text) > 0, L"Commit text must not be empty");
    CHECK_TRUE(wcslen(resp.composition) == 0, L"Context 101 composition must be empty after commit");
    std::wcout << L"[TEST PASS] Context 101 -> Space -> Commit: " << resp.commit_text << std::endl;

    // 5. Verify Context 102 still has composition "b" when typing another char 'C' -> "bc"
    req.context_id = 102;
    req.vk_code = 'C';
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"Key 'C' in Context 102 failed");
    CHECK_TRUE(wcscmp(resp.composition, L"bc") == 0, L"Context 102 composition must be 'bc'");
    std::wcout << L"[TEST PASS] Context 102 remained isolated -> Composition: " << resp.composition << std::endl;

    // 6. Cancel Context 102 with Escape
    req.vk_code = VK_ESCAPE;
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"Cancel Context 102 failed");
    CHECK_TRUE(wcslen(resp.composition) == 0, L"Context 102 composition must be empty after Escape");
    std::wcout << L"[TEST PASS] Context 102 -> Cancelled OK." << std::endl;

    // 7. Rollback Test: type 'D', send Rollback, verify reverted
    req.context_id = 103;
    req.vk_code = 'D';
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"Key 'D' failed");
    CHECK_TRUE(wcscmp(resp.composition, L"d") == 0, L"Composition must be 'd'");

    req.msg_type = static_cast<uint32_t>(IpcMsgType::Rollback);
    CHECK_TRUE(client.SendRequest(req, resp, GetTestTimeoutMs()), L"Rollback request failed");
    CHECK_TRUE(wcslen(resp.composition) == 0, L"Composition must be empty after rollback of first char");
    std::wcout << L"[TEST PASS] Rollback verified successfully." << std::endl;

    // 8. Run concurrent multi-threaded isolation test
    int mtResult = RunMultiThreadedTest();
    if (mtResult != 0) return mtResult;

    // 9. OpenManager IPC Test: verify OpenManager IPC routing & window restore
    {
        HWND hDummyMgr = CreateWindowExW(
            0, L"STATIC", L"PersonalIME 管理器",
            WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
        );
        CHECK_TRUE(hDummyMgr != nullptr, L"Create dummy manager window failed");
        ShowWindow(hDummyMgr, SW_SHOWMINIMIZED);

        bool openOk = client.OpenManager(100, 104);
        CHECK_TRUE(openOk, L"OpenManager IPC call must succeed when manager window exists");

        // Pump message queue to allow asynchronous show/restore to process
        MSG msg;
        for (int i = 0; i < 20 && IsIconic(hDummyMgr); ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        CHECK_TRUE(!IsIconic(hDummyMgr), L"Manager window must be restored from minimized by OpenManager");

        DestroyWindow(hDummyMgr);
        std::wcout << L"[TEST PASS] OpenManager IPC routing and window restore verified successfully." << std::endl;
    }

    std::wcout << L"[TEST COMPLETE] All IPC pipe protocol & context isolation tests passed successfully!" << std::endl;
    return 0;
}
