// The shell's taskbar window.
//
// Upstream 3.0 factors every lookup of Explorer's taskbar through this header, together with
// the responsiveness guards its tray host needs. Yunzhao has no tray host of its own (the bar
// reads the real one), so only the lookup is here; folder_stacks.cpp includes the header.
#pragma once

#include <windows.h>

namespace yunzhao {

// The Shell_TrayWnd of the shell's process (the desktop's), else the first one of another
// process than `skip` (default: this one). No allocation: the crash handler and the guard use it.
inline HWND ShellTaskbar(DWORD skip = GetCurrentProcessId())
{
    DWORD shell = 0;
    if (HWND desktop = GetShellWindow())
        GetWindowThreadProcessId(desktop, &shell);
    HWND other = nullptr;
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) != nullptr;) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (shell && pid == shell)
            return h;
        if (pid != skip && !other)
            other = h;
    }
    return other;
}

// A window that answers WM_NULL in time: Explorer's taskbar can be hung, and a call into it
// would then block the caller. Upstream 3.0 guards every shell query with this; `honorCooldown`
// is upstream's parameter for its own retry window, unused here.
inline bool ShellResponsive(HWND shell, UINT timeoutMs = 500, bool = true)
{
    if (!shell || IsHungAppWindow(shell))
        return false;
    DWORD_PTR answer = 0;
    return SendMessageTimeoutW(shell, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, timeoutMs, &answer) != 0;
}

} // namespace yunzhao
