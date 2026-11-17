// BatteryTray — провайдер геймпадов Xbox (XInput)
#include "providers.h"
#include <windows.h>
#include <xinput.h>
#include <string>

#pragma comment(lib, "xinput.lib")

class XboxProvider : public IProvider {
public:
    const wchar_t* Name() const override { return L"XInput"; }

    bool Poll(std::vector<Device>& out) override {
        bool any = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
            XINPUT_BATTERY_INFORMATION bat{};
            if (XInputGetBatteryInformation(i, BATTERY_DEVTYPE_GAMEPAD, &bat) != ERROR_SUCCESS) continue;
            if (bat.BatteryType == BATTERY_TYPE_WIRED || bat.BatteryType == BATTERY_TYPE_DISCONNECTED) continue;
            int pct = -1;
            switch (bat.BatteryLevel) {
                case BATTERY_LEVEL_EMPTY: pct = 5; break;
                case BATTERY_LEVEL_LOW: pct = 25; break;
                case BATTERY_LEVEL_MEDIUM: pct = 60; break;
                case BATTERY_LEVEL_FULL: pct = 100; break;
                default: pct = -1; break;
            }
            if (pct < 0) continue;
            Device d;
            d.id = L"xinput:" + std::to_wstring(i);
            d.name = L"Геймпад Xbox";
            d.source = L"USB / беспроводной адаптер";
            d.kind = Kind::Gamepad;
            d.pct = pct;
            d.detail = bat.BatteryType == BATTERY_TYPE_ALKALINE ? L"батарейки" : L"аккумулятор";
            d.ts = GetTickCount64();
            out.push_back(d);
            any = true;
        }
        return any;
    }
};

std::unique_ptr<IProvider> MakeXboxProvider() { return std::make_unique<XboxProvider>(); }
