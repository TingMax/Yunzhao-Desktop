#pragma once

#include <windows.h>

namespace atrium {

// Windows 11 is build 22000. Later checks keep the current Windows 11 path.
inline bool Windows11()
{
    static const bool eleven = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW version{sizeof(version)};
        auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return get && get(&version) == 0 && version.dwBuildNumber >= 22000;
    }();
    return eleven;
}

}
