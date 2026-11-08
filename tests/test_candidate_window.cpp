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

    // 7. Candidate selection (keyboard SetSelectedIndex, mouse hover selection, click commit & wheel scroll)
    {
        CCandidateWindow win;
        std::vector<std::wstring> cands = { L"1. 人人", L"2. 输入法", L"3. 测试 [cs]" };
        POINT pt = { 100, 100 };
        win.Show(cands, pt);
        SendMessageW(win.GetHwnd(), WM_PAINT, 0, 0);

        CHECK_EQ(win.GetCandidateCount(), 3, L"Candidate count must be 3");
        CHECK_EQ(win.GetSelectedIndex(), 0, L"Initial selected index must be 0");
        CHECK_EQ((int)win.GetCandidateRects().size(), 3, L"Candidate rects size must match candidate count");

        // Programmatic / Keyboard selection switch
        win.SetSelectedIndex(1);
        CHECK_EQ(win.GetSelectedIndex(), 1, L"SetSelectedIndex(1) must update selected index to 1");
        win.SetSelectedIndex(99);
        CHECK_EQ(win.GetSelectedIndex(), 2, L"SetSelectedIndex out-of-range must clamp to last candidate");
        win.SetSelectedIndex(-5);
        CHECK_EQ(win.GetSelectedIndex(), 0, L"SetSelectedIndex negative must clamp to 0");

        // Mouse hover over candidate 2 switches active selection
        const auto& rects = win.GetCandidateRects();
        POINT ptCand2 = { (rects[2].left + rects[2].right) / 2, (rects[2].top + rects[2].bottom) / 2 };
        SendMessageW(win.GetHwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(ptCand2.x, ptCand2.y));
        CHECK_EQ(win.GetHoveredIndex(), 2, L"Hovering candidate 2 must set hoveredIndex=2");
        CHECK_EQ(win.GetSelectedIndex(), 2, L"Hovering candidate 2 must switch selectedIndex=2");

        // Mouse click on candidate 1 triggers OnSelectCandidate(1)
        int selectedCallbackIdx = -1;
        win.SetOnSelectCandidate([&](int idx) {
            selectedCallbackIdx = idx;
        });
        POINT ptCand1 = { (rects[1].left + rects[1].right) / 2, (rects[1].top + rects[1].bottom) / 2 };
        SendMessageW(win.GetHwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(ptCand1.x, ptCand1.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptCand1.x, ptCand1.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptCand1.x, ptCand1.y));
        CHECK_EQ(selectedCallbackIdx, 1, L"Clicking candidate 1 must invoke OnSelectCandidate(1)");

        // Mouse wheel scroll triggers OnPageScroll
        int scrollDirection = 0; // 1 = nextPage, -1 = prevPage
        win.SetOnPageScroll([&](bool nextPage) {
            scrollDirection = nextPage ? 1 : -1;
        });
        SendMessageW(win.GetHwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(ptCand1.x, ptCand1.y));
        CHECK_EQ(scrollDirection, 1, L"Wheel down (-WHEEL_DELTA) must trigger OnPageScroll(nextPage=true)");
        SendMessageW(win.GetHwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(ptCand1.x, ptCand1.y));
        CHECK_EQ(scrollDirection, -1, L"Wheel up (+WHEEL_DELTA) must trigger OnPageScroll(nextPage=false)");

        win.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destroy restores class ref to 0");
    }
    std::wcout << L"[PASS] Verified: Candidate keyboard/mouse selection switch, click commit, and wheel paging." << std::endl;

    // 8. Paging arrow buttons (< and >) rendering, hit test, and click callbacks
    {
        CCandidateWindow win;
        CandidateDisplaySnapshot snap;
        snap.page_index = 0;
        snap.page_count = 3;
        snap.candidate_version = 10;
        snap.candidates = { L"1. 第一页A", L"2. 第一页B" };

        POINT pt = { 100, 100 };
        win.Show(snap, pt);
        SendMessageW(win.GetHwnd(), WM_PAINT, 0, 0);

        RECT rcPrev = win.GetPrevButtonRect();
        RECT rcNext = win.GetNextButtonRect();
        CHECK_EQ((rcPrev.right - rcPrev.left) >= 18, true, L"Prev button width >= 18px");
        CHECK_EQ((rcNext.right - rcNext.left) >= 18, true, L"Next button width >= 18px");

        // Page 0 of 3: Prev must be disabled, Next must be enabled
        CHECK_EQ(win.IsPrevButtonEnabled(), false, L"Page 0 of 3: Prev button must be disabled");
        CHECK_EQ(win.IsNextButtonEnabled(), true, L"Page 0 of 3: Next button must be enabled");

        bool pageScrolled = false;
        bool scrolledNext = false;
        win.SetOnPageScrollSnapshot([&](const CandidateDisplaySnapshot&, bool nextPage) {
            pageScrolled = true;
            scrolledNext = nextPage;
        });

        // Click disabled Prev button -> must NOT scroll
        POINT ptPrev = { (rcPrev.left + rcPrev.right) / 2, (rcPrev.top + rcPrev.bottom) / 2 };
        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptPrev.x, ptPrev.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptPrev.x, ptPrev.y));
        CHECK_EQ(pageScrolled, false, L"Clicking disabled Prev button must not trigger page scroll");

        // Click enabled Next button -> must scroll to next page
        POINT ptNext = { (rcNext.left + rcNext.right) / 2, (rcNext.top + rcNext.bottom) / 2 };
        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptNext.x, ptNext.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptNext.x, ptNext.y));
        CHECK_EQ(pageScrolled, true, L"Clicking enabled Next button must trigger page scroll");
        CHECK_EQ(scrolledNext, true, L"Scrolled direction must be next (true)");

        // Switch to Page 1 of 3: Both Prev and Next enabled
        snap.page_index = 1;
        win.Show(snap, pt);
        SendMessageW(win.GetHwnd(), WM_PAINT, 0, 0);
        CHECK_EQ(win.IsPrevButtonEnabled(), true, L"Page 1 of 3: Prev button must be enabled");
        CHECK_EQ(win.IsNextButtonEnabled(), true, L"Page 1 of 3: Next button must be enabled");

        pageScrolled = false;
        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptPrev.x, ptPrev.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptPrev.x, ptPrev.y));
        CHECK_EQ(pageScrolled, true, L"Clicking enabled Prev button on page 1 must trigger scroll");
        CHECK_EQ(scrolledNext, false, L"Scrolled direction must be prev (false)");

        // Switch to Page 2 of 3 (last page): Prev enabled, Next disabled
        snap.page_index = 2;
        win.Show(snap, pt);
        SendMessageW(win.GetHwnd(), WM_PAINT, 0, 0);
        CHECK_EQ(win.IsPrevButtonEnabled(), true, L"Page 2 of 3: Prev button must be enabled");
        CHECK_EQ(win.IsNextButtonEnabled(), false, L"Page 2 of 3: Next button must be disabled");

        pageScrolled = false;
        SendMessageW(win.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptNext.x, ptNext.y));
        SendMessageW(win.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptNext.x, ptNext.y));
        CHECK_EQ(pageScrolled, false, L"Clicking disabled Next button on last page must not trigger scroll");

        win.Destroy();
        CHECK_EQ(CCandidateWindow::GetClassRefCount(), 0, L"Destroy restores class ref to 0");
    }
    std::wcout << L"[PASS] Verified: Candidate window paging arrows (< and >) rendering, state enable/disable, and mouse click navigation." << std::endl;

    std::wcout << L"=== All Candidate Window tests PASSED! ===" << std::endl;
    return 0;
}

