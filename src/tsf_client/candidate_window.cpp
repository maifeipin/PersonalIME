#include "candidate_window.h"
#include "globals.h"

static const wchar_t* CANDIDATE_WND_CLASS = L"PersonalIME_CandidateWnd";
static LONG s_cClassRef = 0;

LONG CCandidateWindow::GetClassRefCount() {
    return InterlockedCompareExchange(&s_cClassRef, 0, 0);
}

CCandidateWindow::CCandidateWindow() {}

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
        WS_POPUP | WS_BORDER,
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
            L"Microsoft YaHei"
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

    // Calculate dimensions
    HDC hdc = GetDC(m_hWnd);
    HFONT hOldFont = (HFONT)SelectObject(hdc, m_hFont);

    int totalWidth = 16;
    int maxHeight = 28;

    for (const auto& cand : m_candidates) {
        RECT rcText = { 0, 0, 0, 0 };
        DrawTextW(hdc, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_CALCRECT | DT_SINGLELINE);
        totalWidth += (rcText.right - rcText.left) + 16;
        if (rcText.bottom - rcText.top > maxHeight) {
            maxHeight = rcText.bottom - rcText.top;
        }
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(m_hWnd, hdc);

    int totalHeight = maxHeight + 12;

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

    if (uMsg == WM_PAINT && pThis) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);

        HBRUSH hBg = CreateSolidBrush(RGB(248, 249, 250));
        FillRect(hdc, &clientRc, hBg);
        DeleteObject(hBg);

        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hBorderPen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, clientRc.left, clientRc.top, clientRc.right, clientRc.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hBorderPen);

        HFONT hOldFont = (HFONT)SelectObject(hdc, pThis->m_hFont);
        SetBkMode(hdc, TRANSPARENT);

        int curX = 10;
        int curY = 6;

        for (size_t i = 0; i < pThis->m_candidates.size(); ++i) {
            const auto& cand = pThis->m_candidates[i];
            SetTextColor(hdc, i == 0 ? RGB(0, 102, 204) : RGB(30, 30, 30));

            RECT rcText = { curX, curY, clientRc.right, clientRc.bottom };
            DrawTextW(hdc, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_SINGLELINE);

            RECT rcCalc = { 0, 0, 0, 0 };
            DrawTextW(hdc, cand.c_str(), static_cast<int>(cand.length()), &rcCalc, DT_CALCRECT | DT_SINGLELINE);
            curX += (rcCalc.right - rcCalc.left) + 16;
        }

        SelectObject(hdc, hOldFont);
        EndPaint(hWnd, &ps);
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
