// Desktop widgets after macOS's (Sonoma and later): pieces of the bar's glass
// on the desktop, in a grid on the selected desktop display whose halves hug its edges. They
// stand just above the desktop, under every window, always clear glass with
// its ink and a few of macOS's colours (widget_faces). Widgets never become
// topmost. Drag one to move it (it snaps to the
// grid). Drag one anywhere on either display; near another it snaps to it, and
// none may cover a neighbour or a desktop icon. The calendar follows the pointer
// to any size. Right-click for its sizes, to
// remove it or to add another. Notes and reminders take typing (the input
// method too) after a click. The layout and the widgets' content are saved in
// %LOCALAPPDATA%\Yunzhao\widgets.json.
#pragma once

#include "ui/glass_menu.h"
#include "ui/widget_faces.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace yunzhao {

class DesktopWidgets {
public:
    explicit DesktopWidgets(HINSTANCE instance);
    ~DesktopWidgets();
    DesktopWidgets(const DesktopWidgets&) = delete;
    DesktopWidgets& operator=(const DesktopWidgets&) = delete;

    void SetEnabled(bool on);
    void SetStatus(const SystemStatus& status);
    void SetOnRight(bool onRight);
    // The bar's monitor: the widgets live in its work area, where its capture gives them glass.
    void SetMonitor(HMONITOR monitor, bool refresh = false);
    void SetMaterial(WidgetMaterial material);
    void SetCalendarType(float datePx, float tablePx);
    void SetTimetableBadge(float scale);
    // Adds a widget in the first free place; false when there is no room.
    bool Add(WidgetKind kind, WidgetSize size);
    // One of each kind. 1 added, 3 already on the desktop, 0 when there was no room to add.
    int Toggle(WidgetKind kind, WidgetSize size);
    // Every calendar: 0 shows only today, 1 fills that monitor's work area.
    void SetCalendarScale(float t);
    // The desktop calendar window. Off closes it and takes it off the desktop.
    bool CalendarOpen() const;
    void SetCalendarOpen(bool open);
    // The same by name ("clock", "medium"; the settings app's gallery).
    bool AddNamed(const std::wstring& kind, const std::wstring& size);
    static bool ParseNames(const std::wstring& kind, const std::wstring& size, WidgetKind& outKind, WidgetSize& outSize);

    std::function<void()> onCalendar;    // a calendar widget was clicked
    std::function<void()> onAddWidgets;  // "添加小组件…": the settings app's widget gallery
    std::function<void()> onStatusRequest;
    GlassMenu* menu = nullptr;           // a widget's menu (main's)

private:
    struct Widget;
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    void Build();
    void Clear();
    void Load();
    void Save() const;
    void PublishTodos() const;
    void PollYunzhaoCommand(Widget& w);
    void PublishYunzhaoState(const Widget& w) const;
    void SaveSoon(); // while typing
    Widget& Create(WidgetKind kind, WidgetSize size, int col, int row);
    bool Has(const Widget* w) const;
    void Remove(Widget& w);
    void RemoveNow(Widget& w);
    void Resize(Widget& w, WidgetSize size);
    void ApplyCalendarScale(Widget& w, float t, bool save);
    float CalendarFraction(const Widget& w) const;
    void WatchPopup(Widget& w, POINT content, bool insideWindow);
    void SyncPopupHook(); // a pinned todo stays until a click outside the widget
    // The grid in the work area of the bar's monitor.
    float Scale() const;
    RECT WorkArea() const;
    int Cols() const;
    int Rows() const;
    int VisualCol(int col, WidgetSize size) const;
    bool FromRight(int col, WidgetSize size) const;        // placed from the right edge
    RECT CellRect(int col, int row, WidgetSize size) const; // screen px
    RECT PlacedRect(const Widget& w) const;                  // free position, or the grid until one is saved
    RECT PixelRect(const Widget& w) const;
    RECT LegacyRect(const Widget& w) const;
    void RefreshIcons();
    std::vector<RECT> Blockers(const Widget* except) const;
    bool Blocked(const RECT& rect, const Widget* except) const;
    RECT ClampMonitor(RECT rect, bool allowShrink) const;
    RECT ClampOntoMonitor(RECT rect) const;      // pull a dropped widget back onto the nearest display
    RECT ClampVirtual(RECT rect) const;          // the combined work area, so a drag can cross displays
    RECT SnapToMonitor(RECT rect, bool allowShrink) const; // the display under most of the widget
    RECT Nudge(RECT desired, const Widget* self) const;  // snap, then slide off widgets and icons
    RECT Settle(RECT desired, const Widget* self) const; // Nudge, then a nearby free spot
    RECT FindSpot(int width, int height) const;
    void Remember(Widget& w, const RECT& settled);
    bool Fits(int col, int row, WidgetSize size, const Widget* except) const;
    bool FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const;
    void Place(Widget& w, bool glide);
    void Glide();
    void Restack();
    void Tick();
    void SyncYunzhao(Widget& w);
    // Keeps the calendar page and the timetable on separate windows. The widget
    // itself has no pixels while the calendar page is open.
    void OrderCalendarNote();
    // Slides the calendar up off the top edge, and back down when the pointer comes near.
    void PeekCalendar();
    bool IsOurs(HWND h) const;
    // Calendar: edge bits (1 left, 2 right, 4 top, 8 bottom). Other widgets: -1 or 1
    // on the bottom corners, 0 outside a grip.
    int GripAt(const Widget& w, POINT screen) const;
    void DragSize(Widget& w, POINT screen);
    bool Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    void Menu(Widget& w);
    void Activate(Widget& w);
    void Paint(Widget& w, Canvas& c);
    static void PaintShadow(Canvas& c, D2D1_RECT_F face);
    // Notes and reminders: a click starts typing, keys edit, losing the focus ends it.
    // The calendar does the same for the day under the click.
    void Click(Widget& w, POINT content);
    void TrackDay(Widget& w, POINT content);
    std::wstring DayAt(const Widget& w, POINT content) const;
    void StartEdit(Widget& w, int item);
    void EndEdit(Widget& w);
    std::wstring* EditBuffer(Widget& w);
    bool Key(Widget& w, UINT m, WPARAM wp);
    void Insert(Widget& w, const std::wstring& s);
    void PlaceImeWindow(Widget& w);
    // Photos: a picture from the folder, read and scaled on a worker thread.
    void PickFolder(Widget& w);
    void PickCity(Widget& w, POINT at);
    void LoadPhoto(Widget& w, bool another);
    void SampleSystem();
    void OpenPomo(Widget& w);
    void ClosePomo();
    static LRESULT CALLBACK PomoProc(HWND, UINT, WPARAM, LPARAM);
    void OpenTodo(Widget& w);
    void CloseTodo(bool save);
    void PaintTodo(Canvas& c);
    bool TodoMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    void OpenHandout(Widget& w, int weekday, int period);
    void CloseHandout(bool save);
    void PaintHandout(Canvas& c);
    bool HandoutMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    void OpenStyle(Widget& w);
    void CloseStyle(bool save);
    void PaintStyle(Canvas& c);
    bool StyleMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    void OpenTool(Widget& w, int kind);
    void CloseTool();
    void PaintTool(Canvas& c);
    bool ToolMessage(UINT m, WPARAM wp, LPARAM lp, LRESULT& r);

    HINSTANCE instance_;
    HWND host_ = nullptr; // timers, the menu's owner
    HWINEVENTHOOK hook_ = nullptr, showHook_ = nullptr, uncloakHook_ = nullptr;
    bool enabled_ = false;
    bool onRight_ = false;
    HMONITOR monitor_ = nullptr;
    std::vector<RECT> icons_; // desktop files, screen px, refreshed while placing
    double iconsMs_ = -1e9;
    std::vector<std::unique_ptr<Widget>> widgets_;
    // Saved widgets this screen has no room for (JSON): kept in the layout
    // file, with their notes, until a larger screen shows them again.
    std::vector<std::wstring> unplaced_;
    WidgetWorld world_;
    WidgetMaterial material_ = WidgetMaterial::Glass;
    std::unique_ptr<WeatherSource> weather_;
    std::unique_ptr<BluetoothBatteries> bluetooth_;
    ChineseCalendar lunar_;
    int lastMinute_ = -1;
    uint64_t cpuIdle_ = 0, cpuTotal_ = 0; // the last GetSystemTimes
    unsigned photoGen_ = 0;
    bool restackPosted_ = false;
    // Only Restack may change a widget's place in the z-order: anything else
    // (showing the window, a click, the system) is held back in WM_WINDOWPOSCHANGING.
    bool zMoving_ = false;
    HWND pomo_ = nullptr;
    int pomoLeft_ = 0;
    struct TodoForm {
        bool open = false;
        Widget* owner = nullptr;
        std::wstring text;
        size_t caret = 0;
        bool caretOn = true;
        int pri = 0;
        int y = 0, m = 0, d = 1;
        int dy = 0, dm = 0, dd = 1;
        int picking = 0; // 0 closed, 1 the start date, 2 the due date
        int viewY = 0, viewM = 0;
    } todo_;
    std::unique_ptr<Surface> todoSurface_;
    struct HandoutForm {
        bool open = false;
        Widget* owner = nullptr;
        int lesson = -1;
        int change = -1;
        std::wstring course, teacher, when, text;
        size_t caret = 0;
        bool caretOn = true;
    } handout_;
    std::unique_ptr<Surface> handoutSurface_;
    struct StyleForm {
        bool open = false;
        Widget* owner = nullptr;
        bool calendar = false;
        uint32_t pick = 0, ink = 0, bg = 0;
        float datePx = 0, tablePx = 0;
        std::wstring image;
    } style_;
    std::unique_ptr<Surface> styleSurface_;
    struct ToolForm {
        bool open = false;
        Widget* owner = nullptr;
        int kind = 0; // 0 countdown, 1 stopwatch, 2 draw a number, 3 split into groups, 4 the multidimensional table
        int minutes = 5;
        int remainMs = 5 * 60 * 1000;
        bool running = false;
        ULONGLONG mark = 0;
        ULONGLONG swBase = 0;
        bool swRun = false;
        int lo = 1, hi = 40, rolled = 0;
        bool hasRoll = false;
        int people = 40, groups = 4;
        std::wstring result;
    } tool_;
    std::unique_ptr<Surface> toolSurface_;
};

} // namespace yunzhao
