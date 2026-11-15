#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include "../src/common/ime_guids.h"

typedef HRESULT(STDAPICALLTYPE* FnDllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT(STDAPICALLTYPE* FnDllCanUnloadNow)();

static int RunChildExitWithActiveLogger(const std::filesystem::path& dllPath) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE hMod = LoadLibraryW(dllPath.c_str());
    if (!hMod) {
        return 2;
    }
    auto pGetClassObj = reinterpret_cast<FnDllGetClassObject>(GetProcAddress(hMod, "DllGetClassObject"));
    if (!pGetClassObj) {
        return 3;
    }
    IClassFactory* pFactory = nullptr;
    if (FAILED(pGetClassObj(CLSID_PersonalIME, IID_IClassFactory, reinterpret_cast<void**>(&pFactory))) || !pFactory) {
        return 4;
    }
    ITfTextInputProcessor* pTip = nullptr;
    HRESULT hr = pFactory->CreateInstance(nullptr, IID_ITfTextInputProcessor, reinterpret_cast<void**>(&pTip));
    pFactory->Release();
    if (FAILED(hr) || !pTip) {
        return 5;
    }
    ITfKeyEventSink* pKeySink = nullptr;
    if (FAILED(pTip->QueryInterface(IID_ITfKeyEventSink, reinterpret_cast<void**>(&pKeySink))) || !pKeySink) {
        return 6;
    }

    // Hammer the logger from multiple threads so m_lock is actively contended when ExitProcess terminates them.
    std::atomic<bool> started{false};
    for (int t = 0; t < 4; ++t) {
        std::thread([pKeySink, &started]() {
            started.store(true, std::memory_order_release);
            for (;;) {
                pKeySink->OnSetFocus(FALSE);
            }
        }).detach();
    }
    while (!started.load(std::memory_order_acquire)) {
        Sleep(1);
    }
    Sleep(50);

    // Intentionally exit without FreeLibrary or stopping logger threads:
    // OS terminates worker threads mid-flight and invokes DllMain(DLL_PROCESS_DETACH, lpvReserved != nullptr) + static dtors.
    ExitProcess(0);
}

int main(int argc, char* argv[]) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path dllPath = std::filesystem::path(exePath).parent_path() / L"PersonalIME_LogTest.dll";

    if (argc >= 2 && std::strcmp(argv[1], "--child-exit-with-active-logger") == 0) {
        return RunChildExitWithActiveLogger(dllPath);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"[FAIL] CoInitializeEx failed: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    if (!std::filesystem::exists(dllPath)) {
        std::wcerr << L"[FAIL] Missing test DLL: " << dllPath.wstring() << std::endl;
        CoUninitialize();
        return 1;
    }

    char tempDir[MAX_PATH]{};
    DWORD dirLen = GetTempPathA(MAX_PATH, tempDir);
    std::filesystem::path logFile;
    if (dirLen > 0) {
        logFile = std::filesystem::path(tempDir) / "personalime_tsf_debug.log";
        std::error_code ec;
        std::filesystem::remove(logFile, ec);
    }

    std::wcout << L"[TEST] Repeated LoadLibrary / OnSetFocus / DllCanUnloadNow / FreeLibrary with PERSONALIME_DEBUG_TSF_LOG=1..." << std::endl;

    DWORD baselineHandles = 0;
    for (int iter = 0; iter < 15; ++iter) {
        HMODULE hMod = LoadLibraryW(dllPath.c_str());
        if (!hMod) {
            std::wcerr << L"[FAIL] LoadLibraryW failed at iter " << iter << L" err=" << GetLastError() << std::endl;
            CoUninitialize();
            return 1;
        }

        auto pGetClassObj = reinterpret_cast<FnDllGetClassObject>(GetProcAddress(hMod, "DllGetClassObject"));
        auto pCanUnload = reinterpret_cast<FnDllCanUnloadNow>(GetProcAddress(hMod, "DllCanUnloadNow"));
        if (!pGetClassObj || !pCanUnload) {
            std::wcerr << L"[FAIL] Missing COM exports in test DLL" << std::endl;
            FreeLibrary(hMod);
            CoUninitialize();
            return 1;
        }

        IClassFactory* pFactory = nullptr;
        hr = pGetClassObj(CLSID_PersonalIME, IID_IClassFactory, reinterpret_cast<void**>(&pFactory));
        if (FAILED(hr) || !pFactory) {
            std::wcerr << L"[FAIL] DllGetClassObject failed: 0x" << std::hex << hr << std::endl;
            FreeLibrary(hMod);
            CoUninitialize();
            return 1;
        }

        ITfTextInputProcessor* pTip = nullptr;
        hr = pFactory->CreateInstance(nullptr, IID_ITfTextInputProcessor, reinterpret_cast<void**>(&pTip));
        pFactory->Release();
        if (FAILED(hr) || !pTip) {
            std::wcerr << L"[FAIL] CreateInstance(ITfTextInputProcessor) failed: 0x" << std::hex << hr << std::endl;
            FreeLibrary(hMod);
            CoUninitialize();
            return 1;
        }

        ITfKeyEventSink* pKeySink = nullptr;
        if (SUCCEEDED(pTip->QueryInterface(IID_ITfKeyEventSink, reinterpret_cast<void**>(&pKeySink))) && pKeySink) {
            // Triggers TSF_DBG logging inside the DLL worker thread
            pKeySink->OnSetFocus(FALSE);
            pKeySink->Release();
        } else {
            std::wcerr << L"[FAIL] QueryInterface(IID_ITfKeyEventSink) failed at iter " << iter << std::endl;
            pTip->Release();
            FreeLibrary(hMod);
            CoUninitialize();
            return 1;
        }

        pTip->Release();

        bool canUnload = false;
        for (int poll = 0; poll < 150; ++poll) {
            if (pCanUnload() == S_OK) {
                canUnload = true;
                break;
            }
            Sleep(20);
        }
        if (!canUnload) {
            std::wcerr << L"[FAIL] DllCanUnloadNow did not converge to S_OK at iter " << iter << std::endl;
            FreeLibrary(hMod);
            CoUninitialize();
            return 1;
        }

        if (!FreeLibrary(hMod)) {
            std::wcerr << L"[FAIL] FreeLibrary failed at iter " << iter << L" err=" << GetLastError() << std::endl;
            CoUninitialize();
            return 1;
        }

        // Give OS loader a moment if worker called FreeLibraryAndExitThread concurrently
        for (int w = 0; w < 50; ++w) {
            if (GetModuleHandleW(dllPath.c_str()) == nullptr) {
                break;
            }
            Sleep(10);
        }
        if (GetModuleHandleW(dllPath.c_str()) != nullptr) {
            std::wcerr << L"[FAIL] DLL still mapped in process after FreeLibrary at iter " << iter << std::endl;
            CoUninitialize();
            return 1;
        }
        if (iter == 1) {
            GetProcessHandleCount(GetCurrentProcess(), &baselineHandles);
        } else if (iter > 1) {
            DWORD curHandles = 0;
            GetProcessHandleCount(GetCurrentProcess(), &curHandles);
            if (curHandles > baselineHandles + 1) {
                std::wcerr << L"[FAIL] Process handle count leaked at iter " << iter
                           << L" (baseline=" << baselineHandles << L", current=" << curHandles << L")" << std::endl;
                CoUninitialize();
                return 1;
            }
        }
    }

    if (!logFile.empty()) {
        if (!std::filesystem::exists(logFile) || std::filesystem::file_size(logFile) == 0) {
            std::wcerr << L"[FAIL] Expected non-empty TSF debug log at " << logFile.wstring() << std::endl;
            CoUninitialize();
            return 1;
        }
    }

    CoUninitialize();

    std::wcout << L"[TEST] Child process exit timeout with contended/active TSF logger..." << std::endl;
    for (int run = 0; run < 3; ++run) {
        std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" --child-exit-with-active-logger";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(
                exePath,
                cmdLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi)) {
            std::wcerr << L"[FAIL] CreateProcessW failed err=" << GetLastError() << std::endl;
            return 1;
        }
        DWORD waitRes = WaitForSingleObject(pi.hProcess, 3000);
        if (waitRes != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::wcerr << L"[FAIL] Child process hung during exit with active TSF logger (run=" << run << L")" << std::endl;
            return 1;
        }
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (exitCode != 0) {
            std::wcerr << L"[FAIL] Child process exited with code " << exitCode << L" (run=" << run << L")" << std::endl;
            return 1;
        }
    }

    std::wcout << L"[PASS] Repeated DLL load/log/unload & child exit timeout with PERSONALIME_DEBUG_TSF_LOG=1 succeeded!" << std::endl;
    return 0;
}
