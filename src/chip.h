// BatteryTray — отрисовка плашки на панели задач
#pragma once
#include "common.h"
#include <windows.h>
#include <vector>

// Рисует плашку. hoverIndex — индекс подсвечиваемого устройства (-1 = нет).
// itemRects — области устройств в координатах картинки (для hit-test и подсветки).
HDC RenderChip(std::vector<Device> const& items, int dpi, bool dark, int& W, int& H, int hoverIndex,
               std::vector<RECT>& itemRects);

// Подсказка в стиле Win11 (тёмная скруглённая плашка)
HDC RenderTooltip(std::wstring const& text, int dpi, int& W, int& H);
