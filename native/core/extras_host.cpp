#include "extras_host.h"

#include "bar.h"
#include "log.h"
#include "posted.h"
#include "settings.h"

namespace yunzhao {

namespace {

// The workers' news, on ExtrasHost's own window.
constexpr UINT WM_STACKS_READY = WM_APP + 2;
constexpr wchar_t kWindowClass[] = L"YunzhaoExtras";

// A click on an entry whose popup is open closes it; the click that just closed it (the popup lost
// the focus to the same bar) does not open it again.
template<class Popup> bool JustClosed(const Popup& popup, bool sameBar) { return sameBar && GetTickCount64() - popup.LastClosed() < 400; }

} // namespace

HWND ExtrasHost::CreateHost(HINSTANCE instance, ExtrasHost* self)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc); // a second host (never made) would find it registered
    const HWND h = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (h)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return h;
}

LRESULT CALLBACK ExtrasHost::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<ExtrasHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && m == WM_STACKS_READY) {
        self->stacks_.Refresh();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

ExtrasHost::ExtrasHost(HINSTANCE instance, AppCatalog& catalog, ForEachBar forBars)
    : forBars_(std::move(forBars)),
      host_(CreateHost(instance, this)),
      launchpad_(instance, catalog),
      stackModel_(host_, WM_STACKS_READY),
      stacks_(instance, stackModel_)
{
    stacks_.onManageFolders = [this] {
        if (onOpenSettings)
            onOpenSettings(L"stacks");
    };
}

ExtrasHost::~ExtrasHost()
{
    Shutdown();
    if (host_) {
        SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
        DestroyWindow(host_);
    }
}

void ExtrasHost::Apply(const Settings& s, float radius)
{
    const bool stacksChanged =
        !stacksCached_ || stacksOn_ != s.showFolderStacks || lastFolderStacks_ != s.folderStacks || lastFolderStackFiles_ != s.folderStackFiles;
    stacksCached_ = true;
    stacksOn_ = s.showFolderStacks;
    lastFolderStacks_ = s.folderStacks;
    lastFolderStackFiles_ = s.folderStackFiles;
    stacks_.SetOptions(s.folderStackLayout != L"column", size_t(s.folderStackMax));
    if (stacksChanged)
        stackModel_.SetFolders(stacksOn_ ? s.folderStacks : std::vector<std::wstring>{}, stacksOn_ ? s.folderStackFiles : std::vector<std::wstring>{},
                               96); // read once at a size every scale draws from
    if (!stacksOn_)
        stacks_.Close();
    launchpadOn_ = s.showLaunchpad;
    launchpad_.SetTint(s.launchpadMaterial == L"tint");
    launchpad_.SetFullscreen(s.launchpadLayout == L"fullscreen");
    if (!launchpadOn_)
        launchpad_.Close(false);
    PrewarmLaunchpad();
    launchpad_.SetRadius(radius);
    stacks_.SetRadius(radius);
    // The bar's own entries: Launchpad beside the logo, the folder stack left of the tray.
    const bool lp = launchpadOn_, st = stacksOn_;
    forBars_(std::function<void(Bar&)>([lp, st](Bar& bar) {
        if (bar.ExtrasShown() == std::make_pair(lp, st))
            return; // the entries are already like this: no relayout
        bar.SetExtrasShown(lp, st);
        bar.Relayout(); // the entries come or go
    }));
}

void ExtrasHost::ConfigureBar(Bar& bar)
{
    bar.onExtraClick = [this, &bar](const std::wstring& key, RECT entry) { Click(bar, key, entry); };
}

void ExtrasHost::OnBarRetiring(const Bar* bar)
{
    if (!bar || lastBar_ != bar)
        return;
    lastBar_ = nullptr;
    launchpad_.Close(false);
    stacks_.Close();
}

void ExtrasHost::Click(Bar& bar, const std::wstring& key, RECT entry)
{
    const bool sameBar = &bar == lastBar_;
    if (key == L"#launchpad") {
        if (sameBar && (launchpad_.Visible() || JustClosed(launchpad_, sameBar))) {
            launchpad_.Close(false);
            return;
        }
        lastBar_ = &bar;
        MONITORINFO mi{sizeof(mi)};
        RECT barRect = bar.BarRect();
        GetMonitorInfoW(MonitorFromRect(&barRect, MONITOR_DEFAULTTONEAREST), &mi);
        launchpad_.Open(mi.rcWork, bar.PreviousForeground());
    } else if (key == L"#stacks") {
        if (sameBar && (stacks_.Visible() || JustClosed(stacks_, sameBar))) {
            stacks_.Close();
            return;
        }
        lastBar_ = &bar;
        stacks_.Open(entry, bar.BarRect());
    } else {
        return;
    }
    if (onPanelsChanged)
        onPanelsChanged();
}

void ExtrasHost::OnCatalogChanged(bool listChanged)
{
    launchpad_.Refresh();
    if (listChanged)
        PrewarmLaunchpad();
}

// Every app's icon read ahead for the main display, so Launchpad opens with them all.
void ExtrasHost::PrewarmLaunchpad()
{
    if (!launchpadOn_)
        return;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi))
        launchpad_.Prewarm(mi.rcWork);
}

void ExtrasHost::ClosePopups()
{
    lastBar_ = nullptr;
    launchpad_.Close(false);
    stacks_.Close();
}

bool ExtrasHost::AnyPopupVisible() const { return launchpad_.Visible() || stacks_.Visible(); }

void ExtrasHost::Shutdown() { ClosePopups(); }

} // namespace yunzhao
