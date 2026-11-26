// The bar's two newer entries behind one door: Launchpad (next to the start logo) and the folder
// stack (left of the tray). It owns their models and popups, and a window of its own for their
// workers' messages; main.cpp hands it the settings, each bar to configure, the catalog's news and
// the moments every popup closes.
//
// Upstream 3.0 also had a now-playing entry here, with five bars by the status ring and a card.
// Yunzhao leaves that out: the lyrics and the track belong to the island above the desktop, so the
// bar carries no player of its own.
#pragma once

#include "shell/folder_stacks.h"
#include "ui/folder_stack_popup.h"
#include "ui/launchpad.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yunzhao {

class AppCatalog;
class Bar;
struct Settings;

class ExtrasHost {
public:
    using ForEachBar = std::function<void(const std::function<void(Bar&)>&)>;
    // forBars: every bar now on screen.
    ExtrasHost(HINSTANCE instance, AppCatalog& catalog, ForEachBar forBars);
    ~ExtrasHost();
    ExtrasHost(const ExtrasHost&) = delete;
    ExtrasHost& operator=(const ExtrasHost&) = delete;

    // The settings as applied; radius: the popups' corner (the bar's, as the other popups get it).
    void Apply(const Settings& s, float radius);
    void ConfigureBar(Bar& bar);
    void OnBarRetiring(const Bar* bar);
    void OnCatalogChanged(bool listChanged = true);
    void ClosePopups();
    bool AnyPopupVisible() const;
    void Shutdown(); // before the bars go: the stack's worker thread ends

    std::function<void()> onPanelsChanged;                   // a popup opened (the notification cards step aside)
    std::function<void(const wchar_t* page)> onOpenSettings; // the empty stack's button
    void SetAppMenu(decltype(Launchpad::onAppMenu) menu) { launchpad_.onAppMenu = std::move(menu); } // Launchpad's right click

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static HWND CreateHost(HINSTANCE instance, ExtrasHost* self);
    void Click(Bar& bar, const std::wstring& key, RECT entry);
    void PrewarmLaunchpad(); // every app's icon, for the main display

    ForEachBar forBars_;
    HWND host_; // message-only: the stack's news
    Launchpad launchpad_;
    FolderStackModel stackModel_;
    FolderStackPopup stacks_;
    bool stacksOn_ = false, launchpadOn_ = false;
    bool stacksCached_ = false;
    std::vector<std::wstring> lastFolderStacks_;
    std::vector<std::wstring> lastFolderStackFiles_;
    Bar* lastBar_ = nullptr;
};

} // namespace yunzhao
