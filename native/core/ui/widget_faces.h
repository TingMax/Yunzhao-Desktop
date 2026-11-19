// What each desktop widget draws, after macOS's widgets: clear glass with its
// ink and a few of macOS's colours. desktop_widgets owns the windows, the grid,
// the input and the saved layout.
#pragma once

#include "shell/bluetooth_batteries.h"
#include "shell/lunar.h"
#include "shell/system_status.h"
#include "shell/weather.h"
#include "shell/task_model.h"
#include "ui/surface.h"

#include <memory>
#include <string>
#include <vector>

namespace atrium {

enum class WidgetKind { Clock, Calendar, Battery, Digital, System, Notes, Reminders, Photos, Weather, Bluetooth, Schedule, Marks, Focus, Habits };
enum class WidgetSize { Small, Medium, Large };
// Glass: the bar's clear glass, its ink adapting to what shows through. Dusk: the same glass
// dimmed by a dark veil, everything on it white (macOS's dimmed desktop widgets). Color: each
// widget's own opaque face, white or (weather) its sky, as macOS's full-colour widgets.
enum class WidgetMaterial { Glass, Dusk, Color };

// macOS's grid: a small widget is a square, a medium one two squares wide, a
// large one two by two (DIP).
constexpr float kWidgetCell = 164, kWidgetGap = 16, kWidgetRadius = 22;
inline int WidgetCols(WidgetSize s) { return s == WidgetSize::Small ? 1 : 2; }
inline int WidgetRows(WidgetSize s) { return s == WidgetSize::Large ? 2 : 1; }
inline float WidgetWidth(WidgetSize s) { return float(WidgetCols(s)) * kWidgetCell + float(WidgetCols(s) - 1) * kWidgetGap; }
inline float WidgetHeight(WidgetSize s) { return float(WidgetRows(s)) * kWidgetCell + float(WidgetRows(s) - 1) * kWidgetGap; }
bool WidgetSupports(WidgetKind kind, WidgetSize size);

// The cities a world clock can show, each with its Windows time zone.
struct WorldCity {
    const wchar_t* name;
    const wchar_t* zone;
};
const std::vector<WorldCity>& WorldCities();
// The world clocks of a widget (the medium Clock shows four, the medium Digital three): its own
// choice where it made one, else the defaults.
bool HasWorldClocks(WidgetKind kind, WidgetSize size);
std::vector<std::wstring> WidgetCities(WidgetKind kind, const std::vector<std::wstring>& chosen);

struct Reminder {
    std::wstring text;
    bool done = false;
    int year = 0;  // a calendar todo's date; 0 means the reminders widget (no date)
    int month = 0;
    int day = 0;
    int pri = 0; // calendar: 0 neither, 1 urgent, 2 important, 3 important and urgent
    int dueYear = 0, dueMonth = 0, dueDay = 0; // last day it shows; 0 means the same as year/month/day
};

// A widget's own content and its editing state.
struct WidgetContent {
    std::wstring text;           // notes
    std::vector<Reminder> items; // reminders
    std::wstring folder;         // photos: the folder (empty: Pictures)
    std::vector<std::wstring> cities; // world clocks: their cities (WorldCities' names; empty: the defaults)
    std::wstring photo;          // the photo shown
    std::shared_ptr<const IconImage> image; // it, cropped and scaled to the widget
    std::wstring imageKey;       // the canvas's cache key of `image`
    bool editing = false;
    int item = -1;     // reminders: the item being edited
    size_t caret = 0;  // in the text being edited
    bool caretOn = true; // blinking
    float scroll = 0;   // reminders: viewport offset, not saved
    int shownYear = 0;  // calendar: the month on screen; 0 with shownMonth follows today
    int shownMonth = 0;
    int focusDay = 0;   // day within that month; 0 with the month means today
    int calView = 2;    // 0 day, 1 week, 2 month
    bool calSplit = false; // kept in step with calBoard == 1
    int calBoard = 0;      // 0 todos on the date, 1 calendar above and todos below, 2 the week's timetable
    bool calLock = false;  // locked: a click locks, a 3s hold on the lock unlocks
    float listPane = -1;   // list layout: fraction of the height given to the todos; -1 is the default
    float tablePane = -1;  // timetable: fraction of the height given to the todos; -1 is the default
    bool tableEdit = false; // timetable courses change only while this is on; not saved
    bool toolsOpen = false; // the timetable tool menu is open; not saved
    bool layerOpen = false; // the window-layer menu is open; not saved
    int windowLayer = -1;   // -1 unknown, 0 bottom, 1 normal, 2 top; shared with the calendar page
    bool pullHot = false;   // the pointer is on the list divider; not saved
    float lockPress = 0;    // 0..1 while the lock is held; not saved
    uint32_t pickColor = 0; // selected day; 0 keeps the blue accent
    uint32_t fontColor = 0; // calendar ink; 0 keeps the glass ink
    uint32_t bgColor = 0;   // widget face; 0 keeps the glass
    float ownDate = 0;      // calendar date type; 0 uses the settings size
    float ownTable = 0;     // timetable header type; 0 uses the settings size
    std::wstring bgImage;   // a picture behind this widget
    std::shared_ptr<const IconImage> backdrop;
    std::wstring backdropKey;
    float calT = 0.45f;    // 0 shows only today, 1 fills the screen; the slider thumb, not saved
    float editX = 0, editY = 0; // where the todo field opens, DIP, not saved
    int popupYear = 0, popupMonth = 0, popupDay = 0; // a click opened this day's todos; 0 closed
    int popupIndex = -1; // the one todo a single click opened; ignored while popupAll
    bool popupAll = false; // a double click: every todo of that day
    bool popupPin = false; // timetable: stay until a click outside, not a mouse leave
    float popupX = 0, popupY = 0;
    bool editLesson = false; // the caret is in a timetable course, not a todo
    int editPart = 0;        // 0 the course name, 1 the teacher
    int editPeriod = -1;     // which period's clock is open; -1 closed
    // Eight period clocks the user sets (minutes from midnight). -1 is still unset.
    int periodStart[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int periodEnd[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    // 老师课表: weekday is 0 Monday .. 6 Sunday. startMin and endMin are minutes from midnight.
    struct Lesson {
        int weekday = 0; // 0 Monday .. 6 Sunday
        int period = 0;  // 0..4 morning periods 1..5, 5..7 afternoon periods 1..3
        int startMin = 8 * 60;
        int endMin = 8 * 60 + 45;
        std::wstring name;
        std::wstring teacher;
        std::wstring notes; // 讲义 for this weekly course
    };
    // One week only. year/month/day is the Monday of that week. off cancels the period that week.
    struct LessonChange {
        int year = 0, month = 0, day = 0;
        int weekday = 0;
        int period = 0;
        bool off = false;
        std::wstring name;
        std::wstring teacher;
        std::wstring notes;
    };
    struct Mark {
        std::wstring name;
        int year = 0, month = 0, day = 0;
        bool annual = true; // repeats every year
    };
    struct Habit {
        std::wstring name;
        int year = 0, month = 0, day = 0; // the last day it was checked
        int streak = 0;
    };
    std::vector<Lesson> lessons;
    std::vector<LessonChange> changes;
    int editScope = 0; // 0 the weekly course, 1 this week only
    int editChange = -1;
    int editWeekY = 0, editWeekM = 0, editWeekD = 0; // Monday of the week being edited
    int editWd = 0, editSlot = 0;
    std::wstring snapName, snapTeacher; // the weekly course when this edit began
    bool snapNew = false;              // this edit created the weekly course
    std::vector<Mark> marks;
    std::vector<Habit> habits;
    int pomoMin = 25;          // the length a new tomato uses
    bool pomoRun = false;      // a fullscreen countdown is up; not saved
    int pomoLeft = 0;          // seconds left while it runs; not saved
};

inline int FindLessonChange(const WidgetContent& content, int y, int m, int d, int weekday, int period)
{
    for (int i = 0; i < int(content.changes.size()); ++i) {
        const WidgetContent::LessonChange& change = content.changes[size_t(i)];
        if (change.year == y && change.month == m && change.day == d && change.weekday == weekday && change.period == period)
            return i;
    }
    return -1;
}

// The date the calendar is showing. A zero shown month means today.
inline void CalendarFocus(const WidgetContent& content, const SYSTEMTIME& now, int& y, int& m, int& d)
{
    if (content.shownYear > 0 && content.shownMonth >= 1 && content.shownMonth <= 12) {
        y = content.shownYear;
        m = content.shownMonth;
        d = content.focusDay >= 1 ? content.focusDay : 1;
        int dim = DaysIn(y, m);
        if (d > dim)
            d = dim;
    } else {
        y = int(now.wYear);
        m = int(now.wMonth);
        d = int(now.wDay);
    }
}

// What every widget may show.
struct WidgetWorld {
    SYSTEMTIME now{};
    std::wstring city; // the local time zone's first city
    const ChineseCalendar* lunar = nullptr;
    SystemStatus status;
    float cpu = 0, memory = 0, disk = 0; // in use, 0..1
    double memoryGb = 0, memoryTotalGb = 0, diskFreeGb = 0;
    Weather weather; // the forecast (weather.ok false: none yet)
    std::vector<BluetoothBattery> bluetooth; // the connected devices' batteries
    bool bluetoothRead = false;              // false until the first reading
    float dateSize = 20;   // week and month dates, set in the settings app
    float tableSize = 24;  // timetable header dates, set in the settings app
    float badgeScale = 0.7f; // timetable date badge, relative to the original circle
};

// Places in a widget that respond to the pointer (content DIP): "toggle:<i>"
// and "item:<i>" (a reminder's circle and text), "add" (a new reminder),
// "text" (the note). "caret" is where the caret is (the input method's window).
struct WidgetHit {
    D2D1_RECT_F rect;
    std::wstring id;
};

constexpr float kReminderTop = 42, kReminderRow = 27, kReminderBottom = 14;
float ReminderScrollMax(const WidgetContent& content, WidgetSize size);

// Paints the content over the widget's glass (the face and the ink of `material` included).
// `hoverDay` is "day:YYYY-MM-DD" while the pointer is on a calendar day that has todos.
// `layoutW` and `layoutH`, when set, are the window's size in DIP. The calendar lays its month
// out in that size; every other widget keeps the layout of `size`.
void PaintWidget(Canvas& c, WidgetKind kind, WidgetSize size, WidgetMaterial material, const WidgetContent& content, const WidgetWorld& world,
                 std::vector<WidgetHit>& hits, const std::wstring& hoverDay = {}, float layoutW = 0, float layoutH = 0);

} // namespace atrium
