#include "shell/system_chrome.h"

#include "log.h"
#include "settings.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>

#include <thread>
#include <vector>

namespace yunzhao {

namespace {

// The shell's Do Not Disturb service (undocumented; the same one the Windows
// notification centre's button uses). After IUnknown: the selected profile as
// a CoTaskMemAlloc'd string, and setting it.
constexpr CLSID kQuietHoursSettings = {0xF53321FA, 0x34F8, 0x4B7F, {0xB9, 0xA3, 0x36, 0x18, 0x77, 0xCB, 0x94, 0xCF}};
constexpr IID kIQuietHoursSettings = {0x6BFF4732, 0x81EC, 0x4FFB, {0xAE, 0x67, 0xB6, 0xC1, 0xBC, 0x29, 0x63, 0x1F}};
struct IQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(LPWSTR* profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(LPCWSTR profile) = 0;
};

constexpr wchar_t kUnrestricted[] = L"Microsoft.QuietHoursProfile.Unrestricted";
// Only alarms break through: no app's banner shows (priority apps would).
constexpr wchar_t kQuiet[] = L"Microsoft.QuietHoursProfile.AlarmsOnly";

SystemChrome* g_chrome = nullptr;

std::wstring StatePath()
{
    std::wstring p = SettingsPath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\system.json";
}

std::vector<HWND> Taskbars()
{
    std::vector<HWND> out;
    if (HWND main = FindWindowW(L"Shell_TrayWnd", nullptr))
        out.push_back(main);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        out.push_back(w);
    return out;
}

bool IsTaskbar(HWND w)
{
    wchar_t cls[32] = {};
    GetClassNameW(w, cls, 32);
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

bool AutoHide()
{
    APPBARDATA abd{sizeof(abd)};
    return (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
}

void SetAutoHide(bool on)
{
    auto apply = [&](HWND hwnd) {
        if (!hwnd)
            return;
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = hwnd;
        // Windows 10 applies a zero edge as the top. Read this bar's own edge first.
        SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
        abd.lParam = on ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
        SHAppBarMessage(ABM_SETSTATE, &abd);
    };
    apply(FindWindowW(L"Shell_TrayWnd", nullptr));
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        apply(w);
}

// Windows maximized while the taskbar still held its strip of the work area keep that gap when the
// area grows: not every app lays itself out again. Each one is stretched to its work area, its
// invisible borders outside it as Windows places a maximized window. A moment later: the shell
// moves the work area after the call returns.
void RefitMaximizedLater()
{
    std::thread([] {
        Sleep(500);
        EnumWindows([](HWND w, LPARAM) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            RECT r{}, f{};
            if (!IsWindowVisible(w) || !IsZoomed(w) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) ||
                !GetWindowRect(w, &r) || FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &f, sizeof(f))))
                return TRUE;
            const RECT& a = mi.rcWork;
            if (f.left <= a.left + 1 && f.top <= a.top + 1 && f.right >= a.right - 1 && f.bottom >= a.bottom - 1)
                return TRUE;
            Log(L"chrome: a maximized window stopped short of the work area (%ld,%ld,%ld,%ld); fitted", f.left, f.top, f.right, f.bottom);
            SetWindowPos(w, nullptr, r.left + a.left - f.left, r.top + a.top - f.top, (r.right + a.right - f.right) - (r.left + a.left - f.left),
                         (r.bottom + a.bottom - f.bottom) - (r.top + a.top - f.top), SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            return TRUE;
        }, 0);
    }).detach();
}

void ShowTaskbars()
{
    // Also used by the exception filter: enumerate without allocating.
    if (HWND w = FindWindowW(L"Shell_TrayWnd", nullptr))
        ShowWindow(w, SW_SHOWNA);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        ShowWindow(w, SW_SHOWNA);
}

} // namespace

std::wstring QuietProfile()
{
    IQuietHoursSettings* qh = nullptr;
    if (FAILED(CoCreateInstance(kQuietHoursSettings, nullptr, CLSCTX_ALL, kIQuietHoursSettings, reinterpret_cast<void**>(&qh))) || !qh)
        return {};
    LPWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(qh->get_UserSelectedProfile(&p)) && p)
        out = p;
    CoTaskMemFree(p);
    qh->Release();
    return out;
}

bool SetQuietProfile(const wchar_t* profile)
{
    IQuietHoursSettings* qh = nullptr;
    if (FAILED(CoCreateInstance(kQuietHoursSettings, nullptr, CLSCTX_ALL, kIQuietHoursSettings, reinterpret_cast<void**>(&qh))) || !qh)
        return false;
    bool ok = SUCCEEDED(qh->put_UserSelectedProfile(profile));
    qh->Release();
    return ok && QuietProfile() == profile;
}

SystemChrome::SystemChrome()
{
    g_chrome = this;
    // Left over from a run that did not end cleanly: put it back first (the
    // settings then hide again what they ask for).
    UndoLeftover();
}

bool SystemChrome::UndoLeftover()
{
    std::wstring left = ReadTextFile(StatePath());
    if (left.empty())
        return false;
    Log(L"chrome: undoing the last run's changes: %s", left.c_str());
    if (left.find(L"\"autoHide\":true") != std::wstring::npos)
        SetAutoHide(false);
    ShowTaskbars();
    if (left.find(L"\"quiet\":true") != std::wstring::npos && QuietProfile() == kQuiet)
        SetQuietProfile(kUnrestricted);
    DeleteFileW(StatePath().c_str());
    return true;
}

void SystemChrome::ShowTaskbarsNow() { ShowTaskbars(); }

SystemChrome::~SystemChrome()
{
    RestoreForExit();
    g_chrome = nullptr;
}

void SystemChrome::RestoreForExit()
{
    if (exiting_) return;
    exiting_ = true;
    SetTaskbarHidden(false);
    SetBannersHidden(false);
}

void SystemChrome::SetTaskbarHidden(bool hidden)
{
    if (hidden && exiting_) return;
    if (hidden == taskbarHidden_)
        return;
    taskbarHidden_ = hidden;
    if (hidden) {
        // Auto-hide first: a hidden taskbar that is not auto-hide would still
        // keep its strip of the work area.
        if (!AutoHide()) {
            SetAutoHide(true);
            autoHideOurs_ = true;
            RefitMaximizedLater();
        }
        Save();
        HideTaskbars();
        Watch();
    } else {
        if (hook_)
            UnhookWinEvent(hook_);
        hook_ = nullptr;
        hookedPid_ = 0;
        ShowTaskbars();
        if (autoHideOurs_)
            SetAutoHide(false);
        autoHideOurs_ = false;
        Save();
    }
    Log(L"chrome: system taskbar %s", hidden ? L"hidden" : L"shown");
}

void SystemChrome::SetBannersHidden(bool hidden)
{
    if (hidden && exiting_) return;
    if (hidden == quietOurs_)
        return;
    if (hidden) {
        std::wstring now = QuietProfile();
        if (now.empty()) {
            Log(L"chrome: Do Not Disturb is not available; banners stay");
            return;
        }
        if (now != kUnrestricted) {
            Log(L"chrome: Do Not Disturb is on already (%s); left as it is", now.c_str());
            return; // the user's own: not ours to turn off later
        }
        quietOurs_ = SetQuietProfile(kQuiet);
        Log(L"chrome: Do Not Disturb %s", quietOurs_ ? L"on (banners hidden)" : L"could not be turned on");
    } else {
        if (QuietProfile() == kQuiet)
            SetQuietProfile(kUnrestricted);
        quietOurs_ = false;
        Log(L"chrome: Do Not Disturb off (banners shown)");
    }
    Save();
}

void SystemChrome::Enforce()
{
    if (exiting_) return;
    if (taskbarHidden_) {
        // Hidden but not auto-hide, the taskbar keeps its strip of the work area and maximized
        // windows stop its height short of the bottom. Auto-hide can be lost after it was set:
        // set before Explorer's taskbar was ready at sign-in, Explorer restarted, another program.
        if (FindWindowW(L"Shell_TrayWnd", nullptr) && !AutoHide()) {
            SetAutoHide(true);
            autoHideOurs_ = true;
            Save();
            Log(L"chrome: the taskbar was no longer auto-hide; set again");
            RefitMaximizedLater();
        }
        HideTaskbars();
        Watch(); // Explorer may have been restarted
    }
    if (quietOurs_ && QuietProfile() != kQuiet) {
        // The user turned it off (or chose another mode): theirs from now on.
        Log(L"chrome: Do Not Disturb changed by the user; no longer Yunzhao's");
        quietOurs_ = false;
        Save();
    }
}

void SystemChrome::EmergencyRestore()
{
    if (!g_chrome || !g_chrome->taskbarHidden_.exchange(false))
        return;
    // Stop OnShow/Enforce hiding Explorer again before making it visible.
    // The exception can be on a worker, so these flags are atomic. Do not
    // log or save here; system.json still restores Do Not Disturb next start.
    if (g_chrome->autoHideOurs_.exchange(false))
        SetAutoHide(false);
    ShowTaskbars();
}

void SystemChrome::RehideTaskbars()
{
    if (exiting_ || !taskbarHidden_)
        return;
    HideTaskbars();
}

void SystemChrome::HideTaskbars()
{
    for (HWND w : Taskbars()) {
        if (!IsWindow(w))
            continue;
        RECT r{};
        HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowRect(w, &r) || !GetMonitorInfoW(mon, &mi)) {
            ShowWindow(w, SW_HIDE);
            continue;
        }
        // Windows 10 slides an auto-hidden taskbar back on screen when the pointer
        // hits the edge, and that slide often never sends a show event. If any of
        // it is on the monitor, park it just past the far edge and hide it.
        const bool onScreen = r.bottom > mi.rcMonitor.top && r.top < mi.rcMonitor.bottom && r.right > mi.rcMonitor.left &&
                              r.left < mi.rcMonitor.right;
        if (!onScreen)
            continue;
        const int width = std::max(1L, r.right - r.left);
        const int height = std::max(1L, r.bottom - r.top);
        // Keep the window alive and off the monitor. Hiding it removes the notification
        // icons with it, so a program closed to the tray has nowhere to go.
        SetWindowPos(w, HWND_BOTTOM, r.left, mi.rcMonitor.bottom + height, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    }
}

void SystemChrome::Watch()
{
    // Explorer shows its taskbar again by itself now and then: hide it the
    // moment it does.
    DWORD pid = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
    if (!pid || pid == hookedPid_)
        return;
    if (hook_)
        UnhookWinEvent(hook_);
    // SHOW only. LOCATIONCHANGE fires for every pixel of Explorer's slide and
    // SetWindowPos on the system taskbar then stalls the whole desktop.
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, pid, 0, WINEVENT_OUTOFCONTEXT);
    hookedPid_ = hook_ ? pid : 0;
}

void CALLBACK SystemChrome::OnShow(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD)
{
    if (obj == OBJID_WINDOW && g_chrome && g_chrome->taskbarHidden_ && w && IsTaskbar(w))
        g_chrome->HideTaskbars();
}

void SystemChrome::Save() const
{
    if (!taskbarHidden_ && !quietOurs_) {
        DeleteFileW(StatePath().c_str());
        return;
    }
    std::wstring json = L"{\"taskbarHidden\":";
    json += taskbarHidden_ ? L"true" : L"false";
    json += L",\"autoHide\":";
    json += autoHideOurs_ ? L"true" : L"false";
    json += L",\"quiet\":";
    json += quietOurs_ ? L"true" : L"false";
    json += L"}";
    WriteTextFile(StatePath(), json);
}

} // namespace yunzhao
