#include "shell/wechat_source.h"

#include "log.h"
#include "shell/uia_client.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uiautomation.h>

#include <winrt/base.h>

#include <map>
#include <vector>

namespace atrium {

namespace {

struct Row {
    std::wstring chat;    // the chat's name
    std::wstring summary; // the newest message as the list shows it
    int unread = 0;
    bool muted = false;
};

bool IsTime(const std::wstring& s)
{
    // "17:06", "10/27", "2025/10/27", "昨天", "星期三"
    if (s.empty() || s.size() > 12)
        return false;
    if (s == L"昨天" || s.rfind(L"星期", 0) == 0)
        return true;
    for (wchar_t c : s)
        if (!iswdigit(c) && c != L':' && c != L'/' && c != L'-')
            return false;
    return true;
}

std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back()))
        s.pop_back();
    size_t a = s.find_first_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a);
}

// A row's name, line by line: the chat; then "[N条] summary" or "[N条]" and
// the summary on its own line, or only the summary when read; the time; and
// "消息免打扰" for a muted chat.
Row Parse(const std::wstring& name)
{
    std::vector<std::wstring> lines;
    size_t from = 0;
    for (;;) {
        size_t nl = name.find(L'\n', from);
        lines.push_back(Trim(name.substr(from, nl == std::wstring::npos ? std::wstring::npos : nl - from)));
        if (nl == std::wstring::npos)
            break;
        from = nl + 1;
    }
    Row r;
    if (lines.empty())
        return r;
    r.chat = lines[0];
    for (size_t i = 1; i < lines.size(); ++i) {
        std::wstring l = lines[i];
        if (l == L"消息免打扰") {
            r.muted = true;
            continue;
        }
        if (l.size() > 3 && l[0] == L'[' && r.unread == 0 && r.summary.empty()) {
            size_t close = l.find(L"条]");
            if (close != std::wstring::npos && close > 1) {
                r.unread = _wtoi(l.substr(1, close - 1).c_str());
                l = Trim(l.substr(close + 2));
            }
        }
        if (!l.empty() && r.summary.empty() && !IsTime(l))
            r.summary = l;
    }
    return r;
}

// WeChat's program, from one of its processes.
std::wstring ProgramPath(DWORD pid)
{
    std::wstring path;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD len = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(p, 0, buf, &len))
            path = buf;
        CloseHandle(p);
    }
    return path;
}

// WeChat 4 draws its chat list itself and does not expose those rows to UI
// Automation. The session database still grows when a message arrives. The
// newest account's write-ahead log is that signal; the file is not read.
std::wstring NewestSessionWal()
{
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs)
        return {};
    std::wstring root = docs;
    CoTaskMemFree(docs);
    root += L"\\xwechat_files\\wxid_*";
    WIN32_FIND_DATAW fd{};
    HANDLE found = FindFirstFileW(root.c_str(), &fd);
    if (found == INVALID_HANDLE_VALUE)
        return {};
    std::wstring best;
    ULONGLONG bestWrite = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
            continue;
        std::wstring wal = root.substr(0, root.size() - 6) + fd.cFileName + L"\\db_storage\\session\\session.db-wal";
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(wal.c_str(), GetFileExInfoStandard, &attr))
            continue;
        ULONGLONG write = (ULONGLONG(attr.ftLastWriteTime.dwHighDateTime) << 32) | attr.ftLastWriteTime.dwLowDateTime;
        if (best.empty() || write >= bestWrite) {
            best = wal;
            bestWrite = write;
        }
    } while (FindNextFileW(found, &fd));
    FindClose(found);
    return best;
}

// WeChat's top-level Qt windows (the main window is one of them, shown,
// minimised or hidden in the tray).
std::vector<HWND> WeChatWindows(DWORD pid)
{
    struct Ctx {
        DWORD pid;
        std::vector<HWND> out;
    } ctx{pid, {}};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            wchar_t cls[96] = {};
            GetClassNameW(h, cls, 96);
            if (p == c->pid && wcsstr(cls, L"QWindowIcon"))
                c->out.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

} // namespace

WeChatSource::WeChatSource(HWND notify, UINT msg) : notify_(notify), msg_(msg)
{
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
}

WeChatSource::~WeChatSource()
{
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

void WeChatSource::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomationCondition> cells;
    winrt::com_ptr<IUIAutomationCacheRequest> cache;
    winrt::com_ptr<IUIAutomation> uia = CreateUia(stop_, wake_, L"wechat", [&](IUIAutomation* u) {
        cells = nullptr;
        VARIANT v{};
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"mmui::ChatSessionCell");
        HRESULT hr = u->CreatePropertyCondition(UIA_ClassNamePropertyId, v, cells.put());
        VariantClear(&v);
        if (FAILED(hr))
            return hr;
        cache = nullptr;
        winrt::com_ptr<IUIAutomationCondition> raw;
        if (FAILED(hr = u->CreateCacheRequest(cache.put())) || FAILED(hr = cache->AddProperty(UIA_AutomationIdPropertyId)) ||
            FAILED(hr = cache->AddProperty(UIA_NamePropertyId)) || FAILED(hr = cache->put_TreeScope(TreeScope_Element)) ||
            FAILED(hr = u->get_RawViewCondition(raw.put())) || FAILED(hr = cache->put_TreeFilter(raw.get())))
            return hr;
        return cache->put_AutomationElementMode(AutomationElementMode_None);
    });
    std::map<std::wstring, int> unread; // chat row id -> unread count last seen
    bool baseline = false;
    std::wstring wal;
    ULONGLONG walSize = 0;
    bool walBase = false;
    ULONGLONG walNoted = 0;
    int state = -1; // 0 no WeChat, 1 no list, 2 reading (logged on change)
    auto report = [&](int now, const wchar_t* text) {
        if (now != state)
            Log(L"wechat: %s", text);
        state = now;
    };
    std::vector<std::pair<DWORD, std::wstring>> procs;
    ULONGLONG listed = 0;
    {
        UiaWatch watch(uia.get(), wake_);
        UiaWindowWake windows(wake_, L"QWindowIcon");
        while (!stop_ && uia && cells) {
            try {
                std::wstring exe;
                // Process discovery is infrequent; message events only re-read the client tree.
                ULONGLONG tick = GetTickCount64();
                if (!listed || tick - listed >= 10000 || (state != 2 && tick - listed >= 2000)) {
                    procs.clear();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    PROCESSENTRY32W pe{sizeof(pe)};
                    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
                        if (_wcsicmp(pe.szExeFile, L"Weixin.exe") == 0 || _wcsicmp(pe.szExeFile, L"WeChat.exe") == 0)
                            procs.push_back({pe.th32ProcessID, ProgramPath(pe.th32ProcessID)});
                    if (snap != INVALID_HANDLE_VALUE)
                        CloseHandle(snap);
                    listed = tick;
                    windows.SetProcesses(procs);
                }
                watch.Prune();
                std::vector<std::pair<std::wstring, Row>> rows;
                bool found = false;
                for (const auto& [p, program] : procs) {
                    for (HWND h : WeChatWindows(p)) {
                        winrt::com_ptr<IUIAutomationElement> root;
                        winrt::com_ptr<IUIAutomationElementArray> all;
                        if (FAILED(uia->ElementFromHandle(h, root.put())) || !root ||
                            FAILED(root->FindAllBuildCache(TreeScope_Descendants, cells.get(), cache.get(), all.put())) || !all)
                            continue;
                        int n = 0;
                        all->get_Length(&n);
                        for (int i = 0; i < n; ++i) {
                            winrt::com_ptr<IUIAutomationElement> e;
                            all->GetElement(i, e.put());
                            BSTR id = nullptr, name = nullptr;
                            if (!e || FAILED(e->get_CachedAutomationId(&id)) || FAILED(e->get_CachedName(&name)) || !name) {
                                SysFreeString(id);
                                SysFreeString(name);
                                continue;
                            }
                            Row r = Parse(name);
                            std::wstring key = id && *id ? id : r.chat;
                            SysFreeString(id);
                            SysFreeString(name);
                            if (!r.chat.empty())
                                rows.push_back({key, r});
                        }
                        if (n > 0) {
                            exe = program;
                            if (!watch.Contains(h)) {
                                watch.Clear();
                                watch.Add(h, root.get());
                            }
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        break;
                }
                if (procs.empty()) {
                    report(0, L"WeChat is not running");
                    baseline = false; // after a restart its list is a new baseline
                    walBase = false;
                } else if (!found) {
                    report(1, L"WeChat's chat list is not readable; watching its session database");
                    baseline = false;
                    // WeChat 4's window has no chat rows. A growing session log means a new
                    // message. The database stays closed; the card says a message arrived.
                    if (wal.empty())
                        wal = NewestSessionWal();
                    WIN32_FILE_ATTRIBUTE_DATA attr{};
                    if (!wal.empty() && GetFileAttributesExW(wal.c_str(), GetFileExInfoStandard, &attr)) {
                        ULONGLONG sz = (ULONGLONG(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
                        if (!walBase) {
                            walSize = sz;
                            walBase = true;
                        } else if (sz > walSize) {
                            walSize = sz;
                            HWND fg = GetForegroundWindow();
                            DWORD fgPid = 0;
                            if (fg)
                                GetWindowThreadProcessId(fg, &fgPid);
                            bool front = false;
                            for (const auto& proc : procs)
                                if (proc.first == fgPid)
                                    front = true;
                            ULONGLONG noted = GetTickCount64();
                            if (!front && noted - walNoted >= 3000) {
                                walNoted = noted;
                                auto* n = new Notice();
                                n->source = L"wechat";
                                n->app = L"微信";
                                n->title = L"微信";
                                n->body = L"有新消息";
                                if (!procs.empty()) {
                                    n->appId = procs.front().second;
                                    if (!n->appId.empty())
                                        n->action = L"app:" + n->appId;
                                }
                                if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(n)))
                                    delete n;
                                else
                                    Log(L"wechat: new message (session database grew)");
                            }
                        } else {
                            walSize = sz;
                        }
                    }
                } else {
                    if (state != 2)
                        Log(L"wechat: reading WeChat's chat list (%zu chats in view)", rows.size());
                    state = 2;
                    for (size_t i = 0; i < rows.size(); ++i) {
                        const auto& [key, r] = rows[i];
                        auto old = unread.find(key);
                        // A chat not seen before counts only at the top of the list,
                        // where a new message puts it (scrolling brings old ones in).
                        bool rose = old != unread.end() ? r.unread > old->second : r.unread > 0 && i < 3;
                        if (baseline && rose && !r.muted) {
                            auto* n = new Notice();
                            n->source = L"wechat";
                            n->app = L"微信";
                            n->appId = exe;
                            n->title = r.chat;
                            n->body = r.summary;
                            if (!exe.empty())
                                n->action = L"app:" + exe;
                            if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(n)))
                                delete n;
                            else
                                Log(L"wechat: new message (unread %d)", r.unread);
                        }
                        unread[key] = r.unread;
                    }
                    baseline = true;
                }
            } catch (...) {
                Log(L"wechat: reading failed");
            }
            // Providers can omit UIA events. A slow reconciliation pass covers that case.
            if (!stop_)
                WaitForUiaWake(wake_, state == 2 || walBase ? 1500 : 6000);
        }
    }
    cells = nullptr;
    cache = nullptr;
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
