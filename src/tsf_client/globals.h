#pragma once
#include <windows.h>

extern HINSTANCE g_hInst;
extern LONG g_cDllRef;

inline void DllAddRef() {
    InterlockedIncrement(&g_cDllRef);
}

inline void DllRelease() {
    InterlockedDecrement(&g_cDllRef);
}

