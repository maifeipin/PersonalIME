#pragma once
#include <windows.h>
#include <string>
#include <vector>

class CCandidateWindow {
public:
    CCandidateWindow();
    ~CCandidateWindow();

    void Show(const std::vector<std::wstring>& candidates, POINT pt);
    void Hide();
    void Destroy();

    static LONG GetClassRefCount();

private:
    bool EnsureWindow();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hWnd = nullptr;
    HFONT m_hFont = nullptr;
    bool m_hasRegisteredClassRef = false;
    std::vector<std::wstring> m_candidates;
};
