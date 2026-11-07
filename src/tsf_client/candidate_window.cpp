#include "candidate_window.h"
#include "globals.h"

static const wchar_t* CANDIDATE_WND_CLASS = L"PersonalIME_CandidateWnd";
static LONG s_cClassRef = 0;

LONG CCandidateWindow::GetClassRefCount() {
    return InterlockedCompareExchange(&s_cClassRef, 0, 0);
}

CCandidateWindow::CCandidateWindow() {
    InitDefaultPlugins();
}

void CCandidateWindow::InitDefaultPlugins() {
    m_plugins = {
        { 1, L"计算器", L"🧮", L"calc.exe", L"" },
        { 2, L"画图", L"🎨", L"mspaint.exe", L"" },
        { 3, L"Edge浏览器", L"🌐", L"msedge.exe", L"https://maifeipin.com" }
    };
}

void CCandidateWindow::TriggerPluginButton(uint32_t pluginId) {
    for (const auto& p : m_plugins) {
        if (p.id == pluginId) {
            if (m_onExecutePlugin) {
                m_onExecutePlugin(p);
            }
            break;
        }
    }
}

CCandidateWindow::~CCandidateWindow() {
    Destroy();
}

bool CCandidateWindow::EnsureWindow() {
    if (m_hWnd && IsWindow(m_hWnd)) {
        return true;
    }

    if (!m_hasRegisteredClassRef) {
        if (InterlockedIncrement(&s_cClassRef) == 1) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = CANDIDATE_WND_CLASS;
            if (!RegisterClassExW(&wc)) {
                DWORD err = GetLastError();
                if (err != ERROR_CLASS_ALREADY_EXISTS) {
                    InterlockedDecrement(&s_cClassRef);
                    return false;
                }
            }
        }
        m_hasRegisteredClassRef = true;
    }

    m_hWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        CANDIDATE_WND_CLASS,
        L"",
        WS_POPUP,
        0, 0, 100, 30,
        nullptr,
        nullptr,
        g_hInst,
        this
    );

    if (m_hWnd) {
        m_hFont = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI"
        );
        m_hBoldFont = CreateFontW(
            -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI"
        );
        m_hSubFont = CreateFontW(
            -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );
    } else {
        // Window creation failed, release class ref
        if (m_hasRegisteredClassRef) {
            m_hasRegisteredClassRef = false;
            if (InterlockedDecrement(&s_cClassRef) == 0) {
                UnregisterClassW(CANDIDATE_WND_CLASS, g_hInst);
            }
        }
    }

    return m_hWnd != nullptr;
}

void CCandidateWindow::Destroy() {
    if (m_hWnd && IsWindow(m_hWnd)) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
    if (m_hFont) {
        DeleteObject(m_hFont);
        m_hFont = nullptr;
    }
    if (m_hBoldFont) {
        DeleteObject(m_hBoldFont);
        m_hBoldFont = nullptr;
    }
    if (m_hSubFont) {
        DeleteObject(m_hSubFont);
        m_hSubFont = nullptr;
    }
    if (m_hasRegisteredClassRef) {
        m_hasRegisteredClassRef = false;
        LONG refs = InterlockedDecrement(&s_cClassRef);
        if (refs == 0) {
            UnregisterClassW(CANDIDATE_WND_CLASS, g_hInst);
        }
    }
}

void CCandidateWindow::Hide() {
    if (m_hWnd && IsWindow(m_hWnd)) {
        ShowWindow(m_hWnd, SW_HIDE);
    }
    m_candidates.clear();
    m_candidateRects.clear();
    m_selectedIndex = 0;
    m_hoveredIndex = -1;
    m_pressedIndex = -1;
    m_isHoveringMgrBtn = false;
    m_isPressedMgrBtn = false;
    m_trackingMouse = false;
}

void CCandidateWindow::SetSelectedIndex(int index) {
    if (m_candidates.empty()) {
        m_selectedIndex = 0;
        return;
    }
    int clamped = index;
    if (clamped < 0) clamped = 0;
    if (clamped >= static_cast<int>(m_candidates.size())) {
        clamped = static_cast<int>(m_candidates.size()) - 1;
    }
    if (m_selectedIndex != clamped) {
        m_selectedIndex = clamped;
        if (m_hWnd && IsWindow(m_hWnd)) {
            InvalidateRect(m_hWnd, nullptr, FALSE);
        }
    }
}

void CCandidateWindow::TriggerManagerButton() {
    Hide();
    if (m_onOpenManager) {
        m_onOpenManager();
    }
}

void CCandidateWindow::Show(const std::vector<std::wstring>& candidates, POINT pt) {
    if (candidates.empty()) {
        Hide();
        return;
    }

    if (!EnsureWindow()) {
        return;
    }

    m_candidates = candidates;
    m_selectedIndex = 0;
    m_hoveredIndex = -1;
    m_pressedIndex = -1;
    m_candidateRects.clear();

    // Calculate dimensions using bold font so switching selection never shifts candidate layout
    HDC hdc = GetDC(m_hWnd);
    HFONT hMeasureFont = m_hBoldFont ? m_hBoldFont : m_hFont;
    HFONT hOldFont = (HFONT)SelectObject(hdc, hMeasureFont);

    int maxHeight = 24;
    std::vector<int> candWidths(m_candidates.size(), 0);

    for (size_t i = 0; i < m_candidates.size(); ++i) {
        const auto& cand = m_candidates[i];
        size_t bracketPos = cand.find(L'[');
        if (bracketPos != std::wstring::npos) {
            std::wstring mainPart = cand.substr(0, bracketPos);
            std::wstring hintPart = cand.substr(bracketPos);
            SelectObject(hdc, hMeasureFont);
            RECT rcMain = { 0, 0, 0, 0 };
            DrawTextW(hdc, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMain, DT_CALCRECT | DT_SINGLELINE);
            SelectObject(hdc, m_hSubFont ? m_hSubFont : m_hFont);
            RECT rcHint = { 0, 0, 0, 0 };
            DrawTextW(hdc, hintPart.c_str(), static_cast<int>(hintPart.length()), &rcHint, DT_CALCRECT | DT_SINGLELINE);
            candWidths[i] = (rcMain.right - rcMain.left) + 2 + (rcHint.right - rcHint.left);
            if (rcMain.bottom - rcMain.top > maxHeight) {
                maxHeight = rcMain.bottom - rcMain.top;
            }
        } else {
            SelectObject(hdc, hMeasureFont);
            RECT rcText = { 0, 0, 0, 0 };
            DrawTextW(hdc, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_CALCRECT | DT_SINGLELINE);
            candWidths[i] = (rcText.right - rcText.left);
            if (rcText.bottom - rcText.top > maxHeight) {
                maxHeight = rcText.bottom - rcText.top;
            }
        }
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(m_hWnd, hdc);

    int totalHeight = maxHeight + 14;
    if (totalHeight < 34) totalHeight = 34;

    int curX = 8;
    m_candidateRects.resize(m_candidates.size());
    for (size_t i = 0; i < m_candidates.size(); ++i) {
        int candW = candWidths[i];
        m_candidateRects[i] = { curX - 4, 4, curX + candW + 6, totalHeight - 4 };
        curX += candW + 16;
    }
    int totalWidth = curX + 4;

    // Reserve space for Manager button (30px) and Plugin buttons (28px each)
    constexpr int MGR_BTN_WIDTH = 30;
    constexpr int PLUGIN_BTN_WIDTH = 28;
    int pluginTotalWidth = static_cast<int>(m_plugins.size()) * PLUGIN_BTN_WIDTH;
    totalWidth += MGR_BTN_WIDTH + pluginTotalWidth;

    // Immediately establish button rectangles so geometry checks and mouse hits succeed before WM_PAINT
    int btnStartX = totalWidth - (MGR_BTN_WIDTH + pluginTotalWidth);
    m_rcMgrBtn = { btnStartX, 1, btnStartX + MGR_BTN_WIDTH, totalHeight - 1 };
    int pX = btnStartX + MGR_BTN_WIDTH;
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        m_plugins[i].rcBtn = { pX, 1, pX + PLUGIN_BTN_WIDTH, totalHeight - 1 };
        pX += PLUGIN_BTN_WIDTH;
    }

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    int x = pt.x;
    int y = pt.y;

    if (x + totalWidth > mi.rcWork.right) {
        x = mi.rcWork.right - totalWidth;
    }
    if (y + totalHeight > mi.rcWork.bottom) {
        y = pt.y - totalHeight - 20;
    }
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    // Apply smooth rounded window region
    HRGN hRgn = CreateRoundRectRgn(0, 0, totalWidth + 1, totalHeight + 1, 8, 8);
    SetWindowRgn(m_hWnd, hRgn, TRUE);

    SetWindowPos(
        m_hWnd,
        HWND_TOPMOST,
        x, y, totalWidth, totalHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
    InvalidateRect(m_hWnd, nullptr, TRUE);
}

LRESULT CALLBACK CCandidateWindow::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CCandidateWindow* pThis = nullptr;
    if (uMsg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = static_cast<CCandidateWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<CCandidateWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (uMsg == WM_MOUSEACTIVATE) {
        // Crucial: do NOT activate candidate window on mouse click, preserving host input focus!
        return MA_NOACTIVATE;
    }

    if (uMsg == WM_SETCURSOR && pThis) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        for (const auto& rcCand : pThis->m_candidateRects) {
            if (PtInRect(&rcCand, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        if (PtInRect(&pThis->m_rcMgrBtn, pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        for (const auto& p : pThis->m_plugins) {
            if (PtInRect(&p.rcBtn, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
    }

    if (uMsg == WM_MOUSEWHEEL && pThis) {
        short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (zDelta != 0 && pThis->m_onPageScroll) {
            pThis->m_onPageScroll(zDelta < 0);
        }
        return 0;
    }

    if (uMsg == WM_MOUSEMOVE && pThis) {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int newHovered = -1;
        for (size_t i = 0; i < pThis->m_candidateRects.size(); ++i) {
            if (PtInRect(&pThis->m_candidateRects[i], pt)) {
                newHovered = static_cast<int>(i);
                break;
            }
        }
        if (newHovered != pThis->m_hoveredIndex || (newHovered >= 0 && newHovered != pThis->m_selectedIndex)) {
            pThis->m_hoveredIndex = newHovered;
            if (newHovered >= 0) {
                pThis->m_selectedIndex = newHovered;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        bool inside = PtInRect(&pThis->m_rcMgrBtn, pt);
        if (inside != pThis->m_isHoveringMgrBtn) {
            pThis->m_isHoveringMgrBtn = inside;
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
        }
        for (auto& p : pThis->m_plugins) {
            bool inP = PtInRect(&p.rcBtn, pt);
            if (inP != p.isHovered) {
                p.isHovered = inP;
                InvalidateRect(hWnd, &p.rcBtn, FALSE);
            }
        }
        if (!pThis->m_trackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            pThis->m_trackingMouse = true;
        }
        return 0;
    }

    if (uMsg == WM_MOUSELEAVE && pThis) {
        pThis->m_trackingMouse = false;
        if (pThis->m_hoveredIndex != -1 || pThis->m_pressedIndex != -1) {
            pThis->m_hoveredIndex = -1;
            pThis->m_pressedIndex = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (pThis->m_isHoveringMgrBtn || pThis->m_isPressedMgrBtn) {
            pThis->m_isHoveringMgrBtn = false;
            pThis->m_isPressedMgrBtn = false;
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
        }
        for (auto& p : pThis->m_plugins) {
            if (p.isHovered || p.isPressed) {
                p.isHovered = false;
                p.isPressed = false;
                InvalidateRect(hWnd, &p.rcBtn, FALSE);
            }
        }
        return 0;
    }

    if (uMsg == WM_LBUTTONDOWN && pThis) {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        for (size_t i = 0; i < pThis->m_candidateRects.size(); ++i) {
            if (PtInRect(&pThis->m_candidateRects[i], pt)) {
                pThis->m_pressedIndex = static_cast<int>(i);
                pThis->m_selectedIndex = static_cast<int>(i);
                SetCapture(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
        }
        if (PtInRect(&pThis->m_rcMgrBtn, pt)) {
            pThis->m_isPressedMgrBtn = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
            return 0;
        }
        for (auto& p : pThis->m_plugins) {
            if (PtInRect(&p.rcBtn, pt)) {
                p.isPressed = true;
                SetCapture(hWnd);
                InvalidateRect(hWnd, &p.rcBtn, FALSE);
                return 0;
            }
        }
    }

    if (uMsg == WM_LBUTTONUP && pThis) {
        if (GetCapture() == hWnd) {
            ReleaseCapture();
        }
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        if (pThis->m_pressedIndex >= 0) {
            int clickedIdx = pThis->m_pressedIndex;
            pThis->m_pressedIndex = -1;
            if (clickedIdx < static_cast<int>(pThis->m_candidateRects.size()) &&
                PtInRect(&pThis->m_candidateRects[clickedIdx], pt)) {
                if (pThis->m_onSelectCandidate) {
                    pThis->m_onSelectCandidate(clickedIdx);
                }
                return 0;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (pThis->m_isPressedMgrBtn) {
            pThis->m_isPressedMgrBtn = false;
            if (PtInRect(&pThis->m_rcMgrBtn, pt)) {
                pThis->TriggerManagerButton();
                return 0;
            }
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
        }
        for (auto& p : pThis->m_plugins) {
            if (p.isPressed) {
                p.isPressed = false;
                if (PtInRect(&p.rcBtn, pt)) {
                    pThis->TriggerPluginButton(p.id);
                    return 0;
                }
                InvalidateRect(hWnd, &p.rcBtn, FALSE);
            }
        }
        return 0;
    }

    if (uMsg == WM_PAINT && pThis) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);

        // Double-buffered rendering for 100% flicker-free output
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientRc.right, clientRc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // Crisp pure white background
        HBRUSH hBg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(memDC, &clientRc, hBg);
        DeleteObject(hBg);

        // Subtle, elegant rounded border
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(218, 222, 229));
        HPEN hOldPen = (HPEN)SelectObject(memDC, hBorderPen);
        SelectObject(memDC, GetStockObject(NULL_BRUSH));
        RoundRect(memDC, clientRc.left, clientRc.top, clientRc.right, clientRc.bottom, 8, 8);
        SelectObject(memDC, hOldPen);
        DeleteObject(hBorderPen);

        SetBkMode(memDC, TRANSPARENT);

        int centerY = (clientRc.bottom - clientRc.top) / 2;

        for (size_t i = 0; i < pThis->m_candidates.size(); ++i) {
            const auto& cand = pThis->m_candidates[i];
            bool isSelected = (static_cast<int>(i) == pThis->m_selectedIndex);
            bool isHovered = (static_cast<int>(i) == pThis->m_hoveredIndex);

            HFONT hItemFont = (isSelected && pThis->m_hBoldFont) ? pThis->m_hBoldFont : pThis->m_hFont;
            SelectObject(memDC, hItemFont);
            RECT rcCalc = { 0, 0, 0, 0 };
            DrawTextW(memDC, cand.c_str(), static_cast<int>(cand.length()), &rcCalc, DT_CALCRECT | DT_SINGLELINE);
            int candH = rcCalc.bottom - rcCalc.top;
            int textY = centerY - (candH / 2);

            RECT rcPill = (i < pThis->m_candidateRects.size())
                ? pThis->m_candidateRects[i]
                : RECT{ 5, 4, 60, clientRc.bottom - 4 };
            int curX = rcPill.left + 4;

            if (isSelected) {
                // Selected candidate: modern soft-blue accent pill
                HBRUSH hPillBrush = CreateSolidBrush(RGB(240, 246, 255));
                HPEN hPillPen = CreatePen(PS_SOLID, 1, RGB(190, 220, 255));
                HPEN hOldPillPen = (HPEN)SelectObject(memDC, hPillPen);
                HBRUSH hOldPillBrush = (HBRUSH)SelectObject(memDC, hPillBrush);
                RoundRect(memDC, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom, 6, 6);
                SelectObject(memDC, hOldPillPen);
                SelectObject(memDC, hOldPillBrush);
                DeleteObject(hPillPen);
                DeleteObject(hPillBrush);

                size_t bracketPos = cand.find(L'[');
                if (bracketPos != std::wstring::npos) {
                    std::wstring mainPart = cand.substr(0, bracketPos);
                    std::wstring hintPart = cand.substr(bracketPos);

                    RECT rcMain = { curX, textY, clientRc.right, clientRc.bottom };
                    SetTextColor(memDC, RGB(0, 102, 204));
                    DrawTextW(memDC, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMain, DT_SINGLELINE);

                    RECT rcMainCalc = { 0, 0, 0, 0 };
                    DrawTextW(memDC, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMainCalc, DT_CALCRECT | DT_SINGLELINE);

                    SelectObject(memDC, pThis->m_hSubFont ? pThis->m_hSubFont : pThis->m_hFont);
                    RECT rcHint = { curX + (rcMainCalc.right - rcMainCalc.left) + 2, textY + 2, clientRc.right, clientRc.bottom };
                    SetTextColor(memDC, RGB(100, 130, 165));
                    DrawTextW(memDC, hintPart.c_str(), static_cast<int>(hintPart.length()), &rcHint, DT_SINGLELINE);
                    SelectObject(memDC, hItemFont);
                } else {
                    SetTextColor(memDC, RGB(0, 102, 204));
                    RECT rcText = { curX, textY, clientRc.right, clientRc.bottom };
                    DrawTextW(memDC, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_SINGLELINE);
                }
            } else {
                if (isHovered) {
                    HBRUSH hHoverBrush = CreateSolidBrush(RGB(245, 247, 250));
                    HPEN hHoverPen = CreatePen(PS_SOLID, 1, RGB(220, 226, 235));
                    HPEN hOldHoverPen = (HPEN)SelectObject(memDC, hHoverPen);
                    HBRUSH hOldHoverBrush = (HBRUSH)SelectObject(memDC, hHoverBrush);
                    RoundRect(memDC, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom, 6, 6);
                    SelectObject(memDC, hOldHoverPen);
                    SelectObject(memDC, hOldHoverBrush);
                    DeleteObject(hHoverPen);
                    DeleteObject(hHoverBrush);
                }

                size_t bracketPos = cand.find(L'[');
                if (bracketPos != std::wstring::npos) {
                    std::wstring mainPart = cand.substr(0, bracketPos);
                    std::wstring hintPart = cand.substr(bracketPos);

                    RECT rcMain = { curX, textY, clientRc.right, clientRc.bottom };
                    SetTextColor(memDC, RGB(40, 45, 53));
                    DrawTextW(memDC, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMain, DT_SINGLELINE);

                    RECT rcMainCalc = { 0, 0, 0, 0 };
                    DrawTextW(memDC, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMainCalc, DT_CALCRECT | DT_SINGLELINE);

                    SelectObject(memDC, pThis->m_hSubFont ? pThis->m_hSubFont : pThis->m_hFont);
                    RECT rcHint = { curX + (rcMainCalc.right - rcMainCalc.left) + 2, textY + 2, clientRc.right, clientRc.bottom };
                    SetTextColor(memDC, RGB(156, 163, 175));
                    DrawTextW(memDC, hintPart.c_str(), static_cast<int>(hintPart.length()), &rcHint, DT_SINGLELINE);
                    SelectObject(memDC, pThis->m_hFont);
                } else {
                    SetTextColor(memDC, RGB(40, 45, 53));
                    RECT rcText = { curX, textY, clientRc.right, clientRc.bottom };
                    DrawTextW(memDC, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_SINGLELINE);
                }
            }
        }

        // --- Manager Button Layout & Rendering ---
        constexpr int MGR_BTN_WIDTH = 30;
        constexpr int PLUGIN_BTN_WIDTH = 28;
        int pluginTotalWidth = static_cast<int>(pThis->m_plugins.size()) * PLUGIN_BTN_WIDTH;
        int btnStartX = clientRc.right - (MGR_BTN_WIDTH + pluginTotalWidth);

        pThis->m_rcMgrBtn = { btnStartX, clientRc.top + 1, btnStartX + MGR_BTN_WIDTH, clientRc.bottom - 1 };

        // Subtle vertical separator line
        HPEN hSepPen = CreatePen(PS_SOLID, 1, RGB(230, 233, 238));
        HPEN hOldPen2 = (HPEN)SelectObject(memDC, hSepPen);
        MoveToEx(memDC, pThis->m_rcMgrBtn.left, clientRc.top + 7, nullptr);
        LineTo(memDC, pThis->m_rcMgrBtn.left, clientRc.bottom - 7);

        // Button background
        RECT btnInner = { pThis->m_rcMgrBtn.left + 2, 4, pThis->m_rcMgrBtn.right - 2, clientRc.bottom - 4 };
        if (pThis->m_isPressedMgrBtn) {
            HBRUSH hPressBg = CreateSolidBrush(RGB(226, 234, 246));
            FillRect(memDC, &btnInner, hPressBg);
            DeleteObject(hPressBg);
        } else if (pThis->m_isHoveringMgrBtn) {
            HBRUSH hHoverBg = CreateSolidBrush(RGB(242, 244, 247));
            FillRect(memDC, &btnInner, hHoverBg);
            DeleteObject(hHoverBg);
        }

        // Button icon: gear symbol ⚙ (U+2699)
        SelectObject(memDC, pThis->m_hFont);
        SetTextColor(memDC, pThis->m_isHoveringMgrBtn ? RGB(0, 102, 204) : RGB(140, 146, 156));
        const wchar_t* btnIcon = L"\x2699";
        DrawTextW(memDC, btnIcon, 1, &btnInner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // --- Render Plugin Buttons ---
        int pLeft = btnStartX + MGR_BTN_WIDTH;
        for (size_t i = 0; i < pThis->m_plugins.size(); ++i) {
            auto& p = pThis->m_plugins[i];
            p.rcBtn = { pLeft, clientRc.top + 1, pLeft + PLUGIN_BTN_WIDTH, clientRc.bottom - 1 };

            // Separator line
            MoveToEx(memDC, p.rcBtn.left, clientRc.top + 7, nullptr);
            LineTo(memDC, p.rcBtn.left, clientRc.bottom - 7);

            // Background
            RECT pInner = { p.rcBtn.left + 2, 4, p.rcBtn.right - 2, clientRc.bottom - 4 };
            if (p.isPressed) {
                HBRUSH hPressBg = CreateSolidBrush(RGB(226, 234, 246));
                FillRect(memDC, &pInner, hPressBg);
                DeleteObject(hPressBg);
            } else if (p.isHovered) {
                HBRUSH hHoverBg = CreateSolidBrush(RGB(242, 244, 247));
                FillRect(memDC, &pInner, hHoverBg);
                DeleteObject(hHoverBg);
            }

            // Draw plugin icon (Emoji or symbol)
            DrawTextW(memDC, p.icon.c_str(), static_cast<int>(p.icon.length()), &pInner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            pLeft += PLUGIN_BTN_WIDTH;
        }

        SelectObject(memDC, hOldPen2);
        DeleteObject(hSepPen);

        // Copy memory DC to screen
        BitBlt(hdc, 0, 0, clientRc.right, clientRc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
