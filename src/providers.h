// BatteryTray — интерфейсы провайдеров
#pragma once
#include "common.h"
#include <memory>

std::unique_ptr<IProvider> MakeCompxProvider();      // VGN / ATK / VXE / MAD (донгл 2.4 ГГц)
std::unique_ptr<IProvider> MakeLogitechProvider();   // Logitech HID++ 2.0
std::unique_ptr<IProvider> MakeHidBatteryProvider(); // стандартный HID Battery Strength
std::unique_ptr<IProvider> MakeXboxProvider();       // геймпады Xbox
std::unique_ptr<IProvider> MakeBleProvider();        // любые BLE с GATT Battery Service
std::unique_ptr<IProvider> MakeAppleProvider();      // AirPods / Beats (Apple Continuity)

void DumpHidDevices();  // отладочный дамп

void AppleDebug(int& advSeen, int& appleSeen, int& status);  // отладочный дамп

// колбэк «пришли свежие данные AirPods» (вызывается из потока BLE)
void SetAppleNotify(void (*fn)());
