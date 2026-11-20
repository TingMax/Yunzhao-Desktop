#include "shell/update_checker.h"
#include "log.h"
#include "settings.h"
#include "ui/notifications.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winhttp.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include <cwctype>

namespace atrium {
namespace {
namespace json = winrt::Windows::Data::Json;
constexpr int64_t kIntervalSeconds = 12 * 60 * 60;
constexpr wchar_t kHost[] = L"yunzhao-desktop.com";
constexpr wchar_t kManifest[] = L"https://yunzhao-desktop.com/releases/latest.json";
using Version = std::array<unsigned, 4>;

std::optional<Version> ParseVersion(std::wstring_view tag)
{
    if (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V')) tag.remove_prefix(1);
    tag = tag.substr(0, tag.find(L'+'));
    Version version{};
    size_t part = 0;
    bool digit = false;
    for (wchar_t c : tag) {
        if (c == L'.' && digit && part < 3) {
            ++part;
            digit = false;
        } else if (c >= L'0' && c <= L'9') {
            version[part] = version[part] * 10 + (c - L'0');
            if (version[part] > 65535) return {};
            digit = true;
        } else return {};
    }
    return digit && part >= 1 ? std::optional<Version>(version) : std::nullopt;
}

Version CurrentVersion()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD unused = 0;
    std::vector<BYTE> data(GetFileVersionInfoSizeW(exe, &unused));
    VS_FIXEDFILEINFO* info = nullptr;
    UINT size = 0;
    if (data.empty() || !GetFileVersionInfoW(exe, 0, DWORD(data.size()), data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size))
        throw winrt::hresult_error(E_FAIL, L"Missing application version");
    return {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

DWORD Delay(int64_t last, int64_t now)
{
    // A clock correction must not postpone checks indefinitely.
    return last > 0 && last <= now && now - last < kIntervalSeconds ? DWORD((kIntervalSeconds - (now - last)) * 1000) : 0;
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    return directory.substr(0, directory.find_last_of(L'\\'));
}

std::wstring UpdateCache()
{
    using namespace winrt::Windows::Security::Cryptography;
    std::wstring directory = ExeDirectory();
    int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, directory.data(), int(directory.size()), nullptr, 0, nullptr, nullptr, 0);
    std::wstring upper(size_t(count), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, directory.data(), int(directory.size()), upper.data(), count, nullptr, nullptr, 0);
    auto algorithm = Core::HashAlgorithmProvider::OpenAlgorithm(Core::HashAlgorithmNames::Sha256());
    std::wstring key = CryptographicBuffer::EncodeToHexString(algorithm.HashData(
        CryptographicBuffer::ConvertStringToBinary(upper, BinaryStringEncoding::Utf8))).c_str();
    key.resize(16);
    for (wchar_t& ch : key) ch = wchar_t(std::towupper(ch));
    std::wstring root = SettingsPath();
    root = root.substr(0, root.find_last_of(L'\\')) + L"\\Updates";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\" + key;
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

bool NetworkUp()
{
    const HINTERNET session = WinHttpOpen(L"Yunzhao-Desktop/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    const HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return false; }
    const HINTERNET request = WinHttpOpenRequest(connect, L"HEAD", L"/health", nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (request) {
        ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request, nullptr);
        WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

std::wstring SettingsExe()
{
    std::wstring dir = ExeDirectory();
    for (const wchar_t* name : {L"\\Settings\\Atrium.Settings.exe", L"\\Atrium.Settings.exe"}) {
        std::wstring path = dir + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    Log(L"update: settings program was not found next to the desktop");
    return L"";
}

void StartWorker(const std::wstring& arguments)
{
    std::wstring exe = SettingsExe();
    if (exe.empty()) return;
    std::wstring command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else Log(L"update: could not start background queue (%lu)", GetLastError());
}

void DownloadRelease(const json::JsonObject& manifest, const Version& current)
{
    // Official packages only: main channel, the versioned zip, and this host.
    if (!manifest.HasKey(L"channel") || manifest.GetNamedString(L"channel") != L"main") return;
    if (!manifest.HasKey(L"version") || !manifest.HasKey(L"name") || !manifest.HasKey(L"url")) return;
    std::wstring number = manifest.GetNamedString(L"version").c_str();
    auto latest = ParseVersion(number);
    std::wstring name = manifest.GetNamedString(L"name").c_str();
    std::wstring url = manifest.GetNamedString(L"url").c_str();
    if (!latest || *latest <= current || name != L"Yunzhao-" + number + L"-win-x64.zip") return;
    if (url != L"https://yunzhao-desktop.com/releases/" + name) return;
    std::wstring tag = manifest.HasKey(L"tag") ? manifest.GetNamedString(L"tag").c_str() : L"v" + number;
    json::JsonObject payload;
    std::wstring version;
    for (unsigned part : *latest) version += (version.empty() ? L"" : L".") + std::to_wstring(part);
    payload.SetNamedValue(L"Version", json::JsonValue::CreateStringValue(version));
    payload.SetNamedValue(L"Tag", json::JsonValue::CreateStringValue(tag));
    payload.SetNamedValue(L"Page", json::JsonValue::CreateStringValue(
        manifest.HasKey(L"page") ? manifest.GetNamedString(L"page") : winrt::hstring(L"https://yunzhao-desktop.com/")));
    payload.SetNamedValue(L"Download", json::JsonValue::CreateStringValue(url));
    payload.SetNamedValue(L"Digest", manifest.HasKey(L"digest") ? manifest.GetNamedValue(L"digest") : json::JsonValue::CreateNullValue());
    using namespace winrt::Windows::Security::Cryptography;
    StartWorker(L"--queue-update " + std::wstring(CryptographicBuffer::EncodeToBase64String(
        CryptographicBuffer::ConvertStringToBinary(payload.Stringify(), BinaryStringEncoding::Utf8)).c_str()));
    Log(L"update: queued background download for %s", tag.c_str());
}
} // namespace

UpdateChecker::UpdateChecker(HWND notify)
    : notify_(notify), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), cache_(UpdateCache())
{
    if (!stop_) winrt::throw_last_error();
    if (GetFileAttributesW((cache_ + L"\\download.json").c_str()) != INVALID_FILE_ATTRIBUTES)
        StartWorker(L"--resume-update");
    PostMessageW(notify_, WM_APP + 11, 0, 0);
    worker_ = std::thread([this] { Run(); });
}

UpdateChecker::~UpdateChecker()
{
    SetEvent(stop_);
    worker_.join();
    CloseHandle(stop_);
}

void UpdateChecker::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        const Version current = CurrentVersion();
        const std::wstring path = cache_ + L"\\update-state.json";
        json::JsonObject state;
        if (!json::JsonObject::TryParse(ReadTextFile(path), state)) state = json::JsonObject();
        int64_t checked = int64_t(state.GetNamedNumber(L"checked", 0));
        while (WaitForSingleObject(stop_, Delay(checked, Now())) == WAIT_TIMEOUT) {
            checked = Now();
            try {
                if (!NetworkUp()) {
                    StartWorker(L"--close-update");
                } else {
                    winrt::Windows::Web::Http::HttpClient http;
                    http.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Yunzhao-Desktop/1.0");
                    http.DefaultRequestHeaders().Accept().TryParseAdd(L"application/json");
                    auto request = http.GetAsync(winrt::Windows::Foundation::Uri(kManifest));
                    const auto start = GetTickCount64();
                    while (request.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
                        if (WaitForSingleObject(stop_, 100) == WAIT_OBJECT_0 || GetTickCount64() - start >= 15000) {
                            request.Cancel();
                            break;
                        }
                    }
                    if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
                    if (request.Status() != winrt::Windows::Foundation::AsyncStatus::Completed) {
                        StartWorker(L"--close-update");
                    } else {
                        auto response = request.GetResults();
                        const auto code = response.StatusCode();
                        if (code == winrt::Windows::Web::Http::HttpStatusCode::Ok) {
                            auto parsed = json::JsonValue::Parse(response.Content().ReadAsStringAsync().get());
                            if (parsed.ValueType() == json::JsonValueType::Object)
                                DownloadRelease(parsed.GetObject(), current);
                        } else if (code != winrt::Windows::Web::Http::HttpStatusCode::NotFound) {
                            StartWorker(L"--close-update");
                        }
                    }
                }
            } catch (const winrt::hresult_error&) {
                StartWorker(L"--close-update");
            }
            state.SetNamedValue(L"checked", json::JsonValue::CreateNumberValue(double(checked)));
            if (!WriteTextFile(path, state.Stringify().c_str())) Log(L"update: could not save check state");
        }
    } catch (const winrt::hresult_error& error) {
        Log(L"update: checker stopped (0x%08X)", unsigned(error.code().value));
    }
    winrt::uninit_apartment();
}

void UpdateChecker::NotifyPrepared(Notifications& notifications)
{
    // The download stays in the background. No card, no error, no about-page prompt.
    notifications.ClearPreparedUpdate();
}
} // namespace atrium
