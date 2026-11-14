#pragma once
#include <windows.h>
#include <msctf.h>
#include <oleauto.h>
#include "globals.h"
#include "../common/ime_guids.h"

class CDisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
    CDisplayAttributeInfo() {
        DllAddRef();
        Reset();
    }

    virtual ~CDisplayAttributeInfo() {
        DllRelease();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
            *ppvObj = static_cast<ITfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    STDMETHODIMP GetGUID(GUID* pguid) override {
        if (!pguid) return E_INVALIDARG;
        *pguid = GUID_PersonalIME_DisplayAttributeInput;
        return S_OK;
    }

    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override {
        if (!pbstrDesc) return E_INVALIDARG;
        *pbstrDesc = SysAllocString(L"PersonalIME Input Composition");
        return (*pbstrDesc != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) return E_INVALIDARG;
        *pda = m_attr;
        return S_OK;
    }

    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE* pda) override {
        if (!pda) return E_INVALIDARG;
        m_attr = *pda;
        return S_OK;
    }

    STDMETHODIMP Reset() override {
        m_attr = {};
        m_attr.crText.type = TF_CT_NONE;
        m_attr.crBk.type = TF_CT_NONE;
        m_attr.lsStyle = TF_LS_DOT;
        m_attr.fBoldLine = FALSE;
        m_attr.crLine.type = TF_CT_NONE;
        m_attr.bAttr = TF_ATTR_INPUT;
        return S_OK;
    }

private:
    LONG m_cRef = 1;
    TF_DISPLAYATTRIBUTE m_attr{};
};

class CEnumDisplayAttributeInfo : public IEnumTfDisplayAttributeInfo {
public:
    explicit CEnumDisplayAttributeInfo(ULONG index = 0) : m_index(index) {
        DllAddRef();
    }

    virtual ~CEnumDisplayAttributeInfo() {
        DllRelease();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
            *ppvObj = static_cast<IEnumTfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** ppEnum) override {
        if (!ppEnum) return E_INVALIDARG;
        *ppEnum = new CEnumDisplayAttributeInfo(m_index);
        return S_OK;
    }

    STDMETHODIMP Next(ULONG ulCount, ITfDisplayAttributeInfo** rgInfo, ULONG* pcFetched) override {
        if (!rgInfo) return E_INVALIDARG;
        if (ulCount == 0) {
            if (pcFetched) *pcFetched = 0;
            return S_OK;
        }

        ULONG fetched = 0;
        if (m_index == 0) {
            rgInfo[0] = new CDisplayAttributeInfo();
            m_index = 1;
            fetched = 1;
        }

        if (pcFetched) {
            *pcFetched = fetched;
        }
        return (fetched == ulCount) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }

    STDMETHODIMP Skip(ULONG ulCount) override {
        if (m_index + ulCount > 1) {
            m_index = 1;
            return S_FALSE;
        }
        m_index += ulCount;
        return S_OK;
    }

private:
    LONG m_cRef = 1;
    ULONG m_index = 0;
};
