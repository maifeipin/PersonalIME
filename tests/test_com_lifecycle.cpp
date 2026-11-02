#include <windows.h>
#include <msctf.h>
#include <iostream>
#include "../src/common/ime_guids.h"

typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID, REFIID, void**);
typedef HRESULT (STDAPICALLTYPE *PFN_DllCanUnloadNow)();

#define VERIFY_EQ(val, expected, msg) \
    do { \
        HRESULT _v = (val); \
        HRESULT _e = (expected); \
        if (_v != _e) { \
            std::wcerr << L"[COM TEST FAIL] " << msg << L": expected 0x" << std::hex << _e \
                       << L", got 0x" << _v << std::dec << L" at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (false)

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::wcout << L"=== Testing COM DLL Ref Count & Unload Safety ===" << std::endl;

    HMODULE hDll = LoadLibraryW(L"PersonalIME.dll");
    if (!hDll) {
        std::wcerr << L"Failed to load PersonalIME.dll! Error: " << GetLastError() << std::endl;
        return 1;
    }

    auto fnGetClassObject = reinterpret_cast<PFN_DllGetClassObject>(GetProcAddress(hDll, "DllGetClassObject"));
    auto fnCanUnloadNow = reinterpret_cast<PFN_DllCanUnloadNow>(GetProcAddress(hDll, "DllCanUnloadNow"));

    if (!fnGetClassObject || !fnCanUnloadNow) {
        std::wcerr << L"Failed to get COM entry points from DLL!" << std::endl;
        FreeLibrary(hDll);
        return 1;
    }

    // 1. Initially, no objects alive -> CanUnloadNow must be S_OK (0)
    VERIFY_EQ(fnCanUnloadNow(), S_OK, L"Initial state must allow unload");

    // 2. Obtain Class Factory
    IClassFactory* pFactory = nullptr;
    HRESULT hr = fnGetClassObject(CLSID_PersonalIME, IID_IClassFactory, reinterpret_cast<void**>(&pFactory));
    VERIFY_EQ(hr, S_OK, L"DllGetClassObject must succeed");
    VERIFY_EQ(fnCanUnloadNow(), S_FALSE, L"With factory alive, must NOT allow unload");

    // 3. Create TextInputProcessor instance from factory
    ITfTextInputProcessor* pTip = nullptr;
    hr = pFactory->CreateInstance(nullptr, IID_ITfTextInputProcessor, reinterpret_cast<void**>(&pTip));
    VERIFY_EQ(hr, S_OK, L"CreateInstance must succeed");

    // 4. Release Class Factory - TIP is STILL ALIVE!
    pFactory->Release();
    pFactory = nullptr;

    // CRITICAL CHECK: Previously this returned S_OK (the bug). Now it MUST return S_FALSE!
    VERIFY_EQ(fnCanUnloadNow(), S_FALSE, L"With TIP alive after factory released, must NOT allow unload");
    std::wcout << L"[PASS] Verified: Releasing ClassFactory while TIP is alive DOES NOT allow DLL unload!" << std::endl;

    // 5. Release TIP
    pTip->Release();
    pTip = nullptr;

    // Now all instances released -> must allow unload (S_OK)
    VERIFY_EQ(fnCanUnloadNow(), S_OK, L"After all instances released, must allow unload");
    std::wcout << L"[PASS] Verified: Releasing all instances restores S_OK for DllCanUnloadNow." << std::endl;

    FreeLibrary(hDll);
    std::wcout << L"=== All COM lifecycle tests passed! ===" << std::endl;
    return 0;
}
