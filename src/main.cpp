// BatteryTray — приложение: плашка заряда беспроводных устройств на панели задач
#include "common.h"
#include "config.h"
#include "chip.h"
#include "providers.h"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

#define WM_APP_REFRESH (WM_APP + 1)

static const ULONGLONG FRESH_MS = 25000;   // прочие устройства
static const ULONGLONG FRESH_APPLE_MS = 30000;   // данные старше — устройство считается отключённым
static const ULONGLONG BLE_INTERVAL = 90000;  // как часто опрашивать «медленные» источники

static HWND g_hwnd = nullptr;   // скрытое окно-приёмник сообщений
static HWND g_chip = nullptr;   // плашка
static HWND g_tip = nullptr;    // системный tooltip (не используется)
static HWND g_bubble = nullptr; // своя подсказка в стиле Win11
static bool g_chipVisible = false;
static bool g_dark = true;
static bool g_fullscreenBusy = false;

// чтобы не перерисовывать плашку без изменений (иначе заметно моргает)
static std::wstring g_lastSig;
static RECT g_lastRect{};
static std::vector<BYTE> g_alpha;   // маска прозрачности для WM_NCHITTEST
static int g_maskW = 0, g_maskH = 0;
static int g_fsStreak = 0;          // сколько подряд проверок видим фуллскрин
static int g_hover = -1;            // индекс устройства под курсором
static ULONGLONG g_hoverSince = 0;  // когда начался текущий hover
static std::wstring g_bubbleText;   // что сейчас в подсказке
static std::vector<RECT> g_itemRects;  // области устройств (координаты плашки)
static bool g_trackingLeave = false;

static Config g_cfg;
static std::vector<std::unique_ptr<IProvider>> g_providers;
static std::mutex g_mtx;
static std::map<std::wstring, Device> g_devices;    // id -> последнее известное состояние
static std::map<std::wstring, std::wstring> g_known; // id -> имя (для меню, включая скрытые)
static HANDLE g_wakeEvent = nullptr;
static HWINEVENTHOOK g_winEvent = nullptr;
static bool g_autostartInRegistry = false;

static void RequestRefresh() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_REFRESH, 0, 0);
}

// смена устройства под курсором: подсветка + подсказка с задержкой, как в Windows
static void BubbleHide() {
    if (g_bubble) ShowWindow(g_bubble, SW_HIDE);
}

static void BubbleShow(std::wstring const& text, RECT const& itemRect) {
    if (!g_bubble) return;
    int dpi = GetDpiForWindow(g_bubble);
    if (!dpi) dpi = 96;
    int W = 0, H = 0;
    HDC mem = RenderTooltip(text, dpi, W, H);
    if (!mem || W <= 0 || H <= 0) return;

    RECT wr{};
    GetWindowRect(g_chip, &wr);
    int cx = wr.left + (itemRect.left + itemRect.right) / 2;
    int x = cx - W / 2;
    int y = wr.top - H - 4;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(g_chip, MONITOR_DEFAULTTONEAREST), &mi);
    if (x < mi.rcWork.left + 4) x = mi.rcWork.left + 4;
    if (x + W > mi.rcWork.right - 4) x = mi.rcWork.right - 4 - W;
    if (y < mi.rcWork.top + 4) y = wr.bottom + 4;

    SetWindowPos(g_bubble, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HDC screen = GetDC(nullptr);
    POINT src{0, 0}, dst{x, y};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_bubble, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    DeleteObject((HBITMAP)GetCurrentObject(mem, OBJ_BITMAP));
    DeleteDC(mem);
}

static void HoverChanged(int idx) {
    if (idx == g_hover) return;
    g_hover = idx;
    g_hoverSince = GetTickCount64();
    if (idx < 0) BubbleHide();
}

// ---------------- опрос провайдеров ----------------
static void PollOnce() {
    std::vector<Device> all;
    ULONGLONG now = GetTickCount64();
    static ULONGLONG lastBle = 0;
    for (auto& p : g_providers) {
        bool slow = (wcscmp(p->Name(), L"BLE GATT") == 0);
        if (slow && now - lastBle < BLE_INTERVAL) continue;
        if (slow) lastBle = now;
        std::vector<Device> out;
        try {
            p->Poll(out);
        } catch (...) {
        }
        for (auto& d : out) {
            if (d.ts == 0) d.ts = now;
            all.push_back(d);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& d : all) {
            g_devices[d.id] = d;
            g_known[d.id] = d.name;
        }
        // убираем давно пропавшие
        for (auto it = g_devices.begin(); it != g_devices.end();) {
            ULONGLONG limit = (it->first.rfind(L"apple:", 0) == 0) ? FRESH_APPLE_MS : FRESH_MS;
            if (GetTickCount64() - it->second.ts > limit) it = g_devices.erase(it);
            else ++it;
        }
    }
    RequestRefresh();
}

static void PollThread() {
    while (true) {
        PollOnce();
        WaitForSingleObject(g_wakeEvent, 10000);
    }
}

// ---------------- видимый набор ----------------
static bool KindLess(Device const& a, Device const& b) {
    if ((int)a.kind != (int)b.kind) return (int)a.kind < (int)b.kind;
    return a.name < b.name;
}

static std::vector<Device> VisibleDevices(std::wstring& tip) {
    std::vector<Device> list;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ULONGLONG now = GetTickCount64();
        for (auto const& kv : g_devices) {
            Device d = kv.second;
            if (d.pct < 0) continue;
            ULONGLONG limit = (d.id.rfind(L"apple:", 0) == 0) ? FRESH_APPLE_MS : FRESH_MS;
            if (now - d.ts > limit) continue;
            if (g_cfg.IsHidden(d.id)) continue;
            list.push_back(d);
        }
    }
    // дубли по типу+названию (устройство могло «переехать» на другой PID/канал) — берём самое свежее
    {
        std::map<std::wstring, size_t> best;
        std::vector<bool> drop(list.size(), false);
        for (size_t i = 0; i < list.size(); ++i) {
            std::wstring key = std::to_wstring((int)list[i].kind) + L"|" + list[i].name;
            auto it = best.find(key);
            if (it == best.end()) {
                best[key] = i;
            } else {
                if (list[i].ts >= list[it->second].ts) {
                    drop[it->second] = true;
                    it->second = i;
                } else {
                    drop[i] = true;
                }
            }
        }
        std::vector<Device> kept;
        for (size_t i = 0; i < list.size(); ++i)
            if (!drop[i]) kept.push_back(list[i]);
        list.swap(kept);
    }

    // из нескольких Apple-устройств показываем только ближайшее (самый сильный сигнал)
    {
        int best = -1, bestIdx = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id.rfind(L"apple:", 0) == 0 && list[i].strength > best) { best = list[i].strength; bestIdx = (int)i; }
        if (bestIdx >= 0) {
            Device keep = list[bestIdx];
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [](Device const& d) { return d.id.rfind(L"apple:", 0) == 0; }),
                       list.end());
            list.push_back(keep);
        }
    }

    std::sort(list.begin(), list.end(), KindLess);
    if (list.size() > 4) list.resize(4);

    std::wstring t;
    for (auto const& d : list) {
        if (!t.empty()) t += L"\n";
        t += d.name;
        if (!d.source.empty()) t += L" (" + d.source + L")";
        t += L": " + std::to_wstring(d.pct) + L"%";
        if (d.charging) t += L" — заряжается";
        if (!d.detail.empty()) t += L" • " + d.detail;
    }
    tip = t.empty() ? L"BatteryTray: нет подключённых устройств" : t;
    return list;
}

// ---------------- позиционирование ----------------
static void ComputeChipPos(int W, int H, int& left, int& top) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    bool placed = false;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        RECT tb = abd.rc;
        HWND tray = FindWindowExW(FindWindowW(L"Shell_TrayWnd", nullptr), nullptr, L"TrayNotifyWnd", nullptr);
        RECT tr{};
        if (tray) GetWindowRect(tray, &tr);
        if (tb.right - tb.left > tb.bottom - tb.top) {
            int rightEdge = tray ? tr.left - 6 : tb.right - 200;
            left = rightEdge - W;
            top = tb.top + (tb.bottom - tb.top - H) / 2;
            if (left < tb.left + 4) left = tb.left + 4;
            placed = true;
        }
    }
    if (!placed) {
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        left = wa.right - W - 160;
        top = wa.bottom - H - 6;
    }
}

static bool IsShellWindow(HWND h) {
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    static const wchar_t* shellClasses[] = {L"Shell_TrayWnd",
                                            L"Shell_SecondaryTrayWnd",
                                            L"Progman",
                                            L"WorkerW",
                                            L"NotifyIconOverflowWindow",
                                            L"TopLevelWindowForOverflowXamlIsland",
                                            L"Windows.UI.Core.CoreWindow",
                                            L"XamlExplorerHostIslandWindow",
                                            L"MultitaskingViewFrame",
                                            L"ForegroundStaging",
                                            L"TaskListThumbnailWnd",
                                            L"#32768"};
    for (auto c : shellClasses)
        if (!wcscmp(cls, c)) return true;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    bool shell = false;
    wchar_t path[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &sz)) {
        std::wstring s = path;
        if (s.find(L"explorer.exe") != std::wstring::npos || s.find(L"ShellExperienceHost") != std::wstring::npos ||
            s.find(L"SearchHost") != std::wstring::npos || s.find(L"StartMenuExperienceHost") != std::wstring::npos ||
            s.find(L"TextInputHost") != std::wstring::npos)
            shell = true;
    }
    CloseHandle(p);
    return shell;
}

static bool FullscreenAppActive() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (IsShellWindow(fg)) return false;  // панель задач, пуск, поиск — это не игра
    RECT r{};
    if (!GetWindowRect(fg, &r)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    bool covers = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
                  r.bottom >= mi.rcMonitor.bottom;
    return covers;
}

// ---------------- перерисовка ----------------
static void DebugLog(const std::wstring& line) {
    if (!g_cfg.debug) return;
    FILE* f = _wfopen((Config::Dir() + L"\\debug.log").c_str(), L"ab");
    if (!f) return;
    fwrite(line.c_str(), sizeof(wchar_t), line.size(), f);
    const wchar_t nl[3] = {13, 10, 0};
    fwrite(nl, sizeof(wchar_t), 2, f);
    fclose(f);
}

static int HitItem(POINT screenPt) {
    if (!g_chip) return -1;
    POINT p = screenPt;
    ScreenToClient(g_chip, &p);
    for (size_t i = 0; i < g_itemRects.size(); ++i) {
        RECT const& r = g_itemRects[i];
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return (int)i;
    }
    return -1;
}

static void UpdateBubble(std::vector<Device> const& items) {
    if (g_hover < 0 || g_hover >= (int)items.size() || g_hover >= (int)g_itemRects.size() ||
        GetTickCount64() - g_hoverSince < 400) {
        if (!g_bubbleText.empty()) {
            g_bubbleText.clear();
            BubbleHide();
        } else {
            BubbleHide();
        }
        return;
    }
    Device const& d = items[g_hover];
    std::wstring t = d.name;
    if (!d.detail.empty()) {
        t += L"\n";
        t += d.detail;  // например: L 80%  R 70%  кейс 80%
    } else {
        t += L"\n";
        t += std::to_wstring(d.pct) + L"%";
        if (d.charging) t += L" — заряжается";
    }
    if (t != g_bubbleText || !IsWindowVisible(g_bubble)) {
        g_bubbleText = t;
        BubbleShow(t, g_itemRects[g_hover]);
    }
}

static void RefreshChip() {
    std::wstring tip;
    auto items = VisibleDevices(tip);

    // держим hover по фактической позиции курсора (сообщения мыши могут теряться)
    if (g_chipVisible) {
        POINT cur{};
        if (GetCursorPos(&cur)) {
            RECT wr{};
            if (GetWindowRect(g_chip, &wr) && PtInRect(&wr, cur)) {
                int idx = HitItem(cur);
                HoverChanged(idx);
            } else if (g_hover != -1) {
                HoverChanged(-1);
            }
        }
    }

    // фуллскрин считаем «занятым» только если он держится две проверки подряд (иначе плашка моргает)
    if (FullscreenAppActive()) g_fsStreak++;
    else g_fsStreak = 0;
    g_fullscreenBusy = g_fsStreak >= 2;

    if (g_tip) {
        std::wstring hoverTip = tip;
        if (g_hover >= 0 && g_hover < (int)items.size()) {
            Device const& d = items[g_hover];
            hoverTip = d.name;
            if (!d.source.empty()) hoverTip += L" (" + d.source + L")";
            hoverTip += L"\n" + std::to_wstring(d.pct) + L"%";
            if (d.charging) hoverTip += L" — заряжается";
            if (!d.detail.empty()) hoverTip += L"\n" + d.detail;
        }
        static std::wstring lastTip;
        if (hoverTip != lastTip) {
            lastTip = hoverTip;
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = g_chip;
            ti.uId = 1;
            ti.lpszText = (LPWSTR)hoverTip.c_str();
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }

    if (items.empty() || g_fullscreenBusy) {
        BubbleHide();
        if (g_chipVisible) {
            ShowWindow(g_chip, SW_HIDE);
            g_chipVisible = false;
            g_lastSig.clear();
        }
        return;
    }

    {
        std::wstring dbg = L"items=" + std::to_wstring(items.size()) + L" fs=" + std::to_wstring(g_fsStreak) +
                           L" hover=" + std::to_wstring(g_hover) + L" rects=" + std::to_wstring(g_itemRects.size()) + L" :";
        for (auto const& d : items) dbg += L" " + d.id + L"=" + std::to_wstring(d.pct);
        DebugLog(dbg);
    }

    // подпись содержимого — если не изменилось, не трогаем окно
    std::wstring sig;
    for (auto const& d : items) {
        sig += d.id;
        sig += L'|';
        sig += std::to_wstring(d.pct);
        sig += d.charging ? L'c' : L'-';
        sig += std::to_wstring((int)d.kind);
        sig += L';';
    }
    sig += L"#hover=";
    sig += std::to_wstring(g_hover);

    int dpi = g_chip ? GetDpiForWindow(g_chip) : 96;
    if (!dpi) dpi = 96;
    int W = 0, H = 0;
    HDC mem = RenderChip(items, dpi, g_dark, W, H, g_hover, g_itemRects);
    if (!mem || W <= 0 || H <= 0) return;

    // целевая позиция
    int left = 0, top = 0;
    ComputeChipPos(W, H, left, top);

    if (sig == g_lastSig && g_chipVisible && left == g_lastRect.left && top == g_lastRect.top && W == (g_lastRect.right - g_lastRect.left) &&
        H == (g_lastRect.bottom - g_lastRect.top)) {
        DeleteObject((HBITMAP)GetCurrentObject(mem, OBJ_BITMAP));
        DeleteDC(mem);
        // панель задач при клике поднимается в topmost-слое и закрывает плашку — возвращаем себя наверх
        SetWindowPos(g_chip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        UpdateBubble(items);
        return;
    }

    SetWindowPos(g_chip, HWND_TOPMOST, left, top, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HDC screen = GetDC(nullptr);
    POINT src{0, 0}, dst{left, top};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_chip, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    // сохраняем маску прозрачности (для сквозных кликов)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> px((size_t)W * H * 4);
        GetDIBits(mem, (HBITMAP)GetCurrentObject(mem, OBJ_BITMAP), 0, H, px.data(), &bi, DIB_RGB_COLORS);
        g_alpha.resize((size_t)W * H);
        for (size_t i = 0; i < g_alpha.size(); ++i) g_alpha[i] = px[i * 4 + 3];
        g_maskW = W;
        g_maskH = H;
    }

    ReleaseDC(nullptr, screen);
    DeleteObject((HBITMAP)GetCurrentObject(mem, OBJ_BITMAP));
    DeleteDC(mem);

    g_lastSig = sig;
    g_lastRect = RECT{left, top, left + W, top + H};
    g_chipVisible = true;

    UpdateBubble(items);
}

// ---------------- автозапуск ----------------
static std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static bool AutostartInRegistry() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024];
    DWORD sz = sizeof(buf), type = 0;
    bool on = RegQueryValueExW(k, L"BatteryTray", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, L"BatteryTray", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"BatteryTray");
    }
    RegCloseKey(k);
    g_autostartInRegistry = on;
    g_cfg.autostart = on;
    g_cfg.Save();
}

// ---------------- меню ----------------
static void ShowMenu() {
    BubbleHide();
    HMENU m = CreatePopupMenu();
    std::wstring tip;
    auto visible = VisibleDevices(tip);

    for (auto const& d : visible) {
        std::wstring s = d.name + L"  —  " + std::to_wstring(d.pct) + L"%";
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s.c_str());
    }
    if (visible.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Нет подключённых устройств");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    // какие устройства показывать
    std::map<std::wstring, std::wstring> known;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        known = g_known;
    }
    int cmdId = 100;
    std::map<int, std::wstring> idByCmd;
    HMENU sub = CreatePopupMenu();
    if (known.empty()) {
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"устройств пока не найдено");
    } else {
        for (auto const& kv : known) {
            std::wstring label = kv.second.empty() ? kv.first : kv.second;
            bool shown = !g_cfg.IsHidden(kv.first);
            AppendMenuW(sub, MF_STRING | (shown ? MF_CHECKED : 0), cmdId, label.c_str());
            idByCmd[cmdId] = kv.first;
            cmdId++;
        }
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, cmdId++, L"Показать все");
    }
    int showAllCmd = cmdId - 1;
    AppendMenuW(m, MF_POPUP | MF_STRING, (UINT_PTR)sub, L"Отображаемые устройства");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 1, L"Обновить сейчас");
    AppendMenuW(m, MF_STRING | (g_autostartInRegistry ? MF_CHECKED : 0), 2, L"Автозапуск с Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);

    if (cmd == 1) {
        SetEvent(g_wakeEvent);
    } else if (cmd == 2) {
        SetAutostart(!g_autostartInRegistry);
    } else if (cmd == 3) {
        DestroyWindow(g_hwnd);
    } else if (cmd == showAllCmd) {
        g_cfg.hidden.clear();
        g_cfg.Save();
        RefreshChip();
    } else if (idByCmd.count(cmd)) {
        const std::wstring& id = idByCmd[cmd];
        g_cfg.SetHidden(id, !g_cfg.IsHidden(id));
        RefreshChip();
    }
}

// ---------------- окна ----------------
static LRESULT CALLBACK ChipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // не забираем фокус у активного окна
        case WM_NCHITTEST: {
            // клик/hover только по значкам устройств, остальное — сквозь в панель задач
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            for (auto const& r : g_itemRects) {
                if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) return HTCLIENT;
            }
            return HTTRANSPARENT;
        }
        case WM_MOUSEMOVE: {
            if (!g_trackingLeave) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                g_trackingLeave = true;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            for (size_t i = 0; i < g_itemRects.size(); ++i) {
                RECT const& r = g_itemRects[i];
                if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
                    idx = (int)i;
                    break;
                }
            }
            HoverChanged(idx);
            RefreshChip();
            return 0;
        }
        case WM_MOUSELEAVE: {
            g_trackingLeave = false;
            POINT cur{};
            int idx = -1;
            if (GetCursorPos(&cur)) idx = HitItem(cur);
            HoverChanged(idx);
            RefreshChip();
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ShowMenu();
            (void)pt;
            return 0;
        }
        case WM_RBUTTONUP:
            ShowMenu();
            return 0;
        case WM_DPICHANGED:
            g_lastSig.clear();
            RefreshChip();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_REFRESH:
            RefreshChip();
            return 0;
        case WM_TIMER:
            if (wp == 2) {
                KillTimer(h, 2);
                if (g_hover >= 0) {
                    std::wstring tipText;
                    auto items = VisibleDevices(tipText);
                    if (g_hover < (int)items.size() && g_hover < (int)g_itemRects.size()) {
                        Device const& d = items[g_hover];
                        std::wstring t = d.name;
                        if (!d.detail.empty()) {
                            t += L"\n";
                            t += d.detail;  // например: L 80%  R 70%  кейс 80%
                        } else {
                            t += L"\n";
                            t += std::to_wstring(d.pct) + L"%";
                            if (d.charging) t += L" — заряжается";
                        }
                        BubbleShow(t, g_itemRects[g_hover]);
                    }
                }
                return 0;
            }
            RefreshChip();
            return 0;
        case WM_DISPLAYCHANGE:
            RefreshChip();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void LoadTheme() {
    HKEY k;
    DWORD v = 1, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &k) ==
        ERROR_SUCCESS) {
        RegQueryValueExW(k, L"SystemUsesLightTheme", nullptr, nullptr, (BYTE*)&v, &sz);
        RegCloseKey(k);
    }
    g_dark = (v == 0);
}

static int DumpChip(const wchar_t* path) {
    std::vector<Device> fake;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto const& kv : g_devices) fake.push_back(kv.second);
    }
    if (fake.empty()) {
        Device a; a.id=L"a"; a.name=L"AirPods Pro"; a.kind=Kind::Headset; a.pct=80; a.source=L"Bluetooth";
        Device b; b.id=L"b"; b.name=L"Мышь"; b.kind=Kind::Mouse; b.pct=70; b.source=L"2.4 ГГц";
        Device c; c.id=L"c"; c.name=L"Клавиатура"; c.kind=Kind::Keyboard; c.pct=65; c.charging=true; c.source=L"2.4 ГГц";
        Device d; d.id=L"d"; d.name=L"Геймпад"; d.kind=Kind::Gamepad; d.pct=55; d.source=L"USB";
        fake = {a, b, c, d};
    }
    int W = 0, H = 0;
    std::vector<RECT> dummyRects;
    HDC mem = RenderChip(fake, 96 * 3, g_dark, W, H, 0, dummyRects);
    if (!mem) return 1;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px((size_t)W * H * 4);
    GetDIBits(mem, (HBITMAP)GetCurrentObject(mem, OBJ_BITMAP), 0, H, px.data(), &bi, DIB_RGB_COLORS);
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        int hdr[3] = {W, H, 0};
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }
    DeleteObject((HBITMAP)GetCurrentObject(mem, OBJ_BITMAP));
    DeleteDC(mem);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_cfg.Load();

    if (cmdline && wcsstr(cmdline, L"--dump-chip")) {
        GdiplusStartupInput gsi0;
        ULONG_PTR tok0 = 0;
        GdiplusStartup(&tok0, &gsi0, nullptr);
        LoadTheme();
        int rc = DumpChip(L"chip_dump.bin");
        GdiplusShutdown(tok0);
        return rc;
    }
    if (cmdline && wcsstr(cmdline, L"--dump-devices")) {
        GdiplusStartupInput g0;
        ULONG_PTR t0 = 0;
        GdiplusStartup(&t0, &g0, nullptr);
        g_providers.push_back(MakeAppleProvider());
        g_providers.push_back(MakeCompxProvider());
        g_providers.push_back(MakeLogitechProvider());
        g_providers.push_back(MakeXboxProvider());
        g_providers.push_back(MakeHidBatteryProvider());
        g_providers.push_back(MakeBleProvider());
        PollOnce();
        Sleep(8000);
        PollOnce();
        std::wstring out;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto const& kv : g_devices) {
                out += kv.first + L"  |  " + kv.second.name + L"  |  kind=" + std::to_wstring((int)kv.second.kind) +
                       L"  |  " + std::to_wstring(kv.second.pct) + L"%  |  " + kv.second.source;
                out += L"\n";
            }
        }
        std::wstring file = Config::Dir() + L"\\devices.txt";
        FILE* f = _wfopen(file.c_str(), L"wb");
        if (f) {
            fwrite(out.c_str(), sizeof(wchar_t), out.size(), f);
            fclose(f);
        }
        GdiplusShutdown(t0);
        return 0;
    }

    if (cmdline && wcsstr(cmdline, L"--exit")) {
        HWND w = FindWindowW(L"BatteryTrayWnd", nullptr);
        if (w) PostMessageW(w, WM_CLOSE, 0, 0);
        return 0;
    }

    HANDLE once = CreateMutexW(nullptr, TRUE, L"BatteryTray_single_instance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GdiplusStartupInput gsi;
    ULONG_PTR gpToken = 0;
    GdiplusStartup(&gpToken, &gsi, nullptr);

    LoadTheme();
    g_autostartInRegistry = AutostartInRegistry();

    // провайдеры
    g_providers.push_back(MakeAppleProvider());
    g_providers.push_back(MakeCompxProvider());
    g_providers.push_back(MakeLogitechProvider());
    g_providers.push_back(MakeXboxProvider());
    g_providers.push_back(MakeHidBatteryProvider());
    g_providers.push_back(MakeBleProvider());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"BatteryTrayWnd";
    RegisterClassExW(&wc);

    WNDCLASSEXW cc{};
    cc.cbSize = sizeof(cc);
    cc.lpfnWndProc = ChipProc;
    cc.hInstance = inst;
    cc.lpszClassName = L"BatteryTrayChip";
    cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&cc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"BatteryTray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    g_chip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                             cc.lpszClassName, L"BatteryTrayChip", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);

    WNDCLASSEXW bc{};
    bc.cbSize = sizeof(bc);
    bc.lpfnWndProc = DefWindowProcW;
    bc.hInstance = inst;
    bc.lpszClassName = L"BatteryTrayBubble";
    RegisterClassExW(&bc);
    g_bubble = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                               bc.lpszClassName, L"BatteryTrayBubble", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);

    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_chip, nullptr, inst, nullptr);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = g_chip;
    ti.uId = 1;
    ti.lpszText = (LPWSTR)L"";
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 420);

    // мгновенно поднимаем плашку при смене активного окна (панель задач уходит наверх при клике)
    g_winEvent = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                 [](HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
                                     if (g_hwnd) PostMessageW(g_hwnd, WM_APP_REFRESH, 0, 0);
                                 },
                                 0, 0, WINEVENT_OUTOFCONTEXT);

    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SetAppleNotify([] {
        if (g_wakeEvent) SetEvent(g_wakeEvent);
    });
    std::thread(PollThread).detach();

    SetTimer(g_hwnd, 1, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_winEvent) UnhookWinEvent(g_winEvent);
    CloseHandle(g_wakeEvent);
    GdiplusShutdown(gpToken);
    return 0;
}
