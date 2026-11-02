#pragma once
#include <initguid.h>
#include <windows.h>

// Text Service CLSID: {7A5074A1-4F29-4FBE-A2D0-244D42B28B01}
DEFINE_GUID(CLSID_PersonalIME, 
    0x7a5074a1, 0x4f29, 0x4fbe, 0xa2, 0xd0, 0x24, 0x4d, 0x42, 0xb2, 0x8b, 0x1);

// Profile GUID: {C7613511-92F2-436A-8A46-2BEF5036B959}
DEFINE_GUID(GUID_PersonalIME_Profile, 
    0xc7613511, 0x92f2, 0x436a, 0x8a, 0x46, 0x2b, 0xef, 0x50, 0x36, 0xb9, 0x59);

inline constexpr const wchar_t* IME_NAME = L"Personal IME (私人输入法)";
inline constexpr const wchar_t* IME_PIPE_NAME = L"\\\\.\\pipe\\PersonalIME_Service_Pipe";
inline constexpr LANGID IME_LANGID = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

