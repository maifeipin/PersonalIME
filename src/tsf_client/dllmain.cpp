// Personal IME (人人输入法) - TSF In-proc Server
#include <windows.h>
#include <msctf.h>
#include <strsafe.h>
#include "class_factory.h"
#include "globals.h"
#include "../common/ime_guids.h"

HINSTANCE g_hInst = nullptr;
LONG g_cDllRef = 0;

STDAPI DllUnregisterServer();

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return (g_cDllRef == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_PersonalIME)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    CClassFactory* pFactory = new CClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

static BOOL SetRegString(HKEY hRoot, const wchar_t* subkey, const wchar_t* name, const wchar_t* data) {
    HKEY hKey;
    LONG res = RegCreateKeyExW(hRoot, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (res == ERROR_ACCESS_DENIED && hRoot == HKEY_CLASSES_ROOT) {
        // Fallback to Per-User COM registration under HKCU\Software\Classes
        wchar_t userSubkey[MAX_PATH];
        StringCchPrintfW(userSubkey, ARRAYSIZE(userSubkey), L"Software\\Classes\\%s", subkey);
        res = RegCreateKeyExW(HKEY_CURRENT_USER, userSubkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    }
    if (res != ERROR_SUCCESS) {
        return FALSE;
    }
    DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    res = RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

STDAPI DllRegisterServer() {
    wchar_t szDllPath[MAX_PATH];
    if (!GetModuleFileNameW(g_hInst, szDllPath, ARRAYSIZE(szDllPath))) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t szClsid[64];
    StringFromGUID2(CLSID_PersonalIME, szClsid, ARRAYSIZE(szClsid));

    wchar_t szKey[MAX_PATH];
    StringCchPrintfW(szKey, ARRAYSIZE(szKey), L"CLSID\\%s", szClsid);

    if (!SetRegString(HKEY_CLASSES_ROOT, szKey, nullptr, IME_NAME)) {
        return E_FAIL;
    }

    wchar_t szInprocKey[MAX_PATH];
    StringCchPrintfW(szInprocKey, ARRAYSIZE(szInprocKey), L"CLSID\\%s\\InProcServer32", szClsid);
    if (!SetRegString(HKEY_CLASSES_ROOT, szInprocKey, nullptr, szDllPath)) {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, szKey);
        return E_FAIL;
    }
    if (!SetRegString(HKEY_CLASSES_ROOT, szInprocKey, L"ThreadingModel", L"Apartment")) {
        RegDeleteTreeW(HKEY_CLASSES_ROOT, szKey);
        return E_FAIL;
    }

    // Initialize COM
    HRESULT hrCo = CoInitialize(nullptr);

    // Register with TSF InputProcessorProfiles
    ITfInputProcessorProfiles* pProfiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, (void**)&pProfiles);
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        DllUnregisterServer();
        return hr;
    }

    hr = pProfiles->Register(CLSID_PersonalIME);
    if (FAILED(hr)) {
        pProfiles->Release();
        if (SUCCEEDED(hrCo)) CoUninitialize();
        DllUnregisterServer();
        return hr;
    }

    pProfiles->RemoveLanguageProfile(CLSID_PersonalIME, IME_LANGID, GUID_PersonalIME_Profile);
    hr = pProfiles->AddLanguageProfile(
        CLSID_PersonalIME,
        IME_LANGID,
        GUID_PersonalIME_Profile,
        IME_NAME,
        static_cast<ULONG>(wcslen(IME_NAME)),
        szDllPath,
        static_cast<ULONG>(wcslen(szDllPath)),
        0
    );
    pProfiles->Release();
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        DllUnregisterServer();
        return hr;
    }

    // Explicitly set Description and Display Description in CTF TIP registry
    const wchar_t* tipProfileKey = L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\\LanguageProfile\\0x00000804\\{C7613511-92F2-436A-8A46-2BEF5036B959}";
    SetRegString(HKEY_LOCAL_MACHINE, tipProfileKey, L"Description", IME_NAME);
    SetRegString(HKEY_LOCAL_MACHINE, tipProfileKey, L"Display Description", IME_NAME);
    SetRegString(HKEY_CURRENT_USER, tipProfileKey, L"Description", IME_NAME);
    SetRegString(HKEY_CURRENT_USER, tipProfileKey, L"Display Description", IME_NAME);

    // Register Category
    ITfCategoryMgr* pCatMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr, (void**)&pCatMgr);
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        DllUnregisterServer();
        return hr;
    }

    hr = pCatMgr->RegisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIP_KEYBOARD, CLSID_PersonalIME);
    pCatMgr->Release();
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        DllUnregisterServer();
        return hr;
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return S_OK;
}

STDAPI DllUnregisterServer() {
    HRESULT hrCo = CoInitialize(nullptr);

    // Unregister Category
    ITfCategoryMgr* pCatMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, (void**)&pCatMgr))) {
        pCatMgr->UnregisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIP_KEYBOARD, CLSID_PersonalIME);
        pCatMgr->Release();
    }

    // Unregister Profile
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfiles, (void**)&pProfiles))) {
        pProfiles->Unregister(CLSID_PersonalIME);
        pProfiles->Release();
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();

    // Delete Registry
    wchar_t szClsid[64];
    StringFromGUID2(CLSID_PersonalIME, szClsid, ARRAYSIZE(szClsid));

    wchar_t szKey[MAX_PATH];
    StringCchPrintfW(szKey, ARRAYSIZE(szKey), L"CLSID\\%s", szClsid);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, szKey);

    wchar_t szUserKey[MAX_PATH];
    StringCchPrintfW(szUserKey, ARRAYSIZE(szUserKey), L"Software\\Classes\\CLSID\\%s", szClsid);
    RegDeleteTreeW(HKEY_CURRENT_USER, szUserKey);

    return S_OK;
}
