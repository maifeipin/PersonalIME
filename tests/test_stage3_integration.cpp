#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cassert>
#include "../src/common/ime_guids.h"
#include "../src/common/ipc_protocol.h"
#include "../src/tsf_client/ipc_client.h"
#include "../src/core/engine/ime_engine.h"
#include "../src/core/types.h"
#include "../src/core/storage/sqlite_db.h"
#define private public
#include "../src/tsf_client/text_input_processor.h"
#undef private

LONG g_cDllRef = 0;
HINSTANCE g_hInst = nullptr;

#define CHECK_TRUE(cond, desc) \
    do { \
        if (!(cond)) { \
            std::wcerr << L"[STAGE3 TEST FAILED] " << desc << L" at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (false)

#define CHECK_EQUAL(a, b, desc) \
    do { \
        if ((a) != (b)) { \
            std::wcerr << L"[STAGE3 TEST FAILED] " << desc << L" (expected " << (b) << L", got " << (a) << L") at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (false)

static int QueryStatsCount(const std::string& dbPath, const std::string& code) {
    ime::SqliteDb db;
    if (!db.Open(dbPath)) return -1;
    auto stmt = db.Prepare("SELECT COALESCE(SUM(selected_count),0) FROM entry_stats WHERE code=?;");
    if (!stmt) return -1;
    stmt->BindText(1, code);
    if (!stmt->Step()) return -1;
    return stmt->ColumnInt(0);
}

struct ServiceProcess {
    PROCESS_INFORMATION pi{};
    void Stop() {
        if (pi.hProcess) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
    }
    ~ServiceProcess() {
        Stop();
    }
    bool Start(const std::string& dbPath, const std::string& dictDir) {
        std::wstring serviceExe = L"build\\Release\\ime-service.exe";
        if (!std::filesystem::exists(serviceExe)) serviceExe = L"ime-service.exe";

        std::wstring cmdLine = serviceExe + L" --db " + ime::Utf8ToWide(dbPath) + L" --dict " + ime::Utf8ToWide(dictDir);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!ok) return false;

        IpcClient c;
        for (int i = 0; i < 60; ++i) {
            Sleep(50);
            IpcKeyRequest req{};
            req.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
            req.session_id = 1;
            req.context_id = 1;
            req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
            IpcKeyResponse resp{};
            if (c.SendRequest(req, resp, 80) && resp.handled == 1) {
                return true;
            }
        }
        Stop();
        return false;
    }
};

int main(int argc, char* argv[]) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    SetConsoleOutputCP(CP_UTF8);
    std::wcout << L"=====================================================\n";
    std::wcout << L"  PersonalIME Stage 3 End-to-End Integration Tests\n";
    std::wcout << L"=====================================================\n";

    std::string testDb = "stage3_integration_test.db";
    std::string dictDir = "data/dictionaries";

    // Clean up previous test database if present
    if (std::filesystem::exists(testDb)) {
        std::error_code ec;
        std::filesystem::remove(testDb, ec);
    }

    // Seed test custom phrases into the test database before service launch
    {
        ime::ImeEngine seeder;
        if (!seeder.Initialize(testDb, dictDir)) {
            std::wcerr << L"[FATAL] Failed to initialize seeder engine on test DB: " << ime::Utf8ToWide(testDb) << std::endl;
            return 1;
        }

        // Add custom phrases:
        // 1. Address
        seeder.AddUserEntry(L"北京市海淀区中关村南大街1号", { {ime::InputScheme::Custom, ";dz"} });
        // 2. Multi-line snippet
        seeder.AddUserEntry(L"第一行代码\r\n第二行代码\r\n第三行代码", { {ime::InputScheme::Custom, ";dh"} });
        // 3. Emoji with surrogate pairs
        seeder.AddUserEntry(L"庆祝🎉点赞👍火箭🚀", { {ime::InputScheme::Custom, ";bq"} });
        // 4. Long custom phrase for truncation testing
        std::wstring longText(123, L'x');
        longText += L"😀tail";
        seeder.AddUserEntry(longText, { {ime::InputScheme::Custom, ";long"} });
    }

    ServiceProcess service;
    CHECK_TRUE(service.Start(testDb, dictDir), L"Failed to start ime-service");
    std::wcout << L"[PASS] Connected to live ime-service!" << std::endl;

    IpcClient client;

    // -------------------------------------------------------------
    // Test 1: Real Dictionary Query & Wubi86 Matching
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 1] Testing Real Dictionary Query & Wubi86 Matching..." << std::endl;
    {
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = 1001;
        IpcKeyResponse resp{};

        // Type 'a'
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'A' failed");
        CHECK_TRUE(resp.handled == 1, L"Key 'A' must be eaten");
        CHECK_TRUE(wcscmp(resp.composition, L"a") == 0, L"Composition must be 'a'");
        CHECK_TRUE(resp.candidate_count > 0, L"Candidate count must be > 0");
        std::wcout << L"  'a' candidates count: " << resp.candidate_count << L", top: " << resp.candidates[0] << std::endl;

        // Type 'a' -> 'aa'
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'A' 2 failed");
        CHECK_TRUE(wcscmp(resp.composition, L"aa") == 0, L"Composition must be 'aa'");

        // Type 'a' -> 'aaa'
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'A' 3 failed");
        CHECK_TRUE(wcscmp(resp.composition, L"aaa") == 0, L"Composition must be 'aaa'");

        // Type 'a' -> 'aaaa'
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'A' 4 failed");
        CHECK_TRUE(wcscmp(resp.composition, L"aaaa") == 0, L"Composition must be 'aaaa'");
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates for 'aaaa' must exist");
        CHECK_TRUE(wcsstr(resp.candidates[0], L"工") != nullptr, L"First candidate for 'aaaa' must be '工'");
        std::wcout << L"  'aaaa' top candidate: " << resp.candidates[0] << std::endl;

        // Space commit
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Space commit failed");
        CHECK_TRUE(resp.has_commit == 1, L"Commit flag must be set");
        CHECK_TRUE(wcscmp(resp.commit_text, L"工") == 0, L"Committed text must be '工'");
        CHECK_TRUE(resp.commit_event_id != 0, L"Commit event ID must be non-zero");
        CHECK_TRUE(wcslen(resp.composition) == 0, L"Composition must be empty after commit");

        // Notify commit success
        CHECK_TRUE(client.NotifyCommitSuccess(100, 1001, resp.commit_event_id), L"NotifyCommitSuccess failed");
        std::wcout << L"[PASS] Wubi86 'aaaa' -> '工' successfully queried and committed!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 2: Pinyin Mode & Syllable Separation (xian vs xi'an)
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 2] Testing Pinyin Scheme & Ambiguity/Separators..." << std::endl;
    {
        uint64_t ctx = 1002;
        CHECK_TRUE(client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_PINYIN | IPC_MODE_CHINESE_PUNCT), L"SetMode Pinyin failed");

        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // Type "zhongwen"
        const char* py = "zhongwen";
        for (const char* p = py; *p; ++p) {
            req.vk_code = static_cast<uint32_t>(toupper(*p));
            CHECK_TRUE(client.SendRequest(req, resp, 80), L"Pinyin keystroke failed");
        }
        CHECK_TRUE(wcscmp(resp.composition, L"zhongwen") == 0, L"Composition must be 'zhongwen'");
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates must exist for 'zhongwen'");
        CHECK_TRUE(wcsstr(resp.candidates[0], L"中文") != nullptr, L"Top candidate for 'zhongwen' must be '中文'");
        std::wcout << L"  'zhongwen' candidates: " << resp.candidates[0] << std::endl;

        // Space commit "中文"
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Space commit for Pinyin failed");
        CHECK_TRUE(wcscmp(resp.commit_text, L"中文") == 0, L"Committed text must be '中文'");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, resp.commit_event_id), L"CommitSuccess failed");

        // Test xi'an separator
        const char* xianApos = "xi'an";
        for (const char* p = xianApos; *p; ++p) {
            if (*p == '\'') req.vk_code = VK_OEM_7;
            else req.vk_code = static_cast<uint32_t>(toupper(*p));
            CHECK_TRUE(client.SendRequest(req, resp, 80), L"Apostrophe keystroke failed");
        }
        CHECK_TRUE(wcscmp(resp.composition, L"xi'an") == 0, L"Composition must be 'xi\\'an'");
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates for xi'an must exist");
        CHECK_TRUE(wcsstr(resp.candidates[0], L"西安") != nullptr, L"Top candidate for xi'an must be '西安'");
        std::wcout << L"  'xi\\'an' top candidate: " << resp.candidates[0] << std::endl;

        // Clear composition via Esc
        req.vk_code = VK_ESCAPE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Escape failed");
        CHECK_TRUE(wcslen(resp.composition) == 0, L"Composition must be cleared on Esc");
        std::wcout << L"[PASS] Pinyin 'zhongwen' -> '中文' and 'xi\\'an' -> '西安' verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 3: Custom Phrases, Long Strings, Multi-line & Emoji
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 3] Testing Custom Phrases, Multi-line & Emoji..." << std::endl;
    {
        uint64_t ctx = 1003;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // 1. Test Address: ";dz"
        req.vk_code = VK_OEM_1; // ';'
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key ';' failed");
        CHECK_TRUE(wcscmp(resp.composition, L";") == 0, L"Composition must be ';'");

        req.vk_code = 'D';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'D' failed");
        req.vk_code = 'Z';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'Z' failed");
        CHECK_TRUE(wcscmp(resp.composition, L";dz") == 0, L"Composition must be ';dz'");
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates must exist for ';dz'");
        CHECK_TRUE(wcsstr(resp.candidates[0], L"北京市海淀区中关村南大街1号") != nullptr, L"Candidate text mismatch");
        std::wcout << L"  ';dz' candidate: " << resp.candidates[0] << std::endl;

        // Commit address via Space
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Commit ';dz' failed");
        CHECK_TRUE(wcscmp(resp.commit_text, L"北京市海淀区中关村南大街1号") == 0, L"Commit text mismatch for address");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, resp.commit_event_id), L"CommitSuccess failed");

        // 2. Test Multi-line: ";dh"
        req.vk_code = VK_OEM_1;
        client.SendRequest(req, resp, 80);
        req.vk_code = 'D';
        client.SendRequest(req, resp, 80);
        req.vk_code = 'H';
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates must exist for ';dh'");

        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Commit ';dh' failed");
        CHECK_TRUE(wcscmp(resp.commit_text, L"第一行代码\r\n第二行代码\r\n第三行代码") == 0, L"Multi-line commit mismatch");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, resp.commit_event_id), L"CommitSuccess failed");
        std::wcout << L"  ';dh' committed multi-line text cleanly!" << std::endl;

        // 3. Test Emoji: ";bq" -> "庆祝🎉点赞👍火箭🚀"
        req.vk_code = VK_OEM_1;
        client.SendRequest(req, resp, 80);
        req.vk_code = 'B';
        client.SendRequest(req, resp, 80);
        req.vk_code = 'Q';
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates must exist for ';bq'");

        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Commit ';bq' failed");
        CHECK_TRUE(wcscmp(resp.commit_text, L"庆祝🎉点赞👍火箭🚀") == 0, L"Emoji surrogate pairs mismatch");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, resp.commit_event_id), L"CommitSuccess failed");
        std::wcout << L"  ';bq' committed emojis with intact surrogate pairs: " << resp.commit_text << std::endl;
        std::wcout << L"[PASS] Custom phrases (address, multi-line, emoji) passed cleanly!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 4: Pagination & Number Candidate Selection
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 4] Testing Candidate Pagination & Number Selection..." << std::endl;
    {
        uint64_t ctx = 1004;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // Type 'a' (has multiple pages)
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Key 'A' failed");
        CHECK_TRUE(resp.page_index == 0, L"Initial page must be 0");
        CHECK_TRUE(resp.page_count > 1, L"Total pages for 'a' must be > 1");
        std::wstring page0_cand1 = resp.candidates[1]; // 2nd candidate on page 0
        std::wcout << L"  Page 0: count=" << resp.candidate_count << L", cand[1]=" << page0_cand1 << std::endl;

        // Page Down via '.'
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_OEM_PERIOD;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"PageDown failed");
        CHECK_TRUE(resp.page_index == 1, L"Page index after PageDown must be 1");
        std::wcout << L"  Page 1: count=" << resp.candidate_count << L", top=" << resp.candidates[0] << std::endl;

        // Page Up via ','
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_OEM_COMMA;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"PageUp failed");
        CHECK_TRUE(resp.page_index == 0, L"Page index after PageUp must return to 0");

        // Select candidate 2 with number key '2'
        req.candidate_version = resp.candidate_version;
        req.vk_code = '2';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Number '2' selection failed");
        CHECK_TRUE(resp.has_commit == 1, L"Commit flag must be set for candidate 2");
        CHECK_TRUE(wcslen(resp.commit_text) > 0, L"Commit text must not be empty");
        CHECK_TRUE(wcsstr(page0_cand1.c_str(), resp.commit_text) != nullptr, L"Committed text must match candidate 2");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, resp.commit_event_id), L"CommitSuccess failed");
        std::wcout << L"  Selected candidate 2 via key '2': " << resp.commit_text << std::endl;
        std::wcout << L"[PASS] Pagination and candidate number selection verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 5: Commit Learning Guard & Idempotency
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 5] Testing Commit Learning Guard & Idempotency..." << std::endl;
    {
        uint64_t ctx = 1005;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // 1. Non-commit cancellation (Esc): must NOT learn
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcslen(resp.composition) == 0, L"Esc must clear composition");

        // 2. Commit candidate and learn
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80); // "aaaa"
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;            // commit "工"
        client.SendRequest(req, resp, 80);
        uint64_t evId = resp.commit_event_id;
        CHECK_TRUE(evId != 0, L"Event ID must be non-zero");

        // Send CommitSuccess once
        int beforeSuccess = QueryStatsCount(testDb, "aaaa");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, evId), L"CommitSuccess 1 failed");
        int afterSuccess = QueryStatsCount(testDb, "aaaa");
        CHECK_EQUAL(afterSuccess, beforeSuccess + 1, L"CommitSuccess must increment learning count once");

        // Send CommitSuccess a SECOND time with SAME event_id (idempotency replay)
        // Must return true (idempotently acknowledged) WITHOUT incrementing count!
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, evId), L"Idempotent retry of CommitSuccess must succeed");
        int afterReplay = QueryStatsCount(testDb, "aaaa");
        CHECK_EQUAL(afterReplay, afterSuccess, L"Idempotent replay must not increment learning count again");

        // 3. Rollback / Cancel learning test
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t cancelEvId = resp.commit_event_id;
        CHECK_TRUE(cancelEvId != 0, L"Event ID for cancel test must be non-zero");
        // Send CommitCancel
        CHECK_TRUE(client.NotifyCommitCancel(100, ctx, cancelEvId), L"NotifyCommitCancel failed");
        std::wcout << L"  CommitCancel notification handled without learning." << std::endl;
        std::wcout << L"[PASS] Commit learning guard & idempotency verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 6: Multi-Context Isolation (Wubi vs Pinyin concurrent)
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 6] Testing Multi-Context Scheme & State Isolation..." << std::endl;
    {
        uint64_t ctxA = 2001; // Wubi context
        uint64_t ctxB = 2002; // Pinyin context

        client.SetMode(100, ctxA, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        client.SetMode(100, ctxB, IPC_MODE_CHINESE | IPC_MODE_PINYIN | IPC_MODE_CHINESE_PUNCT);

        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        IpcKeyResponse resp{};

        // Type 'a' in Context A (Wubi)
        req.context_id = ctxA;
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Context A Key 'A' failed");
        CHECK_TRUE(wcscmp(resp.composition, L"a") == 0, L"Context A comp must be 'a'");
        CHECK_TRUE(wcsstr(resp.candidates[0], L"工") != nullptr, L"Context A top candidate must be '工'");

        // Type 'b' in Context B (Pinyin)
        req.context_id = ctxB;
        req.vk_code = 'B';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Context B Key 'B' failed");
        CHECK_TRUE(wcscmp(resp.composition, L"b") == 0, L"Context B comp must be 'b'");
        CHECK_TRUE(wcsstr(resp.candidates[0], L"不") != nullptr || resp.candidate_count > 0, L"Context B candidates must exist");

        // Commit Context A
        req.context_id = ctxA;
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Commit Context A failed");
        CHECK_TRUE(wcscmp(resp.commit_text, L"工") == 0, L"Context A must commit '工'");
        CHECK_TRUE(wcslen(resp.composition) == 0, L"Context A comp must be empty");

        // Check Context B is completely intact
        req.context_id = ctxB;
        req.vk_code = 'A'; // 'ba'
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Context B Key 'A' failed");
        CHECK_TRUE(wcscmp(resp.composition, L"ba") == 0, L"Context B comp must be 'ba'");

        // Cancel Context B
        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);
        std::wcout << L"[PASS] Multi-context cross-scheme isolation verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 7: Mid-Composition Scheme Switch (Cancellation Safety)
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 7] Testing Mid-Composition Mode Switch Cancellation..." << std::endl;
    {
        uint64_t ctx = 3001;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // Type partial code 'z'
        req.vk_code = 'Z';
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcscmp(resp.composition, L"z") == 0, L"Composition must be 'z'");

        // Send Ctrl+Shift to toggle scheme mid-composition
        req.vk_code = VK_SHIFT;
        req.modifiers = 2; // Ctrl held
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Ctrl+Shift toggle failed");
        CHECK_TRUE(resp.handled == 1, L"Ctrl+Shift must be handled");
        CHECK_TRUE(wcslen(resp.composition) == 0, L"Composition must be safely cleared on scheme toggle!");
        CHECK_TRUE(resp.candidate_count == 0, L"Candidate window must be closed");
        std::wcout << L"[PASS] Mid-composition scheme toggle cleanly cancelled and cleared composition!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 8: Review Probe Assertions (Prefix Learning & Separator Keys)
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 8] Testing Prefix & Separator Real Database Learning..." << std::endl;
    {
        uint64_t ctx = 4001;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // 1. Prefix 'a' commit candidate 2
        int beforePrefix = QueryStatsCount(testDb, "a");
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.candidate_version = resp.candidate_version;
        req.vk_code = '2';
        client.SendRequest(req, resp, 80);
        uint64_t prefixToken = resp.commit_event_id;
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, prefixToken), L"Prefix CommitSuccess failed");

        int prefixCount = QueryStatsCount(testDb, "a");
        CHECK_EQUAL(prefixCount, beforePrefix + 1, L"Prefix 'a' learning count must increase by 1");
        std::wcout << L"  Prefix 'a' learning recorded under code 'a': before=" << beforePrefix << L", after=" << prefixCount << std::endl;

        // 2. Separator 'xi'an'
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_PINYIN | IPC_MODE_CHINESE_PUNCT);
        const char* sCode = "xi'an";
        for (const char* p = sCode; *p; ++p) {
            req.vk_code = (*p == '\'') ? VK_OEM_7 : static_cast<uint32_t>(toupper(*p));
            client.SendRequest(req, resp, 80);
        }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t sepToken = resp.commit_event_id;
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, sepToken), L"Separator CommitSuccess failed");

        int sepCount = QueryStatsCount(testDb, "xi'an");
        int spacedCount = QueryStatsCount(testDb, "xi an");
        CHECK_EQUAL(sepCount, 1, L"Separator 'xi\\'an' learning count must be exactly 1");
        CHECK_EQUAL(spacedCount, 0, L"Spaced 'xi an' learning count must be exactly 0");
        std::wcout << L"  Separator 'xi\\'an' learning recorded: xi'an=" << sepCount << L", spaced='xi an'=" << spacedCount << std::endl;

        // 3. CommitCancel restores composition
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT); // Wubi
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t cancelToken = resp.commit_event_id;
        CHECK_TRUE(client.NotifyCommitCancel(100, ctx, cancelToken), L"CommitCancel failed");

        // Next key 'B' should append to restored 'a' -> 'ab'!
        req.vk_code = 'B';
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcscmp(resp.composition, L"ab") == 0, L"Composition after Cancel must be 'ab'");
        std::wcout << L"  CommitCancel cleanly restored composition: 'a' + 'b' -> " << resp.composition << std::endl;
        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);

        // 4. SetMode cancels pending composition
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcscmp(resp.composition, L"a") == 0, L"Comp must be 'a'");
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_PINYIN);
        req.vk_code = 'B';
        client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcscmp(resp.composition, L"b") == 0, L"Composition after SetMode must be 'b' without mixing!");
        std::wcout << L"  SetMode cleanly reset composition without cross-scheme mixing: " << resp.composition << std::endl;

        std::wcout << L"[PASS] Review probe prefix, separator, cancel and mode assertions passed!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 9: Restart Service & Event ID Uniqueness Across Restarts
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 9] Testing Monotonic Event ID Uniqueness Across Service Restarts..." << std::endl;
    {
        uint64_t ctx = 5001;
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // Type 'aaaa' and commit
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t token1 = resp.commit_event_id;
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, token1), L"Token 1 ack failed");

        int countBefore = QueryStatsCount(testDb, "aaaa");
        std::wcout << L"  Run 1 commit 'aaaa': token=" << token1 << L", count=" << countBefore << std::endl;

        // Disconnect and restart the service against the same testDb!
        client.Disconnect();
        service.Stop();
        CHECK_TRUE(service.Start(testDb, dictDir), L"Failed to restart service");

        // Type 'aaaa' and commit again in fresh service process
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t token2 = resp.commit_event_id;
        CHECK_TRUE(token2 != token1, L"Event ID must be strictly unique across restarts!");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, token2), L"Token 2 ack failed");

        int countAfter = QueryStatsCount(testDb, "aaaa");
        CHECK_EQUAL(countAfter, countBefore + 1, L"Commit after restart must increment learning count");
        std::wcout << L"  Run 2 commit 'aaaa': token=" << token2 << L", count=" << countAfter << std::endl;
        std::wcout << L"[PASS] Service restart event ID uniqueness verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 10: Delayed & Out-of-Order ACKs
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 10] Testing Delayed & Out-of-Order ACKs..." << std::endl;
    {
        uint64_t ctx = 6001;
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // Type "aaaa" and select (Token A)
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t tokenA = resp.commit_event_id;

        int beforeA = QueryStatsCount(testDb, "aaaa");

        // Immediately type "b" and select (Token B) BEFORE confirming Token A
        req.vk_code = 'B';
        client.SendRequest(req, resp, 80);
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t tokenB = resp.commit_event_id;
        CHECK_TRUE(tokenB != tokenA, L"Token B must differ from Token A");

        // 1. True out-of-order ACK: Confirm Token B FIRST, then Token A!
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, tokenB), L"Out-of-order ack for Token B failed");
        int countB = QueryStatsCount(testDb, "b");
        CHECK_EQUAL(countB, 1, L"Token B must be recorded first");

        // Now confirm Token A afterwards!
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, tokenA), L"Out-of-order ack for Token A failed");
        int afterA = QueryStatsCount(testDb, "aaaa");
        CHECK_EQUAL(afterA, beforeA + 1, L"Token A must be recorded despite subsequent selection & out-of-order confirmation");

        // 2. Interleaved commit & cancel in true arbitrary order
        // Create Token C ("aaaa") and Token D ("b")
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE; client.SendRequest(req, resp, 80);
        uint64_t tokenC = resp.commit_event_id;

        req.vk_code = 'B'; client.SendRequest(req, resp, 80);
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE; client.SendRequest(req, resp, 80);
        uint64_t tokenD = resp.commit_event_id;

        // Cancel C, Confirm D in reverse order
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctx, tokenD), L"Ack for Token D failed");
        CHECK_TRUE(client.NotifyCommitCancel(100, ctx, tokenC), L"Cancel for Token C failed");

        int countB_after = QueryStatsCount(testDb, "b");
        int countA_after = QueryStatsCount(testDb, "aaaa");
        CHECK_EQUAL(countB_after, countB + 1, L"Token D must increment b count");
        CHECK_EQUAL(countA_after, afterA, L"Cancelled Token C must not increment aaaa count");

        std::wcout << L"  Delayed & Out-of-order ACKs for Token A, B, C, D verified." << std::endl;
        std::wcout << L"[PASS] Delayed & out-of-order ACKs verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 11: Shift, Arrow Keys, Full-Width Digits & Protocol Checks
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 11] Testing Shift, Arrow Keys, Full-Width Digits & Protocol Checks..." << std::endl;
    {
        uint64_t ctx = 7001;
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctx;
        IpcKeyResponse resp{};

        // 1. Arrow keys during composition must NOT be eaten
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.vk_code = VK_LEFT;
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"VK_LEFT during composition must NOT be eaten");
        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);

        // 2. Standalone Shift key must NOT clear composition or flip mode on KeyDown
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        req.msg_type = static_cast<uint32_t>(IpcMsgType::TestKeyDown);
        req.vk_code = VK_SHIFT;
        req.modifiers = 1;
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"Shift TestKeyDown must not be eaten");

        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"Shift KeyDown must not be eaten");
        CHECK_TRUE((resp.status_flags & IPC_MODE_CHINESE) != 0, L"Chinese mode must be preserved on Shift down");
        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);

        // 2b. CapsLock (VK_CAPITAL) and Ctrl+Space switching to English commits uncommitted raw composition!
        req.modifiers = 0;
        req.vk_code = 'H'; client.SendRequest(req, resp, 80);
        req.vk_code = 'I'; client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcscmp(resp.composition, L"hi") == 0, L"Composition is 'hi'");

        // TestKeyDown for VK_CAPITAL must be handled
        req.msg_type = static_cast<uint32_t>(IpcMsgType::TestKeyDown);
        req.vk_code = VK_CAPITAL;
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"VK_CAPITAL TestKeyDown must be handled");

        // KeyDown for VK_CAPITAL switches to English and commits raw 'hi'
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"VK_CAPITAL KeyDown must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"VK_CAPITAL with active composition must commit raw composition");
        CHECK_TRUE(wcscmp(resp.commit_text, L"hi") == 0, L"VK_CAPITAL must commit 'hi'");
        CHECK_TRUE(wcslen(resp.composition) == 0, L"Composition cleared after committing raw input");
        CHECK_EQUAL(resp.status_flags & IPC_MODE_CHINESE, 0u, L"Context switched to English mode on CapsLock");

        // 2c. Verify English mode lowercase default and Shift uppercase assistance
        req.msg_type = static_cast<uint32_t>(IpcMsgType::TestKeyDown);
        req.vk_code = 'A';
        req.modifiers = 0;
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"TestKeyDown for 'A' in English mode must be handled");

        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"KeyDown for 'A' in English mode must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"English 'A' must commit");
        CHECK_TRUE(wcscmp(resp.commit_text, L"a") == 0, L"English 'A' without Shift must commit lowercase 'a'");

        // Shift + 'A' must commit uppercase 'A'
        req.modifiers = 1; // Shift
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"KeyDown for Shift+'A' in English mode must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"Shift+'A' must commit");
        CHECK_TRUE(wcscmp(resp.commit_text, L"A") == 0, L"Shift+'A' must commit uppercase 'A'");

        // Shortcuts like Ctrl+C must NOT be handled (pass through to OS)
        req.modifiers = 2; // Ctrl
        req.vk_code = 'C';
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"Ctrl+C in English mode must pass through (handled = 0)");
        req.modifiers = 0;

        // Press VK_CAPITAL again to return to Chinese mode
        client.SendRequest(req, resp, 80);
        CHECK_TRUE((resp.status_flags & IPC_MODE_CHINESE) != 0, L"Second VK_CAPITAL returns to Chinese mode");

        // Type 'ok' and press Ctrl+Space -> commits 'ok' and switches to English
        req.vk_code = 'O'; client.SendRequest(req, resp, 80);
        req.vk_code = 'K'; client.SendRequest(req, resp, 80);
        CHECK_TRUE(wcscmp(resp.composition, L"ok") == 0, L"Composition is 'ok'");
        req.vk_code = VK_SPACE;
        req.modifiers = 2; // Ctrl+Space
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"Ctrl+Space handled");
        CHECK_EQUAL(resp.has_commit, 1, L"Ctrl+Space with active composition must commit raw 'ok'");
        CHECK_TRUE(wcscmp(resp.commit_text, L"ok") == 0, L"Ctrl+Space committed 'ok'");
        CHECK_EQUAL(resp.status_flags & IPC_MODE_CHINESE, 0u, L"Ctrl+Space switched to English mode");
        req.modifiers = 0;

        // 3. Full-width digit handling
        client.SetMode(100, ctx, IPC_MODE_FULL_WIDTH);
        req.vk_code = '1';
        req.modifiers = 0;
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"Full-width digit '1' must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"Full-width digit '1' must have commit flag");
        CHECK_TRUE(wcscmp(resp.commit_text, L"\uFF11") == 0, L"Full-width digit '1' must commit \uFF11");
        std::wcout << L"  Full-width digit '1' committed: " << resp.commit_text << std::endl;

        // 4. Bad protocol version rejection
        req.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        req.protocol_version = 0xdeadbeef;
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"0xdeadbeef protocol version must be rejected with handled=0");
        std::wcout << L"  Bad protocol version rejected successfully!" << std::endl;

        // Reset protocol version to valid for subsequent requests
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;

        // 5. Candidate preview truncation surrogate pair protection
        client.SetMode(100, ctx, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.vk_code = VK_OEM_1; // ';'
        client.SendRequest(req, resp, 80);
        const char* lCode = "long";
        for (const char* p = lCode; *p; ++p) { req.vk_code = static_cast<uint32_t>(toupper(*p)); client.SendRequest(req, resp, 80); }
        CHECK_TRUE(resp.candidate_count > 0, L"Candidates for ';long' must exist");
        size_t candLen = wcslen(resp.candidates[0]);
        uint16_t lastUnit = static_cast<uint16_t>(resp.candidates[0][candLen - 1]);
        bool isDangling = (lastUnit >= 0xD800 && lastUnit <= 0xDBFF);
        CHECK_EQUAL(isDangling, false, L"Last UTF-16 unit in candidate preview must not be a dangling high surrogate");
        std::wcout << L"  Candidate preview truncation surrogate pair verified safe (lastUnit=0x" << std::hex << lastUnit << std::dec << L")" << std::endl;

        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);
        std::wcout << L"[PASS] Shift, Arrow keys, Full-width digits and protocol checks verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 12: Round 3 Event Ownership, Epoch & Version 0 Bypass
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 12] Testing Round 3 Event Ownership, Epoch, Concurrency & Version 0..." << std::endl;
    {
        // 1. Strict Event Ownership (Wrong Context Rejection)
        uint64_t ctxOwned = 3001;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctxOwned;
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse resp{};

        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t ownedEvId = resp.commit_event_id;
        CHECK_TRUE(ownedEvId != 0, L"Owned event ID must be non-zero");

        int countBeforeWrong = QueryStatsCount(testDb, "aaaa");
        // Attempt wrong context confirmation (context_id = 9999)
        IpcKeyRequest wrongReq{};
        wrongReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitSuccess);
        wrongReq.session_id = 100;
        wrongReq.context_id = 9999;
        wrongReq.event_id = ownedEvId;
        wrongReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse wrongResp{};
        CHECK_TRUE(client.SendRequest(wrongReq, wrongResp, 80), L"Wrong context request send failed");
        CHECK_EQUAL(wrongResp.handled, 0, L"Wrong context CommitSuccess must be rejected with handled=0");
        CHECK_EQUAL(QueryStatsCount(testDb, "aaaa"), countBeforeWrong, L"Wrong context confirmation must not increment count");

        // Attempt cross-context cancellation (context_id = 3002)
        IpcKeyRequest crossCancelReq{};
        crossCancelReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitCancel);
        crossCancelReq.session_id = 100;
        crossCancelReq.context_id = 3002;
        crossCancelReq.event_id = ownedEvId;
        crossCancelReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse crossCancelResp{};
        CHECK_TRUE(client.SendRequest(crossCancelReq, crossCancelResp, 80), L"Cross cancel request send failed");
        CHECK_EQUAL(crossCancelResp.handled, 0, L"Cross-context CommitCancel must be rejected with handled=0");
        CHECK_TRUE(wcslen(crossCancelResp.composition) == 0, L"Cross-context cancel must return empty composition");

        // Correct context confirmation succeeds
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctxOwned, ownedEvId), L"Legitimate CommitSuccess must succeed");
        CHECK_EQUAL(QueryStatsCount(testDb, "aaaa"), countBeforeWrong + 1, L"Legitimate CommitSuccess must increment count once");
        std::wcout << L"  Strict event ownership verified (wrong session/context rejected)." << std::endl;

        // 2. Reconnection Connection Epoch Isolation
        uint64_t ctxEpoch = 3003;
        req.context_id = ctxEpoch;
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        client.SendRequest(req, resp, 80);
        uint64_t oldEpochEvId = resp.commit_event_id;

        // Reconnect client (generates new connection epoch in service)
        client.Disconnect();
        req.context_id = ctxEpoch;
        for (int i = 0; i < 4; ++i) { req.vk_code = 'B'; client.SendRequest(req, resp, 80); }
        req.vk_code = VK_ESCAPE;
        client.SendRequest(req, resp, 80);

        // Cancel old commit on reconnected connection
        IpcKeyRequest oldCancelReq{};
        oldCancelReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitCancel);
        oldCancelReq.session_id = 100;
        oldCancelReq.context_id = ctxEpoch;
        oldCancelReq.event_id = oldEpochEvId;
        oldCancelReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse oldCancelResp{};
        CHECK_TRUE(client.SendRequest(oldCancelReq, oldCancelResp, 80), L"Old cancel send failed");
        CHECK_EQUAL(oldCancelResp.handled, 1, L"Old cancel must be handled (pending entry cleaned)");
        CHECK_TRUE(wcslen(oldCancelResp.composition) == 0, L"Reconnection epoch mismatch must NOT restore composition");
        std::wcout << L"  Reconnection generation aliasing prevented by connection epoch." << std::endl;

        // 3. Candidate Version 0 Bypass Prevention
        uint64_t ctxVersion = 3004;
        req.context_id = ctxVersion;
        req.vk_code = 'A';
        client.SendRequest(req, resp, 80);
        uint32_t verInitial = resp.candidate_version;

        req.candidate_version = verInitial;
        req.vk_code = VK_OEM_PERIOD; // PageDown
        client.SendRequest(req, resp, 80);
        uint32_t verPaged = resp.candidate_version;
        CHECK_TRUE(verPaged != verInitial, L"Paged version must differ from initial version");

        // Stale version selection rejected
        req.candidate_version = verInitial;
        req.vk_code = '2';
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"Stale candidate version selection must be rejected");

        // Version 0 bypass rejected
        req.candidate_version = 0;
        req.vk_code = '2';
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 0, L"Candidate version 0 after paging must NOT bypass check");
        CHECK_EQUAL(resp.has_commit, 0, L"Rejected version 0 must not set has_commit");

        // Matching version selection accepted
        req.candidate_version = verPaged;
        req.vk_code = '2';
        client.SendRequest(req, resp, 80);
        CHECK_EQUAL(resp.handled, 1, L"Matching candidate version must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"Matching candidate version must set has_commit");
        client.NotifyCommitSuccess(100, ctxVersion, resp.commit_event_id);
        std::wcout << L"  Candidate version 0 bypass prevention verified." << std::endl;

        // 4. In-flight State Concurrency (Success vs Cancel Race)
        uint64_t ctxRace = 3005;
        IpcKeyRequest raceReq{};
        raceReq.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        raceReq.session_id = 100;
        raceReq.context_id = ctxRace;
        raceReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse raceResp{};

        for (int i = 0; i < 4; ++i) { raceReq.vk_code = 'A'; client.SendRequest(raceReq, raceResp, 80); }
        raceReq.candidate_version = raceResp.candidate_version;
        raceReq.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(raceReq, raceResp, 80), L"Race space commit failed");
        CHECK_TRUE(raceResp.has_commit == 1, L"Race space must commit");
        uint64_t raceEvId = raceResp.commit_event_id;
        CHECK_TRUE(raceEvId != 0, L"Race event ID must be non-zero");
        int beforeRace = QueryStatsCount(testDb, "aaaa");

        {
            ime::SqliteDb writerLock;
            CHECK_TRUE(writerLock.Open(testDb), L"Failed to open db for lock");
            CHECK_TRUE(writerLock.Execute("BEGIN IMMEDIATE;"), L"Failed to acquire BEGIN IMMEDIATE");

            bool successAckTransport = false;
            IpcKeyResponse successResp{};
            std::thread successThread([&]() {
                IpcKeyRequest r{};
                r.msg_type = static_cast<uint32_t>(IpcMsgType::CommitSuccess);
                r.session_id = 100;
                r.context_id = ctxRace;
                r.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
                r.event_id = raceEvId;
                successAckTransport = client.SendRequest(r, successResp, 3000);
            });

            Sleep(150); // Allow successThread to transition state to Committing and block on SQLite

            IpcClient otherClient;
            IpcKeyRequest cancelReq{};
            cancelReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitCancel);
            cancelReq.session_id = 100;
            cancelReq.context_id = ctxRace;
            cancelReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
            cancelReq.event_id = raceEvId;
            IpcKeyResponse cancelResp{};
            CHECK_TRUE(otherClient.SendRequest(cancelReq, cancelResp, 1000), L"Race cancel send failed");

            // Cancel must lose the race: return handled = 0 because commit is in-flight (Committing)
            CHECK_EQUAL(cancelResp.handled, 0, L"Concurrent cancel during Committing must return handled=0");

            // Release SQLite lock so success can complete
            CHECK_TRUE(writerLock.Execute("COMMIT;"), L"Failed to commit SQLite lock");
            successThread.join();
            otherClient.Disconnect();

            CHECK_TRUE(successAckTransport, L"Success transport must succeed");
            CHECK_EQUAL(successResp.handled, 1, L"In-flight CommitSuccess must succeed upon lock release");
            CHECK_EQUAL(QueryStatsCount(testDb, "aaaa"), beforeRace + 1, L"Raced event must be committed exactly once");
        }
        std::wcout << L"  In-flight success vs cancel concurrency race verified." << std::endl;
        std::wcout << L"[PASS] All Round 3 ownership, epoch, version 0 and concurrency assertions PASSED!" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 13: Testing Round 4 Durable Journaling, Terminal Lock & Snapshots
    // -------------------------------------------------------------------------
    std::wcout << L"\n[TEST 13] Testing Round 4 Durable Journaling, Terminal Lock & Snapshots..." << std::endl;
    {
        uint64_t ctxR4 = 4001;
        client.SetMode(100, ctxR4, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);

        // 1. Ordinary input candidate snapshot version validation (no paging)
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctxR4;
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse resp{};

        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Type 'a' failed");
        uint32_t ver1 = resp.candidate_version;
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Type 'aa' failed");
        uint32_t ver2 = resp.candidate_version;
        CHECK_TRUE(ver2 != ver1, L"Successive key strokes must produce distinct candidate versions");

        // Old candidate version must be rejected
        req.candidate_version = ver1;
        req.vk_code = '1';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Send stale candidate version request");
        CHECK_EQUAL(resp.handled, 0, L"Stale candidate version on normal typing must be rejected");

        // Candidate version 0 must NOT bypass validation
        req.candidate_version = 0;
        req.vk_code = '1';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Send version 0 request");
        CHECK_EQUAL(resp.handled, 0, L"Candidate version 0 on updated candidates must be rejected");
        CHECK_EQUAL(resp.has_commit, 0, L"Candidate version 0 must not commit");

        // Matching version must succeed
        req.candidate_version = ver2;
        req.vk_code = '1';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Send matching candidate version request");
        CHECK_EQUAL(resp.handled, 1, L"Matching candidate version must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"Matching candidate version must commit");
        uint64_t commitToken = resp.commit_event_id;
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctxR4, commitToken), L"CommitSuccess ack failed");
        std::wcout << L"  Ordinary input candidate snapshot version checks verified." << std::endl;

        // 2. Pre-delivery journaling barrier (INSERT failure prevents has_commit)
        {
            ime::SqliteDb triggerDb;
            CHECK_TRUE(triggerDb.Open(testDb), L"Open db for reject trigger");
            CHECK_TRUE(triggerDb.Execute("CREATE TRIGGER reject_pending_test BEFORE INSERT ON pending_commits BEGIN SELECT RAISE(ABORT, 'barrier_test'); END;"), L"Create reject_pending_test trigger");

            for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
            req.candidate_version = resp.candidate_version;
            req.vk_code = VK_SPACE;
            CHECK_TRUE(client.SendRequest(req, resp, 80), L"Send space with barrier active");
            CHECK_EQUAL(resp.has_commit, 0, L"Pre-delivery barrier: commit must NOT be delivered when journaling fails");
            CHECK_EQUAL(resp.handled, 0, L"Handled must be 0 when journaling fails");

            CHECK_TRUE(triggerDb.Execute("DROP TRIGGER reject_pending_test;"), L"Drop reject trigger");
            // Clear dirty composition from test
            req.vk_code = VK_ESCAPE;
            client.SendRequest(req, resp, 80);
        }
        std::wcout << L"  Pre-delivery journaling barrier verified." << std::endl;

        // 3. Durable Cancellation & Terminal Lock (Cancelled never transitions to Recorded)
        uint64_t ctxCancel = 4002;
        client.SetMode(100, ctxCancel, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        req.context_id = ctxCancel;
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Space commit for cancel test");
        CHECK_EQUAL(resp.has_commit, 1, L"Must have commit");
        uint64_t durableCancelEvId = resp.commit_event_id;
        int countBeforeCancel = QueryStatsCount(testDb, "aaaa");

        // Notify CommitCancel
        CHECK_TRUE(client.NotifyCommitCancel(100, ctxCancel, durableCancelEvId), L"CommitCancel notify failed");

        // Replaying CommitSuccess on a Cancelled event MUST fail (handled = 0) and NEVER increase count
        IpcKeyRequest resurrectReq{};
        resurrectReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitSuccess);
        resurrectReq.session_id = 100;
        resurrectReq.context_id = ctxCancel;
        resurrectReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        resurrectReq.event_id = durableCancelEvId;
        IpcKeyResponse resurrectResp{};
        CHECK_TRUE(client.SendRequest(resurrectReq, resurrectResp, 80), L"Send resurrect request");
        CHECK_EQUAL(resurrectResp.handled, 0, L"Cancelled event must NEVER transition to Recorded");
        CHECK_EQUAL(QueryStatsCount(testDb, "aaaa"), countBeforeCancel, L"Cancelled event count must NOT increase");

        // Replaying CommitCancel must return handled = 1
        IpcKeyRequest cancelReplayReq = resurrectReq;
        cancelReplayReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitCancel);
        IpcKeyResponse cancelReplayResp{};
        CHECK_TRUE(client.SendRequest(cancelReplayReq, cancelReplayResp, 80), L"Send cancel replay");
        CHECK_EQUAL(cancelReplayResp.handled, 1, L"Durable Cancel replay must return handled=1");

        // Restart service and verify Cancelled event STILL cannot be resurrected across restart
        client.Disconnect();
        service.Stop();
        CHECK_TRUE(service.Start(testDb, dictDir), L"Restart service for persistent cancel terminal lock");
        IpcKeyResponse restartResurrectResp{};
        CHECK_TRUE(client.SendRequest(resurrectReq, restartResurrectResp, 80), L"Send resurrect request after restart");
        CHECK_EQUAL(restartResurrectResp.handled, 0, L"Cancelled event across restart must NEVER transition to Recorded");
        CHECK_EQUAL(QueryStatsCount(testDb, "aaaa"), countBeforeCancel, L"Cancelled event count across restart must NOT increase");

        IpcKeyResponse restartCancelReplayResp{};
        CHECK_TRUE(client.SendRequest(cancelReplayReq, restartCancelReplayResp, 80), L"Send cancel replay after restart");
        CHECK_EQUAL(restartCancelReplayResp.handled, 1, L"Durable Cancel replay across restart must return handled=1");
        std::wcout << L"  Durable cancellation & terminal lock (in-memory and across restart) verified." << std::endl;

        // 4. Durable Owner Verification Across Restarts
        uint64_t ctxOwner = 4003;
        client.SetMode(100, ctxOwner, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        req.context_id = ctxOwner;
        for (int i = 0; i < 4; ++i) { req.vk_code = 'A'; client.SendRequest(req, resp, 80); }
        req.candidate_version = resp.candidate_version;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Space commit for owner test");
        uint64_t ownerEvId = resp.commit_event_id;
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctxOwner, ownerEvId), L"CommitSuccess ack");

        // Disconnect and restart service
        client.Disconnect();
        service.Stop();
        CHECK_TRUE(service.Start(testDb, dictDir), L"Restart service for owner verification");

        // Send CommitSuccess replay with WRONG owner (context_id = 9999)
        IpcKeyRequest wrongOwnerReq{};
        wrongOwnerReq.msg_type = static_cast<uint32_t>(IpcMsgType::CommitSuccess);
        wrongOwnerReq.session_id = 100;
        wrongOwnerReq.context_id = 9999;
        wrongOwnerReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        wrongOwnerReq.event_id = ownerEvId;
        IpcKeyResponse wrongOwnerResp{};
        CHECK_TRUE(client.SendRequest(wrongOwnerReq, wrongOwnerResp, 80), L"Send wrong owner replay");
        CHECK_EQUAL(wrongOwnerResp.handled, 0, L"Wrong context owner across restart must be rejected (handled=0)");

        // Send CommitSuccess replay with REAL owner (context_id = ctxOwner)
        IpcKeyRequest realOwnerReq = wrongOwnerReq;
        realOwnerReq.context_id = ctxOwner;
        IpcKeyResponse realOwnerResp{};
        CHECK_TRUE(client.SendRequest(realOwnerReq, realOwnerResp, 80), L"Send real owner replay");
        CHECK_EQUAL(realOwnerResp.handled, 1, L"Real context owner across restart must be acknowledged (handled=1)");
        std::wcout << L"  Durable owner verification across restarts verified." << std::endl;

        std::wcout << L"[PASS] Round 4 durable journaling, terminal lock & snapshots PASSED!" << std::endl;
    }

    // -------------------------------------------------------------------------
    // Test 14: Round 5 Space Version Check, Snapshot Sync & Broken Schema Guard
    // -------------------------------------------------------------------------
    std::wcout << L"\n[TEST 14] Testing Round 5 Space Version Check, Snapshot Sync & Broken Schema Guard..." << std::endl;
    {
        uint64_t ctxR5 = 5001;
        client.SetMode(100, ctxR5, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);

        // 1. Space bar candidate snapshot version check (version 0 rejection)
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        req.session_id = 100;
        req.context_id = ctxR5;
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse resp{};

        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Type 'a'");
        req.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Type 'aa'");
        uint32_t currentCandVer = resp.candidate_version;
        CHECK_TRUE(currentCandVer > 0, L"Candidate version must be > 0");

        // VK_SPACE with version 0 must be rejected when candidates exist!
        req.candidate_version = 0;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Send VK_SPACE with version 0");
        CHECK_EQUAL(resp.handled, 0, L"VK_SPACE with candidate_version 0 must be rejected");
        CHECK_EQUAL(resp.has_commit, 0, L"VK_SPACE with candidate_version 0 must not commit");

        // VK_SPACE with matching candidate_version must succeed!
        req.candidate_version = currentCandVer;
        req.vk_code = VK_SPACE;
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Send VK_SPACE with matching version");
        CHECK_EQUAL(resp.handled, 1, L"VK_SPACE with matching candidate_version must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"VK_SPACE with matching candidate_version must commit");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctxR5, resp.commit_event_id), L"CommitSuccess ack");
        std::wcout << L"  Space bar candidate snapshot version checks verified." << std::endl;

        // 2. Snapshot synchronization upon cancel and immediate retry
        uint64_t ctxSync = 5002;
        client.SetMode(100, ctxSync, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT);
        req.context_id = ctxSync;
        req.vk_code = 'A'; client.SendRequest(req, resp, 80);
        req.vk_code = 'A'; client.SendRequest(req, resp, 80);
        uint32_t candVerBefore = resp.candidate_version;

        req.candidate_version = candVerBefore;
        req.vk_code = '1';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Select candidate '1'");
        CHECK_EQUAL(resp.has_commit, 1, L"Selection must commit");
        uint64_t evToCancel = resp.commit_event_id;

        IpcKeyResponse cancelResp{};
        CHECK_TRUE(client.NotifyCommitCancel(100, ctxSync, evToCancel, &cancelResp), L"NotifyCommitCancel with snapshot sync");
        CHECK_EQUAL(cancelResp.handled, 1, L"Cancel must be handled");
        CHECK_TRUE(cancelResp.candidate_version > candVerBefore, L"Cancel restored candidate version must advance");
        CHECK_TRUE(cancelResp.candidate_count > 0, L"Cancel restored candidate count must be > 0");

        // Selecting candidate using restored candidate_version must succeed immediately!
        req.candidate_version = cancelResp.candidate_version;
        req.vk_code = '1';
        CHECK_TRUE(client.SendRequest(req, resp, 80), L"Select candidate '1' using restored snapshot version");
        CHECK_EQUAL(resp.handled, 1, L"Selection with restored candidate version must be handled");
        CHECK_EQUAL(resp.has_commit, 1, L"Selection with restored candidate version must commit");
        CHECK_TRUE(client.NotifyCommitSuccess(100, ctxSync, resp.commit_event_id), L"CommitSuccess ack");
        std::wcout << L"  Restored candidate snapshot sync & retry verified." << std::endl;

        // 3. Broken recovery schema startup rejection
        std::string brokenDb = "test_broken_schema.db";
        if (std::filesystem::exists(brokenDb)) std::filesystem::remove(brokenDb);
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(brokenDb, ""), L"Seed brokenDb");
            ime::SqliteDb db;
            CHECK_TRUE(db.Open(brokenDb), L"Open brokenDb");
            CHECK_TRUE(db.Execute("ALTER TABLE pending_commits RENAME COLUMN text TO missing_text;"), L"Corrupt pending_commits");
        }
        {
            std::wstring serviceExe = L"build\\Release\\ime-service.exe";
            if (!std::filesystem::exists(serviceExe)) serviceExe = L"ime-service.exe";
            std::wstring cmdLine = serviceExe + L" --db " + ime::Utf8ToWide(brokenDb) + L" --dict " + ime::Utf8ToWide(dictDir);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');
            PROCESS_INFORMATION brokenPi{};
            BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &brokenPi);
            CHECK_TRUE(ok, L"CreateProcess for broken service failed");
            WaitForSingleObject(brokenPi.hProcess, 3000);
            DWORD exitCode = 0;
            GetExitCodeProcess(brokenPi.hProcess, &exitCode);
            CloseHandle(brokenPi.hProcess);
            CloseHandle(brokenPi.hThread);
            CHECK_EQUAL(exitCode, 1, L"Service must strictly exit with code 1 when pending recovery schema is broken!");
        }
        if (std::filesystem::exists(brokenDb)) std::filesystem::remove(brokenDb);
        std::wcout << L"  Broken recovery schema service startup rejection (exit code 1) verified." << std::endl;

        std::wcout << L"[PASS] Round 5 snapshot sync, space version check & broken schema guard PASSED!" << std::endl;
    }

    // Test 15: real TIP journal handover, original owner and autonomous timer replay.
    {
        std::wcout << L"\n[TEST 15] Append-only journal and TIP handover..." << std::endl;
        wchar_t oldTemp[MAX_PATH]{}; GetTempPathW(MAX_PATH, oldTemp);
        auto dir = std::filesystem::temp_directory_path() / ("PersonalIME_integ_" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        SetEnvironmentVariableW(L"TEMP", dir.c_str()); SetEnvironmentVariableW(L"TMP", dir.c_str());
        auto tipA = new CTextInputProcessor();
        uint64_t originalSid = tipA->m_sessionId;
        IpcKeyRequest r{}; r.msg_type=static_cast<uint32_t>(IpcMsgType::KeyDown);
        r.protocol_version=IPC_PROTOCOL_VERSION_STAGE3; r.session_id=originalSid; r.context_id=6001;
        IpcKeyResponse out{};
        client.SetMode(originalSid,r.context_id,IPC_MODE_CHINESE);
        for(int i=0;i<4;++i){r.vk_code='A';CHECK_TRUE(client.SendRequest(r,out,80),L"Type handover candidate");}
        r.vk_code=VK_SPACE;r.candidate_version=out.candidate_version;
        CHECK_TRUE(client.SendRequest(r,out,80)&&out.has_commit,L"Select handover candidate");
        uint64_t eid=out.commit_event_id;
        int before=QueryStatsCount(testDb,"aaaa");
        client.Disconnect();service.Stop();
        CHECK_TRUE(tipA->EnqueueUnconfirmedCommit(6001,eid,CTextInputProcessor::UnconfirmedAction::Success),L"Outcome must persist before handover");
        delete tipA;
        auto tipB=new CTextInputProcessor();tipB->LoadSpilloverCommitsFromFile();
        CHECK_EQUAL(tipB->m_unconfirmedCommits.size(),1,L"New TIP restores result");
        CHECK_EQUAL(tipB->m_unconfirmedCommits[0].session_id,originalSid,L"Original owner retained");
        CHECK_TRUE(service.Start(testDb,dictDir),L"Restart service for autonomous replay");
        tipB->StartDrainTimer();
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!tipB->m_unconfirmedCommits.empty()&&std::chrono::steady_clock::now()<deadline){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(20);
        }
        CHECK_EQUAL(tipB->m_unconfirmedCommits.size(),0,L"Real WM_TIMER must complete without fallback");
        CHECK_EQUAL(QueryStatsCount(testDb,"aaaa"),before+1,L"One durable outcome learns exactly once");
        CHECK_TRUE(!tipB->HasPendingSpilloverRecords(),L"ACK suppresses replay");
        CHECK_TRUE(std::filesystem::exists(dir/L"PersonalIME_receipts.wal"),L"Journal is retained, not rewritten or deleted");
        delete tipB;
        auto tipC=new CTextInputProcessor();tipC->LoadSpilloverCommitsFromFile();
        CHECK_EQUAL(tipC->m_unconfirmedCommits.size(),0,L"ACK survives TIP reconstruction");delete tipC;
        SetEnvironmentVariableW(L"TEMP",oldTemp);SetEnvironmentVariableW(L"TMP",oldTemp);
        std::error_code ecDir; std::filesystem::remove_all(dir, ecDir);
        std::wcout << L"[PASS] Original owner, durable handover, real timer and terminal ACK." << std::endl;
    }
    // Stage 4B finalization: query flags describe only the current composition.
    {
        std::wcout << L"\n[TEST 16] Real service query status and reset boundaries..." << std::endl;
        client.Disconnect();
        service.Stop();
        std::string flagsDb = "out/status-flags-" + std::to_string(GetCurrentProcessId()) + ".db";
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(flagsDb, dictDir), L"Seed isolated query flag DB");
            auto policy = *seed.GetPolicy();
            policy.primary_method = ime::InputScheme::Shuangpin;
            policy.shuangpin_layout = ime::ShuangpinLayout::Natural;
            policy.mixed_input_enabled = false;
            CHECK_TRUE(seed.SavePolicy(policy), L"Persist shuangpin policy for real service flag test");
        }
        ServiceProcess flagService;
        CHECK_TRUE(flagService.Start(flagsDb, dictDir), L"Start isolated service for query flags");
        IpcKeyRequest r{};
        r.session_id = 1616;
        r.context_id = 1;
        r.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        IpcKeyResponse out{};
        r.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        CHECK_TRUE(client.SendRequest(r, out, 200), L"Connect status test client before mode update");
        CHECK_TRUE(client.SetScheme(r.session_id, r.context_id, ime::InputScheme::Wubi86), L"Select Wubi context for invalid-code flag");
        CHECK_TRUE(client.SetMode(r.session_id, r.context_id, IPC_MODE_CHINESE), L"Set Chinese mode");
        r.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        r.vk_code = 'Z';
        for (int i = 0; i < 5; ++i) CHECK_TRUE(client.SendRequest(r, out, 200), L"Type invalid Wubi code");
        CHECK_TRUE(out.status_flags & IPC_STATUS_ERROR, L"Invalid code reports query error flag");
        r.msg_type = static_cast<uint32_t>(IpcMsgType::Reset);
        CHECK_TRUE(client.SendRequest(r, out, 200), L"Reset error context");
        constexpr uint32_t queryFlags = IPC_STATUS_HAS_NEXT_DICT | IPC_STATUS_TRUNCATED | IPC_STATUS_ERROR;
        CHECK_EQUAL(out.status_flags & queryFlags, 0u, L"Reset clears all query-specific status flags");

        r.context_id = 2; // A new context inherits the persisted natural shuangpin policy.
        r.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        for (int i = 0; i < 5; ++i) {
            r.vk_code = 'N'; CHECK_TRUE(client.SendRequest(r, out, 200), L"Type ambiguous shuangpin initial");
            r.vk_code = 'T'; CHECK_TRUE(client.SendRequest(r, out, 200), L"Type ambiguous shuangpin final");
        }
        CHECK_TRUE(out.status_flags & IPC_STATUS_TRUNCATED, L"Actual service propagates decoder budget truncation");
        r.vk_code = VK_ESCAPE;
        CHECK_TRUE(client.SendRequest(r, out, 200), L"Cancel truncated composition");
        CHECK_EQUAL(out.status_flags & queryFlags, 0u, L"Escape clears truncation and next-dictionary hints");

        r.context_id = 3;
        r.vk_code = 'N';
        for (int i = 0; i < 65; ++i) CHECK_TRUE(client.SendRequest(r, out, 200), L"Type beyond shuangpin input limit");
        CHECK_TRUE(out.status_flags & IPC_STATUS_ERROR, L"InputLengthExceeded is distinguishable from ordinary empty candidates");
        client.Disconnect();
        flagService.Stop();
        std::wcout << L"[PASS] Real query errors, length limits, truncation and reset status." << std::endl;
    }

    // -------------------------------------------------------------
    // Test 17: Real Fast Pipe Scheme Independence, Mode Updates, and Enabling Lifecycle
    // -------------------------------------------------------------
    {
        std::wcout << L"\n[TEST 17] Real Fast Pipe Scheme Independence, Mode Updates, and Enabling Lifecycle..." << std::endl;
        client.Disconnect();
        service.Stop();

        std::string schemeDb = "out/scheme-test-" + std::to_string(GetCurrentProcessId()) + ".db";
        std::error_code ec;
        std::filesystem::remove(schemeDb, ec);

        // 1. Initial seed with all 3 schemes enabled
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(schemeDb, dictDir), L"Seed isolated scheme DB");
            auto policy = *seed.GetPolicy();
            policy.primary_method = ime::InputScheme::Wubi86;
            policy.SetMethodEnabled(ime::InputScheme::Wubi86, true);
            policy.SetMethodEnabled(ime::InputScheme::Pinyin, true);
            policy.SetMethodEnabled(ime::InputScheme::Shuangpin, true);
            CHECK_TRUE(seed.SavePolicy(policy), L"Save policy with all 3 schemes enabled");
        }

        ServiceProcess schemeService;
        CHECK_TRUE(schemeService.Start(schemeDb, dictDir), L"Start service with all 3 schemes enabled");

        uint64_t sid = 1700;
        uint64_t cid = 1;

        // Connect
        IpcKeyRequest pingReq{};
        pingReq.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        pingReq.session_id = sid;
        pingReq.context_id = cid;
        pingReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
        IpcKeyResponse pingResp{};
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping scheme test service");

        // -------------------------------------------------------------
        // Part 1: Independent Raw Golden Byte Vectors (Wire 1=Wubi, 2=Pinyin, 3=Shuangpin)
        // -------------------------------------------------------------
        std::wcout << L"  [17.1] Testing Raw Golden Frame Byte Vectors for Wire Scheme IDs..." << std::endl;
        {
            // A. Raw Golden Wire 1 -> Wubi86
            IpcKeyRequest goldenReq{};
            goldenReq.msg_type = 13; // SetScheme
            goldenReq.session_id = sid;
            goldenReq.context_id = cid;
            goldenReq.protocol_version = 0x0400; // Stage 4
            goldenReq.scheme_id = 1; // Literal wire value 1 for Wubi86
            goldenReq.update_mask = 1 << 3; // IPC_UPDATE_SCHEME
            IpcKeyResponse goldenResp{};
            CHECK_TRUE(client.SendRequest(goldenReq, goldenResp, 200), L"Send raw golden wire 1");
            CHECK_EQUAL(goldenResp.handled, 1u, L"Raw golden wire 1 accepted");
            CHECK_EQUAL((goldenResp.status_flags & 0x00FF0000u) >> 16, 1u, L"status_flags bits 16..23 must be 1 for Wubi86");

            // Verify typing 'a' under Wire 1 (Wubi86) produces "工"
            IpcKeyRequest keyReq{};
            keyReq.msg_type = 4; // KeyDown
            keyReq.session_id = sid;
            keyReq.context_id = cid;
            keyReq.protocol_version = 0x0400;
            keyReq.vk_code = 'A';
            IpcKeyResponse keyResp{};
            CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Type 'a' in Wubi86");
            CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates exist for 'a' in Wubi86");
            CHECK_TRUE(wcsstr(keyResp.candidates[0], L"工") != nullptr, L"Wire 1 produces Wubi86 top candidate 工");

            // Cancel composition
            keyReq.vk_code = VK_ESCAPE;
            CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Cancel composition");

            // B. Raw Golden Wire 2 -> Pinyin
            goldenReq.scheme_id = 2; // Literal wire value 2 for Pinyin
            CHECK_TRUE(client.SendRequest(goldenReq, goldenResp, 200), L"Send raw golden wire 2");
            CHECK_EQUAL(goldenResp.handled, 1u, L"Raw golden wire 2 accepted");
            CHECK_EQUAL((goldenResp.status_flags & 0x00FF0000u) >> 16, 2u, L"status_flags bits 16..23 must be 2 for Pinyin");

            // Verify typing "zhongwen" under Wire 2 (Pinyin) produces "中文"
            for (char ch : std::string("ZHONGWEN")) {
                keyReq.vk_code = static_cast<uint32_t>(ch);
                CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Type zhongwen letter");
            }
            CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates exist for zhongwen in Pinyin");
            CHECK_TRUE(wcsstr(keyResp.candidates[0], L"中文") != nullptr, L"Wire 2 produces Pinyin top candidate 中文");

            keyReq.vk_code = VK_ESCAPE;
            CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Cancel composition");

            // C. Raw Golden Wire 3 -> Shuangpin
            goldenReq.scheme_id = 3; // Literal wire value 3 for Shuangpin
            CHECK_TRUE(client.SendRequest(goldenReq, goldenResp, 200), L"Send raw golden wire 3");
            CHECK_EQUAL(goldenResp.handled, 1u, L"Raw golden wire 3 accepted");
            CHECK_EQUAL((goldenResp.status_flags & 0x00FF0000u) >> 16, 3u, L"status_flags bits 16..23 must be 3 for Shuangpin");

            // D. Raw Golden Wire 99 -> Invalid scheme rejected with handled = 0
            goldenReq.scheme_id = 99; // Invalid wire scheme
            CHECK_TRUE(client.SendRequest(goldenReq, goldenResp, 200), L"Send raw golden wire 99");
            CHECK_EQUAL(goldenResp.handled, 0u, L"Invalid wire scheme 99 must be rejected with handled=0");
            CHECK_EQUAL((goldenResp.status_flags & 0x00FF0000u) >> 16, 3u, L"Scheme remains wire 3 (Shuangpin) after rejection");

            // E. Raw Golden Wire 4 (Custom) -> Rejected as primary scheme with handled = 0
            goldenReq.scheme_id = 4; // Custom wire scheme
            CHECK_TRUE(client.SendRequest(goldenReq, goldenResp, 200), L"Send raw golden wire 4");
            CHECK_EQUAL(goldenResp.handled, 0u, L"Wire scheme 4 (Custom) must be rejected with handled=0");
            CHECK_EQUAL((goldenResp.status_flags & 0x00FF0000u) >> 16, 3u, L"Scheme remains wire 3 after Custom rejection");

            // F. Stage 4 commands rejected under legacy protocol version 0x0300
            goldenReq.protocol_version = 0x0300; // Legacy Stage 3
            goldenReq.scheme_id = 1;
            CHECK_TRUE(client.SendRequest(goldenReq, goldenResp, 200), L"Send SetScheme with 0x0300");
            CHECK_EQUAL(goldenResp.handled, 0u, L"SetScheme under 0x0300 must be rejected with handled=0");

            IpcKeyRequest getSchemeLegacyReq{};
            getSchemeLegacyReq.msg_type = 14; // GetScheme
            getSchemeLegacyReq.session_id = sid;
            getSchemeLegacyReq.context_id = cid;
            getSchemeLegacyReq.protocol_version = 0x0300;
            IpcKeyResponse getSchemeLegacyResp{};
            CHECK_TRUE(client.SendRequest(getSchemeLegacyReq, getSchemeLegacyResp, 200), L"Send GetScheme with 0x0300");
            CHECK_EQUAL(getSchemeLegacyResp.handled, 0u, L"GetScheme under 0x0300 must be rejected with handled=0");
        }

        // -------------------------------------------------------------
        // Part 2: Selective update_mask & Atomic State Rejection
        // -------------------------------------------------------------
        std::wcout << L"  [17.2] Testing Selective update_mask and Atomic Rejection..." << std::endl;
        {
            // Initial state: ensure Chinese = true, Full-Width = false, Chinese Punct = true, Scheme = Wubi86
            CHECK_TRUE(client.SetScheme(sid, cid, ime::InputScheme::Wubi86), L"SetScheme Wubi86");
            CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4, 0), L"SetMode whole-set baseline");

            uint32_t mf = 0;
            ime::InputScheme sc;
            CHECK_TRUE(client.GetMode(sid, cid, mf, sc), L"GetMode baseline");
            CHECK_TRUE((mf & IPC_MODE_CHINESE) != 0, L"Baseline Chinese=true");
            CHECK_EQUAL(mf & IPC_MODE_FULL_WIDTH, 0u, L"Baseline FullWidth=false");
            CHECK_TRUE((mf & IPC_MODE_CHINESE_PUNCT) != 0, L"Baseline Punct=true");
            CHECK_EQUAL(static_cast<uint32_t>(sc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Baseline Scheme=Wubi86");

            // Selective update: only update Full-Width to true
            CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_FULL_WIDTH, IPC_PROTOCOL_VERSION_STAGE4, IPC_UPDATE_FULL_WIDTH), L"Selective SetMode FullWidth");
            CHECK_TRUE(client.GetMode(sid, cid, mf, sc), L"GetMode after selective FullWidth");
            CHECK_TRUE((mf & IPC_MODE_FULL_WIDTH) != 0, L"FullWidth is now true");
            CHECK_TRUE((mf & IPC_MODE_CHINESE) != 0, L"Chinese is STILL true (preserved)");
            CHECK_TRUE((mf & IPC_MODE_CHINESE_PUNCT) != 0, L"Punct is STILL true (preserved)");
            CHECK_EQUAL(static_cast<uint32_t>(sc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Scheme STILL Wubi86 (preserved)");

            // Selective update: only update Chinese to false (English)
            CHECK_TRUE(client.SetMode(sid, cid, 0, IPC_PROTOCOL_VERSION_STAGE4, IPC_UPDATE_CHINESE), L"Selective SetMode Chinese=false");
            CHECK_TRUE(client.GetMode(sid, cid, mf, sc), L"GetMode after selective English");
            CHECK_EQUAL(mf & IPC_MODE_CHINESE, 0u, L"Chinese is now false");
            CHECK_TRUE((mf & IPC_MODE_FULL_WIDTH) != 0, L"FullWidth is STILL true (preserved)");
            CHECK_TRUE((mf & IPC_MODE_CHINESE_PUNCT) != 0, L"Punct is STILL true (preserved)");
            CHECK_EQUAL(static_cast<uint32_t>(sc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Scheme STILL Wubi86 (preserved)");

            // Unknown mask bit rejection: update_mask = 0x80
            IpcKeyRequest badMaskReq{};
            badMaskReq.msg_type = static_cast<uint32_t>(IpcMsgType::SetMode);
            badMaskReq.session_id = sid;
            badMaskReq.context_id = cid;
            badMaskReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
            badMaskReq.update_mask = 0x80; // Unknown mask bit!
            badMaskReq.mode_arg = IPC_MODE_CHINESE;
            IpcKeyResponse badMaskResp{};
            CHECK_TRUE(client.SendRequest(badMaskReq, badMaskResp, 200), L"Send SetMode with unknown mask bit");
            CHECK_EQUAL(badMaskResp.handled, 0u, L"Unknown mask bit must be rejected with handled=0");

            // Verify state is completely UNTOUCHED after unknown mask rejection
            CHECK_TRUE(client.GetMode(sid, cid, mf, sc), L"GetMode after bad mask");
            CHECK_EQUAL(mf & IPC_MODE_CHINESE, 0u, L"Chinese is STILL false after rejection");
            CHECK_TRUE((mf & IPC_MODE_FULL_WIDTH) != 0, L"FullWidth is STILL true after rejection");

            // Atomic rejection when scheme invalid in SetMode:
            // update_mask has IPC_UPDATE_FULL_WIDTH and IPC_UPDATE_SCHEME, but scheme_id is 99 (invalid)
            IpcKeyRequest badSchemeSetModeReq{};
            badSchemeSetModeReq.msg_type = static_cast<uint32_t>(IpcMsgType::SetMode);
            badSchemeSetModeReq.session_id = sid;
            badSchemeSetModeReq.context_id = cid;
            badSchemeSetModeReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
            badSchemeSetModeReq.update_mask = IPC_UPDATE_FULL_WIDTH | IPC_UPDATE_SCHEME;
            badSchemeSetModeReq.mode_arg = 0; // Request FullWidth=false
            badSchemeSetModeReq.scheme_id = 99; // Invalid scheme!
            IpcKeyResponse badSchemeSetModeResp{};
            CHECK_TRUE(client.SendRequest(badSchemeSetModeReq, badSchemeSetModeResp, 200), L"Send SetMode with invalid scheme");
            CHECK_EQUAL(badSchemeSetModeResp.handled, 0u, L"SetMode with invalid scheme rejected with handled=0");

            // Verify Full-Width was NOT modified to false (atomic rejection!)
            CHECK_TRUE(client.GetMode(sid, cid, mf, sc), L"GetMode after atomic rejection");
            CHECK_TRUE((mf & IPC_MODE_FULL_WIDTH) != 0, L"FullWidth is STILL true (atomic rejection kept previous state)");
            CHECK_EQUAL(static_cast<uint32_t>(sc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Scheme STILL Wubi86");

            // Active composition untouched on rejection vs cleared on valid change:
            // Restore Chinese mode
            CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4, 0), L"Restore whole-set Chinese");
            // Start composition with 'a'
            IpcKeyRequest typeReq{};
            typeReq.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
            typeReq.session_id = sid;
            typeReq.context_id = cid;
            typeReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
            typeReq.vk_code = 'A';
            IpcKeyResponse typeResp{};
            CHECK_TRUE(client.SendRequest(typeReq, typeResp, 200), L"Type 'a'");
            CHECK_TRUE(wcscmp(typeResp.composition, L"a") == 0, L"Composition is 'a'");

            // Send invalid SetScheme (wire 99)
            IpcKeyRequest rejSchemeReq{};
            rejSchemeReq.msg_type = static_cast<uint32_t>(IpcMsgType::SetScheme);
            rejSchemeReq.session_id = sid;
            rejSchemeReq.context_id = cid;
            rejSchemeReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
            rejSchemeReq.scheme_id = 99;
            IpcKeyResponse rejSchemeResp{};
            CHECK_TRUE(client.SendRequest(rejSchemeReq, rejSchemeResp, 200), L"Send invalid SetScheme");
            CHECK_EQUAL(rejSchemeResp.handled, 0u, L"Invalid SetScheme rejected");

            // Verify active composition was NOT cleared by rejected request!
            // Type 'a' again -> should become "aa", not "a"!
            CHECK_TRUE(client.SendRequest(typeReq, typeResp, 200), L"Type second 'a'");
            CHECK_TRUE(wcscmp(typeResp.composition, L"aa") == 0, L"Composition remained 'aa' (unmodified by rejection)");

            // Now send VALID SetScheme (Pinyin) -> composition MUST be cleared cleanly
            CHECK_TRUE(client.SetScheme(sid, cid, ime::InputScheme::Pinyin), L"Valid SetScheme Pinyin");
            // Next keystroke should be fresh composition
            CHECK_TRUE(client.SendRequest(typeReq, typeResp, 200), L"Type 'a' in Pinyin");
            CHECK_TRUE(wcscmp(typeResp.composition, L"a") == 0, L"Composition was reset by valid scheme change");
            typeReq.vk_code = VK_ESCAPE;
            CHECK_TRUE(client.SendRequest(typeReq, typeResp, 200), L"Cancel composition");
        }

        // -------------------------------------------------------------
        // Part 3: Three schemes * Mode flag updates without scheme change (IpcClient API)
        // -------------------------------------------------------------
        std::wcout << L"  [17.3] Testing Scheme Set/Get roundtrip and Mode flag updates..." << std::endl;
        std::vector<ime::InputScheme> testSchemes = {
            ime::InputScheme::Wubi86,
            ime::InputScheme::Pinyin,
            ime::InputScheme::Shuangpin
        };

        for (auto testSc : testSchemes) {
            CHECK_TRUE(client.SetScheme(sid, cid, testSc), L"SetScheme succeeded");
            ime::InputScheme currentScheme;
            CHECK_TRUE(client.GetScheme(sid, cid, currentScheme), L"GetScheme succeeded");
            CHECK_EQUAL(static_cast<uint32_t>(currentScheme), static_cast<uint32_t>(testSc), L"Scheme matches set scheme");

            // Update mode flags: Full-Width + Chinese (Stage 4 whole-set)
            CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_FULL_WIDTH, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese+FullWidth");
            uint32_t modeFlags = 0;
            CHECK_TRUE(client.GetMode(sid, cid, modeFlags, currentScheme), L"GetMode after Chinese+FullWidth");
            CHECK_TRUE(modeFlags & IPC_MODE_CHINESE, L"Mode flag Chinese is set");
            CHECK_TRUE(modeFlags & IPC_MODE_FULL_WIDTH, L"Mode flag FullWidth is set");
            CHECK_EQUAL(static_cast<uint32_t>(currentScheme), static_cast<uint32_t>(testSc), L"Scheme unchanged after Chinese+FullWidth update");

            // Update mode flags: English + Half-Width (Stage 4 whole-set)
            CHECK_TRUE(client.SetMode(sid, cid, 0, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode English+HalfWidth");
            CHECK_TRUE(client.GetMode(sid, cid, modeFlags, currentScheme), L"GetMode after English+HalfWidth");
            CHECK_EQUAL(modeFlags & IPC_MODE_CHINESE, 0u, L"Mode flag Chinese is cleared");
            CHECK_EQUAL(modeFlags & IPC_MODE_FULL_WIDTH, 0u, L"Mode flag FullWidth is cleared");
            CHECK_EQUAL(static_cast<uint32_t>(currentScheme), static_cast<uint32_t>(testSc), L"Scheme unchanged after English update");

            // Restore Chinese + Half-Width + Chinese Punctuation (Stage 4 whole-set)
            CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese+Punct");
            CHECK_TRUE(client.GetMode(sid, cid, modeFlags, currentScheme), L"GetMode after Chinese+Punct");
            CHECK_EQUAL(static_cast<uint32_t>(currentScheme), static_cast<uint32_t>(testSc), L"Scheme unchanged after Punct update");
        }

        // Part 4: Explicit switch and rejection when scheme disabled
        client.Disconnect();
        schemeService.Stop();

        // Disable Shuangpin in DB
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(schemeDb, dictDir), L"Re-open DB to disable Shuangpin");
            auto policy = *seed.GetPolicy();
            policy.primary_method = ime::InputScheme::Wubi86;
            policy.SetMethodEnabled(ime::InputScheme::Wubi86, true);
            policy.SetMethodEnabled(ime::InputScheme::Pinyin, true);
            policy.SetMethodEnabled(ime::InputScheme::Shuangpin, false);
            CHECK_TRUE(seed.SavePolicy(policy), L"Save policy with Shuangpin disabled");
        }

        CHECK_TRUE(schemeService.Start(schemeDb, dictDir), L"Start service with Shuangpin disabled");
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping after service restart");

        // Verify initial scheme is primary (Wubi86)
        ime::InputScheme activeSc;
        CHECK_TRUE(client.GetScheme(sid, cid, activeSc), L"Get initial scheme");
        CHECK_EQUAL(static_cast<uint32_t>(activeSc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Initial scheme is Wubi86");

        // Attempt explicit switch to disabled Shuangpin -> MUST be rejected with handled = 0!
        bool setDisabledOk = client.SetScheme(sid, cid, ime::InputScheme::Shuangpin);
        CHECK_TRUE(!setDisabledOk, L"Explicit SetScheme to disabled scheme MUST return false (rejected)");
        CHECK_TRUE(client.GetScheme(sid, cid, activeSc), L"GetScheme after rejected switch");
        CHECK_EQUAL(static_cast<uint32_t>(activeSc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Context scheme remains unmodified after rejection");

        // Part 5: Hotkey Ctrl+Shift cycles skipping disabled schemes
        // Current scheme: Wubi86. Enabled schemes: Wubi86, Pinyin. Disabled: Shuangpin.
        IpcKeyRequest hotkeyReq{};
        hotkeyReq.session_id = sid;
        hotkeyReq.context_id = cid;
        hotkeyReq.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        hotkeyReq.vk_code = VK_SHIFT;
        hotkeyReq.modifiers = 2; // Ctrl
        hotkeyReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
        IpcKeyResponse hotkeyResp{};

        // Press Ctrl+Shift -> cycles from Wubi86 to Pinyin
        CHECK_TRUE(client.SendRequest(hotkeyReq, hotkeyResp, 200) && hotkeyResp.handled == 1, L"Send Ctrl+Shift #1");
        CHECK_TRUE(client.GetScheme(sid, cid, activeSc), L"Get scheme after Ctrl+Shift #1");
        CHECK_EQUAL(static_cast<uint32_t>(activeSc), static_cast<uint32_t>(ime::InputScheme::Pinyin), L"Ctrl+Shift cycled to Pinyin");

        // Press Ctrl+Shift -> cycles from Pinyin back to Wubi86 (skipping disabled Shuangpin!)
        CHECK_TRUE(client.SendRequest(hotkeyReq, hotkeyResp, 200) && hotkeyResp.handled == 1, L"Send Ctrl+Shift #2");
        CHECK_TRUE(client.GetScheme(sid, cid, activeSc), L"Get scheme after Ctrl+Shift #2");
        CHECK_EQUAL(static_cast<uint32_t>(activeSc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Ctrl+Shift cycled back to Wubi86 (skipping Shuangpin)");

        // Part 6: Single enabled method remaining
        client.Disconnect();
        schemeService.Stop();

        // Enable ONLY Wubi86
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(schemeDb, dictDir), L"Re-open DB for single enabled method");
            auto policy = *seed.GetPolicy();
            policy.primary_method = ime::InputScheme::Wubi86;
            policy.SetMethodEnabled(ime::InputScheme::Wubi86, true);
            policy.SetMethodEnabled(ime::InputScheme::Pinyin, false);
            policy.SetMethodEnabled(ime::InputScheme::Shuangpin, false);
            CHECK_TRUE(seed.SavePolicy(policy), L"Save policy with only Wubi86 enabled");
        }

        CHECK_TRUE(schemeService.Start(schemeDb, dictDir), L"Start service with single enabled scheme");
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping single-scheme service");

        // Press Ctrl+Shift -> stays on Wubi86
        CHECK_TRUE(client.SendRequest(hotkeyReq, hotkeyResp, 200) && hotkeyResp.handled == 1, L"Send Ctrl+Shift on single scheme");
        CHECK_TRUE(client.GetScheme(sid, cid, activeSc), L"Get scheme after Ctrl+Shift on single scheme");
        CHECK_EQUAL(static_cast<uint32_t>(activeSc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Single scheme remains on Wubi86");

        // Part 7: Restart persistence
        client.Disconnect();
        schemeService.Stop();

        CHECK_TRUE(schemeService.Start(schemeDb, dictDir), L"Restart service to test persistence");
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping restarted service");
        CHECK_TRUE(client.GetScheme(sid, cid, activeSc), L"Get scheme after service restart");
        CHECK_EQUAL(static_cast<uint32_t>(activeSc), static_cast<uint32_t>(ime::InputScheme::Wubi86), L"Scheme persists across service restart");
        CHECK_TRUE(!client.SetScheme(sid, cid, ime::InputScheme::Pinyin), L"Disabled Pinyin rejected after restart");
        CHECK_TRUE(!client.SetScheme(sid, cid, ime::InputScheme::Shuangpin), L"Disabled Shuangpin rejected after restart");

        client.Disconnect();
        schemeService.Stop();
        std::filesystem::remove(schemeDb, ec);

        std::wcout << L"[PASS] Scheme independence, mode flag isolation, hotkey skipping, and restart verified!" << std::endl;
    }

    // -------------------------------------------------------------------------
    // TEST 18: Real Candidate Window Reverse Lookup Hint & Pure Commit (Stage 4C-2)
    // -------------------------------------------------------------------------
    {
        std::wcout << L"\n[TEST 18] Testing Real Candidate Window Reverse Lookup Hint & Pure Commit..." << std::endl;

        std::string rlDb = "out/stage4c2_fastpipe.db";
        std::error_code ec;
        std::filesystem::remove(rlDb, ec);

        // 1. Seed policy with reverse lookup ENABLED, target_method = Wubi86
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(rlDb, dictDir), L"Initialize seed engine for Test 18");
            auto policy = *seed.GetPolicy();
            policy.primary_method = ime::InputScheme::Pinyin;
            policy.reverse_lookup.enabled = true;
            policy.reverse_lookup.target_method = ime::InputScheme::Wubi86;
            policy.reverse_lookup.timing = "always";
            CHECK_TRUE(seed.SavePolicy(policy), L"Save policy with reverse lookup enabled");
        }

        ServiceProcess rlService;
        CHECK_TRUE(rlService.Start(rlDb, dictDir), L"Start service with reverse lookup enabled");

        uint64_t sid = 0x4C2001;
        uint64_t cid = 0x4C2002;

        IpcKeyRequest pingReq{};
        pingReq.session_id = sid;
        pingReq.context_id = cid;
        pingReq.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        pingReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
        IpcKeyResponse pingResp{};
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping service for Test 18");

        // Ensure in Chinese Pinyin mode
        CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese");
        CHECK_TRUE(client.SetScheme(sid, cid, ime::InputScheme::Pinyin), L"SetScheme Pinyin");

        // Type 'g', 'o', 'n', 'g'
        IpcKeyRequest keyReq{};
        keyReq.session_id = sid;
        keyReq.context_id = cid;
        keyReq.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        keyReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
        IpcKeyResponse keyResp{};

        const char* gongKeys = "gong";
        for (const char* p = gongKeys; *p; ++p) {
            keyReq.vk_code = static_cast<uint32_t>(toupper(*p));
            CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Type key in gong");
        }

        CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates exist for 'gong'");
        for (uint32_t i = 0; i < keyResp.candidate_count; ++i) {
            std::wcout << L"  Candidate " << i << L" with Wubi reverse lookup: " << keyResp.candidates[i] << std::endl;
            // Every candidate must have reverse lookup code in bracket format
            CHECK_TRUE(wcsstr(keyResp.candidates[i], L"[") != nullptr, L"Candidate label contains reverse lookup hint '['");
            CHECK_TRUE(wcsstr(keyResp.candidates[i], L"]") != nullptr, L"Candidate label contains reverse lookup hint ']'");
        }

        // Commit candidate 0 via Space bar
        keyReq.vk_code = VK_SPACE;
        keyReq.candidate_version = keyResp.candidate_version;
        CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Press Space to commit candidate 0");
        CHECK_EQUAL(keyResp.has_commit, 1u, L"has_commit must be 1");

        // CRITICAL CONTRACT: Reverse lookup hint is purely presentational!
        // The committed text MUST strictly be pure text, NOT containing brackets or reverse codes!
        std::wcout << L"  Committed text: " << keyResp.commit_text << std::endl;
        CHECK_TRUE(wcschr(keyResp.commit_text, L'[') == nullptr, L"Committed text must NOT contain '['");
        CHECK_TRUE(wcschr(keyResp.commit_text, L']') == nullptr, L"Committed text must NOT contain ']'");
        CHECK_TRUE(wcslen(keyResp.commit_text) > 0, L"Committed text is non-empty");

        // 2. Switch reverse lookup target to Shuangpin Natural via seed
        client.Disconnect();
        rlService.Stop();

        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(rlDb, dictDir), L"Reopen DB for Shuangpin reverse lookup target");
            auto policy = *seed.GetPolicy();
            policy.primary_method = ime::InputScheme::Wubi86;
            policy.reverse_lookup.enabled = true;
            policy.reverse_lookup.target_method = ime::InputScheme::Shuangpin;
            policy.reverse_lookup.shuangpin_layout = ime::ShuangpinLayout::Natural;
            policy.reverse_lookup.timing = "always";
            CHECK_TRUE(seed.SavePolicy(policy), L"Save policy with Shuangpin reverse lookup target");
        }

        CHECK_TRUE(rlService.Start(rlDb, dictDir), L"Restart service with Shuangpin reverse lookup target");
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping reconnected client for Shuangpin");

        // In Wubi86 mode, type 'a'
        CHECK_TRUE(client.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese Wubi");
        CHECK_TRUE(client.SetScheme(sid, cid, ime::InputScheme::Wubi86), L"SetScheme Wubi86");

        keyReq.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Type 'a' in Wubi86");
        CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates exist for 'a' in Wubi86");
        std::wcout << L"  Candidate 0 with Shuangpin reverse lookup: " << keyResp.candidates[0] << std::endl;

        // Candidate 0 text is '工', reverse code under Shuangpin Natural is 'gs'
        CHECK_TRUE(wcsstr(keyResp.candidates[0], L"工") != nullptr, L"Candidate 0 text contains '工'");
        CHECK_TRUE(wcsstr(keyResp.candidates[0], L"[gs]") != nullptr, L"Candidate 0 label contains Shuangpin hint '[gs]'");

        // Commit candidate 0 via Space
        keyReq.vk_code = VK_SPACE;
        keyReq.candidate_version = keyResp.candidate_version;
        CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Press Space to commit candidate 0");
        CHECK_EQUAL(keyResp.has_commit, 1u, L"has_commit must be 1");
        CHECK_TRUE(wcscmp(keyResp.commit_text, L"工") == 0, L"Committed text must strictly be '工'");

        // 3. Disable reverse lookup: candidates must NOT contain brackets
        client.Disconnect();
        rlService.Stop();

        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(rlDb, dictDir), L"Reopen DB to disable reverse lookup");
            auto policy = *seed.GetPolicy();
            policy.reverse_lookup.enabled = false;
            CHECK_TRUE(seed.SavePolicy(policy), L"Save policy with reverse lookup disabled");
        }

        CHECK_TRUE(rlService.Start(rlDb, dictDir), L"Restart service with reverse lookup disabled");
        CHECK_TRUE(client.SendRequest(pingReq, pingResp, 200), L"Ping reconnected client for disabled");

        keyReq.vk_code = 'A';
        CHECK_TRUE(client.SendRequest(keyReq, keyResp, 200), L"Type 'a' with reverse lookup disabled");
        CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates exist");
        CHECK_TRUE(wcsstr(keyResp.candidates[0], L"[") == nullptr, L"Candidate label must NOT contain '[' when reverse lookup disabled");
        CHECK_TRUE(wcsstr(keyResp.candidates[0], L"工") != nullptr, L"Candidate label still contains '工'");

        client.Disconnect();
        rlService.Stop();
        std::filesystem::remove(rlDb, ec);

        std::wcout << L"[PASS] Reverse lookup candidate hint display and pure text commit verified!" << std::endl;
    }

    // -------------------------------------------------------------
    // Test 19: Complete Cascading Fallback & Policy Persistence Verification
    // -------------------------------------------------------------
    std::wcout << L"\n[TEST 19] Testing Complete Fallback Chain & Persistence (Wubi -> Pinyin Cascading)..." << std::endl;
    {
        std::string fallbackDb = "fallback_chain_test.db";
        std::error_code ec;
        if (std::filesystem::exists(fallbackDb)) {
            std::filesystem::remove(fallbackDb, ec);
        }

        // 1. Initial configuration: Chinese mode, mixed input enabled, Wubi86 primary, Pinyin enabled
        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(fallbackDb, dictDir), L"Initialize fallback test DB");
            ime::InputPolicy pol;
            pol.primary_method = ime::InputScheme::Wubi86;
            pol.mixed_input_enabled = true;
            pol.input_method_order = {
                ime::InputScheme::Wubi86,
                ime::InputScheme::Pinyin,
                ime::InputScheme::Shuangpin
            };
            pol.SetMethodEnabled(ime::InputScheme::Wubi86, true);
            pol.SetMethodEnabled(ime::InputScheme::Pinyin, true);
            pol.SetMethodEnabled(ime::InputScheme::Shuangpin, true);
            pol.initial_mode.chinese = true;
            CHECK_TRUE(seed.SavePolicy(pol), L"Save initial fallback policy");
        }

        ServiceProcess fbService;
        CHECK_TRUE(fbService.Start(fallbackDb, dictDir), L"Start service for fallback chain test");
        IpcClient fbClient;
        IpcKeyRequest pingReq{};
        pingReq.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        pingReq.session_id = 999;
        pingReq.context_id = 9901;
        pingReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
        IpcKeyResponse pingResp{};
        CHECK_TRUE(fbClient.SendRequest(pingReq, pingResp, 200), L"Ping fallback service");

        uint64_t sid = 999;
        uint64_t cid = 9901;
        CHECK_TRUE(fbClient.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese");
        CHECK_TRUE(fbClient.SetScheme(sid, cid, ime::InputScheme::Wubi86), L"SetScheme Wubi86");

        // 2. Feed keystrokes q -> u -> a -> n
        IpcKeyRequest keyReq{};
        keyReq.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
        keyReq.session_id = sid;
        keyReq.context_id = cid;
        keyReq.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
        IpcKeyResponse keyResp{};

        // 'q'
        keyReq.vk_code = 'Q';
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Send 'q'");
        CHECK_TRUE(wcscmp(keyResp.composition, L"q") == 0, L"Composition is 'q'");

        // 'u'
        keyReq.vk_code = 'U';
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Send 'u'");
        CHECK_TRUE(wcscmp(keyResp.composition, L"qu") == 0, L"Composition is 'qu'");

        // 'a'
        keyReq.vk_code = 'A';
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Send 'a'");
        CHECK_TRUE(wcscmp(keyResp.composition, L"qua") == 0, L"Composition is 'qua'");

        // 'n' -> 'quan'
        keyReq.vk_code = 'N';
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Send 'n'");
        CHECK_TRUE(wcscmp(keyResp.composition, L"quan") == 0, L"Composition is 'quan'");

        // 3. Wubi has no match for 'quan', Pinyin retrieves candidates!
        CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates must exist for 'quan' via pinyin fallback");
        std::wcout << L"  'quan' candidate count: " << keyResp.candidate_count << L", top: " << keyResp.candidates[0] << std::endl;
        CHECK_TRUE(wcsstr(keyResp.candidates[0], L"全") != nullptr, L"Top candidate must contain '全'");

        // 4. Space commits pure Chinese text
        keyReq.vk_code = VK_SPACE;
        keyReq.candidate_version = keyResp.candidate_version;
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Press Space to commit candidate");
        CHECK_EQUAL(keyResp.has_commit, 1u, L"has_commit must be 1");
        CHECK_TRUE(wcslen(keyResp.commit_text) > 0, L"Committed text must not be empty");
        for (size_t i = 0; i < wcslen(keyResp.commit_text); ++i) {
            wchar_t ch = keyResp.commit_text[i];
            CHECK_TRUE((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF), L"Committed text must strictly be Chinese character");
        }
        std::wcout << L"  'quan' committed text: " << keyResp.commit_text << std::endl;
        if (keyResp.commit_event_id != 0) {
            CHECK_TRUE(fbClient.NotifyCommitSuccess(sid, cid, keyResp.commit_event_id), L"Confirm 'quan' commit receipt");
        }

        // 4b. Verify financial macro v12345.67 keystroke-by-keystroke via TestKeyDown + KeyDown in real service
        const uint32_t vMacroKeys[] = { 'V', '1', '2', '3', '4', '5', VK_OEM_PERIOD, '6', '7' };
        for (uint32_t vk : vMacroKeys) {
            IpcKeyRequest tkReq = keyReq;
            tkReq.msg_type = static_cast<uint32_t>(IpcMsgType::TestKeyDown);
            tkReq.vk_code = vk;
            IpcKeyResponse tkResp{};
            CHECK_TRUE(fbClient.SendRequest(tkReq, tkResp, 200) && tkResp.handled == 1, L"v12345.67 TestKeyDown must be eaten");

            keyReq.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
            keyReq.vk_code = vk;
            CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200) && keyResp.handled == 1, L"v12345.67 KeyDown must be handled");
        }
        CHECK_TRUE(wcscmp(keyResp.composition, L"v12345.67") == 0, L"Composition must be 'v12345.67'");
        CHECK_TRUE(keyResp.candidate_count >= 3, L"v12345.67 must return >= 3 financial macro candidates");
        CHECK_TRUE(wcsstr(keyResp.candidates[0], L"壹万贰仟叁佰肆拾伍元陆角柒分") != nullptr, L"Top candidate for v12345.67 must be 壹万贰仟叁佰肆拾伍元陆角柒分");

        // Select 2nd candidate explicitly via IPC_KEYDOWN_EXPLICIT_SELECT (simulating UI/arrow selection)
        keyReq.vk_code = '2';
        keyReq.mode_arg = IPC_KEYDOWN_EXPLICIT_SELECT;
        keyReq.candidate_version = keyResp.candidate_version;
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200) && keyResp.handled == 1, L"Explicit select 2nd candidate on v12345.67");
        keyReq.mode_arg = 0;
        CHECK_EQUAL(keyResp.has_commit, 1u, L"v12345.67 explicit select committed");
        CHECK_TRUE(wcscmp(keyResp.commit_text, L"壹万贰仟叁佰肆拾伍点陆柒") == 0, L"Committed 2nd macro candidate 壹万贰仟叁佰肆拾伍点陆柒");
        CHECK_TRUE(keyResp.commit_event_id != 0, L"Macro commit generated WAL event_id");
        CHECK_TRUE(fbClient.NotifyCommitSuccess(sid, cid, keyResp.commit_event_id), L"Macro NotifyCommitSuccess succeeded and cleaned up pending_commits");

        // 5. Mixed input disabled: no cross-scheme matching
        fbClient.Disconnect();
        fbService.Stop();

        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(fallbackDb, dictDir), L"Reopen DB to disable mixed input");
            auto pol = *seed.GetPolicy();
            pol.mixed_input_enabled = false;
            CHECK_TRUE(seed.SavePolicy(pol), L"Save policy with mixed_input_enabled = false");
        }

        CHECK_TRUE(fbService.Start(fallbackDb, dictDir), L"Restart service with mixed_input_enabled = false");
        CHECK_TRUE(fbClient.SendRequest(pingReq, pingResp, 200), L"Ping service after disabling mixed input");
        CHECK_TRUE(fbClient.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese");
        CHECK_TRUE(fbClient.SetScheme(sid, cid, ime::InputScheme::Wubi86), L"SetScheme Wubi86");

        for (char ch : { 'Q', 'U', 'A', 'N' }) {
            keyReq.vk_code = ch;
            keyReq.candidate_version = 0;
            CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Type key in non-mixed mode");
        }
        CHECK_TRUE(wcscmp(keyResp.composition, L"quan") == 0, L"Composition is 'quan'");
        CHECK_EQUAL(keyResp.candidate_count, 0u, L"No candidates should match 'quan' when mixed input is disabled");

        keyReq.vk_code = VK_ESCAPE;
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Press Esc to cancel composition");
        CHECK_TRUE(wcslen(keyResp.composition) == 0, L"Composition cleared after Esc");

        // 6. Quanpin disabled: mixed input enabled but Pinyin disabled -> no fallback to Pinyin
        fbClient.Disconnect();
        fbService.Stop();

        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(fallbackDb, dictDir), L"Reopen DB to disable Pinyin");
            auto pol = *seed.GetPolicy();
            pol.mixed_input_enabled = true;
            pol.SetMethodEnabled(ime::InputScheme::Pinyin, false);
            pol.SetMethodEnabled(ime::InputScheme::Shuangpin, false);
            CHECK_TRUE(seed.SavePolicy(pol), L"Save policy with Pinyin disabled");
        }

        CHECK_TRUE(fbService.Start(fallbackDb, dictDir), L"Restart service with Pinyin disabled");
        CHECK_TRUE(fbClient.SendRequest(pingReq, pingResp, 200), L"Ping service after disabling Pinyin");
        CHECK_TRUE(fbClient.SetMode(sid, cid, IPC_MODE_CHINESE | IPC_MODE_CHINESE_PUNCT, IPC_PROTOCOL_VERSION_STAGE4), L"SetMode Chinese");
        CHECK_TRUE(fbClient.SetScheme(sid, cid, ime::InputScheme::Wubi86), L"SetScheme Wubi86");

        for (char ch : { 'Q', 'U', 'A', 'N' }) {
            keyReq.vk_code = ch;
            keyReq.candidate_version = 0;
            CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Type key with Pinyin disabled");
        }
        CHECK_EQUAL(keyResp.candidate_count, 0u, L"No candidates should match 'quan' when Pinyin is disabled");

        keyReq.vk_code = VK_ESCAPE;
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Press Esc");

        // 7. Restart service and verify policy persistence + another valid pinyin code beyond 'quan' (e.g. 'zheng')
        fbClient.Disconnect();
        fbService.Stop();

        {
            ime::ImeEngine seed;
            CHECK_TRUE(seed.Initialize(fallbackDb, dictDir), L"Reopen DB to re-enable Pinyin");
            auto pol = *seed.GetPolicy();
            pol.mixed_input_enabled = true;
            pol.SetMethodEnabled(ime::InputScheme::Pinyin, true);
            pol.SetMethodEnabled(ime::InputScheme::Shuangpin, true);
            CHECK_TRUE(seed.SavePolicy(pol), L"Save persisted policy");
        }

        CHECK_TRUE(fbService.Start(fallbackDb, dictDir), L"Restart service to test persistence");
        CHECK_TRUE(fbClient.SendRequest(pingReq, pingResp, 200), L"Ping restarted service");

        for (char ch : { 'Z', 'H', 'E', 'N', 'G' }) {
            keyReq.vk_code = ch;
            keyReq.candidate_version = 0;
            CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Type 'zheng'");
        }
        CHECK_TRUE(wcscmp(keyResp.composition, L"zheng") == 0, L"Composition is 'zheng'");
        CHECK_TRUE(keyResp.candidate_count > 0, L"Candidates exist for 'zheng' via Pinyin fallback");
        std::wcout << L"  'zheng' candidate count: " << keyResp.candidate_count << L", top: " << keyResp.candidates[0] << std::endl;
        // Verify top candidate has Chinese characters after the number prefix
        wchar_t* dotPos = wcschr(keyResp.candidates[0], L'.');
        CHECK_TRUE(dotPos != nullptr && *(dotPos + 1) == L' ' && (*(dotPos + 2) >= 0x4E00 && *(dotPos + 2) <= 0x9FFF), L"Top candidate for 'zheng' must be Chinese");

        keyReq.vk_code = '1';
        keyReq.candidate_version = keyResp.candidate_version;
        CHECK_TRUE(fbClient.SendRequest(keyReq, keyResp, 200), L"Select candidate 1");
        CHECK_EQUAL(keyResp.has_commit, 1u, L"has_commit must be 1");
        CHECK_TRUE(wcslen(keyResp.commit_text) > 0, L"commit_text non-empty");
        std::wcout << L"  'zheng' committed text: " << keyResp.commit_text << std::endl;

        fbClient.Disconnect();
        fbService.Stop();
        std::filesystem::remove(fallbackDb, ec);

        std::wcout << L"[PASS] Fallback chain & persistence verified (mixed input cascading, scheme isolation, persistence, multi-code)!" << std::endl;
    }

    std::wcout << L"\n=======================================================\n";
    std::wcout << L"  ALL Stage 3 & 4C-2 End-to-End Tests PASSED! (19/19)\n";
    std::wcout << L"=======================================================\n";
    return 0;
}
