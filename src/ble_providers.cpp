// BatteryTray — Bluetooth провайдеры: BLE GATT (0x180F) и Apple Continuity (AirPods/Beats)
#include "providers.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <set>
#include <cstdio>
#include <cstdio>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Enumeration;

static const GUID GUID_BATTERY_SVC = {0x0000180F, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
static const GUID GUID_BATTERY_CHR = {0x00002A19, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

static void EnsureApartment() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
}

// ================= BLE GATT Battery Service =================
class BleProvider : public IProvider {
public:
    const wchar_t* Name() const override { return L"BLE GATT"; }

    bool Poll(std::vector<Device>& out) override {
        bool any = false;
        EnsureApartment();
        try {
            auto infos = DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get();
            for (auto const& info : infos) {
                std::wstring name = std::wstring(info.Name().c_str());
                if (IsAppleName(name)) continue;  // AirPods читаем через adv
                try {
                    auto dev = BluetoothLEDevice::FromIdAsync(info.Id()).get();
                    if (!dev || dev.ConnectionStatus() != BluetoothConnectionStatus::Connected) continue;
                    int pct = ReadBattery(dev);
                    if (pct < 0) continue;
                    Device d;
                    d.id = L"ble:" + HexId(info.Id().c_str());
                    d.name = name.empty() ? std::wstring(L"Bluetooth-устройство") : name;
                    d.source = L"Bluetooth";
                    d.kind = GuessKind(name);
                    d.pct = pct;
                    d.ts = GetTickCount64();
                    out.push_back(d);
                    any = true;
                } catch (...) {
                }
            }
        } catch (...) {
        }
        return any;
    }

private:
    static bool IsAppleName(std::wstring const& n) {
        const wchar_t* keys[] = {L"AirPods", L"Beats", L"Powerbeats"};
        for (auto k : keys)
            if (n.find(k) != std::wstring::npos) return true;
        return false;
    }

    static Kind GuessKind(std::wstring const& n) {
        auto has = [&](const wchar_t* s) { return n.find(s) != std::wstring::npos; };
        if (has(L"Pods") || has(L"Headphone") || has(L"Headset") || has(L"WH-") || has(L"HD ") || has(L"Audio"))
            return Kind::Headset;
        if (has(L"Mouse") || has(L"мышь")) return Kind::Mouse;
        if (has(L"Keyboard") || has(L"клавиат")) return Kind::Keyboard;
        if (has(L"Controller") || has(L"Gamepad")) return Kind::Gamepad;
        return Kind::Other;
    }

    static std::wstring HexId(const wchar_t* s) {
        // стабильный хеш строки id
        unsigned long long h = 1469598103934665603ULL;
        for (const wchar_t* p = s; *p; ++p) {
            h ^= (unsigned long long)(*p);
            h *= 1099511628211ULL;
        }
        wchar_t buf[24];
        swprintf_s(buf, L"%016llX", h);
        return buf;
    }

    static int ReadBattery(BluetoothLEDevice const& dev) {
        try {
            auto svcRes = dev.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
            if (svcRes.Status() != GattCommunicationStatus::Success) return -1;
            for (auto const& svc : svcRes.Services()) {
                if (!(svc.Uuid() == winrt::guid(GUID_BATTERY_SVC))) continue;
                auto chRes = svc.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
                if (chRes.Status() != GattCommunicationStatus::Success) return -1;
                for (auto const& ch : chRes.Characteristics()) {
                    if (!(ch.Uuid() == winrt::guid(GUID_BATTERY_CHR))) continue;
                    auto r = ch.ReadValueAsync(BluetoothCacheMode::Uncached).get();
                    if (r.Status() != GattCommunicationStatus::Success) return -1;
                    auto b = r.Value();
                    if (b.Length() < 1) return -1;
                    auto dr = winrt::Windows::Storage::Streams::DataReader::FromBuffer(b);
                    return (int)dr.ReadByte();
                }
            }
        } catch (...) {
        }
        return -1;
    }
};

// ================= Apple Continuity (AirPods / Beats) =================
struct AppleState {
    bool valid = false;
    int left = -1, right = -1, casePct = -1;
    int model = 0;
    ULONGLONG ts = 0;
    int strength = -100;
};

static AppleState g_apple;
static std::atomic<int> g_advSeen{0};
static std::atomic<int> g_appleSeen{0};
static void (*g_appleNotify)() = nullptr;
static std::mutex g_appleMtx;
static BluetoothLEAdvertisementWatcher g_watcher{nullptr};

static std::wstring AppleModel(int m) {
    switch (m) {
        case 0x0220: return L"AirPods 1";
        case 0x0F20: return L"AirPods 2";
        case 0x1320: return L"AirPods 3";
        case 0x1920: return L"AirPods 4";
        case 0x1B20: return L"AirPods 4 ANC";
        case 0x0A20: return L"AirPods Max";
        case 0x1F20: return L"AirPods Max USB-C";
        case 0x0E20: return L"AirPods Pro";
        case 0x1420: return L"AirPods Pro 2";
        case 0x2420: return L"AirPods Pro 2 USB-C";
        case 0x0520: return L"Beats X";
        case 0x0620: return L"Beats Solo3";
        case 0x0920: return L"Beats Studio3";
        case 0x1020: return L"Beats Flex";
        case 0x1120: return L"Beats Studio Buds";
        case 0x1620: return L"Beats Fit Pro";
        default: return L"AirPods";
    }
}

static bool IsKnownAppleModel(int m) {
    switch (m) {
        case 0x0220: case 0x0F20: case 0x1320: case 0x1920: case 0x1B20:
        case 0x0A20: case 0x1F20: case 0x0E20: case 0x1420: case 0x2420:
        case 0x0520: case 0x0620: case 0x0920: case 0x1020: case 0x1120: case 0x1620:
            return true;
        default:
            return false;
    }
}

static int NibbleToPct(BYTE n) {
    if (n <= 0x9) return n * 10;
    if (n <= 0xE) return 100;
    return -1;  // 0xF — нет данных
}

static bool ParseProximity(const uint8_t* d, size_t n, AppleState& out) {
    size_t i = 0;
    while (i + 2 <= n) {
        if (d[i] == 0x07 && i + 27 <= n) {
            const uint8_t* m = d + i;
            BYTE status = m[5], pods = m[6], charge = m[7];
            bool primaryLeft = (status & 0x20) != 0;
            int prim = NibbleToPct(pods & 0x0F);
            int sec = NibbleToPct(pods >> 4);
            out.left = primaryLeft ? prim : sec;
            out.right = primaryLeft ? sec : prim;
            out.casePct = NibbleToPct(charge >> 4);
            out.model = (m[3] << 8) | m[4];
            out.valid = true;
            out.ts = GetTickCount64();
            return true;
        }
        size_t step = 2 + (size_t)d[i + 1];
        if (!step || i + step > n) break;
        i += step;
    }
    return false;
}


// --- пропуск только «своих» Apple-устройств: сопряжены с ПК и сейчас подключены ---
struct AppleGate {
    bool havePaired = false;
    bool connected = false;
    std::set<int> models;  // допустимые model-id для сопряжённых устройств
};
static AppleGate g_gate;
static std::mutex g_gateMtx;
static ULONGLONG g_gateTs = 0;

static void AddModelsForName(std::wstring const& n, std::set<int>& m) {
    auto has = [&](const wchar_t* s) { return n.find(s) != std::wstring::npos; };
    if (has(L"Pro 2")) { m.insert(0x1420); m.insert(0x2420); }
    else if (has(L"Pro")) { m.insert(0x0E20); m.insert(0x1420); m.insert(0x2420); }
    else if (has(L"Max")) { m.insert(0x0A20); m.insert(0x1F20); }
    else if (has(L"4")) { m.insert(0x1920); m.insert(0x1B20); }
    else if (has(L"3")) { m.insert(0x1320); }
    else if (has(L"AirPods 2")) { m.insert(0x0F20); }
    else if (has(L"AirPods")) { m.insert(0x0220); m.insert(0x0F20); m.insert(0x1320); }
    else if (has(L"Beats") || has(L"Powerbeats")) { m.insert(0x0520); m.insert(0x0620); m.insert(0x0920); m.insert(0x1020); m.insert(0x1120); m.insert(0x1620); }
}

static void RefreshGate() {
    AppleGate g;
    try {
        auto props = winrt::single_threaded_vector<winrt::hstring>();
        props.Append(L"System.Devices.Aep.IsConnected");
        auto infos = DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true), props).get();
        for (auto const& info : infos) {
            std::wstring name = std::wstring(info.Name().c_str());
            if (name.find(L"AirPods") == std::wstring::npos && name.find(L"Beats") == std::wstring::npos &&
                name.find(L"Powerbeats") == std::wstring::npos)
                continue;
            g.havePaired = true;
            AddModelsForName(name, g.models);
            bool conn = false;
            try {
                auto v = info.Properties().Lookup(L"System.Devices.Aep.IsConnected");
                if (v) conn = winrt::unbox_value_or<bool>(v, false);
            } catch (...) {
            }
            if (conn) g.connected = true;
        }
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> lk(g_gateMtx);
        g_gate = g;
    }
}

class AppleProvider : public IProvider {
public:
    AppleProvider() { EnsureApartment(); Start(); }
    ~AppleProvider() override {
        try {
            if (g_watcher) g_watcher.Stop();
        } catch (...) {
        }
    }

    const wchar_t* Name() const override { return L"Apple Continuity"; }

    bool Poll(std::vector<Device>& out) override {
        if (GetTickCount64() - g_gateTs > 15000) {
            g_gateTs = GetTickCount64();
            RefreshGate();
        }
        AppleState st;
        {
            std::lock_guard<std::mutex> lk(g_appleMtx);
            st = g_apple;
        }
        if (!st.valid) return false;
        {
            std::lock_guard<std::mutex> lk(g_gateMtx);
            if (!g_gate.connected) return false;   // отключили — сразу убираем
        }
        Device d;
        d.id = L"apple:" + std::to_wstring(st.model);
        d.name = IsKnownAppleModel(st.model) ? AppleModel(st.model) : std::wstring(L"AirPods");
        d.source = L"Bluetooth";
        d.kind = Kind::Headset;
        int level = -1;
        if (st.left >= 0 && st.right >= 0) level = st.left < st.right ? st.left : st.right;
        else if (st.left >= 0) level = st.left;
        else if (st.right >= 0) level = st.right;
        else level = st.casePct;
        d.pct = level;
        auto pctStr = [](int v) { return v < 0 ? std::wstring(L"—") : std::to_wstring(v) + L"%"; };
        d.detail = L"L " + pctStr(st.left) + L"  R " + pctStr(st.right) + L"  кейс " + pctStr(st.casePct);
        d.ts = st.ts;
        d.strength = st.strength;
        out.push_back(d);
        return true;
    }

private:
    static void Start() {
        if (g_watcher) return;
        g_watcher = BluetoothLEAdvertisementWatcher();
        g_watcher.ScanningMode(BluetoothLEScanningMode::Active);
        g_watcher.Received([](BluetoothLEAdvertisementWatcher const&, BluetoothLEAdvertisementReceivedEventArgs const& a) {
            if (a.RawSignalStrengthInDBm() < -70) return;  // слишком далеко — скорее чужое устройство
            g_advSeen++;
            for (auto const& md : a.Advertisement().ManufacturerData()) {
                if (md.CompanyId() != 0x004C) continue;
                auto buf = md.Data();
                if (buf.Length() < 27) continue;
                auto dr = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
                uint8_t tmp[64];
                uint32_t len = buf.Length() > sizeof(tmp) ? sizeof(tmp) : buf.Length();
                dr.ReadBytes(winrt::array_view<uint8_t>(tmp, tmp + len));
                AppleState st;
                if (!ParseProximity(tmp, len, st)) continue;
                {
                    std::lock_guard<std::mutex> lk(g_gateMtx);
                    if (!g_gate.havePaired) continue;  // наушники не сопряжены с ПК — не наше
                    bool modelOk = g_gate.models.empty() || g_gate.models.count(st.model) > 0;
                    if (!modelOk) continue;  // модель не наша (чужие AirPods рядом)
                    // показываем, только если устройство подключено к ПК или сигнал очень сильный (рядом с нами)
                    if (!g_gate.connected && a.RawSignalStrengthInDBm() < -62) continue;
                }
                st.strength = a.RawSignalStrengthInDBm();
                {
                    g_appleSeen++;
                    {
                        std::lock_guard<std::mutex> lk(g_appleMtx);
                        g_apple = st;
                    }
                    if (g_appleNotify) g_appleNotify();
                }
            }
        });
        g_watcher.Start();
    }
};

std::unique_ptr<IProvider> MakeBleProvider() { return std::make_unique<BleProvider>(); }
std::unique_ptr<IProvider> MakeAppleProvider() { return std::make_unique<AppleProvider>(); }

// отладка apple-провайдера
void AppleDebug(int& advSeen, int& appleSeen, int& status) {
    advSeen = g_advSeen.load();
    appleSeen = g_appleSeen.load();
    status = -1;
    try {
        if (g_watcher) status = (int)g_watcher.Status();
    } catch (...) {
    }
}

void SetAppleNotify(void (*fn)()) { g_appleNotify = fn; }
