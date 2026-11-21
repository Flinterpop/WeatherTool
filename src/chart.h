// Chart rendering with GDI+: a banded chart (one slot per year or month,
// holding bars and/or lines) and a year x month heatmap, each with its own
// title row, legend, axes and hover tooltip.
#pragma once

#include "common.h"
#include "gdiplus_inc.h"
#include "theme.h"

#include <array>
#include <string>

namespace wt {

enum class XAxis { Years, Seasons, Months };
constexpr size_t kMaxChartSeries = 4;

struct ChartSeries {
    const double*  v = nullptr;   // `n` values in display units; NaN = gap
    Gdiplus::Color color;
    float          width  = 2.0f; // line width, dip
    bool           bars   = false;
    bool           dashed = false;
    bool           inTip  = true;
    std::wstring   label;
};

struct BandChartInput {
    std::wstring   title;
    std::wstring   subtitle;      // muted, after the title
    XAxis          axis = XAxis::Years;
    int            firstYear = 0; // year of slot 0 (Years / Seasons)
    size_t         n = 0;
    std::array<ChartSeries, kMaxChartSeries> series{};
    size_t         seriesCount = 0;
    int            decimals = 1;
    const wchar_t* unit = L"";
    std::wstring   empty = L"No data for this selection.";
    int            hoverX = -1;   // client px, -1 = none
    int            hoverY = -1;
    const Theme*   theme = nullptr;
};

using YearMonthGrid = std::array<std::array<double, 12>, kMaxYears>;

struct HeatmapInput {
    std::wstring   title;
    std::wstring   subtitle;
    int            firstYear = 0;
    size_t         nYears = 0;
    const YearMonthGrid* values = nullptr;   // display units, NaN = no data
    const YearMonthGrid* extra  = nullptr;   // optional second tooltip line
    std::wstring   valueLabel;
    std::wstring   extraLabel;
    bool           diverging = false;  // values in [-limit, limit] else [0, limit]
    double         limit = 1.0;
    int            decimals = 1;
    const wchar_t* unit = L"";
    bool           signedValues = false;
    int            hoverX = -1;
    int            hoverY = -1;
    const Theme*   theme = nullptr;
};

void DrawBandChart(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const BandChartInput& in, float scale);
void DrawHeatmap(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const HeatmapInput& in, float scale);

// Font size in px for a point size at 96 dpi.
Gdiplus::REAL FontPx(float pt, float scale);

} // namespace wt
