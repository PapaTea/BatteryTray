// BatteryTray — настройки
#include "config.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

std::wstring Config::Dir() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        dir = appdata;
        CoTaskMemFree(appdata);
    } else {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        if (n && n < MAX_PATH) dir = buf;
    }
    if (dir.empty()) dir = L".";
    dir += L"\\BatteryTray";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Config::FilePath() { return Dir() + L"\\config.ini"; }

void Config::Load() {
    hidden.clear();
    std::wifstream f(FilePath());
    if (!f) return;
    std::wstring line, section;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            size_t e = line.find(L']');
            section = (e == std::wstring::npos) ? line.substr(1) : line.substr(1, e - 1);
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == L' ' || k.back() == L'\t')) k.pop_back();
        while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(v.begin());
        if (section == L"general") {
            if (k == L"autostart") autostart = (v == L"1" || v == L"true");
            else if (k == L"debug") debug = (v == L"1" || v == L"true");
        } else if (section == L"hidden") {
            if (v == L"1") hidden.insert(k);
        }
    }
}

void Config::Save() const {
    std::wstring dir = Dir();
    std::wstring tmp = dir + L"\\config.ini.tmp";
    {
        std::wofstream f(tmp);
        if (!f) return;
        f << L"; BatteryTray — настройки\n";
        f << L"[general]\n";
        f << L"autostart=" << (autostart ? 1 : 0) << L"\n";
        f << L"debug=" << (debug ? 1 : 0) << L"\n";
        f << L"\n[hidden]\n; устройства, которые не отображаются (id=1)\n";
        for (auto const& h : hidden) f << h << L"=1\n";
    }
    MoveFileExW(tmp.c_str(), FilePath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Config::SetHidden(const std::wstring& id, bool hide) {
    if (hide) hidden.insert(id);
    else hidden.erase(id);
    Save();
}
