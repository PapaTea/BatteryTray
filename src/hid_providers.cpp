// BatteryTray — HID-провайдеры: Compx (VGN/ATK/VXE), Logitech HID++, стандартный HID-батарея
#include "providers.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")

static bool g_dbg = false;
// ---------------- вспомогательное ----------------
struct HidDev {
    std::wstring path;
    USHORT vid = 0, pid = 0;
    USHORT usagePage = 0, usage = 0;
    USHORT inLen = 0, outLen = 0, featLen = 0;
    std::wstring product;
};

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// быстро открыть и прочитать дескрипторную информацию
static bool ProbeHid(const std::wstring& path, HidDev& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    HIDD_ATTRIBUTES att{};
    att.Size = sizeof(att);
    if (HidD_GetAttributes(h, &att)) {
        out.vid = att.VendorID;
        out.pid = att.ProductID;
    }
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(h, &pp)) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
            out.usagePage = caps.UsagePage;
            out.usage = caps.Usage;
            out.inLen = caps.InputReportByteLength;
            out.outLen = caps.OutputReportByteLength;
            out.featLen = caps.FeatureReportByteLength;
        }
        HidD_FreePreparsedData(pp);
    }
    wchar_t buf[256] = {};
    if (HidD_GetProductString(h, buf, sizeof(buf))) out.product = buf;
    CloseHandle(h);
    return true;
}

static std::vector<HidDev> EnumHid() {
    std::vector<HidDev> res;
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO devs = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return res;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &g, i, &did); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        auto buf = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
        if (!buf) continue;
        buf->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &did, buf, need, &need, nullptr)) { free(buf); continue; }
        HidDev d;
        d.path = buf->DevicePath;
        free(buf);
        if (ProbeHid(d.path, d)) res.push_back(d);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return res;
}

// неблокирующий HID-канал
struct HidIo {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;
    DWORD inLen = 0;
    ~HidIo() { Close(); }
    bool Open(const std::wstring& p) {
        h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(h, &pp)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) inLen = caps.InputReportByteLength;
                HidD_FreePreparsedData(pp);
            }
        }
        return h != INVALID_HANDLE_VALUE;
    }
    void Close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (ev) CloseHandle(ev);
        h = INVALID_HANDLE_VALUE;
        ev = nullptr;
    }
    bool Write(const BYTE* data, DWORD len, int timeoutMs = 300) {
        if (h == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD wrote = 0;
        BOOL ok = WriteFile(h, data, len, &wrote, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ev, timeoutMs) != WAIT_OBJECT_0) { CancelIo(h); return false; }
            ok = GetOverlappedResult(h, &ov, &wrote, FALSE);
        }
        return ok && wrote > 0;
    }
    // читает один отчёт с таймаутом; возвращает длину или 0
    DWORD Read(BYTE* buf, DWORD cap, int timeoutMs) {
        if (h == INVALID_HANDLE_VALUE) return 0;
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf, cap, &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ev, timeoutMs);
            if (w != WAIT_OBJECT_0) {
                CancelIo(h);
                got = 0;
            } else if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
                got = 0;
            }
        } else if (!ok) {
            got = 0;
        }
        if (got == 0) {
            // резервный путь: запросить input report напрямую
            if (HidD_GetInputReport(h, buf, cap)) return inLen ? inLen : 1;
        }
        return got;
    }
};

static std::wstring HashW(std::wstring const& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : s) { h ^= (unsigned long long)c; h *= 1099511628211ULL; }
    wchar_t buf[24];
    swprintf_s(buf, L"%016llX", h);
    return buf;
}

static bool ContainsNoCase(const std::wstring& s, const wchar_t* sub) {
    std::wstring a = s, b = sub;
    for (auto& c : a) c = towlower(c);
    for (auto& c : b) c = towlower(c);
    return a.find(b) != std::wstring::npos;
}

// ================= Compx: VGN / ATK / VXE / MAD =================
// V1: коллекция FF02:0002, отчёт 0x08, 17 байт; crc = (0x55 - sum) & 0xFF - 8
static void CompxV1Frame(BYTE* rep, BYTE cmd, const BYTE* val, int vlen) {
    memset(rep, 0, 17);
    BYTE* d = rep + 1;
    d[0] = cmd;
    if (val && vlen > 0) {
        d[4] = (BYTE)vlen;
        for (int i = 0; i < vlen && 5 + i < 15; ++i) d[5 + i] = val[i];
    }
    int sum = 0;
    for (int i = 0; i < 15; ++i) sum += d[i];
    d[15] = (BYTE)(((85 - (sum & 0xFF)) & 0xFF) - 8);
    rep[0] = 0x08;
}

class CompxProvider : public IProvider {
public:
    const wchar_t* Name() const override { return L"Compx 2.4 ГГц"; }

    bool Poll(std::vector<Device>& out) override {
        bool found = false;
        static ULONGLONG s_lastFull = 0;
        ULONGLONG now = GetTickCount64();
        auto devs = EnumHid();
        if (g_dbg) fwprintf(stderr, L"compx: hid count=%zu|", devs.size());
        for (auto const& d : devs) {
            if (g_dbg && (d.vid == 0x3554 || d.vid == 0x373B))
                fwprintf(stderr, L"  cand up=%04X us=%04X out=%u in=%u prod=%ls|", d.usagePage, d.usage, d.outLen, d.inLen,
                         d.product.c_str());
            if (d.vid != 0x3554 && d.vid != 0x373B) continue;
            if (d.usagePage == 0xFF02 && d.usage == 0x0002 && d.outLen == 17) {
                if (ReadV1(d, out)) found = true;
            } else if (d.usagePage == 0xFF05 && d.outLen == 64) {
                if (now - s_lastFull > 5000 && ReadV2(d, out)) found = true;
            } else if (d.usagePage == 0xFF1C && d.usage == 0x0092 && d.outLen >= 7) {
                if (now - s_lastFull > 5000 && ReadKeyboard(d, out)) found = true;
            }
        }
        if (found) s_lastFull = now;
        return found;
    }

private:
    bool ReadV1(HidDev const& d, std::vector<Device>& out) {
        HidIo io;
        if (!io.Open(d.path)) { if (g_dbg) fwprintf(stderr, L"open fail %ls|", d.path.c_str()); return false; }
        if (g_dbg) fwprintf(stderr, L"v1 try %ls|", d.path.c_str());
        BYTE rep[17];
        BYTE one = 1;
        CompxV1Frame(rep, 0x02, &one, 1);
        bool w1 = io.Write(rep, 17);
        BYTE tmp[64];
        DWORD r1 = io.Read(tmp, sizeof(tmp), 500);
        CompxV1Frame(rep, 0x03, nullptr, 0);
        bool w2 = io.Write(rep, 17);
        DWORD r2 = io.Read(tmp, sizeof(tmp), 500);
        CompxV1Frame(rep, 0x04, nullptr, 0);
        bool w3 = io.Write(rep, 17);
        if (g_dbg) fwprintf(stderr, L"w=%d,%d,%d reads=%lu,%lu|", (int)w1, (int)w2, (int)w3, r1, r2);
        if (!io.Write(rep, 17)) { if (g_dbg) fwprintf(stderr, L"write bat fail|"); return false; }

        ULONGLONG end = GetTickCount64() + 1500;
        bool ok = false;
        int pct = -1, mv = 0;
        bool charging = false;
        while (GetTickCount64() < end && !ok) {
            BYTE r[64] = {};
            DWORD got = io.Read(r, sizeof(r), 300);
            if (got < 10) continue;
            if (g_dbg) fwprintf(stderr, L"rep %02X %02X len=%lu|", r[0], r[1], got);
            if (r[0] != 0x08 || r[1] != 0x04) continue;
            pct = r[6];
            charging = r[7] == 1;
            mv = (r[8] << 8) | r[9];
            ok = (pct >= 0 && pct <= 100);
        }
        io.Close();
        if (!ok) return false;

        Device dev;
        dev.id = L"compx:" + std::to_wstring(d.vid) + L":" + HashW(d.product);
        dev.kind = Kind::Mouse;
        if (ContainsNoCase(d.product, L"keyboard") || ContainsNoCase(d.product, L"kbd")) dev.kind = Kind::Keyboard;
        dev.name = dev.kind == Kind::Keyboard ? L"Клавиатура" : L"Мышь";
        dev.source = L"2.4 ГГц";
        dev.pct = pct;
        dev.charging = charging;
        dev.ts = GetTickCount64();
        out.push_back(dev);
        return true;
    }

    // V2: коллекция FF05, 64-байтный отчёт 0x08, маркер 0x72
    bool ReadV2(HidDev const& d, std::vector<Device>& out) {
        HidIo io;
        if (!io.Open(d.path)) return false;
        auto build = [](BYTE* rep, BYTE cmd, BYTE paramLen, BYTE cmdLen, const BYTE* data, int dl, BYTE target) {
            memset(rep, 0, 64);
            rep[0] = 0x08;
            BYTE* f = rep + 1;  // 63 байта
            f[1] = 0x72;
            f[2] = paramLen;
            f[4] = target;
            f[5] = cmd;
            f[6] = cmdLen;
            if (data && dl > 0) memcpy(f + 7, data, dl > 56 ? 56 : dl);
            int sum = 0;
            for (int i = 1; i < 63; ++i) sum += f[i];
            f[0] = (BYTE)(sum & 0xFF);
        };
        auto query = [&](BYTE cmd, BYTE* payload, int& plen) -> bool {
            BYTE rep[64];
            build(rep, cmd, 2, 1, nullptr, 0, 1);
            if (!io.Write(rep, 64)) return false;
            ULONGLONG end = GetTickCount64() + 1500;
            while (GetTickCount64() < end) {
                BYTE r[80] = {};
                DWORD got = io.Read(r, sizeof(r), 300);
                if (got < 8) continue;
                const BYTE* f = r;
                if (f[0] == 0x08) f = r + 1;
                if (f[0] != 0x72) continue;
                if (f[1] == 0x73) continue;  // событие устройства
                if (f[4] != cmd) continue;
                if (f[1] == 0xFF) return false;
                plen = f[5] < 40 ? f[5] : 40;
                memcpy(payload, f + 6, plen);
                return true;
            }
            return false;
        };

        BYTE bat[40] = {}, chg[40] = {};
        int blen = 0, clen = 0;
        if (!query(0x07, bat, blen) || blen < 1) { io.Close(); return false; }
        bool charging = false;
        if (query(0x0F, chg, clen) && clen >= 1) charging = (chg[0] == 1 || chg[0] == 0x3B);
        io.Close();

        int pct = bat[0];
        if (pct < 0 || pct > 100) return false;
        Device dev;
        dev.id = L"compx2:" + std::to_wstring(d.vid) + L":" + HashW(d.product);
        dev.kind = Kind::Mouse;
        dev.name = L"Мышь";
        dev.source = L"2.4 ГГц";
        dev.pct = pct;
        dev.charging = charging;
        dev.ts = GetTickCount64();
        out.push_back(dev);
        return true;
    }

    // Клавиатура VGN V98Pro: коллекция FF1C:0092, отчёт 4
    bool ReadKeyboard(HidDev const& d, std::vector<Device>& out) {
        HidIo io;
        if (!io.Open(d.path)) return false;
        BYTE rep[16] = {0x04, 0x00, 0x00, 0x1A, 0x06, 0x00, 0x00, 0x00};
        if (!io.Write(rep, d.outLen ? d.outLen : 8)) { io.Close(); return false; }
        ULONGLONG end = GetTickCount64() + 1500;
        bool ok = false;
        int pct = -1;
        bool charging = false;
        while (GetTickCount64() < end && !ok) {
            BYTE r[64] = {};
            DWORD got = io.Read(r, sizeof(r), 300);
            if (got < 10) continue;
            if (r[0] != 0x04 || r[3] != 0x1A || r[4] != 0x06) continue;
            pct = r[8];
            charging = r[9] == 1;
            ok = (pct >= 0 && pct <= 100);
        }
        io.Close();
        if (!ok) return false;
        Device dev;
        dev.id = L"vgn-kbd:" + std::to_wstring(d.vid) + L":" + HashW(d.product);
        dev.kind = Kind::Keyboard;
        dev.name = L"Клавиатура";
        dev.source = L"2.4 ГГц";
        dev.pct = pct;
        dev.charging = charging;
        dev.ts = GetTickCount64();
        out.push_back(dev);
        return true;
    }
};

// ================= Logitech HID++ 2.0 =================
class LogitechProvider : public IProvider {
public:
    const wchar_t* Name() const override { return L"Logitech HID++"; }

    bool Poll(std::vector<Device>& out) override {
        bool found = false;
        for (auto const& d : EnumHid()) {
            if (d.vid != 0x046D) continue;
            if (d.usagePage != 0xFF00) continue;
            if (d.usage != 0x0001 && d.usage != 0x0002) continue;
            if (d.outLen < 7) continue;
            if (PollInterface(d, out)) found = true;
        }
        return found;
    }

private:
    // long report 0x11 (20 байт) — если поддерживается, иначе short 0x10 (7 байт)
    static const int LONG_LEN = 20;
    static const int SHORT_LEN = 7;

    bool PollInterface(HidDev const& d, std::vector<Device>& out) {
        HidIo io;
        if (!io.Open(d.path)) return false;
        bool useLong = d.outLen >= LONG_LEN;
        BYTE sw = 0x05;
        bool found = false;

        for (int idx = 1; idx <= 6 && !found; ++idx) found = ProbeDevice(io, useLong, (BYTE)idx, sw, out);
        if (!found) found = ProbeDevice(io, useLong, 0xFF, sw, out);
        io.Close();
        return found;
    }

    bool Send(HidIo& io, bool useLong, BYTE devIdx, BYTE featIdx, BYTE funcSw, const BYTE* params, int n) {
        BYTE rep[LONG_LEN] = {};
        int len = useLong ? LONG_LEN : SHORT_LEN;
        rep[0] = useLong ? 0x11 : 0x10;
        rep[1] = devIdx;
        rep[2] = featIdx;
        rep[3] = funcSw;
        for (int i = 0; i < n && 4 + i < len; ++i) rep[4 + i] = params[i];
        return io.Write(rep, len);
    }

    // возвращает true и заполняет params при успешном ответе
    bool Request(HidIo& io, bool useLong, BYTE devIdx, BYTE featIdx, BYTE funcId, const BYTE* params, int n,
                 BYTE* reply, int& replyLen, int timeoutMs = 250) {
        BYTE sw = 0x05;
        if (!Send(io, useLong, devIdx, featIdx, (BYTE)(funcId | (sw << 4)), params, n)) return false;
        int len = useLong ? LONG_LEN : SHORT_LEN;
        ULONGLONG end = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < end) {
            BYTE r[64] = {};
            DWORD got = io.Read(r, sizeof(r), 80);
            if (got < 7) continue;
            if (r[1] != devIdx) continue;
            if ((r[3] >> 4) != sw) continue;
            if (r[2] == 0xFF) return false;  // ошибка
            if (r[2] != featIdx) continue;
            replyLen = (int)got - 4;
            memcpy(reply, r + 4, replyLen < 32 ? replyLen : 32);
            (void)len;
            return true;
        }
        return false;
    }

    bool ProbeDevice(HidIo& io, bool useLong, BYTE devIdx, BYTE sw, std::vector<Device>& out) {
        auto featureIndex = [&](USHORT featureId, BYTE& idx) -> bool {
            BYTE p[2] = {(BYTE)(featureId >> 8), (BYTE)(featureId & 0xFF)};
            BYTE rep[32] = {};
            int rl = 0;
            if (!Request(io, useLong, devIdx, 0x00, 0x00, p, 2, rep, rl)) return false;
            if (rl < 1 || rep[0] == 0x00) return false;
            idx = rep[0];
            return true;
        };

        Device dev;
        dev.source = L"Logitech";
        dev.ts = GetTickCount64();

        BYTE fi = 0;
        if (featureIndex(0x1000, fi)) {  // BATTERY_STATUS
            BYTE p[1] = {0x00};
            BYTE rep[32] = {};
            int rl = 0;
            if (Request(io, useLong, devIdx, fi, 0x00, p, 1, rep, rl) && rl >= 3) {
                int level = rep[0];
                int status = rep[2];
                if (level > 0 && level <= 100) {
                    dev.pct = level;
                    dev.charging = (status >= 1 && status <= 3);
                    Finish(dev, devIdx, out, io, useLong);
                    return true;
                }
            }
        }
        if (featureIndex(0x1004, fi)) {  // UNIFIED_BATTERY
            BYTE rep[32] = {};
            int rl = 0;
            if (Request(io, useLong, devIdx, fi, 0x00, nullptr, 0, rep, rl) && rl >= 3) {
                int soc = rep[0];
                if (soc > 0 && soc <= 100) {
                    dev.pct = soc;
                    dev.charging = (rep[2] >= 1 && rep[2] <= 3);
                    Finish(dev, devIdx, out, io, useLong);
                    return true;
                }
            }
        }
        if (featureIndex(0x1001, fi)) {  // BATTERY_VOLTAGE
            BYTE rep[32] = {};
            int rl = 0;
            if (Request(io, useLong, devIdx, fi, 0x00, nullptr, 0, rep, rl) && rl >= 2) {
                int mv = (rep[0] << 8) | rep[1];
                BYTE flags = rl >= 3 ? rep[2] : 0;
                if (mv >= 3000 && mv <= 5000) {
                    int pct = VoltageToPercent(mv);
                    dev.pct = pct;
                    dev.charging = (flags & 0x80) != 0;
                    Finish(dev, devIdx, out, io, useLong);
                    return true;
                }
            }
        }
        return false;
    }

    static int VoltageToPercent(int mv) {
        // приблизительная кривая Li-ion 3.7 В
        struct P { int mv, pct; };
        static const P table[] = {{4200, 100}, {4100, 90}, {4000, 78}, {3900, 65}, {3800, 50},
                                  {3700, 35}, {3600, 22}, {3500, 12}, {3400, 5}, {3300, 0}};
        if (mv >= table[0].mv) return 100;
        for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); ++i) {
            if (mv >= table[i].mv) {
                int lo = table[i].mv, hi = table[i - 1].mv;
                int plo = table[i].pct, phi = table[i - 1].pct;
                return plo + (mv - lo) * (phi - plo) / (hi - lo);
            }
        }
        return 0;
    }

    void Finish(Device& dev, BYTE devIdx, std::vector<Device>& out, HidIo& io, bool useLong) {
        dev.id = L"logi:" + std::to_wstring(devIdx);
        // имя устройства: feature 0x0005 DEVICE_NAME
        std::wstring name;
        BYTE fi = 0;
        BYTE p[2] = {0x00, 0x05};
        BYTE rep[32] = {};
        int rl = 0;
        if (Request(io, useLong, devIdx, 0x00, 0x00, p, 2, rep, rl) && rl >= 1 && rep[0] != 0) {
            BYTE nameIdx = rep[0];
            BYTE cp[1] = {0x00};
            if (Request(io, useLong, devIdx, nameIdx, 0x00, cp, 1, rep, rl) && rl >= 1) {
                int total = rep[0] > 30 ? 30 : rep[0];
                int off = 0;
                while (off < total) {
                    BYTE ip[1] = {(BYTE)off};
                    if (!Request(io, useLong, devIdx, nameIdx, 0x01, ip, 1, rep, rl)) break;
                    int chunk = rl >= 2 ? (rl - 1) : 0;
                    if (chunk <= 0) break;
                    std::string s((char*)rep + 1, chunk);
                    name += std::wstring(s.begin(), s.end());
                    off += chunk;
                    if (off >= total) break;
                }
            }
        }
        if (name.empty()) name = L"Logitech";
        dev.name = name;
        // тип по имени
        if (ContainsNoCase(name, L"keyboard") || ContainsNoCase(name, L"kbd")) dev.kind = Kind::Keyboard;
        else if (ContainsNoCase(name, L"mouse")) dev.kind = Kind::Mouse;
        else dev.kind = Kind::Mouse;
        out.push_back(dev);
    }
};

// ================= стандартный HID Battery Strength (page 0x06, usage 0x20) =================
class HidBatteryProvider : public IProvider {
public:
    const wchar_t* Name() const override { return L"HID Battery"; }

    bool Poll(std::vector<Device>& out) override {
        bool any = false;
        for (auto const& d : EnumHid()) {
            if (d.usagePage == 0xFF02 || d.usagePage == 0xFF00 || d.usagePage == 0xFF05 || d.usagePage == 0xFF1C) continue;
            if (d.inLen == 0 || d.inLen > 64) continue;
            HidIo io;
            if (!io.Open(d.path)) continue;
            PHIDP_PREPARSED_DATA pp = nullptr;
            bool hasBattery = false;
            if (HidD_GetPreparsedData(io.h, &pp)) {
                USHORT n = 0;
                HidP_GetValueCaps(HidP_Input, nullptr, &n, pp);
                if (n) {
                    auto vc = (PHIDP_VALUE_CAPS)malloc(sizeof(HIDP_VALUE_CAPS) * n);
                    if (vc && HidP_GetValueCaps(HidP_Input, vc, &n, pp) == HIDP_STATUS_SUCCESS) {
                        for (USHORT i = 0; i < n; ++i) {
                            USHORT usage = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage;
                            if (vc[i].UsagePage == 0x06 && usage == 0x20) { hasBattery = true; break; }
                        }
                    }
                    free(vc);
                }
                HidD_FreePreparsedData(pp);
            }
            if (!hasBattery) { io.Close(); continue; }
            for (int attempt = 0; attempt < 6; ++attempt) {
                BYTE buf[64] = {};
                DWORD got = io.Read(buf, sizeof(buf), 120);
                if (!got) continue;
                if (!HidD_GetPreparsedData(io.h, &pp)) break;
                ULONG val = 0;
                NTSTATUS st = HidP_GetUsageValue(HidP_Input, 0x06, 0, 0x20, &val, pp, (PCHAR)buf, got);
                HidD_FreePreparsedData(pp);
                if (st == HIDP_STATUS_SUCCESS && val <= 100) {
                    Device dev;
                    dev.id = L"hid:" + HashW(d.path);
                    dev.name = d.product.empty() ? std::wstring(L"USB-устройство") : d.product;
                    dev.source = L"USB";
                    dev.kind = d.usagePage == 0x01 ? (d.usage == 0x02 ? Kind::Mouse : (d.usage == 0x06 ? Kind::Keyboard : Kind::Other)) : Kind::Other;
                    dev.pct = (int)val;
                    dev.ts = GetTickCount64();
                    out.push_back(dev);
                    any = true;
                    break;
                }
            }
            io.Close();
        }
        return any;
    }
};

std::unique_ptr<IProvider> MakeCompxProvider() { return std::make_unique<CompxProvider>(); }
std::unique_ptr<IProvider> MakeLogitechProvider() { return std::make_unique<LogitechProvider>(); }
std::unique_ptr<IProvider> MakeHidBatteryProvider() { return std::make_unique<HidBatteryProvider>(); }

// отладка: перечислить HID-интерфейсы с их caps
void DumpHidDevices() {
    g_dbg = true;
    for (auto const& d : EnumHid()) {
        printf("vid=%04X pid=%04X up=%04X us=%04X in=%u out=%u feat=%u prod=%ls", d.vid, d.pid, d.usagePage,
               d.usage, d.inLen, d.outLen, d.featLen, d.product.c_str());
        printf("\n");
    }
}
