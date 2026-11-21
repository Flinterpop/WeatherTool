// Light/dark colour palettes, including the colour ramps for the heatmaps.
#pragma once

#include "gdiplus_inc.h"

#include <array>
#include <cstddef>

namespace wt {

struct Theme {
    bool dark = false;
    Gdiplus::Color bg;           // window background
    Gdiplus::Color text;
    Gdiplus::Color textSecondary;
    Gdiplus::Color textMuted;
    Gdiplus::Color grid;
    Gdiplus::Color guide;        // hover band
    Gdiplus::Color series1;      // blue: snow, first period
    Gdiplus::Color series2;      // orange: temperature, second period
    Gdiplus::Color series2Soft;  // the yearly temperature line under its mean
    Gdiplus::Color series3;      // aqua: third period
    Gdiplus::Color trend;        // dashed least-squares line
    Gdiplus::Color warm;         // text: above normal
    Gdiplus::Color cool;         // text: below normal
    Gdiplus::Color listBg;
    Gdiplus::Color listSel;
    Gdiplus::Color listDivider;
    Gdiplus::Color tipBg;
    Gdiplus::Color tipBorder;
    Gdiplus::Color tipText;
    std::array<Gdiplus::Color, 7> diverging;   // cold .. neutral .. warm
    std::array<Gdiplus::Color, 5> sequential;  // none .. lots
    COLORREF bgRef     = 0;      // same colours for GDI controls
    COLORREF listBgRef = 0;
    COLORREF textRef   = 0;
};

const Theme& ThemeFor(bool dark);

// Reads the Windows "apps use light theme" preference.
bool SystemPrefersDark();

// Piecewise-linear interpolation through `stops` at t in [0, 1] (clamped).
Gdiplus::Color Ramp(const Gdiplus::Color* stops, size_t count, double t);

} // namespace wt
