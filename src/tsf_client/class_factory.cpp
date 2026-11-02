#include "class_factory.h"
#include "text_input_processor.h"
#include "globals.h"

CClassFactory::CClassFactory() {
    m_cRef = 1;
    DllAddRef();
}

CClassFactory::~CClassFactory() {
    DllRelease();
}

STDMETHODIMP CClassFactory::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
        *ppvObj = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CClassFactory::AddRef() {
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) CClassFactory::Release() {
    LONG cr = InterlockedDecrement(&m_cRef);
    if (cr == 0) delete this;
    return cr;
}

STDMETHODIMP CClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;

    if (pUnkOuter != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }

    CTextInputProcessor* pTip = new CTextInputProcessor();
    if (!pTip) return E_OUTOFMEMORY;

    HRESULT hr = pTip->QueryInterface(riid, ppvObj);
    pTip->Release();
    return hr;
}

STDMETHODIMP CClassFactory::LockServer(BOOL fLock) {
    if (fLock) {
        InterlockedIncrement(&g_cDllRef);
    } else {
        InterlockedDecrement(&g_cDllRef);
    }
    return S_OK;
}

