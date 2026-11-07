#pragma once
#include <windows.h>
#include <olectl.h>
#include <msctf.h>
#include <ctfutb.h>
#include <strsafe.h>

class CTextInputProcessor;

class CLangBarItemButton : public ITfLangBarItemButton, public ITfSource {
public:
    explicit CLangBarItemButton(CTextInputProcessor* pTip);
    virtual ~CLangBarItemButton();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* pInfo) override;
    STDMETHODIMP GetStatus(DWORD* pdwStatus) override;
    STDMETHODIMP Show(BOOL fShow) override;
    STDMETHODIMP GetTooltipString(BSTR* pbstrToolTip) override;

    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) override;
    STDMETHODIMP InitMenu(ITfMenu* pMenu) override;
    STDMETHODIMP OnMenuSelect(UINT wID) override;
    STDMETHODIMP GetIcon(HICON* phIcon) override;
    STDMETHODIMP GetText(BSTR* pbstrText) override;

    // ITfSource
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override;
    STDMETHODIMP UnadviseSink(DWORD dwCookie) override;

    void UpdateStatus();

private:
    LONG m_cRef;
    CTextInputProcessor* m_pTip;
    ITfLangBarItemSink* m_pItemSink;
    DWORD m_cookie;
};
