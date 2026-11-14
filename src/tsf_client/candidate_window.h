#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

struct ITfContext;

struct CandidatePluginItem {
    uint32_t id = 0;
    std::wstring name;
    std::wstring icon;
    std::wstring exec_path;
    std::wstring arguments;
    RECT rcBtn{0, 0, 0, 0};
    bool isHovered = false;
    bool isPressed = false;
};

enum class CandidateWindowMode {
    StandardInline  = 0, // Host renders inline composition + CandidateWindow shows Preedit Header
    Compositionless = 1  // Host lacks inline composition; CandidateWindow exclusively renders Preedit Header
};

enum class PreeditDisplayMode {
    Always              = 0, // Always show Preedit Header above candidate list
    CompositionlessOnly = 1, // Show Preedit Header only when host is in Compositionless mode
    Disabled            = 2  // Pure candidate list only
};

enum class ThemeMode {
    FollowSystem = 0, // Track AppsUseLightTheme
    Light        = 1, // Force Light palette
    Dark         = 2  // Force Dark palette
};

struct CandidateThemePalette {
    COLORREF bgColor = RGB(255, 255, 255);
    COLORREF borderColor = RGB(226, 232, 240);
    COLORREF dividerColor = RGB(226, 232, 240);
    COLORREF headerTextColor = RGB(100, 116, 139);
    COLORREF headerMatchColor = RGB(14, 116, 144);
    COLORREF candidateTextColor = RGB(30, 41, 59);
    COLORREF reverseLookupColor = RGB(100, 116, 139);
    COLORREF selectedPillBg = RGB(241, 245, 249);
    COLORREF hoverPillBg = RGB(248, 250, 252);
    COLORREF pressedPillBg = RGB(226, 232, 240);
    COLORREF accentBarColor = RGB(249, 115, 22);
    COLORREF arrowBtnEnabled = RGB(71, 85, 105);
    COLORREF arrowBtnDisabled = RGB(203, 213, 225);
    COLORREF btnHoverColor = RGB(14, 116, 144);
};

struct CandidateDisplaySnapshot {
    uint64_t session_id = 0;
    uint64_t context_id = 0;
    ITfContext* pContext = nullptr;      // Non-owning borrowed pointer; issuer holds COM ref until invalidated
    uint64_t input_generation = 0;      // Bound to context input generation
    uint64_t context_epoch = 0;         // Bound to context lifecycle epoch (prevents pointer reuse)
    uint32_t candidate_version = 0;
    uint32_t page_index = 0;
    uint32_t page_count = 0;
    std::vector<std::wstring> candidates;
    int initial_selected_index = 0;

    // Stage 7 extensions
    CandidateWindowMode mode = CandidateWindowMode::StandardInline;
    std::wstring raw_input;             // Raw keystrokes (e.g. "dajia")
    std::wstring query_code;            // Normalized/segmented code (e.g. "da'jia")
    std::wstring preview_text;          // Currently highlighted candidate preview text (e.g. "大家")
    std::vector<std::wstring> candidate_core_texts; // Optional explicit core texts without index/hint
};

class CCandidateWindow {
public:
    using OnOpenManagerCallback = std::function<void()>;
    using OnExecutePluginCallback = std::function<void(const CandidatePluginItem& item)>;
    using OnSelectCandidateCallback = std::function<void(int index)>;
    using OnSelectCandidateSnapshotCallback = std::function<void(const CandidateDisplaySnapshot& snapshot, int index)>;
    using OnPageScrollCallback = std::function<void(bool nextPage)>;
    using OnPageScrollSnapshotCallback = std::function<void(const CandidateDisplaySnapshot& snapshot, bool nextPage)>;

    CCandidateWindow();
    ~CCandidateWindow();

    void Show(const std::vector<std::wstring>& candidates, POINT pt);
    void Show(const CandidateDisplaySnapshot& snapshot, POINT pt);
    void Hide();
    void Destroy();

    void SetOnOpenManager(OnOpenManagerCallback cb) { m_onOpenManager = std::move(cb); }
    void SetOnExecutePlugin(OnExecutePluginCallback cb) { m_onExecutePlugin = std::move(cb); }
    void SetOnSelectCandidate(OnSelectCandidateCallback cb) { m_onSelectCandidate = std::move(cb); }
    void SetOnSelectCandidateSnapshot(OnSelectCandidateSnapshotCallback cb) { m_onSelectCandidateSnapshot = std::move(cb); }
    void SetOnPageScroll(OnPageScrollCallback cb) { m_onPageScroll = std::move(cb); }
    void SetOnPageScrollSnapshot(OnPageScrollSnapshotCallback cb) { m_onPageScrollSnapshot = std::move(cb); }
    void TriggerManagerButton();
    void TriggerPluginButton(uint32_t pluginId);

    const CandidateDisplaySnapshot& GetSnapshot() const { return m_snapshot; }
    int GetSelectedIndex() const { return m_selectedIndex; }
    void SetSelectedIndex(int index);
    int GetHoveredIndex() const { return m_hoveredIndex; }
    int GetCandidateCount() const { return static_cast<int>(m_candidates.size()); }
    const std::vector<RECT>& GetCandidateRects() const { return m_candidateRects; }

    // Stage 7.1b: Preedit Header & Pure Candidate Text Extraction
    void SetPreeditDisplayMode(PreeditDisplayMode mode);
    PreeditDisplayMode GetPreeditDisplayMode() const { return m_preeditDisplayMode; }
    bool IsPreeditHeaderVisible() const { return m_isHeaderVisible; }
    RECT GetPreeditHeaderRect() const { return m_rcHeader; }
    std::wstring GetDisplayedPreeditText() const;
    static std::wstring ExtractCandidateCoreText(const std::wstring& formattedCandidate);
    static bool ShouldShowPreeditHeader(const CandidateDisplaySnapshot& snap, PreeditDisplayMode displayMode);
    static bool ShouldShowWindow(const CandidateDisplaySnapshot& snap, PreeditDisplayMode displayMode);
    bool ShouldShowWindow(const CandidateDisplaySnapshot& snap) const {
        return ShouldShowWindow(snap, m_preeditDisplayMode);
    }

    void SetPlugins(const std::vector<CandidatePluginItem>& plugins);
    const std::vector<CandidatePluginItem>& GetPlugins() const { return m_plugins; }

    RECT GetManagerButtonRect() const { return m_rcMgrBtn; }
    bool IsHoveringManagerButton() const { return m_isHoveringMgrBtn; }
    bool IsPressedManagerButton() const { return m_isPressedMgrBtn; }

    // Stage 7.3: Overflow "⋮" drawer & non-activating popup menu
    RECT GetMoreButtonRect() const { return m_rcMoreBtn; }
    bool IsHoveringMoreButton() const { return m_isHoveringMoreBtn; }
    bool IsPressedMoreButton() const { return m_isPressedMoreBtn; }
    bool IsMenuOpen() const { return m_isMenuOpen; }
    void OpenMoreMenu();
    void CloseMoreMenu();
    void ToggleMoreMenu();
    HWND GetMenuHwnd() const { return m_hMenuWnd; }
    uint32_t GetMenuGeneration() const { return m_menuGeneration; }
    RECT GetMenuManagerItemRect() const { return m_rcMenuMgrItem; }
    RECT GetMenuPluginItemRect(size_t index) const {
        return index < m_menuPluginRects.size() ? m_menuPluginRects[index] : RECT{0, 0, 0, 0};
    }
    DWORD GetLastHookInstallError() const { return m_lastHookInstallError; }
    bool IsHookFallbackManagerVisible() const { return m_showFallbackMgrBtn; }
    static bool IsHookObserverActive();
    static void SetHookInstallFailureOverrideForTest(bool fail);
    static void SetHookStartDelayOverrideForTest(DWORD delayMs);
    static void SetHookWaitTimeoutOverrideForTest(DWORD timeoutMs);
    void SimulateExternalClickForTest(POINT screenPt, uint32_t generationOverride = 0);
    int GetMenuScrollOffset() const { return m_menuScrollOffset; }
    int GetMenuMaxScroll() const { return m_menuMaxScroll; }
    bool IsMenuScrollable() const { return m_menuMaxScroll > 0; }

    RECT GetPrevButtonRect() const { return m_rcPrevBtn; }
    RECT GetNextButtonRect() const { return m_rcNextBtn; }
    bool IsPrevButtonEnabled() const { return m_snapshot.page_index > 0; }
    bool IsNextButtonEnabled() const { return m_snapshot.page_count > 1 && (m_snapshot.page_index + 1 < m_snapshot.page_count); }
    bool IsHoveringPrevButton() const { return m_isHoveringPrevBtn; }
    bool IsHoveringNextButton() const { return m_isHoveringNextBtn; }
    bool IsPressedPrevButton() const { return m_isPressedPrevBtn; }
    bool IsPressedNextButton() const { return m_isPressedNextBtn; }

    // Stage 7.2: Theme, DPI, layout & rounding helpers
    void SetThemeMode(ThemeMode mode);
    ThemeMode GetThemeMode() const { return m_themeMode; }
    ThemeMode GetEffectiveTheme() const { return m_effectiveTheme; }
    const CandidateThemePalette& GetPalette() const { return m_palette; }
    void RefreshTheme(bool invalidate = true);
    static void SetSystemAppsUseLightThemeOverrideForTest(int val); // -1 = registry, 0 = dark, 1 = light
    static bool QuerySystemAppsUseLightTheme();
    static CandidateThemePalette BuildPalette(ThemeMode effectiveTheme);
    UINT GetCurrentDpi() const { return m_dpi; }
    int ScaleDip(int dip) const { return MulDiv(dip, static_cast<int>(m_dpi), 96); }
    bool IsUsingNativeRoundedCorners() const { return m_usingNativeRounding; }

    HWND GetHwnd() const { return m_hWnd; }

    static LONG GetClassRefCount();

private:
    bool EnsureWindow();
    void LayoutAndShow(const CandidateDisplaySnapshot& snapshot, POINT anchorPt, UINT overrideDpi, const RECT* pSuggestedRect);
    void RelayoutCurrent();
    void InitDefaultPlugins();
    void RebuildFontsForDpi(UINT dpi);
    UINT QueryDpiForPoint(POINT pt) const;
    void ApplyWindowRounding(int width, int height);
    void ApplyWindowRoundingTo(HWND hWnd, int width, int height, bool* pUsedNative = nullptr);
    bool EnsureMenuWindow();
    void RefreshMenuTheme();
    void UpdateMenuItemRects();
    void ScrollMoreMenu(int deltaItems);
    void HandleExternalClick(POINT screenPt, uint32_t generation);
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hWnd = nullptr;
    UINT m_dpi = 96;
    UINT m_fontDpi = 0;
    HFONT m_hFont = nullptr;
    HFONT m_hBoldFont = nullptr;
    HFONT m_hSubFont = nullptr;
    bool m_hasRegisteredClassRef = false;
    ThemeMode m_themeMode = ThemeMode::FollowSystem;
    ThemeMode m_effectiveTheme = ThemeMode::Light;
    CandidateThemePalette m_palette{};
    bool m_usingNativeRounding = false;
    CandidateDisplaySnapshot m_snapshot;
    POINT m_lastShowPt{0, 0};
    POINT m_lastLayoutPos{0, 0};
    bool m_hasDpiOverride = false;
    std::vector<std::wstring> m_candidates;
    std::vector<RECT> m_candidateRects;
    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_pressedIndex = -1;
    uint32_t m_pressedVersion = 0;
    uint64_t m_pressedContextId = 0;
    uint64_t m_pressedContextEpoch = 0;
    uint64_t m_pressedInputGeneration = 0;
    CandidateWindowMode m_pressedMode = CandidateWindowMode::StandardInline;
    uint32_t m_pressedPageIndex = 0;
    std::wstring m_pressedCandidateText;

    PreeditDisplayMode m_preeditDisplayMode = PreeditDisplayMode::Always;
    bool m_isHeaderVisible = false;
    int m_headerHeight = 0;
    RECT m_rcHeader{0, 0, 0, 0};
    void UpdatePreviewTextFromSelection();

    RECT m_rcMgrBtn{0, 0, 0, 0};
    bool m_isHoveringMgrBtn = false;
    bool m_isPressedMgrBtn = false;

    RECT m_rcMoreBtn{0, 0, 0, 0};
    bool m_isHoveringMoreBtn = false;
    bool m_isPressedMoreBtn = false;
    bool m_showFallbackMgrBtn = false;
    DWORD m_lastHookInstallError = 0;

    HWND m_hMenuWnd = nullptr;
    bool m_hasRegisteredMenuClassRef = false;
    bool m_isMenuOpen = false;
    uint32_t m_menuGeneration = 0;
    int m_menuHoveredIdx = -1; // 0 = manager, 1..N = plugin index + 1
    int m_menuPressedIdx = -1;
    uint32_t m_menuPressedGen = 0;
    bool m_menuTrackingMouse = false;
    RECT m_rcMenuMgrItem{0, 0, 0, 0};
    std::vector<RECT> m_menuPluginRects;
    int m_menuWidth = 0;
    int m_menuHeight = 0;
    int m_menuItemHeight = 0;
    int m_menuPadY = 0;
    int m_menuScrollOffset = 0;
    int m_menuMaxScroll = 0;
    int m_menuVisibleCount = 0;

    RECT m_rcPrevBtn{0, 0, 0, 0};
    RECT m_rcNextBtn{0, 0, 0, 0};
    bool m_isHoveringPrevBtn = false;
    bool m_isHoveringNextBtn = false;
    bool m_isPressedPrevBtn = false;
    bool m_isPressedNextBtn = false;
    CandidateDisplaySnapshot m_pressedArrowSnapshot;

    void ResetAllPressedStates();
    bool m_trackingMouse = false;
    OnOpenManagerCallback m_onOpenManager;
    OnSelectCandidateCallback m_onSelectCandidate;
    OnSelectCandidateSnapshotCallback m_onSelectCandidateSnapshot;
    OnPageScrollCallback m_onPageScroll;
    OnPageScrollSnapshotCallback m_onPageScrollSnapshot;

    std::vector<CandidatePluginItem> m_plugins;
    OnExecutePluginCallback m_onExecutePlugin;
};
