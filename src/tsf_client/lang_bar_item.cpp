#include "lang_bar_item.h"
#include "text_input_processor.h"
#include "globals.h"
#include "../common/ime_guids.h"

CLangBarItemButton::CLangBarItemButton(CTextInputProcessor* pTip)
    : m_cRef(1), m_pTip(pTip), m_pItemSink(nullptr), m_cookie(0) {
    DllAddRef();
}

CLangBarItemButton::~CLangBarItemButton() {
    if (m_pItemSink) {
        m_pItemSink->Release();
        m_pItemSink = nullptr;
    }
    DllRelease();
}

STDMETHODIMP CLangBarItemButton::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_INVALIDARG;
    *ppvObj = nullptr;

    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) {
        *ppvObj = static_cast<ITfLangBarItemButton*>(this);
    } else if (riid == IID_ITfSource) {
        *ppvObj = static_cast<ITfSource*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CLangBarItemButton::AddRef() {
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) CLangBarItemButton::Release() {
    LONG cr = InterlockedDecrement(&m_cRef);
    if (cr == 0) {
        delete this;
    }
    return cr;
}

STDMETHODIMP CLangBarItemButton::GetInfo(TF_LANGBARITEMINFO* pInfo) {
    if (!pInfo) return E_INVALIDARG;
    pInfo->clsidService = CLSID_PersonalIME;
    pInfo->guidItem = GUID_PersonalIME_LangBarItem;
    // TF_LBI_STYLE_SHOWNINTRAY instructs Windows to dock this item on the taskbar notification tray!
    pInfo->dwStyle = TF_LBI_STYLE_SHOWNINTRAY | TF_LBI_STYLE_BTN_BUTTON;
    pInfo->ulSort = 0;
    StringCchCopyW(pInfo->szDescription, ARRAYSIZE(pInfo->szDescription), L"人人");
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetStatus(DWORD* pdwStatus) {
    if (!pdwStatus) return E_INVALIDARG;
    *pdwStatus = 0;
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::Show(BOOL fShow) {
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetTooltipString(BSTR* pbstrToolTip) {
    if (!pbstrToolTip) return E_INVALIDARG;
    *pbstrToolTip = SysAllocString(L"人人");
    return *pbstrToolTip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CLangBarItemButton::OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) {
    if (m_pTip) {
        m_pTip->OpenManagerFromTray();
    }
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::InitMenu(ITfMenu* pMenu) {
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::OnMenuSelect(UINT wID) {
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetIcon(HICON* phIcon) {
    if (!phIcon) return E_INVALIDARG;
    *phIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!*phIcon) {
        *phIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::GetText(BSTR* pbstrText) {
    if (!pbstrText) return E_INVALIDARG;
    *pbstrText = SysAllocString(L"人人");
    return *pbstrText ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CLangBarItemButton::AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) {
    if (!punk || !pdwCookie) return E_INVALIDARG;
    if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
    if (m_pItemSink) return CONNECT_E_ADVISELIMIT;

    if (FAILED(punk->QueryInterface(IID_ITfLangBarItemSink, (void**)&m_pItemSink))) {
        return E_NOINTERFACE;
    }
    *pdwCookie = 1;
    m_cookie = 1;
    return S_OK;
}

STDMETHODIMP CLangBarItemButton::UnadviseSink(DWORD dwCookie) {
    if (dwCookie != m_cookie || !m_pItemSink) return CONNECT_E_NOCONNECTION;
    m_pItemSink->Release();
    m_pItemSink = nullptr;
    m_cookie = 0;
    return S_OK;
}

void CLangBarItemButton::UpdateStatus() {
    if (m_pItemSink) {
        m_pItemSink->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_STATUS);
    }
}
