#include <windows.h>
#include <iostream>
#include "../src/tsf_client/candidate_window.h"

HINSTANCE g_hInst = nullptr;
LONG g_cDllRef = 0;

#define CHECK_EQ(actual, expected, desc) \
    do { \
        if ((actual) != (expected)) { \
            std::wcerr << L"[FAIL] " << desc << L": expected " << (expected) \
                       << L", got " << (actual) << L" (line " << __LINE__ << L")\n"; \
            return 1; \
        } \
    } while (false)

int main() {
    SetConsoleOutputCP(CP_UTF8);
    g_hInst = GetModuleHandleW(nullptr);

    std::wcout << L"=== Testing Candidate Window Class RefCount Safety ===" << std::endl;

    // 1. Initial count must be 0
    CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Initial s_cClassRef must be 0");

    // 2. Unused window constructed and destroyed multiple times
    {
        CCandidateWindow window;
        window.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destroy on unused window must not decrement below 0");
        window.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Repeated Destroy must be idempotent");
    }
    CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destructor of unused window must not decrement");
    std::wcout << L"[PASS] Verified: Unused window destruction is 100% idempotent (refs=0)." << std::endl;

    // 3. Single window show and multiple destroys
    {
        CCandidateWindow window;
        std::vector<std::wstring> cands = { L"1. 测试", L"2. 验证" };
        POINT pt = { 100, 100 };
        window.Show(cands, pt);
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 1, L"Show must increment class ref to 1");

        window.Hide();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 1, L"Hide does not destroy class ref");

        window.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destroy restores class ref to 0");

        window.Destroy(); // Call again
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Second destroy on shown window must not decrement below 0");
    }
    CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"After scope exit, refs remains 0");
    std::wcout << L"[PASS] Verified: Show + multiple Destroy() calls return to exactly refs=0." << std::endl;

    // 4. Two concurrent windows
    {
        CCandidateWindow win1;
        CCandidateWindow win2;
        std::vector<std::wstring> cands = { L"1. A" };
        POINT pt = { 100, 100 };
        win1.Show(cands, pt);
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 1, L"win1 show -> refs=1");

        win2.Show(cands, pt);
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 2, L"win2 show -> refs=2");

        win1.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 1, L"win1 destroy -> refs=1");

        win2.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"win2 destroy -> refs=0");
    }
    CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"All windows destroyed -> refs=0");
    std::wcout << L"[PASS] Verified: Multiple window instances correctly manage shared class ref." << std::endl;

    std::wcout << L"=== All Candidate Window RefCount tests PASSED! ===" << std::endl;
    return 0;
}

