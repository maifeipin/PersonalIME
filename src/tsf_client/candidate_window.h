#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

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

class CCandidateWindow {
public:
    using OnOpenManagerCallback = std::function<void()>;
    using OnExecutePluginCallback = std::function<void(const CandidatePluginItem& item)>;
    using OnSelectCandidateCallback = std::function<void(int index)>;
    using OnPageScrollCallback = std::function<void(bool nextPage)>;

    CCandidateWindow();
    ~CCandidateWindow();

    void Show(const std::vector<std::wstring>& candidates, POINT pt);
    void Hide();
    void Destroy();

    void SetOnOpenManager(OnOpenManagerCallback cb) { m_onOpenManager = std::move(cb); }
    void SetOnExecutePlugin(OnExecutePluginCallback cb) { m_onExecutePlugin = std::move(cb); }
    void SetOnSelectCandidate(OnSelectCandidateCallback cb) { m_onSelectCandidate = std::move(cb); }
    void SetOnPageScroll(OnPageScrollCallback cb) { m_onPageScroll = std::move(cb); }
    void TriggerManagerButton();
    void TriggerPluginButton(uint32_t pluginId);

    int GetSelectedIndex() const { return m_selectedIndex; }
    void SetSelectedIndex(int index);
    int GetHoveredIndex() const { return m_hoveredIndex; }
    int GetCandidateCount() const { return static_cast<int>(m_candidates.size()); }
    const std::vector<RECT>& GetCandidateRects() const { return m_candidateRects; }

    void SetPlugins(const std::vector<CandidatePluginItem>& plugins) { m_plugins = plugins; }
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
    std::vector<std::wstring> m_candidates;
    std::vector<RECT> m_candidateRects;
    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;
    int m_pressedIndex = -1;

    RECT m_rcMgrBtn{0, 0, 0, 0};
    bool m_isHoveringMgrBtn = false;
    bool m_isPressedMgrBtn = false;
    bool m_trackingMouse = false;
    OnOpenManagerCallback m_onOpenManager;
    OnSelectCandidateCallback m_onSelectCandidate;
    OnPageScrollCallback m_onPageScroll;

    std::vector<CandidatePluginItem> m_plugins;
    OnExecutePluginCallback m_onExecutePlugin;
};
