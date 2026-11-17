// BatteryTray — настройки (%APPDATA%\BatteryTray\config.ini)
#pragma once
#include "common.h"
#include <map>
#include <set>
#include <string>

class Config {
public:
    void Load();
    void Save() const;

    bool debug = false;
    bool autostart = false;              // хранится и в реестре, тут дублируется
    bool showPercentOnHover = true;
    std::set<std::wstring> hidden;       // id устройств, которые не показываем

    bool IsHidden(const std::wstring& id) const { return hidden.count(id) > 0; }
    void SetHidden(const std::wstring& id, bool hide);

    static std::wstring Dir();           // %APPDATA%\BatteryTray
    static std::wstring FilePath();
};
