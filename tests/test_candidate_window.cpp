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

    // 5. Manager button geometry, non-activation focus safety, hover & click callback
    {
        CCandidateWindow win;
        std::vector<std::wstring> cands = { L"1. 候选一", L"2. 候选二" };
        POINT pt = { 100, 100 };
        win.Show(cands, pt);

        // Force paint message to establish layout
        SendMessageW(win.GetHwnd(), WM_PAINT, 0, 0);

        RECT rcBtn = win.GetManagerButtonRect();
        int btnWidth = rcBtn.right - rcBtn.left;
        int btnHeight = rcBtn.bottom - rcBtn.top;
        CHECK_EQ(btnWidth > 20, true, L"Manager button width must be > 20px");
        CHECK_EQ(btnHeight > 0, true, L"Manager button height must be > 0");

        // Focus preservation check: must return MA_NOACTIVATE
        LRESULT actRes = SendMessageW(win.GetHwnd(), WM_MOUSEACTIVATE, (WPARAM)win.GetHwnd(), MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        CHECK_EQ(actRes, MA_NOACTIVATE, L"WM_MOUSEACTIVATE must return MA_NOACTIVATE to preserve host application focus");

        // Hover test
        POINT insidePt = { (rcBtn.left + rcBtn.right) / 2, (rcBtn.top + rcBtn.bottom) / 2 };
        SendMessageW(win.GetHwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(insidePt.x, insidePt.y));
        CHECK_EQ(win.IsHoveringManagerButton(), true, L"Mouse move over manager button must set hover");

        SendMessageW(win.GetHwnd(), WM_MOUSELEAVE, 0, 0);
        CHECK_EQ(win.IsHoveringManagerButton(), false, L"Mouse leave must clear hover");

        // Click callback test
        bool mgrClicked = false;
        win.SetOnOpenManager([&]() { mgrClicked = true; });

        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(insidePt.x, insidePt.y));
        CHECK_EQ(win.IsPressedManagerButton(), true, L"LButtonDown inside button must set pressed");

        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(insidePt.x, insidePt.y));
        CHECK_EQ(win.IsPressedManagerButton(), false, L"LButtonUp must clear pressed");
        CHECK_EQ(mgrClicked, true, L"LButtonUp inside manager button must invoke OnOpenManager");
        CHECK_EQ(IsWindowVisible(win.GetHwnd()), FALSE, L"Window must be hidden after triggering manager button");

        win.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destroy restores class ref to 0");
    }
    std::wcout << L"[PASS] Verified: Manager button on candidate window preserves host focus (MA_NOACTIVATE) and handles click." << std::endl;

    // 6. Plugin buttons rendering, count, and execution callback test
    {
        CCandidateWindow win;
        CHECK_EQ(win.GetPlugins().size(), 3, L"Default plugins count must be 3 (calc, paint, edge)");
        std::vector<std::wstring> cands = { L"1. 测试候选" };
        POINT pt = { 100, 100 };
        win.Show(cands, pt);
        SendMessageW(win.GetHwnd(), WM_PAINT, 0, 0);

        uint32_t executedId = 0;
        win.SetOnExecutePlugin([&](const CandidatePluginItem& item) {
            executedId = item.id;
        });

        const auto& plugins = win.GetPlugins();
        CHECK_EQ(plugins[0].name, L"计算器", L"Plugin 0 must be 计算器");
        CHECK_EQ(plugins[1].name, L"画图", L"Plugin 1 must be 画图");
        CHECK_EQ(plugins[2].name, L"Edge浏览器", L"Plugin 2 must be Edge浏览器");

        // Click plugin 1 (calc)
        RECT rcP0 = plugins[0].rcBtn;
        POINT ptP0 = { (rcP0.left + rcP0.right) / 2, (rcP0.top + rcP0.bottom) / 2 };
        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptP0.x, ptP0.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptP0.x, ptP0.y));
        CHECK_EQ(executedId, 1, L"Clicking plugin 0 must trigger OnExecutePlugin with id=1");

        win.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destroy restores class ref to 0");
    }
    std::wcout << L"[PASS] Verified: Plugin list loaded on candidate bar, aligned with settings button, and executes cleanly." << std::endl;

    std::wcout << L"=== All Candidate Window tests PASSED! ===" << std::endl;
    return 0;
}

