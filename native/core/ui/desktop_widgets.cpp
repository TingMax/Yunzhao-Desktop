#include "account.h"
#include "ui/desktop_widgets.h"

#include "clock.h"
#include "log.h"
#include "os.h"
#include "motion.h"
#include "settings.h"
#include "shell/displays.h"

#include <commctrl.h>

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#pragma comment(lib, "dwmapi.lib")

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr float kMargin = 24;
constexpr float kGrip = 30; // DIP: the corner that resizes a widget when dragged
constexpr float kCalMinW = 240, kCalMinH = 180;
constexpr float kCalDefaultW = 840, kCalDefaultH = 560;
constexpr float kCalSmallW = 420, kCalSmallH = 320, kCalMediumW = 640, kCalMediumH = 460, kCalLargeW = 920, kCalLargeH = 640;

void StepDays(int& y, int& m, int& d, int delta)
{
    int step = delta < 0 ? -1 : 1;
    int n = delta < 0 ? -delta : delta;
    for (int i = 0; i < n; ++i) {
        d += step;
        if (d > DaysIn(y, m)) {
            d = 1;
            if (++m > 12) {
                m = 1;
                ++y;
            }
        } else if (d < 1) {
            if (--m < 1) {
                m = 12;
                --y;
            }
            d = DaysIn(y, m);
        }
    }
}

void StoreFocus(WidgetContent& content, int y, int m, int d)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (y == int(now.wYear) && m == int(now.wMonth) && d == int(now.wDay)) {
        content.shownYear = 0;
        content.shownMonth = 0;
        content.focusDay = 0;
        return;
    }
    content.shownYear = y;
    content.shownMonth = m;
    content.focusDay = d;
}

// Day view steps one day, week view seven, month view one month. Zero jumps to today.
void ShiftShownMonth(WidgetContent& content, int delta)
{
    if (delta == 0) {
        StoreFocus(content, 0, 0, 0);
        content.shownYear = 0;
        content.shownMonth = 0;
        content.focusDay = 0;
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int y = 0, m = 0, d = 0;
    CalendarFocus(content, now, y, m, d);
    if (content.calBoard == 2 || content.calView == 1)
        StepDays(y, m, d, delta * 7);
    else if (content.calView == 0)
        StepDays(y, m, d, delta);
    else {
        m += delta;
        while (m < 1) {
            m += 12;
            --y;
        }
        while (m > 12) {
            m -= 12;
            ++y;
        }
        int dim = DaysIn(y, m);
        if (d > dim)
            d = dim;
    }
    StoreFocus(content, y, m, d);
}

HWND DesktopListView()
{
    if (HWND progman = FindWindowW(L"Progman", nullptr))
        if (HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
            if (HWND list = FindWindowExW(def, nullptr, L"SysListView32", nullptr))
                return list;
    HWND found = nullptr;
    EnumWindows(
        [](HWND top, LPARAM lp) -> BOOL {
            wchar_t cls[32] = {};
            GetClassNameW(top, cls, 32);
            if (wcscmp(cls, L"WorkerW") != 0)
                return TRUE;
            HWND def = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
            HWND list = def ? FindWindowExW(def, nullptr, L"SysListView32", nullptr) : nullptr;
            if (!list)
                return TRUE;
            *reinterpret_cast<HWND*>(lp) = list;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// Icon bounds in screen pixels, on every monitor. Empty when Explorer will not answer.
std::vector<RECT> DesktopIconRects()
{
    HWND list = DesktopListView();
    if (!list)
        return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(list, &pid);
    HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    struct Guard {
        HANDLE process;
        void* remote;
        ~Guard()
        {
            if (remote)
                VirtualFreeEx(process, remote, 0, MEM_RELEASE);
            if (process)
                CloseHandle(process);
        }
    } guard{process, VirtualAllocEx(process, nullptr, sizeof(RECT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
    if (!guard.remote)
        return {};
    DWORD_PTR countMsg = 0;
    if (!SendMessageTimeoutW(list, LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 80, &countMsg))
        return {};
    int count = int(countMsg);
    if (count <= 0 || count > 2000)
        return {};
    std::vector<RECT> icons;
    icons.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        RECT local{LVIR_BOUNDS, 0, 0, 0};
        if (!WriteProcessMemory(process, guard.remote, &local, sizeof(local), nullptr))
            continue;
        DWORD_PTR ok = 0;
        if (!SendMessageTimeoutW(list, LVM_GETITEMRECT, WPARAM(i), LPARAM(guard.remote), SMTO_ABORTIFHUNG, 40, &ok) || !ok)
            continue;
        RECT got{};
        if (!ReadProcessMemory(process, guard.remote, &got, sizeof(got), nullptr))
            continue;
        if (got.right <= got.left || got.bottom <= got.top)
            continue;
        MapWindowPoints(list, nullptr, reinterpret_cast<POINT*>(&got), 2);
        InflateRect(&got, 8, 6);
        icons.push_back(got);
    }
    return icons;
}
constexpr double kGlideMs = 240;
constexpr UINT_PTR kTickTimer = 1, kGlideTimer = 3, kSafetyTimer = 4, kCaretTimer = 5, kSaveTimer = 6, kPopupTimer = 8, kDisplayTimer = 9, kLockTimer = 10, kToolTimer = 11, kPeekTimer = 12;
constexpr int kSnapPx = 18, kWidgetGapPx = 12;
constexpr UINT kCaretMs = 530, kSaveMs = 800;
constexpr ULONGLONG kPhotoMs = 10 * 60 * 1000; // a new photo every ten minutes
constexpr const wchar_t* kHostClass = L"AtriumWidgetsHost";
constexpr UINT WM_WIDGET_REMOVE = WM_APP + 1;  // lParam Widget*: removed once its own message is done
constexpr UINT WM_WIDGET_RESTACK = WM_APP + 2; // a burst of window events, one restack
constexpr UINT WM_WIDGET_PHOTO = WM_APP + 3;   // lParam PhotoRead*: a photo was read
constexpr UINT WM_WIDGET_WEATHER = WM_APP + 4; // a new forecast (WeatherSource::Latest)
constexpr UINT WM_WIDGET_BLUETOOTH = WM_APP + 5; // the Bluetooth batteries changed (BluetoothBatteries::Latest)
constexpr UINT WM_WIDGET_OUTSIDE = WM_APP + 6;    // a click landed outside a pinned todo

HHOOK g_outsideHook = nullptr;
HWND g_outsideHost = nullptr;

LRESULT CALLBACK OutsideHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_outsideHost && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_NCLBUTTONDOWN))
        PostMessageW(g_outsideHost, WM_WIDGET_OUTSIDE, 0, 0);
    return CallNextHookEx(g_outsideHook, code, wp, lp);
}

DesktopWidgets* g_widgets = nullptr; // the WinEvent hook's way back

struct KindName {
    WidgetKind kind;
    const wchar_t* name;
};
const KindName kKinds[] = {{WidgetKind::Clock, L"clock"},     {WidgetKind::Calendar, L"calendar"},   {WidgetKind::Battery, L"battery"},
                           {WidgetKind::Digital, L"digital"}, {WidgetKind::System, L"system"},       {WidgetKind::Notes, L"notes"},
                           {WidgetKind::Reminders, L"reminders"}, {WidgetKind::Photos, L"photos"},  {WidgetKind::Weather, L"weather"},
                           {WidgetKind::Bluetooth, L"bluetooth"}, {WidgetKind::Schedule, L"schedule"}, {WidgetKind::Marks, L"marks"},
                           {WidgetKind::Focus, L"focus"}, {WidgetKind::Habits, L"habits"}};

const wchar_t* NameOf(WidgetKind k)
{
    for (const KindName& n : kKinds)
        if (n.kind == k)
            return n.name;
    return L"clock";
}
bool ParseKind(const std::wstring& s, WidgetKind& k)
{
    for (const KindName& n : kKinds)
        if (s == n.name) {
            k = n.kind;
            return true;
        }
    return false;
}
const wchar_t* SizeName(WidgetSize s) { return s == WidgetSize::Small ? L"small" : s == WidgetSize::Medium ? L"medium" : L"large"; }
WidgetSize ParseSize(const std::wstring& s) { return s == L"large" ? WidgetSize::Large : s == L"medium" ? WidgetSize::Medium : WidgetSize::Small; }
bool Editable(WidgetKind k) { return k == WidgetKind::Notes || k == WidgetKind::Reminders; }
bool Resizable(WidgetKind k) { return WidgetSupports(k, WidgetSize::Medium) || WidgetSupports(k, WidgetSize::Large); }

std::wstring LayoutPath()
{
    return AccountDataDir() + L"\\widgets.json";
}

std::wstring ClassOf(HWND h)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return cls;
}

// Without a message to the window (GetWindowTextLength would wait for a hung one).
bool HasTitle(HWND h)
{
    wchar_t t[2] = {};
    return InternalGetWindowText(h, t, 2) > 0;
}

bool IsDesktopWindow(HWND h)
{
    std::wstring c = ClassOf(h);
    return c == L"Progman" || c == L"WorkerW";
}

// The first city of the local time zone's name: "(UTC+08:00) 北京，重庆，…" -> "北京".
std::wstring LocalCity()
{
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID)
        return {};
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + std::wstring(tz.TimeZoneKeyName);
    wchar_t display[256] = {};
    DWORD size = sizeof(display);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Display", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS)
        return {};
    std::wstring d = display;
    size_t open = d.find(L") ");
    if (open != std::wstring::npos)
        d = d.substr(open + 2);
    size_t end = d.find_first_of(L"，,");
    return end == std::wstring::npos ? d : d.substr(0, end);
}

std::wstring PicturesFolder()
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

// The pictures of a folder and its subfolders (two levels, a few thousand at most).
void ListPhotos(const std::wstring& dir, std::vector<std::wstring>& out, int depth = 0)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2)
                ListPhotos(dir + L"\\" + name, out, depth + 1);
            continue;
        }
        size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos)
            continue;
        std::wstring ext = name.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        for (const wchar_t* e : {L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".webp", L".heic", L".tif", L".tiff"})
            if (ext == e) {
                out.push_back(dir + L"\\" + name);
                break;
            }
    } while (out.size() < 5000 && FindNextFileW(h, &fd));
    FindClose(h);
}

// A photo cropped to fill w x h px (centred) and turned upright, premultiplied BGRA.
IconImage ReadPhoto(const std::wstring& path, UINT w, UINT h)
{
    IconImage out;
    winrt::com_ptr<IWICImagingFactory> f;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))) ||
        FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return out;
    winrt::com_ptr<IWICBitmapSource> src = frame;
    // The camera's orientation (EXIF): phone pictures are often stored sideways.
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;
    winrt::com_ptr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(meta.put()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &v)) && v.vt == VT_UI2)
            turn = v.uiVal == 3   ? WICBitmapTransformRotate180
                   : v.uiVal == 6 ? WICBitmapTransformRotate90
                   : v.uiVal == 8 ? WICBitmapTransformRotate270
                                  : WICBitmapTransformRotate0;
        PropVariantClear(&v);
    }
    if (turn != WICBitmapTransformRotate0) {
        winrt::com_ptr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(f->CreateBitmapFlipRotator(rot.put())) && SUCCEEDED(rot->Initialize(src.get(), turn)))
            src = rot;
    }
    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || !sw || !sh)
        return out;
    double k = std::max(double(w) / sw, double(h) / sh);
    UINT cw = std::min(sw, UINT(std::lround(w / k))), ch = std::min(sh, UINT(std::lround(h / k)));
    WICRect crop{INT((sw - cw) / 2), INT((sh - ch) / 2), INT(cw), INT(ch)};
    winrt::com_ptr<IWICBitmapClipper> clip;
    winrt::com_ptr<IWICBitmapScaler> scale;
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(f->CreateBitmapClipper(clip.put())) || FAILED(clip->Initialize(src.get(), &crop)) || FAILED(f->CreateBitmapScaler(scale.put())) ||
        FAILED(scale->Initialize(clip.get(), w, h, WICBitmapInterpolationModeHighQualityCubic)) || FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scale.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return out;
    out.bgra.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()))) {
        out.bgra.clear();
        return out;
    }
    out.width = int(w);
    out.height = int(h);
    return out;
}

std::wstring TopHitId(const std::vector<WidgetHit>& hits, float x, float y)
{
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (x >= it->rect.left && x < it->rect.right && y >= it->rect.top && y < it->rect.bottom)
            return it->id;
    return {};
}

void ShiftDate(int& y, int& m, int& d, int delta)
{
    SYSTEMTIME st{};
    st.wYear = WORD(y);
    st.wMonth = WORD(m);
    st.wDay = WORD(std::max(1, d));
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft))
        return;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const LONGLONG step = 24LL * 60 * 60 * 10000000;
    if (delta < 0 && u.QuadPart < ULONGLONG(-delta) * ULONGLONG(step))
        return;
    u.QuadPart += LONGLONG(delta) * step;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    if (!FileTimeToSystemTime(&ft, &st))
        return;
    y = st.wYear;
    m = st.wMonth;
    d = st.wDay;
}

} // namespace

struct DesktopWidgets::Widget {
    WidgetKind kind = WidgetKind::Clock;
    WidgetSize size = WidgetSize::Small;
    int col = 0, row = 0;
    std::unique_ptr<Surface> surface;
    bool down = false, dragging = false;
    bool hover = false;    // the mouse is over it: the resize grip shows
    bool tracking = false; // TrackMouseEvent asked for its WM_MOUSELEAVE
    bool resizing = false; // dragged by the grip
    bool growLeft = false; // the selected grip is at the bottom left
    int edges = 0;          // calendar: 1 left, 2 right, 4 top, 8 bottom
    ULONGLONG pomoEnd = 0;  // when a running tomato reaches zero
    WidgetSize downSize = WidgetSize::Small;
    int downCol = 0;
    POINT downAt{};
    RECT downRect{};
    RECT glideFrom{}, glideTo{};
    double glideMs = 0;
    WidgetContent content;
    float spanW = 0, spanH = 0;  // calendar: its free size in DIP
    LONG originX = LONG_MIN, originY = LONG_MIN; // screen px; LONG_MIN until the grid place is copied once
    std::wstring hoverDay;       // "day:YYYY-MM-DD" under the pointer
    bool zooming = false; // the calendar's size slider
    bool pulling = false; // dragging the list/timetable divider up or down
    ULONGLONG lockHoldStart = 0; // holding the lock to unlock it
    bool creatorHold = false;    // the timetable is showing the note creator, so the page stays visible
    int noteAsk = 0;              // bumps when 新建待办 asks the calendar page to open 新建便签
    bool tucked = false;          // pulled up to a strip at the top edge
    int tuckShown = 0;            // visible height while it slides; 0 means the full widget
    double tuckWait = 0;          // when the pointer left, before it slides up
    double popupLeaveMs = 0; // when the pointer left an open todo; 0 while it is over the popup
    double clickMs = 0;
    double prevClickMs = 0;
    POINT clickPt{};
    POINT prevClickPt{};
    std::wstring clickHead; // the timetable header of the previous click
    bool clickTodo = false; // the last click opened one todo, so a second click can open the day
    std::vector<WidgetHit> hits; // of the last paint
    ULONGLONG photoAt = 0;       // when the photo was last changed
    unsigned photoGen = 0;       // the photo being read
};

namespace {
struct PhotoRead {
    void* widget;
    unsigned gen;
    std::wstring path;
    IconImage image;
};
} // namespace

DesktopWidgets::DesktopWidgets(HINSTANCE instance) : instance_(instance)
{
    g_widgets = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: timers, display changes, the menu's owner.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Yunzhao Widgets", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    world_.city = LocalCity();
    world_.lunar = &lunar_;
    weather_ = std::make_unique<WeatherSource>(host_, WM_WIDGET_WEATHER);
    world_.weather = weather_->Latest(); // the last one, until the first fetch
    bluetooth_ = std::make_unique<BluetoothBatteries>(host_, WM_WIDGET_BLUETOOTH);
}

void DesktopWidgets::SetCalendarType(float datePx, float tablePx)
{
    datePx = std::clamp(datePx, 12.f, 36.f);
    tablePx = std::clamp(tablePx, 16.f, 48.f);
    if (datePx == world_.dateSize && tablePx == world_.tableSize)
        return;
    world_.dateSize = datePx;
    world_.tableSize = tablePx;
    for (auto& w : widgets_)
        if (w->kind == WidgetKind::Calendar && w->surface) {
            w->surface->Invalidate();
            PublishYunzhaoState(*w);
        }
}

void DesktopWidgets::SetTimetableBadge(float scale)
{
    scale = std::clamp(scale, 0.4f, 1.6f);
    if (scale == world_.badgeScale)
        return;
    world_.badgeScale = scale;
    for (auto& w : widgets_)
        if (w->kind == WidgetKind::Calendar && w->surface) {
            w->surface->Invalidate();
            PublishYunzhaoState(*w);
        }
}

void DesktopWidgets::SetMaterial(WidgetMaterial material)
{
    if (material == material_)
        return;
    material_ = material;
    for (auto& w : widgets_)
        if (w->surface) {
            w->surface->SetGlassless(material_ == WidgetMaterial::Color);
            w->surface->Invalidate();
        }
}

DesktopWidgets::~DesktopWidgets()
{
    ClosePomo();
    if (g_outsideHook) {
        UnhookWindowsHookEx(g_outsideHook);
        g_outsideHook = nullptr;
        g_outsideHost = nullptr;
    }
    SetEnabled(false);
    if (host_)
        DestroyWindow(host_);
    g_widgets = nullptr;
}

void DesktopWidgets::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        if (onStatusRequest)
            onStatusRequest();
        SampleSystem();
        Load();
        Build();
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0, flags);
        // A window that appears without taking the foreground, or uncloaks
        // after it did (WinUI), must also send raised widgets back down.
        showHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnWinEvent, 0, 0, flags);
        uncloakHook_ = SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnWinEvent, 0, 0, flags);
        SetTimer(host_, kTickTimer, 1000, nullptr);
        SetTimer(host_, kSafetyTimer, 3000, nullptr);
        SetTimer(host_, kPeekTimer, 60, nullptr);
        Restack();
        Log(L"widgets: on (%zu)", widgets_.size());
    } else {
        for (HWINEVENTHOOK* h : {&hook_, &showHook_, &uncloakHook_}) {
            if (*h)
                UnhookWinEvent(*h);
            *h = nullptr;
        }
        KillTimer(host_, kTickTimer);
        KillTimer(host_, kSafetyTimer);
        KillTimer(host_, kPeekTimer);
        weather_->SetActive(false);
        bluetooth_->SetActive(false);
        KillTimer(host_, kCaretTimer);
        if (KillTimer(host_, kSaveTimer))
            Save(); // typing not saved yet
        Clear();
        Log(L"widgets: off");
    }
}

void DesktopWidgets::SetMonitor(HMONITOR monitor, bool refresh)
{
    if (monitor == monitor_ && !refresh)
        return;
    monitor_ = monitor;
    if (enabled_)
        for (auto& w : widgets_)
            Place(*w, false);
}

// ---- layout ------------------------------------------------------------------

void DesktopWidgets::SetStatus(const SystemStatus& status)
{
    bool batteryChanged = world_.status.battery != status.battery || world_.status.batteryPercent != status.batteryPercent ||
                          world_.status.charging != status.charging;
    world_.status = status;
    if (batteryChanged)
        for (auto& widget : widgets_)
            if (widget->kind == WidgetKind::Battery && widget->surface)
                widget->surface->Invalidate();
}

void DesktopWidgets::SetOnRight(bool onRight)
{
    if (onRight == onRight_)
        return;
    onRight_ = onRight;
    if (enabled_)
        for (auto& w : widgets_)
            Place(*w, true);
}

RECT DesktopWidgets::WorkArea() const
{
    MONITORINFO mi{sizeof(mi)};
    HMONITOR m = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(m, &mi);
    return mi.rcWork;
}

float DesktopWidgets::Scale() const { return PopupScale(WorkArea()); }

int DesktopWidgets::Cols() const
{
    RECT wa = WorkArea();
    return std::max(1, int((float(wa.right - wa.left) / Scale() - 2 * kMargin + kWidgetGap) / (kWidgetCell + kWidgetGap)));
}

int DesktopWidgets::Rows() const
{
    RECT wa = WorkArea();
    // The bar floats over the bottom of the work area: leave it a row's worth of room.
    return std::max(1, int((float(wa.bottom - wa.top) / Scale() - 2 * kMargin - 90 + kWidgetGap) / (kWidgetCell + kWidgetGap)));
}

// Each half of the grid hugs its own edge (D44): a widget whose middle is left of the grid's
// middle stands kMargin from the left edge, any other kMargin from the right, so whatever the
// grid does not fill is left in the middle and both sides have the same margin.
int DesktopWidgets::VisualCol(int col, WidgetSize size) const { return onRight_ ? Cols() - col - WidgetCols(size) : col; }

bool DesktopWidgets::FromRight(int col, WidgetSize size) const { return 2 * VisualCol(col, size) + WidgetCols(size) >= Cols(); }

RECT DesktopWidgets::CellRect(int col, int row, WidgetSize size) const
{
    RECT wa = WorkArea();
    float s = Scale();
    const float step = kWidgetCell + kWidgetGap;
    const int visualCol = VisualCol(col, size);
    float left = FromRight(col, size) ? float(wa.right - wa.left) / s - kMargin - float(Cols() - visualCol - WidgetCols(size)) * step - WidgetWidth(size)
                                      : kMargin + float(visualCol) * step;
    int x = wa.left + int(std::lround(left * s));
    int y = wa.top + int(std::lround((kMargin + float(row) * (kWidgetCell + kWidgetGap)) * s));
    return RECT{x, y, x + int(std::lround(WidgetWidth(size) * s)), y + int(std::lround(WidgetHeight(size) * s))};
}

RECT DesktopWidgets::LegacyRect(const Widget& w) const
{
    RECT r = CellRect(w.col, w.row, w.size);
    if (w.kind != WidgetKind::Calendar || w.spanW < kCalMinW || w.spanH < kCalMinH)
        return r;
    float s = Scale();
    int width = std::max(1, int(std::lround(w.spanW * s)));
    int height = std::max(1, int(std::lround(w.spanH * s)));
    if (FromRight(w.col, w.size))
        r.left = r.right - width;
    else
        r.right = r.left + width;
    r.bottom = r.top + height;
    return r;
}

RECT DesktopWidgets::PixelRect(const Widget& w) const
{
    RECT probe = w.originX != LONG_MIN ? RECT{w.originX, w.originY, w.originX + 20, w.originY + 20} : WorkArea();
    float s = std::max(PopupScale(probe), 0.01f);
    int width = 0, height = 0;
    if (w.kind == WidgetKind::Calendar && w.spanW >= kCalMinW) {
        width = std::max(1, int(std::lround(w.spanW * s)));
        height = std::max(1, int(std::lround(w.spanH * s)));
    } else {
        width = std::max(1, int(std::lround(WidgetWidth(w.size) * s)));
        height = std::max(1, int(std::lround(WidgetHeight(w.size) * s)));
    }
    LONG x = w.originX != LONG_MIN ? w.originX : 0;
    LONG y = w.originY != LONG_MIN ? w.originY : 0;
    return RECT{x, y, x + width, y + height};
}

RECT DesktopWidgets::PlacedRect(const Widget& w) const
{
    if (w.originX == LONG_MIN)
        return LegacyRect(w);
    return PixelRect(w);
}

void DesktopWidgets::RefreshIcons()
{
    double now = ClockMs();
    if (now - iconsMs_ < 1500)
        return;
    icons_ = DesktopIconRects();
    iconsMs_ = now;
}

std::vector<RECT> DesktopWidgets::Blockers(const Widget* except) const
{
    std::vector<RECT> out = icons_;
    for (const auto& other : widgets_) {
        if (other.get() == except || (!other->surface && other->originX == LONG_MIN))
            continue;
        RECT r = other->surface && other->surface->Visible() ? other->surface->Rect() : PixelRect(*other);
        if (IsRectEmpty(&r))
            continue;
        InflateRect(&r, kWidgetGapPx / 2, kWidgetGapPx / 2);
        out.push_back(r);
    }
    return out;
}

bool DesktopWidgets::Blocked(const RECT& rect, const Widget* except) const
{
    RECT hit{};
    for (const RECT& obstacle : Blockers(except))
        if (IntersectRect(&hit, &rect, &obstacle))
            return true;
    return false;
}

RECT ClampToWork(RECT rect, const RECT& wa, bool allowShrink)
{
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width >= wa.right - wa.left) {
        rect.left = wa.left;
        rect.right = allowShrink ? wa.right : wa.left + width;
    } else if (rect.left < wa.left) {
        rect.right += wa.left - rect.left;
        rect.left = wa.left;
    } else if (rect.right > wa.right) {
        rect.left -= rect.right - wa.right;
        rect.right = wa.right;
    }
    if (height >= wa.bottom - wa.top) {
        rect.top = wa.top;
        rect.bottom = allowShrink ? wa.bottom : wa.top + height;
    } else if (rect.top < wa.top) {
        rect.bottom += wa.top - rect.top;
        rect.top = wa.top;
    } else if (rect.bottom > wa.bottom) {
        rect.top -= rect.bottom - wa.bottom;
        rect.bottom = wa.bottom;
    }
    return rect;
}

RECT DesktopWidgets::ClampMonitor(RECT rect, bool allowShrink) const
{
    POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return rect;
    return ClampToWork(rect, info.rcWork, allowShrink);
}

RECT DesktopWidgets::ClampOntoMonitor(RECT rect) const
{
    POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return rect;
    return ClampToWork(rect, info.rcMonitor, false);
}

RECT DesktopWidgets::ClampVirtual(RECT rect) const
{
    RECT span{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (const DisplayInfo& display : ConnectedDisplays()) {
        span.left = std::min(span.left, display.work.left);
        span.top = std::min(span.top, display.work.top);
        span.right = std::max(span.right, display.work.right);
        span.bottom = std::max(span.bottom, display.work.bottom);
    }
    if (span.left >= span.right || span.top >= span.bottom)
        return rect;
    RECT hit{};
    // A disconnected display drops out of this span. Bring the widget onto the
    // nearest display that is still connected instead of leaving it off-screen.
    if (!IntersectRect(&hit, &rect, &span))
        return ClampMonitor(rect, false);
    return ClampToWork(rect, span, false);
}

RECT DesktopWidgets::SnapToMonitor(RECT rect, bool allowShrink) const
{
    const std::vector<DisplayInfo> displays = ConnectedDisplays();
    const DisplayInfo* best = nullptr;
    LONG bestArea = -1;
    for (const DisplayInfo& display : displays) {
        RECT hit{};
        if (!IntersectRect(&hit, &rect, &display.work))
            continue;
        LONG area = (hit.right - hit.left) * (hit.bottom - hit.top);
        if (area > bestArea) {
            bestArea = area;
            best = &display;
        }
    }
    if (!best)
        return ClampMonitor(rect, allowShrink);
    return ClampToWork(rect, best->work, allowShrink);
}

RECT DesktopWidgets::Nudge(RECT desired, const Widget* self) const
{
    const_cast<DesktopWidgets*>(this)->RefreshIcons();
    int bestDx = 0, bestDy = 0, absX = kSnapPx + 1, absY = kSnapPx + 1;
    for (const auto& other : widgets_) {
        if (other.get() == self)
            continue;
        RECT o = other->surface && other->surface->Visible() ? other->surface->Rect() : PixelRect(*other);
        if (IsRectEmpty(&o))
            continue;
        bool yNear = desired.bottom >= o.top - 48 && desired.top <= o.bottom + 48;
        bool xNear = desired.right >= o.left - 48 && desired.left <= o.right + 48;
        if (yNear) {
            int dxs[] = {o.right + kWidgetGapPx - desired.left, o.left - kWidgetGapPx - desired.right, o.left - desired.left, o.right - desired.right};
            for (int d : dxs)
                if (std::abs(d) <= kSnapPx && std::abs(d) < absX) {
                    absX = std::abs(d);
                    bestDx = d;
                }
        }
        if (xNear) {
            int dys[] = {o.bottom + kWidgetGapPx - desired.top, o.top - kWidgetGapPx - desired.bottom, o.top - desired.top, o.bottom - desired.bottom};
            for (int d : dys)
                if (std::abs(d) <= kSnapPx && std::abs(d) < absY) {
                    absY = std::abs(d);
                    bestDy = d;
                }
        }
    }
    if (absX <= kSnapPx) {
        desired.left += bestDx;
        desired.right += bestDx;
    }
    if (absY <= kSnapPx) {
        desired.top += bestDy;
        desired.bottom += bestDy;
    }
    desired = ClampVirtual(desired);
    for (int pass = 0; pass < 12; ++pass) {
        bool hit = false;
        for (const RECT& obstacle : Blockers(self)) {
            RECT overlap{};
            if (!IntersectRect(&overlap, &desired, &obstacle))
                continue;
            hit = true;
            int left = desired.right - obstacle.left;
            int right = obstacle.right - desired.left;
            int up = desired.bottom - obstacle.top;
            int down = obstacle.bottom - desired.top;
            int least = std::min({left, right, up, down});
            if (least == left) {
                desired.left -= left;
                desired.right -= left;
            } else if (least == right) {
                desired.left += right;
                desired.right += right;
            } else if (least == up) {
                desired.top -= up;
                desired.bottom -= up;
            } else {
                desired.top += down;
                desired.bottom += down;
            }
            break;
        }
        if (!hit)
            break;
    }
    return ClampVirtual(desired);
}

RECT DesktopWidgets::Settle(RECT desired, const Widget* self) const
{
    const bool shrink = self && self->kind == WidgetKind::Calendar;
    desired = SnapToMonitor(Nudge(desired, self), shrink);
    if (!Blocked(desired, self))
        return desired;
    const int width = desired.right - desired.left, height = desired.bottom - desired.top;
    for (int ring = 1; ring <= 14; ++ring) {
        const int step = ring * 24;
        const POINT hops[] = {{step, 0}, {-step, 0}, {0, step}, {0, -step}, {step, step}, {-step, step}, {step, -step}, {-step, -step}};
        for (POINT hop : hops) {
            RECT trial{desired.left + hop.x, desired.top + hop.y, desired.left + hop.x + width, desired.top + hop.y + height};
            trial = SnapToMonitor(trial, false);
            trial.right = trial.left + width;
            trial.bottom = trial.top + height;
            trial = SnapToMonitor(trial, false);
            if (!Blocked(trial, self))
                return trial;
        }
    }
    return desired;
}

RECT DesktopWidgets::FindSpot(int width, int height) const
{
    const_cast<DesktopWidgets*>(this)->RefreshIcons();
    std::vector<DisplayInfo> displays = ConnectedDisplays();
    if (displays.empty()) {
        RECT wa = WorkArea();
        displays.push_back({nullptr, L"", wa, wa, true});
    }
    for (const DisplayInfo& display : displays) {
        const RECT& wa = display.work;
        for (int y = wa.top + 16; y + height <= wa.bottom - 8; y += 28)
            for (int x = wa.left + 16; x + width <= wa.right - 8; x += 28) {
                RECT trial{x, y, x + width, y + height};
                if (!Blocked(trial, nullptr))
                    return trial;
            }
    }
    RECT wa = displays.front().work;
    return ClampMonitor(RECT{wa.left + 16, wa.top + 16, wa.left + 16 + width, wa.top + 16 + height}, true);
}

void DesktopWidgets::Remember(Widget& w, const RECT& settled)
{
    w.originX = settled.left;
    w.originY = settled.top;
    if (w.kind == WidgetKind::Calendar) {
        float s = std::max(PopupScale(settled), 0.01f);
        w.spanW = std::max(kCalMinW, float(settled.right - settled.left) / s);
        w.spanH = std::max(kCalMinH, float(settled.bottom - settled.top) / s);
    }
}

bool DesktopWidgets::Fits(int col, int row, WidgetSize size, const Widget* except) const
{
    if (col < 0 || row < 0 || col + WidgetCols(size) > Cols() || row + WidgetRows(size) > Rows())
        return false;
    for (const auto& w : widgets_) {
        if (w.get() == except)
            continue;
        bool apart = col + WidgetCols(size) <= w->col || w->col + WidgetCols(w->size) <= col || row + WidgetRows(size) <= w->row ||
                     w->row + WidgetRows(w->size) <= row;
        if (!apart)
            return false;
    }
    return true;
}

bool DesktopWidgets::FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const
{
    float best = 1e9f;
    for (int r = 0; r < Rows(); ++r)
        for (int c = 0; c < Cols(); ++c) {
            if (!Fits(c, r, size, except))
                continue;
            float d = (float(c) - col) * (float(c) - col) + (float(r) - row) * (float(r) - row);
            if (d < best) {
                best = d;
                outCol = c;
                outRow = r;
            }
        }
    return best < 1e9f;
}

void DesktopWidgets::Load()
{
    widgets_.clear();
    unplaced_.clear();
    std::wstring text = ReadTextFile(LayoutPath());
    bool read = false;
    auto add = [&](WidgetKind k, WidgetSize s, int c, int r, WidgetContent content) {
        if (!WidgetSupports(k, s))
            s = WidgetSize::Small;
        if (!Fits(c, r, s, nullptr) && !FreeCellNear(float(c), float(r), s, nullptr, c, r))
            return false; // no room left on this screen
        Create(k, s, c, r).content = std::move(content);
        return true;
    };
    if (!text.empty()) {
        json::JsonObject o{nullptr};
        if (json::JsonObject::TryParse(text, o) && o.HasKey(L"widgets")) {
            read = true;
            std::vector<WidgetContent::Lesson> carried;
            for (const auto& v : o.GetNamedArray(L"widgets")) {
                if (v.ValueType() != json::JsonValueType::Object)
                    continue;
                json::JsonObject e = v.GetObject();
                WidgetKind k;
                if (!ParseKind(std::wstring(e.GetNamedString(L"kind", L"")), k))
                    continue;
                if (k == WidgetKind::Notes || k == WidgetKind::Reminders || k == WidgetKind::Photos)
                    continue;
                WidgetContent content;
                content.text = e.GetNamedString(L"text", L"");
                content.folder = e.GetNamedString(L"folder", L"");
                if (e.HasKey(L"cities") && e.GetNamedValue(L"cities").ValueType() == json::JsonValueType::Array)
                    for (const auto& cv : e.GetNamedArray(L"cities"))
                        if (cv.ValueType() == json::JsonValueType::String)
                            content.cities.emplace_back(cv.GetString());
                if (e.HasKey(L"items") && e.GetNamedValue(L"items").ValueType() == json::JsonValueType::Array)
                    for (const auto& iv : e.GetNamedArray(L"items")) {
                        if (iv.ValueType() != json::JsonValueType::Object)
                            continue;
                        json::JsonObject io = iv.GetObject();
                        Reminder item{std::wstring(io.GetNamedString(L"text", L"")), io.GetNamedBoolean(L"done", false),
                                      int(io.GetNamedNumber(L"year", 0)), int(io.GetNamedNumber(L"month", 0)),
                                      int(io.GetNamedNumber(L"day", 0))};
                        item.pri = std::clamp(int(io.GetNamedNumber(L"pri", 0)), 0, 3);
                        item.dueYear = int(io.GetNamedNumber(L"dueY", 0));
                        item.dueMonth = int(io.GetNamedNumber(L"dueM", 0));
                        item.dueDay = int(io.GetNamedNumber(L"dueD", 0));
                        content.items.push_back(std::move(item));
                    }
                if (e.HasKey(L"lessons") && e.GetNamedValue(L"lessons").ValueType() == json::JsonValueType::Array)
                    for (const auto& lv : e.GetNamedArray(L"lessons")) {
                        if (lv.ValueType() != json::JsonValueType::Object)
                            continue;
                        json::JsonObject lo = lv.GetObject();
                        WidgetContent::Lesson lesson;
                        lesson.weekday = std::clamp(int(lo.GetNamedNumber(L"wd", 0)), 0, 6);
                        lesson.period = int(lo.GetNamedNumber(L"period", -1));
                        lesson.startMin = std::clamp(int(lo.GetNamedNumber(L"start", 8 * 60)), 0, 24 * 60 - 1);
                        lesson.endMin = std::clamp(int(lo.GetNamedNumber(L"end", lesson.startMin + 45)), lesson.startMin, 24 * 60);
                        if (lesson.period < 0 || lesson.period > 7) {
                            int hour = lesson.startMin / 60;
                            lesson.period = hour < 13 ? std::clamp(hour - 8, 0, 4) : std::clamp(5 + hour - 13, 5, 7);
                        }
                        lesson.name = lo.GetNamedString(L"name", L"");
                        lesson.teacher = lo.GetNamedString(L"teacher", L"");
                        lesson.notes = lo.GetNamedString(L"notes", L"");
                        content.lessons.push_back(std::move(lesson));
                    }
                if (e.HasKey(L"changes") && e.GetNamedValue(L"changes").ValueType() == json::JsonValueType::Array)
                    for (const auto& cv : e.GetNamedArray(L"changes")) {
                        if (cv.ValueType() != json::JsonValueType::Object)
                            continue;
                        json::JsonObject co = cv.GetObject();
                        WidgetContent::LessonChange change;
                        change.year = int(co.GetNamedNumber(L"y", 0));
                        change.month = int(co.GetNamedNumber(L"m", 0));
                        change.day = int(co.GetNamedNumber(L"d", 0));
                        change.weekday = std::clamp(int(co.GetNamedNumber(L"wd", 0)), 0, 6);
                        change.period = std::clamp(int(co.GetNamedNumber(L"period", 0)), 0, 7);
                        change.off = co.GetNamedBoolean(L"off", false);
                        change.name = co.GetNamedString(L"name", L"");
                        change.teacher = co.GetNamedString(L"teacher", L"");
                        change.notes = co.GetNamedString(L"notes", L"");
                        if (change.year < 1 || change.month < 1 || change.day < 1)
                            continue;
                        if (!change.off && change.name.empty() && change.teacher.empty() && change.notes.empty())
                            continue;
                        content.changes.push_back(std::move(change));
                    }
                if (e.HasKey(L"marks") && e.GetNamedValue(L"marks").ValueType() == json::JsonValueType::Array)
                    for (const auto& mv : e.GetNamedArray(L"marks")) {
                        if (mv.ValueType() != json::JsonValueType::Object)
                            continue;
                        json::JsonObject mo = mv.GetObject();
                        WidgetContent::Mark mark;
                        mark.name = mo.GetNamedString(L"name", L"");
                        mark.year = int(mo.GetNamedNumber(L"y", 0));
                        mark.month = int(mo.GetNamedNumber(L"m", 0));
                        mark.day = int(mo.GetNamedNumber(L"d", 0));
                        mark.annual = mo.GetNamedBoolean(L"annual", true);
                        content.marks.push_back(std::move(mark));
                    }
                if (e.HasKey(L"habits") && e.GetNamedValue(L"habits").ValueType() == json::JsonValueType::Array)
                    for (const auto& hv : e.GetNamedArray(L"habits")) {
                        if (hv.ValueType() != json::JsonValueType::Object)
                            continue;
                        json::JsonObject ho = hv.GetObject();
                        WidgetContent::Habit habit;
                        habit.name = ho.GetNamedString(L"name", L"");
                        habit.year = int(ho.GetNamedNumber(L"y", 0));
                        habit.month = int(ho.GetNamedNumber(L"m", 0));
                        habit.day = int(ho.GetNamedNumber(L"d", 0));
                        habit.streak = std::max(0, int(ho.GetNamedNumber(L"streak", 0)));
                        content.habits.push_back(std::move(habit));
                    }
                if (e.HasKey(L"periods") && e.GetNamedValue(L"periods").ValueType() == json::JsonValueType::Array) {
                    int i = 0;
                    for (const auto& pv : e.GetNamedArray(L"periods")) {
                        if (i >= 8)
                            break;
                        if (pv.ValueType() == json::JsonValueType::Object) {
                            json::JsonObject po = pv.GetObject();
                            content.periodStart[i] = int(po.GetNamedNumber(L"s", -1));
                            content.periodEnd[i] = int(po.GetNamedNumber(L"e", -1));
                        }
                        ++i;
                    }
                }
                if (k == WidgetKind::Focus)
                    content.pomoMin = std::clamp(int(e.GetNamedNumber(L"pomo", 25)), 1, 180);
                if (k == WidgetKind::Schedule) {
                    for (auto& lesson : content.lessons)
                        if (!lesson.name.empty())
                            carried.push_back(std::move(lesson));
                    continue;
                }
                int board = k == WidgetKind::Calendar ? int(e.GetNamedNumber(L"calBoard", -1)) : 0;
                if (k == WidgetKind::Calendar) {
                    if (board < 0)
                        board = e.GetNamedNumber(L"split", 0) != 0 ? 1 : 0;
                    content.calBoard = std::clamp(board, 0, 2);
                    content.calSplit = content.calBoard == 1;
                    content.calLock = e.GetNamedBoolean(L"lock", false);
                    if (e.HasKey(L"listPane"))
                        content.listPane = std::clamp(float(e.GetNamedNumber(L"listPane", -1)), 0.18f, 0.78f);
                    else if (e.HasKey(L"todoPane"))
                        content.listPane = std::clamp(float(e.GetNamedNumber(L"todoPane", -1)), 0.18f, 0.78f);
                    if (e.HasKey(L"tablePane"))
                        content.tablePane = std::clamp(float(e.GetNamedNumber(L"tablePane", -1)), 0.18f, 0.78f);
                    content.pickColor = uint32_t(e.GetNamedNumber(L"pick", 0));
                    content.fontColor = uint32_t(e.GetNamedNumber(L"ink", 0));
                    content.ownDate = float(e.GetNamedNumber(L"datePx", 0));
                    content.ownTable = float(e.GetNamedNumber(L"tablePx", 0));
                }
                content.bgColor = uint32_t(e.GetNamedNumber(L"bg", 0));
                content.bgImage = std::wstring(e.GetNamedString(L"bgImage", L""));
                float savedW = k == WidgetKind::Calendar ? float(e.GetNamedNumber(L"w", 0)) : 0;
                float savedH = k == WidgetKind::Calendar ? float(e.GetNamedNumber(L"h", 0)) : 0;
                int view = k == WidgetKind::Calendar ? int(e.GetNamedNumber(L"view", 2)) : 2;
                bool hasOrigin = e.HasKey(L"x") && e.HasKey(L"y");
                LONG ox = hasOrigin ? LONG(std::lround(e.GetNamedNumber(L"x"))) : LONG_MIN;
                LONG oy = hasOrigin ? LONG(std::lround(e.GetNamedNumber(L"y"))) : LONG_MIN;
                if (!add(k, ParseSize(std::wstring(e.GetNamedString(L"size", L"small"))), int(e.GetNamedNumber(L"col", 0)),
                         int(e.GetNamedNumber(L"row", 0)), std::move(content)))
                    unplaced_.emplace_back(e.Stringify());
                else {
                    if (k == WidgetKind::Calendar && savedW >= kCalMinW && savedH >= kCalMinH) {
                        widgets_.back()->spanW = savedW;
                        widgets_.back()->spanH = savedH;
                    }
                    if (k == WidgetKind::Calendar && view >= 0 && view <= 2)
                        widgets_.back()->content.calView = view;
                    if (hasOrigin) {
                        widgets_.back()->originX = ox;
                        widgets_.back()->originY = oy;
                    }
                }
            }
            if (!carried.empty()) {
                for (auto& w : widgets_) {
                    if (w->kind != WidgetKind::Calendar)
                        continue;
                    w->content.lessons.insert(w->content.lessons.end(), carried.begin(), carried.end());
                    break;
                }
            }
        } else {
            Log(L"widgets: %s is not valid, starting afresh", LayoutPath().c_str());
        }
    }
    if (!read) { // first run: a column of widgets at the top left, as macOS starts
        add(WidgetKind::Clock, WidgetSize::Small, 0, 0, {});
        add(WidgetKind::Battery, WidgetSize::Small, 1, 0, {});
        add(WidgetKind::Calendar, WidgetSize::Medium, 0, 1, {});
        add(WidgetKind::Clock, WidgetSize::Medium, 0, 2, {});
    }
    bool migrated = false;
    for (auto& w : widgets_) {
        if (w->originX != LONG_MIN)
            continue;
        RECT legacy = LegacyRect(*w);
        w->originX = legacy.left;
        w->originY = legacy.top;
        migrated = true;
    }
    if (migrated)
        Save();
    else
        PublishTodos();
}

void DesktopWidgets::Save() const
{
    json::JsonArray list;
    for (const auto& w : widgets_) {
        json::JsonObject e;
        e.SetNamedValue(L"kind", json::JsonValue::CreateStringValue(NameOf(w->kind)));
        e.SetNamedValue(L"size", json::JsonValue::CreateStringValue(SizeName(w->size)));
        e.SetNamedValue(L"col", json::JsonValue::CreateNumberValue(w->col));
        e.SetNamedValue(L"row", json::JsonValue::CreateNumberValue(w->row));
        if (w->originX != LONG_MIN) {
            e.SetNamedValue(L"x", json::JsonValue::CreateNumberValue(double(w->originX)));
            e.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(double(w->originY)));
        }
        if (w->kind == WidgetKind::Calendar && w->spanW >= kCalMinW && w->spanH >= kCalMinH) {
            e.SetNamedValue(L"w", json::JsonValue::CreateNumberValue(double(w->spanW)));
            e.SetNamedValue(L"h", json::JsonValue::CreateNumberValue(double(w->spanH)));
            e.SetNamedValue(L"view", json::JsonValue::CreateNumberValue(w->content.calView));
            e.SetNamedValue(L"split", json::JsonValue::CreateNumberValue(w->content.calBoard == 1 ? 1 : 0));
            e.SetNamedValue(L"calBoard", json::JsonValue::CreateNumberValue(w->content.calBoard));
            e.SetNamedValue(L"lock", json::JsonValue::CreateBooleanValue(w->content.calLock));
            if (w->content.listPane >= 0)
                e.SetNamedValue(L"listPane", json::JsonValue::CreateNumberValue(double(w->content.listPane)));
            if (w->content.tablePane >= 0)
                e.SetNamedValue(L"tablePane", json::JsonValue::CreateNumberValue(double(w->content.tablePane)));
            if (w->content.pickColor)
                e.SetNamedValue(L"pick", json::JsonValue::CreateNumberValue(w->content.pickColor));
            if (w->content.fontColor)
                e.SetNamedValue(L"ink", json::JsonValue::CreateNumberValue(w->content.fontColor));
            if (w->content.ownDate >= 12.f)
                e.SetNamedValue(L"datePx", json::JsonValue::CreateNumberValue(w->content.ownDate));
            if (w->content.ownTable >= 16.f)
                e.SetNamedValue(L"tablePx", json::JsonValue::CreateNumberValue(w->content.ownTable));
            json::JsonArray lessons;
            for (const auto& lesson : w->content.lessons) {
                if (lesson.name.empty() && lesson.teacher.empty() && lesson.notes.empty())
                    continue;
                json::JsonObject lo;
                lo.SetNamedValue(L"wd", json::JsonValue::CreateNumberValue(lesson.weekday));
                lo.SetNamedValue(L"period", json::JsonValue::CreateNumberValue(lesson.period));
                lo.SetNamedValue(L"name", json::JsonValue::CreateStringValue(lesson.name));
                if (!lesson.teacher.empty())
                    lo.SetNamedValue(L"teacher", json::JsonValue::CreateStringValue(lesson.teacher));
                if (!lesson.notes.empty())
                    lo.SetNamedValue(L"notes", json::JsonValue::CreateStringValue(lesson.notes));
                lessons.Append(lo);
            }
            e.SetNamedValue(L"lessons", lessons);
            json::JsonArray changes;
            for (const auto& change : w->content.changes) {
                if (!change.off && change.name.empty() && change.teacher.empty() && change.notes.empty())
                    continue;
                json::JsonObject co;
                co.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(change.year));
                co.SetNamedValue(L"m", json::JsonValue::CreateNumberValue(change.month));
                co.SetNamedValue(L"d", json::JsonValue::CreateNumberValue(change.day));
                co.SetNamedValue(L"wd", json::JsonValue::CreateNumberValue(change.weekday));
                co.SetNamedValue(L"period", json::JsonValue::CreateNumberValue(change.period));
                if (change.off)
                    co.SetNamedValue(L"off", json::JsonValue::CreateBooleanValue(true));
                if (!change.name.empty())
                    co.SetNamedValue(L"name", json::JsonValue::CreateStringValue(change.name));
                if (!change.teacher.empty())
                    co.SetNamedValue(L"teacher", json::JsonValue::CreateStringValue(change.teacher));
                if (!change.notes.empty())
                    co.SetNamedValue(L"notes", json::JsonValue::CreateStringValue(change.notes));
                changes.Append(co);
            }
            if (changes.Size() > 0)
                e.SetNamedValue(L"changes", changes);
            bool anyClock = false;
            for (int i = 0; i < 8; ++i)
                if (w->content.periodStart[i] >= 0)
                    anyClock = true;
            if (anyClock) {
                json::JsonArray periods;
                for (int i = 0; i < 8; ++i) {
                    json::JsonObject po;
                    po.SetNamedValue(L"s", json::JsonValue::CreateNumberValue(w->content.periodStart[i]));
                    po.SetNamedValue(L"e", json::JsonValue::CreateNumberValue(w->content.periodEnd[i]));
                    periods.Append(po);
                }
                e.SetNamedValue(L"periods", periods);
            }
        }
        const WidgetContent& c = w->content;
        if (w->kind == WidgetKind::Notes)
            e.SetNamedValue(L"text", json::JsonValue::CreateStringValue(c.text));
        if (w->kind == WidgetKind::Reminders || w->kind == WidgetKind::Calendar) {
            json::JsonArray items;
            for (const Reminder& r : c.items) {
                if (r.text.empty())
                    continue; // one being typed
                json::JsonObject io;
                io.SetNamedValue(L"text", json::JsonValue::CreateStringValue(r.text));
                io.SetNamedValue(L"done", json::JsonValue::CreateBooleanValue(r.done));
                if (r.year) {
                    io.SetNamedValue(L"year", json::JsonValue::CreateNumberValue(r.year));
                    io.SetNamedValue(L"month", json::JsonValue::CreateNumberValue(r.month));
                    io.SetNamedValue(L"day", json::JsonValue::CreateNumberValue(r.day));
                    io.SetNamedValue(L"pri", json::JsonValue::CreateNumberValue(r.pri));
                    int dueY = r.dueYear ? r.dueYear : r.year;
                    int dueM = r.dueMonth ? r.dueMonth : r.month;
                    int dueD = r.dueDay ? r.dueDay : r.day;
                    io.SetNamedValue(L"dueY", json::JsonValue::CreateNumberValue(dueY));
                    io.SetNamedValue(L"dueM", json::JsonValue::CreateNumberValue(dueM));
                    io.SetNamedValue(L"dueD", json::JsonValue::CreateNumberValue(dueD));
                }
                items.Append(io);
            }
            e.SetNamedValue(L"items", items);
        }
        if (w->content.bgColor)
            e.SetNamedValue(L"bg", json::JsonValue::CreateNumberValue(w->content.bgColor));
        if (!w->content.bgImage.empty())
            e.SetNamedValue(L"bgImage", json::JsonValue::CreateStringValue(w->content.bgImage));
        if (w->kind == WidgetKind::Photos && !c.folder.empty())
            e.SetNamedValue(L"folder", json::JsonValue::CreateStringValue(c.folder));
        if (w->kind == WidgetKind::Schedule) {
            json::JsonArray lessons;
            for (const auto& lesson : c.lessons) {
                if (lesson.name.empty())
                    continue;
                json::JsonObject lo;
                lo.SetNamedValue(L"wd", json::JsonValue::CreateNumberValue(lesson.weekday));
                lo.SetNamedValue(L"start", json::JsonValue::CreateNumberValue(lesson.startMin));
                lo.SetNamedValue(L"end", json::JsonValue::CreateNumberValue(lesson.endMin));
                lo.SetNamedValue(L"name", json::JsonValue::CreateStringValue(lesson.name));
                lessons.Append(lo);
            }
            e.SetNamedValue(L"lessons", lessons);
            e.SetNamedValue(L"board", json::JsonValue::CreateNumberValue(c.focusDay));
        }
        if (w->kind == WidgetKind::Marks) {
            json::JsonArray marks;
            for (const auto& mark : c.marks) {
                if (mark.name.empty())
                    continue;
                json::JsonObject mo;
                mo.SetNamedValue(L"name", json::JsonValue::CreateStringValue(mark.name));
                mo.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(mark.year));
                mo.SetNamedValue(L"m", json::JsonValue::CreateNumberValue(mark.month));
                mo.SetNamedValue(L"d", json::JsonValue::CreateNumberValue(mark.day));
                mo.SetNamedValue(L"annual", json::JsonValue::CreateBooleanValue(mark.annual));
                marks.Append(mo);
            }
            e.SetNamedValue(L"marks", marks);
        }
        if (w->kind == WidgetKind::Habits) {
            json::JsonArray habits;
            for (const auto& habit : c.habits) {
                if (habit.name.empty())
                    continue;
                json::JsonObject ho;
                ho.SetNamedValue(L"name", json::JsonValue::CreateStringValue(habit.name));
                ho.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(habit.year));
                ho.SetNamedValue(L"m", json::JsonValue::CreateNumberValue(habit.month));
                ho.SetNamedValue(L"d", json::JsonValue::CreateNumberValue(habit.day));
                ho.SetNamedValue(L"streak", json::JsonValue::CreateNumberValue(habit.streak));
                habits.Append(ho);
            }
            e.SetNamedValue(L"habits", habits);
        }
        if (w->kind == WidgetKind::Focus)
            e.SetNamedValue(L"pomo", json::JsonValue::CreateNumberValue(c.pomoMin));
        if (!c.cities.empty()) {
            json::JsonArray cities;
            for (const std::wstring& city : c.cities)
                cities.Append(json::JsonValue::CreateStringValue(city));
            e.SetNamedValue(L"cities", cities);
        }
        list.Append(e);
    }
    for (const auto& s : unplaced_)
        if (json::JsonObject e{nullptr}; json::JsonObject::TryParse(s, e))
            list.Append(e);
    json::JsonObject o;
    o.SetNamedValue(L"widgets", list);
    if (!WriteTextFile(LayoutPath(), std::wstring(o.Stringify())))
        Log(L"widgets: could not save the layout");
    PublishTodos();
}

void DesktopWidgets::PublishTodos() const
{
    json::JsonArray items;
    for (const auto& w : widgets_) {
        if (w->kind != WidgetKind::Calendar)
            continue;
        for (int i = 0; i < int(w->content.items.size()); ++i) {
            const Reminder& r = w->content.items[size_t(i)];
            if (r.text.empty() || !r.year)
                continue;
            json::JsonObject io;
            io.SetNamedValue(L"text", json::JsonValue::CreateStringValue(r.text));
            io.SetNamedValue(L"done", json::JsonValue::CreateBooleanValue(r.done));
            io.SetNamedValue(L"i", json::JsonValue::CreateNumberValue(i));
            io.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(r.year));
            io.SetNamedValue(L"m", json::JsonValue::CreateNumberValue(r.month));
            io.SetNamedValue(L"d", json::JsonValue::CreateNumberValue(r.day));
            io.SetNamedValue(L"pri", json::JsonValue::CreateNumberValue(r.pri));
            io.SetNamedValue(L"dueY", json::JsonValue::CreateNumberValue(r.dueYear ? r.dueYear : r.year));
            io.SetNamedValue(L"dueM", json::JsonValue::CreateNumberValue(r.dueMonth ? r.dueMonth : r.month));
            io.SetNamedValue(L"dueD", json::JsonValue::CreateNumberValue(r.dueDay ? r.dueDay : r.day));
            items.Append(io);
        }
        break;
    }
    json::JsonObject wrap;
    wrap.SetNamedValue(L"items", items);
    WriteTextFile(AccountDataDir() + L"\\todos.json", std::wstring(wrap.Stringify()));
}

void DesktopWidgets::SaveSoon() { SetTimer(host_, kSaveTimer, kSaveMs, nullptr); }

// ---- windows -----------------------------------------------------------------

DesktopWidgets::Widget& DesktopWidgets::Create(WidgetKind kind, WidgetSize size, int col, int row)
{
    auto w = std::make_unique<Widget>();
    w->kind = kind;
    w->size = size;
    w->col = col;
    w->row = row;
    if (kind == WidgetKind::Calendar) {
        w->spanW = kCalDefaultW;
        w->spanH = kCalDefaultH;
    }
    widgets_.push_back(std::move(w));
    return *widgets_.back();
}

bool DesktopWidgets::Has(const Widget* w) const
{
    return std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& p) { return p.get() == w; });
}

void DesktopWidgets::Build()
{
    for (auto& wp : widgets_) {
        Widget& w = *wp;
        if (w.surface)
            continue;
        // Notes, reminders and the calendar (its todos) take the keyboard: their windows can be activated.
        bool type = Editable(w.kind) || w.kind == WidgetKind::Calendar || w.kind == WidgetKind::Schedule || w.kind == WidgetKind::Marks ||
                    w.kind == WidgetKind::Habits;
        w.surface = std::make_unique<Surface>(instance_, L"Yunzhao Widget", type, 14.f, true);
        w.surface->SetRadius(kWidgetRadius);
        w.surface->SetGlassless(material_ == WidgetMaterial::Color); // its own face, no glass under it
        w.surface->paint = [this, &w](Canvas& c) { Paint(w, c); };
        w.surface->message = [this, &w](UINT m, WPARAM wp2, LPARAM lp, LRESULT& r) { return Message(w, m, wp2, lp, r); };
        Place(w, false);
        if (w.kind == WidgetKind::Photos)
            LoadPhoto(w, true);
    }
}

void DesktopWidgets::Clear()
{
    for (auto& w : widgets_)
        if (w->surface)
            w->surface->Hide(false);
    widgets_.clear();
}

void DesktopWidgets::Place(Widget& w, bool glide)
{
    if (!w.surface)
        return;
    if (w.originX == LONG_MIN) {
        RECT legacy = LegacyRect(w);
        w.originX = legacy.left;
        w.originY = legacy.top;
    }
    RECT to = Settle(PixelRect(w), &w);
    Remember(w, to);
    if (!w.surface->Visible()) {
        w.surface->Show(to, false, POINT{(to.left + to.right) / 2, (to.top + to.bottom) / 2}, true);
        Restack();
        return;
    }
    if (!glide) {
        w.glideMs = 0;
        w.surface->Move(to);
        return;
    }
    RECT current = w.surface->Rect();
    if (current.right - current.left != to.right - to.left || current.bottom - current.top != to.bottom - to.top) {
        float s = std::max(Scale(), 0.01f);
        w.surface->ReserveResize(D2D1::SizeF(std::max(float(current.right - current.left), float(to.right - to.left)) / s,
                                             std::max(float(current.bottom - current.top), float(to.bottom - to.top)) / s));
    }
    w.glideFrom = w.surface->Rect();
    w.glideTo = to;
    w.glideMs = ClockMs();
    SetTimer(host_, kGlideTimer, 8, nullptr);
}

void DesktopWidgets::Glide()
{
    bool any = false;
    double now = ClockMs();
    for (auto& w : widgets_) {
        if (w->glideMs <= 0 || !w->surface)
            continue;
        float k = motion::EaseOut(float((now - w->glideMs) / kGlideMs));
        // Size changes too (a widget made larger or smaller).
        auto lerp = [&](LONG a, LONG b) { return LONG(std::lround(float(a) + float(b - a) * k)); };
        RECT r{lerp(w->glideFrom.left, w->glideTo.left), lerp(w->glideFrom.top, w->glideTo.top), lerp(w->glideFrom.right, w->glideTo.right),
               lerp(w->glideFrom.bottom, w->glideTo.bottom)};
        w->surface->Move(r);
        if (k >= 1) {
            w->glideMs = 0;
            if (!w->resizing)
                w->surface->ReserveResize(D2D1::SizeF());
        } else
            any = true;
    }
    if (!any)
        KillTimer(host_, kGlideTimer);
}

bool DesktopWidgets::Add(WidgetKind kind, WidgetSize size)
{
    if (!WidgetSupports(kind, size))
        size = WidgetSize::Small;
    Widget& created = Create(kind, size, 0, 0);
    RECT guess = PixelRect(created);
    RECT spot = FindSpot(guess.right - guess.left, guess.bottom - guess.top);
    created.originX = spot.left;
    created.originY = spot.top;
    if (enabled_)
        Build();
    Save();
    Log(L"widgets: added %s %s at %d,%d", NameOf(kind), SizeName(size), spot.left, spot.top);
    return true;
}

namespace {

bool HostedCalendarAlive()
{
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return false;
    HANDLE file = CreateFileW((std::wstring(base) + L"\\Yunzhao\\yunzhao-hwnd.txt").c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char buf[64]{};
    DWORD got = 0;
    ReadFile(file, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(file);
    unsigned long long value = _strtoui64(buf, nullptr, 10);
    HWND note = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
    return value && IsWindow(note);
}

void StartHostedCalendar()
{
    if (HostedCalendarAlive())
        return;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring folder(exe);
    const size_t slash = folder.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        folder.resize(slash);
    auto resolve = [](const std::wstring& candidate) {
        wchar_t full[MAX_PATH]{};
        if (!GetFullPathNameW(candidate.c_str(), MAX_PATH, full, nullptr))
            return std::wstring();
        if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
            return std::wstring();
        return std::wstring(full);
    };
    std::wstring packaged = resolve(folder + L"\\Calendar\\YunzhaoNote.exe");
    if (!packaged.empty()) {
        std::wstring calendarDir = packaged.substr(0, packaged.find_last_of(L"\\/"));
        ShellExecuteW(nullptr, L"open", packaged.c_str(), nullptr, calendarDir.c_str(), SW_SHOWNORMAL);
        return;
    }
    std::wstring script = resolve(folder + L"\\..\\..\\..\\tools\\yunzhao-note.ps1");
    if (script.empty())
        script = resolve(folder + L"\\tools\\yunzhao-note.ps1");
    if (script.empty())
        return;
    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), nullptr, SW_HIDE);
}

void StopHostedCalendar()
{
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    std::wstring path = std::wstring(base) + L"\\Yunzhao\\yunzhao-hwnd.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    unsigned long long value = 0;
    if (file != INVALID_HANDLE_VALUE) {
        char buf[64]{};
        DWORD got = 0;
        ReadFile(file, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(file);
        value = _strtoui64(buf, nullptr, 10);
    }
    HWND note = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
    DWORD pid = 0;
    if (value && IsWindow(note))
        GetWindowThreadProcessId(note, &pid);
    if (pid && pid != GetCurrentProcessId()) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
        }
    }
    DeleteFileW(path.c_str());
}

} // namespace

bool DesktopWidgets::CalendarOpen() const
{
    return HostedCalendarAlive();
}

void DesktopWidgets::SetCalendarOpen(bool open)
{
    if (!open) {
        StopHostedCalendar();
        std::vector<Widget*> calendars;
        for (auto& w : widgets_)
            if (w->kind == WidgetKind::Calendar)
                calendars.push_back(w.get());
        for (Widget* calendar : calendars)
            RemoveNow(*calendar);
        return;
    }
    bool has = false;
    for (const auto& w : widgets_)
        if (w->kind == WidgetKind::Calendar)
            has = true;
    if (!has)
        Add(WidgetKind::Calendar, WidgetSize::Medium);
    StartHostedCalendar();
}

int DesktopWidgets::Toggle(WidgetKind kind, WidgetSize size)
{
    if (kind == WidgetKind::Notes || kind == WidgetKind::Reminders || kind == WidgetKind::Photos)
        return 0;
    if (kind == WidgetKind::Calendar) {
        bool has = false;
        for (const auto& w : widgets_)
            if (w->kind == WidgetKind::Calendar)
                has = true;
        bool alive = HostedCalendarAlive();
        if (!has && !Add(kind, size))
            return 0;
        if (!alive)
            StartHostedCalendar();
        return has && alive ? 3 : 1;
    }
    if (kind == WidgetKind::Schedule) {
        for (auto& w : widgets_) {
            if (w->kind != WidgetKind::Calendar)
                continue;
            w->content.calBoard = 2;
            w->content.calSplit = false;
            if (w->surface)
                w->surface->Invalidate();
            Save();
            if (!HostedCalendarAlive())
                StartHostedCalendar();
            return 1;
        }
        if (!Add(WidgetKind::Calendar, size))
            return 0;
        StartHostedCalendar();
        return 1;
    }
    std::vector<Widget*> found;
    for (auto& w : widgets_)
        if (w->kind == kind)
            found.push_back(w.get());
    if (!found.empty())
        return 3;
    return Add(kind, size) ? 1 : 0;
}

bool DesktopWidgets::AddNamed(const std::wstring& kind, const std::wstring& size)
{
    WidgetKind k;
    WidgetSize s;
    return ParseNames(kind, size, k, s) && Add(k, s);
}

bool DesktopWidgets::ParseNames(const std::wstring& kind, const std::wstring& size, WidgetKind& outKind, WidgetSize& outSize)
{
    outSize = ParseSize(size);
    return ParseKind(kind, outKind);
}

void DesktopWidgets::Remove(Widget& w)
{
    // Its window may be handling the message that asked for this.
    PostMessageW(host_, WM_WIDGET_REMOVE, 0, reinterpret_cast<LPARAM>(&w));
}

void DesktopWidgets::RemoveNow(Widget& w)
{
    auto it = std::find_if(widgets_.begin(), widgets_.end(), [&](const auto& p) { return p.get() == &w; });
    if (it == widgets_.end())
        return;
    std::unique_ptr<Widget> gone = std::move(*it);
    widgets_.erase(it);
    if (gone->kind == WidgetKind::Focus && gone->content.pomoRun)
        ClosePomo();
    if (todo_.owner == gone.get())
        CloseTodo(false);
    if (handout_.owner == gone.get())
        CloseHandout(false);
    if (style_.owner == gone.get())
        CloseStyle(false);
    if (tool_.owner == gone.get())
        CloseTool();
    if (gone->surface)
        gone->surface->Hide(false);
    Save();
    Log(L"widgets: removed %s", NameOf(gone->kind));
}

void DesktopWidgets::Resize(Widget& w, WidgetSize size)
{
    if (w.kind == WidgetKind::Calendar) {
        float pw = size == WidgetSize::Small ? kCalSmallW : size == WidgetSize::Medium ? kCalMediumW : kCalLargeW;
        float ph = size == WidgetSize::Small ? kCalSmallH : size == WidgetSize::Medium ? kCalMediumH : kCalLargeH;
        RECT probe = w.originX != LONG_MIN ? RECT{w.originX, w.originY, w.originX + 20, w.originY + 20} : WorkArea();
        RECT wa = ClampMonitor(probe, false);
        HMONITOR monitor = MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(monitor, &info))
            wa = info.rcWork;
        float s = std::max(PopupScale(wa), 0.01f);
        pw = std::clamp(pw, kCalMinW, std::max(kCalMinW, float(wa.right - wa.left) / s));
        ph = std::clamp(ph, kCalMinH, std::max(kCalMinH, float(wa.bottom - wa.top) / s));
        if (w.size == size && std::abs(w.spanW - pw) < 1.f && std::abs(w.spanH - ph) < 1.f)
            return;
        w.spanW = pw;
        w.spanH = ph;
        w.size = size;
        Place(w, true);
        if (w.surface)
            w.surface->Invalidate();
        Save();
        return;
    }
    if (w.size == size || !WidgetSupports(w.kind, size))
        return;
    w.size = size;
    Place(w, true);
    w.surface->Invalidate();
    if (w.kind == WidgetKind::Photos)
        LoadPhoto(w, false); // the same photo, cropped for the new size
    Save();
}

float DesktopWidgets::CalendarFraction(const Widget& w) const
{
    RECT cur = w.surface && w.surface->Visible() ? w.surface->Rect() : RECT{w.originX, w.originY, w.originX + 20, w.originY + 20};
    HMONITOR monitor = MonitorFromRect(&cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return 0.45f;
    float s = std::max(PopupScale(info.rcWork), 0.01f);
    float maxH = std::max(kCalMinH + 1.f, float(info.rcWork.bottom - info.rcWork.top) / s - 16.f);
    return std::clamp((w.spanH - kCalMinH) / (maxH - kCalMinH), 0.f, 1.f);
}

void DesktopWidgets::ApplyCalendarScale(Widget& w, float t, bool save)
{
    if (!w.surface || w.kind != WidgetKind::Calendar)
        return;
    t = std::clamp(t, 0.f, 1.f);
    RECT cur = w.surface->Rect();
    HMONITOR monitor = MonitorFromRect(&cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return;
    float s = std::max(PopupScale(info.rcWork), 0.01f);
    float maxW = std::max(kCalMinW, float(info.rcWork.right - info.rcWork.left) / s - 16.f);
    float maxH = std::max(kCalMinH, float(info.rcWork.bottom - info.rcWork.top) / s - 16.f);
    int nw = std::max(1, int(std::lround((kCalMinW + t * (maxW - kCalMinW)) * s)));
    int nh = std::max(1, int(std::lround((kCalMinH + t * (maxH - kCalMinH)) * s)));
    RECT r{cur.left, cur.top, cur.left + nw, cur.top + nh};
    if (r.right > info.rcWork.right) {
        r.left -= r.right - info.rcWork.right;
        r.right = info.rcWork.right;
    }
    if (r.bottom > info.rcWork.bottom) {
        r.top -= r.bottom - info.rcWork.bottom;
        r.bottom = info.rcWork.bottom;
    }
    if (r.left < info.rcWork.left) {
        r.right += info.rcWork.left - r.left;
        r.left = info.rcWork.left;
    }
    if (r.top < info.rcWork.top) {
        r.bottom += info.rcWork.top - r.top;
        r.top = info.rcWork.top;
    }
    w.spanW = float(r.right - r.left) / s;
    w.spanH = float(r.bottom - r.top) / s;
    w.content.calT = t;
    w.glideMs = 0;
    w.surface->Move(r);
    w.surface->Invalidate();
    if (save) {
        Remember(w, r);
        Save();
    }
}

void DesktopWidgets::SetCalendarScale(float t)
{
    for (auto& widget : widgets_)
        if (widget->kind == WidgetKind::Calendar && widget->surface)
            ApplyCalendarScale(*widget, t, false);
    Save();
}

void DesktopWidgets::SyncPopupHook()
{
    bool pin = false;
    for (const auto& w : widgets_)
        if (w->content.popupPin && w->content.popupDay > 0)
            pin = true;
    if (pin) {
        if (!g_outsideHook && host_) {
            g_outsideHost = host_;
            g_outsideHook = SetWindowsHookExW(WH_MOUSE_LL, OutsideHook, nullptr, 0);
        }
    } else if (g_outsideHook) {
        UnhookWindowsHookEx(g_outsideHook);
        g_outsideHook = nullptr;
        g_outsideHost = nullptr;
    }
}

void DesktopWidgets::WatchPopup(Widget& w, POINT content, bool insideWindow)
{
    if (w.kind != WidgetKind::Calendar || w.content.popupDay < 1 || w.content.editing || w.content.popupPin) {
        w.popupLeaveMs = 0;
        return;
    }
    bool onPopup = false;
    if (insideWindow && w.surface) {
        float s = std::max(w.surface->Scale(), 0.01f);
        std::wstring id = TopHitId(w.hits, float(content.x) / s, float(content.y) / s);
        onPopup = id == L"popup" || id.rfind(L"check:", 0) == 0 || id.rfind(L"sel:", 0) == 0;
    }
    if (onPopup) {
        w.popupLeaveMs = 0;
        return;
    }
    if (w.popupLeaveMs == 0) {
        w.popupLeaveMs = ClockMs();
        SetTimer(host_, kPopupTimer, 3000, nullptr);
    }
}

int DesktopWidgets::GripAt(const Widget& w, POINT screen) const
{
    if (!w.surface || !Resizable(w.kind) || (w.kind == WidgetKind::Calendar && w.content.calLock))
        return 0;
    RECT r = w.surface->Rect();
    if (!PtInRect(&r, screen))
        return 0;
    if (w.kind == WidgetKind::Calendar) {
        int g = int(std::lround(14 * Scale()));
        int edges = 0;
        if (screen.x < r.left + g)
            edges |= 1;
        if (screen.x >= r.right - g)
            edges |= 2;
        if (screen.y < r.top + g)
            edges |= 4;
        if (screen.y >= r.bottom - g)
            edges |= 8;
        return edges;
    }
    int g = int(std::lround(kGrip * Scale()));
    if (screen.y < r.bottom - g)
        return 0;
    return screen.x < r.left + g ? -1 : screen.x >= r.right - g ? 1 : 0;
}

// Dragging the grip: the size whose corner is nearest the pointer, among those the widget has
// and that fit where it stands; it glides there at once, as the sizes of the menu do.
void DesktopWidgets::DragSize(Widget& w, POINT screen)
{
    float s = std::max(PopupScale(w.downRect), 0.01f);
    if (w.kind == WidgetKind::Calendar) {
        const RECT& d = w.downRect;
        int minW = int(std::lround(kCalMinW * s));
        int minH = int(std::lround(kCalMinH * s));
        RECT r = d;
        if (w.edges & 1)
            r.left = screen.x;
        if (w.edges & 2)
            r.right = screen.x;
        if (w.edges & 4)
            r.top = screen.y;
        if (w.edges & 8)
            r.bottom = screen.y;
        if (r.right - r.left < minW) {
            if (w.edges & 1)
                r.left = r.right - minW;
            else
                r.right = r.left + minW;
        }
        if (r.bottom - r.top < minH) {
            if (w.edges & 4)
                r.top = r.bottom - minH;
            else
                r.bottom = r.top + minH;
        }
        r = Nudge(r, &w);
        if (r.right < r.left + minW)
            r.right = r.left + minW;
        if (r.bottom < r.top + minH)
            r.bottom = r.top + minH;
        s = std::max(PopupScale(r), 0.01f);
        w.spanW = std::max(kCalMinW, float(r.right - r.left) / s);
        w.spanH = std::max(kCalMinH, float(r.bottom - r.top) / s);
        w.glideMs = 0;
        w.surface->Move(r);
        return;
    }
    const RECT& d = w.downRect;
    float width = float(w.growLeft ? d.right - screen.x : screen.x - d.left) / s;
    float height = float(screen.y - d.top) / s;
    const float half = kWidgetCell + (kWidgetGap + kWidgetCell) / 2;
    WidgetSize size = height > half ? WidgetSize::Large : width > half ? WidgetSize::Medium : WidgetSize::Small;
    if (size == WidgetSize::Large && !WidgetSupports(w.kind, size))
        size = WidgetSize::Medium;
    if (!WidgetSupports(w.kind, size))
        size = WidgetSize::Small;
    if (size == w.size)
        return;
    int nw = std::max(1, int(std::lround(WidgetWidth(size) * s)));
    int nh = std::max(1, int(std::lround(WidgetHeight(size) * s)));
    RECT r = d;
    if (w.growLeft)
        r.left = r.right - nw;
    else
        r.right = r.left + nw;
    r.bottom = r.top + nh;
    r = Nudge(r, &w);
    w.size = size;
    w.glideMs = 0;
    w.surface->Move(r);
    w.surface->Invalidate();
}

bool DesktopWidgets::IsOurs(HWND h) const
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// ---- the layer -----------------------------------------------------------------

void CALLBACK DesktopWidgets::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG obj, LONG child, DWORD, DWORD)
{
    if (!g_widgets || obj != OBJID_WINDOW || child != CHILDID_SELF)
        return;
    // Only top-level show/hide events: child controls change constantly.
    bool appeared = event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_UNCLOAKED;
    if (appeared && (!h || GetAncestor(h, GA_ROOT) != h))
        return;
    if (!g_widgets->restackPosted_) {
        g_widgets->restackPosted_ = true; // a burst of events, one restack right after it
        PostMessageW(g_widgets->host_, WM_WIDGET_RESTACK, 0, 0);
    }
}

// Always non-topmost, above the desktop when possible and under application windows.
// Clicking or editing never lifts a widget over another application's window.
void DesktopWidgets::Restack()
{
    if (!enabled_ || widgets_.empty())
        return;
    // Moving the widget windows while a game is taking the screen drops the game
    // back to the desktop. They are already under every application.
    HWND fg = GetForegroundWindow();
    if (FullScreenInFront(monitor_) || FullScreenApp(fg))
        return;
    zMoving_ = true;
    struct Done {
        bool& flag;
        ~Done() { flag = false; }
    } done{zMoving_};
    // Find the lowest visible application on this screen.
    struct Scan {
        const DesktopWidgets* self;
        HMONITOR monitor;
        HWND lowest = nullptr;
    } scan{this, monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* s = reinterpret_cast<Scan*>(l);
            if (!IsWindowVisible(h) || IsIconic(h) || s->self->IsOurs(h) || IsDesktopWindow(h))
                return TRUE;
            RECT rect{};
            if (!GetWindowRect(h, &rect) || IsRectEmpty(&rect) || (!HasTitle(h) && !(GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION)))
                return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            std::wstring c = ClassOf(h);
            if (cloaked || c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd" || c == L"Windows.UI.Core.CoreWindow")
                return TRUE;
            s->lowest = h;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&scan));
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    // Inserting after a topmost window does not remove WS_EX_TOPMOST.
    // Always leave that band first, including while Explorer is starting.
    for (auto& w : widgets_) {
        if (!w->surface || w->content.windowLayer == 2)
            continue;
        if (GetWindowLongPtrW(w->surface->Hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST)
            SetWindowPos(w->surface->Hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    }
    // The window just above the desktop's top window (Progman, or the
    // WorkerW with the icons); the widgets go right under it.
    HWND above = nullptr, h = GetTopWindow(nullptr);
    bool found = false;
    for (; h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (IsWindowVisible(h) && IsDesktopWindow(h)) {
            found = true;
            break;
        }
        bool widget = std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& w) { return w->surface && w->surface->Hwnd() == h; });
        if (!widget)
            above = h;
    }
    if (!found)
        above = nullptr; // no desktop window: keep them at the bottom of the windows
    HWND after = found ? (above ? above : HWND_TOP) : HWND_BOTTOM;
    // Show-desktop and startup can put Progman/WorkerW above an application. Show-desktop
    // (Win+D) also gives the desktop the focus: the widgets come up with it, and stay up
    // while one of them or the bar is clicked. Startup's temporary position is not used
    // to jump over the application.
    const bool desktopInFront = fg && (IsDesktopWindow(fg) || IsOurs(fg));
    if (scan.lowest && !desktopInFront)
        for (HWND below = found ? h : nullptr; below; below = GetWindow(below, GW_HWNDNEXT))
            if (below == scan.lowest) {
                after = scan.lowest;
                break;
            }
    if (after != HWND_TOP && after != HWND_BOTTOM && (GetWindowLongPtrW(after, GWL_EXSTYLE) & WS_EX_TOPMOST))
        after = HWND_TOP; // widgets have already been made non-topmost
    for (auto& w : widgets_) {
        if (!w->surface || w->content.windowLayer == 2)
            continue;
        // 置底 stays above the desktop, with the other widgets. HWND_BOTTOM hides the
        // timetable under the wallpaper because this window is the whole monitor.
        SetWindowPos(w->surface->Hwnd(), after, 0, 0, 0, 0, flags);
        after = w->surface->Hwnd();
    }
    for (auto& w : widgets_)
        if (w->surface && w->content.windowLayer == 2)
            SetWindowPos(w->surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, flags);
    // A screenshot can show the calendar page over the timetable. Put it back
    // the moment that window appears, and redraw the timetable over any white frame.
    OrderCalendarNote();
}

LRESULT CALLBACK DesktopWidgets::HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<DesktopWidgets*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        switch (m) {
        case WM_TIMER:
            if (w == kTickTimer) {
                self->Tick();
            } else if (w == kSafetyTimer) {
                self->Restack();
            } else if (w == kGlideTimer) {
                self->Glide();
            } else if (w == kSaveTimer) {
                KillTimer(h, kSaveTimer);
                self->Save();
                AccountPublishLayout();
            } else if (w == kCaretTimer) {
                bool any = false;
                for (auto& wd : self->widgets_)
                    if (wd->content.editing) {
                        wd->content.caretOn = !wd->content.caretOn;
                        wd->surface->Invalidate();
                        any = true;
                    }
                if (self->todo_.open) {
                    self->todo_.caretOn = !self->todo_.caretOn;
                    if (self->todoSurface_)
                        self->todoSurface_->Invalidate();
                    any = true;
                }
                if (self->handout_.open) {
                    self->handout_.caretOn = !self->handout_.caretOn;
                    if (self->handoutSurface_)
                        self->handoutSurface_->Invalidate();
                    any = true;
                }
                if (!any)
                    KillTimer(h, kCaretTimer);
            } else if (w == kLockTimer) {
                bool any = false;
                for (auto& wd : self->widgets_) {
                    if (!wd->lockHoldStart)
                        continue;
                    any = true;
                    float t = float(GetTickCount64() - wd->lockHoldStart) / 3000.f;
                    wd->content.lockPress = std::min(1.f, std::max(0.02f, t));
                    if (t >= 1.f) {
                        wd->lockHoldStart = 0;
                        wd->content.calLock = false;
                        wd->content.lockPress = 0;
                        if (wd->surface && GetCapture() == wd->surface->Hwnd())
                            ReleaseCapture();
                        self->SaveSoon();
                    }
                    if (wd->surface)
                        wd->surface->Invalidate();
                }
                if (!any)
                    KillTimer(h, kLockTimer);
            } else if (w == kToolTimer) {
                if (!self->tool_.open || (!self->tool_.running && !self->tool_.swRun)) {
                    KillTimer(h, kToolTimer);
                } else if (self->tool_.running) {
                    int left = self->tool_.remainMs - int(GetTickCount64() - self->tool_.mark);
                    if (left <= 0) {
                        self->tool_.running = false;
                        self->tool_.remainMs = 0;
                        KillTimer(h, kToolTimer);
                    }
                    if (self->toolSurface_)
                        self->toolSurface_->Invalidate();
                } else if (self->toolSurface_) {
                    self->toolSurface_->Invalidate();
                }
            } else if (w == kPeekTimer) {
                self->PeekCalendar();
            } else if (w == kDisplayTimer) {
                KillTimer(h, kDisplayTimer);
                if (!self->enabled_)
                    return 0;
                for (auto& wd : self->widgets_)
                    self->Place(*wd, false);
            } else if (w == kPopupTimer) {
                bool pending = false;
                double now = ClockMs();
                for (auto& wd : self->widgets_) {
                    if (wd->kind != WidgetKind::Calendar || wd->content.popupDay < 1 || wd->popupLeaveMs == 0)
                        continue;
                    if (now - wd->popupLeaveMs >= 3000) {
                        wd->content.popupDay = 0;
                        wd->content.popupIndex = -1;
                        wd->content.popupAll = false;
                        wd->content.popupPin = false;
                        wd->popupLeaveMs = 0;
                        if (wd->surface)
                            wd->surface->Invalidate();
                    } else {
                        pending = true;
                    }
                }
                if (!pending)
                    KillTimer(h, kPopupTimer);
                self->SyncPopupHook();
            }
            return 0;
        case WM_WIDGET_OUTSIDE: {
            POINT pt;
            GetCursorPos(&pt);
            for (auto& wd : self->widgets_) {
                if (!wd->content.popupPin || wd->content.popupDay < 1 || !wd->surface)
                    continue;
                RECT r = wd->surface->Rect();
                if (PtInRect(&r, pt))
                    continue;
                wd->content.popupDay = 0;
                wd->content.popupIndex = -1;
                wd->content.popupAll = false;
                wd->content.popupPin = false;
                wd->popupLeaveMs = 0;
                wd->surface->Invalidate();
            }
            self->SyncPopupHook();
            return 0;
        }
        case WM_WIDGET_RESTACK:
            self->restackPosted_ = false;
            self->Restack();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            // The topology arrives in several messages. Wait until it settles,
            // then put each widget back on the screen it already occupied.
            SetTimer(h, kDisplayTimer, 700, nullptr);
            return 0;
        case WM_SETTINGCHANGE:
            if (w == SPI_SETWORKAREA)
                SetTimer(h, kDisplayTimer, 700, nullptr);
            return 0;
        case WM_WIDGET_REMOVE: {
            auto* gone = reinterpret_cast<Widget*>(l);
            if (self->Has(gone))
                self->RemoveNow(*gone);
            return 0;
        }
        case WM_WIDGET_PHOTO: {
            std::unique_ptr<PhotoRead> read(reinterpret_cast<PhotoRead*>(l));
            auto* wd = static_cast<Widget*>(read->widget);
            if (!self->Has(wd) || read->gen != wd->photoGen || !wd->surface)
                return 0; // removed, or a newer photo is on its way
            WidgetContent& c = wd->content;
            if (!c.imageKey.empty())
                wd->surface->ForgetImage(c.imageKey);
            c.photo = read->path;
            c.image = read->image.empty() ? nullptr : std::make_shared<const IconImage>(std::move(read->image));
            c.imageKey = L"photo:" + std::to_wstring(read->gen);
            wd->photoAt = GetTickCount64();
            wd->surface->Invalidate();
            return 0;
        }
        case WM_WIDGET_WEATHER:
            self->world_.weather = self->weather_->Latest();
            for (auto& wd : self->widgets_)
                if (wd->kind == WidgetKind::Weather && wd->surface)
                    wd->surface->Invalidate();
            return 0;
        case WM_WIDGET_BLUETOOTH:
            self->world_.bluetooth = self->bluetooth_->Latest();
            self->world_.bluetoothRead = self->bluetooth_->Read();
            for (auto& wd : self->widgets_)
                if (wd->kind == WidgetKind::Bluetooth && wd->surface)
                    wd->surface->Invalidate();
            return 0;
        case WM_TIMECHANGE:
            self->world_.city = LocalCity();
            self->Tick();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void HideBlurCover()
{
    // The note page leaves a topmost blur sheet behind after it hides. That sheet
    // is exactly the timetable rectangle, so the timetable looks covered.
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"BlurOverlayWindow") == 0 && IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_HIDE);
        return TRUE;
    }, 0);
}

void DesktopWidgets::OrderCalendarNote()
{
    HWND widget = nullptr;
    for (auto& w : widgets_) {
        if (w->kind == WidgetKind::Calendar && w->surface) {
            widget = w->surface->Hwnd();
            w->surface->SetDesktopClip(true, RECT{}); // both views are the page; this window keeps no pixels
            break;
        }
    }
    HideBlurCover();
    if (!widget || !IsWindow(widget))
        return;
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    std::wstring path = std::wstring(base) + L"\\Yunzhao\\yunzhao-hwnd.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    char buf[64]{};
    DWORD got = 0;
    ReadFile(file, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(file);
    unsigned long long value = _strtoui64(buf, nullptr, 10);
    HWND note = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
    if (!value || !IsWindow(note) || note == widget)
        return;
    LONG_PTR ex = GetWindowLongPtrW(note, GWL_EXSTYLE);
    LONG_PTR tool = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
    if (tool != ex) {
        SetWindowLongPtrW(note, GWL_EXSTYLE, tool);
        SetWindowPos(note, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    // A 1-bit rounded region composites the curve against black. Windows 11 rounds
    // the window itself, and the page clips to the same curve, so the corner is wallpaper.
    // Windows 10 has no system corner preference; the window stays square there.
    if (Windows11()) {
        const int corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(note, 33, &corner, sizeof(corner)); // DWMWA_WINDOW_CORNER_PREFERENCE
        const COLORREF border = 0xFFFFFFFE; // DWMWA_COLOR_NONE, no dark frame
        DwmSetWindowAttribute(note, 34, &border, sizeof(border)); // DWMWA_BORDER_COLOR
    }
    HRGN probe = CreateRectRgn(0, 0, 0, 0);
    const int kind = GetWindowRgn(note, probe);
    DeleteObject(probe);
    bool changed = false;
    if (kind != ERROR) {
        SetWindowRgn(note, nullptr, TRUE);
        changed = true;
    }
    if (!IsWindowVisible(note)) {
        ShowWindow(note, SW_SHOWNA);
        changed = true;
    }
    if (!changed)
        return;
    for (auto& w : widgets_)
        if (w->kind == WidgetKind::Calendar && w->surface)
            w->surface->Invalidate();
}

void DesktopWidgets::PollYunzhaoCommand(Widget& w)
{
    std::wstring path = AccountDataDir() + L"\\yunzhao-cmd.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    std::wstring text = ReadTextFile(path);
    DeleteFileW(path.c_str());
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o))
        return;
    const std::wstring cmd(o.GetNamedString(L"cmd", L""));
    if (cmd == L"board") {
        int board = std::clamp(int(o.GetNamedNumber(L"board", 0)), 0, 2);
        if (board == 1)
            board = 0;
        w.content.calBoard = board;
        w.content.calSplit = false;
        w.content.tableEdit = false;
        w.content.toolsOpen = false;
        w.content.editPeriod = -1;
        EndEdit(w);
        SaveSoon();
        if (w.surface)
            w.surface->Invalidate();
        SyncYunzhao(w);
    } else if (cmd == L"lock") {
        if (!w.content.calLock) {
            w.content.calLock = true;
            EndEdit(w);
            w.content.tableEdit = false;
            w.content.toolsOpen = false;
            w.content.editPeriod = -1;
            SaveSoon();
            if (w.surface)
                w.surface->Invalidate();
        }
    } else if (cmd == L"unlock") {
        w.content.calLock = false;
        w.content.lockPress = 0;
        SaveSoon();
        if (w.surface)
            w.surface->Invalidate();
    } else if (cmd == L"layer") {
        const std::wstring layer(o.GetNamedString(L"layer", L""));
        int next = layer == L"top" ? 2 : layer == L"bottom" ? 0 : layer == L"normal" ? 1 : -1;
        if (next >= 0 && w.content.windowLayer != next) {
            w.content.windowLayer = next;
            if (w.surface)
                w.surface->Invalidate();
            Restack();
        }
    } else if (cmd == L"creator-done") {
        w.creatorHold = false;
    } else if (cmd == L"place") {
        RECT next{
            LONG(std::lround(o.GetNamedNumber(L"left", 0))),
            LONG(std::lround(o.GetNamedNumber(L"top", 0))),
            LONG(std::lround(o.GetNamedNumber(L"right", 0))),
            LONG(std::lround(o.GetNamedNumber(L"bottom", 0)))
        };
        if (w.surface && next.right > next.left && next.bottom > next.top) {
            float scale = std::max(PopupScale(next), 0.01f);
            int minW = int(std::lround(kCalMinW * scale));
            int minH = int(std::lround(kCalMinH * scale));
            if (next.right - next.left < minW)
                next.right = next.left + minW;
            if (next.bottom - next.top < minH)
                next.bottom = next.top + minH;
            next = ClampOntoMonitor(next);
            Remember(w, next);
            w.glideMs = 0;
            w.surface->Move(next);
            SaveSoon();
            SyncYunzhao(w);
        }
    } else if (cmd == L"check") {
        const int index = int(o.GetNamedNumber(L"index", -1));
        if (index >= 0 && index < int(w.content.items.size()) && !w.content.items[size_t(index)].text.empty()) {
            w.content.items[size_t(index)].done = !w.content.items[size_t(index)].done;
            PublishTodos();
            SaveSoon();
        }
    } else if (cmd == L"cell") {
        const int wd = std::clamp(int(o.GetNamedNumber(L"wd", 0)), 0, 6);
        const int period = std::clamp(int(o.GetNamedNumber(L"period", 0)), 0, 7);
        const std::wstring name(o.GetNamedString(L"name", L""));
        const std::wstring teacher(o.GetNamedString(L"teacher", L""));
        const int y = int(o.GetNamedNumber(L"y", 0));
        const int m = int(o.GetNamedNumber(L"m", 0));
        const int d = int(o.GetNamedNumber(L"d", 0));
        const bool weekOnly = std::wstring(o.GetNamedString(L"scope", L"")) == L"week";
        const int change = y > 0 ? FindLessonChange(w.content, y, m, d, wd, period) : -1;
        if (weekOnly) {
            if (name.empty()) {
                if (change >= 0)
                    w.content.changes.erase(w.content.changes.begin() + change);
            } else if (y > 0) {
                int slot = change;
                if (slot < 0) {
                    WidgetContent::LessonChange added;
                    added.year = y;
                    added.month = m;
                    added.day = d;
                    added.weekday = wd;
                    added.period = period;
                    w.content.changes.push_back(std::move(added));
                    slot = int(w.content.changes.size()) - 1;
                }
                w.content.changes[size_t(slot)].name = name;
                w.content.changes[size_t(slot)].teacher = teacher;
                w.content.changes[size_t(slot)].off = false;
            }
            SaveSoon();
            SyncYunzhao(w);
            return;
        }
        if (change >= 0)
            w.content.changes.erase(w.content.changes.begin() + change);
        {
            int lesson = -1;
            for (int i = 0; i < int(w.content.lessons.size()); ++i)
                if (w.content.lessons[size_t(i)].weekday == wd && w.content.lessons[size_t(i)].period == period)
                    lesson = i;
            if (name.empty()) {
                if (lesson >= 0)
                    w.content.lessons.erase(w.content.lessons.begin() + lesson);
            } else if (lesson >= 0) {
                w.content.lessons[size_t(lesson)].name = name;
                w.content.lessons[size_t(lesson)].teacher = teacher;
            } else {
                WidgetContent::Lesson added;
                added.weekday = wd;
                added.period = period;
                added.name = name;
                added.teacher = teacher;
                if (w.content.periodStart[period] >= 0) {
                    added.startMin = w.content.periodStart[period];
                    added.endMin = w.content.periodEnd[period];
                }
                w.content.lessons.push_back(std::move(added));
            }
        }
        SaveSoon();
        SyncYunzhao(w);
    } else if (cmd == L"period") {
        const int row = std::clamp(int(o.GetNamedNumber(L"row", 0)), 0, 7);
        int start = std::clamp(int(o.GetNamedNumber(L"s", 8 * 60)), 0, 24 * 60 - 5);
        int end = std::clamp(int(o.GetNamedNumber(L"e", start + 45)), start + 5, 24 * 60);
        w.content.periodStart[row] = start;
        w.content.periodEnd[row] = end;
        SaveSoon();
        SyncYunzhao(w);
    } else if (cmd == L"sign-out") {
        AccountSignOut();
    } else if (cmd == L"tool") {
        OpenTool(w, int(o.GetNamedNumber(L"kind", 0)));
    } else if (cmd == L"handout") {
        OpenHandout(w, int(o.GetNamedNumber(L"wd", -1)), int(o.GetNamedNumber(L"period", -1)));
    } else if (cmd == L"week") {
        ShiftShownMonth(w.content, int(o.GetNamedNumber(L"delta", 0)));
        SaveSoon();
        SyncYunzhao(w);
    } else if (cmd == L"focus") {
        StoreFocus(w.content, int(o.GetNamedNumber(L"y", 0)), int(o.GetNamedNumber(L"m", 0)), int(o.GetNamedNumber(L"d", 0)));
        SaveSoon();
        SyncYunzhao(w);
    }
}

void DesktopWidgets::PublishYunzhaoState(const Widget& w) const
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int y = 0, m = 0, d = 0;
    CalendarFocus(w.content, now, y, m, d);
    const char* layer = w.content.windowLayer == 2 ? "top" : w.content.windowLayer == 0 ? "bottom" : w.content.windowLayer == 1 ? "normal" : "";
    char buf[192];
    sprintf_s(buf, "{\"board\":%d,\"lock\":%s,\"layer\":\"%s\",\"ask\":%d,\"date\":\"%04d-%02d-%02d\",\"dateSize\":%.1f,\"tableSize\":%.1f,\"badgeScale\":%.3f}",
        w.content.calBoard == 2 ? 2 : 0, w.content.calLock ? "true" : "false", layer, w.noteAsk, y, m, d,
        world_.dateSize, world_.tableSize, world_.badgeScale);
    static std::string last;
    if (last != buf) {
        last = buf;
        std::wstring wide(buf, buf + strlen(buf));
        WriteTextFile(AccountDataDir() + L"\\yunzhao-state.json", wide);
    }

    json::JsonObject table;
    json::JsonArray lessons;
    for (const WidgetContent::Lesson& lesson : w.content.lessons) {
        json::JsonObject row;
        row.SetNamedValue(L"wd", json::JsonValue::CreateNumberValue(lesson.weekday));
        row.SetNamedValue(L"period", json::JsonValue::CreateNumberValue(lesson.period));
        row.SetNamedValue(L"name", json::JsonValue::CreateStringValue(lesson.name));
        row.SetNamedValue(L"teacher", json::JsonValue::CreateStringValue(lesson.teacher));
        lessons.Append(row);
    }
    json::JsonArray changes;
    for (const WidgetContent::LessonChange& change : w.content.changes) {
        json::JsonObject row;
        row.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(change.year));
        row.SetNamedValue(L"m", json::JsonValue::CreateNumberValue(change.month));
        row.SetNamedValue(L"d", json::JsonValue::CreateNumberValue(change.day));
        row.SetNamedValue(L"wd", json::JsonValue::CreateNumberValue(change.weekday));
        row.SetNamedValue(L"period", json::JsonValue::CreateNumberValue(change.period));
        row.SetNamedValue(L"off", json::JsonValue::CreateBooleanValue(change.off));
        row.SetNamedValue(L"name", json::JsonValue::CreateStringValue(change.name));
        row.SetNamedValue(L"teacher", json::JsonValue::CreateStringValue(change.teacher));
        changes.Append(row);
    }
    json::JsonArray periods;
    for (int i = 0; i < 8; ++i) {
        json::JsonObject row;
        row.SetNamedValue(L"s", json::JsonValue::CreateNumberValue(w.content.periodStart[i]));
        row.SetNamedValue(L"e", json::JsonValue::CreateNumberValue(w.content.periodEnd[i]));
        periods.Append(row);
    }
    json::JsonArray marks;
    for (const Reminder& item : w.content.items) {
        if (item.year < 1 || item.month < 1 || item.day < 1)
            continue;
        json::JsonObject row;
        row.SetNamedValue(L"y", json::JsonValue::CreateNumberValue(item.year));
        row.SetNamedValue(L"m", json::JsonValue::CreateNumberValue(item.month));
        row.SetNamedValue(L"d", json::JsonValue::CreateNumberValue(item.day));
        row.SetNamedValue(L"ey", json::JsonValue::CreateNumberValue(item.dueYear > 0 ? item.dueYear : item.year));
        row.SetNamedValue(L"em", json::JsonValue::CreateNumberValue(item.dueMonth > 0 ? item.dueMonth : item.month));
        row.SetNamedValue(L"ed", json::JsonValue::CreateNumberValue(item.dueDay > 0 ? item.dueDay : item.day));
        marks.Append(row);
    }
    table.SetNamedValue(L"lessons", lessons);
    table.SetNamedValue(L"changes", changes);
    table.SetNamedValue(L"periods", periods);
    table.SetNamedValue(L"marks", marks);
    const std::wstring tableText(table.Stringify());
    static std::wstring lastTable;
    if (lastTable != tableText) {
        lastTable = tableText;
        WriteTextFile(AccountDataDir() + L"\\yunzhao-table.json", tableText);
    }
}

void DesktopWidgets::PeekCalendar()
{
    for (auto& owned : widgets_) {
        Widget& w = *owned;
        if (w.kind != WidgetKind::Calendar || !w.surface)
            continue;
        const bool held = w.creatorHold;
        PollYunzhaoCommand(w);
        if (w.tucked || w.tuckShown > 0) {
            w.tucked = false;
            w.tuckWait = 0;
            w.tuckShown = 0;
        }
        OrderCalendarNote();
        if (held || w.creatorHold)
            SyncYunzhao(w);
    }
}

void DesktopWidgets::SyncYunzhao(Widget& w)
{
    if (!w.surface || w.kind != WidgetKind::Calendar)
        return;
    PollYunzhaoCommand(w);
    PublishYunzhaoState(w);
    OrderCalendarNote();
    const bool show = true;
    RECT body = w.surface->Rect();
    char payload[320];
    sprintf_s(payload, "{\"show\":%s,\"view\":\"keep\",\"left\":%ld,\"top\":%ld,\"right\":%ld,\"bottom\":%ld}",
        show ? "true" : "false", body.left, body.top, body.right, body.bottom);
    static std::string last;
    if (last == payload)
        return;
    last = payload;
    wchar_t base[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        return;
    std::wstring dir = std::wstring(base) + L"\\Yunzhao";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\yunzhao-place.json";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(file, payload, DWORD(strlen(payload)), &wrote, nullptr);
        CloseHandle(file);
    }
    static bool launched = false;
    if (launched)
        return;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring folder(exe);
    const size_t slash = folder.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        folder.resize(slash);
    auto resolve = [](const std::wstring& candidate) {
        wchar_t full[MAX_PATH]{};
        if (!GetFullPathNameW(candidate.c_str(), MAX_PATH, full, nullptr))
            return std::wstring();
        if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
            return std::wstring();
        return std::wstring(full);
    };
    std::wstring packaged = resolve(folder + L"\\Calendar\\YunzhaoNote.exe");
    if (!packaged.empty()) {
        std::wstring calendarDir = packaged.substr(0, packaged.find_last_of(L"\\/"));
        HINSTANCE startedPackaged = ShellExecuteW(nullptr, L"open", packaged.c_str(), nullptr, calendarDir.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(startedPackaged) > 32)
            launched = true;
        return;
    }
    std::wstring script = resolve(folder + L"\\..\\..\\..\\tools\\yunzhao-note.ps1");
    if (script.empty())
        script = resolve(folder + L"\\tools\\yunzhao-note.ps1");
    if (script.empty())
        return;
    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    HINSTANCE started = ShellExecuteW(nullptr, L"open", L"powershell.exe", args.c_str(), nullptr, SW_HIDE);
    if (reinterpret_cast<INT_PTR>(started) > 32)
        launched = true;
}

void DesktopWidgets::Tick()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    bool minute = t.wMinute != lastMinute_;
    lastMinute_ = t.wMinute;
    if ((minute || t.wSecond % 30 == 0) && onStatusRequest)
        onStatusRequest();
    bool sample = t.wSecond % 2 == 0;
    if (sample)
        SampleSystem();
    ULONGLONG now = GetTickCount64();
    // The forecast is fetched while a weather widget is on the desktop.
    weather_->SetActive(std::any_of(widgets_.begin(), widgets_.end(), [](const auto& w) { return w->kind == WidgetKind::Weather; }));
    // The Bluetooth batteries are read while a Bluetooth widget is.
    bluetooth_->SetActive(std::any_of(widgets_.begin(), widgets_.end(), [](const auto& w) { return w->kind == WidgetKind::Bluetooth; }));
    for (auto& w : widgets_) {
        if (!w->surface)
            continue;
        if (w->kind == WidgetKind::Calendar)
            SyncYunzhao(*w);
        if (w->kind == WidgetKind::Clock || minute || (sample && w->kind == WidgetKind::System))
            w->surface->Invalidate();
        if (w->kind == WidgetKind::Photos && w->photoAt && now - w->photoAt > kPhotoMs)
            LoadPhoto(*w, true);
        if (w->kind == WidgetKind::Focus && w->content.pomoRun) {
            long long left = (long long)w->pomoEnd - (long long)now;
            int sec = left > 0 ? int((left + 999) / 1000) : 0;
            w->content.pomoLeft = sec;
            pomoLeft_ = sec;
            if (pomo_)
                InvalidateRect(pomo_, nullptr, FALSE);
            w->surface->Invalidate();
            if (sec <= 0) {
                w->content.pomoRun = false;
                ClosePomo();
            }
        }
    }
}

void DesktopWidgets::SampleSystem()
{
    auto u64 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        uint64_t i = u64(idle), total = u64(kernel) + u64(user); // kernel time includes idle time
        if (cpuTotal_ && total > cpuTotal_)
            world_.cpu = std::clamp(1.f - float(double(i - cpuIdle_) / double(total - cpuTotal_)), 0.f, 1.f);
        cpuIdle_ = i;
        cpuTotal_ = total;
    }
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) {
        constexpr double kGb = 1024.0 * 1024 * 1024;
        world_.memory = float(ms.dwMemoryLoad) / 100.f;
        world_.memoryTotalGb = double(ms.ullTotalPhys) / kGb;
        world_.memoryGb = double(ms.ullTotalPhys - ms.ullAvailPhys) / kGb;
    }
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::wstring root = std::wstring(windows).substr(0, 3); // "C:\"
    ULARGE_INTEGER avail{}, total{}, free{};
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &free) && total.QuadPart) {
        world_.disk = 1.f - float(double(free.QuadPart) / double(total.QuadPart));
        world_.diskFreeGb = double(avail.QuadPart) / (1024.0 * 1024 * 1024);
    }
}

// ---- photos ------------------------------------------------------------------

void DesktopWidgets::PickFolder(Widget& w)
{
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return;
    FILEOPENDIALOGOPTIONS o = 0;
    dialog->GetOptions(&o);
    dialog->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"选择照片文件夹");
    Widget* wp = &w;
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(host_)) || FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    if (Has(wp)) { // the dialog ran its own message loop
        wp->content.folder = path;
        Save();
        LoadPhoto(*wp, true);
        Log(L"widgets: photos from a chosen folder");
    }
    CoTaskMemFree(path);
}

// another: a different picture of the folder; else the one shown, read again (a new size).
void DesktopWidgets::LoadPhoto(Widget& w, bool another)
{
    if (!w.surface)
        return;
    std::wstring folder = w.content.folder.empty() ? PicturesFolder() : w.content.folder;
    std::wstring current = w.content.photo;
    float s = Scale();
    UINT pw = UINT(std::lround(WidgetWidth(w.size) * s)), ph = UINT(std::lround(WidgetHeight(w.size) * s));
    unsigned gen = w.photoGen = ++photoGen_;
    w.photoAt = GetTickCount64(); // no second request while this one runs
    HWND host = host_;
    void* widget = &w;
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::wstring path = current;
        if (another || path.empty()) {
            std::vector<std::wstring> all;
            ListPhotos(folder, all);
            if (all.size() > 1)
                all.erase(std::remove(all.begin(), all.end(), current), all.end());
            path.clear();
            if (!all.empty()) {
                std::mt19937 rng{std::random_device{}()};
                path = all[std::uniform_int_distribution<size_t>(0, all.size() - 1)(rng)];
            }
        }
        auto* read = new PhotoRead{widget, gen, path, path.empty() ? IconImage{} : ReadPhoto(path, pw, ph)};
        CoUninitialize();
        if (!PostMessageW(host, WM_WIDGET_PHOTO, 0, reinterpret_cast<LPARAM>(read)))
            delete read;
    }).detach();
}

// ---- input -------------------------------------------------------------------

bool DesktopWidgets::Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r)
{
    switch (m) {
    case WM_WINDOWPOSCHANGING: {
        // Activation still gives editable widgets keyboard focus, without changing their layer.
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (!zMoving_)
            pos->flags |= SWP_NOZORDER;
        return false;
    }
    case WM_MOUSEWHEEL:
        if (w.kind != WidgetKind::Reminders)
            return false;
        w.content.scroll = std::clamp(w.content.scroll - float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * kReminderRow * 3,
                                      0.f, ReminderScrollMax(w.content, w.size));
        w.surface->Invalidate();
        return true;
    case WM_SETCURSOR: {
        if (w.kind == WidgetKind::Calendar && w.content.calLock)
            return false;
        POINT p;
        GetCursorPos(&p);
        if (w.kind == WidgetKind::Calendar && w.surface && !w.content.calLock) {
            float s = std::max(w.surface->Scale(), 0.01f);
            POINT local = w.surface->ScreenToContent(p);
            if (TopHitId(w.hits, float(local.x) / s, float(local.y) / s) == L"pull") {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                r = TRUE;
                return true;
            }
        }
        int grip = w.resizing ? (w.kind == WidgetKind::Calendar ? w.edges : (w.growLeft ? -1 : 1)) : w.down ? 0 : GripAt(w, p);
        if (LOWORD(lp) != HTCLIENT || !grip)
            return false;
        const wchar_t* cur = IDC_SIZENWSE;
        if (w.kind == WidgetKind::Calendar) {
            bool left = (grip & 1) != 0, right = (grip & 2) != 0, top = (grip & 4) != 0, bottom = (grip & 8) != 0;
            if ((left && bottom) || (right && top))
                cur = IDC_SIZENESW;
            else if ((left && top) || (right && bottom))
                cur = IDC_SIZENWSE;
            else if (left || right)
                cur = IDC_SIZEWE;
            else
                cur = IDC_SIZENS;
        } else if (grip < 0) {
            cur = IDC_SIZENESW;
        }
        SetCursor(LoadCursorW(nullptr, cur));
        r = TRUE;
        return true;
    }
    case WM_MOUSELEAVE:
        w.tracking = false;
        if (!w.hoverDay.empty())
            w.hoverDay.clear();
        if (w.content.pullHot && !w.pulling) {
            w.content.pullHot = false;
            if (w.surface)
                w.surface->Invalidate();
        }
        WatchPopup(w, {}, false);
        if (w.hover && !w.down) {
            w.hover = false;
            w.surface->Invalidate();
        } else if (w.surface) {
            w.surface->Invalidate();
        }
        return true;
    case WM_LBUTTONDOWN: {
        if (w.kind == WidgetKind::Calendar && w.content.calLock && w.surface) {
            float s = std::max(w.surface->Scale(), 0.01f);
            std::wstring hit = TopHitId(w.hits, float(GET_X_LPARAM(lp)) / s, float(GET_Y_LPARAM(lp)) / s);
            w.down = false;
            w.dragging = false;
            if (hit == L"lock") {
                w.lockHoldStart = GetTickCount64();
                w.content.lockPress = 0.02f;
                SetCapture(w.surface->Hwnd());
                SetTimer(host_, kLockTimer, 40, nullptr);
                w.surface->Invalidate();
            }
            return true;
        }
        w.down = true;
        w.dragging = false;
        w.zooming = false;
        if (w.kind == WidgetKind::Calendar && w.surface) {
            float s = std::max(w.surface->Scale(), 0.01f);
            std::wstring hit = TopHitId(w.hits, float(GET_X_LPARAM(lp)) / s, float(GET_Y_LPARAM(lp)) / s);
            if (hit == L"pull" && !w.content.calLock && (w.content.calBoard == 1 || w.content.calBoard == 2)) {
                w.pulling = true;
                w.resizing = false;
                SetCapture(w.surface->Hwnd());
                return true;
            }
            if (hit == L"zoom") {
                w.zooming = true;
                w.resizing = false;
                GetCursorPos(&w.downAt);
                w.downRect = w.surface->Rect();
                const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                w.surface->ReserveResize(D2D1::SizeF(float(vw) / s, float(vh) / s));
                SetCapture(w.surface->Hwnd());
                return true;
            }
        }
        GetCursorPos(&w.downAt);
        w.downRect = w.surface->Rect();
        int grip = GripAt(w, w.downAt);
        if (w.kind == WidgetKind::Calendar) {
            w.edges = grip;
            w.growLeft = false;
            w.resizing = grip != 0;
        } else {
            w.edges = 0;
            w.growLeft = grip < 0;
            w.resizing = grip != 0;
        }
        w.downSize = w.size;
        w.downCol = w.col;
        if (w.resizing) {
            EndEdit(w);
            w.glideMs = 0;
            RefreshIcons();
            if (w.kind == WidgetKind::Calendar) {
                const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                float s = std::max(w.surface->Scale(), 0.01f);
                w.surface->ReserveResize(D2D1::SizeF(float(vw) / s, float(vh) / s));
            } else {
                w.surface->ReserveResize(D2D1::SizeF(WidgetWidth(WidgetSupports(w.kind, WidgetSize::Medium) ? WidgetSize::Medium : WidgetSize::Small),
                                                    WidgetHeight(WidgetSupports(w.kind, WidgetSize::Large) ? WidgetSize::Large : WidgetSize::Small)));
            }
        }
        SetCapture(w.surface->Hwnd());
        return true;
    }
    case WM_MOUSEMOVE: {
        if (w.kind == WidgetKind::Calendar && w.content.calLock) {
            if (w.lockHoldStart && w.surface) {
                float s = std::max(w.surface->Scale(), 0.01f);
                std::wstring hit = TopHitId(w.hits, float(GET_X_LPARAM(lp)) / s, float(GET_Y_LPARAM(lp)) / s);
                if (hit != L"lock") {
                    w.lockHoldStart = 0;
                    w.content.lockPress = 0;
                    if (GetCapture() == w.surface->Hwnd())
                        ReleaseCapture();
                    w.surface->Invalidate();
                }
            }
            return true;
        }
        if (!w.tracking) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, w.surface->Hwnd(), 0};
            w.tracking = TrackMouseEvent(&t) != FALSE;
        }
        if (!w.hover) {
            w.hover = true;
            w.surface->Invalidate();
        }
        if (w.kind == WidgetKind::Calendar && w.surface && !w.pulling) {
            float s = std::max(w.surface->Scale(), 0.01f);
            bool hot = TopHitId(w.hits, float(GET_X_LPARAM(lp)) / s, float(GET_Y_LPARAM(lp)) / s) == L"pull";
            if (hot != w.content.pullHot) {
                w.content.pullHot = hot;
                w.surface->Invalidate();
            }
        }
        if (!w.dragging && !w.resizing && !w.zooming && !(w.kind == WidgetKind::Calendar && w.content.calLock))
            TrackDay(w, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (w.kind == WidgetKind::Calendar && !w.down)
            WatchPopup(w, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, true);
        if (w.pulling && w.surface) {
            float s = std::max(w.surface->Scale(), 0.01f);
            float y = float(GET_Y_LPARAM(lp)) / s;
            float face = std::max(1.f, w.spanH);
            float& pane = w.content.calBoard == 2 ? w.content.tablePane : w.content.listPane;
            pane = std::clamp((face - y) / face, 0.18f, 0.78f);
            w.content.pullHot = true;
            w.surface->Invalidate();
            return true;
        }
        if (!w.down)
            return true;
        if (w.zooming && w.surface) {
            POINT at;
            GetCursorPos(&at);
            float s = std::max(PopupScale(w.downRect), 0.01f);
            float width = float(w.downRect.right - w.downRect.left) / s;
            float x = float(at.x - w.downRect.left) / s;
            float left = 50.f, right = width - 50.f;
            float t = right > left + 8.f ? (x - left) / (right - left) : 0.5f;
            ApplyCalendarScale(w, t, false);
            return true;
        }
        POINT p;
        GetCursorPos(&p);
        if (w.resizing) {
            DragSize(w, p);
            if (w.kind == WidgetKind::Calendar)
                SyncYunzhao(w);
            return true;
        }
        int dx = p.x - w.downAt.x, dy = p.y - w.downAt.y;
        if (w.kind == WidgetKind::Calendar && w.content.calLock)
            return true;
        if (!w.dragging && std::abs(dx) + std::abs(dy) > int(6 * Scale())) {
            w.dragging = true;
            w.glideMs = 0;
            EndEdit(w);
            RefreshIcons();
            Restack(); // dragging stays on the desktop, below open application windows
        }
        if (w.dragging) {
            RECT moved{w.downRect.left + dx, w.downRect.top + dy, w.downRect.right + dx, w.downRect.bottom + dy};
            w.surface->Move(Nudge(moved, &w));
            if (w.kind == WidgetKind::Calendar)
                SyncYunzhao(w);
        }
        return true;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
        if (w.lockHoldStart || (w.kind == WidgetKind::Calendar && w.content.calLock && m == WM_LBUTTONUP)) {
            ULONGLONG held = w.lockHoldStart ? GetTickCount64() - w.lockHoldStart : 0;
            bool finish = m == WM_LBUTTONUP && w.lockHoldStart;
            w.lockHoldStart = 0;
            w.content.lockPress = 0;
            w.down = false;
            if (m == WM_LBUTTONUP && w.surface && GetCapture() == w.surface->Hwnd())
                ReleaseCapture();
            if (finish && held >= 3000 && w.content.calLock) {
                w.content.calLock = false;
                SaveSoon();
            }
            if (w.surface)
                w.surface->Invalidate();
            return true;
        }
        if (w.pulling) {
            w.pulling = false;
            w.down = false;
            if (m == WM_LBUTTONUP && w.surface && GetCapture() == w.surface->Hwnd())
                ReleaseCapture();
            Save();
            return true;
        }
        if (!w.down)
            return m == WM_LBUTTONUP;
        w.down = false;
        bool dragged = w.dragging, resized = w.resizing, zoomed = w.zooming;
        w.dragging = w.resizing = w.zooming = false;
        if (m == WM_LBUTTONUP && GetCapture() == w.surface->Hwnd())
            ReleaseCapture();
        POINT at;
        GetCursorPos(&at);
        if (RECT now = w.surface->Rect(); w.hover && !PtInRect(&now, at)) {
            w.hover = false; // let go outside it (its WM_MOUSELEAVE came while it was held)
            w.surface->Invalidate();
        }
        if (zoomed) {
            RECT cur = Settle(w.surface->Rect(), &w);
            Remember(w, cur);
            w.glideMs = 0;
            w.surface->Move(cur);
            w.spanW = float(cur.right - cur.left) / std::max(w.surface->Scale(), 0.01f);
            w.spanH = float(cur.bottom - cur.top) / std::max(w.surface->Scale(), 0.01f);
            w.content.calT = CalendarFraction(w);
            w.surface->ReserveResize(D2D1::SizeF());
            Save();
            Restack();
            return true;
        }
        if (resized || dragged) {
            RECT cur = ClampOntoMonitor(Settle(w.surface->Rect(), &w));
            if (Blocked(cur, &w) && !Blocked(w.downRect, &w))
                cur = w.downRect;
            Remember(w, cur);
            w.glideMs = 0;
            w.surface->Move(cur);
            w.surface->ReserveResize(D2D1::SizeF());
            if (resized && w.kind == WidgetKind::Photos)
                LoadPhoto(w, false);
            Save();
            Restack();
            Log(L"widgets: %s %s to %d,%d", NameOf(w.kind), resized ? L"resized" : L"moved", cur.left, cur.top);
        } else if (m == WM_LBUTTONUP) {
            POINT atContent{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (w.kind == WidgetKind::Calendar) {
                if (w.content.calLock) {
                    Click(w, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                    return true;
                }
                double now = ClockMs();
                int limX = std::max(1, GetSystemMetrics(SM_CXDOUBLECLK));
                int limY = std::max(1, GetSystemMetrics(SM_CYDOUBLECLK));
                bool dbl = w.clickTodo && now - w.clickMs <= double(GetDoubleClickTime()) && std::abs(atContent.x - w.clickPt.x) <= limX &&
                           std::abs(atContent.y - w.clickPt.y) <= limY;
                w.prevClickMs = w.clickMs;
                w.prevClickPt = w.clickPt;
                w.clickMs = now;
                w.clickPt = atContent;
                w.clickTodo = false;
                if (dbl && w.content.popupDay > 0) {
                    w.content.popupAll = true;
                    w.content.popupIndex = -1;
                    w.surface->Invalidate();
                } else {
                    Click(w, atContent);
                }
            } else if (Editable(w.kind) || w.kind == WidgetKind::Marks || w.kind == WidgetKind::Habits || w.kind == WidgetKind::Focus) {
                Click(w, atContent);
            } else {
                Activate(w);
            }
        }
        return true;
    }
    case WM_RBUTTONUP:
        if (!(w.kind == WidgetKind::Calendar && w.content.calLock))
            Menu(w);
        return true;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        return true;
    case WM_KEYDOWN:
        if (w.kind == WidgetKind::Calendar && w.content.calLock)
            return true;
        if (wp == VK_ESCAPE && w.kind == WidgetKind::Calendar && w.content.popupDay > 0 && !w.content.editing) {
            w.content.popupDay = 0;
            w.content.popupIndex = -1;
            w.content.popupAll = false;
            w.surface->Invalidate();
            return true;
        }
        return w.content.editing && Key(w, m, wp);
    case WM_CHAR:
        if (w.kind == WidgetKind::Calendar && w.content.calLock)
            return true;
        return w.content.editing && Key(w, m, wp);
    case WM_KILLFOCUS:
        EndEdit(w);
        return false;
    case WM_IME_STARTCOMPOSITION:
        PlaceImeWindow(w);
        return false; // the input method's own window
    }
    return false;
}

void DesktopWidgets::Activate(Widget& w)
{
    POINT p;
    GetCursorPos(&p);
    POINT c = w.surface->ScreenToContent(p);
    float s = w.surface->Scale();
    w.surface->Ripple(float(c.x) / s, float(c.y) / s);
    std::wstring open;
    switch (w.kind) {
    case WidgetKind::Clock:
    case WidgetKind::Digital:
        open = L"ms-clock:";
        break;
    case WidgetKind::Calendar:
        break; // a click edits that day's todo; the bar's clock still opens the month
    case WidgetKind::Battery:
        open = world_.status.battery ? L"ms-settings:batterysaver" : L"ms-settings:powersleep";
        break;
    case WidgetKind::System:
        open = L"taskmgr.exe";
        break;
    case WidgetKind::Bluetooth:
        open = L"ms-settings:bluetooth"; // Bluetooth & devices: each device with its battery
        break;
    case WidgetKind::Photos:
        if (w.content.photo.empty())
            PickFolder(w);
        else
            open = w.content.photo;
        break;
    case WidgetKind::Weather:
        // Windows' weather app, else its web page.
        if (INT_PTR(ShellExecuteW(nullptr, L"open", L"msnweather:", nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            open = L"https://www.msn.cn/zh-cn/weather/forecast";
        break;
    default:
        break;
    }
    if (!open.empty())
        ShellExecuteW(nullptr, L"open", open.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"widgets: %s opened", NameOf(w.kind));
}

std::wstring DesktopWidgets::DayAt(const Widget& w, POINT content) const
{
    float s = w.surface ? w.surface->Scale() : 1;
    D2D1_POINT_2F p = D2D1::Point2F(float(content.x) / s, float(content.y) / s);
    std::wstring id;
    for (auto it = w.hits.rbegin(); it != w.hits.rend(); ++it) {
        if (it->id.rfind(L"day:", 0) != 0)
            continue;
        if (p.x >= it->rect.left && p.x < it->rect.right && p.y >= it->rect.top && p.y < it->rect.bottom)
            return it->id;
    }
    return id;
}

void DesktopWidgets::TrackDay(Widget& w, POINT content)
{
    if (w.kind != WidgetKind::Calendar || w.content.editing)
        return;
    std::wstring id = DayAt(w, content);
    if (id == w.hoverDay)
        return;
    w.hoverDay = std::move(id);
    if (w.surface)
        w.surface->Invalidate();
}

void DesktopWidgets::Click(Widget& w, POINT content)
{
    if (w.kind == WidgetKind::Calendar) {
        float s = w.surface ? w.surface->Scale() : 1;
        D2D1_POINT_2F p = D2D1::Point2F(float(content.x) / s, float(content.y) / s);
        auto inside = [&](const WidgetHit& hit) { return p.x >= hit.rect.left && p.x < hit.rect.right && p.y >= hit.rect.top && p.y < hit.rect.bottom; };
        auto toggleLock = [&] {
            if (w.content.calLock)
                return; // a short click only locks; holding the icon for 3s unlocks
            w.content.calLock = true;
            if (w.content.calLock) {
                EndEdit(w);
                w.content.tableEdit = false;
                w.content.toolsOpen = false;
                w.content.editPeriod = -1;
                w.content.pullHot = false;
                w.hoverDay.clear();
                w.content.popupDay = 0;
                w.content.popupIndex = -1;
                w.content.popupAll = false;
                w.content.popupPin = false;
                w.popupLeaveMs = 0;
                SyncPopupHook();
            }
            SaveSoon();
            w.surface->Invalidate();
        };
        if (w.content.calLock)
            return;
        auto closePopup = [&] {
            bool was = w.content.popupDay > 0;
            w.content.popupDay = 0;
            w.content.popupIndex = -1;
            w.content.popupAll = false;
            w.content.popupPin = false;
            w.popupLeaveMs = 0;
            if (was)
                SyncPopupHook();
        };
        auto placePopup = [&](int y, int m, int day, int index, bool all) {
            EndEdit(w);
            w.content.popupYear = y;
            w.content.popupMonth = m;
            w.content.popupDay = day;
            w.content.popupIndex = index;
            w.content.popupAll = all;
            w.content.popupPin = w.content.calBoard == 2;
            w.popupLeaveMs = 0;
            w.content.popupX = p.x;
            w.content.popupY = p.y;
            w.hoverDay.clear();
            w.surface->Invalidate();
            SyncPopupHook();
        };
        std::wstring topId;
        for (auto it = w.hits.rbegin(); it != w.hits.rend(); ++it) {
            if (inside(*it)) {
                topId = it->id;
                break;
            }
        }
        if (w.content.toolsOpen && topId != L"tools" && topId.rfind(L"tool:", 0) != 0) {
            w.content.toolsOpen = false;
            w.surface->Invalidate();
        }
        if (w.content.layerOpen && topId != L"layer" && topId.rfind(L"z:", 0) != 0) {
            w.content.layerOpen = false;
            w.surface->Invalidate();
        }
        bool timeHit = topId.rfind(L"time:", 0) == 0 || topId.rfind(L"ts:", 0) == 0 || topId.rfind(L"te:", 0) == 0 || topId == L"timekeep";
        if (!timeHit && w.content.editPeriod >= 0) {
            w.content.editPeriod = -1;
            w.surface->Invalidate();
        }
        auto revertTemplate = [&] {
            WidgetContent& c = w.content;
            if (c.snapNew) {
                if (c.item >= 0 && c.item < int(c.lessons.size()))
                    c.lessons.erase(c.lessons.begin() + c.item);
                c.item = -1;
                c.snapNew = false;
            } else if (c.item >= 0 && c.item < int(c.lessons.size())) {
                c.lessons[size_t(c.item)].name = c.snapName;
                c.lessons[size_t(c.item)].teacher = c.snapTeacher;
            }
        };
        auto finishCourse = [&] {
            WidgetContent& c = w.content;
            c.editing = false;
            c.editLesson = false;
            c.editPart = 0;
            c.editScope = 0;
            c.editChange = -1;
            c.snapNew = false;
            c.snapName.clear();
            c.snapTeacher.clear();
            c.item = -1;
            KillTimer(host_, kCaretTimer);
            Save();
            if (w.surface)
                w.surface->Invalidate();
        };
        auto upsertChange = [&](bool off, const std::wstring& name, const std::wstring& teacher) {
            WidgetContent& c = w.content;
            int ch = FindLessonChange(c, c.editWeekY, c.editWeekM, c.editWeekD, c.editWd, c.editSlot);
            if (ch < 0) {
                WidgetContent::LessonChange change;
                change.year = c.editWeekY;
                change.month = c.editWeekM;
                change.day = c.editWeekD;
                change.weekday = c.editWd;
                change.period = c.editSlot;
                c.changes.push_back(std::move(change));
                ch = int(c.changes.size()) - 1;
            }
            WidgetContent::LessonChange& slot = c.changes[size_t(ch)];
            slot.off = off;
            slot.name = off ? L"" : name;
            slot.teacher = off ? L"" : teacher;
            return ch;
        };
        auto applyScope = [&](int scope) {
            WidgetContent& c = w.content;
            if (!c.editLesson || !c.editing)
                return;
            if (scope == 0) {
                if (c.editScope == 0)
                    return;
                std::wstring name, teacher;
                if (c.editChange >= 0 && c.editChange < int(c.changes.size())) {
                    name = c.changes[size_t(c.editChange)].name;
                    teacher = c.changes[size_t(c.editChange)].teacher;
                    c.changes.erase(c.changes.begin() + c.editChange);
                }
                c.editChange = -1;
                c.editScope = 0;
                int index = -1;
                for (int i = 0; i < int(c.lessons.size()); ++i)
                    if (c.lessons[size_t(i)].weekday == c.editWd && c.lessons[size_t(i)].period == c.editSlot)
                        index = i;
                if (index < 0) {
                    WidgetContent::Lesson lesson;
                    lesson.weekday = c.editWd;
                    lesson.period = c.editSlot;
                    lesson.name = name;
                    lesson.teacher = teacher;
                    c.lessons.push_back(std::move(lesson));
                    index = int(c.lessons.size()) - 1;
                } else {
                    c.lessons[size_t(index)].name = name;
                    c.lessons[size_t(index)].teacher = teacher;
                }
                c.snapNew = c.snapName.empty() && c.snapTeacher.empty();
                c.item = index;
            } else if (scope == 1) {
                bool off = c.editScope == 1 && c.editChange >= 0 && c.editChange < int(c.changes.size()) && c.changes[size_t(c.editChange)].off;
                if (c.editScope == 1 && !off)
                    return;
                if (off) {
                    c.changes[size_t(c.editChange)].off = false;
                    w.surface->Invalidate();
                    return;
                }
                std::wstring name, teacher;
                if (c.item >= 0 && c.item < int(c.lessons.size())) {
                    name = c.lessons[size_t(c.item)].name;
                    teacher = c.lessons[size_t(c.item)].teacher;
                }
                revertTemplate();
                c.editChange = upsertChange(false, name, teacher);
                c.editScope = 1;
                c.item = -1;
            } else if (scope == 2) {
                if (c.editScope == 0)
                    revertTemplate();
                upsertChange(true, L"", L"");
                finishCourse();
                return;
            } else if (scope == 3) {
                if (c.editScope == 0)
                    revertTemplate();
                int ch = FindLessonChange(c, c.editWeekY, c.editWeekM, c.editWeekD, c.editWd, c.editSlot);
                if (ch >= 0)
                    c.changes.erase(c.changes.begin() + ch);
                finishCourse();
                return;
            }
            if (std::wstring* buffer = EditBuffer(w))
                c.caret = buffer->size();
            w.surface->Invalidate();
        };
        for (auto it = w.hits.rbegin(); it != w.hits.rend(); ++it) {
            if (!inside(*it))
                continue;
            if (it->id == L"field" || it->id == L"popup" || it->id.rfind(L"sel:", 0) == 0)
                return;
            if (it->id == L"lock") {
                toggleLock();
                return;
            }
            if (it->id.rfind(L"pri:", 0) == 0) {
                int pri = std::clamp(_wtoi(it->id.c_str() + 4), 0, 3);
                if (w.content.editing && w.content.item >= 0 && w.content.item < int(w.content.items.size()))
                    w.content.items[size_t(w.content.item)].pri = pri;
                w.surface->Invalidate();
                return;
            }
            if (it->id.rfind(L"check:", 0) == 0) {
                int index = _wtoi(it->id.c_str() + 6);
                if (index >= 0 && index < int(w.content.items.size()) && !w.content.items[size_t(index)].text.empty()) {
                    w.content.items[size_t(index)].done = !w.content.items[size_t(index)].done;
                    SaveSoon();
                    w.surface->Invalidate();
                }
                return;
            }
            if (it->id.rfind(L"view:", 0) == 0) {
                int view = _wtoi(it->id.c_str() + 5);
                if (view >= 0 && view <= 2)
                    w.content.calView = view;
                closePopup();
                EndEdit(w);
                w.surface->Invalidate();
                SaveSoon();
                return;
            }
            if (it->id.rfind(L"due:", 0) == 0) {
                int delta = _wtoi(it->id.c_str() + 4);
                if (w.content.editing && w.content.item >= 0 && w.content.item < int(w.content.items.size()) && delta != 0) {
                    Reminder& item = w.content.items[size_t(w.content.item)];
                    int y = item.dueYear ? item.dueYear : item.year;
                    int m = item.dueMonth ? item.dueMonth : item.month;
                    int d = item.dueDay ? item.dueDay : item.day;
                    ShiftDate(y, m, d, delta);
                    item.dueYear = y;
                    item.dueMonth = m;
                    item.dueDay = d;
                    SaveSoon();
                    w.surface->Invalidate();
                }
                return;
            }
            if (it->id.rfind(L"layout:", 0) == 0) {
                int board = std::clamp(_wtoi(it->id.c_str() + 7), 0, 2);
                w.content.calBoard = board;
                w.content.calSplit = board == 1;
                w.content.tableEdit = false;
                w.content.editPeriod = -1;
                closePopup();
                EndEdit(w);
                w.surface->Invalidate();
                SaveSoon();
                SyncYunzhao(w);
                return;
            }
            auto editCourse = [&](int day, int period, int part) {
                if (day < 0 || day > 6 || period < 0 || period > 7)
                    return;
                EndEdit(w);
                closePopup();
                SYSTEMTIME now{};
                GetLocalTime(&now);
                int wy = 0, wm = 0, wd = 0;
                CalendarFocus(w.content, now, wy, wm, wd);
                StepDays(wy, wm, wd, -Weekday(wy, wm, wd));
                WidgetContent& c = w.content;
                c.editWeekY = wy;
                c.editWeekM = wm;
                c.editWeekD = wd;
                c.editWd = day;
                c.editSlot = period;
                int change = FindLessonChange(c, wy, wm, wd, day, period);
                int index = -1;
                for (int i = 0; i < int(c.lessons.size()); ++i)
                    if (c.lessons[size_t(i)].weekday == day && c.lessons[size_t(i)].period == period)
                        index = i;
                if (index >= 0) {
                    c.snapName = c.lessons[size_t(index)].name;
                    c.snapTeacher = c.lessons[size_t(index)].teacher;
                    c.snapNew = false;
                } else {
                    c.snapName.clear();
                    c.snapTeacher.clear();
                    c.snapNew = false;
                }
                c.editLesson = true;
                c.editPart = part;
                c.editX = p.x;
                c.editY = p.y;
                if (change >= 0) {
                    c.editScope = 1;
                    c.editChange = change;
                    StartEdit(w, -1);
                } else {
                    if (index < 0) {
                        WidgetContent::Lesson lesson;
                        lesson.weekday = day;
                        lesson.period = period;
                        c.lessons.push_back(std::move(lesson));
                        index = int(c.lessons.size()) - 1;
                        c.snapNew = true;
                    }
                    c.editScope = 0;
                    c.editChange = -1;
                    StartEdit(w, index);
                }
                c.editPart = part;
                if (std::wstring* buffer = EditBuffer(w))
                    c.caret = buffer->size();
                w.surface->Invalidate();
            };
            if (it->id.rfind(L"head:", 0) == 0) {
                int y = 0, m = 0, day = 0;
                if (swscanf_s(it->id.c_str() + 5, L"%d-%d-%d", &y, &m, &day) == 3) {
                    int limX = std::max(1, GetSystemMetrics(SM_CXDOUBLECLK));
                    int limY = std::max(1, GetSystemMetrics(SM_CYDOUBLECLK));
                    bool sameSpot = std::abs(content.x - w.prevClickPt.x) <= limX && std::abs(content.y - w.prevClickPt.y) <= limY;
                    bool dbl = w.clickHead == it->id && (w.clickMs - w.prevClickMs) <= double(GetDoubleClickTime()) && sameSpot && w.prevClickMs > 0;
                    w.clickHead = it->id;
                    w.content.shownYear = y;
                    w.content.shownMonth = m;
                    w.content.focusDay = day;
                    closePopup();
                    if (dbl)
                        placePopup(y, m, day, -1, true);
                    else
                        EndEdit(w);
                    w.surface->Invalidate();
                }
                return;
            }
            if (it->id.rfind(L"scope:", 0) == 0) {
                applyScope(_wtoi(it->id.c_str() + 6));
                return;
            }
            if (it->id.rfind(L"time:", 0) == 0 || it->id.rfind(L"ts:", 0) == 0 || it->id.rfind(L"te:", 0) == 0 || it->id == L"timekeep") {
                if (!w.content.tableEdit)
                    return;
                closePopup();
                EndEdit(w);
                if (it->id == L"timekeep")
                    return;
                if (it->id.rfind(L"time:", 0) == 0) {
                    int row = _wtoi(it->id.c_str() + 5);
                    if (row >= 0 && row < 8) {
                        if (w.content.periodStart[row] < 0) {
                            w.content.periodStart[row] = 8 * 60;
                            w.content.periodEnd[row] = 8 * 60 + 45;
                            SaveSoon();
                        }
                        w.content.editPeriod = row;
                        w.surface->Invalidate();
                    }
                    return;
                }
                int row = w.content.editPeriod;
                if (row >= 0 && row < 8) {
                    int delta = _wtoi(it->id.c_str() + 3);
                    int& slot = it->id[1] == L's' ? w.content.periodStart[row] : w.content.periodEnd[row];
                    if (slot < 0)
                        slot = 8 * 60;
                    slot = std::clamp(slot + delta, 0, 24 * 60 - 5);
                    if (w.content.periodEnd[row] < w.content.periodStart[row])
                        w.content.periodEnd[row] = std::min(24 * 60 - 1, w.content.periodStart[row] + 40);
                    SaveSoon();
                    w.surface->Invalidate();
                }
                return;
            }
            if (it->id.rfind(L"slot:", 0) == 0 || it->id.rfind(L"teach:", 0) == 0) {
                int day = 0, period = 0;
                const wchar_t* body = it->id.c_str() + (it->id.rfind(L"teach:", 0) == 0 ? 6 : 5);
                if (swscanf_s(body, L"%d:%d", &day, &period) != 2)
                    return;
                if (!w.content.tableEdit) {
                    wchar_t key[24];
                    swprintf_s(key, L"course:%d:%d", day, period);
                    int limX = std::max(1, GetSystemMetrics(SM_CXDOUBLECLK));
                    int limY = std::max(1, GetSystemMetrics(SM_CYDOUBLECLK));
                    bool sameSpot = std::abs(content.x - w.prevClickPt.x) <= limX && std::abs(content.y - w.prevClickPt.y) <= limY;
                    bool dbl = w.clickHead == key && (w.clickMs - w.prevClickMs) <= double(GetDoubleClickTime()) && sameSpot && w.prevClickMs > 0;
                    w.clickHead = key;
                    if (dbl)
                        OpenHandout(w, day, period);
                    return;
                }
                editCourse(day, period, it->id.rfind(L"teach:", 0) == 0 ? 1 : 0);
                return;
            }
            if (it->id == L"tools") {
                w.content.toolsOpen = !w.content.toolsOpen;
                w.content.layerOpen = false;
                w.surface->Invalidate();
                return;
            }
            if (it->id == L"layer") {
                w.content.layerOpen = !w.content.layerOpen;
                w.content.toolsOpen = false;
                w.surface->Invalidate();
                return;
            }
            if (it->id.rfind(L"z:", 0) == 0) {
                const std::wstring which = it->id.substr(2);
                w.content.windowLayer = which == L"top" ? 2 : which == L"bottom" ? 0 : 1;
                w.content.layerOpen = false;
                PublishYunzhaoState(w);
                Restack();
                return;
            }
            if (it->id.rfind(L"tool:", 0) == 0) {
                int which = std::clamp(_wtoi(it->id.c_str() + 5), 0, 4);
                w.content.toolsOpen = false;
                OpenTool(w, which);
                return;
            }
            if (it->id == L"tableedit") {
                if (w.content.tableEdit) {
                    EndEdit(w);
                    w.content.tableEdit = false;
                    w.content.editPeriod = -1;
                } else {
                    EndEdit(w);
                    closePopup();
                    w.content.tableEdit = true;
                }
                SaveSoon();
                w.surface->Invalidate();
                return;
            }
            if (it->id == L"style") {
                OpenStyle(w);
                return;
            }
            if (it->id == L"newtodo") {
                EndEdit(w);
                closePopup();
                w.noteAsk++;
                w.creatorHold = w.content.calBoard == 2;
                PublishYunzhaoState(w);
                OrderCalendarNote();
                return;
            }
            if (it->id.rfind(L"add:", 0) == 0)
                return; // new todos open only from the button beside the switch
            if (it->id == L"pull")
                return;
            if (it->id.rfind(L"day:", 0) == 0) {
                int y = 0, m = 0, day = 0;
                if (swscanf_s(it->id.c_str() + 4, L"%d-%d-%d", &y, &m, &day) == 3) {
                    int limX = std::max(1, GetSystemMetrics(SM_CXDOUBLECLK));
                    int limY = std::max(1, GetSystemMetrics(SM_CYDOUBLECLK));
                    bool sameSpot = std::abs(content.x - w.prevClickPt.x) <= limX && std::abs(content.y - w.prevClickPt.y) <= limY;
                    bool dbl = w.clickHead == it->id && (w.clickMs - w.prevClickMs) <= double(GetDoubleClickTime()) && sameSpot && w.prevClickMs > 0;
                    w.clickHead = it->id;
                    EndEdit(w);
                    closePopup();
                    w.content.shownYear = y;
                    w.content.shownMonth = m;
                    w.content.focusDay = day;
                    if (dbl)
                        placePopup(y, m, day, -1, true);
                    SaveSoon();
                    w.surface->Invalidate();
                }
                return;
            }
            if (it->id.rfind(L"todo:", 0) == 0) {
                int index = _wtoi(it->id.c_str() + 5);
                if (index >= 0 && index < int(w.content.items.size())) {
                    const Reminder item = w.content.items[size_t(index)];
                    if (w.content.editing && w.content.item >= 0 && w.content.item < index &&
                        w.content.items[size_t(w.content.item)].text.empty())
                        --index;
                    placePopup(item.year, item.month, item.day, index, false);
                    w.clickTodo = true;
                }
                return;
            }
            if (it->id.rfind(L"todos:", 0) == 0) {
                int y = 0, m = 0, day = 0;
                if (swscanf_s(it->id.c_str() + 6, L"%d-%d-%d", &y, &m, &day) == 3)
                    placePopup(y, m, day, -1, true);
                return;
            }
            if (it->id.rfind(L"month:", 0) == 0) {
                EndEdit(w);
                closePopup();
                ShiftShownMonth(w.content, _wtoi(it->id.c_str() + 6));
                w.hoverDay.clear();
                w.surface->Invalidate();
                return;
            }
        }
        bool hadPopup = w.content.popupDay > 0;
        closePopup();
        if (hadPopup)
            w.surface->Invalidate();
        std::wstring id = DayAt(w, content);
        int y = 0, m = 0, day = 0;
        if (id.empty() || swscanf_s(id.c_str() + 4, L"%d-%d-%d", &y, &m, &day) != 3) {
            EndEdit(w);
            return;
        }
        if (w.content.editing && w.content.item >= 0 && w.content.item < int(w.content.items.size())) {
            const Reminder& cur = w.content.items[size_t(w.content.item)];
            if (cur.year == y && cur.month == m && cur.day == day)
                return;
        }
        EndEdit(w);
        w.content.shownYear = y;
        w.content.shownMonth = m;
        w.content.focusDay = day;
        SaveSoon();
        w.surface->Invalidate();
        return;
    }
    if (w.kind == WidgetKind::Schedule || w.kind == WidgetKind::Marks || w.kind == WidgetKind::Habits || w.kind == WidgetKind::Focus) {
        float s = w.surface ? w.surface->Scale() : 1;
        D2D1_POINT_2F p = D2D1::Point2F(float(content.x) / s, float(content.y) / s);
        std::wstring id;
        for (auto it = w.hits.rbegin(); it != w.hits.rend(); ++it) {
            if (it->id == L"caret")
                continue;
            if (p.x >= it->rect.left && p.x < it->rect.right && p.y >= it->rect.top && p.y < it->rect.bottom) {
                id = it->id;
                break;
            }
        }
        WidgetContent& c = w.content;
        if (id.rfind(L"wd:", 0) == 0) {
            EndEdit(w);
            c.focusDay = _wtoi(id.c_str() + 3);
            SaveSoon();
            w.surface->Invalidate();
            return;
        }
        if (id.rfind(L"name:", 0) == 0) {
            StartEdit(w, _wtoi(id.c_str() + 5));
            return;
        }
        if (id.rfind(L"toggle:", 0) == 0 && w.kind == WidgetKind::Habits) {
            int i = _wtoi(id.c_str() + 7);
            if (i >= 0 && i < int(c.habits.size())) {
                auto& habit = c.habits[size_t(i)];
                SYSTEMTIME t{};
                GetLocalTime(&t);
                bool today = habit.year == int(t.wYear) && habit.month == int(t.wMonth) && habit.day == int(t.wDay);
                if (today) {
                    habit.year = habit.month = habit.day = 0;
                    habit.streak = std::max(0, habit.streak - 1);
                } else {
                    FILETIME f{};
                    SystemTimeToFileTime(&t, &f);
                    ULARGE_INTEGER u{};
                    u.LowPart = f.dwLowDateTime;
                    u.HighPart = f.dwHighDateTime;
                    u.QuadPart -= 864000000000ULL;
                    FILETIME back{u.LowPart, u.HighPart};
                    SYSTEMTIME y{};
                    FileTimeToSystemTime(&back, &y);
                    bool cont = habit.year == int(y.wYear) && habit.month == int(y.wMonth) && habit.day == int(y.wDay);
                    habit.streak = cont ? habit.streak + 1 : 1;
                    habit.year = int(t.wYear);
                    habit.month = int(t.wMonth);
                    habit.day = int(t.wDay);
                }
                SaveSoon();
            }
            w.surface->Invalidate();
            return;
        }
        if (id == L"add") {
            EndEdit(w);
            SYSTEMTIME t{};
            GetLocalTime(&t);
            if (w.kind == WidgetKind::Schedule) {
                int day = c.focusDay >= 1 && c.focusDay <= 7 ? c.focusDay - 1 : (int(t.wDayOfWeek) + 6) % 7;
                int start = std::min(23 * 60, (int(t.wHour) + 1) * 60);
                WidgetContent::Lesson lesson;
                lesson.weekday = day;
                lesson.period = std::clamp(t.wHour < 13 ? int(t.wHour) - 8 : 5 + int(t.wHour) - 13, 0, 7);
                lesson.startMin = start;
                lesson.endMin = std::min(24 * 60, start + 45);
                c.lessons.push_back(std::move(lesson));
                StartEdit(w, int(c.lessons.size()) - 1);
            } else if (w.kind == WidgetKind::Marks) {
                c.marks.push_back(WidgetContent::Mark{L"", int(t.wYear), int(t.wMonth), int(t.wDay), true});
                StartEdit(w, int(c.marks.size()) - 1);
            } else {
                c.habits.push_back({});
                StartEdit(w, int(c.habits.size()) - 1);
            }
            return;
        }
        if (w.kind == WidgetKind::Focus && !c.pomoRun) {
            if (id == L"start") {
                OpenPomo(w);
                return;
            }
            if (id == L"shorter" || id == L"longer") {
                c.pomoMin = std::clamp(c.pomoMin + (id == L"longer" ? 5 : -5), 5, 180);
                SaveSoon();
                w.surface->Invalidate();
                return;
            }
        }
        EndEdit(w);
        return;
    }
    if (w.kind == WidgetKind::Notes) {
        StartEdit(w, -1);
        return;
    }
    float s = w.surface->Scale();
    D2D1_POINT_2F p = D2D1::Point2F(float(content.x) / s, float(content.y) / s);
    std::wstring id;
    for (const WidgetHit& h : w.hits)
        if (h.id != L"caret" && p.x >= h.rect.left && p.x < h.rect.right && p.y >= h.rect.top && p.y < h.rect.bottom) {
            id = h.id;
            break;
        }
    WidgetContent& c = w.content;
    if (id.rfind(L"toggle:", 0) == 0) {
        size_t i = size_t(_wtoi(id.c_str() + 7));
        if (c.editing && c.item == int(i))
            EndEdit(w);
        if (i < c.items.size() && !c.items[i].text.empty()) {
            c.items[i].done = !c.items[i].done;
            SaveSoon();
        }
        w.surface->Invalidate();
    } else if (id.rfind(L"item:", 0) == 0) {
        StartEdit(w, _wtoi(id.c_str() + 5));
    } else if (id == L"add") {
        EndEdit(w);
        c.items.push_back({});
        StartEdit(w, int(c.items.size()) - 1);
    } else {
        EndEdit(w);
    }
}

std::wstring* DesktopWidgets::EditBuffer(Widget& w)
{
    WidgetContent& c = w.content;
    auto ok = [](int i, int n) { return i >= 0 && i < n; };
    if (w.kind == WidgetKind::Calendar && c.editLesson) {
        if (c.editScope == 1) {
            if (!ok(c.editChange, int(c.changes.size())))
                return nullptr;
            WidgetContent::LessonChange& change = c.changes[size_t(c.editChange)];
            return c.editPart == 1 ? &change.teacher : &change.name;
        }
        if (!ok(c.item, int(c.lessons.size())))
            return nullptr;
        WidgetContent::Lesson& lesson = c.lessons[size_t(c.item)];
        return c.editPart == 1 ? &lesson.teacher : &lesson.name;
    }
    switch (w.kind) {
    case WidgetKind::Notes:
        return &c.text;
    case WidgetKind::Schedule:
        return ok(c.item, int(c.lessons.size())) ? &c.lessons[size_t(c.item)].name : nullptr;
    case WidgetKind::Marks:
        return ok(c.item, int(c.marks.size())) ? &c.marks[size_t(c.item)].name : nullptr;
    case WidgetKind::Habits:
        return ok(c.item, int(c.habits.size())) ? &c.habits[size_t(c.item)].name : nullptr;
    default:
        return ok(c.item, int(c.items.size())) ? &c.items[size_t(c.item)].text : nullptr;
    }
}

void DesktopWidgets::StartEdit(Widget& w, int item)
{
    WidgetContent& c = w.content;
    bool lesson = c.editLesson;
    if (c.editing && c.item != item) {
        // Another reminder: an empty one being left goes, which may move this one up.
        if (w.kind == WidgetKind::Reminders && c.item >= 0 && c.item < item && c.items[size_t(c.item)].text.empty())
            --item;
        EndEdit(w);
        c.editLesson = lesson;
    }
    if (c.editLesson) {
        if (c.editScope == 1) {
            if (c.editChange < 0 || c.editChange >= int(c.changes.size())) {
                c.editLesson = false;
                c.editScope = 0;
                return;
            }
        } else if (item < 0 || item >= int(c.lessons.size())) {
            c.editLesson = false;
            return;
        }
    } else if ((w.kind == WidgetKind::Reminders || w.kind == WidgetKind::Calendar) && (item < 0 || item >= int(c.items.size())))
        return;
    c.editing = true;
    c.item = item;
    std::wstring* buffer = EditBuffer(w);
    if (!buffer) {
        c.editing = false;
        c.item = -1;
        return;
    }
    c.caret = buffer->size();
    c.caretOn = true;
    if (w.kind == WidgetKind::Reminders) {
        float view = WidgetHeight(w.size) - kReminderTop - kReminderBottom;
        float top = float(item) * kReminderRow;
        c.scroll = std::clamp(c.scroll, std::max(0.f, top + kReminderRow - view), top);
    }
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr);
    if (GetForegroundWindow() != w.surface->Hwnd())
        SetForegroundWindow(w.surface->Hwnd());
    SetFocus(w.surface->Hwnd());
    w.surface->Invalidate();
}

void DesktopWidgets::EndEdit(Widget& w)
{
    WidgetContent& c = w.content;
    if (!c.editing)
        return;
    c.editing = false;
    if (c.editLesson) {
        if (c.editScope == 1 && c.editChange >= 0 && c.editChange < int(c.changes.size())) {
            WidgetContent::LessonChange& change = c.changes[size_t(c.editChange)];
            if (change.off && change.name.empty() && change.teacher.empty()) {
            } else if (change.name.empty() && change.teacher.empty()) {
                c.changes.erase(c.changes.begin() + c.editChange);
            } else if (!change.off && change.name == c.snapName && change.teacher == c.snapTeacher) {
                c.changes.erase(c.changes.begin() + c.editChange);
            } else {
                change.off = false;
            }
        } else if (c.item >= 0 && c.item < int(c.lessons.size()) && c.lessons[size_t(c.item)].name.empty() &&
                   c.lessons[size_t(c.item)].teacher.empty()) {
            c.lessons.erase(c.lessons.begin() + c.item);
        }
        c.editLesson = false;
        c.editPart = 0;
        c.editScope = 0;
        c.editChange = -1;
        c.snapNew = false;
        c.snapName.clear();
        c.snapTeacher.clear();
        c.item = -1;
    }
    auto dropEmpty = [&](auto& list) {
        if (c.item >= 0 && c.item < int(list.size()) && list[size_t(c.item)].name.empty())
            list.erase(list.begin() + c.item);
    };
    if (w.kind == WidgetKind::Schedule)
        dropEmpty(c.lessons);
    else if (w.kind == WidgetKind::Marks)
        dropEmpty(c.marks);
    else if (w.kind == WidgetKind::Habits)
        dropEmpty(c.habits);
    else if ((w.kind == WidgetKind::Reminders || w.kind == WidgetKind::Calendar) && c.item >= 0 && c.item < int(c.items.size()) &&
             c.items[size_t(c.item)].text.empty())
        c.items.erase(c.items.begin() + c.item); // nothing was typed
    c.item = -1;
    KillTimer(host_, kSaveTimer);
    Save();
    if (w.surface)
        w.surface->Invalidate();
    PostMessageW(host_, WM_WIDGET_RESTACK, 0, 0); // back under the windows
}

void DesktopWidgets::Insert(Widget& w, const std::wstring& s)
{
    WidgetContent& c = w.content;
    std::wstring* buffer = EditBuffer(w);
    if (!buffer)
        return;
    std::wstring& text = *buffer;
    std::wstring add = s;
    if (w.kind != WidgetKind::Notes) // one line
        std::replace_if(add.begin(), add.end(), [](wchar_t ch) { return ch == L'\r' || ch == L'\n'; }, L' ');
    else
        add.erase(std::remove(add.begin(), add.end(), L'\r'), add.end());
    c.caret = std::min(c.caret, text.size());
    text.insert(c.caret, add);
    c.caret += add.size();
    if (w.kind == WidgetKind::Calendar && c.editLesson && c.editScope == 1 && c.editChange >= 0 && c.editChange < int(c.changes.size()))
        c.changes[size_t(c.editChange)].off = false;
}

bool DesktopWidgets::Key(Widget& w, UINT m, WPARAM wp)
{
    WidgetContent& c = w.content;
    std::wstring* buffer = EditBuffer(w);
    if (!buffer)
        return false;
    std::wstring& text = *buffer;
    size_t& at = c.caret;
    at = std::min(at, text.size());
    auto back = [&](size_t i) -> size_t { return i >= 2 && IS_LOW_SURROGATE(text[i - 1]) && IS_HIGH_SURROGATE(text[i - 2]) ? i - 2 : i ? i - 1 : 0; };
    auto ahead = [&](size_t i) -> size_t {
        return i + 1 < text.size() && IS_HIGH_SURROGATE(text[i]) && IS_LOW_SURROGATE(text[i + 1]) ? i + 2 : std::min(i + 1, text.size());
    };
    bool changed = false;
    if (m == WM_KEYDOWN) {
        switch (wp) {
        case VK_ESCAPE:
            EndEdit(w);
            return true;
        case VK_LEFT:
            at = back(at);
            break;
        case VK_RIGHT:
            at = ahead(at);
            break;
        case VK_HOME: {
            size_t nl = at ? text.rfind(L'\n', at - 1) : std::wstring::npos;
            at = nl == std::wstring::npos ? 0 : nl + 1;
            break;
        }
        case VK_END: {
            size_t nl = text.find(L'\n', at);
            at = nl == std::wstring::npos ? text.size() : nl;
            break;
        }
        case VK_DELETE:
            if (at < text.size()) {
                text.erase(at, ahead(at) - at);
                changed = true;
            }
            break;
        default:
            return false; // WM_CHAR follows for the rest
        }
    } else {
        wchar_t ch = wchar_t(wp);
        if (ch == 27) { // Esc
            EndEdit(w);
            return true;
        }
        if (ch == L'\r') {
            if (w.kind == WidgetKind::Notes) {
                Insert(w, L"\n");
                changed = true;
            } else if (w.kind != WidgetKind::Reminders) {
                EndEdit(w); // enter saves the line
                return true;
            } else if (text.empty()) {
                EndEdit(w);
                return true;
            } else { // the next reminder, right below
                int next = c.item + 1;
                c.items.insert(c.items.begin() + next, Reminder{});
                c.editing = false; // the one typed stays as it is
                SaveSoon();
                StartEdit(w, next);
                return true;
            }
        } else if (ch == 8) { // Backspace
            if (at) {
                size_t from = back(at);
                text.erase(from, at - from);
                at = from;
                changed = true;
            }
        } else if (ch == 22) { // Ctrl+V
            if (OpenClipboard(w.surface->Hwnd())) {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
                        Insert(w, p);
                        GlobalUnlock(data);
                        changed = true;
                    }
                CloseClipboard();
            }
        } else if (ch >= 0x20 || ch == L'\t') {
            Insert(w, std::wstring(1, ch));
            changed = true;
        } else {
            return true; // other control keys
        }
    }
    if (changed)
        SaveSoon();
    c.caretOn = true;
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr); // the caret stays lit while typing
    w.surface->Invalidate();
    return true;
}

// The input method's composition and candidates next to the caret.
void DesktopWidgets::PlaceImeWindow(Widget& w)
{
    auto caret = std::find_if(w.hits.begin(), w.hits.end(), [](const WidgetHit& h) { return h.id == L"caret"; });
    if (caret == w.hits.end() || !w.surface)
        return;
    float s = w.surface->Scale();
    POINT p = w.surface->ContentToScreen(POINT{LONG(caret->rect.left * s), LONG(caret->rect.top * s)});
    ScreenToClient(w.surface->Hwnd(), &p);
    HIMC imc = ImmGetContext(w.surface->Hwnd());
    if (!imc)
        return;
    COMPOSITIONFORM cf{CFS_POINT, p, {}};
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand{0, CFS_CANDIDATEPOS, POINT{p.x, p.y + LONG((caret->rect.bottom - caret->rect.top) * s) + 4}, {}};
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(w.surface->Hwnd(), imc);
}

// A world clock's cities: which one to change, then the city in its place (both in menus at `at`).
void DesktopWidgets::PickCity(Widget& w, POINT at)
{
    if (!menu)
        return;
    const std::vector<std::wstring> cities = WidgetCities(w.kind, w.content.cities);
    std::vector<MenuEntry> slots{{L"更改哪个城市", 0, false, false, true}};
    for (size_t i = 0; i < cities.size(); ++i)
        slots.push_back({cities[i], int(i) + 1});
    Widget* target = &w;
    menu->Open(std::move(slots), at, MenuPlace::Below, [this, target, at, cities](int slot) {
        if (slot <= 0 || !Has(target))
            return;
        std::vector<MenuEntry> list{{cities[size_t(slot - 1)] + L" 换成", 0, false, false, true}};
        const auto& all = WorldCities();
        for (size_t i = 0; i < all.size(); ++i)
            list.push_back({all[i].name, int(i) + 1, cities[size_t(slot - 1)] == all[i].name});
        menu->Open(std::move(list), at, MenuPlace::Below, [this, target, slot, cities](int pick) {
            if (pick <= 0 || !Has(target))
                return;
            Widget& w = *target;
            std::vector<std::wstring> chosen = cities;
            chosen[size_t(slot - 1)] = WorldCities()[size_t(pick - 1)].name;
            w.content.cities = std::move(chosen);
            Save();
            w.surface->Invalidate();
            Log(L"widgets: a world clock shows %s", w.content.cities[size_t(slot - 1)].c_str());
        });
    });
}

void DesktopWidgets::Menu(Widget& w)
{
    if (!menu)
        return;
    enum { kSmall = 1, kMedium, kLarge, kRemove, kFolder, kNextPhoto, kClearDone, kAdd, kCities, kEdit, kLayoutInline, kLayoutSplit, kLayoutBoard, kStyle };
    std::vector<MenuEntry> entries;
    int editIndex = -1;
    if (w.kind == WidgetKind::Calendar && w.surface) {
        POINT cursor;
        GetCursorPos(&cursor);
        POINT local = w.surface->ScreenToContent(cursor);
        float s = std::max(w.surface->Scale(), 0.01f);
        D2D1_POINT_2F at = D2D1::Point2F(float(local.x) / s, float(local.y) / s);
        auto hit = [&](const WidgetHit& h) { return at.x >= h.rect.left && at.x < h.rect.right && at.y >= h.rect.top && at.y < h.rect.bottom; };
        for (auto it = w.hits.rbegin(); it != w.hits.rend(); ++it) {
            if (!hit(*it))
                continue;
            const wchar_t* body = nullptr;
            if (it->id.rfind(L"todo:", 0) == 0)
                body = it->id.c_str() + 5;
            else if (it->id.rfind(L"check:", 0) == 0)
                body = it->id.c_str() + 6;
            else if (it->id.rfind(L"sel:", 0) == 0)
                body = it->id.c_str() + 4;
            if (!body) {
                if (it->id == L"popup" || it->id == L"field")
                    break;
                continue;
            }
            int index = _wtoi(body);
            if (index >= 0 && index < int(w.content.items.size()) && !w.content.items[size_t(index)].text.empty())
                editIndex = index;
            break;
        }
        if (editIndex < 0 && w.content.popupDay > 0 && !w.content.popupAll && w.content.popupIndex >= 0 &&
            w.content.popupIndex < int(w.content.items.size()) && !w.content.items[size_t(w.content.popupIndex)].text.empty())
            editIndex = w.content.popupIndex;
        entries.push_back({L"编辑", kEdit, false, editIndex < 0});
        entries.push_back({});
        entries.push_back({L"待办显示在日期上", kLayoutInline, w.content.calBoard == 0, false});
        entries.push_back({L"日历在上、待办在下", kLayoutSplit, w.content.calBoard == 1, false});
        entries.push_back({L"课表", kLayoutBoard, w.content.calBoard == 2, false});
        entries.push_back({});
    }
    if (w.kind == WidgetKind::Calendar) {
        RECT wa = WorkArea();
        float sc = std::max(Scale(), 0.01f);
        float maxW = float(wa.right - wa.left) / sc, maxH = float(wa.bottom - wa.top) / sc;
        auto preset = [&](int id, float sw, float sh, const wchar_t* label) {
            sw = std::clamp(sw, kCalMinW, std::max(kCalMinW, maxW));
            sh = std::clamp(sh, kCalMinH, std::max(kCalMinH, maxH));
            entries.push_back({label, id, std::abs(w.spanW - sw) < 12.f && std::abs(w.spanH - sh) < 12.f, false});
        };
        preset(kSmall, kCalSmallW, kCalSmallH, L"小");
        preset(kMedium, kCalMediumW, kCalMediumH, L"中");
        preset(kLarge, kCalLargeW, kCalLargeH, L"大");
    } else {
        auto size = [&](int id, WidgetSize s, const wchar_t* label) { entries.push_back({label, id, w.size == s, !WidgetSupports(w.kind, s)}); };
        size(kSmall, WidgetSize::Small, L"小");
        size(kMedium, WidgetSize::Medium, L"中");
        size(kLarge, WidgetSize::Large, L"大");
    }
    entries.push_back({});
    if (w.kind == WidgetKind::Photos) {
        entries.push_back({L"选择照片文件夹…", kFolder});
        entries.push_back({L"换一张", kNextPhoto});
        entries.push_back({});
    }
    if (HasWorldClocks(w.kind, w.size)) {
        entries.push_back({L"更改城市…", kCities});
        entries.push_back({});
    }
    if (w.kind == WidgetKind::Reminders) {
        bool anyDone = std::any_of(w.content.items.begin(), w.content.items.end(), [](const Reminder& r) { return r.done; });
        entries.push_back({L"清除已完成", kClearDone, false, !anyDone});
        entries.push_back({});
    }
    entries.push_back({L"调节", kStyle});
    entries.push_back({L"移除组件", kRemove});
    entries.push_back({L"添加组件…", kAdd});
    POINT p;
    GetCursorPos(&p);
    Widget* target = &w;
    menu->Open(std::move(entries), p, MenuPlace::Below, [this, target, p, editIndex](int cmd) {
        if (cmd == kAdd) {
            if (onAddWidgets)
                onAddWidgets();
            return;
        }
        if (!Has(target))
            return; // gone while the menu was open
        Widget& w = *target;
        switch (cmd) {
        case kSmall:
        case kMedium:
        case kLarge:
            Resize(w, cmd == kSmall ? WidgetSize::Small : cmd == kMedium ? WidgetSize::Medium : WidgetSize::Large);
            break;
        case kFolder:
            PickFolder(w);
            break;
        case kCities:
            PickCity(w, p);
            break;
        case kNextPhoto:
            LoadPhoto(w, true);
            break;
        case kClearDone:
            std::erase_if(w.content.items, [](const Reminder& r) { return r.done; });
            Save();
            w.surface->Invalidate();
            break;
        case kRemove:
            Remove(w); // w is gone
            break;
        case kLayoutInline:
        case kLayoutSplit:
        case kLayoutBoard:
            if (w.kind == WidgetKind::Calendar) {
                w.content.calBoard = cmd == kLayoutBoard ? 2 : cmd == kLayoutSplit ? 1 : 0;
                w.content.calSplit = w.content.calBoard == 1;
                w.content.popupDay = 0;
                w.content.popupIndex = -1;
                w.content.popupAll = false;
                Save();
                w.surface->Invalidate();
                SyncYunzhao(w);
            }
            break;
        case kStyle:
            OpenStyle(w);
            break;
        case kEdit:
            if (w.kind == WidgetKind::Calendar && editIndex >= 0 && editIndex < int(w.content.items.size())) {
                POINT local = w.surface->ScreenToContent(p);
                float s = std::max(w.surface->Scale(), 0.01f);
                w.content.popupDay = 0;
                w.content.popupIndex = -1;
                w.content.popupAll = false;
                w.content.editX = float(local.x) / s;
                w.content.editY = float(local.y) / s;
                StartEdit(w, editIndex);
            }
            break;
        }
    });
}

// ---- painting ------------------------------------------------------------------

// Without glass there is no glass shadow: a soft one of its own in the margin, a little lower
// than the face, as under macOS's widgets. Only around the face (it is opaque, and already drawn).
void DesktopWidgets::PaintShadow(Canvas& c, D2D1_RECT_F face)
{
    ID2D1DeviceContext* d2d = c.D2D();
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    winrt::com_ptr<ID2D1RectangleGeometry> outer;
    winrt::com_ptr<ID2D1RoundedRectangleGeometry> inner;
    winrt::com_ptr<ID2D1PathGeometry> ring;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    const D2D1_RECT_F px = c.Px(face);
    const float r = kWidgetRadius * c.scale, room = 14 * c.scale;
    if (FAILED(factory->CreateRectangleGeometry(D2D1::RectF(px.left - room, px.top - room, px.right + room, px.bottom + room), outer.put())) ||
        FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(px, r, r), inner.put())) || FAILED(factory->CreatePathGeometry(ring.put())) ||
        FAILED(ring->Open(sink.put())) || FAILED(outer->CombineWithGeometry(inner.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get())) ||
        FAILED(sink->Close()))
        return;
    d2d->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), ring.get()), nullptr);
    for (int i = 9; i >= 1; --i) {
        const float d = float(i);
        c.Fill(D2D1::RectF(face.left - d, face.top - d + 2, face.right + d, face.bottom + d + 2), kWidgetRadius + d, D2D1::ColorF(0, 0, 0, 0.018f));
    }
    d2d->PopLayer();
}

void DesktopWidgets::Paint(Widget& w, Canvas& c)
{
    // The hosted page is the only calendar and the only timetable. One monitor
    // or two, one desktop or two, this widget must not draw a second copy.
    if (w.kind == WidgetKind::Calendar) {
        w.hits.clear();
        return;
    }
    RECT rect = w.surface->Rect();
    const float width = float(rect.right - rect.left) / c.scale, height = float(rect.bottom - rect.top) / c.scale;
    const auto bounds = D2D1::RectF(0, 0, width, height);
    // Preserve each size's layout. Only its viewport grows or shrinks, so a
    // large face is revealed without squeezing its columns into a small one.
    c.Clip(bounds);
    if (c.panel && material_ != WidgetMaterial::Color)
        c.Fill(bounds, 0, c.theme.panel);
    if (!w.content.bgImage.empty() && w.surface && !w.resizing && !w.dragging) {
        RECT px = w.surface->Rect();
        int pw = std::max(1, int(px.right - px.left));
        int ph = std::max(1, int(px.bottom - px.top));
        if (!w.content.backdrop || w.content.backdrop->width != pw || w.content.backdrop->height != ph) {
            IconImage img = ReadPhoto(w.content.bgImage, UINT(pw), UINT(ph));
            if (!img.empty()) {
                w.content.backdrop = std::make_shared<const IconImage>(std::move(img));
                w.content.backdropKey = L"wbg:" + std::to_wstring(pw) + L"x" + std::to_wstring(ph) + L":" + w.content.bgImage;
            }
        }
    }
    if (w.content.bgColor)
        c.Fill(bounds, kWidgetRadius, D2D1::ColorF(w.content.bgColor & 0xFFFFFFu, 0.94f));
    if (w.content.backdrop && !w.content.bgImage.empty())
        c.ImageRounded(w.content.backdropKey, *w.content.backdrop, bounds, kWidgetRadius);
    GetLocalTime(&world_.now);
    if (w.kind == WidgetKind::Reminders)
        w.content.scroll = std::clamp(w.content.scroll, 0.f, ReminderScrollMax(w.content, w.size));
    // Other widgets keep their size's layout while the window glides. The calendar
    // lays its month out in the window it actually has.
    if (w.kind == WidgetKind::Calendar)
        w.content.calT = CalendarFraction(w);
    const float layoutW = w.kind == WidgetKind::Calendar ? width : 0;
    const float layoutH = w.kind == WidgetKind::Calendar ? height : 0;
    PaintWidget(c, w.kind, w.size, material_, w.content, world_, w.hits, w.hoverDay, layoutW, layoutH);
    if ((w.hover || w.resizing) && !w.dragging && Resizable(w.kind) && !(w.kind == WidgetKind::Calendar && w.content.calLock)) {
        if (w.kind == WidgetKind::Calendar) {
            c.Stroke(D2D1::RectF(2, 2, width - 2, height - 2), kWidgetRadius, c.theme.subtext, 1.5f);
        } else {
            const float r = kWidgetRadius, h = height;
            c.Arc(r, h - r, r - 6, 100.f, 170.f, c.theme.subtext, 3.5f);
            c.Arc(width - r, h - r, r - 6, 10.f, 80.f, c.theme.subtext, 3.5f);
        }
    }
    c.Unclip();
    c.MaskRounded(bounds, kWidgetRadius);
    if (material_ == WidgetMaterial::Color)
        PaintShadow(c, bounds);
    if (w.content.editing)
        PlaceImeWindow(w);
}

namespace {

struct TodoLayout {
    float width = 0, height = 0;
    D2D1_RECT_F card{}, field{}, dateField{}, dueField{}, save{}, cancel{}, close{};
    D2D1_RECT_F chip[4]{};
};

TodoLayout LayoutTodoForm(float width, float height)
{
    const float cardW = std::min(460.f, std::max(320.f, width - 48.f));
    const float cardH = std::min(372.f, std::max(300.f, height - 48.f));
    const float x = (width - cardW) / 2.f;
    const float y = (height - cardH) / 2.f;
    TodoLayout lay;
    lay.width = width;
    lay.height = height;
    lay.card = D2D1::RectF(x, y, x + cardW, y + cardH);
    lay.close = D2D1::RectF(x + 10, y + 10, x + 40, y + 40);
    lay.field = D2D1::RectF(x + 20, y + 56, x + cardW - 20, y + 100);
    const float half = (cardW - 52.f) / 2.f;
    lay.dateField = D2D1::RectF(x + 20, y + 112, x + 20 + half, y + 148);
    lay.dueField = D2D1::RectF(lay.dateField.right + 12, y + 112, x + cardW - 20, y + 148);
    const float colW = half;
    const float gridTop = y + 176.f;
    for (int i = 0; i < 4; ++i) {
        float lx = x + 20.f + float(i % 2) * (colW + 12.f);
        float ly = gridTop + float(i / 2) * 40.f;
        lay.chip[i] = D2D1::RectF(lx, ly, lx + colW, ly + 34.f);
    }
    lay.cancel = D2D1::RectF(x + 20, y + cardH - 58, x + 20 + (cardW - 52) / 2, y + cardH - 20);
    lay.save = D2D1::RectF(lay.cancel.right + 12, lay.cancel.top, x + cardW - 20, lay.cancel.bottom);
    return lay;
}

bool InRect(D2D1_RECT_F r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring DateLabel(int y, int m, int d)
{
    return std::to_wstring(y) + L"年" + std::to_wstring(m) + L"月" + std::to_wstring(d) + L"日";
}

RECT WidgetMonitor(RECT widget)
{
    HMONITOR monitor = MonitorFromRect(&widget, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return widget;
    return info.rcMonitor;
}

const uint32_t kSwatches[] = {0, 0x0A84FF, 0xFF3B30, 0xFF9F0A, 0x30D158, 0xBF5AF2, 0x5E5CE6, 0xFFD60A, 0xFFFFFF, 0x1C1C1E};
constexpr int kSwatchCount = 10;

struct HandoutLayout {
    D2D1_RECT_F card{}, field{}, save{}, cancel{};
};

HandoutLayout LayoutHandout(float width, float height)
{
    const float cardW = std::min(560.f, std::max(320.f, width - 48.f));
    const float cardH = std::min(480.f, std::max(300.f, height - 48.f));
    const float x = (width - cardW) / 2.f;
    const float y = (height - cardH) / 2.f;
    HandoutLayout lay;
    lay.card = D2D1::RectF(x, y, x + cardW, y + cardH);
    lay.field = D2D1::RectF(x + 20, y + 108, x + cardW - 20, y + cardH - 72);
    lay.cancel = D2D1::RectF(x + 20, y + cardH - 52, x + 20 + (cardW - 52.f) / 2.f, y + cardH - 18);
    lay.save = D2D1::RectF(lay.cancel.right + 12, lay.cancel.top, x + cardW - 20, lay.cancel.bottom);
    return lay;
}

struct StyleLayout {
    D2D1_RECT_F card{}, done{}, image{}, clear{};
    D2D1_RECT_F datePrev{}, dateNext{}, tablePrev{}, tableNext{};
    D2D1_RECT_F dot[3][kSwatchCount]{};
    bool rowOn[3]{};
    bool calendar = false;
    float dateRow = 0, tableRow = 0;
};

StyleLayout LayoutStyle(float width, float height, bool calendar)
{
    const float cardW = std::min(520.f, std::max(420.f, width - 48.f));
    const float cardH = std::min(calendar ? 520.f : 280.f, std::max(240.f, height - 48.f));
    const float x = (width - cardW) / 2.f;
    const float y = (height - cardH) / 2.f;
    StyleLayout lay;
    lay.calendar = calendar;
    lay.card = D2D1::RectF(x, y, x + cardW, y + cardH);
    float row = y + 64.f;
    float step = std::min(36.f, (cardW - 150.f) / float(kSwatchCount));
    auto dots = [&](int which) {
        lay.rowOn[which] = true;
        for (int i = 0; i < kSwatchCount; ++i) {
            float dx = x + 132.f + float(i) * step;
            lay.dot[which][i] = D2D1::RectF(dx, row, dx + 26.f, row + 26.f);
        }
        row += 48.f;
    };
    if (calendar)
        dots(0);
    if (calendar)
        dots(1);
    if (calendar) {
        lay.dateRow = row;
        lay.datePrev = D2D1::RectF(x + 132, row, x + 160, row + 32);
        lay.dateNext = D2D1::RectF(x + 248, row, x + 276, row + 32);
        row += 44.f;
        lay.tableRow = row;
        lay.tablePrev = D2D1::RectF(x + 132, row, x + 160, row + 32);
        lay.tableNext = D2D1::RectF(x + 248, row, x + 276, row + 32);
        row += 48.f;
    }
    dots(2);
    lay.image = D2D1::RectF(x + 132, row, x + 228, row + 32);
    lay.clear = D2D1::RectF(x + 236, row, x + 300, row + 32);
    lay.done = D2D1::RectF(x + cardW - 112, y + cardH - 52, x + cardW - 20, y + cardH - 16);
    return lay;
}

std::wstring FileTail(const std::wstring& path)
{
    size_t n = path.find_last_of(L"\\/");
    return n == std::wstring::npos ? path : path.substr(n + 1);
}

void StepMonth(int& y, int& m, int delta)
{
    int month = m + delta;
    int year = y;
    while (month < 1) {
        month += 12;
        --year;
    }
    while (month > 12) {
        month -= 12;
        ++year;
    }
    y = year;
    m = month;
}

bool BeforeDay(int y, int m, int d, int y2, int m2, int d2)
{
    return y < y2 || (y == y2 && (m < m2 || (m == m2 && d < d2)));
}

struct DatePop {
    D2D1_RECT_F card{}, prev{}, next{}, today{};
    D2D1_RECT_F day[42]{};
    int y[42]{}, m[42]{}, d[42]{};
};

DatePop LayoutDatePop(D2D1_RECT_F anchor, float width, float height, int viewY, int viewM)
{
    const float pw = 292.f, ph = 304.f;
    float x = anchor.left;
    float y = anchor.bottom + 8.f;
    if (x + pw > width - 16.f)
        x = width - 16.f - pw;
    if (x < 16.f)
        x = 16.f;
    if (y + ph > height - 16.f)
        y = std::max(16.f, anchor.top - 8.f - ph);
    DatePop pop;
    pop.card = D2D1::RectF(x, y, x + pw, y + ph);
    pop.prev = D2D1::RectF(x + pw - 76, y + 8, x + pw - 46, y + 36);
    pop.next = D2D1::RectF(x + pw - 42, y + 8, x + pw - 12, y + 36);
    pop.today = D2D1::RectF(x + pw / 2.f - 40, y + ph - 36, x + pw / 2.f + 40, y + ph - 8);
    int sy = viewY, sm = viewM, sd = 1;
    StepDays(sy, sm, sd, -((Weekday(viewY, viewM, 1) + 1) % 7));
    const float cell = (pw - 24.f) / 7.f;
    for (int i = 0; i < 42; ++i) {
        int cy = sy, cm = sm, cd = sd;
        StepDays(cy, cm, cd, i);
        pop.y[i] = cy;
        pop.m[i] = cm;
        pop.d[i] = cd;
        float cx = x + 12.f + float(i % 7) * cell;
        float cy0 = y + 74.f + float(i / 7) * 30.f;
        pop.day[i] = D2D1::RectF(cx, cy0, cx + cell, cy0 + 28.f);
    }
    return pop;
}

} // namespace

void DesktopWidgets::OpenTodo(Widget& w)
{
    if (!w.surface || w.content.calLock)
        return;
    CloseHandout(false);
    CloseStyle(false);
    CloseTool();
    EndEdit(w);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int y = 0, m = 0, d = 0;
    CalendarFocus(w.content, now, y, m, d);
    todo_.open = true;
    todo_.owner = &w;
    todo_.text.clear();
    todo_.caret = 0;
    todo_.caretOn = true;
    todo_.pri = 0;
    todo_.y = y;
    todo_.m = m;
    todo_.d = d;
    todo_.dy = y;
    todo_.dm = m;
    todo_.dd = d;
    todo_.picking = 0;
    todo_.viewY = y;
    todo_.viewM = m;
    if (!todoSurface_) {
        todoSurface_ = std::make_unique<Surface>(instance_, L"新建待办", true, 0.f, false);
        todoSurface_->SetGlassless(true);
        todoSurface_->SetRadius(0);
        todoSurface_->paint = [this](Canvas& c) { PaintTodo(c); };
        todoSurface_->message = [this](UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) { return TodoMessage(msg, wp, lp, result); };
        todoSurface_->onDismiss = [] {};
    }
    RECT widget = w.surface->Rect();
    HMONITOR monitor = MonitorFromRect(&widget, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        info.rcMonitor = widget;
    POINT anchor{(info.rcMonitor.left + info.rcMonitor.right) / 2, (info.rcMonitor.top + info.rcMonitor.bottom) / 2};
    todoSurface_->Show(info.rcMonitor, true, anchor, false);
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr);
    SetForegroundWindow(todoSurface_->Hwnd());
    todoSurface_->Invalidate();
}

void DesktopWidgets::CloseTodo(bool save)
{
    if (!todo_.open && !(todoSurface_ && todoSurface_->Visible()))
        return;
    Widget* owner = todo_.owner;
    std::wstring text = todo_.text;
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\n' || text.back() == L'\r'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == L' ')
        ++start;
    text = text.substr(start);
    int pri = todo_.pri;
    int y = todo_.y, m = todo_.m, d = todo_.d;
    int dy = todo_.dy, dm = todo_.dm, dd = todo_.dd;
    todo_.open = false;
    todo_.owner = nullptr;
    todo_.text.clear();
    if (todoSurface_)
        todoSurface_->Hide(false);
    if (save && owner && Has(owner) && !text.empty()) {
        Reminder item;
        item.text = std::move(text);
        item.year = y;
        item.month = m;
        item.day = d;
        item.pri = std::clamp(pri, 0, 3);
        if (dy != y || dm != m || dd != d) {
            item.dueYear = dy;
            item.dueMonth = dm;
            item.dueDay = dd;
        }
        owner->content.items.push_back(std::move(item));
        owner->content.shownYear = y;
        owner->content.shownMonth = m;
        owner->content.focusDay = d;
        Save();
        if (owner->surface)
            owner->surface->Invalidate();
    }
}

void DesktopWidgets::PaintTodo(Canvas& c)
{
    if (!todoSurface_)
        return;
    RECT rect = todoSurface_->Rect();
    float scale = std::max(todoSurface_->Scale(), 0.01f);
    float width = float(rect.right - rect.left) / scale;
    float height = float(rect.bottom - rect.top) / scale;
    TodoLayout lay = LayoutTodoForm(width, height);
    c.halo = false;
    c.Fill(D2D1::RectF(0, 0, width, height), 0, D2D1::ColorF(0, 0, 0, 0.45f));
    c.Fill(lay.card, 18, D2D1::ColorF(0x1C1C1E, 0.98f));
    c.Stroke(lay.card, 18, D2D1::ColorF(0x0A84FF, 0.85f), 1.4f);
    const D2D1_COLOR_F ink = D2D1::ColorF(1, 1, 1, 0.96f);
    const D2D1_COLOR_F dim = D2D1::ColorF(1, 1, 1, 0.62f);
    const D2D1_COLOR_F blue = D2D1::ColorF(0x0A84FF, 1);
    c.Glyph(L"\uE711", lay.close, 14, dim);
    c.Text(L"新建待办", D2D1::RectF(lay.close.right + 8, lay.card.top + 12, lay.card.right - 20, lay.card.top + 42), 18, ink, DWRITE_FONT_WEIGHT_BOLD);
    if (todo_.text.empty())
        c.Text(L"输入待办内容", D2D1::RectF(lay.field.left + 10, lay.field.top, lay.field.right - 10, lay.field.bottom), 15, D2D1::ColorF(1, 1, 1, 0.35f));
    c.Fill(lay.field, 10, D2D1::ColorF(1, 1, 1, 0.06f));
    c.Stroke(lay.field, 10, D2D1::ColorF(1, 1, 1, 0.16f), 1);
    c.Text(todo_.text, D2D1::RectF(lay.field.left + 10, lay.field.top, lay.field.right - 10, lay.field.bottom), 15, ink);
    if (todo_.caretOn) {
        float caretX = lay.field.left + 10 + c.TextWidth(todo_.text.substr(0, std::min(todo_.caret, todo_.text.size())), 15);
        c.Fill(D2D1::RectF(caretX, lay.field.top + 10, caretX + 1.5f, lay.field.bottom - 10), 0, blue);
    }
    auto dateBtn = [&](D2D1_RECT_F r, const wchar_t* label, int y, int mo, int d, bool open) {
        c.Fill(r, 10, D2D1::ColorF(1, 1, 1, open ? 0.14f : 0.06f));
        c.Stroke(r, 10, open ? blue : D2D1::ColorF(1, 1, 1, 0.16f), open ? 1.4f : 1.f);
        c.Text(label, D2D1::RectF(r.left + 8, r.top, r.left + 42, r.bottom), 12, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        (void)y;
        std::wstring value = std::to_wstring(mo) + L"月" + std::to_wstring(d) + L"日";
        c.Text(value, D2D1::RectF(r.left + 42, r.top, r.right - 20, r.bottom), 13, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Glyph(open ? L"\uE70E" : L"\uE70D", D2D1::RectF(r.right - 20, r.top, r.right - 2, r.bottom), 10, dim);
    };
    dateBtn(lay.dateField, L"日期", todo_.y, todo_.m, todo_.d, todo_.picking == 1);
    dateBtn(lay.dueField, L"截止", todo_.dy, todo_.dm, todo_.dd, todo_.picking == 2);
    c.Text(L"紧急程度", D2D1::RectF(lay.card.left + 20, lay.chip[0].top - 22, lay.card.right - 20, lay.chip[0].top), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const wchar_t* names[] = {L"不紧急不重要", L"紧急不重要", L"重要不紧急", L"重要且紧急"};
    const uint32_t colors[] = {0xFFFFFFu, 0x0A84FFu, 0xFFD60Au, 0xFF3B30u};
    // chips are laid out as 重要且紧急, 重要不紧急, 紧急不重要, 不紧急不重要 to match the old editor
    const int order[] = {3, 2, 1, 0};
    for (int i = 0; i < 4; ++i) {
        int pri = order[i];
        bool on = todo_.pri == pri;
        D2D1_COLOR_F color = D2D1::ColorF(colors[pri], 1);
        if (on)
            c.Fill(lay.chip[i], 8, D2D1::ColorF(color.r, color.g, color.b, 0.24f));
        c.Stroke(lay.chip[i], 8, on ? color : D2D1::ColorF(1, 1, 1, 0.16f), on ? 1.5f : 1);
        c.Circle(lay.chip[i].left + 14, (lay.chip[i].top + lay.chip[i].bottom) / 2, 5, color);
        c.Text(names[pri], D2D1::RectF(lay.chip[i].left + 26, lay.chip[i].top, lay.chip[i].right - 6, lay.chip[i].bottom), 12.5f,
               D2D1::ColorF(1, 1, 1, on ? 0.96f : 0.72f));
    }
    c.Fill(lay.save, 10, blue);
    c.Text(L"保存", lay.save, 14, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    if (todo_.picking == 1 || todo_.picking == 2) {
        D2D1_RECT_F anchor = todo_.picking == 1 ? lay.dateField : lay.dueField;
        DatePop pop = LayoutDatePop(anchor, width, height, todo_.viewY, todo_.viewM);
        int selY = todo_.picking == 1 ? todo_.y : todo_.dy;
        int selM = todo_.picking == 1 ? todo_.m : todo_.dm;
        int selD = todo_.picking == 1 ? todo_.d : todo_.dd;
        SYSTEMTIME today{};
        GetLocalTime(&today);
        c.Fill(pop.card, 14, D2D1::ColorF(0x2C2C2E, 0.98f));
        c.Stroke(pop.card, 14, D2D1::ColorF(1, 1, 1, 0.12f), 1);
        c.Text(std::to_wstring(todo_.viewY) + L"年" + std::to_wstring(todo_.viewM) + L"月",
               D2D1::RectF(pop.card.left + 14, pop.card.top + 8, pop.prev.left - 4, pop.card.top + 36), 15, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Glyph(L"\uE76B", pop.prev, 13, ink);
        c.Glyph(L"\uE76C", pop.next, 13, ink);
        const wchar_t* week[] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
        for (int i = 0; i < 7; ++i)
            c.Text(week[i], D2D1::RectF(pop.day[i].left, pop.card.top + 44, pop.day[i].right, pop.card.top + 68), 12, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        for (int i = 0; i < 42; ++i) {
            bool inMonth = pop.m[i] == todo_.viewM && pop.y[i] == todo_.viewY;
            bool isToday = pop.y[i] == int(today.wYear) && pop.m[i] == int(today.wMonth) && pop.d[i] == int(today.wDay);
            bool selected = pop.y[i] == selY && pop.m[i] == selM && pop.d[i] == selD;
            float cx = (pop.day[i].left + pop.day[i].right) / 2.f;
            float cy = (pop.day[i].top + pop.day[i].bottom) / 2.f;
            if (isToday)
                c.Circle(cx, cy, 12.f, blue);
            else if (selected)
                c.Circle(cx, cy, 12.f, D2D1::ColorF(0x0A84FF, 0.28f));
            D2D1_COLOR_F num = isToday ? D2D1::ColorF(1, 1, 1, 1) : inMonth ? ink : D2D1::ColorF(1, 1, 1, 0.32f);
            c.Text(std::to_wstring(pop.d[i]), pop.day[i], 13, num, isToday || selected ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, 1);
        }
        c.Text(L"今天", pop.today, 14, blue, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

bool DesktopWidgets::TodoMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT&)
{
    if (!todo_.open || !todoSurface_)
        return false;
    auto layout = [&] {
        RECT rect = todoSurface_->Rect();
        float scale = std::max(todoSurface_->Scale(), 0.01f);
        return LayoutTodoForm(float(rect.right - rect.left) / scale, float(rect.bottom - rect.top) / scale);
    };
    auto point = [&] {
        float scale = std::max(todoSurface_->Scale(), 0.01f);
        return D2D1::Point2F(float(GET_X_LPARAM(lp)) / scale, float(GET_Y_LPARAM(lp)) / scale);
    };
    if (m == WM_LBUTTONUP) {
        TodoLayout lay = layout();
        D2D1_POINT_2F p = point();
        auto keepOrder = [&] {
            if (BeforeDay(todo_.dy, todo_.dm, todo_.dd, todo_.y, todo_.m, todo_.d)) {
                todo_.dy = todo_.y;
                todo_.dm = todo_.m;
                todo_.dd = todo_.d;
            }
        };
        auto choose = [&](int y, int mo, int d) {
            if (todo_.picking == 1) {
                todo_.y = y;
                todo_.m = mo;
                todo_.d = d;
            } else {
                todo_.dy = y;
                todo_.dm = mo;
                todo_.dd = d;
            }
            keepOrder();
            todo_.picking = 0;
        };
        if (todo_.picking) {
            DatePop pop = LayoutDatePop(todo_.picking == 1 ? lay.dateField : lay.dueField, lay.width, lay.height, todo_.viewY, todo_.viewM);
            if (InRect(pop.card, p.x, p.y)) {
                if (InRect(pop.prev, p.x, p.y))
                    StepMonth(todo_.viewY, todo_.viewM, -1);
                else if (InRect(pop.next, p.x, p.y))
                    StepMonth(todo_.viewY, todo_.viewM, 1);
                else if (InRect(pop.today, p.x, p.y)) {
                    SYSTEMTIME now{};
                    GetLocalTime(&now);
                    choose(int(now.wYear), int(now.wMonth), int(now.wDay));
                } else {
                    for (int i = 0; i < 42; ++i)
                        if (InRect(pop.day[i], p.x, p.y))
                            choose(pop.y[i], pop.m[i], pop.d[i]);
                }
                todoSurface_->Invalidate();
                return true;
            }
            int was = todo_.picking;
            todo_.picking = 0;
            bool sameField = (was == 1 && InRect(lay.dateField, p.x, p.y)) || (was == 2 && InRect(lay.dueField, p.x, p.y));
            if (sameField) {
                todoSurface_->Invalidate();
                return true;
            }
        }
        if (InRect(lay.close, p.x, p.y)) {
            CloseTodo(false);
            return true;
        }
        if (!InRect(lay.card, p.x, p.y))
            return true;
        if (InRect(lay.dateField, p.x, p.y)) {
            todo_.picking = 1;
            todo_.viewY = todo_.y;
            todo_.viewM = todo_.m;
        } else if (InRect(lay.dueField, p.x, p.y)) {
            todo_.picking = 2;
            todo_.viewY = todo_.dy;
            todo_.viewM = todo_.dm;
        } else if (InRect(lay.save, p.x, p.y)) {
            CloseTodo(true);
            return true;
        } else {
            const int order[] = {3, 2, 1, 0};
            for (int i = 0; i < 4; ++i)
                if (InRect(lay.chip[i], p.x, p.y))
                    todo_.pri = order[i];
            if (InRect(lay.field, p.x, p.y))
                todo_.caret = todo_.text.size();
        }
        todoSurface_->Invalidate();
        return true;
    }
    if (m == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (todo_.picking) {
                todo_.picking = 0;
                todoSurface_->Invalidate();
            }
            return true;
        }
        if (wp == VK_RETURN) {
            CloseTodo(true);
            return true;
        }
        if (wp == VK_BACK && todo_.caret > 0) {
            size_t from = todo_.caret >= 2 && IS_LOW_SURROGATE(todo_.text[todo_.caret - 1]) && IS_HIGH_SURROGATE(todo_.text[todo_.caret - 2])
                              ? todo_.caret - 2
                              : todo_.caret - 1;
            todo_.text.erase(from, todo_.caret - from);
            todo_.caret = from;
            todoSurface_->Invalidate();
            return true;
        }
        if (wp == VK_DELETE && todo_.caret < todo_.text.size()) {
            size_t to = todo_.caret + 1 < todo_.text.size() && IS_HIGH_SURROGATE(todo_.text[todo_.caret]) && IS_LOW_SURROGATE(todo_.text[todo_.caret + 1])
                            ? todo_.caret + 2
                            : todo_.caret + 1;
            todo_.text.erase(todo_.caret, to - todo_.caret);
            todoSurface_->Invalidate();
            return true;
        }
        if (wp == VK_LEFT)
            todo_.caret = todo_.caret >= 2 && IS_LOW_SURROGATE(todo_.text[todo_.caret - 1]) && IS_HIGH_SURROGATE(todo_.text[todo_.caret - 2])
                              ? todo_.caret - 2
                              : todo_.caret ? todo_.caret - 1 : 0;
        else if (wp == VK_RIGHT)
            todo_.caret = todo_.caret + 1 < todo_.text.size() && IS_HIGH_SURROGATE(todo_.text[todo_.caret]) && IS_LOW_SURROGATE(todo_.text[todo_.caret + 1])
                              ? todo_.caret + 2
                              : std::min(todo_.caret + 1, todo_.text.size());
        else if (wp == VK_HOME)
            todo_.caret = 0;
        else if (wp == VK_END)
            todo_.caret = todo_.text.size();
        else
            return false;
        todoSurface_->Invalidate();
        return true;
    }
    if (m == WM_CHAR) {
        wchar_t ch = wchar_t(wp);
        if (ch < 32 || ch == 127)
            return true;
        todo_.text.insert(todo_.caret, 1, ch);
        todo_.caret += 1;
        todoSurface_->Invalidate();
        return true;
    }
    return false;
}

void DesktopWidgets::OpenHandout(Widget& w, int weekday, int period)
{
    if (!w.surface || w.content.calLock || w.content.tableEdit || weekday < 0 || weekday > 6 || period < 0 || period > 7)
        return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int y = 0, m = 0, d = 0;
    CalendarFocus(w.content, now, y, m, d);
    StepDays(y, m, d, -Weekday(y, m, d));
    int change = FindLessonChange(w.content, y, m, d, weekday, period);
    int lesson = -1;
    for (int i = 0; i < int(w.content.lessons.size()); ++i)
        if (w.content.lessons[size_t(i)].weekday == weekday && w.content.lessons[size_t(i)].period == period)
            lesson = i;
    std::wstring course, teacher, notes;
    int notesChange = -1;
    int notesLesson = -1;
    if (change >= 0 && w.content.changes[size_t(change)].off)
        return;
    if (change >= 0 && !w.content.changes[size_t(change)].name.empty()) {
        course = w.content.changes[size_t(change)].name;
        teacher = w.content.changes[size_t(change)].teacher;
        notes = w.content.changes[size_t(change)].notes;
        notesChange = change;
    } else if (lesson >= 0 && !w.content.lessons[size_t(lesson)].name.empty()) {
        course = w.content.lessons[size_t(lesson)].name;
        teacher = w.content.lessons[size_t(lesson)].teacher;
        notes = w.content.lessons[size_t(lesson)].notes;
        notesLesson = lesson;
    }
    if (course.empty())
        return;
    CloseTodo(false);
    CloseStyle(false);
    handout_.open = true;
    handout_.owner = &w;
    handout_.lesson = notesLesson;
    handout_.change = notesChange;
    handout_.course = std::move(course);
    handout_.teacher = std::move(teacher);
    handout_.text = std::move(notes);
    handout_.caret = handout_.text.size();
    handout_.caretOn = true;
    handout_.when.clear();
    if (w.content.periodStart[period] >= 0) {
        auto hhmm = [](int mins) {
            wchar_t b[8];
            swprintf_s(b, L"%02d:%02d", std::clamp(mins, 0, 24 * 60 - 1) / 60, std::clamp(mins, 0, 24 * 60 - 1) % 60);
            return std::wstring(b);
        };
        handout_.when = hhmm(w.content.periodStart[period]) + L" – " + hhmm(w.content.periodEnd[period]);
    }
    if (!handoutSurface_) {
        handoutSurface_ = std::make_unique<Surface>(instance_, L"讲义", true, 0.f, false);
        handoutSurface_->SetGlassless(true);
        handoutSurface_->SetRadius(0);
        handoutSurface_->paint = [this](Canvas& c) { PaintHandout(c); };
        handoutSurface_->message = [this](UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) { return HandoutMessage(msg, wp, lp, result); };
        handoutSurface_->onDismiss = [this] { CloseHandout(false); };
    }
    RECT mon = WidgetMonitor(w.surface->Rect());
    POINT anchor{(mon.left + mon.right) / 2, (mon.top + mon.bottom) / 2};
    handoutSurface_->Show(mon, true, anchor, false);
    SetTimer(host_, kCaretTimer, kCaretMs, nullptr);
    SetForegroundWindow(handoutSurface_->Hwnd());
    handoutSurface_->Invalidate();
}

void DesktopWidgets::CloseHandout(bool save)
{
    if (!handout_.open && !(handoutSurface_ && handoutSurface_->Visible()))
        return;
    Widget* owner = handout_.owner;
    std::wstring text = handout_.text;
    int lesson = handout_.lesson;
    int change = handout_.change;
    handout_.open = false;
    handout_.owner = nullptr;
    handout_.text.clear();
    if (handoutSurface_)
        handoutSurface_->Hide(false);
    if (!save || !owner || !Has(owner))
        return;
    if (change >= 0 && change < int(owner->content.changes.size()))
        owner->content.changes[size_t(change)].notes = text;
    else if (lesson >= 0 && lesson < int(owner->content.lessons.size()))
        owner->content.lessons[size_t(lesson)].notes = text;
    Save();
    if (owner->surface)
        owner->surface->Invalidate();
}

void DesktopWidgets::PaintHandout(Canvas& c)
{
    if (!handoutSurface_ || !handout_.open)
        return;
    RECT rect = handoutSurface_->Rect();
    float scale = std::max(handoutSurface_->Scale(), 0.01f);
    float width = float(rect.right - rect.left) / scale;
    float height = float(rect.bottom - rect.top) / scale;
    HandoutLayout lay = LayoutHandout(width, height);
    c.halo = false;
    c.Fill(D2D1::RectF(0, 0, width, height), 0, D2D1::ColorF(0, 0, 0, 0.45f));
    c.Fill(lay.card, 18, D2D1::ColorF(0x1C1C1E, 0.98f));
    c.Stroke(lay.card, 18, D2D1::ColorF(0x0A84FF, 0.85f), 1.4f);
    const D2D1_COLOR_F ink = D2D1::ColorF(1, 1, 1, 0.96f);
    const D2D1_COLOR_F dim = D2D1::ColorF(1, 1, 1, 0.62f);
    c.Text(handout_.course, D2D1::RectF(lay.card.left + 20, lay.card.top + 16, lay.card.right - 20, lay.card.top + 46), 20, ink, DWRITE_FONT_WEIGHT_BOLD);
    std::wstring sub = handout_.teacher;
    if (!handout_.when.empty())
        sub += (sub.empty() ? L"" : L"    ") + handout_.when;
    c.Text(sub, D2D1::RectF(lay.card.left + 20, lay.card.top + 46, lay.card.right - 20, lay.card.top + 70), 13, dim);
    c.Text(L"讲义", D2D1::RectF(lay.card.left + 20, lay.card.top + 76, lay.card.right - 20, lay.field.top), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Fill(lay.field, 10, D2D1::ColorF(1, 1, 1, 0.06f));
    D2D1_RECT_F text = D2D1::RectF(lay.field.left + 12, lay.field.top + 8, lay.field.right - 12, lay.field.bottom - 8);
    if (handout_.text.empty())
        c.Text(L"暂无讲义，可在这里填写", text, 15, D2D1::ColorF(1, 1, 1, 0.35f), DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    else
        c.Text(handout_.text, text, 15, ink, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    if (handout_.caretOn) {
        D2D1_RECT_F caret = c.Caret(handout_.text, text, 15, DWRITE_FONT_WEIGHT_NORMAL, handout_.caret);
        c.Fill(caret, 0, D2D1::ColorF(0x0A84FF, 1));
    }
    c.Fill(lay.cancel, 10, D2D1::ColorF(1, 1, 1, 0.08f));
    c.Text(L"取消", lay.cancel, 14, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Fill(lay.save, 10, D2D1::ColorF(0x0A84FF, 1));
    c.Text(L"保存", lay.save, 14, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
}

bool DesktopWidgets::HandoutMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT&)
{
    if (!handout_.open || !handoutSurface_)
        return false;
    auto layout = [&] {
        RECT rect = handoutSurface_->Rect();
        float scale = std::max(handoutSurface_->Scale(), 0.01f);
        return LayoutHandout(float(rect.right - rect.left) / scale, float(rect.bottom - rect.top) / scale);
    };
    if (m == WM_LBUTTONUP) {
        float scale = std::max(handoutSurface_->Scale(), 0.01f);
        float x = float(GET_X_LPARAM(lp)) / scale;
        float y = float(GET_Y_LPARAM(lp)) / scale;
        HandoutLayout lay = layout();
        if (!InRect(lay.card, x, y) || InRect(lay.cancel, x, y)) {
            CloseHandout(false);
            return true;
        }
        if (InRect(lay.save, x, y)) {
            CloseHandout(true);
            return true;
        }
        if (InRect(lay.field, x, y))
            handout_.caret = handout_.text.size();
        handoutSurface_->Invalidate();
        return true;
    }
    if (m == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            CloseHandout(false);
            return true;
        }
        if (wp == VK_RETURN) {
            handout_.text.insert(handout_.caret, 1, L'\n');
            handout_.caret += 1;
            handoutSurface_->Invalidate();
            return true;
        }
        if (wp == VK_BACK && handout_.caret > 0) {
            size_t from = handout_.caret >= 2 && IS_LOW_SURROGATE(handout_.text[handout_.caret - 1]) && IS_HIGH_SURROGATE(handout_.text[handout_.caret - 2])
                              ? handout_.caret - 2
                              : handout_.caret - 1;
            handout_.text.erase(from, handout_.caret - from);
            handout_.caret = from;
            handoutSurface_->Invalidate();
            return true;
        }
        if (wp == VK_DELETE && handout_.caret < handout_.text.size()) {
            size_t to = handout_.caret + 1 < handout_.text.size() && IS_HIGH_SURROGATE(handout_.text[handout_.caret]) && IS_LOW_SURROGATE(handout_.text[handout_.caret + 1])
                            ? handout_.caret + 2
                            : handout_.caret + 1;
            handout_.text.erase(handout_.caret, to - handout_.caret);
            handoutSurface_->Invalidate();
            return true;
        }
        if (wp == VK_LEFT)
            handout_.caret = handout_.caret ? handout_.caret - 1 : 0;
        else if (wp == VK_RIGHT)
            handout_.caret = std::min(handout_.caret + 1, handout_.text.size());
        else if (wp == VK_HOME)
            handout_.caret = 0;
        else if (wp == VK_END)
            handout_.caret = handout_.text.size();
        else
            return false;
        handoutSurface_->Invalidate();
        return true;
    }
    if (m == WM_CHAR) {
        wchar_t ch = wchar_t(wp);
        if (ch < 32 || ch == 127)
            return true;
        handout_.text.insert(handout_.caret, 1, ch);
        handout_.caret += 1;
        handoutSurface_->Invalidate();
        return true;
    }
    return false;
}

void DesktopWidgets::OpenStyle(Widget& w)
{
    if (!w.surface || (w.kind == WidgetKind::Calendar && w.content.calLock))
        return;
    CloseTodo(false);
    CloseHandout(false);
    style_.open = true;
    style_.owner = &w;
    style_.calendar = w.kind == WidgetKind::Calendar;
    style_.pick = w.content.pickColor;
    style_.ink = w.content.fontColor;
    style_.bg = w.content.bgColor;
    style_.datePx = w.content.ownDate;
    style_.tablePx = w.content.ownTable;
    style_.image = w.content.bgImage;
    if (!styleSurface_) {
        styleSurface_ = std::make_unique<Surface>(instance_, L"调节", true, 0.f, false);
        styleSurface_->SetGlassless(true);
        styleSurface_->SetRadius(0);
        styleSurface_->paint = [this](Canvas& c) { PaintStyle(c); };
        styleSurface_->message = [this](UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) { return StyleMessage(msg, wp, lp, result); };
        styleSurface_->onDismiss = [this] { CloseStyle(false); };
    }
    RECT mon = WidgetMonitor(w.surface->Rect());
    POINT anchor{(mon.left + mon.right) / 2, (mon.top + mon.bottom) / 2};
    styleSurface_->Show(mon, true, anchor, false);
    SetForegroundWindow(styleSurface_->Hwnd());
    styleSurface_->Invalidate();
}

void DesktopWidgets::CloseStyle(bool save)
{
    if (!style_.open && !(styleSurface_ && styleSurface_->Visible()))
        return;
    Widget* owner = style_.owner;
    style_.open = false;
    style_.owner = nullptr;
    if (styleSurface_)
        styleSurface_->Hide(false);
    if (!owner || !Has(owner))
        return;
    if (!save) {
        owner->content.pickColor = style_.pick;
        owner->content.fontColor = style_.ink;
        owner->content.bgColor = style_.bg;
        owner->content.ownDate = style_.datePx;
        owner->content.ownTable = style_.tablePx;
        owner->content.bgImage = style_.image;
        owner->content.backdrop.reset();
        owner->content.backdropKey.clear();
    }
    Save();
    if (owner->surface)
        owner->surface->Invalidate();
}

void DesktopWidgets::PaintStyle(Canvas& c)
{
    if (!styleSurface_ || !style_.open || !style_.owner || !Has(style_.owner))
        return;
    Widget& w = *style_.owner;
    RECT rect = styleSurface_->Rect();
    float scale = std::max(styleSurface_->Scale(), 0.01f);
    float width = float(rect.right - rect.left) / scale;
    float height = float(rect.bottom - rect.top) / scale;
    StyleLayout lay = LayoutStyle(width, height, style_.calendar);
    c.halo = false;
    c.Fill(D2D1::RectF(0, 0, width, height), 0, D2D1::ColorF(0, 0, 0, 0.45f));
    c.Fill(lay.card, 18, D2D1::ColorF(0x1C1C1E, 0.98f));
    c.Stroke(lay.card, 18, D2D1::ColorF(0x0A84FF, 0.85f), 1.4f);
    const D2D1_COLOR_F ink = D2D1::ColorF(1, 1, 1, 0.96f);
    const D2D1_COLOR_F dim = D2D1::ColorF(1, 1, 1, 0.62f);
    c.Text(L"调节", D2D1::RectF(lay.card.left + 20, lay.card.top + 14, lay.card.right - 20, lay.card.top + 40), 18, ink, DWRITE_FONT_WEIGHT_BOLD);
    c.Text(L"只影响这个组件", D2D1::RectF(lay.card.left + 72, lay.card.top + 18, lay.card.right - 20, lay.card.top + 40), 12, dim);
    auto swatches = [&](int which, const wchar_t* label, uint32_t current) {
        if (!lay.rowOn[which])
            return;
        float top = lay.dot[which][0].top;
        c.Text(label, D2D1::RectF(lay.card.left + 20, top, lay.card.left + 126, top + 26), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        for (int i = 0; i < kSwatchCount; ++i) {
            D2D1_RECT_F r = lay.dot[which][i];
            float cx = (r.left + r.right) / 2.f;
            float cy = (r.top + r.bottom) / 2.f;
            bool on = kSwatches[i] == (current & 0xFFFFFFu) || (kSwatches[i] == 0 && current == 0);
            if (on)
                c.Circle(cx, cy, 13.f, D2D1::ColorF(1, 1, 1, 0.92f));
            if (kSwatches[i] == 0) {
                c.Circle(cx, cy, 10.f, D2D1::ColorF(1, 1, 1, 0.12f));
                c.Line(cx - 6, cy + 6, cx + 6, cy - 6, D2D1::ColorF(1, 1, 1, 0.75f), 1.4f);
            } else {
                c.Circle(cx, cy, 10.f, D2D1::ColorF(kSwatches[i], 1.f));
            }
        }
    };
    swatches(0, L"选中颜色", w.content.pickColor);
    swatches(1, L"字体颜色", w.content.fontColor);
    if (style_.calendar) {
        float datePx = w.content.ownDate >= 12.f ? w.content.ownDate : world_.dateSize;
        float tablePx = w.content.ownTable >= 16.f ? w.content.ownTable : world_.tableSize;
        c.Text(L"日期字号", D2D1::RectF(lay.card.left + 20, lay.dateRow, lay.card.left + 126, lay.dateRow + 32), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Glyph(L"\uE76B", lay.datePrev, 14, ink);
        c.Glyph(L"\uE76C", lay.dateNext, 14, ink);
        c.Text(std::to_wstring(int(std::lround(datePx))), D2D1::RectF(lay.datePrev.right, lay.dateRow, lay.dateNext.left, lay.dateRow + 32), 16, ink,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(L"课表日期", D2D1::RectF(lay.card.left + 20, lay.tableRow, lay.card.left + 126, lay.tableRow + 32), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Glyph(L"\uE76B", lay.tablePrev, 14, ink);
        c.Glyph(L"\uE76C", lay.tableNext, 14, ink);
        c.Text(std::to_wstring(int(std::lround(tablePx))), D2D1::RectF(lay.tablePrev.right, lay.tableRow, lay.tableNext.left, lay.tableRow + 32), 16, ink,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    swatches(2, L"背景颜色", w.content.bgColor);
    c.Text(L"背景图片", D2D1::RectF(lay.card.left + 20, lay.image.top, lay.card.left + 126, lay.image.bottom), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Fill(lay.image, 8, D2D1::ColorF(0x0A84FF, 0.22f));
    c.Text(L"选择图片", lay.image, 13, D2D1::ColorF(0x0A84FF, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Fill(lay.clear, 8, D2D1::ColorF(1, 1, 1, 0.08f));
    c.Text(L"清除", lay.clear, 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    if (!w.content.bgImage.empty())
        c.Text(FileTail(w.content.bgImage), D2D1::RectF(lay.clear.right + 10, lay.image.top, lay.card.right - 16, lay.image.bottom), 12, dim);
    c.Fill(lay.done, 10, D2D1::ColorF(0x0A84FF, 1));
    c.Text(L"完成", lay.done, 14, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
}

bool DesktopWidgets::StyleMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT&)
{
    if (!style_.open || !styleSurface_ || !style_.owner || !Has(style_.owner))
        return false;
    Widget& w = *style_.owner;
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) {
        CloseStyle(false);
        return true;
    }
    if (m == WM_KEYDOWN && wp == VK_RETURN) {
        CloseStyle(true);
        return true;
    }
    if (m != WM_LBUTTONUP)
        return false;
    RECT rect = styleSurface_->Rect();
    float scale = std::max(styleSurface_->Scale(), 0.01f);
    float x = float(GET_X_LPARAM(lp)) / scale;
    float y = float(GET_Y_LPARAM(lp)) / scale;
    StyleLayout lay = LayoutStyle(float(rect.right - rect.left) / scale, float(rect.bottom - rect.top) / scale, style_.calendar);
    if (!InRect(lay.card, x, y)) {
        CloseStyle(false);
        return true;
    }
    auto touch = [&] {
        if (w.surface)
            w.surface->Invalidate();
        styleSurface_->Invalidate();
    };
    for (int which = 0; which < 3; ++which) {
        if (!lay.rowOn[which])
            continue;
        for (int i = 0; i < kSwatchCount; ++i) {
            if (!InRect(lay.dot[which][i], x, y))
                continue;
            uint32_t color = kSwatches[i];
            if (which == 0)
                w.content.pickColor = color;
            else if (which == 1)
                w.content.fontColor = color;
            else
                w.content.bgColor = color;
            touch();
            return true;
        }
    }
    if (style_.calendar && InRect(lay.datePrev, x, y)) {
        float cur = w.content.ownDate >= 12.f ? w.content.ownDate : world_.dateSize;
        w.content.ownDate = std::clamp(cur - 1.f, 12.f, 36.f);
        touch();
        return true;
    }
    if (style_.calendar && InRect(lay.dateNext, x, y)) {
        float cur = w.content.ownDate >= 12.f ? w.content.ownDate : world_.dateSize;
        w.content.ownDate = std::clamp(cur + 1.f, 12.f, 36.f);
        touch();
        return true;
    }
    if (style_.calendar && InRect(lay.tablePrev, x, y)) {
        float cur = w.content.ownTable >= 16.f ? w.content.ownTable : world_.tableSize;
        w.content.ownTable = std::clamp(cur - 1.f, 16.f, 48.f);
        touch();
        return true;
    }
    if (style_.calendar && InRect(lay.tableNext, x, y)) {
        float cur = w.content.ownTable >= 16.f ? w.content.ownTable : world_.tableSize;
        w.content.ownTable = std::clamp(cur + 1.f, 16.f, 48.f);
        touch();
        return true;
    }
    if (InRect(lay.clear, x, y)) {
        w.content.bgImage.clear();
        w.content.backdrop.reset();
        w.content.backdropKey.clear();
        touch();
        return true;
    }
    if (InRect(lay.done, x, y)) {
        CloseStyle(true);
        return true;
    }
    if (InRect(lay.image, x, y)) {
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
            return true;
        FILEOPENDIALOGOPTIONS opts = 0;
        dialog->GetOptions(&opts);
        dialog->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        COMDLG_FILTERSPEC spec[] = {{L"图片", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff"}};
        dialog->SetFileTypes(1, spec);
        dialog->SetTitle(L"选择背景图片");
        winrt::com_ptr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->Show(styleSurface_->Hwnd())) && SUCCEEDED(dialog->GetResult(item.put())) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            if (style_.open && style_.owner && Has(style_.owner)) {
                style_.owner->content.bgImage = path;
                style_.owner->content.backdrop.reset();
                style_.owner->content.backdropKey.clear();
                if (style_.owner->surface)
                    style_.owner->surface->Invalidate();
                styleSurface_->Invalidate();
            }
            CoTaskMemFree(path);
        }
        return true;
    }
    return true;
}

namespace {

struct ToolLayout {
    D2D1_RECT_F card{}, aDec{}, aInc{}, bDec{}, bInc{}, go{}, reset{}, body{}, dismiss{};
};

ToolLayout LayoutTool(float width, float height, int kind)
{
    const float cardW = std::min(420.f, std::max(300.f, width - 48.f));
    const float cardH = std::min(kind == 3 ? 420.f : 300.f, std::max(260.f, height - 48.f));
    const float x = (width - cardW) / 2.f;
    const float y = (height - cardH) / 2.f;
    ToolLayout lay;
    lay.card = D2D1::RectF(x, y, x + cardW, y + cardH);
    lay.aDec = D2D1::RectF(x + 28, y + 128, x + 60, y + 160);
    lay.aInc = D2D1::RectF(x + 150, y + 128, x + 182, y + 160);
    lay.bDec = D2D1::RectF(x + cardW / 2.f + 8, y + 128, x + cardW / 2.f + 40, y + 160);
    lay.bInc = D2D1::RectF(x + cardW - 60, y + 128, x + cardW - 28, y + 160);
    lay.go = D2D1::RectF(x + 24, y + cardH - 62, x + 24 + (cardW - 60) / 2.f, y + cardH - 22);
    lay.reset = D2D1::RectF(lay.go.right + 12, lay.go.top, x + cardW - 24, lay.go.bottom);
    lay.dismiss = D2D1::RectF(x + cardW - 40, y + 8, x + cardW - 8, y + 40);
    lay.body = D2D1::RectF(x + 24, y + 176, x + cardW - 24, lay.go.top - 10);
    return lay;
}

std::wstring ClockText(int ms)
{
    ms = std::max(0, ms);
    int s = ms / 1000;
    wchar_t b[16];
    swprintf_s(b, L"%02d:%02d", s / 60, s % 60);
    return b;
}

} // namespace

void DesktopWidgets::OpenTool(Widget& w, int kind)
{
    if (!w.surface || w.content.calLock)
        return;
    kind = std::clamp(kind, 0, 3);
    CloseTodo(false);
    CloseHandout(false);
    CloseStyle(false);
    tool_.open = true;
    tool_.owner = &w;
    tool_.kind = kind;
    tool_.running = false;
    tool_.swRun = false;
    tool_.result.clear();
    tool_.hasRoll = false;
    if (!toolSurface_) {
        toolSurface_ = std::make_unique<Surface>(instance_, L"小工具", true, 0.f, false);
        toolSurface_->SetGlassless(true);
        toolSurface_->SetRadius(0);
        toolSurface_->paint = [this](Canvas& c) { PaintTool(c); };
        toolSurface_->message = [this](UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) { return ToolMessage(msg, wp, lp, result); };
        toolSurface_->onDismiss = [this] { CloseTool(); };
    }
    RECT mon = WidgetMonitor(w.surface->Rect());
    POINT anchor{(mon.left + mon.right) / 2, (mon.top + mon.bottom) / 2};
    toolSurface_->Show(mon, true, anchor, false);
    SetForegroundWindow(toolSurface_->Hwnd());
    toolSurface_->Invalidate();
}

void DesktopWidgets::CloseTool()
{
    if (!tool_.open && !(toolSurface_ && toolSurface_->Visible()))
        return;
    tool_.open = false;
    tool_.running = false;
    tool_.swRun = false;
    tool_.owner = nullptr;
    KillTimer(host_, kToolTimer);
    if (toolSurface_)
        toolSurface_->Hide(false);
}

void DesktopWidgets::PaintTool(Canvas& c)
{
    if (!toolSurface_ || !tool_.open)
        return;
    RECT rect = toolSurface_->Rect();
    float scale = std::max(toolSurface_->Scale(), 0.01f);
    float width = float(rect.right - rect.left) / scale;
    float height = float(rect.bottom - rect.top) / scale;
    ToolLayout lay = LayoutTool(width, height, tool_.kind);
    c.halo = false;
    c.Fill(D2D1::RectF(0, 0, width, height), 0, D2D1::ColorF(0, 0, 0, 0.45f));
    c.Fill(lay.card, 18, D2D1::ColorF(0x1C1C1E, 0.98f));
    c.Stroke(lay.card, 18, D2D1::ColorF(0x0A84FF, 0.85f), 1.4f);
    if (tool_.kind <= 3)
        c.Glyph(L"\uE711", lay.dismiss, 15, D2D1::ColorF(1, 1, 1, 0.9f));
    const D2D1_COLOR_F ink = D2D1::ColorF(1, 1, 1, 0.96f);
    const D2D1_COLOR_F dim = D2D1::ColorF(1, 1, 1, 0.62f);
    const D2D1_COLOR_F blue = D2D1::ColorF(0x0A84FF, 1);
    const wchar_t* title[] = {L"倒计时", L"秒表", L"随机抽签", L"分组"};
    if (tool_.kind >= 0 && tool_.kind <= 3)
        c.Text(title[tool_.kind], D2D1::RectF(lay.card.left + 20, lay.card.top + 16, lay.card.right - 20, lay.card.top + 46), 18, ink, DWRITE_FONT_WEIGHT_BOLD);
    auto stepper = [&](D2D1_RECT_F dec, D2D1_RECT_F inc, const wchar_t* label, int value) {
        c.Text(label, D2D1::RectF(dec.left, dec.top - 22, inc.right, dec.top), 12, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Glyph(L"\uE76B", dec, 14, ink);
        c.Glyph(L"\uE76C", inc, 14, ink);
        c.Text(std::to_wstring(value), D2D1::RectF(dec.right, dec.top, inc.left, dec.bottom), 16, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    };
    if (tool_.kind == 0) {
        int ms = tool_.remainMs;
        if (tool_.running)
            ms -= int(GetTickCount64() - tool_.mark);
        c.Text(ClockText(ms), D2D1::RectF(lay.card.left, lay.card.top + 52, lay.card.right, lay.card.top + 112), 42, ink, DWRITE_FONT_WEIGHT_BOLD, 1);
        if (!tool_.running)
            stepper(lay.aDec, lay.aInc, L"分钟", tool_.minutes);
        c.Fill(lay.go, 10, blue);
        c.Text(tool_.running ? L"暂停" : L"开始", lay.go, 15, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Fill(lay.reset, 10, D2D1::ColorF(1, 1, 1, 0.08f));
        c.Text(L"重置", lay.reset, 15, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    } else if (tool_.kind == 1) {
        ULONGLONG ms = tool_.swBase;
        if (tool_.swRun)
            ms += GetTickCount64() - tool_.mark;
        c.Text(ClockText(int(ms)), D2D1::RectF(lay.card.left, lay.card.top + 70, lay.card.right, lay.card.top + 140), 42, ink, DWRITE_FONT_WEIGHT_BOLD, 1);
        c.Fill(lay.go, 10, blue);
        c.Text(tool_.swRun ? L"暂停" : L"开始", lay.go, 15, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Fill(lay.reset, 10, D2D1::ColorF(1, 1, 1, 0.08f));
        c.Text(L"重置", lay.reset, 15, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    } else if (tool_.kind == 2) {
        c.Text(tool_.hasRoll ? std::to_wstring(tool_.rolled) : L"—", D2D1::RectF(lay.card.left, lay.card.top + 48, lay.card.right, lay.card.top + 108), 42, ink,
               DWRITE_FONT_WEIGHT_BOLD, 1);
        stepper(lay.aDec, lay.aInc, L"最小", tool_.lo);
        stepper(lay.bDec, lay.bInc, L"最大", tool_.hi);
        c.Fill(lay.go, 10, blue);
        c.Text(L"抽取", lay.go, 15, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Fill(lay.reset, 10, D2D1::ColorF(1, 1, 1, 0.08f));
        c.Text(L"清空", lay.reset, 15, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    } else if (tool_.kind == 3) {
        stepper(lay.aDec, lay.aInc, L"人数", tool_.people);
        stepper(lay.bDec, lay.bInc, L"组数", tool_.groups);
        if (tool_.result.empty())
            c.Text(L"按编号随机分组", lay.body, 14, dim, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        else
            c.Text(tool_.result, lay.body, 13, ink, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        c.Fill(lay.go, 10, blue);
        c.Text(L"分组", lay.go, 15, D2D1::ColorF(1, 1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Fill(lay.reset, 10, D2D1::ColorF(1, 1, 1, 0.08f));
        c.Text(L"清空", lay.reset, 15, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

bool DesktopWidgets::ToolMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT&)
{
    if (!tool_.open || !toolSurface_)
        return false;
    if (m == WM_KEYDOWN && (wp == VK_ESCAPE || wp == VK_RETURN))
        return true;
    if (m != WM_LBUTTONUP)
        return false;
    RECT rect = toolSurface_->Rect();
    float scale = std::max(toolSurface_->Scale(), 0.01f);
    float x = float(GET_X_LPARAM(lp)) / scale;
    float y = float(GET_Y_LPARAM(lp)) / scale;
    ToolLayout lay = LayoutTool(float(rect.right - rect.left) / scale, float(rect.bottom - rect.top) / scale, tool_.kind);
    if (tool_.kind <= 3 && InRect(lay.dismiss, x, y)) {
        CloseTool();
        return true;
    }
    if (!InRect(lay.card, x, y))
        return true;
    auto nudge = [&](int& value, int delta, int lo, int hi) { value = std::clamp(value + delta, lo, hi); };
    if (tool_.kind == 0) {
        if (!tool_.running && InRect(lay.aDec, x, y)) {
            nudge(tool_.minutes, -1, 1, 180);
            tool_.remainMs = tool_.minutes * 60 * 1000;
        } else if (!tool_.running && InRect(lay.aInc, x, y)) {
            nudge(tool_.minutes, 1, 1, 180);
            tool_.remainMs = tool_.minutes * 60 * 1000;
        } else if (InRect(lay.go, x, y)) {
            if (tool_.running) {
                tool_.remainMs = std::max(0, tool_.remainMs - int(GetTickCount64() - tool_.mark));
                tool_.running = false;
                KillTimer(host_, kToolTimer);
            } else {
                if (tool_.remainMs <= 0)
                    tool_.remainMs = tool_.minutes * 60 * 1000;
                tool_.mark = GetTickCount64();
                tool_.running = true;
                SetTimer(host_, kToolTimer, 100, nullptr);
            }
        } else if (InRect(lay.reset, x, y)) {
            tool_.running = false;
            tool_.remainMs = tool_.minutes * 60 * 1000;
            KillTimer(host_, kToolTimer);
        }
    } else if (tool_.kind == 1) {
        if (InRect(lay.go, x, y)) {
            if (tool_.swRun) {
                tool_.swBase += GetTickCount64() - tool_.mark;
                tool_.swRun = false;
                KillTimer(host_, kToolTimer);
            } else {
                tool_.mark = GetTickCount64();
                tool_.swRun = true;
                SetTimer(host_, kToolTimer, 100, nullptr);
            }
        } else if (InRect(lay.reset, x, y)) {
            tool_.swRun = false;
            tool_.swBase = 0;
            KillTimer(host_, kToolTimer);
        }
    } else if (tool_.kind == 2) {
        if (InRect(lay.aDec, x, y))
            nudge(tool_.lo, -1, 0, tool_.hi);
        else if (InRect(lay.aInc, x, y))
            nudge(tool_.lo, 1, 0, tool_.hi);
        else if (InRect(lay.bDec, x, y))
            nudge(tool_.hi, -1, tool_.lo, 999);
        else if (InRect(lay.bInc, x, y))
            nudge(tool_.hi, 1, tool_.lo, 999);
        else if (InRect(lay.go, x, y)) {
            std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> dist(tool_.lo, std::max(tool_.lo, tool_.hi));
            tool_.rolled = dist(rng);
            tool_.hasRoll = true;
        } else if (InRect(lay.reset, x, y)) {
            tool_.hasRoll = false;
        }
    } else if (tool_.kind == 3) {
        if (InRect(lay.aDec, x, y)) {
            nudge(tool_.people, -1, 2, 200);
            if (tool_.groups > tool_.people)
                tool_.groups = tool_.people;
        } else if (InRect(lay.aInc, x, y))
            nudge(tool_.people, 1, 2, 200);
        else if (InRect(lay.bDec, x, y))
            nudge(tool_.groups, -1, 2, tool_.people);
        else if (InRect(lay.bInc, x, y))
            nudge(tool_.groups, 1, 2, tool_.people);
        else if (InRect(lay.go, x, y)) {
            std::vector<int> id(size_t(tool_.people));
            for (int i = 0; i < tool_.people; ++i)
                id[size_t(i)] = i + 1;
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(id.begin(), id.end(), rng);
            int base = tool_.people / tool_.groups;
            int extra = tool_.people % tool_.groups;
            std::wstring out;
            int at = 0;
            for (int g = 0; g < tool_.groups; ++g) {
                int count = base + (g < extra ? 1 : 0);
                out += L"第" + std::to_wstring(g + 1) + L"组：";
                for (int k = 0; k < count; ++k) {
                    if (k)
                        out += L"、";
                    out += std::to_wstring(id[size_t(at++)]);
                }
                if (g + 1 < tool_.groups)
                    out += L"\n";
            }
            tool_.result = std::move(out);
        } else if (InRect(lay.reset, x, y)) {
            tool_.result.clear();
        }
    }
    if (toolSurface_)
        toolSurface_->Invalidate();
    return true;
}

void DesktopWidgets::OpenPomo(Widget& w)
{
    if (w.content.pomoRun)
        return;
    int minutes = std::clamp(w.content.pomoMin, 5, 180);
    w.content.pomoMin = minutes;
    w.content.pomoRun = true;
    w.content.pomoLeft = minutes * 60;
    w.pomoEnd = GetTickCount64() + ULONGLONG(minutes) * 60 * 1000;
    pomoLeft_ = w.content.pomoLeft;
    if (!pomo_) {
        static bool registered = false;
        const wchar_t* cls = L"AtriumFocus";
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.hInstance = instance_;
            wc.lpfnWndProc = PomoProc;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = cls;
            RegisterClassExW(&wc);
            registered = true;
        }
        pomo_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls, L"", WS_POPUP, GetSystemMetrics(SM_XVIRTUALSCREEN),
                                GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                                nullptr, nullptr, instance_, nullptr);
    }
    ShowWindow(pomo_, SW_SHOW);
    SetWindowPos(pomo_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(pomo_);
    if (w.surface)
        w.surface->Invalidate();
    Log(L"widgets: tomato started for %d min", minutes);
}

void DesktopWidgets::ClosePomo()
{
    if (pomo_) {
        DestroyWindow(pomo_);
        pomo_ = nullptr;
    }
    pomoLeft_ = 0;
    for (auto& widget : widgets_)
        if (widget->kind == WidgetKind::Focus) {
            widget->content.pomoRun = false;
            if (widget->surface)
                widget->surface->Invalidate();
        }
}

LRESULT CALLBACK DesktopWidgets::PomoProc(HWND hwnd, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE || (m == WM_SYSCOMMAND && wp == SC_CLOSE))
        return 0;
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (g_widgets)
            g_widgets->ClosePomo();
        return 0;
    }
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_MOUSEWHEEL)
        return 0;
    if (m == WM_ERASEBKGND)
        return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{};
        GetClientRect(hwnd, &r);
        HBRUSH brush = CreateSolidBrush(RGB(8, 8, 12));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        int sec = g_widgets && g_widgets->pomoLeft_ > 0 ? g_widgets->pomoLeft_ : 0;
        wchar_t buf[16];
        swprintf_s(buf, L"%02d:%02d", sec / 60, sec % 60);
        int px = int((r.bottom - r.top) / 6);
        if (px < 48)
            px = 48;
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        DrawTextW(dc, buf, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT sub = r;
        sub.top = r.top + (r.bottom - r.top) / 2 + (r.bottom - r.top) / 10;
        HFONT caption = CreateFontW(-28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SelectObject(dc, caption);
        SetTextColor(dc, RGB(180, 180, 188));
        DrawTextW(dc, L"专注中", -1, &sub, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
        DeleteObject(caption);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
}

} // namespace atrium
