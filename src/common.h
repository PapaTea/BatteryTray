// BatteryTray — общие типы
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

enum class Kind { Headset = 0, Mouse = 1, Keyboard = 2, Gamepad = 3, Other = 4 };

inline const wchar_t* KindGlyph(Kind k) {
    switch (k) {
        case Kind::Headset: return L"\uE7F6";   // наушники
        case Kind::Mouse: return L"\uE962";     // мышь
        case Kind::Keyboard: return L"\uE765";  // клавиатура
        case Kind::Gamepad: return L"\uE7FC";   // геймпад
        default: return L"\uE83F";              // прочее — батарейка
    }
}

inline const wchar_t* KindName(Kind k) {
    switch (k) {
        case Kind::Headset: return L"Наушники";
        case Kind::Mouse: return L"Мышь";
        case Kind::Keyboard: return L"Клавиатура";
        case Kind::Gamepad: return L"Геймпад";
        default: return L"Устройство";
    }
}

struct Device {
    std::wstring id;      // стабильный ключ (для настройки «скрыть»)
    std::wstring name;    // отображаемое имя
    std::wstring source;  // «2.4 ГГц», «Bluetooth», «USB», «BLE»
    Kind kind = Kind::Other;
    int pct = -1;         // -1 = неизвестно
    bool charging = false;
    std::wstring detail;  // доп. инфа (L/R/кейс, мВ)
    ULONGLONG ts = 0;     // когда получено
    int strength = 0;     // уровень сигнала (BLE), чтобы выбрать ближайшее устройство
};

// Провайдер: один способ связи (донгл Compx, Logitech HID++, BLE, Apple adv, XInput, HID battery)
struct IProvider {
    virtual const wchar_t* Name() const = 0;
    // заполняет out найденными устройствами; return true если что-то нашлось
    virtual bool Poll(std::vector<Device>& out) = 0;
    virtual ~IProvider() = default;
};
