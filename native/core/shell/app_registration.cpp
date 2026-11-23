#include "shell/app_registration.h"

#include "log.h"
#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>

#include <functional>
#include <vector>

#include <winrt/base.h>

namespace yunzhao {

bool IsRegisteredApplication(const std::wstring& directory)
{
    wchar_t value[32768]{};
    auto matches = [&](const wchar_t* key) {
        DWORD bytes = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, key, L"InstallLocation", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS &&
               _wcsicmp(value, directory.c_str()) == 0;
    };
    return matches(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Yunzhao") ||
           matches(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Atrium");
}

void RegisterApplication(const std::wstring& directory)
{
    if (IsDevelopmentBuild()) return;
    const std::wstring exe = directory + L"\\Yunzhao.exe";
    const std::wstring uninstall = directory + L"\\Uninstall.ps1";
    if (GetFileAttributesW(uninstall.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((directory + L"\\Settings\\Yunzhao.Settings.exe").c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW((directory + L"\\Settings\\Atrium.Settings.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a native-only development build is not an installed app

    constexpr auto registration = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Yunzhao";
    constexpr auto legacyRegistration = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Atrium";
    const std::pair<const wchar_t*, std::wstring> fields[] = {
        {L"DisplayName", L"Yunzhao"}, {L"Publisher", L"Yunzhao"},
        {L"InstallLocation", directory}, {L"DisplayIcon", L"\"" + exe + L"\",0"},
        {L"UninstallString", L"\"" + exe + L"\" --uninstall"},
        {L"URLInfoAbout", L"https://github.com/TingMax/Yunzhao-Desktop"},
    };
    auto read = [](HKEY key, const wchar_t* name) {
        wchar_t value[1024]{};
        DWORD bytes = sizeof(value);
        return key && RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS
            ? std::wstring(value) : std::wstring();
    };
    HKEY key = nullptr;
    bool changed = RegOpenKeyExW(HKEY_CURRENT_USER, registration, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS;
    std::wstring previousDirectory = read(key, L"InstallLocation");
    if (key) {
        for (const auto& [name, value] : fields) changed |= read(key, name) != value;
        for (const wchar_t* name : {L"NoModify", L"NoRepair"}) {
            DWORD value = 0, bytes = sizeof(value);
            changed |= RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS || value != 1;
        }
        DWORD bytes = 0;
        changed |= RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
        RegCloseKey(key);
        key = nullptr;
    }
    if (previousDirectory.empty()) {
        HKEY legacy = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, legacyRegistration, 0, KEY_QUERY_VALUE, &legacy) == ERROR_SUCCESS) {
            previousDirectory = read(legacy, L"InstallLocation");
            RegCloseKey(legacy);
            changed = true;
        }
    }
    if (changed) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, registration, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            bool ok = true;
            for (const auto& [name, value] : fields)
                ok &= RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                     DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
            RegDeleteValueW(key, L"DisplayVersion");
            const DWORD one = 1;
            for (const wchar_t* name : {L"NoModify", L"NoRepair"})
                ok &= RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS;
            RegCloseKey(key);
            RegDeleteTreeW(HKEY_CURRENT_USER, legacyRegistration);
            Log(ok ? L"app: registered Yunzhao in Installed apps (%s)" : L"app: could not write all application information (%s)", directory.c_str());
        } else {
            Log(L"app: could not register Yunzhao in Installed apps");
        }
    }

    auto fileName = [](const std::wstring& path) {
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    };
    auto ourExe = [&](const std::wstring& path) {
        const std::wstring name = fileName(path);
        return _wcsicmp(name.c_str(), L"Yunzhao.exe") == 0 || _wcsicmp(name.c_str(), L"Atrium.exe") == 0 ||
               _wcsicmp(name.c_str(), L"Atrium.Core.exe") == 0;
    };
    auto parentDir = [](const std::wstring& path) {
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    };
    // Older copies left start-menu shortcuts named Atrium / Atrium.Core. A shortcut that
    // points at another program is left alone.
    PWSTR programs = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs);
    const std::wstring programsDir = programs ? programs : L"";
    CoTaskMemFree(programs);
    std::wstring retired;
    winrt::com_ptr<IShellLinkW> link;
    if (!programsDir.empty() && SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()));
    for (const wchar_t* oldName : {L"Atrium.lnk", L"Atrium.Core.lnk"}) {
        if (!link)
            break;
        const std::wstring path = programsDir + L"\\" + oldName;
        if (FAILED(link.as<IPersistFile>()->Load(path.c_str(), STGM_READ)))
            continue;
        wchar_t target[MAX_PATH]{};
        link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH);
        if (!ourExe(target))
            continue;
        const std::wstring oldDir = parentDir(target);
        if (!oldDir.empty() && _wcsicmp(oldDir.c_str(), directory.c_str()) != 0)
            retired = oldDir;
        if (DeleteFileW(path.c_str()))
            SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, path.c_str(), nullptr);
    }
    if (!previousDirectory.empty() && _wcsicmp(previousDirectory.c_str(), directory.c_str()) != 0)
        retired = previousDirectory;

    const std::wstring shortcut = programsDir.empty() ? L"" : programsDir + L"\\Yunzhao.lnk";
    bool existing = link && !shortcut.empty() && SUCCEEDED(link.as<IPersistFile>()->Load(shortcut.c_str(), STGM_READ));
    wchar_t oldTarget[MAX_PATH]{};
    if (existing) {
        link->GetPath(oldTarget, MAX_PATH, nullptr, SLGP_RAWPATH);
        if (_wcsicmp(oldTarget, exe.c_str()) != 0)
            hr = link.as<IPersistFile>()->Load(shortcut.c_str(), STGM_READWRITE);
        else
            hr = S_FALSE; // already points here
    }
    if (hr == S_OK) {
        hr = link->SetPath(exe.c_str());
        if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(directory.c_str());
        if (SUCCEEDED(hr)) hr = link->SetIconLocation(exe.c_str(), 0);
        if (SUCCEEDED(hr)) hr = link->SetDescription(L"Yunzhao");
        if (SUCCEEDED(hr)) {
            auto properties = link.as<IPropertyStore>();
            PROPVARIANT id{};
            hr = InitPropVariantFromString(kAppId, &id);
            if (SUCCEEDED(hr)) hr = properties->SetValue(PKEY_AppUserModel_ID, id);
            PropVariantClear(&id);
            if (SUCCEEDED(hr)) hr = properties->Commit();
        }
        if (SUCCEEDED(hr)) hr = link.as<IPersistFile>()->Save(shortcut.c_str(), TRUE);
        if (FAILED(hr))
            Log(L"app: could not register the start menu shortcut (0x%08X)", unsigned(hr));
        else
            SHChangeNotify(existing ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, shortcut.c_str(), nullptr);
    }

    if (!retired.empty() && GetFileAttributesW((retired + L"\\Uninstall.ps1").c_str()) != INVALID_FILE_ATTRIBUTES &&
        (GetFileAttributesW((retired + L"\\Yunzhao.exe").c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW((retired + L"\\Yunzhao.exe").c_str()) != INVALID_FILE_ATTRIBUTES)) {
        EnumWindows([](HWND window, LPARAM param) -> BOOL {
            wchar_t cls[64]{};
            if (!GetClassNameW(window, cls, 64) ||
                (wcscmp(cls, L"YunzhaoCoreHost") != 0 && wcscmp(cls, L"AtriumCoreHost") != 0))
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            wchar_t image[32768]{};
            DWORD length = DWORD(std::size(image));
            const auto* keep = reinterpret_cast<const std::wstring*>(param);
            if (process && QueryFullProcessImageNameW(process, 0, image, &length) && _wcsicmp(image, keep->c_str()) != 0) {
                PostMessageW(window, WM_CLOSE, 0, 0);
                WaitForSingleObject(process, 8000);
            }
            if (process)
                CloseHandle(process);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&exe));
        // The folder of the previous copy. Account data is not in it.
        std::wstring pattern = retired + L"\\*";
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &found);
        std::vector<std::wstring> pending;
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0)
                    pending.push_back(retired + L"\\" + found.cFileName);
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
        std::function<void(const std::wstring&)> removeTree = [&](const std::wstring& path) {
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
                return;
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                WIN32_FIND_DATAW inner{};
                HANDLE innerSearch = FindFirstFileW((path + L"\\*").c_str(), &inner);
                if (innerSearch != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(inner.cFileName, L".") != 0 && wcscmp(inner.cFileName, L"..") != 0)
                            removeTree(path + L"\\" + inner.cFileName);
                    } while (FindNextFileW(innerSearch, &inner));
                    FindClose(innerSearch);
                }
                RemoveDirectoryW(path.c_str());
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        };
        for (const std::wstring& path : pending)
            removeTree(path);
        if (RemoveDirectoryW(retired.c_str()))
            Log(L"app: removed the previous Yunzhao copy at %s", retired.c_str());
        else
            Log(L"app: the previous Yunzhao copy is still at %s", retired.c_str());
    }
}

} // namespace yunzhao
