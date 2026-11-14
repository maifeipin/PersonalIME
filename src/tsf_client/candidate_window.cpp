#include "candidate_window.h"
#include "globals.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

static const wchar_t* CANDIDATE_WND_CLASS = L"PersonalIME_CandidateWnd";
static const wchar_t* MORE_MENU_WND_CLASS = L"PersonalIME_MoreMenuWnd";
static LONG s_cClassRef = 0;
static LONG s_cMenuClassRef = 0;
static std::atomic<int> s_appsUseLightThemeOverride{-1};
static std::atomic<bool> s_forceHookInstallFailForTest{false};
static std::atomic<DWORD> s_hookStartDelayMsForTest{0};
static std::atomic<DWORD> s_hookExitDelayMsForTest{0};
static std::atomic<DWORD> s_hookWaitTimeoutMsForTest{0};
static std::atomic<int> s_liveObserverInstances{0};
static constexpr UINT WM_APP_EXTERNAL_CLICK = WM_APP + 0x73;

namespace {

struct MenuObserverEntry {
    uint32_t generation = 0;
    HWND uiHwnd = nullptr;
    HWND menuHwnd = nullptr;
};

enum class ObserverLifecycle {
    Starting,
    Running,
    Stopping
};

struct ObserverInstance {
    uint64_t instanceGen = 0;
    HANDLE hThread = nullptr;
    DWORD threadId = 0;
    HANDLE hReadyEvent = nullptr;
    HHOOK hHook = nullptr;
    DWORD lastError = 0;
    ObserverLifecycle lifecycle = ObserverLifecycle::Starting;
    bool cancelRequested = false;

    ObserverInstance() {
        s_liveObserverInstances.fetch_add(1, std::memory_order_relaxed);
    }

    ~ObserverInstance() {
        if (hReadyEvent) {
            CloseHandle(hReadyEvent);
            hReadyEvent = nullptr;
        }
        if (hThread) {
            CloseHandle(hThread);
            hThread = nullptr;
        }
        s_liveObserverInstances.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct HookObserverState {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t nextInstanceGen = 1;
    std::vector<MenuObserverEntry> entries;
    std::shared_ptr<ObserverInstance> current;
};

HookObserverState& GetHookObserverState() {
    static HookObserverState s_state;
    return s_state;
}

DWORD GetObserverWaitTimeoutMs(DWORD defaultMs) {
    DWORD ov = s_hookWaitTimeoutMsForTest.load(std::memory_order_acquire);
    return (ov > 0) ? ov : defaultMs;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && lParam != 0) {
        bool isMouseDown =
            wParam == WM_LBUTTONDOWN ||
            wParam == WM_RBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN ||
            wParam == WM_NCLBUTTONDOWN ||
            wParam == WM_NCRBUTTONDOWN ||
            wParam == WM_NCMBUTTONDOWN;
        if (isMouseDown) {
            POINT screenPt = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam)->pt;
            std::vector<MenuObserverEntry> snapshot;
            {
                auto& state = GetHookObserverState();
                std::lock_guard<std::mutex> lock(state.mutex);
                snapshot = state.entries;
            }
            LPARAM packedPt = MAKELPARAM(static_cast<short>(screenPt.x), static_cast<short>(screenPt.y));
            for (const auto& entry : snapshot) {
                if (entry.uiHwnd && entry.generation != 0) {
                    PostMessageW(entry.uiHwnd, WM_APP_EXTERNAL_CLICK, static_cast<WPARAM>(entry.generation), packedPt);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

DWORD WINAPI HookObserverThreadProc(LPVOID lpParam) {
    auto* pHolder = static_cast<std::shared_ptr<ObserverInstance>*>(lpParam);
    std::shared_ptr<ObserverInstance> inst = *pHolder;
    delete pHolder;

    HMODULE hMod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&LowLevelMouseProc),
        &hMod);

    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    DWORD delayMs = s_hookStartDelayMsForTest.load(std::memory_order_acquire);
    if (delayMs > 0) {
        Sleep(delayMs);
    }

    auto& state = GetHookObserverState();
    bool cancelledBeforeHook = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        cancelledBeforeHook = inst->cancelRequested;
    }

    HHOOK hHook = nullptr;
    DWORD err = 0;
    if (cancelledBeforeHook) {
        err = ERROR_CANCELLED;
    } else if (s_forceHookInstallFailForTest.load(std::memory_order_acquire)) {
        err = ERROR_ACCESS_DENIED;
    } else {
        hHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);
        if (!hHook) {
            err = GetLastError();
            if (err == 0) err = ERROR_HOOK_NOT_INSTALLED;
        }
    }

    bool shouldRun = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        inst->hHook = hHook;
        inst->lastError = err;
        if (hHook != nullptr && !inst->cancelRequested) {
            inst->lifecycle = ObserverLifecycle::Running;
            shouldRun = true;
        } else {
            inst->lifecycle = ObserverLifecycle::Stopping;
            shouldRun = false;
        }
        if (inst->hReadyEvent) {
            SetEvent(inst->hReadyEvent);
        }
        state.cv.notify_all();
    }

    if (!shouldRun) {
        if (hHook) {
            UnhookWindowsHookEx(hHook);
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            inst->hHook = nullptr;
            if (state.current == inst) {
                state.current.reset();
            }
            state.cv.notify_all();
        }
        // Release thread-side shared_ptr before FreeLibraryAndExitThread (which never unwinds locals)
        inst.reset();
        if (hMod) {
            FreeLibraryAndExitThread(hMod, 1);
        }
        return 1;
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DWORD exitDelayMs = s_hookExitDelayMsForTest.load(std::memory_order_acquire);
    if (exitDelayMs > 0) {
        Sleep(exitDelayMs);
    }

    UnhookWindowsHookEx(hHook);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        inst->hHook = nullptr;
        inst->lifecycle = ObserverLifecycle::Stopping;
        if (state.current == inst) {
            state.current.reset();
        }
        state.cv.notify_all();
    }

    // Release thread-side shared_ptr before FreeLibraryAndExitThread (which never unwinds locals)
    inst.reset();
    if (hMod) {
        FreeLibraryAndExitThread(hMod, 0);
    }
    return 0;
}

bool RegisterMenuObserver(uint32_t generation, HWND uiHwnd, HWND menuHwnd, DWORD* pOutError) {
    if (pOutError) *pOutError = 0;
    if (generation == 0 || !uiHwnd || !menuHwnd) return false;

    DWORD waitTimeoutMs = GetObserverWaitTimeoutMs(2000);
    auto& state = GetHookObserverState();
    std::shared_ptr<ObserverInstance> inst;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        bool stoppedInTime = state.cv.wait_for(
            lock,
            std::chrono::milliseconds(waitTimeoutMs),
            [&state]() {
                return state.current == nullptr || state.current->lifecycle != ObserverLifecycle::Stopping;
            });
        if (!stoppedInTime) {
            if (pOutError) *pOutError = WAIT_TIMEOUT;
            return false;
        }

        state.entries.push_back({ generation, uiHwnd, menuHwnd });

        if (state.current != nullptr && state.current->lifecycle == ObserverLifecycle::Running) {
            return true;
        }

        if (state.current == nullptr) {
            inst = std::make_shared<ObserverInstance>();
            inst->instanceGen = state.nextInstanceGen++;
            inst->hReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            inst->lifecycle = ObserverLifecycle::Starting;
            auto* pArg = new std::shared_ptr<ObserverInstance>(inst);
            DWORD tid = 0;
            inst->hThread = CreateThread(nullptr, 0, HookObserverThreadProc, pArg, 0, &tid);
            inst->threadId = tid;
            if (!inst->hThread) {
                DWORD err = GetLastError();
                if (err == 0) err = ERROR_NOT_ENOUGH_MEMORY;
                delete pArg;
                state.entries.erase(
                    std::remove_if(state.entries.begin(), state.entries.end(),
                        [generation, uiHwnd](const MenuObserverEntry& e) {
                            return e.generation == generation && e.uiHwnd == uiHwnd;
                        }),
                    state.entries.end());
                if (pOutError) *pOutError = err;
                return false;
            }
            state.current = inst;
        } else {
            inst = state.current;
        }
    }

    DWORD waitRes = inst->hReadyEvent ? WaitForSingleObject(inst->hReadyEvent, waitTimeoutMs) : WAIT_FAILED;

    bool ok = false;
    DWORD err = 0;
    HANDLE hThreadToWait = nullptr;
    DWORD tidToQuit = 0;
    bool shouldBoundedJoin = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (waitRes == WAIT_OBJECT_0 &&
            inst->lifecycle == ObserverLifecycle::Running &&
            inst->hHook != nullptr &&
            !inst->cancelRequested) {
            ok = true;
        } else {
            err = (waitRes == WAIT_TIMEOUT) ? WAIT_TIMEOUT : inst->lastError;
            state.entries.erase(
                std::remove_if(state.entries.begin(), state.entries.end(),
                    [generation, uiHwnd](const MenuObserverEntry& e) {
                        return e.generation == generation && e.uiHwnd == uiHwnd;
                    }),
                state.entries.end());
            if (state.entries.empty() && state.current == inst) {
                inst->cancelRequested = true;
                inst->lifecycle = ObserverLifecycle::Stopping;
                tidToQuit = inst->threadId;
                hThreadToWait = inst->hThread;
                // Only do a brief bounded join on immediate hook-install failure, never after a start timeout
                shouldBoundedJoin = (waitRes != WAIT_TIMEOUT);
            }
        }
    }

    if (tidToQuit != 0) {
        PostThreadMessageW(tidToQuit, WM_QUIT, 0, 0);
    }
    if (shouldBoundedJoin && hThreadToWait != nullptr && GetCurrentThreadId() != tidToQuit) {
        if (WaitForSingleObject(hThreadToWait, 200) == WAIT_OBJECT_0) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.current == inst) {
                state.current.reset();
            }
            state.cv.notify_all();
        }
    }

    if (!ok && pOutError) {
        *pOutError = err ? err : ERROR_HOOK_NOT_INSTALLED;
    }
    return ok;
}

void UnregisterMenuObserverForWindow(HWND uiHwnd) {
    if (!uiHwnd) return;
    auto& state = GetHookObserverState();
    std::shared_ptr<ObserverInstance> instToStop;
    HANDLE hThreadToWait = nullptr;
    DWORD tidToQuit = 0;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.entries.erase(
            std::remove_if(state.entries.begin(), state.entries.end(),
                [uiHwnd](const MenuObserverEntry& e) { return e.uiHwnd == uiHwnd; }),
            state.entries.end());
        if (state.entries.empty() && state.current != nullptr) {
            instToStop = state.current;
            instToStop->cancelRequested = true;
            instToStop->lifecycle = ObserverLifecycle::Stopping;
            tidToQuit = instToStop->threadId;
            hThreadToWait = instToStop->hThread;
        }
    }
    if (instToStop) {
        if (tidToQuit != 0) {
            PostThreadMessageW(tidToQuit, WM_QUIT, 0, 0);
        }
        DWORD exitWaitMs = GetObserverWaitTimeoutMs(200);
        if (hThreadToWait && GetCurrentThreadId() != tidToQuit) {
            if (WaitForSingleObject(hThreadToWait, exitWaitMs) == WAIT_OBJECT_0) {
                std::lock_guard<std::mutex> lock(state.mutex);
                if (state.current == instToStop) {
                    state.current.reset();
                }
                state.cv.notify_all();
            }
            // If timed out, background thread retains instToStop + module ref and cleans up upon exit
        }
    }
}

bool IsMenuObserverEntryAlive(uint32_t generation, HWND uiHwnd, HWND menuHwnd) {
    if (generation == 0 || !uiHwnd || !menuHwnd) return false;
    auto& state = GetHookObserverState();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (const auto& e : state.entries) {
        if (e.generation == generation && e.uiHwnd == uiHwnd && e.menuHwnd == menuHwnd) {
            return true;
        }
    }
    return false;
}

} // namespace

bool CCandidateWindow::IsHookObserverActive() {
    auto& state = GetHookObserverState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.current != nullptr &&
           state.current->lifecycle == ObserverLifecycle::Running &&
           state.current->hHook != nullptr &&
           !state.entries.empty();
}

int CCandidateWindow::GetLiveObserverInstanceCountForTest() {
    return s_liveObserverInstances.load(std::memory_order_acquire);
}

void CCandidateWindow::SetHookInstallFailureOverrideForTest(bool fail) {
    s_forceHookInstallFailForTest.store(fail, std::memory_order_release);
}

void CCandidateWindow::SetHookStartDelayOverrideForTest(DWORD delayMs) {
    s_hookStartDelayMsForTest.store(delayMs, std::memory_order_release);
}

void CCandidateWindow::SetHookExitDelayOverrideForTest(DWORD delayMs) {
    s_hookExitDelayMsForTest.store(delayMs, std::memory_order_release);
}

void CCandidateWindow::SetHookWaitTimeoutOverrideForTest(DWORD timeoutMs) {
    s_hookWaitTimeoutMsForTest.store(timeoutMs, std::memory_order_release);
}

LONG CCandidateWindow::GetClassRefCount() {
    return InterlockedCompareExchange(&s_cClassRef, 0, 0);
}

void CCandidateWindow::SetSystemAppsUseLightThemeOverrideForTest(int val) {
    s_appsUseLightThemeOverride.store(val, std::memory_order_release);
}

bool CCandidateWindow::QuerySystemAppsUseLightTheme() {
    int ov = s_appsUseLightThemeOverride.load(std::memory_order_acquire);
    if (ov == 0) return false;
    if (ov == 1) return true;

    DWORD value = 1; // Explicit Light fallback on read failure
    DWORD size = sizeof(value);
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0,
            KEY_QUERY_VALUE,
            &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            RegCloseKey(hKey);
            return value != 0;
        }
        RegCloseKey(hKey);
    }
    return true;
}

CandidateThemePalette CCandidateWindow::BuildPalette(ThemeMode effectiveTheme) {
    CandidateThemePalette p{};
    if (effectiveTheme == ThemeMode::Dark) {
        p.bgColor = RGB(32, 32, 32);
        p.borderColor = RGB(60, 60, 60);
        p.dividerColor = RGB(55, 55, 55);
        p.headerTextColor = RGB(156, 163, 175);
        p.headerMatchColor = RGB(56, 189, 248);
        p.candidateTextColor = RGB(243, 244, 246);
        p.reverseLookupColor = RGB(156, 163, 175);
        p.selectedPillBg = RGB(55, 55, 55);
        p.hoverPillBg = RGB(48, 48, 48);
        p.pressedPillBg = RGB(68, 68, 68);
        p.accentBarColor = RGB(251, 146, 60);
        p.arrowBtnEnabled = RGB(220, 220, 220);
        p.arrowBtnDisabled = RGB(80, 80, 80);
        p.btnHoverColor = RGB(56, 189, 248);
    } else {
        p.bgColor = RGB(255, 255, 255);
        p.borderColor = RGB(226, 232, 240);
        p.dividerColor = RGB(226, 232, 240);
        p.headerTextColor = RGB(100, 116, 139);
        p.headerMatchColor = RGB(14, 116, 144);
        p.candidateTextColor = RGB(30, 41, 59);
        p.reverseLookupColor = RGB(100, 116, 139);
        p.selectedPillBg = RGB(241, 245, 249);
        p.hoverPillBg = RGB(248, 250, 252);
        p.pressedPillBg = RGB(226, 232, 240);
        p.accentBarColor = RGB(249, 115, 22);
        p.arrowBtnEnabled = RGB(71, 85, 105);
        p.arrowBtnDisabled = RGB(203, 213, 225);
        p.btnHoverColor = RGB(14, 116, 144);
    }
    return p;
}

void CCandidateWindow::SetThemeMode(ThemeMode mode) {
    m_themeMode = mode;
    RefreshTheme(true);
}

void CCandidateWindow::RefreshTheme(bool invalidate) {
    ThemeMode eff = ThemeMode::Light;
    if (m_themeMode == ThemeMode::Dark) {
        eff = ThemeMode::Dark;
    } else if (m_themeMode == ThemeMode::Light) {
        eff = ThemeMode::Light;
    } else {
        eff = QuerySystemAppsUseLightTheme() ? ThemeMode::Light : ThemeMode::Dark;
    }
    m_effectiveTheme = eff;
    m_palette = BuildPalette(eff);

    if (m_hWnd && IsWindow(m_hWnd)) {
        HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
        if (!hDwm) hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm) {
            using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
            auto pDwmSet = reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(hDwm, "DwmSetWindowAttribute"));
            if (pDwmSet) {
                constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
                BOOL darkMode = (m_effectiveTheme == ThemeMode::Dark) ? TRUE : FALSE;
                pDwmSet(m_hWnd, kDwmwaUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
            }
        }
        if (invalidate) {
            InvalidateRect(m_hWnd, nullptr, TRUE);
        }
    }
    RefreshMenuTheme();
}

void CCandidateWindow::RefreshMenuTheme() {
    if (m_hMenuWnd && IsWindow(m_hMenuWnd)) {
        HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
        if (!hDwm) hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm) {
            using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
            auto pDwmSet = reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(hDwm, "DwmSetWindowAttribute"));
            if (pDwmSet) {
                constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
                BOOL darkMode = (m_effectiveTheme == ThemeMode::Dark) ? TRUE : FALSE;
                pDwmSet(m_hMenuWnd, kDwmwaUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
            }
        }
        if (IsWindowVisible(m_hMenuWnd)) {
            InvalidateRect(m_hMenuWnd, nullptr, TRUE);
        }
    }
}

UINT CCandidateWindow::QueryDpiForPoint(POINT pt) const {
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        HMODULE hShcore = GetModuleHandleW(L"shcore.dll");
        if (!hShcore) hShcore = LoadLibraryW(L"shcore.dll");
        if (hShcore) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(
                GetProcAddress(hShcore, "GetDpiForMonitor"));
            if (pGetDpiForMonitor) {
                UINT dpiX = 96, dpiY = 96;
                if (SUCCEEDED(pGetDpiForMonitor(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiX >= 72) {
                    return dpiX;
                }
            }
        }
    }
    if (m_hWnd && IsWindow(m_hWnd)) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
            auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(hUser32, "GetDpiForWindow"));
            if (pGetDpiForWindow) {
                UINT dpi = pGetDpiForWindow(m_hWnd);
                if (dpi >= 72) return dpi;
            }
        }
    }
    HDC screenDc = GetDC(nullptr);
    UINT fallbackDpi = 96;
    if (screenDc) {
        int px = GetDeviceCaps(screenDc, LOGPIXELSX);
        if (px >= 72) fallbackDpi = static_cast<UINT>(px);
        ReleaseDC(nullptr, screenDc);
    }
    return fallbackDpi;
}

void CCandidateWindow::RebuildFontsForDpi(UINT dpi) {
    if (dpi < 72) dpi = 96;
    if (m_fontDpi == dpi && m_hFont && m_hBoldFont && m_hSubFont) {
        return;
    }
    if (m_hFont) { DeleteObject(m_hFont); m_hFont = nullptr; }
    if (m_hBoldFont) { DeleteObject(m_hBoldFont); m_hBoldFont = nullptr; }
    if (m_hSubFont) { DeleteObject(m_hSubFont); m_hSubFont = nullptr; }

    int mainPx = -MulDiv(16, static_cast<int>(dpi), 96);
    int subPx = -MulDiv(12, static_cast<int>(dpi), 96);
    m_hFont = CreateFontW(
        mainPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI"
    );
    m_hBoldFont = CreateFontW(
        mainPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI"
    );
    m_hSubFont = CreateFontW(
        subPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    m_fontDpi = dpi;
}

void CCandidateWindow::ApplyWindowRoundingTo(HWND hWnd, int width, int height, bool* pUsedNative) {
    if (pUsedNative) *pUsedNative = false;
    if (!hWnd || !IsWindow(hWnd)) {
        return;
    }
    // Remove any previous custom region before attempting native Win11 DWM rounded corners
    SetWindowRgn(hWnd, nullptr, FALSE);

    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (!hDwm) hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto pDwmSet = reinterpret_cast<DwmSetWindowAttributeFn>(
            GetProcAddress(hDwm, "DwmSetWindowAttribute"));
        if (pDwmSet) {
            constexpr DWORD kDwmwaWindowCornerPreference = 33;
            constexpr DWORD kDwmwcpRound = 2;
            DWORD pref = kDwmwcpRound;
            if (SUCCEEDED(pDwmSet(hWnd, kDwmwaWindowCornerPreference, &pref, sizeof(pref)))) {
                if (pUsedNative) *pUsedNative = true;
                return;
            }
        }
    }

    int radius = ScaleDip(8);
    HRGN hRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    SetWindowRgn(hWnd, hRgn, TRUE);
}

void CCandidateWindow::ApplyWindowRounding(int width, int height) {
    ApplyWindowRoundingTo(m_hWnd, width, height, &m_usingNativeRounding);
}

CCandidateWindow::CCandidateWindow() {
    InitDefaultPlugins();
    RefreshTheme(false);
}

void CCandidateWindow::InitDefaultPlugins() {
    m_plugins = {
        { 1, L"计算器", L"🧮", L"calc.exe", L"" },
        { 2, L"画图", L"🎨", L"mspaint.exe", L"" },
        { 3, L"Edge浏览器", L"🌐", L"msedge.exe", L"https://maifeipin.com" }
    };
}

void CCandidateWindow::SetPlugins(const std::vector<CandidatePluginItem>& plugins) {
    m_plugins = plugins;
    if (m_isMenuOpen) {
        CloseMoreMenu();
    }
    if ((!m_candidates.empty() || m_isHeaderVisible) && m_hWnd && IsWindow(m_hWnd) && IsWindowVisible(m_hWnd)) {
        RelayoutCurrent();
    }
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
        m_dpi = QueryDpiForPoint(m_lastShowPt);
        RebuildFontsForDpi(m_dpi);
        RefreshTheme(false);
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
    CloseMoreMenu();
    if (m_hMenuWnd && IsWindow(m_hMenuWnd)) {
        DestroyWindow(m_hMenuWnd);
        m_hMenuWnd = nullptr;
    }
    if (m_hasRegisteredMenuClassRef) {
        m_hasRegisteredMenuClassRef = false;
        if (InterlockedDecrement(&s_cMenuClassRef) == 0) {
            UnregisterClassW(MORE_MENU_WND_CLASS, g_hInst);
        }
    }
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

void CCandidateWindow::ResetAllPressedStates() {
    m_pressedIndex = -1;
    m_pressedVersion = 0;
    m_pressedContextId = 0;
    m_pressedContextEpoch = 0;
    m_pressedInputGeneration = 0;
    m_pressedMode = CandidateWindowMode::StandardInline;
    m_pressedPageIndex = 0;
    m_pressedCandidateText.clear();

    m_isPressedPrevBtn = false;
    m_isPressedNextBtn = false;
    m_pressedArrowSnapshot = {};

    m_isPressedMgrBtn = false;
    m_isPressedMoreBtn = false;
    for (auto& p : m_plugins) {
        p.isPressed = false;
    }
}

std::wstring CCandidateWindow::ExtractCandidateCoreText(const std::wstring& formattedCandidate) {
    if (formattedCandidate.empty()) return L"";
    std::wstring s = formattedCandidate;
    // Strip leading "1. " or "2. " index prefix
    size_t dotPos = s.find(L". ");
    if (dotPos != std::wstring::npos && dotPos > 0 && dotPos <= 3) {
        bool allDigits = true;
        for (size_t i = 0; i < dotPos; ++i) {
            if (s[i] < L'0' || s[i] > L'9') {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            s = s.substr(dotPos + 2);
        }
    }
    // Strip trailing " [reverse_code]" auxiliary code hint only if it is a well-formed ASCII suffix at the end
    if (s.size() >= 4 && s.back() == L']') {
        size_t bracketPos = s.rfind(L" [");
        if (bracketPos != std::wstring::npos && bracketPos > 0 && bracketPos + 2 < s.size() - 1) {
            bool isValidHint = true;
            for (size_t i = bracketPos + 2; i < s.size() - 1; ++i) {
                wchar_t ch = s[i];
                bool isAsciiHintChar =
                    (ch >= L'a' && ch <= L'z') ||
                    (ch >= L'A' && ch <= L'Z') ||
                    (ch >= L'0' && ch <= L'9') ||
                    ch == L'\'' || ch == L'-' || ch == L'/' ||
                    ch == L',' || ch == L' ' || ch == L'*' ||
                    ch == L'?' || ch == L'+' || ch == L';' || ch == L'.';
                if (!isAsciiHintChar) {
                    isValidHint = false;
                    break;
                }
            }
            if (isValidHint) {
                s = s.substr(0, bracketPos);
            }
        }
    }
    while (!s.empty() && s.back() == L' ') {
        s.pop_back();
    }
    return s;
}

bool CCandidateWindow::ShouldShowPreeditHeader(const CandidateDisplaySnapshot& snap, PreeditDisplayMode displayMode) {
    bool hasPreedit = !snap.raw_input.empty() || !snap.query_code.empty();
    if (!hasPreedit) return false;
    if (displayMode == PreeditDisplayMode::Always) return true;
    if (displayMode == PreeditDisplayMode::CompositionlessOnly) {
        return snap.mode == CandidateWindowMode::Compositionless;
    }
    return false;
}

bool CCandidateWindow::ShouldShowWindow(const CandidateDisplaySnapshot& snap, PreeditDisplayMode displayMode) {
    return !snap.candidates.empty() || ShouldShowPreeditHeader(snap, displayMode);
}

void CCandidateWindow::SetPreeditDisplayMode(PreeditDisplayMode mode) {
    if (m_preeditDisplayMode == mode) return;
    m_preeditDisplayMode = mode;
    if (m_hWnd && IsWindow(m_hWnd) && IsWindowVisible(m_hWnd)) {
        RelayoutCurrent();
    }
}

void CCandidateWindow::UpdatePreviewTextFromSelection() {
    if (!m_candidates.empty() && m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_candidates.size())) {
        if (static_cast<size_t>(m_selectedIndex) < m_snapshot.candidate_core_texts.size() &&
            !m_snapshot.candidate_core_texts[m_selectedIndex].empty()) {
            m_snapshot.preview_text = m_snapshot.candidate_core_texts[m_selectedIndex];
        } else {
            m_snapshot.preview_text = ExtractCandidateCoreText(m_candidates[m_selectedIndex]);
        }
    } else {
        m_snapshot.preview_text.clear();
    }
}

std::wstring CCandidateWindow::GetDisplayedPreeditText() const {
    const std::wstring& code = !m_snapshot.query_code.empty() ? m_snapshot.query_code : m_snapshot.raw_input;
    if (code.empty()) return L"";
    if (!m_snapshot.preview_text.empty()) {
        return code + L" [" + m_snapshot.preview_text + L"]";
    }
    return code;
}

void CCandidateWindow::Hide() {
    CloseMoreMenu();
    if (m_hWnd && IsWindow(m_hWnd)) {
        if (GetCapture() == m_hWnd) {
            ReleaseCapture();
        }
        ShowWindow(m_hWnd, SW_HIDE);
    }
    m_hasDpiOverride = false;
    m_snapshot = {};
    m_candidates.clear();
    m_candidateRects.clear();
    m_isHeaderVisible = false;
    m_headerHeight = 0;
    m_rcHeader = { 0, 0, 0, 0 };
    m_selectedIndex = 0;
    m_hoveredIndex = -1;
    m_isHoveringMgrBtn = false;
    m_isHoveringMoreBtn = false;
    m_isHoveringPrevBtn = false;
    m_isHoveringNextBtn = false;
    for (auto& p : m_plugins) {
        p.isHovered = false;
    }
    m_trackingMouse = false;
    ResetAllPressedStates();
}

void CCandidateWindow::SetSelectedIndex(int index) {
    if (m_candidates.empty()) {
        m_selectedIndex = 0;
        m_snapshot.preview_text.clear();
        return;
    }
    int clamped = index;
    if (clamped < 0) clamped = 0;
    if (clamped >= static_cast<int>(m_candidates.size())) {
        clamped = static_cast<int>(m_candidates.size()) - 1;
    }
    if (m_selectedIndex != clamped) {
        m_selectedIndex = clamped;
        UpdatePreviewTextFromSelection();
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
    CandidateDisplaySnapshot snap{};
    snap.candidates = candidates;
    snap.initial_selected_index = 0;
    Show(snap, pt);
}

void CCandidateWindow::Show(const CandidateDisplaySnapshot& snapshot, POINT pt) {
    LayoutAndShow(snapshot, pt, 0, nullptr);
}

void CCandidateWindow::RelayoutCurrent() {
    CandidateDisplaySnapshot snap = m_snapshot;
    snap.initial_selected_index = m_selectedIndex;
    if (m_hasDpiOverride) {
        RECT suggested = { m_lastLayoutPos.x, m_lastLayoutPos.y, m_lastLayoutPos.x + 1, m_lastLayoutPos.y + 1 };
        LayoutAndShow(snap, m_lastLayoutPos, m_dpi, &suggested);
    } else {
        Show(snap, m_lastShowPt);
    }
}

void CCandidateWindow::LayoutAndShow(const CandidateDisplaySnapshot& snapshot, POINT anchorPt, UINT overrideDpi, const RECT* pSuggestedRect) {
    bool showHeader = ShouldShowPreeditHeader(snapshot, m_preeditDisplayMode);
    if (snapshot.candidates.empty() && !showHeader) {
        Hide();
        return;
    }

    if (!EnsureWindow()) {
        return;
    }

    if (m_hWnd && IsWindow(m_hWnd) && GetCapture() == m_hWnd) {
        ReleaseCapture();
    }
    if (m_isMenuOpen && overrideDpi == 0) {
        CloseMoreMenu();
    }

    m_snapshot = snapshot;
    if (overrideDpi >= 72) {
        m_dpi = overrideDpi;
        m_hasDpiOverride = true;
    } else {
        m_lastShowPt = anchorPt;
        m_hasDpiOverride = false;
        m_dpi = QueryDpiForPoint(anchorPt);
    }
    RebuildFontsForDpi(m_dpi);
    RefreshTheme(false);

    m_candidates = snapshot.candidates;
    int initSel = snapshot.initial_selected_index;
    if (initSel < 0) initSel = 0;
    if (!m_candidates.empty() && initSel >= static_cast<int>(m_candidates.size())) {
        initSel = static_cast<int>(m_candidates.size()) - 1;
    }
    m_selectedIndex = initSel;
    UpdatePreviewTextFromSelection();
    m_isHeaderVisible = showHeader;
    m_hoveredIndex = -1;
    m_isHoveringPrevBtn = false;
    m_isHoveringNextBtn = false;
    m_isHoveringMgrBtn = false;
    m_isHoveringMoreBtn = false;
    for (auto& p : m_plugins) {
        p.isHovered = false;
    }
    m_candidateRects.clear();
    ResetAllPressedStates();

    HMONITOR hMon = pSuggestedRect
        ? MonitorFromRect(pSuggestedRect, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    int workWidth = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
    if (workWidth <= 0) workWidth = ScaleDip(800);
    int maxWindowWidth = (std::min)(workWidth, ScaleDip(760));

    // Calculate dimensions using bold font so switching selection never shifts candidate layout
    HDC hdc = GetDC(m_hWnd);
    HFONT hMeasureFont = m_hBoldFont ? m_hBoldFont : m_hFont;
    HFONT hOldFont = (HFONT)SelectObject(hdc, hMeasureFont);

    int headerMinWidth = 0;
    if (m_isHeaderVisible) {
        std::wstring headerText = GetDisplayedPreeditText();
        SelectObject(hdc, m_hSubFont ? m_hSubFont : m_hFont);
        RECT rcHdr = { 0, 0, 0, 0 };
        DrawTextW(hdc, headerText.c_str(), static_cast<int>(headerText.length()), &rcHdr, DT_CALCRECT | DT_SINGLELINE);
        int hdrTextH = static_cast<int>(rcHdr.bottom - rcHdr.top);
        m_headerHeight = (std::max)(ScaleDip(22), hdrTextH + ScaleDip(6));
        headerMinWidth = static_cast<int>(rcHdr.right - rcHdr.left) + ScaleDip(20);
    } else {
        m_headerHeight = 0;
    }

    int maxHeight = ScaleDip(22);
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
            candWidths[i] = static_cast<int>(rcMain.right - rcMain.left) + ScaleDip(2) + static_cast<int>(rcHint.right - rcHint.left);
            if (rcMain.bottom - rcMain.top > maxHeight) {
                maxHeight = static_cast<int>(rcMain.bottom - rcMain.top);
            }
        } else {
            SelectObject(hdc, hMeasureFont);
            RECT rcText = { 0, 0, 0, 0 };
            DrawTextW(hdc, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_CALCRECT | DT_SINGLELINE);
            candWidths[i] = static_cast<int>(rcText.right - rcText.left);
            if (rcText.bottom - rcText.top > maxHeight) {
                maxHeight = static_cast<int>(rcText.bottom - rcText.top);
            }
        }
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(m_hWnd, hdc);

    int rowTop = m_headerHeight;
    int totalWidth = 0;
    int totalHeight = 0;

    if (!m_candidates.empty()) {
        int candRowHeight = (std::max)(ScaleDip(32), maxHeight + ScaleDip(12));
        totalHeight = rowTop + candRowHeight;

        const int pageBtnW = ScaleDip(20);
        const int mgrBtnW = ScaleDip(28);
        const int moreBtnW = ScaleDip(24);
        int pagingTotalWidth = pageBtnW * 2;
        int fallbackMgrW = m_showFallbackMgrBtn ? mgrBtnW : 0;
        int controlsTotalWidth = pagingTotalWidth + fallbackMgrW + moreBtnW;

        int padLeft = ScaleDip(8);
        int candGap = ScaleDip(16);
        int curX = padLeft;
        for (size_t i = 0; i < m_candidates.size(); ++i) {
            curX += candWidths[i] + candGap;
        }
        int contentWidth = curX + ScaleDip(4) + controlsTotalWidth;
        totalWidth = (std::max)(contentWidth, headerMinWidth);
        if (totalWidth > maxWindowWidth) {
            totalWidth = maxWindowWidth;
        }
        int minRequiredWidth = controlsTotalWidth + ScaleDip(48);
        if (totalWidth < minRequiredWidth && minRequiredWidth <= workWidth) {
            totalWidth = minRequiredWidth;
        }

        // Fit all candidate pills strictly within [availCandLeft, availCandRight)
        // using water-filling allocation so a single long candidate never starves
        // subsequent items on the same page and never overlaps control buttons.
        int controlsStartX = totalWidth - controlsTotalWidth;
        int availCandLeft = ScaleDip(4);
        int availCandRight = (std::max)(availCandLeft, controlsStartX - ScaleDip(4));
        int availTotalWidth = availCandRight - availCandLeft;

        int pillPadL = ScaleDip(4);
        int pillPadR = ScaleDip(6);
        int pillPadV = ScaleDip(4);
        int interPillGap = ScaleDip(6);
        size_t n = m_candidates.size();

        m_candidateRects.assign(n, { 0, 0, 0, 0 });
        if (availTotalWidth > 0 && n > 0) {
            int gapCount = (n > 1) ? static_cast<int>(n - 1) : 0;
            if (gapCount > 0 && availTotalWidth < static_cast<int>(n) * ScaleDip(16) + gapCount * interPillGap) {
                int maxGapBudget = (std::max)(0, availTotalWidth - static_cast<int>(n) * ScaleDip(12));
                interPillGap = (std::min)(interPillGap, maxGapBudget / gapCount);
            }
            int pillBudget = availTotalWidth - gapCount * interPillGap;

            if (pillBudget > 0) {
                std::vector<int> natPillW(n, 0);
                int sumNat = 0;
                for (size_t i = 0; i < n; ++i) {
                    natPillW[i] = pillPadL + candWidths[i] + pillPadR;
                    sumNat += natPillW[i];
                }

                std::vector<int> allocW(n, 0);
                if (sumNat <= pillBudget) {
                    allocW = natPillW;
                } else {
                    std::vector<bool> settled(n, false);
                    int remainingBudget = pillBudget;
                    int remainingCount = static_cast<int>(n);
                    bool changed = true;
                    while (changed && remainingCount > 0) {
                        changed = false;
                        int share = remainingBudget / remainingCount;
                        for (size_t i = 0; i < n; ++i) {
                            if (!settled[i] && natPillW[i] <= share) {
                                allocW[i] = natPillW[i];
                                remainingBudget -= natPillW[i];
                                remainingCount--;
                                settled[i] = true;
                                changed = true;
                            }
                        }
                    }
                    if (remainingCount > 0) {
                        int share = remainingBudget / remainingCount;
                        int rem = remainingBudget % remainingCount;
                        for (size_t i = 0; i < n; ++i) {
                            if (!settled[i]) {
                                allocW[i] = share + (rem > 0 ? 1 : 0);
                                if (rem > 0) --rem;
                            }
                        }
                    }
                }

                int pillX = availCandLeft;
                for (size_t i = 0; i < n; ++i) {
                    int w = allocW[i];
                    if (w > 0 && pillX < availCandRight) {
                        int pillRight = (std::min)(pillX + w, availCandRight);
                        if (pillRight > pillX) {
                            m_candidateRects[i] = { pillX, rowTop + pillPadV, pillRight, totalHeight - pillPadV };
                        }
                    }
                    pillX += w + interPillGap;
                }
            }
        }

        // Immediately establish button rectangles so geometry checks and mouse hits succeed before WM_PAINT
        m_rcPrevBtn = { controlsStartX, rowTop + 1, controlsStartX + pageBtnW, totalHeight - 1 };
        m_rcNextBtn = { controlsStartX + pageBtnW, rowTop + 1, controlsStartX + pagingTotalWidth, totalHeight - 1 };

        int btnStartX = controlsStartX + pagingTotalWidth;
        if (m_showFallbackMgrBtn) {
            m_rcMgrBtn = { btnStartX, rowTop + 1, btnStartX + mgrBtnW, totalHeight - 1 };
            btnStartX += mgrBtnW;
        } else {
            m_rcMgrBtn = { 0, 0, 0, 0 };
        }
        m_rcMoreBtn = { btnStartX, rowTop + 1, btnStartX + moreBtnW, totalHeight - 1 };
        for (auto& p : m_plugins) {
            p.rcBtn = { 0, 0, 0, 0 };
        }
    } else {
        // Header-only mode when raw_input is non-empty but no candidates match
        totalWidth = (std::max)(headerMinWidth, ScaleDip(96));
        if (totalWidth > maxWindowWidth) {
            totalWidth = maxWindowWidth;
        }
        totalHeight = m_headerHeight + ScaleDip(6);
        m_rcPrevBtn = { 0, 0, 0, 0 };
        m_rcNextBtn = { 0, 0, 0, 0 };
        m_rcMgrBtn = { 0, 0, 0, 0 };
        m_rcMoreBtn = { 0, 0, 0, 0 };
        for (auto& p : m_plugins) {
            p.rcBtn = { 0, 0, 0, 0 };
        }
    }

    if (m_isHeaderVisible) {
        m_rcHeader = { 0, 0, totalWidth, m_headerHeight };
    } else {
        m_rcHeader = { 0, 0, 0, 0 };
    }

    int x = pSuggestedRect ? pSuggestedRect->left : anchorPt.x;
    int y = pSuggestedRect ? pSuggestedRect->top : anchorPt.y;

    if (x + totalWidth > mi.rcWork.right) {
        x = mi.rcWork.right - totalWidth;
    }
    if (y + totalHeight > mi.rcWork.bottom) {
        y = (pSuggestedRect ? pSuggestedRect->top : anchorPt.y) - totalHeight - ScaleDip(20);
    }
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x + totalWidth > mi.rcWork.right) x = (std::max)(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - totalWidth);
    if (y + totalHeight > mi.rcWork.bottom) y = (std::max)(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - totalHeight);

    m_lastLayoutPos = { x, y };
    ApplyWindowRounding(totalWidth, totalHeight);

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

    if (uMsg == WM_APP_EXTERNAL_CLICK && pThis) {
        POINT screenPt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        pThis->HandleExternalClick(screenPt, static_cast<uint32_t>(wParam));
        return 0;
    }

    if ((uMsg == WM_SETTINGCHANGE || uMsg == WM_THEMECHANGED) && pThis) {
        if (uMsg == WM_THEMECHANGED || lParam == 0 ||
            wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
            pThis->RefreshTheme(true);
        }
        return 0;
    }

    if (uMsg == 0x02E0 /* WM_DPICHANGED */ && pThis) {
        UINT newDpi = HIWORD(wParam);
        const RECT* pSuggested = reinterpret_cast<const RECT*>(lParam);
        if (newDpi >= 72) {
            pThis->m_dpi = newDpi;
            pThis->RebuildFontsForDpi(newDpi);
            if ((!pThis->m_candidates.empty() || pThis->m_isHeaderVisible) && IsWindowVisible(hWnd)) {
                CandidateDisplaySnapshot snap = pThis->m_snapshot;
                snap.initial_selected_index = pThis->m_selectedIndex;
                POINT anchor = pThis->m_lastShowPt;
                if (pSuggested) {
                    anchor.x = pSuggested->left;
                    anchor.y = pSuggested->top;
                }
                pThis->LayoutAndShow(snap, anchor, newDpi, pSuggested);
            }
        }
        return 0;
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
        if (PtInRect(&pThis->m_rcPrevBtn, pt) && pThis->IsPrevButtonEnabled()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (PtInRect(&pThis->m_rcNextBtn, pt) && pThis->IsNextButtonEnabled()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (PtInRect(&pThis->m_rcMgrBtn, pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (PtInRect(&pThis->m_rcMoreBtn, pt)) {
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
        if (zDelta != 0 && !pThis->m_candidates.empty()) {
            CandidateDisplaySnapshot snapCopy = pThis->m_snapshot;
            if (pThis->m_onPageScrollSnapshot) {
                pThis->m_onPageScrollSnapshot(snapCopy, zDelta < 0);
            }
            if (pThis->m_onPageScroll) {
                pThis->m_onPageScroll(zDelta < 0);
            }
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
                pThis->UpdatePreviewTextFromSelection();
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        bool inPrev = PtInRect(&pThis->m_rcPrevBtn, pt) && pThis->IsPrevButtonEnabled();
        if (inPrev != pThis->m_isHoveringPrevBtn) {
            pThis->m_isHoveringPrevBtn = inPrev;
            InvalidateRect(hWnd, &pThis->m_rcPrevBtn, FALSE);
        }
        bool inNext = PtInRect(&pThis->m_rcNextBtn, pt) && pThis->IsNextButtonEnabled();
        if (inNext != pThis->m_isHoveringNextBtn) {
            pThis->m_isHoveringNextBtn = inNext;
            InvalidateRect(hWnd, &pThis->m_rcNextBtn, FALSE);
        }

        bool inside = PtInRect(&pThis->m_rcMgrBtn, pt);
        if (inside != pThis->m_isHoveringMgrBtn) {
            pThis->m_isHoveringMgrBtn = inside;
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
        }
        bool insideMore = PtInRect(&pThis->m_rcMoreBtn, pt);
        if (insideMore != pThis->m_isHoveringMoreBtn) {
            pThis->m_isHoveringMoreBtn = insideMore;
            InvalidateRect(hWnd, &pThis->m_rcMoreBtn, FALSE);
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
        if (pThis->m_isHoveringPrevBtn || pThis->m_isPressedPrevBtn) {
            pThis->m_isHoveringPrevBtn = false;
            pThis->m_isPressedPrevBtn = false;
            InvalidateRect(hWnd, &pThis->m_rcPrevBtn, FALSE);
        }
        if (pThis->m_isHoveringNextBtn || pThis->m_isPressedNextBtn) {
            pThis->m_isHoveringNextBtn = false;
            pThis->m_isPressedNextBtn = false;
            InvalidateRect(hWnd, &pThis->m_rcNextBtn, FALSE);
        }
        if (pThis->m_isHoveringMgrBtn || pThis->m_isPressedMgrBtn) {
            pThis->m_isHoveringMgrBtn = false;
            pThis->m_isPressedMgrBtn = false;
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
        }
        if (pThis->m_isHoveringMoreBtn || pThis->m_isPressedMoreBtn) {
            pThis->m_isHoveringMoreBtn = false;
            pThis->m_isPressedMoreBtn = false;
            InvalidateRect(hWnd, &pThis->m_rcMoreBtn, FALSE);
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
                pThis->m_pressedVersion = pThis->m_snapshot.candidate_version;
                pThis->m_pressedContextId = pThis->m_snapshot.context_id;
                pThis->m_pressedContextEpoch = pThis->m_snapshot.context_epoch;
                pThis->m_pressedInputGeneration = pThis->m_snapshot.input_generation;
                pThis->m_pressedPageIndex = pThis->m_snapshot.page_index;
                pThis->m_pressedMode = pThis->m_snapshot.mode;
                pThis->m_pressedCandidateText = (i < pThis->m_candidates.size()) ? pThis->m_candidates[i] : L"";
                pThis->m_selectedIndex = static_cast<int>(i);
                pThis->UpdatePreviewTextFromSelection();
                SetCapture(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
        }
        if (PtInRect(&pThis->m_rcPrevBtn, pt) && pThis->IsPrevButtonEnabled()) {
            pThis->m_isPressedPrevBtn = true;
            pThis->m_pressedArrowSnapshot = pThis->m_snapshot;
            SetCapture(hWnd);
            InvalidateRect(hWnd, &pThis->m_rcPrevBtn, FALSE);
            return 0;
        }
        if (PtInRect(&pThis->m_rcNextBtn, pt) && pThis->IsNextButtonEnabled()) {
            pThis->m_isPressedNextBtn = true;
            pThis->m_pressedArrowSnapshot = pThis->m_snapshot;
            SetCapture(hWnd);
            InvalidateRect(hWnd, &pThis->m_rcNextBtn, FALSE);
            return 0;
        }
        if (PtInRect(&pThis->m_rcMgrBtn, pt)) {
            pThis->m_isPressedMgrBtn = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
            return 0;
        }
        if (PtInRect(&pThis->m_rcMoreBtn, pt)) {
            pThis->m_isPressedMoreBtn = true;
            SetCapture(hWnd);
            InvalidateRect(hWnd, &pThis->m_rcMoreBtn, FALSE);
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
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        if (pThis->m_pressedIndex >= 0) {
            int clickedIdx = pThis->m_pressedIndex;
            uint32_t pressedVer = pThis->m_pressedVersion;
            uint64_t pressedCtx = pThis->m_pressedContextId;
            uint64_t pressedEpoch = pThis->m_pressedContextEpoch;
            uint64_t pressedGen = pThis->m_pressedInputGeneration;
            uint32_t pressedPage = pThis->m_pressedPageIndex;
            CandidateWindowMode pressedMode = pThis->m_pressedMode;
            std::wstring pressedText = pThis->m_pressedCandidateText;

            pThis->m_pressedIndex = -1;
            pThis->m_pressedVersion = 0;
            pThis->m_pressedContextId = 0;
            pThis->m_pressedContextEpoch = 0;
            pThis->m_pressedInputGeneration = 0;
            pThis->m_pressedPageIndex = 0;
            pThis->m_pressedMode = CandidateWindowMode::StandardInline;
            pThis->m_pressedCandidateText.clear();

            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }

            bool hitSameCandidate =
                clickedIdx < static_cast<int>(pThis->m_candidateRects.size()) &&
                clickedIdx < static_cast<int>(pThis->m_candidates.size()) &&
                PtInRect(&pThis->m_candidateRects[clickedIdx], pt) &&
                pThis->m_snapshot.candidate_version == pressedVer &&
                pThis->m_snapshot.context_id == pressedCtx &&
                pThis->m_snapshot.context_epoch == pressedEpoch &&
                pThis->m_snapshot.input_generation == pressedGen &&
                pThis->m_snapshot.page_index == pressedPage &&
                pThis->m_snapshot.mode == pressedMode &&
                pThis->m_candidates[clickedIdx] == pressedText;

            if (hitSameCandidate) {
                CandidateDisplaySnapshot snapCopy = pThis->m_snapshot;
                if (pThis->m_onSelectCandidateSnapshot) {
                    pThis->m_onSelectCandidateSnapshot(snapCopy, clickedIdx);
                }
                if (pThis->m_onSelectCandidate) {
                    pThis->m_onSelectCandidate(clickedIdx);
                }
                return 0;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (pThis->m_isPressedPrevBtn) {
            bool wasPressed = pThis->m_isPressedPrevBtn;
            CandidateDisplaySnapshot snapAtDown = pThis->m_pressedArrowSnapshot;
            pThis->m_isPressedPrevBtn = false;
            pThis->m_pressedArrowSnapshot = {};

            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }
            InvalidateRect(hWnd, &pThis->m_rcPrevBtn, FALSE);

            if (wasPressed && PtInRect(&pThis->m_rcPrevBtn, pt) &&
                pThis->m_snapshot.session_id == snapAtDown.session_id &&
                pThis->m_snapshot.context_id == snapAtDown.context_id &&
                pThis->m_snapshot.context_epoch == snapAtDown.context_epoch &&
                pThis->m_snapshot.input_generation == snapAtDown.input_generation &&
                pThis->m_snapshot.candidate_version == snapAtDown.candidate_version &&
                pThis->m_snapshot.page_index == snapAtDown.page_index &&
                pThis->m_snapshot.mode == snapAtDown.mode &&
                pThis->IsPrevButtonEnabled()) {
                if (pThis->m_onPageScrollSnapshot) {
                    pThis->m_onPageScrollSnapshot(snapAtDown, false);
                } else if (pThis->m_onPageScroll) {
                    pThis->m_onPageScroll(false);
                }
                return 0;
            }
        }
        if (pThis->m_isPressedNextBtn) {
            bool wasPressed = pThis->m_isPressedNextBtn;
            CandidateDisplaySnapshot snapAtDown = pThis->m_pressedArrowSnapshot;
            pThis->m_isPressedNextBtn = false;
            pThis->m_pressedArrowSnapshot = {};

            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }
            InvalidateRect(hWnd, &pThis->m_rcNextBtn, FALSE);

            if (wasPressed && PtInRect(&pThis->m_rcNextBtn, pt) &&
                pThis->m_snapshot.session_id == snapAtDown.session_id &&
                pThis->m_snapshot.context_id == snapAtDown.context_id &&
                pThis->m_snapshot.context_epoch == snapAtDown.context_epoch &&
                pThis->m_snapshot.input_generation == snapAtDown.input_generation &&
                pThis->m_snapshot.candidate_version == snapAtDown.candidate_version &&
                pThis->m_snapshot.page_index == snapAtDown.page_index &&
                pThis->m_snapshot.mode == snapAtDown.mode &&
                pThis->IsNextButtonEnabled()) {
                if (pThis->m_onPageScrollSnapshot) {
                    pThis->m_onPageScrollSnapshot(snapAtDown, true);
                } else if (pThis->m_onPageScroll) {
                    pThis->m_onPageScroll(true);
                }
                return 0;
            }
        }
        if (pThis->m_isPressedMgrBtn) {
            pThis->m_isPressedMgrBtn = false;
            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }
            if (PtInRect(&pThis->m_rcMgrBtn, pt)) {
                pThis->TriggerManagerButton();
                return 0;
            }
            InvalidateRect(hWnd, &pThis->m_rcMgrBtn, FALSE);
        }
        if (pThis->m_isPressedMoreBtn) {
            pThis->m_isPressedMoreBtn = false;
            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }
            InvalidateRect(hWnd, &pThis->m_rcMoreBtn, FALSE);
            if (PtInRect(&pThis->m_rcMoreBtn, pt)) {
                pThis->ToggleMoreMenu();
                return 0;
            }
        }
        for (auto& p : pThis->m_plugins) {
            if (p.isPressed) {
                p.isPressed = false;
                if (GetCapture() == hWnd) {
                    ReleaseCapture();
                }
                if (PtInRect(&p.rcBtn, pt)) {
                    pThis->TriggerPluginButton(p.id);
                    return 0;
                }
                InvalidateRect(hWnd, &p.rcBtn, FALSE);
            }
        }
        if (GetCapture() == hWnd) {
            ReleaseCapture();
        }
        return 0;
    }

    if (uMsg == WM_CAPTURECHANGED && pThis) {
        if (reinterpret_cast<HWND>(lParam) != hWnd) {
            pThis->ResetAllPressedStates();
            InvalidateRect(hWnd, nullptr, FALSE);
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

        const CandidateThemePalette& pal = pThis->m_palette;

        // Background
        HBRUSH hBg = CreateSolidBrush(pal.bgColor);
        FillRect(memDC, &clientRc, hBg);
        DeleteObject(hBg);

        // Subtle rounded border when using GDI fallback or light hairline
        if (!pThis->m_usingNativeRounding) {
            int rad = pThis->ScaleDip(8);
            HPEN hBorderPen = CreatePen(PS_SOLID, 1, pal.borderColor);
            HPEN hOldPen = (HPEN)SelectObject(memDC, hBorderPen);
            SelectObject(memDC, GetStockObject(NULL_BRUSH));
            RoundRect(memDC, clientRc.left, clientRc.top, clientRc.right, clientRc.bottom, rad, rad);
            SelectObject(memDC, hOldPen);
            DeleteObject(hBorderPen);
        } else {
            HPEN hBorderPen = CreatePen(PS_SOLID, 1, pal.borderColor);
            HPEN hOldPen = (HPEN)SelectObject(memDC, hBorderPen);
            SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, clientRc.left, clientRc.top, clientRc.right, clientRc.bottom);
            SelectObject(memDC, hOldPen);
            DeleteObject(hBorderPen);
        }

        SetBkMode(memDC, TRANSPARENT);

        // --- Row 1: Preedit Header (if visible) ---
        if (pThis->m_isHeaderVisible) {
            std::wstring codePart = !pThis->m_snapshot.query_code.empty()
                                    ? pThis->m_snapshot.query_code
                                    : pThis->m_snapshot.raw_input;
            HFONT hHeaderFont = pThis->m_hSubFont ? pThis->m_hSubFont : pThis->m_hFont;
            SelectObject(memDC, hHeaderFont);

            RECT rcCodeCalc = { 0, 0, 0, 0 };
            DrawTextW(memDC, codePart.c_str(), static_cast<int>(codePart.length()), &rcCodeCalc, DT_CALCRECT | DT_SINGLELINE);
            int headerTextH = rcCodeCalc.bottom - rcCodeCalc.top;
            int headerTextY = (pThis->m_headerHeight - headerTextH) / 2;
            int padX = pThis->ScaleDip(8);

            RECT rcCode = { padX, headerTextY, clientRc.right - padX, pThis->m_headerHeight };
            SetTextColor(memDC, pal.headerTextColor);
            DrawTextW(memDC, codePart.c_str(), static_cast<int>(codePart.length()), &rcCode, DT_SINGLELINE | DT_END_ELLIPSIS);

            if (!pThis->m_snapshot.preview_text.empty()) {
                std::wstring previewPart = L" [" + pThis->m_snapshot.preview_text + L"]";
                int previewX = padX + (rcCodeCalc.right - rcCodeCalc.left);
                if (previewX < clientRc.right - padX) {
                    RECT rcPreview = { previewX, headerTextY, clientRc.right - padX, pThis->m_headerHeight };
                    SetTextColor(memDC, pal.headerMatchColor);
                    DrawTextW(memDC, previewPart.c_str(), static_cast<int>(previewPart.length()), &rcPreview, DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }

            if (!pThis->m_candidates.empty()) {
                HPEN hDivPen = CreatePen(PS_SOLID, 1, pal.dividerColor);
                HPEN hOldDivPen = (HPEN)SelectObject(memDC, hDivPen);
                MoveToEx(memDC, clientRc.left + pThis->ScaleDip(6), pThis->m_headerHeight - 1, nullptr);
                LineTo(memDC, clientRc.right - pThis->ScaleDip(6), pThis->m_headerHeight - 1);
                SelectObject(memDC, hOldDivPen);
                DeleteObject(hDivPen);
            }
        }

        int rowTop = pThis->m_headerHeight;
        int centerY = rowTop + (clientRc.bottom - rowTop) / 2;
        int pillRadius = pThis->ScaleDip(6);

        for (size_t i = 0; i < pThis->m_candidates.size(); ++i) {
            if (i >= pThis->m_candidateRects.size() || IsRectEmpty(&pThis->m_candidateRects[i])) {
                continue;
            }
            const auto& cand = pThis->m_candidates[i];
            bool isSelected = (static_cast<int>(i) == pThis->m_selectedIndex);
            bool isHovered = (static_cast<int>(i) == pThis->m_hoveredIndex);

            HFONT hItemFont = (isSelected && pThis->m_hBoldFont) ? pThis->m_hBoldFont : pThis->m_hFont;
            SelectObject(memDC, hItemFont);
            RECT rcCalc = { 0, 0, 0, 0 };
            DrawTextW(memDC, cand.c_str(), static_cast<int>(cand.length()), &rcCalc, DT_CALCRECT | DT_SINGLELINE);
            int candH = rcCalc.bottom - rcCalc.top;
            int textY = centerY - (candH / 2);

            RECT rcPill = pThis->m_candidateRects[i];
            int curX = rcPill.left + pThis->ScaleDip(6);

            if (isSelected) {
                HBRUSH hPillBrush = CreateSolidBrush(pal.selectedPillBg);
                HPEN hPillPen = CreatePen(PS_SOLID, 1, pal.selectedPillBg);
                HPEN hOldPillPen = (HPEN)SelectObject(memDC, hPillPen);
                HBRUSH hOldPillBrush = (HBRUSH)SelectObject(memDC, hPillBrush);
                RoundRect(memDC, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom, pillRadius, pillRadius);
                SelectObject(memDC, hOldPillPen);
                SelectObject(memDC, hOldPillBrush);
                DeleteObject(hPillPen);
                DeleteObject(hPillBrush);

                // Warm orange vertical accent bar on the left of the selected pill
                int barW = (std::max)(2, pThis->ScaleDip(3));
                int barPadV = pThis->ScaleDip(4);
                RECT rcAccent = { rcPill.left + 1, rcPill.top + barPadV, rcPill.left + 1 + barW, rcPill.bottom - barPadV };
                HBRUSH hAccent = CreateSolidBrush(pal.accentBarColor);
                FillRect(memDC, &rcAccent, hAccent);
                DeleteObject(hAccent);
            } else if (isHovered) {
                HBRUSH hHoverBrush = CreateSolidBrush(pal.hoverPillBg);
                HPEN hHoverPen = CreatePen(PS_SOLID, 1, pal.hoverPillBg);
                HPEN hOldHoverPen = (HPEN)SelectObject(memDC, hHoverPen);
                HBRUSH hOldHoverBrush = (HBRUSH)SelectObject(memDC, hHoverBrush);
                RoundRect(memDC, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom, pillRadius, pillRadius);
                SelectObject(memDC, hOldHoverPen);
                SelectObject(memDC, hOldHoverBrush);
                DeleteObject(hHoverPen);
                DeleteObject(hHoverBrush);
            }

            int textRight = rcPill.right - pThis->ScaleDip(2);
            if (curX < textRight) {
                size_t bracketPos = cand.find(L'[');
                if (bracketPos != std::wstring::npos) {
                    std::wstring mainPart = cand.substr(0, bracketPos);
                    std::wstring hintPart = cand.substr(bracketPos);

                    RECT rcMain = { curX, textY, textRight, clientRc.bottom };
                    SetTextColor(memDC, pal.candidateTextColor);
                    DrawTextW(memDC, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMain, DT_SINGLELINE | DT_END_ELLIPSIS);

                    RECT rcMainCalc = { 0, 0, 0, 0 };
                    DrawTextW(memDC, mainPart.c_str(), static_cast<int>(mainPart.length()), &rcMainCalc, DT_CALCRECT | DT_SINGLELINE);

                    int hintX = curX + (rcMainCalc.right - rcMainCalc.left) + pThis->ScaleDip(2);
                    if (hintX < textRight) {
                        SelectObject(memDC, pThis->m_hSubFont ? pThis->m_hSubFont : pThis->m_hFont);
                        RECT rcHint = { hintX, textY + pThis->ScaleDip(2), textRight, clientRc.bottom };
                        SetTextColor(memDC, pal.reverseLookupColor);
                        DrawTextW(memDC, hintPart.c_str(), static_cast<int>(hintPart.length()), &rcHint, DT_SINGLELINE | DT_END_ELLIPSIS);
                        SelectObject(memDC, hItemFont);
                    }
                } else {
                    SetTextColor(memDC, pal.candidateTextColor);
                    RECT rcText = { curX, textY, textRight, clientRc.bottom };
                    DrawTextW(memDC, cand.c_str(), static_cast<int>(cand.length()), &rcText, DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
        }

        if (!pThis->m_candidates.empty()) {
            const int pageBtnW = pThis->ScaleDip(20);
            const int mgrBtnW = pThis->ScaleDip(28);
            const int moreBtnW = pThis->ScaleDip(24);
            int pagingTotalWidth = pageBtnW * 2;
            int fallbackMgrW = pThis->m_showFallbackMgrBtn ? mgrBtnW : 0;
            int controlsStartX = clientRc.right - (pagingTotalWidth + fallbackMgrW + moreBtnW);

            pThis->m_rcPrevBtn = { controlsStartX, rowTop + 1, controlsStartX + pageBtnW, clientRc.bottom - 1 };
            pThis->m_rcNextBtn = { controlsStartX + pageBtnW, rowTop + 1, controlsStartX + pagingTotalWidth, clientRc.bottom - 1 };
            int btnStartX = controlsStartX + pagingTotalWidth;

            if (pThis->m_showFallbackMgrBtn) {
                pThis->m_rcMgrBtn = { btnStartX, rowTop + 1, btnStartX + mgrBtnW, clientRc.bottom - 1 };
                btnStartX += mgrBtnW;
            } else {
                pThis->m_rcMgrBtn = { 0, 0, 0, 0 };
            }
            pThis->m_rcMoreBtn = { btnStartX, rowTop + 1, btnStartX + moreBtnW, clientRc.bottom - 1 };

            HPEN hSepPen = CreatePen(PS_SOLID, 1, pal.dividerColor);
            HPEN hOldPen2 = (HPEN)SelectObject(memDC, hSepPen);
            MoveToEx(memDC, pThis->m_rcPrevBtn.left, rowTop + pThis->ScaleDip(7), nullptr);
            LineTo(memDC, pThis->m_rcPrevBtn.left, clientRc.bottom - pThis->ScaleDip(7));

            auto drawBtnBg = [&](const RECT& rc, bool pressed, bool hovered, int rad) {
                if (!pressed && !hovered) return;
                COLORREF bg = pressed ? pal.pressedPillBg : pal.hoverPillBg;
                HBRUSH hB = CreateSolidBrush(bg);
                HPEN hP = CreatePen(PS_SOLID, 1, bg);
                HPEN hOP = (HPEN)SelectObject(memDC, hP);
                HBRUSH hOB = (HBRUSH)SelectObject(memDC, hB);
                RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, rad, rad);
                SelectObject(memDC, hOP);
                SelectObject(memDC, hOB);
                DeleteObject(hP);
                DeleteObject(hB);
            };

            bool prevEnabled = pThis->IsPrevButtonEnabled();
            RECT prevInner = { pThis->m_rcPrevBtn.left + 2, rowTop + 4, pThis->m_rcPrevBtn.right - 2, clientRc.bottom - 4 };
            if (prevEnabled) {
                drawBtnBg(prevInner, pThis->m_isPressedPrevBtn, pThis->m_isHoveringPrevBtn, pThis->ScaleDip(4));
            }
            SelectObject(memDC, pThis->m_hSubFont ? pThis->m_hSubFont : pThis->m_hFont);
            COLORREF prevColor = !prevEnabled ? pal.arrowBtnDisabled
                                 : (pThis->m_isHoveringPrevBtn ? pal.btnHoverColor : pal.arrowBtnEnabled);
            SetTextColor(memDC, prevColor);
            const wchar_t* prevIcon = L"\x25C0";
            DrawTextW(memDC, prevIcon, 1, &prevInner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            bool nextEnabled = pThis->IsNextButtonEnabled();
            RECT nextInner = { pThis->m_rcNextBtn.left + 2, rowTop + 4, pThis->m_rcNextBtn.right - 2, clientRc.bottom - 4 };
            if (nextEnabled) {
                drawBtnBg(nextInner, pThis->m_isPressedNextBtn, pThis->m_isHoveringNextBtn, pThis->ScaleDip(4));
            }
            COLORREF nextColor = !nextEnabled ? pal.arrowBtnDisabled
                                 : (pThis->m_isHoveringNextBtn ? pal.btnHoverColor : pal.arrowBtnEnabled);
            SetTextColor(memDC, nextColor);
            const wchar_t* nextIcon = L"\x25B6";
            DrawTextW(memDC, nextIcon, 1, &nextInner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            if (pThis->m_showFallbackMgrBtn) {
                MoveToEx(memDC, pThis->m_rcMgrBtn.left, rowTop + pThis->ScaleDip(7), nullptr);
                LineTo(memDC, pThis->m_rcMgrBtn.left, clientRc.bottom - pThis->ScaleDip(7));

                RECT btnInner = { pThis->m_rcMgrBtn.left + 2, rowTop + 4, pThis->m_rcMgrBtn.right - 2, clientRc.bottom - 4 };
                drawBtnBg(btnInner, pThis->m_isPressedMgrBtn, pThis->m_isHoveringMgrBtn, pillRadius);

                SelectObject(memDC, pThis->m_hFont);
                SetTextColor(memDC, pThis->m_isHoveringMgrBtn ? pal.btnHoverColor : pal.arrowBtnEnabled);
                const wchar_t* btnIcon = L"\x2699";
                DrawTextW(memDC, btnIcon, 1, &btnInner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            MoveToEx(memDC, pThis->m_rcMoreBtn.left, rowTop + pThis->ScaleDip(7), nullptr);
            LineTo(memDC, pThis->m_rcMoreBtn.left, clientRc.bottom - pThis->ScaleDip(7));

            RECT moreInner = { pThis->m_rcMoreBtn.left + 2, rowTop + 4, pThis->m_rcMoreBtn.right - 2, clientRc.bottom - 4 };
            drawBtnBg(moreInner, pThis->m_isPressedMoreBtn || pThis->m_isMenuOpen, pThis->m_isHoveringMoreBtn, pillRadius);

            SelectObject(memDC, pThis->m_hFont);
            SetTextColor(memDC, (pThis->m_isHoveringMoreBtn || pThis->m_isMenuOpen) ? pal.btnHoverColor : pal.arrowBtnEnabled);
            const wchar_t* moreIcon = L"\x22EE";
            DrawTextW(memDC, moreIcon, 1, &moreInner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, hOldPen2);
            DeleteObject(hSepPen);
        }

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

bool CCandidateWindow::EnsureMenuWindow() {
    if (m_hMenuWnd && IsWindow(m_hMenuWnd)) {
        return true;
    }
    if (!EnsureWindow()) {
        return false;
    }
    if (!m_hasRegisteredMenuClassRef) {
        if (InterlockedIncrement(&s_cMenuClassRef) == 1) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = MenuWndProc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = MORE_MENU_WND_CLASS;
            if (!RegisterClassExW(&wc)) {
                DWORD err = GetLastError();
                if (err != ERROR_CLASS_ALREADY_EXISTS) {
                    InterlockedDecrement(&s_cMenuClassRef);
                    return false;
                }
            }
        }
        m_hasRegisteredMenuClassRef = true;
    }

    m_hMenuWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        MORE_MENU_WND_CLASS,
        L"",
        WS_POPUP,
        0, 0, 120, 40,
        m_hWnd,
        nullptr,
        g_hInst,
        this
    );

    if (!m_hMenuWnd) {
        if (m_hasRegisteredMenuClassRef) {
            m_hasRegisteredMenuClassRef = false;
            if (InterlockedDecrement(&s_cMenuClassRef) == 0) {
                UnregisterClassW(MORE_MENU_WND_CLASS, g_hInst);
            }
        }
        return false;
    }
    RefreshMenuTheme();
    return true;
}

void CCandidateWindow::OpenMoreMenu() {
    if (m_isMenuOpen) return;
    if (!m_hWnd || !IsWindow(m_hWnd) || !IsWindowVisible(m_hWnd)) {
        return;
    }
    if (!EnsureMenuWindow()) {
        return;
    }

    RECT candWndRc{};
    GetWindowRect(m_hWnd, &candWndRc);

    POINT moreRightBottom = { m_rcMoreBtn.right, m_rcMoreBtn.bottom };
    ClientToScreen(m_hWnd, &moreRightBottom);

    HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    int workWidth = static_cast<int>(mi.rcWork.right - mi.rcWork.left);
    int maxMenuWidth = (std::max)(ScaleDip(120), (std::min)(workWidth - ScaleDip(8), ScaleDip(260)));

    HDC hdc = GetDC(m_hMenuWnd);
    HFONT hMenuFont = m_hFont;
    HFONT hOldFont = (HFONT)SelectObject(hdc, hMenuFont);

    auto measureItem = [&](const std::wstring& text) -> int {
        RECT rc = { 0, 0, 0, 0 };
        DrawTextW(hdc, text.c_str(), static_cast<int>(text.length()), &rc, DT_CALCRECT | DT_SINGLELINE);
        return static_cast<int>(rc.right - rc.left);
    };

    const std::wstring mgrLabel = L"\x2699  设置与词库管理";
    int maxTextW = measureItem(mgrLabel);
    for (const auto& p : m_plugins) {
        std::wstring label = p.icon + L"  " + p.name;
        maxTextW = (std::max)(maxTextW, measureItem(label));
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(m_hMenuWnd, hdc);

    int padX = ScaleDip(14);
    int padY = ScaleDip(6);
    int itemH = ScaleDip(28);
    int menuW = (std::min)(maxMenuWidth, (std::max)(ScaleDip(156), maxTextW + padX * 2));
    int itemCount = 1 + static_cast<int>(m_plugins.size());

    int workHeight = static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
    int maxMenuHeight = (std::max)(padY * 2 + itemH, workHeight - ScaleDip(16));
    int naturalMenuH = padY * 2 + itemCount * itemH;
    int menuH = (std::min)(naturalMenuH, maxMenuHeight);
    int visibleCount = (std::max)(1, (menuH - padY * 2) / itemH);
    if (visibleCount > itemCount) visibleCount = itemCount;
    menuH = padY * 2 + visibleCount * itemH;

    m_menuWidth = menuW;
    m_menuHeight = menuH;
    m_menuItemHeight = itemH;
    m_menuPadY = padY;
    m_menuVisibleCount = visibleCount;
    m_menuMaxScroll = (std::max)(0, itemCount - visibleCount);
    m_menuScrollOffset = 0;
    UpdateMenuItemRects();

    int x = moreRightBottom.x - menuW;
    int y = candWndRc.bottom + ScaleDip(4);
    if (y + menuH > mi.rcWork.bottom) {
        y = candWndRc.top - menuH - ScaleDip(4);
    }
    if (x + menuW > mi.rcWork.right) x = mi.rcWork.right - menuW;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (y + menuH > mi.rcWork.bottom) y = (std::max)(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - menuH);

    if (++m_menuGeneration == 0) ++m_menuGeneration;
    DWORD hookErr = 0;
    if (!RegisterMenuObserver(m_menuGeneration, m_hWnd, m_hMenuWnd, &hookErr)) {
        if (++m_menuGeneration == 0) ++m_menuGeneration;
        m_isMenuOpen = false;
        m_lastHookInstallError = hookErr;
        m_showFallbackMgrBtn = true;
        RelayoutCurrent();
        return;
    }

    m_lastHookInstallError = 0;
    m_isMenuOpen = true;
    m_menuHoveredIdx = -1;
    m_menuPressedIdx = -1;
    m_menuPressedGen = 0;
    m_menuTrackingMouse = false;

    ApplyWindowRoundingTo(m_hMenuWnd, menuW, menuH, nullptr);
    RefreshMenuTheme();

    SetWindowPos(
        m_hMenuWnd,
        HWND_TOPMOST,
        x, y, menuW, menuH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
    InvalidateRect(m_hMenuWnd, nullptr, TRUE);
    if (m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, &m_rcMoreBtn, FALSE);
    }
}

void CCandidateWindow::UpdateMenuItemRects() {
    int rightMargin = (m_menuMaxScroll > 0) ? ScaleDip(10) : ScaleDip(4);
    int leftMargin = ScaleDip(4);
    auto rectForIndex = [&](int itemIdx) -> RECT {
        if (itemIdx < m_menuScrollOffset || itemIdx >= m_menuScrollOffset + m_menuVisibleCount) {
            return { 0, 0, 0, 0 };
        }
        int row = itemIdx - m_menuScrollOffset;
        int top = m_menuPadY + row * m_menuItemHeight;
        return { leftMargin, top, m_menuWidth - rightMargin, top + m_menuItemHeight };
    };
    m_rcMenuMgrItem = rectForIndex(0);
    m_menuPluginRects.resize(m_plugins.size());
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        m_menuPluginRects[i] = rectForIndex(static_cast<int>(i) + 1);
    }
}

void CCandidateWindow::ScrollMoreMenu(int deltaItems) {
    if (!m_isMenuOpen || m_menuMaxScroll <= 0) return;
    int nextOffset = (std::max)(0, (std::min)(m_menuMaxScroll, m_menuScrollOffset + deltaItems));
    if (nextOffset != m_menuScrollOffset) {
        m_menuScrollOffset = nextOffset;
        m_menuHoveredIdx = -1;
        UpdateMenuItemRects();
        if (m_hMenuWnd && IsWindow(m_hMenuWnd)) {
            InvalidateRect(m_hMenuWnd, nullptr, TRUE);
        }
    }
}

void CCandidateWindow::CloseMoreMenu() {
    if (++m_menuGeneration == 0) ++m_menuGeneration;
    if (m_hWnd) {
        UnregisterMenuObserverForWindow(m_hWnd);
    }
    bool wasOpen = m_isMenuOpen;
    m_isMenuOpen = false;
    m_menuHoveredIdx = -1;
    m_menuPressedIdx = -1;
    m_menuPressedGen = 0;
    m_menuTrackingMouse = false;
    if (m_hMenuWnd && IsWindow(m_hMenuWnd)) {
        if (GetCapture() == m_hMenuWnd) {
            ReleaseCapture();
        }
        ShowWindow(m_hMenuWnd, SW_HIDE);
    }
    if (wasOpen && m_hWnd && IsWindow(m_hWnd)) {
        InvalidateRect(m_hWnd, &m_rcMoreBtn, FALSE);
    }
}

void CCandidateWindow::ToggleMoreMenu() {
    if (m_isMenuOpen) {
        CloseMoreMenu();
    } else {
        OpenMoreMenu();
    }
}

void CCandidateWindow::HandleExternalClick(POINT screenPt, uint32_t generation) {
    if (!m_isMenuOpen || generation == 0 || generation != m_menuGeneration) {
        return;
    }
    if (!m_hWnd || !IsWindow(m_hWnd) || !m_hMenuWnd || !IsWindow(m_hMenuWnd) || !IsWindowVisible(m_hMenuWnd)) {
        return;
    }
    if (!IsMenuObserverEntryAlive(generation, m_hWnd, m_hMenuWnd)) {
        return;
    }

    RECT menuScreenRc{};
    GetWindowRect(m_hMenuWnd, &menuScreenRc);
    if (PtInRect(&menuScreenRc, screenPt)) {
        return;
    }

    RECT moreBtnScreenRc = m_rcMoreBtn;
    MapWindowPoints(m_hWnd, nullptr, reinterpret_cast<LPPOINT>(&moreBtnScreenRc), 2);
    if (PtInRect(&moreBtnScreenRc, screenPt)) {
        return;
    }

    CloseMoreMenu();
}

void CCandidateWindow::SimulateExternalClickForTest(POINT screenPt, uint32_t generationOverride) {
    if (!m_hWnd || !IsWindow(m_hWnd)) return;
    uint32_t gen = (generationOverride != 0) ? generationOverride : m_menuGeneration;
    PostMessageW(
        m_hWnd,
        WM_APP_EXTERNAL_CLICK,
        static_cast<WPARAM>(gen),
        MAKELPARAM(static_cast<short>(screenPt.x), static_cast<short>(screenPt.y)));
}

LRESULT CALLBACK CCandidateWindow::MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CCandidateWindow* pThis = nullptr;
    if (uMsg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = static_cast<CCandidateWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<CCandidateWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (uMsg == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }

    if (uMsg == WM_MOUSEWHEEL && pThis) {
        short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (zDelta != 0) {
            pThis->ScrollMoreMenu(zDelta < 0 ? 1 : -1);
        }
        return 0;
    }

    if (uMsg == WM_SETCURSOR && pThis) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        if (PtInRect(&pThis->m_rcMenuMgrItem, pt)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        for (const auto& rcP : pThis->m_menuPluginRects) {
            if (PtInRect(&rcP, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
    }

    if (uMsg == WM_MOUSEMOVE && pThis) {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int newHovered = -1;
        if (PtInRect(&pThis->m_rcMenuMgrItem, pt)) {
            newHovered = 0;
        } else {
            for (size_t i = 0; i < pThis->m_menuPluginRects.size(); ++i) {
                if (PtInRect(&pThis->m_menuPluginRects[i], pt)) {
                    newHovered = static_cast<int>(i) + 1;
                    break;
                }
            }
        }
        if (newHovered != pThis->m_menuHoveredIdx) {
            pThis->m_menuHoveredIdx = newHovered;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (!pThis->m_menuTrackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            pThis->m_menuTrackingMouse = true;
        }
        return 0;
    }

    if (uMsg == WM_MOUSELEAVE && pThis) {
        pThis->m_menuTrackingMouse = false;
        if (pThis->m_menuHoveredIdx != -1) {
            pThis->m_menuHoveredIdx = -1;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    if (uMsg == WM_LBUTTONDOWN && pThis) {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int hit = -1;
        if (PtInRect(&pThis->m_rcMenuMgrItem, pt)) {
            hit = 0;
        } else {
            for (size_t i = 0; i < pThis->m_menuPluginRects.size(); ++i) {
                if (PtInRect(&pThis->m_menuPluginRects[i], pt)) {
                    hit = static_cast<int>(i) + 1;
                    break;
                }
            }
        }
        if (hit >= 0) {
            pThis->m_menuPressedIdx = hit;
            pThis->m_menuPressedGen = pThis->m_menuGeneration;
            SetCapture(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
    }

    if (uMsg == WM_LBUTTONUP && pThis) {
        if (pThis->m_menuPressedIdx >= 0) {
            int pressed = pThis->m_menuPressedIdx;
            uint32_t pressedGen = pThis->m_menuPressedGen;
            pThis->m_menuPressedIdx = -1;
            pThis->m_menuPressedGen = 0;
            if (GetCapture() == hWnd) {
                ReleaseCapture();
            }

            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            bool sameGen = (pressedGen != 0 && pressedGen == pThis->m_menuGeneration && pThis->m_isMenuOpen);
            if (sameGen) {
                if (pressed == 0 && PtInRect(&pThis->m_rcMenuMgrItem, pt)) {
                    pThis->CloseMoreMenu();
                    pThis->TriggerManagerButton();
                    return 0;
                }
                int pluginIdx = pressed - 1;
                if (pluginIdx >= 0 &&
                    static_cast<size_t>(pluginIdx) < pThis->m_menuPluginRects.size() &&
                    static_cast<size_t>(pluginIdx) < pThis->m_plugins.size() &&
                    PtInRect(&pThis->m_menuPluginRects[pluginIdx], pt)) {
                    uint32_t pid = pThis->m_plugins[pluginIdx].id;
                    pThis->CloseMoreMenu();
                    pThis->TriggerPluginButton(pid);
                    return 0;
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    if (uMsg == WM_CAPTURECHANGED && pThis) {
        if (reinterpret_cast<HWND>(lParam) != hWnd) {
            pThis->m_menuPressedIdx = -1;
            pThis->m_menuPressedGen = 0;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    if (uMsg == WM_PAINT && pThis) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientRc.right, clientRc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        const CandidateThemePalette& pal = pThis->m_palette;
        HBRUSH hBg = CreateSolidBrush(pal.bgColor);
        FillRect(memDC, &clientRc, hBg);
        DeleteObject(hBg);

        HPEN hBorderPen = CreatePen(PS_SOLID, 1, pal.borderColor);
        HPEN hOldPen = (HPEN)SelectObject(memDC, hBorderPen);
        SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, clientRc.left, clientRc.top, clientRc.right, clientRc.bottom);
        SelectObject(memDC, hOldPen);
        DeleteObject(hBorderPen);

        SetBkMode(memDC, TRANSPARENT);
        SelectObject(memDC, pThis->m_hFont);

        int rad = pThis->ScaleDip(6);
        int textPadX = pThis->ScaleDip(10);

        auto drawMenuItem = [&](const RECT& rc, int idx, const std::wstring& label) {
            if (IsRectEmpty(&rc)) return;
            bool isHovered = (pThis->m_menuHoveredIdx == idx);
            bool isPressed = (pThis->m_menuPressedIdx == idx);
            if (isHovered || isPressed) {
                COLORREF bg = isPressed ? pal.pressedPillBg : pal.hoverPillBg;
                HBRUSH hB = CreateSolidBrush(bg);
                HPEN hP = CreatePen(PS_SOLID, 1, bg);
                HPEN hOP = (HPEN)SelectObject(memDC, hP);
                HBRUSH hOB = (HBRUSH)SelectObject(memDC, hB);
                RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, rad, rad);
                SelectObject(memDC, hOP);
                SelectObject(memDC, hOB);
                DeleteObject(hP);
                DeleteObject(hB);
            }
            SetTextColor(memDC, (isHovered || isPressed) ? pal.btnHoverColor : pal.candidateTextColor);
            RECT rcText = { rc.left + textPadX, rc.top, rc.right - textPadX, rc.bottom };
            DrawTextW(memDC, label.c_str(), static_cast<int>(label.length()), &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };

        drawMenuItem(pThis->m_rcMenuMgrItem, 0, L"\x2699  设置与词库管理");
        for (size_t i = 0; i < pThis->m_plugins.size() && i < pThis->m_menuPluginRects.size(); ++i) {
            const auto& p = pThis->m_plugins[i];
            drawMenuItem(pThis->m_menuPluginRects[i], static_cast<int>(i) + 1, p.icon + L"  " + p.name);
        }

        if (pThis->m_menuMaxScroll > 0) {
            int totalItems = 1 + static_cast<int>(pThis->m_plugins.size());
            int trackTop = pThis->m_menuPadY;
            int trackBottom = clientRc.bottom - pThis->m_menuPadY;
            int trackH = (std::max)(1, trackBottom - trackTop);
            int thumbH = (std::max)(pThis->ScaleDip(16), (trackH * pThis->m_menuVisibleCount) / totalItems);
            int maxThumbTravel = (std::max)(0, trackH - thumbH);
            int thumbTop = trackTop + (pThis->m_menuMaxScroll > 0 ? (maxThumbTravel * pThis->m_menuScrollOffset) / pThis->m_menuMaxScroll : 0);
            int barW = (std::max)(2, pThis->ScaleDip(3));
            RECT rcThumb = { clientRc.right - pThis->ScaleDip(5), thumbTop, clientRc.right - pThis->ScaleDip(5) + barW, thumbTop + thumbH };
            HBRUSH hThumb = CreateSolidBrush(pal.dividerColor);
            FillRect(memDC, &rcThumb, hThumb);
            DeleteObject(hThumb);
        }

        BitBlt(hdc, 0, 0, clientRc.right, clientRc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
