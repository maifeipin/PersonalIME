// Personal IME (人人输入法) - TSF In-proc Server
#include <windows.h>
#include <msctf.h>
#include <strsafe.h>
#include <vector>
#include <string>
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
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        ShutdownTsfDebugLoggerAsync(lpvReserved != nullptr);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    if (g_cDllRef == 1 && IsTsfDebugLoggerHoldingDllRef()) {
        ShutdownTsfDebugLoggerAsync();
    }
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

struct RegValueSnapshot {
    BOOL existed = FALSE;
    DWORD type = REG_SZ;
    std::vector<BYTE> data;
};

static LONG OpenRegKeyWithHKCRFallback(HKEY hRoot, const wchar_t* subkey, REGSAM samDesired, BOOL createIfWrite, HKEY* phKey) {
    LONG res = createIfWrite
        ? RegCreateKeyExW(hRoot, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, samDesired, nullptr, phKey, nullptr)
        : RegOpenKeyExW(hRoot, subkey, 0, samDesired, phKey);
    if ((res == ERROR_ACCESS_DENIED || (!createIfWrite && res == ERROR_FILE_NOT_FOUND)) && hRoot == HKEY_CLASSES_ROOT) {
        wchar_t userSubkey[MAX_PATH];
        if (SUCCEEDED(StringCchPrintfW(userSubkey, ARRAYSIZE(userSubkey), L"Software\\Classes\\%s", subkey))) {
            res = createIfWrite
                ? RegCreateKeyExW(HKEY_CURRENT_USER, userSubkey, 0, nullptr, REG_OPTION_NON_VOLATILE, samDesired, nullptr, phKey, nullptr)
                : RegOpenKeyExW(HKEY_CURRENT_USER, userSubkey, 0, samDesired, phKey);
        }
    }
    return res;
}

static BOOL CaptureRegValue(HKEY hRoot, const wchar_t* subkey, const wchar_t* name, RegValueSnapshot& out) {
    out.existed = FALSE;
    out.type = REG_SZ;
    out.data.clear();

    HKEY hKey = nullptr;
    LONG openRes = OpenRegKeyWithHKCRFallback(hRoot, subkey, KEY_READ, FALSE, &hKey);
    if (openRes == ERROR_FILE_NOT_FOUND || openRes == ERROR_PATH_NOT_FOUND) return TRUE;
    if (openRes != ERROR_SUCCESS) return FALSE;

    DWORD type = 0;
    DWORD bytes = 0;
    LONG res = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &bytes);
    if (res == ERROR_SUCCESS) {
        out.data.resize(bytes);
        res = RegQueryValueExW(hKey, name, nullptr, &type, bytes > 0 ? out.data.data() : nullptr, &bytes);
        if (res == ERROR_SUCCESS) {
            out.existed = TRUE;
            out.type = type;
            out.data.resize(bytes);
        } else {
            out.data.clear();
        }
    }
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND;
}

static BOOL RestoreRegValue(HKEY hRoot, const wchar_t* subkey, const wchar_t* name, const RegValueSnapshot& snap) {
    HKEY hKey = nullptr;
    if (snap.existed) {
        if (OpenRegKeyWithHKCRFallback(hRoot, subkey, KEY_WRITE, TRUE, &hKey) != ERROR_SUCCESS) {
            return FALSE;
        }
        LONG res = RegSetValueExW(
            hKey,
            name,
            0,
            snap.type,
            snap.data.empty() ? nullptr : snap.data.data(),
            static_cast<DWORD>(snap.data.size()));
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }

    LONG openRes = OpenRegKeyWithHKCRFallback(hRoot, subkey, KEY_SET_VALUE, FALSE, &hKey);
    if (openRes == ERROR_FILE_NOT_FOUND) {
        return TRUE;
    }
    if (openRes != ERROR_SUCCESS) {
        return FALSE;
    }
    LONG delRes = RegDeleteValueW(hKey, name);
    RegCloseKey(hKey);
    return (delRes == ERROR_SUCCESS || delRes == ERROR_FILE_NOT_FOUND);
}

struct RegistrationSnapshot {
    std::wstring machineClsidKey;
    std::wstring userClsidKey;
    std::wstring machineInprocKey;
    std::wstring userInprocKey;
    const wchar_t* tipProfileKey = nullptr;
    RegValueSnapshot machineClsidDefault;
    RegValueSnapshot userClsidDefault;
    RegValueSnapshot machineInprocDefault;
    RegValueSnapshot userInprocDefault;
    RegValueSnapshot machineThreadingModel;
    RegValueSnapshot userThreadingModel;
    RegValueSnapshot hklmIconFile;
    RegValueSnapshot hklmDesc;
    RegValueSnapshot hklmDisplayDesc;
    RegValueSnapshot hkcuDesc;
    RegValueSnapshot hkcuDisplayDesc;
};

static BOOL RollbackDllRegister(const RegistrationSnapshot& snap) {
    if (snap.machineInprocDefault.existed || snap.userInprocDefault.existed) {
        BOOL ok = TRUE;
        // HKCR is a merged view. Restore the physical hives separately so an
        // HKCU override cannot leave a newly written HKLM value behind.
        ok = RestoreRegValue(HKEY_LOCAL_MACHINE, snap.machineClsidKey.c_str(), nullptr, snap.machineClsidDefault) && ok;
        ok = RestoreRegValue(HKEY_CURRENT_USER, snap.userClsidKey.c_str(), nullptr, snap.userClsidDefault) && ok;
        ok = RestoreRegValue(HKEY_LOCAL_MACHINE, snap.machineInprocKey.c_str(), nullptr, snap.machineInprocDefault) && ok;
        ok = RestoreRegValue(HKEY_CURRENT_USER, snap.userInprocKey.c_str(), nullptr, snap.userInprocDefault) && ok;
        ok = RestoreRegValue(HKEY_LOCAL_MACHINE, snap.machineInprocKey.c_str(), L"ThreadingModel", snap.machineThreadingModel) && ok;
        ok = RestoreRegValue(HKEY_CURRENT_USER, snap.userInprocKey.c_str(), L"ThreadingModel", snap.userThreadingModel) && ok;
        ok = RestoreRegValue(HKEY_LOCAL_MACHINE, snap.tipProfileKey, L"IconFile", snap.hklmIconFile) && ok;
        ok = RestoreRegValue(HKEY_LOCAL_MACHINE, snap.tipProfileKey, L"Description", snap.hklmDesc) && ok;
        ok = RestoreRegValue(HKEY_LOCAL_MACHINE, snap.tipProfileKey, L"Display Description", snap.hklmDisplayDesc) && ok;
        ok = RestoreRegValue(HKEY_CURRENT_USER, snap.tipProfileKey, L"Description", snap.hkcuDesc) && ok;
        ok = RestoreRegValue(HKEY_CURRENT_USER, snap.tipProfileKey, L"Display Description", snap.hkcuDisplayDesc) && ok;
        return ok;
    }
    return SUCCEEDED(DllUnregisterServer());
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

    wchar_t szInprocKey[MAX_PATH];
    StringCchPrintfW(szInprocKey, ARRAYSIZE(szInprocKey), L"CLSID\\%s\\InProcServer32", szClsid);

    const wchar_t* tipProfileKey = L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7A5074A1-4F29-4FBE-A2D0-244D42B28B01}\\LanguageProfile\\0x00000804\\{C7613511-92F2-436A-8A46-2BEF5036B959}";

    RegistrationSnapshot snap;
    snap.machineClsidKey = std::wstring(L"SOFTWARE\\Classes\\") + szKey;
    snap.userClsidKey = std::wstring(L"Software\\Classes\\") + szKey;
    snap.machineInprocKey = std::wstring(L"SOFTWARE\\Classes\\") + szInprocKey;
    snap.userInprocKey = std::wstring(L"Software\\Classes\\") + szInprocKey;
    snap.tipProfileKey = tipProfileKey;
    if (!CaptureRegValue(HKEY_LOCAL_MACHINE, snap.machineClsidKey.c_str(), nullptr, snap.machineClsidDefault) ||
        !CaptureRegValue(HKEY_CURRENT_USER, snap.userClsidKey.c_str(), nullptr, snap.userClsidDefault) ||
        !CaptureRegValue(HKEY_LOCAL_MACHINE, snap.machineInprocKey.c_str(), nullptr, snap.machineInprocDefault) ||
        !CaptureRegValue(HKEY_CURRENT_USER, snap.userInprocKey.c_str(), nullptr, snap.userInprocDefault) ||
        !CaptureRegValue(HKEY_LOCAL_MACHINE, snap.machineInprocKey.c_str(), L"ThreadingModel", snap.machineThreadingModel) ||
        !CaptureRegValue(HKEY_CURRENT_USER, snap.userInprocKey.c_str(), L"ThreadingModel", snap.userThreadingModel) ||
        !CaptureRegValue(HKEY_LOCAL_MACHINE, tipProfileKey, L"IconFile", snap.hklmIconFile) ||
        !CaptureRegValue(HKEY_LOCAL_MACHINE, tipProfileKey, L"Description", snap.hklmDesc) ||
        !CaptureRegValue(HKEY_LOCAL_MACHINE, tipProfileKey, L"Display Description", snap.hklmDisplayDesc) ||
        !CaptureRegValue(HKEY_CURRENT_USER, tipProfileKey, L"Description", snap.hkcuDesc) ||
        !CaptureRegValue(HKEY_CURRENT_USER, tipProfileKey, L"Display Description", snap.hkcuDisplayDesc)) {
        // A read failure is not an absent value. Never mutate state without a reliable snapshot.
        return E_FAIL;
    }

    if (!SetRegString(HKEY_CLASSES_ROOT, szKey, nullptr, IME_NAME)) {
        return E_FAIL;
    }

    if (!SetRegString(HKEY_CLASSES_ROOT, szInprocKey, nullptr, szDllPath) ||
        !SetRegString(HKEY_CLASSES_ROOT, szInprocKey, L"ThreadingModel", L"Apartment")) {
        return RollbackDllRegister(snap) ? E_FAIL : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
    }

    // Initialize COM
    HRESULT hrCo = CoInitialize(nullptr);

    // Register with TSF InputProcessorProfiles
    ITfInputProcessorProfiles* pProfiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, (void**)&pProfiles);
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return RollbackDllRegister(snap) ? hr : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
    }

    hr = pProfiles->Register(CLSID_PersonalIME);
    if (FAILED(hr)) {
        pProfiles->Release();
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return RollbackDllRegister(snap) ? hr : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
    }

    BSTR bstrExistingDesc = nullptr;
    BOOL profileExists = SUCCEEDED(pProfiles->GetLanguageProfileDescription(
        CLSID_PersonalIME, IME_LANGID, GUID_PersonalIME_Profile, &bstrExistingDesc));
    if (bstrExistingDesc) {
        SysFreeString(bstrExistingDesc);
        bstrExistingDesc = nullptr;
    }

    if (!profileExists) {
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
        if (SUCCEEDED(hr)) {
            pProfiles->EnableLanguageProfileByDefault(CLSID_PersonalIME, IME_LANGID, GUID_PersonalIME_Profile, TRUE);
            pProfiles->EnableLanguageProfile(CLSID_PersonalIME, IME_LANGID, GUID_PersonalIME_Profile, TRUE);
        }
    } else {
        // Upgrade path: preserve user's existing profile enablement/order and only update module/display metadata
        hr = SetRegString(HKEY_LOCAL_MACHINE, tipProfileKey, L"IconFile", szDllPath) ? S_OK : E_FAIL;
    }
    pProfiles->Release();
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return RollbackDllRegister(snap) ? hr : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
    }

    // Explicitly set Description and Display Description in CTF TIP registry
    if (!SetRegString(HKEY_LOCAL_MACHINE, tipProfileKey, L"Description", IME_NAME) ||
        !SetRegString(HKEY_LOCAL_MACHINE, tipProfileKey, L"Display Description", IME_NAME)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return RollbackDllRegister(snap) ? E_FAIL : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
    }
    SetRegString(HKEY_CURRENT_USER, tipProfileKey, L"Description", IME_NAME);
    SetRegString(HKEY_CURRENT_USER, tipProfileKey, L"Display Description", IME_NAME);

    // Register Category
    ITfCategoryMgr* pCatMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr, (void**)&pCatMgr);
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return RollbackDllRegister(snap) ? hr : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
    }

    hr = pCatMgr->RegisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIP_KEYBOARD, CLSID_PersonalIME);
    if (SUCCEEDED(hr)) {
        hr = pCatMgr->RegisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, CLSID_PersonalIME);
    }
    if (SUCCEEDED(hr)) {
        hr = pCatMgr->RegisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, CLSID_PersonalIME);
    }
    if (SUCCEEDED(hr)) {
        hr = pCatMgr->RegisterCategory(CLSID_PersonalIME, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, CLSID_PersonalIME);
    }
    pCatMgr->Release();
    if (FAILED(hr)) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return RollbackDllRegister(snap) ? hr : HRESULT_FROM_WIN32(ERROR_CAN_NOT_COMPLETE);
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
        pCatMgr->UnregisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, CLSID_PersonalIME);
        pCatMgr->UnregisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, CLSID_PersonalIME);
        pCatMgr->UnregisterCategory(CLSID_PersonalIME, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, CLSID_PersonalIME);
        // Idempotently clean up historical input mode compartment category if registered by prior versions
        pCatMgr->UnregisterCategory(CLSID_PersonalIME, GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, CLSID_PersonalIME);
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
