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

struct CandidateDisplaySnapshot {
    uint64_t session_id = 0;
    uint64_t context_id = 0;
    ITfContext* pContext = nullptr;
    uint32_t candidate_version = 0;
    uint32_t page_index = 0;
    uint32_t page_count = 0;
    std::vector<std::wstring> candidates;
    int initial_selected_index = 0;
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

    void SetPlugins(const std::vector<CandidatePluginItem>& plugins);
    const std::vector<CandidatePluginItem>& GetPlugins() const { return m_plugins; }

    RECT GetManagerButtonRect() const { return m_rcMgrBtn; }
    bool IsHoveringManagerButton() const { return m_isHoveringMgrBtn; }
    bool IsPressedManagerButton() const { return m_isPressedMgrBtn; }
    HWND GetHwnd() const { return m_hWnd; }

    static LONG GetClassRefCount();

private:
    bool EnsureWindow();
    void InitDefaultPlugins();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hWnd = nullptr;
    HFONT m_hFont = nullptr;
    HFONT m_hBoldFont = nullptr;
    HFONT m_hSubFont = nullptr;
    bool m_hasRegisteredClassRef = false;
    CandidateDisplaySnapshot m_snapshot;
    POINT m_lastShowPt{0, 0};
    std::vector<std::wstring> m_candidates;
    std::vector<RECT> m_candidateRects;
    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_pressedIndex = -1;
    uint32_t m_pressedVersion = 0;
    uint64_t m_pressedContextId = 0;
    uint32_t m_pressedPageIndex = 0;
    std::wstring m_pressedCandidateText;

    RECT m_rcMgrBtn{0, 0, 0, 0};
    bool m_isHoveringMgrBtn = false;
    bool m_isPressedMgrBtn = false;
    bool m_trackingMouse = false;
    OnOpenManagerCallback m_onOpenManager;
    OnSelectCandidateCallback m_onSelectCandidate;
    OnSelectCandidateSnapshotCallback m_onSelectCandidateSnapshot;
    OnPageScrollCallback m_onPageScroll;
    OnPageScrollSnapshotCallback m_onPageScrollSnapshot;

    std::vector<CandidatePluginItem> m_plugins;
    OnExecutePluginCallback m_onExecutePlugin;
};
