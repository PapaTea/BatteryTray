// BatteryTray — отрисовка плашки: глиф устройства (Segoe Fluent Icons) + процент
#include "chip.h"
#include <objidl.h>
#include <gdiplus.h>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;

// Win11-подобная подсветка под курсором: скруглённый прямоугольник, лёгкая заливка
static void DrawHoverRect(Graphics& g, RectF r, bool dark) {
    GraphicsPath p;
    REAL rad = r.Height * 0.22f;
    if (rad < 3) rad = 3;
    p.AddArc(RectF(r.X, r.Y, rad * 2, rad * 2), 180, 90);
    p.AddArc(RectF(r.GetRight() - rad * 2, r.Y, rad * 2, rad * 2), 270, 90);
    p.AddArc(RectF(r.GetRight() - rad * 2, r.GetBottom() - rad * 2, rad * 2, rad * 2), 0, 90);
    p.AddArc(RectF(r.X, r.GetBottom() - rad * 2, rad * 2, rad * 2), 90, 90);
    p.CloseFigure();
    SolidBrush b(dark ? Color(26, 255, 255, 255) : Color(20, 0, 0, 0));
    g.FillPath(&b, &p);
}

HDC RenderChip(std::vector<Device> const& items, int dpi, bool dark, int& outW, int& outH, int hoverIndex,
               std::vector<RECT>& itemRects) {
    outW = outH = 0;
    itemRects.clear();
    if (items.empty()) return nullptr;
    float s = dpi / 96.0f;
    float padX = 8 * s, glyph = 16 * s, gapTxt = 5 * s, gapDev = 16 * s, padY = 4 * s;

    FontFamily uiF(L"Segoe UI");
    Font font(&uiF, 13.0f * s, FontStyleBold, UnitPixel);
    FontFamily iconF(L"Segoe Fluent Icons");
    Font fontIcons(&iconF, glyph, FontStyleRegular, UnitPixel);
    Font fontBolt(&iconF, 11.0f * s, FontStyleRegular, UnitPixel);
    const float bolt = 11.0f * s, boltGap = 2.0f * s, thick = glyph * 0.035f;
    StringFormat sf;
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);

    std::vector<std::wstring> texts(items.size());
    std::vector<float> txtW(items.size(), 0);
    float maxTxtH = 0;
    {
        HDC screen = GetDC(nullptr);
        Graphics meas(screen);
        meas.SetTextRenderingHint(TextRenderingHintAntiAlias);
        for (size_t i = 0; i < items.size(); ++i) {
            texts[i] = std::to_wstring(items[i].pct) + L"%";
            RectF bb;
            meas.MeasureString(texts[i].c_str(), (INT)texts[i].size(), &font, PointF(0, 0), &sf, &bb);
            txtW[i] = bb.Width;
            if (bb.Height > maxTxtH) maxTxtH = bb.Height;
        }
        ReleaseDC(nullptr, screen);
    }

    float h = maxTxtH + padY * 2;
    if (h < glyph + padY * 2) h = glyph + padY * 2;
    // высота подсветки — как у иконок трея Windows (чуть меньше панели задач)
    float hoverH = h + 6 * s;
    if (hoverH < 26 * s) hoverH = 26 * s;

    float w = padX * 2;
    for (size_t i = 0; i < items.size(); ++i)
        w += glyph + gapTxt + (items[i].charging ? bolt + boltGap : 0.0f) + txtW[i] +
             (i + 1 < items.size() ? gapDev : 0);

    int W = (int)ceilf(w), H = (int)ceilf((hoverH > h) ? hoverH : h);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bmp);
    memset(bits, 0, (size_t)W * H * 4);

    {
        // premultiplied ARGB — требование UpdateLayeredWindow(ULW_ALPHA)
        Bitmap gpb(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&gpb);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        Color cText = dark ? Color(255, 255, 255, 255) : Color(255, 16, 16, 16);
        Color cGlyph = dark ? Color(235, 240, 240, 240) : Color(240, 30, 30, 30);
        SolidBrush bText(cText), bGlyph(cGlyph);

        float x = padX, cy = H / 2.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            float itemX = x - 4 * s;
            float itemW = glyph + gapTxt + (items[i].charging ? bolt + boltGap : 0.0f) + txtW[i] + 8 * s;
            if ((int)i == hoverIndex) DrawHoverRect(g, RectF(itemX, cy - hoverH / 2, itemW, hoverH), dark);
            itemRects.push_back(RECT{(LONG)itemX, (LONG)(cy - hoverH / 2), (LONG)(itemX + itemW), (LONG)(cy + hoverH / 2)});

            // глиф устройства — с утолщением (двойная отрисовка со сдвигом)
            const wchar_t* code = KindGlyph(items[i].kind);
            g.DrawString(code, 1, &fontIcons, PointF(x, cy), &sf, &bGlyph);
            g.DrawString(code, 1, &fontIcons, PointF(x + thick, cy), &sf, &bGlyph);
            g.DrawString(code, 1, &fontIcons, PointF(x, cy + thick), &sf, &bGlyph);
            x += glyph + gapTxt;
            if (items[i].charging) {
                // молния перед числом
                g.DrawString(L"\uE945", 1, &fontBolt, PointF(x, cy), &sf, &bGlyph);
                x += bolt + boltGap;
            }
            g.DrawString(texts[i].c_str(), (INT)texts[i].size(), &font, PointF(x, cy), &sf, &bText);
            x += txtW[i] + gapDev;
        }
    }

    outW = W;
    outH = H;
    return mem;
}

// Всплывающая подсказка в стиле Win11: тёмная скруглённая плашка с текстом
HDC RenderTooltip(std::wstring const& text, int dpi, int& outW, int& outH) {
    outW = outH = 0;
    float s = dpi / 96.0f;
    FontFamily uiF(L"Segoe UI");
    Font font(&uiF, 12.5f * s, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    float padX = 12 * s, padY = 8 * s, lineH = 19 * s;

    std::vector<std::wstring> lines;
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find(L'\n', start);
            lines.push_back(text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
            if (nl == std::wstring::npos) break;
            start = nl + 1;
        }
    }

    float maxW = 0;
    {
        HDC screen = GetDC(nullptr);
        Graphics meas(screen);
        meas.SetTextRenderingHint(TextRenderingHintAntiAlias);
        for (auto const& l : lines) {
            RectF bb;
            meas.MeasureString(l.c_str(), (INT)l.size(), &font, PointF(0, 0), &sf, &bb);
            if (bb.Width > maxW) maxW = bb.Width;
        }
        ReleaseDC(nullptr, screen);
    }
    int W = (int)ceilf(maxW + padX * 2), H = (int)ceilf(lineH * (float)lines.size() + padY * 2);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(mem, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bmp);
    memset(bits, 0, (size_t)W * H * 4);

    {
        Bitmap gpb(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&gpb);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        GraphicsPath p;
        REAL rad = 8 * s;
        p.AddArc(RectF(0.5f, 0.5f, rad * 2, rad * 2), 180, 90);
        p.AddArc(RectF(W - 0.5f - rad * 2, 0.5f, rad * 2, rad * 2), 270, 90);
        p.AddArc(RectF(W - 0.5f - rad * 2, H - 0.5f - rad * 2, rad * 2, rad * 2), 0, 90);
        p.AddArc(RectF(0.5f, H - 0.5f - rad * 2, rad * 2, rad * 2), 90, 90);
        p.CloseFigure();
        SolidBrush bg(Color(243, 44, 44, 44));
        g.FillPath(&bg, &p);
        Pen border(Color(32, 255, 255, 255), 1.0f);
        g.DrawPath(&border, &p);

        SolidBrush fg(Color(255, 245, 245, 245));
        for (size_t i = 0; i < lines.size(); ++i) {
            Color c = (i == 0) ? Color(255, 245, 245, 245) : Color(200, 200, 200, 200);
            SolidBrush lb(c);
            g.DrawString(lines[i].c_str(), (INT)lines[i].size(), &font, PointF(padX, padY + lineH * (float)i), &sf, &lb);
        }
        (void)fg;
    }

    outW = W;
    outH = H;
    return mem;
}
