#include "ui/widget_faces.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace yunzhao {

namespace {

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// What the weather's symbols are drawn over: light ink (clear glass, a sky), dark ink (bright
// glass: grey clouds, a gold moon), or the dimmed glass, where everything is white.
enum class Ink { Light, Dark, Mono };

// The colours of a widget: its ink, and macOS's accents.
struct Pal {
    D2D1_COLOR_F text, sub, faint, face, red, orange, green, track, blue; // blue: the calendar's accent
    D2D1_COLOR_F onAccent;  // ink on an accent's disc (today, a done reminder)
    D2D1_COLOR_F nightFace; // the world clock's faces where it is night
    Ink icon;
};

Pal GlassPal(const Theme& t)
{
    return {t.text, t.subtext, t.faint, Alpha(t.text, 0.10f), Rgba(0xFF453A, 0.98f), Rgba(0xFF9F0A, 0.98f), Rgba(0x30D158, 0.98f),
            Alpha(t.text, 0.18f), Rgba(0x0A84FF, 0.98f), Rgba(0xFFFFFF, 0.98f), Rgba(0x000000, 0.42f), t.light ? Ink::Dark : Ink::Light};
}

// Dusk: white only, the accents too (macOS's dimmed widgets are monochrome); a disc's ink is dark.
Pal DuskPal()
{
    const D2D1_COLOR_F white = Rgba(0xFFFFFF, 0.96f);
    return {white, Rgba(0xFFFFFF, 0.72f), Rgba(0xFFFFFF, 0.50f), Rgba(0xFFFFFF, 0.12f), white, white, white, Rgba(0xFFFFFF, 0.22f), white,
            Rgba(0x1C1C1E, 0.92f), Rgba(0x000000, 0.32f), Ink::Mono};
}

// Color, on a white face: dark ink and macOS's light-mode accents.
Pal PaperPal()
{
    const D2D1_COLOR_F ink = Rgba(0x1D1D1F, 0.94f);
    return {ink, Rgba(0x1D1D1F, 0.58f), Rgba(0x1D1D1F, 0.36f), Rgba(0x1D1D1F, 0.05f), Rgba(0xFF3B30, 1), Rgba(0xFF9500, 1), Rgba(0x34C759, 1),
            Rgba(0x1D1D1F, 0.10f), Rgba(0x007AFF, 1), Rgba(0xFFFFFF, 0.98f), Rgba(0x1C1C1E, 1), Ink::Dark};
}

const wchar_t* const kWeekLong[] = {L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日"};
const wchar_t* const kWeekShort[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
const wchar_t* const kMonths[] = {L"一月", L"二月", L"三月", L"四月", L"五月", L"六月",
                                  L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"};

bool ZoneInfo(const wchar_t* key, DYNAMIC_TIME_ZONE_INFORMATION& out)
{
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> zones; // read once
    if (zones.empty()) {
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; ++i)
            zones[d.TimeZoneKeyName] = d;
    }
    auto it = zones.find(key);
    if (it == zones.end())
        return false;
    out = it->second;
    return true;
}

int64_t Minutes(const SYSTEMTIME& t)
{
    FILETIME f{};
    SystemTimeToFileTime(&t, &f);
    return int64_t((ULARGE_INTEGER{f.dwLowDateTime, f.dwHighDateTime}.QuadPart) / 600000000ULL);
}

// The time in a city, and how far it is from here: "今天 +1小时" (half hours as "+5.5小时").
SYSTEMTIME CityTime(const std::wstring& name, const SYSTEMTIME& now, std::wstring& when)
{
    SYSTEMTIME utc, ct = now;
    GetSystemTime(&utc);
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    for (const WorldCity& city : WorldCities())
        if (name == city.name && ZoneInfo(city.zone, z))
            SystemTimeToTzSpecificLocalTimeEx(&z, &utc, &ct);
    const int64_t minutes = Minutes(ct) - Minutes(now);
    // Minutes() floors the seconds: a quarter-hour zone may come out a minute short.
    const int64_t quarters = (minutes + (minutes >= 0 ? 7 : -7)) / 15;
    int dayDiff = int((Minutes(ct) / 1440) - (Minutes(now) / 1440));
    when = dayDiff > 0 ? L"明天" : dayDiff < 0 ? L"昨天" : L"今天";
    std::wstring hours = std::to_wstring(quarters / 4);
    if (quarters % 4) {
        wchar_t b[16];
        swprintf_s(b, L"%g", double(quarters) / 4);
        hours = b;
    }
    when += (quarters >= 0 ? L" +" : L" ") + hours + L"小时";
    return ct;
}

} // namespace

const std::vector<WorldCity>& WorldCities()
{
    static const std::vector<WorldCity> cities = {
        {L"北京", L"China Standard Time"},          {L"上海", L"China Standard Time"},        {L"香港", L"China Standard Time"},
        {L"台北", L"Taipei Standard Time"},         {L"东京", L"Tokyo Standard Time"},        {L"首尔", L"Korea Standard Time"},
        {L"新加坡", L"Singapore Standard Time"},    {L"曼谷", L"SE Asia Standard Time"},      {L"新德里", L"India Standard Time"},
        {L"迪拜", L"Arabian Standard Time"},        {L"莫斯科", L"Russian Standard Time"},    {L"伊斯坦布尔", L"Turkey Standard Time"},
        {L"开罗", L"Egypt Standard Time"},          {L"约翰内斯堡", L"South Africa Standard Time"}, {L"柏林", L"W. Europe Standard Time"},
        {L"巴黎", L"Romance Standard Time"},        {L"伦敦", L"GMT Standard Time"},          {L"圣保罗", L"E. South America Standard Time"},
        {L"纽约", L"Eastern Standard Time"},        {L"多伦多", L"Eastern Standard Time"},    {L"芝加哥", L"Central Standard Time"},
        {L"丹佛", L"Mountain Standard Time"},       {L"洛杉矶", L"Pacific Standard Time"},    {L"温哥华", L"Pacific Standard Time"},
        {L"安克雷奇", L"Alaskan Standard Time"},    {L"檀香山", L"Hawaiian Standard Time"},   {L"悉尼", L"AUS Eastern Standard Time"},
        {L"墨尔本", L"AUS Eastern Standard Time"},  {L"奥克兰", L"New Zealand Standard Time"},
    };
    return cities;
}

bool HasWorldClocks(WidgetKind kind, WidgetSize size) { return size != WidgetSize::Small && (kind == WidgetKind::Clock || kind == WidgetKind::Digital); }

std::vector<std::wstring> WidgetCities(WidgetKind kind, const std::vector<std::wstring>& chosen)
{
    // macOS's medium Clock shows four cities; the digital one lists three beside the local time.
    std::vector<std::wstring> cities = kind == WidgetKind::Digital ? std::vector<std::wstring>{L"东京", L"伦敦", L"纽约"}
                                                                  : std::vector<std::wstring>{L"北京", L"东京", L"伦敦", L"纽约"};
    for (size_t i = 0; i < cities.size() && i < chosen.size(); ++i)
        if (std::any_of(WorldCities().begin(), WorldCities().end(), [&](const WorldCity& c) { return chosen[i] == c.name; }))
            cities[i] = chosen[i];
    return cities;
}

namespace {

std::wstring HourMinute(const SYSTEMTIME& t)
{
    wchar_t b[16];
    swprintf_s(b, L"%d:%02d", t.wHour, t.wMinute);
    return b;
}

int FirstWeekday()
{
    DWORD fd = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&fd), 2))
        return int(fd % 7);
    return 0;
}

// An analogue face: hands as on macOS (a thin neck, then the blade), the
// second hand in orange over the others. numbers: 1-12 inside the rim, else ticks.
void ClockFace(Canvas& c, float cx, float cy, float R, const SYSTEMTIME& t, const Pal& p, D2D1_COLOR_F face, D2D1_COLOR_F ink, bool numbers,
               const std::wstring& label, D2D1_COLOR_F labelInk)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    c.Circle(cx, cy, R, face);
    for (int h = 1; h <= 12; ++h) {
        float a = (float(h) * 30.f - 90.f) * kDeg;
        if (numbers) {
            float r = R * 0.78f, fs = R * 0.21f;
            std::wstring n = std::to_wstring(h);
            c.Text(n, D2D1::RectF(cx + r * std::cos(a) - fs, cy + r * std::sin(a) - fs * 0.72f, cx + r * std::cos(a) + fs, cy + r * std::sin(a) + fs * 0.72f), fs,
                   ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        } else {
            float r0 = R * 0.80f, r1 = R * 0.92f;
            c.Capsule(cx + r0 * std::cos(a), cy + r0 * std::sin(a), cx + r1 * std::cos(a), cy + r1 * std::sin(a), Alpha(ink, h % 3 ? 0.45f : 0.9f),
                      std::max(1.f, R * 0.045f));
        }
    }
    if (!label.empty())
        c.Text(label, D2D1::RectF(cx - R, cy + R * 0.26f, cx + R, cy + R * 0.26f + R * 0.2f), R * 0.15f, labelInk, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    float sec = float(t.wSecond), min = float(t.wMinute) + sec / 60.f, hour = float(t.wHour % 12) + min / 60.f;
    auto hand = [&](float angle, float length, float width) {
        float a = (angle - 90.f) * kDeg, neck = R * 0.16f;
        float dx = std::cos(a), dy = std::sin(a);
        c.Capsule(cx, cy, cx + dx * neck, cy + dy * neck, ink, std::max(1.2f, width * 0.36f));
        c.Capsule(cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width);
    };
    hand(hour * 30.f, R * 0.52f, std::max(2.5f, R * 0.075f));
    hand(min * 6.f, R * 0.80f, std::max(2.5f, R * 0.075f));
    c.Circle(cx, cy, std::max(2.2f, R * 0.055f), ink);
    float a = (sec * 6.f - 90.f) * kDeg;
    c.Capsule(cx - std::cos(a) * R * 0.18f, cy - std::sin(a) * R * 0.18f, cx + std::cos(a) * R * 0.86f, cy + std::sin(a) * R * 0.86f, p.orange,
              std::max(1.f, R * 0.02f));
    c.Circle(cx, cy, std::max(1.8f, R * 0.04f), p.orange);
    c.Circle(cx, cy, std::max(0.8f, R * 0.016f), face);
}

constexpr float kPad = 14;

void PaintClock(Canvas& c, WidgetSize size, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H)
{
    if (size == WidgetSize::Small) {
        ClockFace(c, W / 2, H / 2, H / 2 - 9, w.now, p, p.face, p.text, true, w.city, p.sub);
        return;
    }
    // The world clock: four faces, dark where it is night.
    const float colW = (W - 2 * kPad) / 4, R = 31;
    const std::vector<std::wstring> cities = WidgetCities(WidgetKind::Clock, content.cities);
    for (int i = 0; i < 4; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(cities[size_t(i)], w.now, when);
        bool night = ct.wHour < 6 || ct.wHour >= 18;
        float cx = kPad + colW * (float(i) + 0.5f), cy = (H - 2 * R - 42) / 2 + R - 2;
        D2D1_COLOR_F face = night ? p.nightFace : p.face, ink = night ? Rgba(0xFFFFFF, 0.96f) : p.text;
        ClockFace(c, cx, cy, R, ct, p, face, ink, false, {}, ink);
        c.Text(cities[size_t(i)], D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 12.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(when, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Digits instead of hands: the time large, the day under it; the medium one
// lists three more cities on the right.
void PaintDigital(Canvas& c, WidgetSize size, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const int wd = Weekday(w.now.wYear, w.now.wMonth, w.now.wDay);
    const float leftW = size == WidgetSize::Small ? W - 2 * kPad : 150;
    c.Text(w.city.empty() ? L"本地" : w.city, D2D1::RectF(kPad + 2, kPad, kPad + leftW, kPad + 18), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    // Keep one size all day. Leave one DIP for glyph placement / rounding:
    // tabular figures can still differ slightly at the exact trimming boundary.
    const float fit = c.TextWidth(L"00:00", 50, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float timeW = leftW - 1;
    const float timeSize = fit > timeW ? 50 * timeW / fit : 50.f;
    c.Text(HourMinute(w.now), D2D1::RectF(kPad, kPad + 22, kPad + leftW, kPad + 84), timeSize, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(kWeekLong[wd], D2D1::RectF(kPad + 2, H - kPad - 40, kPad + leftW, H - kPad - 20), 13, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(w.now.wMonth) + L"月" + std::to_wstring(w.now.wDay) + L"日", D2D1::RectF(kPad + 2, H - kPad - 20, kPad + leftW, H - kPad), 13,
           p.text, DWRITE_FONT_WEIGHT_NORMAL);
    if (size == WidgetSize::Small)
        return;
    const float x0 = kPad + leftW + 8, rowH = (H - 2 * kPad) / 3;
    const std::vector<std::wstring> cities = WidgetCities(WidgetKind::Digital, content.cities);
    for (int i = 0; i < 3; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(cities[size_t(i)], w.now, when);
        float y = kPad + rowH * float(i);
        if (i > 0)
            c.Line(x0, y, W - kPad, y, Alpha(p.text, 0.12f));
        c.Text(cities[size_t(i)], D2D1::RectF(x0, y + 4, W - kPad - 70, y + rowH / 2 + 2), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(when, D2D1::RectF(x0, y + rowH / 2, W - kPad - 70, y + rowH - 4), 10.5f, p.sub);
        c.Text(HourMinute(ct), D2D1::RectF(W - kPad - 80, y, W - kPad, y + rowH), 22, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

std::wstring DayId(int y, int m, int d)
{
    wchar_t buf[24];
    swprintf_s(buf, L"day:%04d-%02d-%02d", y, m, d);
    return buf;
}

bool ParseDayId(const std::wstring& id, int& y, int& m, int& d)
{
    return id.rfind(L"day:", 0) == 0 && swscanf_s(id.c_str() + 4, L"%d-%d-%d", &y, &m, &d) == 3;
}

int DayKey(int y, int m, int d) { return y * 10000 + m * 100 + d; }

// A todo shows on every day from the day it was added through its due date.
bool CoversDay(const Reminder& item, int y, int m, int d)
{
    if (item.year == 0 || item.text.empty())
        return false;
    int start = DayKey(item.year, item.month, item.day);
    int due = DayKey(item.dueYear ? item.dueYear : item.year, item.dueMonth ? item.dueMonth : item.month, item.dueDay ? item.dueDay : item.day);
    if (start > due)
        std::swap(start, due);
    int cur = DayKey(y, m, d);
    return cur >= start && cur <= due;
}

// Todos saved on this calendar for one day (an empty line being typed does not count).
int TodosOn(const WidgetContent& content, int y, int m, int d, bool openOnly)
{
    int n = 0;
    for (const Reminder& item : content.items)
        if (CoversDay(item, y, m, d) && (!openOnly || !item.done))
            ++n;
    return n;
}

// 0 neither (white), 1 urgent (blue), 2 important (yellow), 3 both (red). Done is always WeChat green.
D2D1_COLOR_F TodoMarkColor(const Reminder& item)
{
    if (item.done)
        return Rgba(0x07C160, 0.98f);
    switch (item.pri) {
    case 3:
        return Rgba(0xFF3B30, 0.98f);
    case 2:
        return Rgba(0xFFD60A, 0.98f);
    case 1:
        return Rgba(0x0A84FF, 0.98f);
    default:
        return Rgba(0xFFFFFF, 0.96f);
    }
}

D2D1_COLOR_F TodoTextColor(bool done) { return done ? Rgba(0xAEAEB2, 0.95f) : Rgba(0xFFFFFF, 0.96f); }

// A hollow ring in the priority colour. Done draws a WeChat-green ring and check.
void PaintMark(Canvas& c, float cx, float cy, float radius, const Reminder& item)
{
    const D2D1_COLOR_F ink = TodoMarkColor(item);
    c.Arc(cx, cy, radius, 0.f, 359.f, ink, std::max(1.15f, radius * 0.28f));
    if (!item.done)
        return;
    const float x0 = cx - radius * 0.46f, y0 = cy + radius * 0.02f;
    const float x1 = cx - radius * 0.08f, y1 = cy + radius * 0.40f;
    const float x2 = cx + radius * 0.52f, y2 = cy - radius * 0.42f;
    const float stroke = std::max(1.2f, radius * 0.28f);
    c.Line(x0, y0, x1, y1, ink, stroke);
    c.Line(x1, y1, x2, y2, ink, stroke);
}

void PaintTodoText(Canvas& c, const std::wstring& text, D2D1_RECT_F r, float size, bool done, bool wrap)
{
    const D2D1_COLOR_F ink = TodoTextColor(done);
    c.Text(text, r, size, ink, DWRITE_FONT_WEIGHT_NORMAL, 0, wrap);
    if (!done)
        return;
    if (!wrap) {
        float tw = std::min(c.TextWidth(text, size), std::max(0.f, r.right - r.left));
        float y = (r.top + r.bottom) * 0.5f;
        c.Line(r.left, y, r.left + tw, y, ink, 1.15f);
        return;
    }
    const float width = std::max(1.f, r.right - r.left);
    const float tw = c.TextWidth(text, size);
    const int lines = std::max(1, int(std::ceil(tw / width)));
    const float lineH = size * 1.35f;
    for (int i = 0; i < lines; ++i) {
        float y = r.top + lineH * (float(i) + 0.5f);
        if (y > r.bottom)
            break;
        float seg = i + 1 == lines ? std::max(8.f, tw - width * float(i)) : width;
        c.Line(r.left, y, r.left + std::min(seg, width), y, ink, 1.15f);
    }
}

// The day being typed, opening where the day was clicked, with a priority to pick.
void PaintTodoEditor(Canvas& c, const WidgetContent& content, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    if (content.editLesson || !content.editing || content.item < 0 || content.item >= int(content.items.size()))
        return;
    const Reminder& item = content.items[size_t(content.item)];
    struct Choice {
        int pri;
        const wchar_t* label;
    };
    const Choice choices[] = {{3, L"重要且紧急"}, {2, L"重要不紧急"}, {1, L"紧急不重要"}, {0, L"不紧急不重要"}};
    const float cardW = std::min(320.f, std::max(180.f, W - 16.f));
    const float cardH = 156.f;
    float x = content.editX > 0 || content.editY > 0 ? content.editX : (W - cardW) / 2;
    float y = content.editX > 0 || content.editY > 0 ? content.editY : (H - cardH) / 2;
    x = std::clamp(x, 8.f, std::max(8.f, W - cardW - 8.f));
    y = std::clamp(y, 8.f, std::max(8.f, H - cardH - 8.f));
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + cardW, y + cardH);
    const int pri = item.pri < 0 || item.pri > 3 ? 0 : item.pri;
    Reminder shown = item;
    shown.pri = pri;
    c.Fill(card, 12, Rgba(0x1C1C1E, 0.94f));
    c.Stroke(card, 12, Alpha(TodoMarkColor(shown), 0.9f), 1.25f);
    hits.push_back({card, L"field"});
    const D2D1_RECT_F field = D2D1::RectF(card.left + 12, card.top + 8, card.right - 12, card.top + 36);
    if (item.text.empty())
        c.Text(L"输入待办，回车保存", field, 13.5f, Rgba(0xFFFFFF, 0.40f));
    c.Text(item.text, field, 13.5f, Rgba(0xFFFFFF, 0.96f));
    float caretX = field.left + c.TextWidth(item.text.substr(0, std::min(content.caret, item.text.size())), 13.5f);
    D2D1_RECT_F caret = D2D1::RectF(std::min(caretX, field.right - 2), field.top + 2, std::min(caretX, field.right - 2) + 1.5f, field.bottom - 2);
    if (content.caretOn)
        c.Fill(caret, 0.75f, TodoMarkColor(shown));
    hits.push_back({caret, L"caret"});
    int dueY = item.dueYear ? item.dueYear : item.year;
    int dueM = item.dueMonth ? item.dueMonth : item.month;
    int dueD = item.dueDay ? item.dueDay : item.day;
    const D2D1_RECT_F dueRow = D2D1::RectF(card.left + 8, card.top + 40, card.right - 8, card.top + 68);
    c.Text(L"截止", D2D1::RectF(dueRow.left, dueRow.top, dueRow.left + 36, dueRow.bottom), 12.5f, Rgba(0xFFFFFF, 0.72f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 0);
    const D2D1_RECT_F duePrev = D2D1::RectF(dueRow.left + 40, dueRow.top + 2, dueRow.left + 64, dueRow.bottom - 2);
    const D2D1_RECT_F dueNext = D2D1::RectF(dueRow.right - 24, dueRow.top + 2, dueRow.right, dueRow.bottom - 2);
    c.Glyph(L"\uE76B", duePrev, 12, Rgba(0xFFFFFF, 0.9f));
    c.Glyph(L"\uE76C", dueNext, 12, Rgba(0xFFFFFF, 0.9f));
    std::wstring due = std::to_wstring(dueM) + L"月" + std::to_wstring(dueD) + L"日";
    if (dueY != item.year)
        due = std::to_wstring(dueY) + L"年" + due;
    c.Text(due, D2D1::RectF(duePrev.right + 4, dueRow.top, dueNext.left - 4, dueRow.bottom), 13.f, Rgba(0xFFFFFF, 0.96f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    hits.push_back({duePrev, L"due:-1"});
    hits.push_back({dueNext, L"due:1"});
    const float colW = (cardW - 20.f) / 2.f;
    const float rowH = 30.f;
    const float gridTop = card.top + 74.f;
    for (int i = 0; i < 4; ++i) {
        const float lx = card.left + 8.f + float(i % 2) * (colW + 4.f);
        const float ly = gridTop + float(i / 2) * (rowH + 4.f);
        D2D1_RECT_F chip = D2D1::RectF(lx, ly, lx + colW, ly + rowH);
        Reminder sample;
        sample.pri = choices[i].pri;
        const D2D1_COLOR_F color = TodoMarkColor(sample);
        const bool on = pri == choices[i].pri;
        if (on)
            c.Fill(chip, 8, Alpha(color, 0.22f));
        c.Stroke(chip, 8, on ? color : Rgba(0xFFFFFF, 0.16f), on ? 1.4f : 1.f);
        PaintMark(c, chip.left + 14, (chip.top + chip.bottom) / 2, 5.f, sample);
        c.Text(choices[i].label, D2D1::RectF(chip.left + 24, chip.top, chip.right - 4, chip.bottom), 11.5f, Rgba(0xFFFFFF, on ? 0.96f : 0.72f),
               DWRITE_FONT_WEIGHT_NORMAL, 0);
        wchar_t id[16];
        swprintf_s(id, L"pri:%d", choices[i].pri);
        hits.push_back({chip, id});
    }
    (void)p;
}

void AddDays(int& y, int& m, int& d, int delta)
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

// Todo lines in a cell. Each line is a hit ("todo:<index>"); a dot or "+N" is
// "todos:YYYY-MM-DD". A click on either opens the day's popup.
void PaintTodoLines(Canvas& c, const WidgetContent& content, const Pal& p, std::vector<WidgetHit>& hits, int y, int m, int d, float x, float top,
                    float width, float bottom, float lineH, bool inMonth)
{
    (void)inMonth;
    struct Row {
        size_t index;
        const Reminder* item;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < content.items.size(); ++i) {
        const Reminder& item = content.items[i];
        if (CoversDay(item, y, m, d))
            rows.push_back({i, &item});
    }
    if (rows.empty())
        return;
    wchar_t dayHit[40];
    swprintf_s(dayHit, L"todos:%04d-%02d-%02d", y, m, d);
    const int slots = std::max(0, int((bottom - top) / std::max(1.f, lineH)));
    if (slots < 1) {
        PaintMark(c, x + width - 8, top + 4, 3.2f, *rows.front().item);
        hits.push_back({D2D1::RectF(x + width - 18, top - 2, x + width, top + 16), dayHit});
        return;
    }
    const int count = int(rows.size());
    const int extra = count > slots ? 1 : 0;
    const int show = std::min(count, slots - extra);
    for (int n = 0; n < show; ++n) {
        const float ly = top + float(n) * lineH;
        const Reminder& item = *rows[size_t(n)].item;
        const float radius = std::clamp(lineH * 0.28f, 3.2f, 5.f);
        PaintMark(c, x + 8, ly + lineH * 0.42f, radius, item);
        // A todo carrying its result is two lines high in the same slot: title, then the
        // result in a smaller, quieter ink. The slot height does not change, so the count
        // of visible todos stays predictable.
        if (!item.progress.empty()) {
            const float titleH = lineH * 0.56f;
            PaintTodoText(c, item.text, D2D1::RectF(x + 16, ly - 1.f, x + width - 3, ly + titleH), std::max(9.f, lineH * 0.5f),
                item.done, false);
            const D2D1_COLOR_F ink = item.done ? Alpha(Rgba(0x07C160, 0.98f), 0.82f) : Alpha(p.sub, 0.92f);
            c.Text(item.progress, D2D1::RectF(x + 16, ly + titleH - 2.f, x + width - 3, ly + lineH), std::max(8.f, lineH * 0.42f), ink);
        } else {
            PaintTodoText(c, item.text, D2D1::RectF(x + 16, ly, x + width - 3, ly + lineH), std::max(10.f, lineH - 2), item.done, false);
        }
        wchar_t id[32];
        swprintf_s(id, L"todo:%zu", rows[size_t(n)].index);
        hits.push_back({D2D1::RectF(x + 2, ly, x + width - 2, ly + lineH), id});
    }
    if (extra) {
        const float ly = top + float(show) * lineH;
        c.Text(L"+" + std::to_wstring(int(rows.size()) - show), D2D1::RectF(x + 5, ly, x + width - 3, ly + lineH), std::max(10.f, lineH - 2), p.sub,
               DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hits.push_back({D2D1::RectF(x + 2, ly, x + width - 2, ly + lineH), dayHit});
    }
}

// Month and week bars, laid out the way yunzhao_note2 draws a note across the
// days it covers. A bar that runs past this week is square on that side.
void PaintSpanBars(Canvas& c, const WidgetContent& content, std::vector<WidgetHit>& hits, int y0, int m0, int d0, float originX, float cellW,
                   float gap, float top, float bottom)
{
    if (bottom - top < 16.f)
        return;
    int daysY[7], daysM[7], daysD[7];
    for (int i = 0; i < 7; ++i) {
        daysY[i] = y0;
        daysM[i] = m0;
        daysD[i] = d0;
        AddDays(daysY[i], daysM[i], daysD[i], i);
    }
    const int weekStart = DayKey(daysY[0], daysM[0], daysD[0]);
    const int weekEnd = DayKey(daysY[6], daysM[6], daysD[6]);
    struct Seg {
        size_t index;
        int a, b, lane;
        bool before, after;
    };
    std::vector<Seg> segs;
    std::vector<int> laneEnd;
    for (size_t i = 0; i < content.items.size(); ++i) {
        const Reminder& item = content.items[i];
        if (item.year == 0 || item.text.empty())
            continue;
        int start = DayKey(item.year, item.month, item.day);
        int due = DayKey(item.dueYear ? item.dueYear : item.year, item.dueMonth ? item.dueMonth : item.month,
                         item.dueDay ? item.dueDay : item.day);
        if (start > due)
            std::swap(start, due);
        if (due < weekStart || start > weekEnd)
            continue;
        int a = 0, b = 6;
        for (int col = 0; col < 7; ++col)
            if (DayKey(daysY[col], daysM[col], daysD[col]) >= start) {
                a = col;
                break;
            }
        for (int col = 6; col >= 0; --col)
            if (DayKey(daysY[col], daysM[col], daysD[col]) <= due) {
                b = col;
                break;
            }
        int lane = 0;
        while (lane < int(laneEnd.size()) && laneEnd[size_t(lane)] >= a)
            ++lane;
        if (lane == int(laneEnd.size()))
            laneEnd.push_back(b);
        else
            laneEnd[size_t(lane)] = b;
        segs.push_back({i, a, b, lane, start < weekStart, due > weekEnd});
    }
    const float laneH = 20.f;
    const int maxLanes = std::max(1, int((bottom - top) / laneH));
    int overflow[7] = {};
    for (const Seg& seg : segs) {
        if (seg.lane >= maxLanes) {
            for (int col = seg.a; col <= seg.b; ++col)
                ++overflow[col];
            continue;
        }
        const Reminder& item = content.items[seg.index];
        const float left = originX + float(seg.a) * cellW + (seg.before ? 1.f : gap * 0.5f + 4.f);
        const float right = originX + float(seg.b + 1) * cellW - (seg.after ? 1.f : gap * 0.5f + 4.f);
        const float ly = top + float(seg.lane) * laneH;
        D2D1_RECT_F bar = D2D1::RectF(left, ly, std::max(left + 8.f, right), ly + laneH - 3.f);
        D2D1_COLOR_F fill = TodoMarkColor(item);
        if (!item.done && item.pri == 0)
            fill = Rgba(0xFFFFFF, 0.30f);
        if (item.done)
            fill.a *= 0.72f;
        c.Fill(bar, 5, fill);
        const bool yellow = item.pri == 2 && !item.done;
        const D2D1_COLOR_F ink = yellow ? Rgba(0x1C1C1E, 0.92f) : Rgba(0xFFFFFF, item.done ? 0.82f : 0.98f);
        c.Text(item.text, D2D1::RectF(bar.left + 6, bar.top, bar.right - 4, bar.bottom), 11.5f, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (item.done) {
            float tw = std::min(c.TextWidth(item.text, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), std::max(0.f, bar.right - bar.left - 10.f));
            float mid = (bar.top + bar.bottom) * 0.5f;
            c.Line(bar.left + 6, mid, bar.left + 6 + tw, mid, ink, 1.1f);
        }
        wchar_t id[32];
        swprintf_s(id, L"todo:%zu", seg.index);
        hits.push_back({bar, id});
    }
    for (int col = 0; col < 7; ++col) {
        if (!overflow[col])
            continue;
        D2D1_RECT_F badge = D2D1::RectF(originX + float(col + 1) * cellW - gap * 0.5f - 30.f, bottom - 16.f,
                                        originX + float(col + 1) * cellW - gap * 0.5f - 4.f, bottom - 2.f);
        c.Fill(badge, 4, Rgba(0x0A84FF, 0.95f));
        c.Text(L"+" + std::to_wstring(overflow[col]), badge, 10, Rgba(0xFFFFFF, 1.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        wchar_t id[40];
        swprintf_s(id, L"todos:%04d-%02d-%02d", daysY[col], daysM[col], daysD[col]);
        hits.push_back({badge, id});
    }
}

// One todo after a click, or every todo of that day after a double click.
void PaintTodoPopup(Canvas& c, const WidgetContent& content, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    if (content.popupDay < 1)
        return;
    std::vector<size_t> rows;
    for (size_t i = 0; i < content.items.size(); ++i) {
        const Reminder& item = content.items[i];
        if (!CoversDay(item, content.popupYear, content.popupMonth, content.popupDay))
            continue;
        if (!content.popupAll && int(i) != content.popupIndex)
            continue;
        rows.push_back(i);
    }
    if (rows.empty())
        return;
    const float cardW = std::min(300.f, std::max(180.f, W - 16.f));
    const float textW = cardW - 48;
    float body = 0;
    auto progressLines = [&](const Reminder& item) {
        if (item.progress.empty())
            return 0;
        const float tw = c.TextWidth(item.progress, 11.5f);
        return std::max(1, int(std::ceil(tw / std::max(1.f, textW))));
    };
    for (size_t index : rows) {
        const Reminder& item = content.items[index];
        float tw = c.TextWidth(item.text, 13.5f);
        int lines = std::max(1, int(std::ceil(tw / std::max(1.f, textW))));
        body += float(lines) * 18.f + 10.f;
        body += float(progressLines(item)) * 15.f + (progressLines(item) ? 4.f : 0.f);
    }
    const float foot = 8.f;
    const float cardH = std::min(H - 16.f, 40.f + body + foot);
    float x = std::clamp(content.popupX, 8.f, std::max(8.f, W - cardW - 8.f));
    float y = std::clamp(content.popupY, 8.f, std::max(8.f, H - cardH - 8.f));
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + cardW, y + cardH);
    c.Fill(card, 12, Rgba(0x1C1C1E, 0.96f));
    c.Stroke(card, 12, Alpha(p.blue, 0.65f), 1.25f);
    hits.push_back({card, L"popup"});
    std::wstring title = std::to_wstring(content.popupMonth) + L"月" + std::to_wstring(content.popupDay) + L"日";
    if (content.popupAll)
        title += L"  全部";
    c.Text(title, D2D1::RectF(card.left + 12, card.top + 8, card.right - 12, card.top + 28), 12.5f, Rgba(0xFFFFFF, 0.72f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float ly = card.top + 34;
    for (size_t index : rows) {
        const Reminder& item = content.items[index];
        float tw = c.TextWidth(item.text, 13.5f);
        int lines = std::max(1, int(std::ceil(tw / std::max(1.f, textW))));
        float lh = float(lines) * 18.f;
        const int extra = progressLines(item);
        const float progressH = extra ? float(extra) * 15.f + 4.f : 0.f;
        if (ly + 8 > card.bottom - 8.f)
            break;
        const float cy = ly + 9;
        PaintMark(c, card.left + 18, cy, 7.f, item);
        wchar_t check[32];
        swprintf_s(check, L"check:%zu", index);
        hits.push_back({D2D1::RectF(card.left + 6, ly, card.left + 34, ly + std::max(22.f, lh + progressH)), check});
        D2D1_RECT_F text = D2D1::RectF(card.left + 34, ly, card.right - 12, std::min(card.bottom - 8, ly + lh));
        PaintTodoText(c, item.text, text, 13.5f, item.done, true);
        if (extra) {
            const D2D1_COLOR_F ink = item.done ? Alpha(Rgba(0x07C160, 0.98f), 0.88f) : Alpha(p.sub, 0.95f);
            D2D1_RECT_F line = D2D1::RectF(card.left + 34, ly + lh + 2.f, card.right - 12, std::min(card.bottom - 8, ly + lh + progressH));
            c.Text(item.progress, line, 11.5f, ink, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        }
        wchar_t sel[32];
        swprintf_s(sel, L"sel:%zu", index);
        hits.push_back({text, sel});
        ly += lh + progressH + 10.f;
    }
}

D2D1_COLOR_F Picked(const WidgetContent& content, const Pal& p)
{
    return content.pickColor ? Rgba(content.pickColor & 0xFFFFFFu, 0.98f) : p.blue;
}

void PaintDayBoard(Canvas& c, const WidgetContent& content, const WidgetWorld& world, const Pal& p, float W, float H, std::vector<WidgetHit>& hits,
                   int y, int m, int d)
{
    const float pad = 14, top = pad + 44;
    const float fs = std::clamp(world.dateSize, 12.f, 36.f);
    const bool today = y == int(world.now.wYear) && m == int(world.now.wMonth) && d == int(world.now.wDay);
    D2D1_RECT_F badge = D2D1::RectF(pad, top + 4, pad + 72, top + 64);
    const D2D1_COLOR_F pick = Picked(content, p);
    c.Fill(badge, 16, today ? Alpha(Rgba(0xFF453A, 1.f), 0.18f) : Alpha(pick, 0.16f));
    c.Stroke(badge, 16, today ? Rgba(0xFF453A, 0.98f) : pick, today ? 2.2f : 1.4f);
    c.Text(std::to_wstring(d), badge, std::max(28.f, fs * 1.4f), p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(kWeekLong[Weekday(y, m, d)], D2D1::RectF(pad + 84, top + 8, W - pad, top + 32), std::max(16.f, fs * 0.8f), p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (world.lunar) {
        bool festival = false;
        std::wstring lunar = world.lunar->MonthDay(y, m, d);
        std::wstring label = world.lunar->DayLabel(y, m, d, &festival);
        std::wstring sub = lunar.empty() ? label : L"农历" + lunar;
        if (!label.empty() && label != lunar)
            sub += L"  " + label;
        c.Text(sub, D2D1::RectF(pad + 84, top + 34, W - pad, top + 56), std::max(13.f, fs * 0.65f), festival ? p.blue : p.sub);
    }
    const float rowH = 30, listTop = top + 78;
    int total = 0;
    for (const Reminder& item : content.items)
        if (CoversDay(item, y, m, d))
            ++total;
    const int room = std::max(1, int((H - pad - listTop) / rowH));
    const int extraRoom = total > room ? 1 : 0;
    const int show = std::max(0, std::min(total, room - extraRoom));
    int shown = 0;
    for (size_t i = 0; i < content.items.size() && shown < show; ++i) {
        const Reminder& item = content.items[i];
        if (!CoversDay(item, y, m, d))
            continue;
        float ly = listTop + float(shown) * rowH;
        D2D1_RECT_F row = D2D1::RectF(pad, ly, W - pad, ly + rowH - 4);
        D2D1_COLOR_F fill = TodoMarkColor(item);
        if (!item.done && item.pri == 0)
            fill = Rgba(0xFFFFFF, 0.28f);
        if (item.done)
            fill.a *= 0.72f;
        c.Fill(row, 6, fill);
        const bool yellow = item.pri == 2 && !item.done;
        const D2D1_COLOR_F ink = yellow ? Rgba(0x1C1C1E, 0.92f) : Rgba(0xFFFFFF, item.done ? 0.82f : 0.98f);
        c.Text(item.text, D2D1::RectF(row.left + 10, row.top, row.right - 8, row.bottom), 13.5f, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (item.done) {
            float tw = std::min(c.TextWidth(item.text, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), std::max(0.f, row.right - row.left - 18.f));
            c.Line(row.left + 10, (row.top + row.bottom) * 0.5f, row.left + 10 + tw, (row.top + row.bottom) * 0.5f, ink, 1.15f);
        }
        wchar_t id[32];
        swprintf_s(id, L"todo:%zu", i);
        hits.push_back({row, id});
        ++shown;
    }
    float ly = listTop + float(shown) * rowH;
    if (shown < total) {
        wchar_t more[40];
        swprintf_s(more, L"todos:%04d-%02d-%02d", y, m, d);
        c.Text(L"+" + std::to_wstring(total - shown), D2D1::RectF(pad + 8, ly, W - pad, ly + rowH), 13, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hits.push_back({D2D1::RectF(pad, ly, W - pad, ly + rowH), more});
        ly += rowH;
    }
}

void PaintWeekBoard(Canvas& c, const WidgetContent& content, const WidgetWorld& world, const Pal& p, float W, float H, std::vector<WidgetHit>& hits,
                    const std::wstring& hoverDay, int y, int m, int d, bool inlineTodos)
{
    const float pad = 14, headerH = 34, weekH = 22;
    const int first = FirstWeekday();
    int sy = y, sm = m, sd = d;
    AddDays(sy, sm, sd, -((Weekday(y, m, d) - first + 7) % 7));
    const float cellW = std::max(1.f, (W - 2 * pad) / 7.f);
    const float top = pad + headerH;
    const float gridTop = top + weekH;
    const float cellH = std::max(20.f, H - pad - gridTop);
    const float lineH = std::clamp(cellH * 0.12f, 13.f, 18.f);
    const D2D1_COLOR_F pick = Picked(content, p);
    const D2D1_COLOR_F ink = Rgba(0x1D1D1F, 0.92f);
    c.Fill(D2D1::RectF(8, top - 4, W - 8, std::max(top + 8.f, H - 8)), 16, D2D1::ColorF(0.965f, 0.965f, 0.975f, 0.97f));
    for (int i = 0; i < 7; ++i) {
        int cy = sy, cm = sm, cd = sd;
        AddDays(cy, cm, cd, i);
        float x0 = pad + float(i) * cellW;
        bool today = cy == int(world.now.wYear) && cm == int(world.now.wMonth) && cd == int(world.now.wDay);
        bool picked = cy == y && cm == m && cd == d;
        bool hot = hoverDay == DayId(cy, cm, cd);
        (void)hot;
        D2D1_RECT_F head = D2D1::RectF(x0, top, x0 + cellW, gridTop);
        const float fs = std::clamp(world.dateSize, 12.f, 36.f);
        c.Text(kWeekShort[(first + i) % 7], head, std::max(12.f, fs * 0.72f), Rgba(0x6E6E73, 1.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        const float gap = 4.f;
        D2D1_RECT_F cell = D2D1::RectF(x0 + gap * 0.5f, gridTop + gap * 0.5f, x0 + cellW - gap * 0.5f, gridTop + cellH - gap * 0.5f);
        c.Fill(cell, 9, D2D1::ColorF(1, 1, 1, 0.98f));
            if (today)
            c.Stroke(cell, 9, Rgba(0xFF453A, 0.98f), 1.6f);
        else
            c.Stroke(cell, 9, picked ? pick : Rgba(0x000000, 0.08f), picked ? 1.6f : 1.f);
        D2D1_RECT_F num = D2D1::RectF(cell.left + 6, cell.top + 4, cell.left + 6 + fs + 8, cell.top + 4 + fs + 6);
        c.Text(std::to_wstring(cd), num, fs, ink, today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (today)
            c.Text(L"今", D2D1::RectF(cell.right - 28, cell.top + 4, cell.right - 6, cell.top + 22), 11, p.blue, DWRITE_FONT_WEIGHT_BOLD, 2);
        hits.push_back({cell, DayId(cy, cm, cd)});
    }
    PaintSpanBars(c, content, hits, sy, sm, sd, pad, cellW, 4.f, gridTop + std::clamp(world.dateSize, 12.f, 36.f) + 16.f, gridTop + cellH - 6.f);
    (void)lineH;
    (void)inlineTodos;
}

// The date's todos under the calendar. The circle completes one; the last row adds one.
void PaintSplitList(Canvas& c, const WidgetContent& content, const Pal& p, float W, float H, float top, std::vector<WidgetHit>& hits, int y, int m,
                    int d)
{
    const float pad = 14;
    hits.push_back({D2D1::RectF(pad, top - 12, W - pad, top + 12), L"pull"});
    if (content.pullHot) {
        c.Line(pad, top, W - pad, top, Alpha(p.blue, 0.55f), 1.5f);
        c.Fill(D2D1::RectF(W / 2 - 26, top - 3, W / 2 + 26, top + 3), 3, p.blue);
    }
    std::wstring title = std::to_wstring(m) + L"月" + std::to_wstring(d) + L"日";
    c.Text(title, D2D1::RectF(pad, top + 8, W - pad - 72, top + 32), 14, p.blue, DWRITE_FONT_WEIGHT_BOLD, 0);
    const float rowH = 30;
    float ly = top + 34;
    int shown = 0;
    int total = TodosOn(content, y, m, d, false);
    const int room = std::max(1, int((H - pad - 28 - ly) / rowH));
    const int extra = total > room ? 1 : 0;
    const int show = std::min(total, room - extra);
    for (size_t i = 0; i < content.items.size() && shown < show; ++i) {
        const Reminder& item = content.items[i];
        if (!CoversDay(item, y, m, d))
            continue;
        D2D1_RECT_F row = D2D1::RectF(pad, ly, W - pad, ly + rowH - 4);
        c.Fill(row, 8, Alpha(p.text, 0.07f));
        PaintMark(c, row.left + 16, (row.top + row.bottom) / 2, 6.5f, item);
        wchar_t check[32];
        swprintf_s(check, L"check:%zu", i);
        hits.push_back({D2D1::RectF(row.left + 4, row.top, row.left + 30, row.bottom), check});
        // The result rides under the title, so a finished todo still says what it produced.
        const bool hasProgress = !item.progress.empty();
        const float textTop = hasProgress ? row.top + 1.f : row.top;
        const float textBottom = hasProgress ? row.bottom - 13.f : row.bottom;
        PaintTodoText(c, item.text, D2D1::RectF(row.left + 32, textTop, row.right - 8, textBottom), 13.f, item.done, false);
        if (hasProgress) {
            const D2D1_COLOR_F ink = item.done ? Alpha(Rgba(0x07C160, 0.98f), 0.85f) : p.sub;
            c.Text(item.progress, D2D1::RectF(row.left + 32, row.bottom - 14.f, row.right - 8, row.bottom), 10.5f, ink);
        }
        wchar_t id[32];
        swprintf_s(id, L"todo:%zu", i);
        hits.push_back({D2D1::RectF(row.left + 30, row.top, row.right, row.bottom), id});
        ly += rowH;
        ++shown;
    }
    if (extra) {
        D2D1_RECT_F more = D2D1::RectF(pad, ly, W - pad, ly + rowH - 4);
        c.Text(L"+" + std::to_wstring(total - shown), more, 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        wchar_t all[40];
        swprintf_s(all, L"todos:%04d-%02d-%02d", y, m, d);
        hits.push_back({more, all});
        ly += rowH;
    }
}

// How tall the todo list is under a list or a timetable. Dragging the grip stores
// a fraction; until then the list keeps its old height.
float TodoListHeight(const WidgetContent& content, float faceH, bool timetable)
{
    float pane = timetable ? content.tablePane : content.listPane;
    float fallback = timetable ? std::clamp(faceH * 0.30f, 132.f, 240.f) : std::clamp(faceH * 0.58f, 150.f, faceH * 0.70f);
    float list = pane < 0 ? fallback : pane * faceH;
    const float minList = 96.f;
    const float minTop = timetable ? 168.f : 148.f;
    return std::clamp(list, minList, std::max(minList, faceH - minTop));
}

// Month, week or day, filling whatever size the widget has. A todo's full text
// opens in a popup on click. New todos are added from the header, not from a day.
void PaintCaretLine(Canvas& c, const WidgetContent& content, const Pal& p, D2D1_RECT_F text, const std::wstring& s, float size,
                    std::vector<WidgetHit>& hits);

void PaintTimetable(Canvas& c, const WidgetContent& content, const WidgetWorld& world, const Pal& p, float W, float H,
                    std::vector<WidgetHit>& hits, float top)
{
    const wchar_t* dayName[] = {L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日"};
    int focusY = 0, focusM = 0, focusD = 0;
    CalendarFocus(content, world.now, focusY, focusM, focusD);
    int y = focusY, m = focusM, d = focusD;
    AddDays(y, m, d, -Weekday(y, m, d));
    const float dayFs = std::clamp(world.tableSize, 16.f, 48.f);
    const float dateFs = dayFs * 0.92f;
    const float pad = 10, ampmW = 16, numW = 18, timeW = 66, headH = dayFs + dateFs + 18.f;
    const float leftW = ampmW + numW + timeW;
    const bool editingCourse = content.editLesson && content.editing;
    const float barH = editingCourse ? 34.f : 0.f;
    const float gridTop = top + 6 + headH;
    const float gridH = std::max(8.f, H - gridTop - 8 - barH);
    const float colW = std::max(1.f, (W - 2 * pad - leftW) / 7.f);
    const float rowH = gridH / 8.f;
    const float nameSize = std::clamp(rowH * 0.46f, 14.f, 20.f);
    const float teacherSize = nameSize * 0.5f;
    auto hhmm = [](int mins) {
        mins = std::clamp(mins, 0, 24 * 60 - 1);
        wchar_t b[8];
        swprintf_s(b, L"%02d:%02d", mins / 60, mins % 60);
        return std::wstring(b);
    };
    auto lessonAt = [&](int day, int period) -> const WidgetContent::Lesson* {
        for (const auto& lesson : content.lessons)
            if (lesson.weekday == day && lesson.period == period)
                return &lesson;
        return nullptr;
    };
    auto lessonIndex = [&](int day, int period) {
        for (int i = 0; i < int(content.lessons.size()); ++i)
            if (content.lessons[size_t(i)].weekday == day && content.lessons[size_t(i)].period == period)
                return i;
        return -1;
    };
    const D2D1_COLOR_F pick = Picked(content, p);
    for (int col = 0; col < 7; ++col) {
        int cy = y, cm = m, cd = d;
        AddDays(cy, cm, cd, col);
        float x0 = pad + leftW + colW * col;
        bool today = cy == int(world.now.wYear) && cm == int(world.now.wMonth) && cd == int(world.now.wDay);
        bool picked = cy == focusY && cm == focusM && cd == focusD;
        D2D1_RECT_F head = D2D1::RectF(x0, top + 4, x0 + colW, top + 4 + headH);
        if (picked)
            c.Fill(head, 8, Alpha(pick, today ? 0.50f : 0.36f));
        else if (today)
            c.Fill(head, 8, Alpha(p.blue, 0.16f));
        if (picked)
            c.Stroke(head, 8, today ? Rgba(0xFFFFFF, 0.95f) : pick, 2.8f);
        c.Text(dayName[col], D2D1::RectF(x0, head.top, x0 + colW, head.top + dayFs + 6), dayFs, picked ? pick : today ? p.blue : p.sub,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        wchar_t date[16];
        swprintf_s(date, L"%d/%d", cm, cd);
        c.Text(date, D2D1::RectF(x0, head.top + dayFs + 2, x0 + colW, head.top + dayFs + dateFs + 8), dateFs, picked ? pick : today ? p.blue : p.text,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        int n = 0, open = 0;
        for (int i = 0; i < int(content.items.size()); ++i) {
            if (!CoversDay(content.items[size_t(i)], cy, cm, cd))
                continue;
            ++n;
            if (!content.items[size_t(i)].done)
                ++open;
        }
        if (n > 0) {
            const float scale = std::clamp(world.badgeScale, 0.4f, 1.6f);
            const float radius = 8.f * scale;
            const float cx = x0 + colW - 6.f - radius;
            const float cy0 = head.top + 4.f + radius;
            const D2D1_RECT_F badge = D2D1::RectF(cx - radius, cy0 - radius, cx + radius, cy0 + radius);
            if (open == 0) {
                c.Circle(cx, cy0, radius, Rgba(0x30D158, 1.f));
                c.Text(L"\u2713", badge, 11.f * scale, Rgba(0xFFFFFF, 1.f), DWRITE_FONT_WEIGHT_BOLD, 1);
            } else {
                c.Circle(cx, cy0, radius, Rgba(0xFF3B30, 1.f));
                c.Text(std::to_wstring(open), badge, (open > 9 ? 9.f : 10.f) * scale, Rgba(0xFFFFFF, 1.f), DWRITE_FONT_WEIGHT_BOLD, 1);
            }
        }
        wchar_t id[40];
        swprintf_s(id, L"head:%04d-%02d-%02d", cy, cm, cd);
        hits.push_back({head, id});
    }
    c.Text(L"上", D2D1::RectF(pad, gridTop + rowH * 1.5f, pad + ampmW, gridTop + rowH * 2.4f), 12, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"午", D2D1::RectF(pad, gridTop + rowH * 2.4f, pad + ampmW, gridTop + rowH * 3.3f), 12, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"下", D2D1::RectF(pad, gridTop + rowH * 5.6f, pad + ampmW, gridTop + rowH * 6.5f), 12, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"午", D2D1::RectF(pad, gridTop + rowH * 6.5f, pad + ampmW, gridTop + rowH * 7.4f), 12, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    for (int row = 0; row < 8; ++row) {
        float y0 = gridTop + rowH * row;
        int shown = row < 5 ? row + 1 : row - 4;
        c.Text(std::to_wstring(shown), D2D1::RectF(pad + ampmW, y0, pad + ampmW + numW, y0 + rowH), 12, p.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        D2D1_RECT_F clock = D2D1::RectF(pad + ampmW + numW, y0 + 1, pad + leftW - 2, y0 + rowH - 1);
        if (content.periodStart[row] < 0) {
            if (content.tableEdit)
                c.Text(L"设置", clock, 11, p.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        } else {
            c.Text(hhmm(content.periodStart[row]), D2D1::RectF(clock.left, y0 + 2, clock.right, y0 + rowH * 0.5f), 11, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            c.Text(hhmm(content.periodEnd[row]), D2D1::RectF(clock.left, y0 + rowH * 0.45f, clock.right, y0 + rowH - 1), 11, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
        }
        if (content.tableEdit) {
            wchar_t timeId[16];
            swprintf_s(timeId, L"time:%d", row);
            hits.push_back({clock, timeId});
        }
        if (row == 5)
            c.Line(pad, y0, W - pad, y0, Alpha(p.text, 0.18f), 1);
        for (int col = 0; col < 7; ++col) {
            float x0 = pad + leftW + colW * col;
            D2D1_RECT_F cell = D2D1::RectF(x0 + 1, y0 + 1, x0 + colW - 1, y0 + rowH - 1);
            c.Stroke(cell, 4, Alpha(p.text, 0.10f), 1);
            int index = lessonIndex(col, row);
            const WidgetContent::Lesson* lesson = lessonAt(col, row);
            int changeIndex = FindLessonChange(content, y, m, d, col, row);
            const WidgetContent::LessonChange* change = changeIndex >= 0 ? &content.changes[size_t(changeIndex)] : nullptr;
            bool editing = editingCourse && ((content.editScope == 1 && changeIndex == content.editChange) ||
                                             (content.editScope == 0 && index >= 0 && content.item == index));
            const bool off = change && change->off && !editing;
            const std::wstring* shownName = nullptr;
            const std::wstring* shownTeacher = nullptr;
            if (!off) {
                if (change && !change->off) {
                    shownName = &change->name;
                    shownTeacher = &change->teacher;
                } else if (editing && content.editScope == 1 && change) {
                    shownName = &change->name;
                    shownTeacher = &change->teacher;
                } else if (lesson) {
                    shownName = &lesson->name;
                    shownTeacher = &lesson->teacher;
                }
            }
            wchar_t id[24];
            if (off) {
                float fs = std::max(12.f, nameSize * 0.72f);
                float mid = (cell.top + cell.bottom) / 2.f;
                c.Text(L"停课", D2D1::RectF(cell.left, mid - fs * 0.7f, cell.right, mid + fs * 0.7f), fs, p.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
                swprintf_s(id, L"slot:%d:%d", col, row);
                hits.push_back({cell, id});
            } else if (shownName && (!shownName->empty() || (shownTeacher && !shownTeacher->empty()) || editing)) {
                const std::wstring& course = *shownName;
                const std::wstring& teacher = shownTeacher ? *shownTeacher : content.snapTeacher;
                if (change)
                    c.Text(L"调", D2D1::RectF(cell.right - 16, cell.top + 1, cell.right - 1, cell.top + 13), 10, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
                bool teachLine = editing || !teacher.empty();
                float block = nameSize + (teachLine ? teacherSize + 4.f : 0.f);
                float yText = cell.top + std::max(1.f, (cell.bottom - cell.top - block) / 2.f);
                D2D1_RECT_F nameR = D2D1::RectF(cell.left + 2, yText, cell.right - 2, yText + nameSize + 2);
                bool namePh = course.empty();
                c.Text(namePh ? L"课程" : course, nameR, nameSize, namePh ? p.faint : p.text, DWRITE_FONT_WEIGHT_BOLD, 1);
                if (editing && content.editPart == 0) {
                    float tw = std::max(2.f, c.TextWidth(course, nameSize, DWRITE_FONT_WEIGHT_BOLD));
                    float mid = (nameR.left + nameR.right) / 2.f;
                    PaintCaretLine(c, content, p, D2D1::RectF(mid - tw / 2.f, nameR.top, mid + tw / 2.f, nameR.bottom), course, nameSize, hits);
                }
                if (teachLine) {
                    D2D1_RECT_F teachR = D2D1::RectF(cell.left + 2, nameR.bottom, cell.right - 2, nameR.bottom + teacherSize + 4.f);
                    bool teachPh = teacher.empty();
                    c.Text(teachPh ? L"老师" : teacher, teachR, teacherSize, teachPh ? p.faint : p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
                    if (editing && content.editPart == 1) {
                        float tw = std::max(2.f, c.TextWidth(teacher, teacherSize));
                        float mid = (teachR.left + teachR.right) / 2.f;
                        PaintCaretLine(c, content, p, D2D1::RectF(mid - tw / 2.f, teachR.top, mid + tw / 2.f, teachR.bottom), teacher, teacherSize, hits);
                    }
                    swprintf_s(id, L"slot:%d:%d", col, row);
                    hits.push_back({D2D1::RectF(cell.left, cell.top, cell.right, (cell.top + cell.bottom) / 2), id});
                    swprintf_s(id, L"teach:%d:%d", col, row);
                    hits.push_back({D2D1::RectF(cell.left, (cell.top + cell.bottom) / 2, cell.right, cell.bottom), id});
                } else {
                    swprintf_s(id, L"slot:%d:%d", col, row);
                    hits.push_back({cell, id});
                }
            } else {
                swprintf_s(id, L"slot:%d:%d", col, row);
                hits.push_back({cell, id});
            }
        }
    }
    if (editingCourse) {
        const float yb = H - barH - 2;
        const bool restore = FindLessonChange(content, y, m, d, content.editWd, content.editSlot) >= 0;
        const wchar_t* labels[] = {L"每周", L"仅本周", L"停课", L"恢复"};
        const int n = restore ? 4 : 3;
        float widths[4] = {};
        float total = 0;
        for (int i = 0; i < n; ++i) {
            widths[i] = std::max(44.f, c.TextWidth(labels[i], 12) + 16.f);
            total += widths[i] + 6.f;
        }
        float x = pad;
        bool offNow = content.editScope == 1 && content.editChange >= 0 && content.editChange < int(content.changes.size()) &&
                      content.changes[size_t(content.editChange)].off;
        for (int i = 0; i < n; ++i) {
            D2D1_RECT_F chip = D2D1::RectF(x, yb, x + widths[i], yb + 26);
            bool on = (i == 0 && content.editScope == 0) || (i == 1 && content.editScope == 1 && !offNow) || (i == 2 && offNow);
            if (on)
                c.Fill(chip, 7, p.blue);
            else
                c.Fill(chip, 7, Alpha(p.text, 0.10f));
            c.Text(labels[i], chip, 12, on ? p.onAccent : p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            wchar_t id[16];
            swprintf_s(id, L"scope:%d", i);
            hits.push_back({chip, id});
            x += widths[i] + 6.f;
        }
        (void)total;
    }
    if (content.editPeriod >= 0 && content.editPeriod < 8) {
        int row = content.editPeriod;
        const float cardW = 188.f, cardH = 76.f;
        float cx = pad + ampmW + numW;
        if (cx + cardW > W - pad)
            cx = std::max(pad, W - pad - cardW);
        float cy = gridTop + rowH * float(row);
        if (cy + cardH > H - barH - 6)
            cy = std::max(top, H - barH - 6 - cardH);
        D2D1_RECT_F card = D2D1::RectF(cx, cy, cx + cardW, cy + cardH);
        c.Fill(card, 12, Rgba(0x1C1C1E, 0.94f));
        const D2D1_COLOR_F ink = Rgba(0xFFFFFF, 0.96f);
        const D2D1_COLOR_F dim = Rgba(0xFFFFFF, 0.62f);
        auto rowOf = [&](int which, const wchar_t* label, int mins, const wchar_t* minus, const wchar_t* plus) {
            float y0 = card.top + 8 + float(which) * 32;
            c.Text(label, D2D1::RectF(card.left + 12, y0, card.left + 52, y0 + 28), 13, dim, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0);
            D2D1_RECT_F dec = D2D1::RectF(card.left + 56, y0, card.left + 80, y0 + 28);
            D2D1_RECT_F inc = D2D1::RectF(card.right - 32, y0, card.right - 8, y0 + 28);
            c.Glyph(L"\uE76B", dec, 12, ink);
            c.Glyph(L"\uE76C", inc, 12, ink);
            c.Text(hhmm(mins < 0 ? 8 * 60 : mins), D2D1::RectF(dec.right, y0, inc.left, y0 + 28), 14, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            hits.push_back({dec, minus});
            hits.push_back({inc, plus});
        };
        hits.push_back({card, L"timekeep"});
        int start = content.periodStart[row];
        int end = content.periodEnd[row];
        rowOf(0, L"开始", start, L"ts:-5", L"ts:5");
        rowOf(1, L"结束", end, L"te:-5", L"te:5");
    }
}

void PaintCalendar(Canvas& c, WidgetSize, const WidgetContent& content, WidgetWorld w, Pal p, float W, float H,
                   std::vector<WidgetHit>& hits, const std::wstring& hoverDay)
{
    if (content.ownDate >= 12.f)
        w.dateSize = std::clamp(content.ownDate, 12.f, 36.f);
    if (content.ownTable >= 16.f)
        w.tableSize = std::clamp(content.ownTable, 16.f, 48.f);
    if (content.fontColor) {
        uint32_t rgb = content.fontColor & 0xFFFFFFu;
        p.text = Rgba(rgb, 0.96f);
        p.sub = Rgba(rgb, 0.68f);
        p.faint = Rgba(rgb, 0.42f);
    }
    const float faceH = std::max(120.f, H);
    const SYSTEMTIME& now = w.now;
    int focusY = 0, focusM = 0, focusD = 0;
    CalendarFocus(content, now, focusY, focusM, focusD);
    const int year = focusY, month = focusM;
    const bool onToday = focusY == int(now.wYear) && focusM == int(now.wMonth) && focusD == int(now.wDay);
    const int view = content.calView < 0 || content.calView > 2 ? 2 : content.calView;
    const bool compact = faceH < 280.f || W < 360.f;
    const bool split = content.calSplit && view != 0 && !compact;
    const float listH = content.calBoard == 2 ? TodoListHeight(content, faceH, true) : split ? TodoListHeight(content, faceH, false) : 0.f;
    const float bodyH = std::max(110.f, faceH - listH);
    const float pad = 14, headerH = 34, weekH = 22;
    const float gridTop = pad + headerH + weekH;
    const float gridH = std::max(6.f, bodyH - pad - gridTop);
    const float cellW = std::max(1.f, (W - 2 * pad) / 7.f);
    const float cellH = gridH / 6.f;

    {
    const bool table = content.calBoard == 2;
    if (!table)
        return; // the calendar page draws this row itself; this window must not cover it
    const float rowH = 48.f;
    const D2D1_COLOR_F paper = D2D1::ColorF(0.965f, 0.965f, 0.973f, 1.f);
    const D2D1_COLOR_F ink = Rgba(0x1D1D1F, 1.f);
    const D2D1_COLOR_F mute = Rgba(0x6E6E73, 1.f);
    const D2D1_COLOR_F accent = Rgba(0x0A84FF, 1.f);
    const float lockS = 32.f, segW = 64.f, addW = 76.f, gap = 8.f, btnH = 28.f;
    const float editW = 58.f, toolW = 32.f, layerW = 32.f;
    const float y0 = (rowH - btnH) / 2.f;
    const float groupW = layerW + 4.f + lockS + gap + segW * 2.f;
    const float x0 = std::max(12.f + toolW + gap, (W - groupW) / 2.f);
    if (table)
        c.Fill(D2D1::RectF(0, 0, W, H), 0, paper);
    c.Line(0, rowH - 0.5f, W, rowH - 0.5f, Rgba(0xE5E5EA, 1.f), 1.f);
    D2D1_RECT_F lockR = D2D1::RectF(x0, y0, x0 + lockS, y0 + btnH);
    D2D1_RECT_F layerBtn = D2D1::RectF(lockR.right + 4.f, y0, lockR.right + 4.f + layerW, y0 + btnH);
    c.Mdl2(content.calLock ? L"\uE72E" : L"\uE785", lockR, 16, content.calLock ? Rgba(0xFF9F0A, 1.f) : mute);
    if (content.calLock && content.lockPress > 0.f) {
        float cx = (lockR.left + lockR.right) / 2.f;
        float cy = (lockR.top + lockR.bottom) / 2.f;
        c.Arc(cx, cy, 13.f, -90.f, 269.f, Rgba(0xFF9F0A, 0.28f), 2.f);
        c.Arc(cx, cy, 13.f, -90.f, -90.f + 359.f * std::clamp(content.lockPress, 0.f, 1.f), Rgba(0xFF9F0A, 1.f), 2.6f);
    }
    hits.push_back({lockR, L"lock"});
    const bool layerOn = content.windowLayer == 0 || content.windowLayer == 2 || content.layerOpen;
    c.Mdl2(L"\uE718", layerBtn, 16, layerOn ? accent : mute);
    hits.push_back({layerBtn, L"layer"});
    const float segX = layerBtn.right + gap;
    D2D1_RECT_F calBtn = D2D1::RectF(segX, y0, segX + segW, y0 + btnH);
    D2D1_RECT_F tableBtn = D2D1::RectF(segX + segW, y0, segX + segW * 2.f, y0 + btnH);
    c.Fill(D2D1::RectF(segX - 2, y0 - 2, segX + segW * 2.f + 2, y0 + btnH + 2), 8, Rgba(0xF2F2F7, 1.f));
    if (!table)
        c.Fill(calBtn, 7, accent);
    else
        c.Fill(tableBtn, 7, accent);
    c.Text(L"日历", calBtn, 13, table ? mute : Rgba(0xFFFFFF, 1.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"课表", tableBtn, 13, table ? Rgba(0xFFFFFF, 1.f) : mute, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    hits.push_back({calBtn, L"layout:0"});
    hits.push_back({tableBtn, L"layout:2"});
    const float right = W - 12.f;
    D2D1_RECT_F addBtn = D2D1::RectF(right - addW, y0, right, y0 + btnH);
    D2D1_RECT_F editBtn = D2D1::RectF(addBtn.left - gap - editW, y0, addBtn.left - gap, y0 + btnH);
    D2D1_RECT_F toolBtn = D2D1::RectF(12.f, y0, 12.f + toolW, y0 + btnH);
    c.Fill(addBtn, 8, Rgba(0x0A84FF, 0.14f));
    c.Text(L"新建待办", addBtn, 12, accent, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    hits.push_back({addBtn, L"newtodo"});
    if (table) {
        c.Fill(editBtn, 8, content.tableEdit ? accent : Rgba(0x0A84FF, 0.14f));
        c.Text(content.tableEdit ? L"保存" : L"编辑", editBtn, 12, content.tableEdit ? Rgba(0xFFFFFF, 1.f) : accent, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        hits.push_back({editBtn, L"tableedit"});
        c.Fill(toolBtn, 8, content.toolsOpen ? accent : Rgba(0x1D1D1F, 0.06f));
        c.Mdl2(content.toolsOpen ? L"\uE70D" : L"\uE76C", toolBtn, 12, content.toolsOpen ? Rgba(0xFFFFFF, 1.f) : ink);
        hits.push_back({toolBtn, L"tools"});
    }
    Pal inkPal = p;
    inkPal.text = ink;
    inkPal.sub = mute;
    inkPal.faint = Rgba(0x8E8E93, 0.8f);
    inkPal.onAccent = Rgba(0xFFFFFF, 1.f);
    const float paneList = TodoListHeight(content, faceH - rowH, true);
    const float tableH = std::max(160.f, faceH - rowH - paneList);
    const size_t mark = hits.size();
    c.BeginGroup(1.f, 1.f, 0.f, 0.f, 0.f, rowH);
    PaintTimetable(c, content, w, inkPal, W, tableH, hits, 8.f);
    PaintSplitList(c, content, inkPal, W, faceH - rowH, tableH, hits, focusY, focusM, focusD);
    PaintTodoPopup(c, content, inkPal, W, faceH - rowH, hits);
    PaintTodoEditor(c, content, inkPal, W, faceH - rowH, hits);
    c.EndGroup();
    for (size_t i = mark; i < hits.size(); ++i) {
        hits[i].rect.top += rowH;
        hits[i].rect.bottom += rowH;
    }
    if (content.toolsOpen) {
        const wchar_t* toolName[] = {L"倒计时", L"秒表", L"随机抽签", L"分组"};
        const float mw = 156.f, menuRow = 34.f, mh = 8.f + 4.f * menuRow;
        float mx = toolBtn.left;
        float my = toolBtn.bottom + 6.f;
        if (my + mh > faceH - 8.f)
            my = std::max(rowH + 4.f, toolBtn.top - 6.f - mh);
        D2D1_RECT_F menu = D2D1::RectF(mx, my, mx + mw, my + mh);
        c.Fill(menu, 12, Rgba(0x1C1C1E, 0.96f));
        c.Stroke(menu, 12, Rgba(0x0A84FF, 0.75f), 1.2f);
        for (int i = 0; i < 4; ++i) {
            D2D1_RECT_F row = D2D1::RectF(mx + 4, my + 4 + float(i) * menuRow, mx + mw - 4, my + 2 + float(i + 1) * menuRow);
            c.Text(toolName[i], row, 14, Rgba(0xFFFFFF, 0.96f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            wchar_t id[16];
            swprintf_s(id, L"tool:%d", i);
            hits.push_back({row, id});
        }
    }
    if (content.layerOpen) {
        const wchar_t* layerName[] = {L"始终置顶", L"正常", L"置底"};
        const wchar_t* layerId[] = {L"z:top", L"z:normal", L"z:bottom"};
        const int layerValue[] = {2, 1, 0};
        const float mw = 132.f, menuRow = 34.f, mh = 8.f + 3.f * menuRow;
        float mx = std::max(8.f, layerBtn.right - mw);
        float my = layerBtn.bottom + 6.f;
        if (my + mh > faceH - 8.f)
            my = std::max(rowH + 4.f, layerBtn.top - 6.f - mh);
        D2D1_RECT_F menu = D2D1::RectF(mx, my, mx + mw, my + mh);
        c.Fill(menu, 12, Rgba(0x1C1C1E, 0.96f));
        c.Stroke(menu, 12, Rgba(0x0A84FF, 0.75f), 1.2f);
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F row = D2D1::RectF(mx + 4, my + 4 + float(i) * menuRow, mx + mw - 4, my + 2 + float(i + 1) * menuRow);
            const bool on = content.windowLayer == layerValue[i] || (content.windowLayer < 0 && layerValue[i] == 1);
            c.Text(layerName[i], row, 14, on ? accent : Rgba(0xFFFFFF, 0.96f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            hits.push_back({row, layerId[i]});
        }
        }
        return;
    }

    const D2D1_RECT_F lockR = D2D1::RectF(pad, pad + 2, pad + 26, pad + headerH - 4);
    c.Glyph(content.calLock ? L"\uE72E" : L"\uE785", lockR, 15, content.calLock ? p.orange : p.sub);
    if (content.calLock && content.lockPress > 0.f) {
        float cx = (lockR.left + lockR.right) / 2.f;
        float cy = (lockR.top + lockR.bottom) / 2.f;
        c.Arc(cx, cy, 12.f, -90.f, 269.f, Alpha(p.orange, 0.28f), 2.f);
        c.Arc(cx, cy, 12.f, -90.f, -90.f + 359.f * std::clamp(content.lockPress, 0.f, 1.f), p.orange, 2.6f);
    }
    hits.push_back({lockR, L"lock"});
    const D2D1_RECT_F gearR = D2D1::RectF(pad + 28, pad + 2, pad + 50, pad + headerH - 4);
    c.Glyph(L"\uE713", gearR, 14, p.sub);
    hits.push_back({gearR, L"style"});
    const D2D1_RECT_F prev = D2D1::RectF(pad + 54, pad + 2, pad + 78, pad + headerH - 4);
    const D2D1_RECT_F next = D2D1::RectF(pad + 78, pad + 2, pad + 102, pad + headerH - 4);
    c.Glyph(L"\uE76B", prev, 13, p.text);
    c.Glyph(L"\uE76C", next, 13, p.text);
    hits.push_back({prev, L"month:-1"});
    hits.push_back({next, L"month:1"});
    const float segH = 22;
    const float segY = pad + (headerH - segH) / 2.f;
    // Day, week and month keep a fixed slot on the right. Hiding them in the timetable
    // must not slide the layout switch or the today button.
    const float viewW = 30.f;
    const float viewX = W - pad - 3.f * viewW;
    const float todayW = 46.f;
    const float todayRight = viewX - 6.f;
    if (content.calBoard != 2) {
        c.Fill(D2D1::RectF(viewX - 1, segY - 1, viewX + 3.f * viewW + 1, segY + segH + 1), 8, Alpha(p.text, 0.08f));
        const wchar_t* viewName[] = {L"日", L"周", L"月"};
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F seg = D2D1::RectF(viewX + float(i) * viewW, segY, viewX + float(i + 1) * viewW, segY + segH);
            if (view == i)
                c.Fill(seg, 7, p.blue);
            c.Text(viewName[i], seg, 12, view == i ? p.onAccent : p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            wchar_t id[16];
            swprintf_s(id, L"view:%d", i);
            hits.push_back({seg, id});
        }
    }
    const D2D1_RECT_F todayBtn = D2D1::RectF(todayRight - todayW, pad + 4, todayRight, pad + headerH - 6);
    if (!onToday)
        c.Stroke(todayBtn, 8, Alpha(p.blue, 0.55f), 1);
    c.Text(L"今天", todayBtn, 12, onToday ? p.faint : p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    hits.push_back({todayBtn, L"month:0"});
    if (content.calBoard == 2) {
        D2D1_RECT_F editBtn = D2D1::RectF(viewX, todayBtn.top, viewX + 52.f, todayBtn.bottom);
        if (content.tableEdit)
            c.Fill(editBtn, 8, p.blue);
        else
            c.Fill(editBtn, 8, Alpha(p.blue, 0.22f));
        c.Text(content.tableEdit ? L"保存" : L"编辑", editBtn, 12, content.tableEdit ? p.onAccent : p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        hits.push_back({editBtn, L"tableedit"});
        D2D1_RECT_F toolBtn = D2D1::RectF(viewX + 56.f, todayBtn.top, viewX + 86.f, todayBtn.bottom);
        c.Fill(toolBtn, 8, content.toolsOpen ? p.blue : Alpha(p.text, 0.10f));
        c.Glyph(content.toolsOpen ? L"\uE70D" : L"\uE76C", toolBtn, 12, content.toolsOpen ? p.onAccent : p.text);
        hits.push_back({toolBtn, L"tools"});
    }
    const float leftLimit = pad + 108.f;
    float addW = 76.f;
    float addRight = todayBtn.left - 6.f;
    float addLeft = addRight - addW;
    if (addLeft < leftLimit) {
        addLeft = leftLimit;
        addW = addRight - addLeft;
    }
    if (addW >= 56.f) {
        D2D1_RECT_F addBtn = D2D1::RectF(addLeft, todayBtn.top, addRight, todayBtn.bottom);
        c.Fill(addBtn, 8, Alpha(p.blue, 0.22f));
        c.Text(L"新建待办", addBtn, addW < 68.f ? 11.f : 12.f, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        hits.push_back({addBtn, L"newtodo"});
    }
    const float rightLimit = (addW >= 56.f ? addLeft : todayBtn.left) - 8.f;
    float layW = 48.f;
    if (rightLimit - leftLimit < 3.f * layW)
        layW = std::max(36.f, (rightLimit - leftLimit) / 3.f);
    const float laySpan = 3.f * layW;
    float layX = (W - laySpan) / 2.f;
    if (layX < leftLimit)
        layX = leftLimit;
    if (layX + laySpan > rightLimit)
        layX = std::max(leftLimit, rightLimit - laySpan);
    float titleRight = layX - 6;
    {
        const float layY = segY;
        c.Fill(D2D1::RectF(layX - 1, layY - 1, layX + 3 * layW + 1, layY + segH + 1), 8, Alpha(p.text, 0.08f));
        const wchar_t* layName[] = {L"表格", L"列表", L"课表"};
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F seg = D2D1::RectF(layX + float(i) * layW, layY, layX + float(i + 1) * layW, layY + segH);
            bool on = content.calBoard == i;
            if (on)
                c.Fill(seg, 7, p.blue);
            c.Text(layName[i], seg, layW < 38.f ? 11.f : 12.f, on ? p.onAccent : p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            wchar_t id[16];
            swprintf_s(id, L"layout:%d", i);
            hits.push_back({seg, id});
        }
    }
    std::wstring title = std::to_wstring(year) + L"年" + std::to_wstring(month) + L"月";
    if (content.calBoard == 2) {
        int ay = focusY, am = focusM, ad = focusD, by = focusY, bm = focusM, bd = focusD;
        AddDays(ay, am, ad, -Weekday(ay, am, ad));
        AddDays(by, bm, bd, 6 - Weekday(by, bm, bd));
        title = std::to_wstring(am) + L"月" + std::to_wstring(ad) + L"日 – " + std::to_wstring(bm) + L"月" + std::to_wstring(bd) + L"日";
    } else if (view == 0)
        title = std::to_wstring(month) + L"月" + std::to_wstring(focusD) + L"日";
    else if (view == 1) {
        int ay = focusY, am = focusM, ad = focusD, by = focusY, bm = focusM, bd = focusD;
        int lead = (Weekday(focusY, focusM, focusD) - FirstWeekday() + 7) % 7;
        AddDays(ay, am, ad, -lead);
        AddDays(by, bm, bd, 6 - lead);
        title = std::to_wstring(am) + L"月" + std::to_wstring(ad) + L"日";
        if (am != bm || ad != bd)
            title += L" – " + std::to_wstring(bm) + L"月" + std::to_wstring(bd) + L"日";
    }
    c.Text(title, D2D1::RectF(leftLimit, pad, titleRight, pad + headerH - 2), std::clamp(w.dateSize * 0.8f, 15.f, 28.f), p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (content.calBoard == 2) {
        const float tableH = std::max(pad + headerH + 120.f, faceH - listH);
        PaintTimetable(c, content, w, p, W, tableH, hits, pad + headerH);
        PaintSplitList(c, content, p, W, faceH, tableH, hits, focusY, focusM, focusD);
        PaintTodoPopup(c, content, p, W, H, hits);
        PaintTodoEditor(c, content, p, W, H, hits);
        if (content.toolsOpen) {
            const wchar_t* toolName[] = {L"倒计时", L"秒表", L"随机抽签", L"分组"};
            const float mw = 156.f, rowH = 34.f, mh = 8.f + 4.f * rowH;
            D2D1_RECT_F toolBtn = D2D1::RectF(viewX + 56.f, todayBtn.top, viewX + 86.f, todayBtn.bottom);
            float mx = std::max(pad, toolBtn.right - mw);
            float my = toolBtn.bottom + 6.f;
            if (my + mh > faceH - 8.f)
                my = std::max(pad, toolBtn.top - 6.f - mh);
            D2D1_RECT_F menu = D2D1::RectF(mx, my, mx + mw, my + mh);
            c.Fill(menu, 12, Rgba(0x1C1C1E, 0.96f));
            c.Stroke(menu, 12, Alpha(p.blue, 0.75f), 1.2f);
            for (int i = 0; i < 4; ++i) {
                D2D1_RECT_F row = D2D1::RectF(mx + 4, my + 4 + float(i) * rowH, mx + mw - 4, my + 2 + float(i + 1) * rowH);
                c.Text(toolName[i], row, 14, Rgba(0xFFFFFF, 0.96f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
                wchar_t id[16];
                swprintf_s(id, L"tool:%d", i);
                hits.push_back({row, id});
            }
        }
        return;
    }
    // 日历页由 Yunzhao_note 盖在这块区域上。课表则由本窗口画在它上面。
    c.Fill(D2D1::RectF(8, pad + headerH, W - 8, faceH - 8), 16, D2D1::ColorF(0.965f, 0.965f, 0.973f, 0.96f));
    return;
    if (compact || view == 0) {
        PaintDayBoard(c, content, w, p, W, faceH, hits, focusY, focusM, focusD);
        PaintTodoPopup(c, content, p, W, H, hits);
        PaintTodoEditor(c, content, p, W, H, hits);
        return;
    }
    if (view == 1) {
        const float dateFs = std::clamp(w.dateSize, 12.f, 36.f);
        const float weekCap = pad + headerH + weekH + dateFs + 36.f;
        const float weekFace = !split ? bodyH : content.listPane < 0.f ? std::min(bodyH, weekCap) : bodyH;
        PaintWeekBoard(c, content, w, p, W, weekFace, hits, hoverDay, focusY, focusM, focusD, !split);
        if (split)
            PaintSplitList(c, content, p, W, faceH, weekFace, hits, focusY, focusM, focusD);
        PaintTodoPopup(c, content, p, W, H, hits);
        PaintTodoEditor(c, content, p, W, H, hits);
        return;
    }

    c.Fill(D2D1::RectF(8, pad + headerH - 2, W - 8, std::max(pad + headerH + 40.f, bodyH - 4)), 16, D2D1::ColorF(0.965f, 0.965f, 0.975f, 0.97f));
    const D2D1_COLOR_F ink = Rgba(0x1D1D1F, 0.92f);
    const D2D1_COLOR_F mute = Rgba(0x6E6E73, 0.95f);
    const D2D1_COLOR_F pick = Picked(content, p);
    const float side = !split && W > 720.f ? 214.f : 0.f;
    if (side > 0.f) {
        D2D1_RECT_F rail = D2D1::RectF(pad, gridTop - 8, pad + side - 8, bodyH - 12);
        c.Fill(rail, 14, D2D1::ColorF(1, 1, 1, 0.72f));
        std::wstring head = std::to_wstring(focusM) + L"月" + std::to_wstring(focusD) + L"日 · " + std::wstring(kWeekLong[Weekday(focusY, focusM, focusD)]);
        c.Text(head, D2D1::RectF(rail.left + 12, rail.top + 10, rail.right - 8, rail.top + 34), 15, ink, DWRITE_FONT_WEIGHT_BOLD);
        int count = TodosOn(content, focusY, focusM, focusD, false);
        c.Text(std::to_wstring(count) + L" 条待办", D2D1::RectF(rail.left + 12, rail.top + 34, rail.right - 8, rail.top + 54), 12, mute);
        float ly = rail.top + 62;
        int shown = 0;
        for (size_t i = 0; i < content.items.size() && ly + 52 < rail.bottom; ++i) {
            const Reminder& item = content.items[i];
            if (!CoversDay(item, focusY, focusM, focusD))
                continue;
            D2D1_RECT_F card = D2D1::RectF(rail.left + 8, ly, rail.right - 8, ly + 48);
            c.Fill(card, 10, D2D1::ColorF(1, 1, 1, 0.96f));
            c.Stroke(card, 10, Rgba(0x000000, 0.06f), 1);
            D2D1_COLOR_F mark = TodoMarkColor(item);
            if (!item.done && item.pri == 0)
                mark = Rgba(0x8E8E93, 0.95f);
            c.Circle(card.left + 16, card.top + 16, 5.5f, mark);
            c.Text(item.text, D2D1::RectF(card.left + 28, card.top + 4, card.right - 6, card.top + 26), 13, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(item.done ? L"已完成" : L"进行中", D2D1::RectF(card.left + 28, card.top + 24, card.right - 6, card.bottom - 4), 11,
                   item.done ? Rgba(0x30D158, 1.f) : mute);
            wchar_t id[32];
            swprintf_s(id, L"todo:%zu", i);
            hits.push_back({card, id});
            ly += 54;
            ++shown;
        }
        (void)shown;
    }
    const float originX = side > 0.f ? pad + side : pad;
    const float colW = std::max(1.f, (W - pad - originX) / 7.f);
    const wchar_t* headName[] = {L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日"};
    for (int i = 0; i < 7; ++i)
        c.Text(headName[i], D2D1::RectF(originX + float(i) * colW, pad + headerH, originX + float(i + 1) * colW, gridTop), 13, mute,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    int startY = year, startM = month, startD = 1;
    AddDays(startY, startM, startD, -Weekday(year, month, 1));
    const float gap = 5.f;
    for (int i = 0; i < 42; ++i) {
        int cy = startY, cm = startM, cd = startD;
        AddDays(cy, cm, cd, i);
        const float x0 = originX + float(i % 7) * colW;
        const float y0 = gridTop + float(i / 7) * cellH;
        const D2D1_RECT_F cell = D2D1::RectF(x0 + gap * 0.5f, y0 + gap * 0.5f, x0 + colW - gap * 0.5f, y0 + cellH - gap * 0.5f);
        const bool inMonth = cy == year && cm == month;
        const bool today = cy == int(now.wYear) && cm == int(now.wMonth) && cd == int(now.wDay);
        const std::wstring id = DayId(cy, cm, cd);
        const bool picked = cy == focusY && cm == focusM && cd == focusD;
        const bool hot = hoverDay == id;
        c.Fill(cell, 10, hot && !picked ? D2D1::ColorF(0.93f, 0.94f, 0.96f, 1.f) : D2D1::ColorF(1, 1, 1, inMonth ? 0.98f : 0.72f));
        if (today)
            c.Stroke(cell, 10, Rgba(0xFF453A, 1.f), 1.7f);
        else
            c.Stroke(cell, 10, picked ? pick : Rgba(0x000000, 0.08f), picked ? 1.8f : 1.f);
        if (today && picked)
            c.Stroke(D2D1::RectF(cell.left + 3, cell.top + 3, cell.right - 3, cell.bottom - 3), 8, pick, 1.3f);
        const float numSize = std::clamp(std::min(16.f, cellH * 0.28f), 12.f, 18.f);
        c.Text(std::to_wstring(cd), D2D1::RectF(cell.left + 6, cell.top + 3, cell.left + 6 + numSize + 8, cell.top + 4 + numSize), numSize,
               inMonth ? ink : Rgba(0xC7C7CC, 1.f), DWRITE_FONT_WEIGHT_BOLD);
        if (w.lunar && cell.right - cell.left > 64.f) {
            bool festival = false;
            std::wstring label = w.lunar->DayLabel(cy, cm, cd, &festival);
            if (label.empty())
                label = w.lunar->MonthDay(cy, cm, cd);
            if (!label.empty())
                c.Text(label, D2D1::RectF(cell.left + numSize + 14, cell.top + 5, cell.right - (today ? 24.f : 6.f), cell.top + 5 + numSize),
                       std::max(10.f, numSize * 0.72f), festival ? Rgba(0x0A84FF, 1.f) : mute);
        }
        if (today)
            c.Text(L"今", D2D1::RectF(cell.right - 22, cell.top + 4, cell.right - 4, cell.top + 20), 11, Rgba(0x0A84FF, 1.f), DWRITE_FONT_WEIGHT_BOLD, 2);
        hits.push_back({cell, id});
        float pillTop = cell.top + numSize + 8;
        int covered = 0, drawn = 0;
        for (size_t n = 0; n < content.items.size(); ++n) {
            const Reminder& item = content.items[n];
            if (!CoversDay(item, cy, cm, cd))
                continue;
            ++covered;
            if (pillTop + 18 > cell.bottom - 18)
                continue;
            D2D1_RECT_F pill = D2D1::RectF(cell.left + 4, pillTop, cell.right - 4, pillTop + 16);
            D2D1_COLOR_F fill = TodoMarkColor(item);
            if (!item.done && item.pri == 0)
                fill = Rgba(0x8E8E93, 0.92f);
            if (item.done)
                fill.a *= 0.55f;
            c.Fill(pill, 4, fill);
            const bool yellow = item.pri == 2 && !item.done;
            c.Text(item.text, D2D1::RectF(pill.left + 6, pill.top, pill.right - 4, pill.bottom), 11, yellow ? Rgba(0x1D1D1F, 0.92f) : Rgba(0xFFFFFF, 0.98f),
                   DWRITE_FONT_WEIGHT_SEMI_BOLD);
            wchar_t tid[32];
            swprintf_s(tid, L"todo:%zu", n);
            hits.push_back({pill, tid});
            pillTop += 18;
            ++drawn;
        }
        if (covered > drawn) {
            D2D1_RECT_F more = D2D1::RectF(cell.right - 28, cell.bottom - 18, cell.right - 4, cell.bottom - 3);
            c.Fill(more, 4, Rgba(0x0A84FF, 1.f));
            c.Text(std::to_wstring(covered), more, 10, Rgba(0xFFFFFF, 1.f), DWRITE_FONT_WEIGHT_BOLD, 1);
            wchar_t all[40];
            swprintf_s(all, L"todos:%04d-%02d-%02d", cy, cm, cd);
            hits.push_back({more, all});
        }
        D2D1_RECT_F plus = D2D1::RectF(cell.left + 2, cell.bottom - 18, cell.left + 22, cell.bottom - 1);
        c.Text(L"+", plus, 16, mute, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        wchar_t add[40];
        swprintf_s(add, L"add:%04d-%02d-%02d", cy, cm, cd);
        hits.push_back({plus, add});
    }
    (void)cellW;
    if (split)
        PaintSplitList(c, content, p, W, faceH, bodyH, hits, focusY, focusM, focusD);
    PaintTodoPopup(c, content, p, W, H, hits);
    PaintTodoEditor(c, content, p, W, H, hits);
}

// Batteries: a ring round the computer, the level under it.
void PaintBattery(Canvas& c, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const float cx = W / 2, cy = H / 2 - 12, R = 44;
    c.Arc(cx, cy, R, -90, 270, p.track, 9);
    if (w.status.battery) {
        int pct = std::clamp(w.status.batteryPercent, 0, 100);
        D2D1_COLOR_F level = pct <= 20 && !w.status.charging ? p.red : p.green;
        c.Arc(cx, cy, R, -90, -90 + 360.f * float(pct) / 100.f, level, 9);
        c.Glyph(L"\uE7F8", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 24, p.text);
        // The level, with a bolt before it while charging.
        std::wstring n = std::to_wstring(pct) + L"%";
        float tw = c.TextWidth(n, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD), bolt = w.status.charging ? 16.f : 0.f, x0 = (W - tw - bolt) / 2;
        if (w.status.charging)
            c.Glyph(L"\uE945", D2D1::RectF(x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36), 13, level);
        c.Text(n, D2D1::RectF(x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36), 19, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    } else {
        c.Arc(cx, cy, R, -90, 270, p.green, 9);
        c.Glyph(L"\uE83E", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 22, p.text);
        c.Text(L"已接通电源", D2D1::RectF(0, cy + R + 12, W, cy + R + 34), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    (void)H;
}

// The processor, the memory and the system disk: bars (small) or rings
// (medium), green, orange from 70 %, red from 90 %.
void PaintSystem(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    struct Gauge {
        const wchar_t* name;
        float value;
        std::wstring detail;
    };
    wchar_t mem[32], disk[32];
    swprintf_s(mem, L"%.1f / %.0f GB", w.memoryGb, w.memoryTotalGb);
    swprintf_s(disk, L"%.0f GB 可用", w.diskFreeGb);
    const Gauge gauges[] = {{L"CPU", w.cpu, L"处理器"}, {L"内存", w.memory, mem}, {L"磁盘", w.disk, disk}};
    auto colour = [&](float v) { return v >= 0.9f ? p.red : v >= 0.7f ? p.orange : p.green; };
    auto pct = [](float v) { return std::to_wstring(int(std::lround(std::clamp(v, 0.f, 1.f) * 100))) + L"%"; };
    if (size == WidgetSize::Small) {
        c.Text(L"系统", D2D1::RectF(kPad + 2, kPad - 2, W - kPad, kPad + 16), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float rowH = (H - 2 * kPad - 18) / 3;
        for (int i = 0; i < 3; ++i) {
            float y = kPad + 18 + rowH * float(i);
            c.Text(gauges[i].name, D2D1::RectF(kPad + 2, y + 2, W / 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(pct(gauges[i].value), D2D1::RectF(W / 2, y + 2, W - kPad - 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
            D2D1_RECT_F track = D2D1::RectF(kPad + 2, y + 26, W - kPad - 2, y + 32);
            c.Fill(track, 3, p.track);
            float v = std::clamp(gauges[i].value, 0.f, 1.f);
            if (v > 0.01f)
                c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * v), track.bottom), 3, colour(v));
        }
        return;
    }
    const float colW = (W - 2 * kPad) / 3, R = 34, cy = kPad + 6 + R;
    for (int i = 0; i < 3; ++i) {
        float cx = kPad + colW * (float(i) + 0.5f), v = std::clamp(gauges[i].value, 0.f, 1.f);
        c.Arc(cx, cy, R, -90, 270, p.track, 8);
        if (v > 0.005f)
            c.Arc(cx, cy, R, -90, -90 + 360.f * v, colour(v), 8);
        c.Text(pct(v), D2D1::RectF(cx - R, cy - 12, cx + R, cy + 12), 17, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].name, D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].detail, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Bluetooth devices' batteries (macOS's Batteries widget): small, four rings, each the device's
// symbol over its level; medium, four rows of symbol, name, bar and level. Red at 20 % or less.
const wchar_t* BluetoothGlyph(BluetoothKind k)
{
    switch (k) {
    case BluetoothKind::Headset: return L"\uE7F6";
    case BluetoothKind::Gamepad: return L"\uE7FC";
    case BluetoothKind::Keyboard: return L"\uE765";
    case BluetoothKind::Mouse: return L"\uE962";
    case BluetoothKind::Pen: return L"\uEDC6";
    case BluetoothKind::Phone: return L"\uE8EA";
    default: return L"\uE702";
    }
}

void PaintBluetooth(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const auto& devices = w.bluetooth;
    const size_t n = std::min<size_t>(devices.size(), 4);
    auto colour = [&](int pct) { return pct <= 20 ? p.red : p.green; };
    if (w.bluetoothRead && n == 0) {
        c.Glyph(L"\uE702", D2D1::RectF(0, H / 2 - 40, W, H / 2 - 4), 26, p.sub);
        c.Text(L"没有已连接的设备", D2D1::RectF(kPad, H / 2 + 4, W - kPad, H / 2 + 24), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        return;
    }
    if (size == WidgetSize::Small) {
        const float cw = (W - 2 * kPad) / 2, ch = (H - 2 * kPad) / 2, R = 21;
        for (size_t i = 0; i < 4; ++i) {
            const float cx = kPad + cw * (float(i % 2) + 0.5f), cy = kPad + ch * float(i / 2) + 26;
            c.Arc(cx, cy, R, -90, 270, p.track, 5);
            if (i >= n)
                continue;
            const int pct = std::clamp(devices[i].percent, 0, 100);
            if (pct > 0)
                c.Arc(cx, cy, R, -90, -90 + 360.f * float(pct) / 100.f, colour(pct), 5);
            c.Glyph(BluetoothGlyph(devices[i].kind), D2D1::RectF(cx - R, cy - R, cx + R, cy + R), 15, p.text);
            c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(cx - cw / 2, cy + R + 4, cx + cw / 2, cy + R + 20), 11.5f, p.text,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        }
        return;
    }
    const float rowH = (H - 2 * kPad) / 4;
    for (size_t i = 0; i < n; ++i) {
        const float y = kPad + rowH * float(i), mid = y + rowH / 2;
        const int pct = std::clamp(devices[i].percent, 0, 100);
        c.Glyph(BluetoothGlyph(devices[i].kind), D2D1::RectF(kPad, mid - 12, kPad + 26, mid + 12), 16, p.text);
        c.Text(devices[i].name, D2D1::RectF(kPad + 34, mid - 10, W - kPad - 112, mid + 10), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const D2D1_RECT_F track = D2D1::RectF(W - kPad - 106, mid - 3, W - kPad - 50, mid + 3);
        c.Fill(track, 3, p.track);
        if (pct > 0)
            c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * float(pct) / 100.f), track.bottom), 3,
                   colour(pct));
        c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(W - kPad - 46, mid - 10, W - kPad, mid + 10), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// A note: its title, then the text; while editing, the caret, and the text
// scrolled up when the caret goes below the card.
void PaintNotes(Canvas& c, const WidgetContent& n, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    c.Text(L"备忘录", D2D1::RectF(kPad + 2, kPad - 2, W - kPad, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F box = D2D1::RectF(kPad + 2, kPad + 24, W - kPad - 2, H - kPad);
    hits.push_back({D2D1::RectF(0, 0, W, H), L"text"});
    const float size = 13.5f;
    if (n.text.empty() && !n.editing) {
        c.Text(L"点按以记录…", box, size, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    if (!n.editing) {
        c.Text(n.text, box, size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    D2D1_RECT_F caret = c.Caret(n.text, box, size, DWRITE_FONT_WEIGHT_NORMAL, n.caret);
    float scroll = std::max(0.f, caret.bottom - box.bottom);
    c.Clip(box);
    c.Text(n.text, D2D1::RectF(box.left, box.top - scroll, box.right, box.top - scroll + 10000), size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    caret.top -= scroll;
    caret.bottom -= scroll;
    if (n.caretOn)
        c.Fill(caret, 0.75f, p.orange);
    c.Unclip();
    hits.push_back({caret, L"caret"});
}

// Reminders: the title and the count still to do, then the list
// in their original order, each with its circle; a last row adds one.
void PaintReminders(Canvas& c, const WidgetContent& r, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    int open = int(std::count_if(r.items.begin(), r.items.end(), [](const Reminder& i) { return !i.done; }));
    c.Text(L"提醒事项", D2D1::RectF(kPad + 2, kPad - 2, W - kPad - 40, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(open), D2D1::RectF(W - kPad - 48, kPad - 6, W - kPad - 2, kPad + 22), 22, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    const float bottom = H - kReminderBottom, view = bottom - kReminderTop;
    const float limit = std::max(0.f, float(r.items.size() + 1) * kReminderRow - view);
    const float scroll = std::clamp(r.scroll, 0.f, limit);
    auto hit = [&](D2D1_RECT_F rect, std::wstring id) {
        rect.top = std::max(rect.top, kReminderTop);
        rect.bottom = std::min(rect.bottom, bottom);
        if (rect.bottom > rect.top)
            hits.push_back({rect, std::move(id)});
    };
    c.Clip(D2D1::RectF(kPad - 4, kReminderTop, W - kPad, bottom));
    int first = int(scroll / kReminderRow);
    int last = std::min(int(r.items.size()), int(std::ceil((scroll + view) / kReminderRow)));
    for (int idx = first; idx < last; ++idx) {
        const Reminder& it = r.items[size_t(idx)];
        float y = kReminderTop + kReminderRow * float(idx) - scroll;
        float cx = kPad + 9, cy = y + kReminderRow / 2;
        if (it.done) {
            c.Circle(cx, cy, 8, p.orange);
            c.Circle(cx, cy, 3.2f, p.onAccent);
        } else {
            c.Arc(cx, cy, 7.4f, -90, 270, p.sub, 1.5f);
        }
        hit(D2D1::RectF(kPad - 4, y, kPad + 22, y + kReminderRow), L"toggle:" + std::to_wstring(idx));
        D2D1_RECT_F text = D2D1::RectF(kPad + 26, y, W - kPad, y + kReminderRow);
        bool editing = r.editing && r.item == idx;
        c.Text(it.text.empty() && !editing ? L"新提醒" : it.text, text, 13.5f, it.done ? p.sub : it.text.empty() ? p.faint : p.text);
        if (it.done && !it.text.empty())
            c.Line(text.left, cy, std::min(text.right, text.left + c.TextWidth(it.text, 13.5f)), cy, p.sub);
        if (editing) {
            // One line: the caret after the text up to it.
            float x = text.left + c.TextWidth(it.text.substr(0, std::min(r.caret, it.text.size())), 13.5f);
            D2D1_RECT_F caret = D2D1::RectF(std::min(x, text.right - 2), cy - 9, std::min(x, text.right - 2) + 1.5f, cy + 9);
            if (r.caretOn)
                c.Fill(caret, 0.75f, p.orange);
            hit(caret, L"caret");
        }
        hit(text, L"item:" + std::to_wstring(idx));
    }
    if (!r.editing) {
        float y = kReminderTop + kReminderRow * float(r.items.size()) - scroll;
        if (y < bottom) {
            c.Text(L"+", D2D1::RectF(kPad - 1, y, kPad + 19, y + kReminderRow), 17, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 1);
            c.Text(r.items.empty() ? L"点按以添加提醒" : L"新提醒", D2D1::RectF(kPad + 26, y, W - kPad, y + kReminderRow), 13.5f, p.faint);
            hit(D2D1::RectF(kPad - 4, y, W - kPad, y + kReminderRow), L"add");
        }
    }
    c.Unclip();
    if (limit > 0) {
        float thumb = std::max(18.f, view * view / (view + limit));
        float y = kReminderTop + (view - thumb) * scroll / limit;
        c.Fill(D2D1::RectF(W - 7, y, W - 5, y + thumb), 1, p.faint);
    }
}

// A photo filling the card; without one, how to choose a folder.
void PaintPhotos(Canvas& c, const WidgetContent& ph, const Pal& p, float W, float H)
{
    if (ph.image && !ph.image->empty()) {
        c.Image(ph.imageKey, *ph.image, D2D1::RectF(0, 0, W, H)); // the widget masks its complete content once
        return;
    }
    c.Glyph(L"\uE91B", D2D1::RectF(0, H / 2 - 46, W, H / 2 - 6), 30, p.sub);
    c.Text(L"照片", D2D1::RectF(0, H / 2, W, H / 2 + 20), 14, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"右键选择照片文件夹", D2D1::RectF(0, H / 2 + 20, W, H / 2 + 38), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
}

// ---- weather -------------------------------------------------------------------

// What the sky does, for the icon and the widget's tint.
enum class Sky { Clear, Partly, Cloudy, Fog, Rain, Snow, Storm };

Sky SkyOf(int code)
{
    if (code <= 1)
        return Sky::Clear;
    if (code == 2)
        return Sky::Partly;
    if (code == 3)
        return Sky::Cloudy;
    if (code == 45 || code == 48)
        return Sky::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return Sky::Snow;
    if (code >= 95)
        return Sky::Storm;
    return Sky::Rain;
}

// The symbols' colours over each ink. White that is not the glass's ink stays white over bright
// glass (InkAt only switches the theme's own ink).
struct IconInk {
    D2D1_COLOR_F sun, moon, cloud, grey, rain, arrow;
};

IconInk IconColors(Ink ink)
{
    switch (ink) {
    case Ink::Dark:
        return {Rgba(0xFFD60A, 1), Rgba(0xF5C451, 1), Rgba(0xA4ADB8, 1), Rgba(0x7D8794, 1), Rgba(0x64D2FF, 1), Rgba(0x6E7681, 1)};
    case Ink::Mono:
        return {Rgba(0xFFFFFF, 0.72f), Rgba(0xFFFFFF, 0.9f), Rgba(0xFFFFFF, 0.96f), Rgba(0xFFFFFF, 0.8f), Rgba(0xFFFFFF, 0.85f), Rgba(0xFFFFFF, 0.96f)};
    default:
        return {Rgba(0xFFD60A, 1), Rgba(0xF2F2F7, 1), Rgba(0xF7F8FA, 1), Rgba(0xD1D5DB, 1), Rgba(0x64D2FF, 1), Rgba(0xF2F5F9, 1)};
    }
}

// A cloud filling (about) a box s wide, its middle at (cx, cy).
void Cloud(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F ink)
{
    c.Fill(D2D1::RectF(cx - 0.42f * s, cy - 0.02f * s, cx + 0.42f * s, cy + 0.26f * s), 0.14f * s, ink);
    c.Circle(cx - 0.17f * s, cy + 0.02f * s, 0.19f * s, ink);
    c.Circle(cx + 0.08f * s, cy - 0.06f * s, 0.25f * s, ink);
    c.Circle(cx + 0.29f * s, cy + 0.09f * s, 0.15f * s, ink);
}

void Sun(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F gold)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    c.Circle(cx, cy, 0.2f * s, gold);
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 45.f * kDeg;
        c.Capsule(cx + std::cos(a) * 0.31f * s, cy + std::sin(a) * 0.31f * s, cx + std::cos(a) * 0.43f * s, cy + std::sin(a) * 0.43f * s, gold,
                  std::max(1.f, 0.075f * s));
    }
}

void Moon(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F ink)
{
    c.Crescent(cx, cy, 0.3f * s, 0.17f * s, -0.12f * s, 0.25f * s, ink);
}

// macOS's weather symbols in their colours: a yellow sun, a pale moon, white clouds, blue rain
// (IconColors: grey clouds over bright glass, all white on the dimmed glass).
// event: 1 a sunrise, 2 a sunset (a half sun on the horizon, an arrow up or down).
void WeatherIcon(Canvas& c, int code, bool day, float cx, float cy, float s, int event, Ink ink)
{
    const IconInk k = IconColors(ink);
    if (event) {
        c.Clip(D2D1::RectF(cx - s, cy - s, cx + s, cy + 0.12f * s));
        Sun(c, cx, cy + 0.12f * s, s * 0.9f, k.sun);
        c.Unclip();
        c.Capsule(cx - 0.42f * s, cy + 0.2f * s, cx + 0.42f * s, cy + 0.2f * s, k.sun, std::max(1.f, 0.07f * s));
        const float y0 = cy - 0.46f * s, y1 = cy - 0.2f * s, w = std::max(1.f, 0.07f * s);
        c.Capsule(cx, y0, cx, y1, k.arrow, w);
        float tip = event == 1 ? y0 : y1, back = event == 1 ? y0 + 0.1f * s : y1 - 0.1f * s;
        c.Capsule(cx - 0.1f * s, back, cx, tip, k.arrow, w);
        c.Capsule(cx + 0.1f * s, back, cx, tip, k.arrow, w);
        return;
    }
    switch (SkyOf(code)) {
    case Sky::Clear:
        day ? Sun(c, cx, cy, s, k.sun) : Moon(c, cx, cy, s, k.moon);
        break;
    case Sky::Partly:
        day ? Sun(c, cx - 0.16f * s, cy - 0.14f * s, s * 0.78f, k.sun) : Moon(c, cx - 0.14f * s, cy - 0.14f * s, s * 0.8f, k.moon);
        Cloud(c, cx + 0.08f * s, cy + 0.06f * s, s * 0.82f, k.cloud);
        break;
    case Sky::Cloudy:
        Cloud(c, cx, cy - 0.04f * s, s, k.cloud);
        break;
    case Sky::Fog:
        for (int i = 0; i < 3; ++i) {
            float y = cy - 0.2f * s + float(i) * 0.2f * s, inset = i == 1 ? 0.f : 0.08f * s;
            c.Capsule(cx - 0.4f * s + inset, y, cx + 0.4f * s - inset, y, k.cloud, std::max(1.f, 0.09f * s));
        }
        break;
    case Sky::Rain:
    case Sky::Snow:
    case Sky::Storm: {
        const bool snow = SkyOf(code) == Sky::Snow, storm = SkyOf(code) == Sky::Storm;
        Cloud(c, cx, cy - 0.16f * s, s, storm ? k.grey : k.cloud);
        for (int i = 0; i < 3; ++i) {
            float x = cx - 0.2f * s + float(i) * 0.2f * s, y = cy + 0.24f * s;
            if (snow)
                c.Circle(x, y + (i % 2 ? 0.08f : 0.f) * s, 0.055f * s, k.cloud);
            else if (!storm || i != 1)
                c.Capsule(x + 0.03f * s, y - 0.04f * s, x - 0.04f * s, y + 0.12f * s, k.rain, std::max(1.f, 0.07f * s));
        }
        if (storm) {
            const D2D1_COLOR_F bolt = ink == Ink::Mono ? k.arrow : k.sun;
            const float w = std::max(1.f, 0.08f * s);
            c.Capsule(cx + 0.04f * s, cy + 0.08f * s, cx - 0.06f * s, cy + 0.26f * s, bolt, w);
            c.Capsule(cx - 0.06f * s, cy + 0.26f * s, cx + 0.05f * s, cy + 0.26f * s, bolt, w);
            c.Capsule(cx + 0.05f * s, cy + 0.26f * s, cx - 0.05f * s, cy + 0.44f * s, bolt, w);
        }
        break;
    }
    }
}

// The full-colour weather widget's sky, top and bottom: blue by day, navy by night, greyer
// under clouds, rain and fog (as macOS's Weather).
std::pair<uint32_t, uint32_t> SkyFace(const Weather& w)
{
    if (!w.ok)
        return {0x2F7BD6, 0x63A9EA};
    switch (SkyOf(w.code)) {
    case Sky::Clear:
    case Sky::Partly:
        return w.day ? std::pair<uint32_t, uint32_t>{0x2F7BD6, 0x63A9EA} : std::pair<uint32_t, uint32_t>{0x0C1631, 0x2A3E68};
    case Sky::Cloudy:
    case Sky::Fog:
        return w.day ? std::pair<uint32_t, uint32_t>{0x6A7E92, 0x98A8B8} : std::pair<uint32_t, uint32_t>{0x262F3C, 0x47525F};
    case Sky::Snow:
        return w.day ? std::pair<uint32_t, uint32_t>{0x7F95AC, 0xAFC0D0} : std::pair<uint32_t, uint32_t>{0x2B3646, 0x4C5868};
    default: // rain, storms
        return w.day ? std::pair<uint32_t, uint32_t>{0x4A5D70, 0x728699} : std::pair<uint32_t, uint32_t>{0x1D2530, 0x3A4552};
    }
}

std::wstring Degrees(float t) { return std::to_wstring(int(std::lround(t))) + L"°"; }

// A day's span of the week's temperatures: blue when cold, through green and yellow, orange when hot.
D2D1_COLOR_F TempColor(float t)
{
    struct Stop {
        float t;
        uint32_t rgb;
    };
    const Stop stops[] = {{-5, 0x5AC8FA}, {8, 0x63E6BE}, {18, 0xFFD60A}, {28, 0xFF9F0A}, {36, 0xFF453A}};
    if (t <= stops[0].t)
        return Rgba(stops[0].rgb, 1);
    for (size_t i = 1; i < std::size(stops); ++i)
        if (t <= stops[i].t) {
            float k = (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
            D2D1_COLOR_F a = Rgba(stops[i - 1].rgb, 1), b = Rgba(stops[i].rgb, 1);
            return D2D1::ColorF(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1);
        }
    return Rgba(stops[std::size(stops) - 1].rgb, 1);
}

// macOS's weather widget: the place, the temperature large, the sky and the day's high and low;
// the medium one adds the next hours, the large one the next days with their spans.
void PaintWeather(Canvas& c, WidgetSize size, const WidgetWorld& world, const Pal& p, float W, float H)
{
    const Weather& w = world.weather;
    const D2D1_COLOR_F text = p.text, sub = p.sub, faint = p.track;
    const Ink ink = p.icon;
    const bool dark = ink == Ink::Dark;
    const float pad = 16;
    const std::wstring city = w.city.empty() ? (world.city.empty() ? L"天气" : world.city) : w.city;
    c.Text(city, D2D1::RectF(pad, 12, W - pad - (size == WidgetSize::Small ? 0 : 120), 32), 14.5f, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!w.ok) {
        c.Text(w.error.empty() ? L"正在获取天气…" : w.error, D2D1::RectF(pad, H / 2 - 10, W - pad, H / 2 + 10), 13, sub,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, size == WidgetSize::Small ? 0 : 1);
        return;
    }
    c.Text(Degrees(w.temp), D2D1::RectF(pad - 2, 30, W / 2 + 40, 82), 44, text, DWRITE_FONT_WEIGHT_LIGHT);
    const std::wstring range = L"最高 " + Degrees(w.hi) + L"  最低 " + Degrees(w.lo);
    if (size == WidgetSize::Small) {
        WeatherIcon(c, w.code, w.day, pad + 11, H - 62, 22, 0, ink);
        c.Text(WeatherCondition(w.code), D2D1::RectF(pad, H - 48, W - pad, H - 30), 13, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(range, D2D1::RectF(pad, H - 30, W - pad, H - 12), 12, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        return;
    }
    // The sky and the day's span at the top right.
    WeatherIcon(c, w.code, w.day, W - pad - 10, 24, 20, 0, ink);
    c.Text(WeatherCondition(w.code), D2D1::RectF(W / 2, 40, W - pad, 58), 13, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    c.Text(range, D2D1::RectF(W / 2 - 20, 58, W - pad, 76), 12.5f, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    // The next hours.
    const float hy = size == WidgetSize::Large ? 92.f : 90.f, colW = (W - 2 * pad) / 6;
    if (size == WidgetSize::Large)
        c.Line(pad, hy - 6, W - pad, hy - 6, faint);
    for (size_t i = 0; i < w.hours.size() && i < 6; ++i) {
        const WeatherHour& h = w.hours[i];
        float x0 = pad + colW * float(i), cx = x0 + colW / 2;
        c.Text(h.label, D2D1::RectF(x0 - 4, hy, x0 + colW + 4, hy + 16), 11.5f, sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        WeatherIcon(c, h.code, h.day, cx, hy + 31, 22, h.event, ink);
        c.Text(h.event ? (h.event == 1 ? L"日出" : L"日落") : Degrees(h.temp), D2D1::RectF(x0 - 4, hy + 46, x0 + colW + 4, hy + 64),
               h.event ? 12.f : 13.5f, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    if (size != WidgetSize::Large)
        return;
    // The next days: each one's low, its span on the week's scale, its high.
    const float top = hy + 76;
    c.Line(pad, top - 6, W - pad, top - 6, faint);
    const size_t n = std::min<size_t>(w.days.size(), 5);
    float weekLo = 1e9f, weekHi = -1e9f;
    for (size_t i = 0; i < n; ++i)
        weekLo = std::min(weekLo, w.days[i].lo), weekHi = std::max(weekHi, w.days[i].hi);
    const float rowH = (H - 12 - top) / float(std::max<size_t>(n, 1));
    const float barL = 170, barR = W - pad - 44, span = std::max(1.f, weekHi - weekLo);
    for (size_t i = 0; i < n; ++i) {
        const WeatherDay& d = w.days[i];
        float y = top + rowH * float(i), cy = y + rowH / 2;
        c.Text(d.label, D2D1::RectF(pad, y, pad + 60, y + rowH), 14, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        WeatherIcon(c, d.code, true, pad + 84, cy, 22, 0, ink);
        c.Text(Degrees(d.lo), D2D1::RectF(barL - 46, y, barL - 8, y + rowH), 14, sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
        c.Fill(D2D1::RectF(barL, cy - 2.5f, barR, cy + 2.5f), 2.5f, faint);
        float x0 = barL + (barR - barL) * (d.lo - weekLo) / span, x1 = barL + (barR - barL) * (d.hi - weekLo) / span;
        x1 = std::max(x1, x0 + 5);
        if (ink == Ink::Mono)
            c.Fill(D2D1::RectF(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, text);
        else
            c.Gradient(D2D1::RectF(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, TempColor(d.lo), TempColor(d.hi), false);
        if (i == 0) {
            // Today: where it is now on the span.
            float x = barL + (barR - barL) * (std::clamp(w.temp, d.lo, d.hi) - weekLo) / span;
            c.Circle(x, cy, 4.5f, dark ? Rgba(0xFFFFFF, 1) : Rgba(0x1C2333, 0.55f));
            c.Circle(x, cy, 3.2f, dark ? Rgba(0x1C2333, 0.9f) : Rgba(0xFFFFFF, 1));
        }
        c.Text(Degrees(d.hi), D2D1::RectF(barR + 6, y, W - pad, y + rowH), 14, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

std::wstring Hhmm(int mins)
{
    mins = std::clamp(mins, 0, 24 * 60 - 1);
    wchar_t b[8];
    swprintf_s(b, L"%02d:%02d", mins / 60, mins % 60);
    return b;
}

int MondayOf(const SYSTEMTIME& now) { return (int(now.wDayOfWeek) + 6) % 7; }

int DaysUntil(const WidgetContent::Mark& m, const SYSTEMTIME& now)
{
    auto pack = [](int y, int mo, int d, FILETIME& f) {
        SYSTEMTIME s{};
        s.wYear = WORD(y);
        s.wMonth = WORD(mo);
        s.wDay = WORD(d);
        return SystemTimeToFileTime(&s, &f) != FALSE;
    };
    FILETIME today{};
    if (!pack(now.wYear, now.wMonth, now.wDay, today))
        return 0;
    int y = m.annual ? int(now.wYear) : m.year;
    if (y < 1601)
        y = int(now.wYear);
    int month = std::clamp(m.month, 1, 12);
    int day = std::clamp(m.day, 1, 31);
    FILETIME when{};
    if (!pack(y, month, day, when)) {
        day = std::min(day, 28);
        if (!pack(y, month, day, when))
            return 0;
    }
    if (m.annual && CompareFileTime(&when, &today) < 0) {
        ++y;
        if (!pack(y, month, day, when) && !pack(y, month, std::min(day, 28), when))
            return 0;
    }
    ULARGE_INTEGER a{}, b{};
    a.LowPart = when.dwLowDateTime;
    a.HighPart = when.dwHighDateTime;
    b.LowPart = today.dwLowDateTime;
    b.HighPart = today.dwHighDateTime;
    return int((long long)(a.QuadPart - b.QuadPart) / 864000000000LL);
}

void PaintCaretLine(Canvas& c, const WidgetContent& content, const Pal& p, D2D1_RECT_F text, const std::wstring& s, float size,
                    std::vector<WidgetHit>& hits)
{
    float x = text.left + c.TextWidth(s.substr(0, std::min(content.caret, s.size())), size);
    float cy = (text.top + text.bottom) / 2;
    D2D1_RECT_F caret = D2D1::RectF(std::min(x, text.right - 2), cy - 8, std::min(x, text.right - 2) + 1.5f, cy + 8);
    if (content.caretOn)
        c.Fill(caret, 0.75f, p.blue);
    hits.push_back({caret, L"caret"});
}

void PaintSchedule(Canvas& c, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    const wchar_t* names[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
    int today = MondayOf(w.now);
    int day = content.focusDay >= 1 && content.focusDay <= 7 ? content.focusDay - 1 : today;
    c.Text(L"课表", D2D1::RectF(kPad, kPad - 2, 70, kPad + 18), 13.5f, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float chip = 22, gap = 4;
    float chips = 7 * chip + 6 * gap;
    float x = W - kPad - chips;
    for (int i = 0; i < 7; ++i) {
        float cx = x + i * (chip + gap);
        if (i == day)
            c.Fill(D2D1::RectF(cx, kPad - 2, cx + chip, kPad + 18), 8, i == today ? p.blue : Alpha(p.blue, 0.35f));
        c.Text(names[i], D2D1::RectF(cx, kPad - 2, cx + chip, kPad + 18), 11, i == day ? p.onAccent : p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        hits.push_back({D2D1::RectF(cx, kPad - 4, cx + chip, kPad + 20), L"wd:" + std::to_wstring(i + 1)});
    }
    std::vector<int> rows;
    for (int i = 0; i < int(content.lessons.size()); ++i)
        if (content.lessons[size_t(i)].weekday == day)
            rows.push_back(i);
    std::sort(rows.begin(), rows.end(), [&](int a, int b) { return content.lessons[size_t(a)].startMin < content.lessons[size_t(b)].startMin; });
    const float top = kPad + 26, rowH = 26, bottom = H - 8;
    if (rows.empty())
        c.Text(L"这天没有课", D2D1::RectF(kPad, top + 8, W - kPad, top + 32), 13, p.faint);
    int slots = std::max(1, int((bottom - top - rowH) / rowH));
    int shown = 0;
    for (int idx : rows) {
        if (shown >= slots)
            break;
        const auto& lesson = content.lessons[size_t(idx)];
        float y = top + rowH * shown;
        std::wstring when = Hhmm(lesson.startMin) + L"–" + Hhmm(lesson.endMin);
        c.Text(when, D2D1::RectF(kPad, y, kPad + 92, y + rowH), 12, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 0);
        bool editing = content.editing && content.item == idx;
        std::wstring name = lesson.name.empty() && !editing ? L"新课程" : lesson.name;
        D2D1_RECT_F text = D2D1::RectF(kPad + 96, y, W - kPad, y + rowH);
        c.Text(name, text, 13.5f, lesson.name.empty() ? p.faint : p.text);
        hits.push_back({text, L"name:" + std::to_wstring(idx)});
        if (editing)
            PaintCaretLine(c, content, p, text, lesson.name, 13.5f, hits);
        ++shown;
    }
    float y = top + rowH * shown;
    if (y + rowH <= bottom + 2) {
        c.Text(L"+ 添加", D2D1::RectF(kPad, y, W - kPad, y + rowH), 13.5f, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hits.push_back({D2D1::RectF(kPad, y, W - kPad, y + rowH), L"add"});
    }
}

void PaintMarks(Canvas& c, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    c.Text(L"纪念日", D2D1::RectF(kPad, kPad - 2, W - kPad, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float top = kPad + 26, rowH = 28, bottom = H - 8;
    if (content.marks.empty())
        c.Text(L"还没有日子", D2D1::RectF(kPad, top + 6, W - kPad, top + 28), 13, p.faint);
    int slots = std::max(1, int((bottom - top - rowH) / rowH));
    int shown = 0;
    for (int i = 0; i < int(content.marks.size()) && shown < slots; ++i, ++shown) {
        const auto& m = content.marks[size_t(i)];
        float y = top + rowH * shown;
        int days = DaysUntil(m, w.now);
        std::wstring when = days == 0 ? L"今天" : days > 0 ? L"还有 " + std::to_wstring(days) + L" 天" : L"已过 " + std::to_wstring(-days) + L" 天";
        bool editing = content.editing && content.item == i;
        std::wstring name = m.name.empty() && !editing ? L"新日子" : m.name;
        D2D1_RECT_F text = D2D1::RectF(kPad, y, W - kPad - 110, y + rowH);
        c.Text(name, text, 13.5f, m.name.empty() ? p.faint : p.text);
        c.Text(when, D2D1::RectF(W - kPad - 108, y, W - kPad, y + rowH), 12.5f, days == 0 ? p.orange : p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
        hits.push_back({D2D1::RectF(kPad, y, W - kPad, y + rowH), L"name:" + std::to_wstring(i)});
        if (editing)
            PaintCaretLine(c, content, p, text, m.name, 13.5f, hits);
    }
    float y = top + rowH * shown;
    if (y + rowH <= bottom + 2) {
        c.Text(L"+ 添加", D2D1::RectF(kPad, y, W - kPad, y + rowH), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hits.push_back({D2D1::RectF(kPad, y, W - kPad, y + rowH), L"add"});
    }
}

void PaintHabits(Canvas& c, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    int open = 0;
    for (const auto& h : content.habits)
        if (!(h.year == int(w.now.wYear) && h.month == int(w.now.wMonth) && h.day == int(w.now.wDay)))
            ++open;
    c.Text(L"习惯", D2D1::RectF(kPad, kPad - 2, W - kPad - 40, kPad + 18), 13.5f, p.green, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(open), D2D1::RectF(W - kPad - 40, kPad - 4, W - kPad, kPad + 20), 18, p.green, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    const float top = kPad + 26, rowH = 28, bottom = H - 8;
    if (content.habits.empty())
        c.Text(L"还没有习惯", D2D1::RectF(kPad, top + 6, W - kPad, top + 28), 13, p.faint);
    int slots = std::max(1, int((bottom - top - rowH) / rowH));
    int shown = 0;
    for (int i = 0; i < int(content.habits.size()) && shown < slots; ++i, ++shown) {
        const auto& h = content.habits[size_t(i)];
        float y = top + rowH * shown;
        bool done = h.year == int(w.now.wYear) && h.month == int(w.now.wMonth) && h.day == int(w.now.wDay);
        float cx = kPad + 9, cy = y + rowH / 2;
        if (done) {
            c.Circle(cx, cy, 8, p.green);
            c.Glyph(L"\uE73E", D2D1::RectF(cx - 7, cy - 7, cx + 7, cy + 7), 11, p.onAccent);
        } else {
            c.Arc(cx, cy, 7.4f, -90, 270, p.sub, 1.5f);
        }
        hits.push_back({D2D1::RectF(kPad - 4, y, kPad + 24, y + rowH), L"toggle:" + std::to_wstring(i)});
        bool editing = content.editing && content.item == i;
        std::wstring name = h.name.empty() && !editing ? L"新习惯" : h.name;
        D2D1_RECT_F text = D2D1::RectF(kPad + 26, y, W - kPad - 36, y + rowH);
        c.Text(name, text, 13.5f, h.name.empty() ? p.faint : p.text);
        if (h.streak > 0)
            c.Text(std::to_wstring(h.streak), D2D1::RectF(W - kPad - 32, y, W - kPad, y + rowH), 12, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
        hits.push_back({text, L"name:" + std::to_wstring(i)});
        if (editing)
            PaintCaretLine(c, content, p, text, h.name, 13.5f, hits);
    }
    float y = top + rowH * shown;
    if (y + rowH <= bottom + 2) {
        c.Text(L"+ 添加", D2D1::RectF(kPad, y, W - kPad, y + rowH), 13.5f, p.green, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        hits.push_back({D2D1::RectF(kPad, y, W - kPad, y + rowH), L"add"});
    }
}

void PaintFocus(Canvas& c, const WidgetContent& content, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    c.Text(L"番茄钟", D2D1::RectF(kPad, kPad - 2, W - kPad, kPad + 18), 13.5f, p.red, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    int sec = content.pomoRun ? std::max(0, content.pomoLeft) : std::max(1, content.pomoMin) * 60;
    wchar_t clock[8];
    swprintf_s(clock, L"%02d:%02d", sec / 60, sec % 60);
    c.Text(clock, D2D1::RectF(kPad, kPad + 22, W - kPad, H - 46), 42, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    if (content.pomoRun) {
        c.Text(L"进行中", D2D1::RectF(kPad, H - 40, W - kPad, H - 12), 13, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        return;
    }
    float bw = 72, bh = 28;
    D2D1_RECT_F start = D2D1::RectF((W - bw) / 2, H - 44, (W + bw) / 2, H - 44 + bh);
    c.Fill(start, 14, p.red);
    c.Text(L"开始", start, 14, p.onAccent, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    hits.push_back({start, L"start"});
    c.Text(L"−", D2D1::RectF(kPad, H - 44, kPad + 28, H - 16), 18, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"+", D2D1::RectF(W - kPad - 28, H - 44, W - kPad, H - 16), 18, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    hits.push_back({D2D1::RectF(kPad, H - 48, kPad + 36, H - 8), L"shorter"});
    hits.push_back({D2D1::RectF(W - kPad - 36, H - 48, W - kPad, H - 8), L"longer"});
}

// The material's face under the content and the ink over it. Faces cover the largest size: while
// a widget resizes its viewport outgrows the size it lays out.
Pal Face(Canvas& c, WidgetKind kind, WidgetMaterial material, const WidgetWorld& world, float W, float H)
{
    if (material == WidgetMaterial::Glass)
        return GlassPal(c.theme);
    // One ink everywhere: the face, not the desktop, decides it. A freely sized
    // calendar is larger than the grid's large cell, so the face covers that too.
    c.ink = {};
    const D2D1_RECT_F all = D2D1::RectF(0, 0, std::max(W, WidgetWidth(WidgetSize::Large)), std::max(H, WidgetHeight(WidgetSize::Large)));
    if (material == WidgetMaterial::Dusk) {
        c.theme = Theme::For(false);
        c.theme.halo = Rgba(0x000000, 0.5f); // a deeper shadow under the white keeps it legible over bright glass
        c.inkDark = 0;
        c.Fill(all, 0, Rgba(0x000000, 0.22f)); // a light veil: the glass stays clear
        return DuskPal();
    }
    c.halo = false; // an opaque face needs none
    if (kind == WidgetKind::Weather) {
        c.theme = Theme::For(false);
        c.inkDark = 0;
        auto [top, bottom] = SkyFace(world.weather);
        c.Fill(all, 0, Rgba(bottom, 1));
        c.Gradient(D2D1::RectF(0, 0, all.right, H), 0, Rgba(top, 1), Rgba(bottom, 1));
        Pal p = GlassPal(c.theme);
        p.icon = Ink::Light;
        return p;
    }
    c.theme = Theme::For(true);
    c.inkDark = 1;
    c.Fill(all, 0, Rgba(0xFFFFFF, 1));
    return PaperPal();
}

} // namespace

float ReminderScrollMax(const WidgetContent& content, WidgetSize size)
{
    return std::max(0.f, float(content.items.size() + 1) * kReminderRow - (WidgetHeight(size) - kReminderTop - kReminderBottom));
}

bool WidgetSupports(WidgetKind kind, WidgetSize size)
{
    switch (kind) {
    case WidgetKind::Battery:
        return size == WidgetSize::Small;
    case WidgetKind::Bluetooth:
        return size != WidgetSize::Large;
    case WidgetKind::Clock:
    case WidgetKind::Digital:
    case WidgetKind::System:
        return size != WidgetSize::Large;
    default:
        return true;
    }
}

void PaintWidget(Canvas& c, WidgetKind kind, WidgetSize size, WidgetMaterial material, const WidgetContent& content, const WidgetWorld& world,
                 std::vector<WidgetHit>& hits, const std::wstring& hoverDay, float layoutW, float layoutH)
{
    const float W = layoutW > 1.f ? layoutW : WidgetWidth(size);
    const float H = layoutH > 1.f ? layoutH : WidgetHeight(size);
    const Pal p = Face(c, kind, material, world, W, H);
    hits.clear();
    switch (kind) {
    case WidgetKind::Clock:
        PaintClock(c, size, content, world, p, W, H);
        break;
    case WidgetKind::Digital:
        PaintDigital(c, size, content, world, p, W, H);
        break;
    case WidgetKind::Calendar: {
        const bool keepHalo = c.halo;
        c.noteFont = true;
        c.halo = false;
        PaintCalendar(c, size, content, world, p, W, H, hits, hoverDay);
        c.halo = keepHalo;
        c.noteFont = false;
        break;
    }
    case WidgetKind::Battery:
        PaintBattery(c, world, p, W, H);
        break;
    case WidgetKind::System:
        PaintSystem(c, size, world, p, W, H);
        break;
    case WidgetKind::Notes:
        PaintNotes(c, content, p, W, H, hits);
        break;
    case WidgetKind::Reminders:
        PaintReminders(c, content, p, W, H, hits);
        break;
    case WidgetKind::Photos:
        PaintPhotos(c, content, p, W, H);
        break;
    case WidgetKind::Weather:
        PaintWeather(c, size, world, p, W, H);
        break;
    case WidgetKind::Bluetooth:
        PaintBluetooth(c, size, world, p, W, H);
        break;
    case WidgetKind::Schedule:
        PaintSchedule(c, content, world, p, W, H, hits);
        break;
    case WidgetKind::Marks:
        PaintMarks(c, content, world, p, W, H, hits);
        break;
    case WidgetKind::Habits:
        PaintHabits(c, content, world, p, W, H, hits);
        break;
    case WidgetKind::Focus:
        PaintFocus(c, content, p, W, H, hits);
        break;
    }
}

} // namespace yunzhao
