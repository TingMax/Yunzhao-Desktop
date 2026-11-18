#include "account.h"

#include "log.h"
#include "settings.h"

#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <ctime>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace json = winrt::Windows::Data::Json;

namespace atrium {
namespace {

constexpr wchar_t kHost[] = L"yunzhao-desktop.com";

struct Session {
    std::wstring id, email, username, name, role, provider;
    std::wstring access, refresh;
    long long expiresAt = 0;
};

std::wstring DataRoot()
{
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        dir = std::wstring(base) + L"\\Yunzhao";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Utf8ToWide(std::string_view bytes)
{
    if (bytes.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool SafeId(const std::wstring& id)
{
    if (id.size() < 32 || id.size() > 40)
        return false;
    for (wchar_t ch : id)
        if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F') || ch == L'-'))
            return false;
    return true;
}

std::wstring SessionPath() { return DataRoot() + L"\\session.bin"; }

bool Protect(const std::string& plain, std::string& sealed)
{
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Yunzhao", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    sealed.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool Unprotect(const std::string& sealed, std::string& plain)
{
    if (sealed.empty())
        return false;
    DATA_BLOB in{DWORD(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.data()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plain.assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool LoadSession(Session& session)
{
    std::ifstream in(SessionPath(), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string plain;
    if (!Unprotect(ss.str(), plain))
        return false;
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Utf8ToWide(plain), o))
        return false;
    session.id = std::wstring(o.GetNamedString(L"id", L""));
    session.email = std::wstring(o.GetNamedString(L"email", L""));
    session.username = std::wstring(o.GetNamedString(L"username", L""));
    session.name = std::wstring(o.GetNamedString(L"name", L""));
    session.role = std::wstring(o.GetNamedString(L"role", L"editor"));
    session.provider = std::wstring(o.GetNamedString(L"provider", L"password"));
    session.access = std::wstring(o.GetNamedString(L"access_token", L""));
    session.refresh = std::wstring(o.GetNamedString(L"refresh_token", L""));
    session.expiresAt = (long long)o.GetNamedNumber(L"expires_at", 0);
    return SafeId(session.id) && !session.access.empty();
}

bool SaveSession(const Session& session)
{
    json::JsonObject o;
    o.SetNamedValue(L"id", json::JsonValue::CreateStringValue(session.id));
    o.SetNamedValue(L"email", json::JsonValue::CreateStringValue(session.email));
    o.SetNamedValue(L"username", json::JsonValue::CreateStringValue(session.username));
    o.SetNamedValue(L"name", json::JsonValue::CreateStringValue(session.name));
    o.SetNamedValue(L"role", json::JsonValue::CreateStringValue(session.role));
    o.SetNamedValue(L"provider", json::JsonValue::CreateStringValue(session.provider));
    o.SetNamedValue(L"access_token", json::JsonValue::CreateStringValue(session.access));
    o.SetNamedValue(L"refresh_token", json::JsonValue::CreateStringValue(session.refresh));
    o.SetNamedValue(L"expires_at", json::JsonValue::CreateNumberValue(double(session.expiresAt)));
    std::string sealed;
    if (!Protect(WideToUtf8(o.Stringify()), sealed))
        return false;
    std::wstring tmp = SessionPath() + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(sealed.data(), std::streamsize(sealed.size()));
        out.flush();
        if (!out)
            return false;
    }
    return MoveFileExW(tmp.c_str(), SessionPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

struct HttpResult {
    int status = 0;
    std::string body;
};

HttpResult Request(const wchar_t* method, const std::wstring& path, const std::string& body, const std::wstring& token)
{
    HttpResult result;
    HINTERNET session = WinHttpOpen(L"Yunzhao", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return result;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!token.empty())
        headers += L"Authorization: Bearer " + token + L"\r\n";
    BOOL sent = request && WinHttpSendRequest(request, headers.c_str(), DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), DWORD(body.size()), DWORD(body.size()), 0);
    if (sent && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        result.status = int(status);
        for (;;) {
            DWORD n = 0;
            if (!WinHttpQueryDataAvailable(request, &n) || n == 0)
                break;
            std::string chunk(n, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), n, &read) || read == 0)
                break;
            chunk.resize(read);
            result.body += chunk;
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

bool FillSession(const json::JsonObject& data, const std::wstring& provider, Session& session)
{
    if (!data.HasKey(L"user") || !data.HasKey(L"tokens"))
        return false;
    json::JsonObject user = data.GetNamedObject(L"user");
    json::JsonObject tokens = data.GetNamedObject(L"tokens");
    session.id = std::wstring(user.GetNamedString(L"id", L""));
    session.email = std::wstring(user.GetNamedString(L"email", L""));
    session.username = std::wstring(user.GetNamedString(L"username", L""));
    session.name = std::wstring(user.GetNamedString(L"name", session.username));
    session.role = std::wstring(user.GetNamedString(L"role", L"editor"));
    session.provider = provider;
    session.access = std::wstring(tokens.GetNamedString(L"access_token", L""));
    session.refresh = std::wstring(tokens.GetNamedString(L"refresh_token", session.refresh));
    double expires = tokens.GetNamedNumber(L"expires_in", 86400);
    session.expiresAt = (long long)time(nullptr) + (long long)expires;
    return SafeId(session.id) && !session.access.empty();
}

std::wstring ErrorText(const HttpResult& http, const wchar_t* fallback)
{
    if (http.status == 0)
        return L"登录服务器还没有连上";
    json::JsonObject o{nullptr};
    if (json::JsonObject::TryParse(Utf8ToWide(http.body), o)) {
        std::wstring detail = std::wstring(o.GetNamedString(L"detail", L""));
        if (!detail.empty())
            return detail;
        std::wstring message = std::wstring(o.GetNamedString(L"message", L""));
        if (!message.empty())
            return message;
    }
    return fallback;
}

void Bind(const Session& session)
{
    WriteTextFile(DataRoot() + L"\\account.json", L"{\"id\":\"" + session.id + L"\"}");
    SetEnvironmentVariableW(L"YUNZHAO_ACCOUNT_ID", session.id.c_str());
    AccountDataDir();
}

void AdoptLocalFiles()
{
    std::wstring root = DataRoot();
    std::wstring home = ReadTextFile(root + L"\\account-home.json");
    std::wstring id;
    json::JsonObject o{nullptr};
    if (json::JsonObject::TryParse(home, o))
        id = std::wstring(o.GetNamedString(L"id", L""));
    std::wstring mine = ReadTextFile(root + L"\\account.json");
    json::JsonObject account{nullptr};
    std::wstring current;
    if (json::JsonObject::TryParse(mine, account))
        current = std::wstring(account.GetNamedString(L"id", L""));
    if (!SafeId(current))
        return;
    const bool standIn = id == L"a1000000-0000-4000-8000-000000000001";
    if (standIn)
        id.clear();
    if (SafeId(id) && id != current)
        return;
    if (!SafeId(id))
        WriteTextFile(root + L"\\account-home.json", L"{\"id\":\"" + current + L"\"}");
    std::wstring dir = AccountDataDir();
    auto take = [&](const std::wstring& fromDir, const wchar_t* name) {
        std::wstring from = fromDir + L"\\" + name;
        std::wstring to = dir + L"\\" + name;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
            return;
        if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES)
            return;
        CopyFileW(from.c_str(), to.c_str(), TRUE);
    };
    take(root, L"widgets.json");
    take(root, L"todos.json");
    std::wstring standDir = root + L"\\users\\a1000000-0000-4000-8000-000000000001";
    take(standDir, L"widgets.json");
    take(standDir, L"todos.json");
}

bool Refresh(Session& session);
HttpResult Authed(const wchar_t* method, const std::wstring& path, const std::string& body, Session& session);

void PullLayout(bool force = false)
{
    Session session;
    if (!LoadSession(session))
        return;
    std::wstring dest = AccountDataDir() + L"\\widgets.json";
    if (!force && GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    HttpResult http = Authed(L"GET", L"/items?kind=widgets&limit=1", {}, session);
    if (http.status != 200)
        return;
    json::JsonObject root{nullptr};
    if (!json::JsonObject::TryParse(Utf8ToWide(http.body), root))
        return;
    if (!root.HasKey(L"items"))
        return;
    json::JsonArray items = root.GetNamedArray(L"items");
    if (items.Size() == 0)
        return;
    json::JsonObject row = items.GetObjectAt(0);
    if (!row.HasKey(L"payload"))
        return;
    json::JsonObject payload = row.GetNamedObject(L"payload");
    WriteTextFile(dest, std::wstring(payload.Stringify()));
    double id = row.GetNamedNumber(L"id", 0);
    if (id > 0)
        WriteTextFile(AccountDataDir() + L"\\sync.json", L"{\"widgets\":" + std::to_wstring((long long)id) + L"}");
}

bool Refresh(Session& session)
{
    json::JsonObject body;
    body.SetNamedValue(L"refresh_token", json::JsonValue::CreateStringValue(session.refresh));
    HttpResult http = Request(L"POST", L"/api/auth/refresh", WideToUtf8(body.Stringify()), {});
    if (http.status == 0)
        return !session.access.empty();
    if (http.status != 200)
        return false;
    json::JsonObject root{nullptr};
    if (!json::JsonObject::TryParse(Utf8ToWide(http.body), root) || !root.GetNamedBoolean(L"success", false))
        return false;
    if (!root.HasKey(L"data"))
        return false;
    json::JsonObject data = root.GetNamedObject(L"data");
    if (!data.HasKey(L"tokens"))
        return false;
    json::JsonObject tokens = data.GetNamedObject(L"tokens");
    std::wstring access = std::wstring(tokens.GetNamedString(L"access_token", L""));
    if (access.empty())
        return false;
    session.access = access;
    std::wstring refresh = std::wstring(tokens.GetNamedString(L"refresh_token", L""));
    if (!refresh.empty())
        session.refresh = refresh;
    session.expiresAt = (long long)time(nullptr) + (long long)tokens.GetNamedNumber(L"expires_in", 86400);
    return SaveSession(session);
}

bool StandIn(const Session& session)
{
    return session.access == L"local-admin" || session.refresh == L"local-admin" || session.id == L"a1000000-0000-4000-8000-000000000001";
}

bool Usable(Session& session)
{
    if (StandIn(session) || !SafeId(session.id) || session.access.empty() || session.refresh.empty())
        return false;
    HttpResult me = Request(L"GET", L"/api/auth/me", {}, session.access);
    if (me.status == 200)
        return true;
    if (me.status == 401 && Refresh(session)) {
        me = Request(L"GET", L"/api/auth/me", {}, session.access);
        return me.status == 200;
    }
    return me.status == 0 && session.expiresAt > (long long)time(nullptr);
}

HttpResult Authed(const wchar_t* method, const std::wstring& path, const std::string& body, Session& session)
{
    HttpResult http = Request(method, path, body, session.access);
    if (http.status != 401)
        return http;
    if (!Refresh(session))
        return http;
    return Request(method, path, body, session.access);
}

bool StrongPassword(const std::wstring& password)
{
    if (password.size() < 8)
        return false;
    bool letter = false;
    bool digit = false;
    for (wchar_t c : password) {
        if (iswalpha(c))
            letter = true;
        if (iswdigit(c))
            digit = true;
    }
    return letter && digit;
}

std::wstring SignPassword(const std::wstring& email, const std::wstring& username, const std::wstring& password, bool create, Session& session)
{
    if (create && !StrongPassword(password))
        return L"密码至少 8 位，并且同时包含字母和数字";
    json::JsonObject body;
    body.SetNamedValue(L"password", json::JsonValue::CreateStringValue(password));
    if (create) {
        body.SetNamedValue(L"email", json::JsonValue::CreateStringValue(email));
        body.SetNamedValue(L"username", json::JsonValue::CreateStringValue(username));
        body.SetNamedValue(L"name", json::JsonValue::CreateStringValue(username.empty() ? email : username));
    } else if (!email.empty()) {
        body.SetNamedValue(L"email", json::JsonValue::CreateStringValue(email));
    } else {
        body.SetNamedValue(L"username", json::JsonValue::CreateStringValue(username));
    }
    const wchar_t* path = create ? L"/api/auth/register" : L"/api/auth/login";
    HttpResult http = Request(L"POST", path, WideToUtf8(body.Stringify()), {});
    int expect = create ? 201 : 200;
    json::JsonObject root{nullptr};
    if (http.status == 401)
        return L"账号或密码不正确";
    if (http.status == 409)
        return L"邮箱或用户名已经存在";
    if (http.status == 422)
        return L"填写的内容不符合要求";
    if (http.status != expect || !json::JsonObject::TryParse(Utf8ToWide(http.body), root) || !root.GetNamedBoolean(L"success", false) || !root.HasKey(L"data"))
        return ErrorText(http, create ? L"注册没有成功" : L"登录没有成功");
    if (!FillSession(root.GetNamedObject(L"data"), L"password", session) || !SaveSession(session))
        return L"登录结果无法保存";
    return {};
}

std::wstring StartProvider(const wchar_t* provider)
{
    std::wstring path = L"/api/auth/providers/";
    path += provider;
    path += L"/start";
    HttpResult http = Request(L"POST", path, "{}", {});
    if (http.status == 404 || http.status == 501 || http.status == 0)
        return L"该登录方式尚未开通";
    json::JsonObject root{nullptr};
    if (!json::JsonObject::TryParse(Utf8ToWide(http.body), root))
        return L"该登录方式尚未开通";
    json::JsonObject data = root.HasKey(L"data") ? root.GetNamedObject(L"data") : json::JsonObject();
    std::wstring message = std::wstring(data.GetNamedString(L"message", L""));
    std::wstring url = std::wstring(data.GetNamedString(L"qr_url", L""));
    if (!message.empty() && !url.empty())
        return message + L" " + url;
    if (!message.empty())
        return message;
    if (!url.empty())
        return url;
    return ErrorText(http, L"该登录方式尚未开通");
}

struct Form {
    HWND window{};
    HWND email{};
    HWND user{};
    HWND pass{};
    HWND status{};
    HFONT titleFont{};
    HFONT bodyFont{};
    HBRUSH page{};
    HBRUSH field{};
    bool done = false;
    bool ok = false;
};

std::wstring Field(HWND edit)
{
    wchar_t text[256]{};
    GetWindowTextW(edit, text, 256);
    return text;
}

void SetStatus(Form* form, const std::wstring& text)
{
    SetWindowTextW(form->status, text.c_str());
}

void Finish(Form* form, const Session& session)
{
    Bind(session);
    AdoptLocalFiles();
    SetStatus(form, L"正在从服务器拉取数据");
    UpdateWindow(form->window);
    PullLayout(true);
    WriteTextFile(AccountDataDir() + L"\\pull-notes.once", L"1");
    WriteTextFile(AccountDataDir() + L"\\pull-sheet.once", L"1");
    form->ok = true;
    form->done = true;
}

void PaintButton(const DRAWITEMSTRUCT* draw)
{
    RECT box = draw->rcItem;
    bool primary = draw->CtlID == 1004;
    bool quiet = draw->CtlID == 1006 || draw->CtlID == 1007 || draw->CtlID == 1009;
    COLORREF fill = primary ? RGB(10, 132, 255) : RGB(255, 255, 255);
    COLORREF ink = primary ? RGB(255, 255, 255) : quiet ? RGB(110, 110, 115) : RGB(29, 29, 31);
    if (draw->itemState & ODS_SELECTED)
        fill = primary ? RGB(0, 112, 230) : RGB(242, 242, 247);
    HBRUSH page = CreateSolidBrush(RGB(246, 246, 248));
    FillRect(draw->hDC, &box, page);
    DeleteObject(page);
    if (!quiet) {
        HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : RGB(229, 229, 234));
        HBRUSH face = CreateSolidBrush(fill);
        HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
        HGDIOBJ oldBrush = SelectObject(draw->hDC, face);
        RoundRect(draw->hDC, box.left, box.top, box.right - 1, box.bottom - 1, 16, 16);
        SelectObject(draw->hDC, oldBrush);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(face);
        DeleteObject(pen);
    }
    wchar_t text[64]{};
    GetWindowTextW(draw->hwndItem, text, 64);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ink);
    HFONT font = (HFONT)SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    DrawTextW(draw->hDC, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont)
        SelectObject(draw->hDC, oldFont);
}

void OnCommand(Form* form, int id)
{
    if (id == 1009) {
        form->done = true;
        return;
    }
    if (id == 1006) {
        SetStatus(form, StartProvider(L"wechat_qr"));
        return;
    }
    if (id == 1007) {
        SetStatus(form, StartProvider(L"wecom_qr"));
        return;
    }
    if (id != 1004 && id != 1005)
        return;
    std::wstring email = Field(form->email);
    std::wstring username = Field(form->user);
    std::wstring password = Field(form->pass);
    if (password.empty() || (username.empty() && email.empty())) {
        SetStatus(form, L"请填写账号和密码");
        return;
    }
    if (id == 1005 && (email.empty() || username.empty())) {
        SetStatus(form, L"注册需要邮箱和用户名");
        return;
    }
    Session session;
    std::wstring error = SignPassword(email, username, password, id == 1005, session);
    if (!error.empty()) {
        SetStatus(form, error);
        return;
    }
    Finish(form, session);
}

LRESULT CALLBACK SignProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Form* form = reinterpret_cast<Form*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, form && form->page ? form->page : (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(29, 29, 31));
        if (form && form->titleFont)
            SelectObject(dc, form->titleFont);
        RECT title{28, 22, client.right - 48, 52};
        DrawTextW(dc, L"Yunzhao", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, RGB(110, 110, 115));
        if (form && form->bodyFont)
            SelectObject(dc, form->bodyFont);
        RECT sub{28, 52, client.right - 28, 76};
        DrawTextW(dc, L"登录后使用日历、课表和多维表格", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(29, 29, 31));
        if (msg == WM_CTLCOLOREDIT)
            return (LRESULT)(form && form->field ? form->field : GetStockObject(WHITE_BRUSH));
        return (LRESULT)(form && form->page ? form->page : GetStockObject(WHITE_BRUSH));
    }
    case WM_DRAWITEM:
        PaintButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < 18)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_COMMAND:
        if (form)
            OnCommand(form, LOWORD(wp));
        return 0;
    case WM_CLOSE:
        if (form)
            form->done = true;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND Add(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    DWORD extra = 0;
    if (cls == std::wstring_view(L"BUTTON"))
        extra = BS_OWNERDRAW;
    if (cls == std::wstring_view(L"EDIT"))
        extra = WS_BORDER;
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style | extra,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

bool ShowForm(HINSTANCE instance)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = SignProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"YunzhaoSignIn";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    Form form;
    form.page = CreateSolidBrush(RGB(246, 246, 248));
    form.field = CreateSolidBrush(RGB(255, 255, 255));
    form.titleFont = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    form.bodyFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    form.window = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"登录 Yunzhao", WS_POPUP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 400, 460, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(form.window, GWLP_USERDATA, (LONG_PTR)&form);
    int corner = 2;
    DwmSetWindowAttribute(form.window, 33, &corner, sizeof(corner));
    COLORREF border = 0xFFFFFFFE;
    DwmSetWindowAttribute(form.window, 34, &border, sizeof(border));
    auto label = [&](const wchar_t* text, int y) {
        HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, 28, y, 344, 18, form.window, nullptr, instance, nullptr);
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)form.bodyFont, TRUE);
    };
    label(L"邮箱", 84);
    form.email = Add(form.window, L"EDIT", L"", ES_AUTOHSCROLL, 28, 104, 344, 28, 1001);
    label(L"用户名", 140);
    form.user = Add(form.window, L"EDIT", L"", ES_AUTOHSCROLL, 28, 160, 344, 28, 1002);
    label(L"密码", 196);
    form.pass = Add(form.window, L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL, 28, 216, 344, 28, 1003);
    Add(form.window, L"BUTTON", L"登录", 0, 28, 260, 344, 34, 1004);
    Add(form.window, L"BUTTON", L"注册", 0, 28, 302, 344, 34, 1005);
    Add(form.window, L"BUTTON", L"微信扫码", 0, 28, 344, 160, 28, 1006);
    Add(form.window, L"BUTTON", L"企业微信扫码", 0, 212, 344, 160, 28, 1007);
    form.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 28, 380, 344, 36, form.window, (HMENU)1008, instance, nullptr);
    Add(form.window, L"BUTTON", L"×", 0, 360, 12, 28, 28, 1009);
    EnumChildWindows(form.window, [](HWND child, LPARAM lp) { SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE; }, (LPARAM)form.bodyFont);
    RECT desk{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desk, 0);
    SetWindowPos(form.window, HWND_TOP, desk.left + ((desk.right - desk.left) - 400) / 2, desk.top + ((desk.bottom - desk.top) - 460) / 2,
                 400, 460, SWP_SHOWWINDOW);
    MSG msg;
    while (!form.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(form.window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(form.window);
    DeleteObject(form.titleFont);
    DeleteObject(form.bodyFont);
    DeleteObject(form.page);
    DeleteObject(form.field);
    return form.ok;
}

} // namespace

bool AccountEnsure(HINSTANCE instance)
{
    Session session;
    if (LoadSession(session) && Usable(session)) {
        Bind(session);
        AdoptLocalFiles();
        PullLayout();
        Log(L"account: %s", session.username.c_str());
        return true;
    }
    return ShowForm(instance);
}

void AccountSignOut()
{
    Session session;
    if (LoadSession(session) && !session.access.empty()) {
        std::wstring file = AccountDataDir() + L"\\widgets.json";
        std::wstring text = ReadTextFile(file);
        if (!text.empty() && text[0] == L'{') {
            std::string payload = WideToUtf8(text);
            std::wstring mapText = ReadTextFile(AccountDataDir() + L"\\sync.json");
            json::JsonObject map{nullptr};
            long long id = 0;
            if (json::JsonObject::TryParse(mapText, map))
                id = (long long)map.GetNamedNumber(L"widgets", 0);
            HttpResult http;
            if (id > 0)
                http = Authed(L"PUT", L"/items/" + std::to_wstring(id), std::string("{\"payload\":") + payload + "}", session);
            else
                http = Authed(L"POST", L"/items", std::string("{\"kind\":\"widgets\",\"payload\":") + payload + "}", session);
            json::JsonObject row{nullptr};
            if ((http.status == 200 || http.status == 201) && json::JsonObject::TryParse(Utf8ToWide(http.body), row)) {
                long long saved = (long long)row.GetNamedNumber(L"id", double(id));
                if (saved > 0)
                    WriteTextFile(AccountDataDir() + L"\\sync.json", L"{\"widgets\":" + std::to_wstring(saved) + L"}");
            }
        }
    }
    DeleteFileW(SessionPath().c_str());
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring args = L"/c ping 127.0.0.1 -n 3 >nul & start \"\" \"" + std::wstring(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), nullptr, SW_HIDE);
    ExitProcess(0);
}

void AccountPublishLayout()
{
    std::thread([] {
        Session session;
        if (!LoadSession(session) || session.access.empty())
            return;
        std::wstring file = AccountDataDir() + L"\\widgets.json";
        std::wstring text = ReadTextFile(file);
        if (text.empty() || text[0] != L'{')
            return;
        std::string payload = WideToUtf8(text);
        std::wstring mapText = ReadTextFile(AccountDataDir() + L"\\sync.json");
        json::JsonObject map{nullptr};
        long long id = 0;
        if (json::JsonObject::TryParse(mapText, map))
            id = (long long)map.GetNamedNumber(L"widgets", 0);
        HttpResult http;
        if (id > 0)
            http = Authed(L"PUT", L"/items/" + std::to_wstring(id), std::string("{\"payload\":") + payload + "}", session);
        else
            http = Authed(L"POST", L"/items", std::string("{\"kind\":\"widgets\",\"payload\":") + payload + "}", session);
        if (http.status != 200 && http.status != 201)
            return;
        json::JsonObject row{nullptr};
        if (!json::JsonObject::TryParse(Utf8ToWide(http.body), row))
            return;
        long long saved = (long long)row.GetNamedNumber(L"id", double(id));
        if (saved > 0)
            WriteTextFile(AccountDataDir() + L"\\sync.json", L"{\"widgets\":" + std::to_wstring(saved) + L"}");
    }).detach();
}

} // namespace atrium
