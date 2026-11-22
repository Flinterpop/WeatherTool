#include "chart.h"

#include "units.h"

#include <cassert>
#include <cmath>

namespace wt {
namespace {

using namespace Gdiplus;

constexpr const wchar_t* kMonthShort[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                             L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
constexpr const wchar_t* kMonthLong[12]  = { L"January", L"February", L"March", L"April", L"May", L"June",
                                             L"July", L"August", L"September", L"October", L"November", L"December" };

// Band chart plot margins (dip): the y labels on the left, a little room on the right.
constexpr float kPlotLeft  = 46.0f;
constexpr float kPlotSides = 56.0f;

Font MakeFont(float pt, float s, INT style = FontStyleRegular) {
    return Font(L"Segoe UI", FontPx(pt, s), style, UnitPixel);
}

float TextWidth(Graphics& g, const std::wstring& t, const Font& f) {
    RectF box;
    g.MeasureString(t.c_str(), static_cast<INT>(t.size()), &f, PointF(0, 0), &box);
    return box.Width;
}

void Text(Graphics& g, const std::wstring& t, const Font& f, const Color& c, float x, float y) {
    SolidBrush b(c);
    g.DrawString(t.c_str(), static_cast<INT>(t.size()), &f, PointF(x, y), &b);
}

// Text centred on x, top at y.
void TextC(Graphics& g, const std::wstring& t, const Font& f, const Color& c, float x, float y) {
    Text(g, t, f, c, x - TextWidth(g, t, f) / 2.0f, y);
}

double NiceStep(double raw) {
    assert(raw > 0.0);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f   = raw / mag;
    const double nice = f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 2.5 ? 2.5 : f <= 5.0 ? 5.0 : 10.0;
    return nice * mag;
}

// Every how many years an axis label goes, so labels are >= minPx apart.
int LabelEvery(float slotPx, float minPx) {
    static constexpr int kSteps[] = { 1, 2, 5, 10, 20, 25, 50 };
    for (int k : kSteps) {
        if (static_cast<float>(k) * slotPx >= minPx) { return k; }
    }
    return 100;
}

std::wstring SlotLabel(const BandChartInput& in, size_t i) {
    const XAxis axis = in.axis;
    const int y = in.firstYear + static_cast<int>(i);
    if (axis == XAxis::Custom) { return in.heads != nullptr ? in.heads[i] : std::wstring(); }
    if (axis == XAxis::Months) { return kMonthLong[i % 12]; }
    if (axis == XAxis::Seasons) { return SeasonLabel(y); }
    return std::to_wstring(y);
}

void RoundedRect(GraphicsPath& p, const RectF& r, float rad) {
    const float d = rad * 2.0f;
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

// A bar with rounded top corners, square at the baseline.
void FillBar(Graphics& g, const Brush& b, float x, float w, float yTop, float yBase, float s) {
    const float h = yBase - yTop;
    if (h <= 0.0f || w <= 0.0f) { return; }
    const float r = std::fmin(2.0f * s, std::fmin(w / 2.0f, h));
    if (r < 1.0f) {
        g.FillRectangle(&b, x, yTop, w, h);
        return;
    }
    GraphicsPath p;
    p.AddArc(x, yTop, 2 * r, 2 * r, 180.0f, 90.0f);
    p.AddArc(x + w - 2 * r, yTop, 2 * r, 2 * r, 270.0f, 90.0f);
    p.AddLine(x + w, yTop + r, x + w, yBase);
    p.AddLine(x + w, yBase, x, yBase);
    p.CloseFigure();
    g.FillPath(&b, &p);
}

struct Row {
    Color        swatch;
    bool         hasSwatch = false;
    std::wstring label;
    std::wstring value;
};

// Tooltip box beside (x, y), kept inside `bounds`.
void DrawTip(Graphics& g, const RectF& bounds, float x, float y, const std::wstring& head,
             const Row* rows, size_t count, const Theme& th, float s) {
    const Font fh = MakeFont(9.0f, s, FontStyleBold);
    const Font fr = MakeFont(9.0f, s);
    const float pad = 8.0f * s;
    const float lineH = FontPx(9.0f, s) * 1.45f;
    const float sw = 9.0f * s;
    float labelW = TextWidth(g, head, fh);
    float valueW = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        labelW = std::fmax(labelW, (rows[i].hasSwatch ? sw + 5.0f * s : 0.0f) + TextWidth(g, rows[i].label, fr));
        valueW = std::fmax(valueW, TextWidth(g, rows[i].value, fr));
    }
    const float w = pad * 2 + labelW + (valueW > 0 ? 14.0f * s + valueW : 0.0f);
    const float h = pad * 2 + lineH * static_cast<float>(count + 1) - 2.0f * s;
    float bx = x + 14.0f * s;
    if (bx + w > bounds.X + bounds.Width) { bx = x - 14.0f * s - w; }
    bx = std::fmax(bx, bounds.X);
    float by = std::fmin(y, bounds.Y + bounds.Height - h);
    by = std::fmax(by, bounds.Y);
    GraphicsPath box;
    RoundedRect(box, RectF(bx, by, w, h), 5.0f * s);
    SolidBrush bg(th.tipBg);
    Pen border(th.tipBorder, 1.0f);
    g.FillPath(&bg, &box);
    g.DrawPath(&border, &box);
    Text(g, head, fh, th.tipText, bx + pad, by + pad - 2.0f * s);
    for (size_t i = 0; i < count; ++i) {
        const float ry = by + pad - 2.0f * s + lineH * static_cast<float>(i + 1);
        float lx = bx + pad;
        if (rows[i].hasSwatch) {
            SolidBrush b(rows[i].swatch);
            g.FillRectangle(&b, lx, ry + (lineH - sw) / 2.0f - 1.0f * s, sw, sw);
            lx += sw + 5.0f * s;
        }
        Text(g, rows[i].label, fr, th.tipText, lx, ry);
        Text(g, rows[i].value, fr, th.tipText, bx + w - pad - TextWidth(g, rows[i].value, fr), ry);
    }
}

// Title, muted subtitle and (optionally) a legend right-aligned on one row.
float DrawTitleRow(Graphics& g, const RectF& rc, const std::wstring& title, const std::wstring& sub,
                   const Theme& th, float s) {
    const Font ft = MakeFont(10.5f, s, FontStyleBold);
    const Font fs = MakeFont(9.0f, s);
    Text(g, title, ft, th.text, rc.X, rc.Y);
    if (!sub.empty()) {
        const float x = rc.X + TextWidth(g, title, ft) + 6.0f * s;
        Text(g, sub, fs, th.textSecondary, x, rc.Y + 2.0f * s);
    }
    return 24.0f * s;
}

void DrawLegend(Graphics& g, const RectF& rc, const BandChartInput& in, float s) {
    const Font f = MakeFont(8.5f, s);
    float x = rc.X + rc.Width;
    const float y = rc.Y + 3.0f * s;
    for (size_t k = in.seriesCount; k-- > 0;) {
        const ChartSeries& cs = in.series[k];
        if (cs.label.empty() || cs.tipOnly) { continue; }
        const float tw = TextWidth(g, cs.label, f);
        x -= tw;
        Text(g, cs.label, f, in.theme->textSecondary, x, y);
        x -= 18.0f * s;
        const float my = y + FontPx(8.5f, s) * 0.75f;
        if (cs.bars) {
            SolidBrush b(cs.color);
            g.FillRectangle(&b, x, my - 5.0f * s, 12.0f * s, 10.0f * s);
        } else {
            Pen p(cs.color, std::fmax(cs.width, 2.0f) * s);
            if (cs.dashed) { p.SetDashStyle(DashStyleDash); }
            g.DrawLine(&p, x, my, x + 14.0f * s, my);
        }
        x -= 12.0f * s;
    }
}

} // namespace

REAL FontPx(float pt, float scale) {
    return pt * 96.0f / 72.0f * scale;
}

int BandChartSlotAt(const RectF& rc, size_t n, float x, float s) {
    assert(n <= kMaxSlots && s > 0.0f);
    const float left = rc.X + kPlotLeft * s;
    const float width = rc.Width - kPlotSides * s;
    if (n == 0 || width <= 0.0f || x < left || x >= left + width) { return -1; }
    const int slot = static_cast<int>((x - left) / (width / static_cast<float>(n)));
    return slot < static_cast<int>(n) ? slot : static_cast<int>(n) - 1;
}

void DrawBandChart(Graphics& g, const RectF& rc, const BandChartInput& in, float s) {
    assert(in.theme != nullptr);
    assert(in.n <= kMaxSlots);
    assert(in.axis != XAxis::Custom || (in.heads != nullptr && in.ticks != nullptr));
    const Theme& th = *in.theme;
    const float titleH = DrawTitleRow(g, rc, in.title, in.subtitle, th, s);
    DrawLegend(g, RectF(rc.X, rc.Y + titleH - 18.0f * s, rc.Width, 18.0f * s), in, s);

    const float extraLine = in.ticks2 != nullptr ? 15.0f * s : 0.0f;
    const RectF plot(rc.X + kPlotLeft * s, rc.Y + titleH + 8.0f * s, rc.Width - kPlotSides * s,
                     rc.Height - titleH - 30.0f * s - extraLine);
    const Font fAxis = MakeFont(8.0f, s);
    if (plot.Width < 40.0f || plot.Height < 30.0f) { return; }

    // Value range.
    double lo = 1e300, hi = -1e300;
    bool anyBars = false;
    for (size_t k = 0; k < in.seriesCount; ++k) {
        if (in.series[k].tipOnly) { continue; }
        anyBars = anyBars || in.series[k].bars;
        for (size_t i = 0; i < in.n; ++i) {
            const double v = in.series[k].v[i];
            if (std::isnan(v)) { continue; }
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
    }
    if (in.n == 0 || lo > hi) {
        TextC(g, in.empty, MakeFont(9.5f, s), th.textMuted, plot.X + plot.Width / 2.0f, plot.Y + plot.Height / 2.0f - 8.0f * s);
        return;
    }
    if (anyBars) { lo = std::fmin(lo, 0.0); }
    if (hi - lo < 1e-9) { hi = lo + 1.0; }
    if (!anyBars) {
        const double pad = (hi - lo) * 0.06;
        lo -= pad;
        hi += pad;
    }
    const double step = NiceStep((hi - lo) / 5.0);
    lo = std::floor(lo / step + 1e-9) * step;
    hi = std::ceil(hi / step - 1e-9) * step;
    auto Y = [&](double v) { return plot.Y + plot.Height - static_cast<float>((v - lo) / (hi - lo)) * plot.Height; };
    const float slot = plot.Width / static_cast<float>(in.n);
    auto X = [&](size_t i) { return plot.X + (static_cast<float>(i) + 0.5f) * slot; };

    // Grid and y labels.
    Pen grid(th.grid, 1.0f);
    const int ydec = step < 1.0 ? (step < 0.1 ? 2 : 1) : 0;
    for (int k = 0; k < 40; ++k) {
        const double v = lo + step * k;
        if (v > hi + step * 1e-6) { break; }
        const float y = Y(v);
        g.DrawLine(&grid, plot.X, y, plot.X + plot.Width, y);
        const std::wstring t = FmtNumber(v, ydec, L"");
        Text(g, t, fAxis, th.textMuted, plot.X - 6.0f * s - TextWidth(g, t, fAxis), y - FontPx(8.0f, s) * 0.7f);
    }
    if (lo < 0.0 && hi > 0.0) {
        Pen zero(th.textMuted, 1.0f);
        g.DrawLine(&zero, plot.X, Y(0.0), plot.X + plot.Width, Y(0.0));
    }

    // X labels.
    Pen tick(th.textMuted, 1.0f);
    const float axisY = plot.Y + plot.Height;
    if (in.highlight >= 0 && static_cast<size_t>(in.highlight) < in.n) {
        SolidBrush band(th.guide);
        g.FillRectangle(&band, X(static_cast<size_t>(in.highlight)) - slot / 2.0f, plot.Y, slot, plot.Height);
    }
    if (in.axis == XAxis::Custom) {
        for (size_t i = 0; i < in.n; ++i) {
            if (!in.ticks[i].empty()) {
                g.DrawLine(&tick, X(i), axisY, X(i), axisY + 3.0f * s);
                TextC(g, in.ticks[i], fAxis, th.textMuted, X(i), axisY + 4.0f * s);
            }
            if (in.ticks2 != nullptr && !in.ticks2[i].empty()) {
                // Segoe UI Symbol: the second line carries weather symbols, and GDI+ has no font fallback.
                const Font fSym(L"Segoe UI Symbol", FontPx(8.0f, s), FontStyleRegular, UnitPixel);
                TextC(g, in.ticks2[i], fSym, th.textSecondary, X(i), axisY + 4.0f * s + extraLine);
            }
        }
    } else if (in.axis == XAxis::Months) {
        const bool tiny = slot < 26.0f * s;
        for (size_t i = 0; i < in.n && i < 12; ++i) {
            const std::wstring t = tiny ? std::wstring(1, kMonthShort[i][0]) : std::wstring(kMonthShort[i]);
            TextC(g, t, fAxis, th.textMuted, X(i), axisY + 4.0f * s);
        }
    } else {
        const int every = LabelEvery(slot, 46.0f * s);
        for (size_t i = 0; i < in.n; ++i) {
            const int y = in.firstYear + static_cast<int>(i);
            if (y % every != 0) { continue; }
            g.DrawLine(&tick, X(i), axisY, X(i), axisY + 3.0f * s);
            TextC(g, std::to_wstring(y), fAxis, th.textMuted, X(i), axisY + 4.0f * s);
        }
    }

    // Hover slot.
    long hover = -1;
    if (in.hoverX >= plot.X && in.hoverX < plot.X + plot.Width && in.hoverY >= rc.Y && in.hoverY < rc.Y + rc.Height) {
        hover = static_cast<long>((static_cast<float>(in.hoverX) - plot.X) / slot);
        hover = hover < 0 ? 0 : (hover >= static_cast<long>(in.n) ? static_cast<long>(in.n) - 1 : hover);
        SolidBrush guide(th.guide);
        g.FillRectangle(&guide, X(static_cast<size_t>(hover)) - std::fmax(slot, 2.0f) / 2.0f, plot.Y,
                        std::fmax(slot, 2.0f), plot.Height);
    }

    // Bars (grouped when several series are bars).
    size_t nb = 0;
    for (size_t k = 0; k < in.seriesCount; ++k) { nb += (in.series[k].bars && !in.series[k].tipOnly) ? 1 : 0; }
    if (nb > 0) {
        const float group = slot * (nb > 1 ? 0.8f : 0.7f);
        const float bw = group / static_cast<float>(nb);
        const float gap = (nb > 1 && bw > 4.0f * s) ? 1.0f * s : 0.0f;
        const float base = Y(std::fmax(lo, 0.0));
        size_t b = 0;
        for (size_t k = 0; k < in.seriesCount; ++k) {
            const ChartSeries& cs = in.series[k];
            if (!cs.bars || cs.tipOnly) { continue; }
            SolidBrush brush(cs.color);
            for (size_t i = 0; i < in.n; ++i) {
                const double v = cs.v[i];
                if (std::isnan(v) || v <= 0.0) { continue; }
                const float x = X(i) - group / 2.0f + static_cast<float>(b) * bw + gap / 2.0f;
                FillBar(g, brush, x, std::fmax(bw - gap, 1.0f), Y(v), base, s);
            }
            ++b;
        }
    }

    // Lines, broken at gaps; an isolated point gets a dot.
    for (size_t k = 0; k < in.seriesCount; ++k) {
        const ChartSeries& cs = in.series[k];
        if (cs.bars || cs.tipOnly) { continue; }
        Pen pen(cs.color, cs.width * s);
        pen.SetLineJoin(LineJoinRound);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        if (cs.dashed) {
            const REAL dash[] = { 4.0f, 3.0f };
            pen.SetDashPattern(dash, 2);
            pen.SetStartCap(LineCapFlat);
            pen.SetEndCap(LineCapFlat);
        }
        std::array<PointF, kMaxYears> pts{};
        size_t np = 0;
        for (size_t i = 0; i <= in.n; ++i) {
            const bool have = i < in.n && !std::isnan(cs.v[i]);
            if (have) { pts[np++] = PointF(X(i), Y(cs.v[i])); continue; }
            if (np >= 2) { g.DrawLines(&pen, pts.data(), static_cast<INT>(np)); }
            if (np == 1) {
                SolidBrush dot(cs.color);
                const float r = std::fmax(cs.width, 2.0f) * s;
                g.FillEllipse(&dot, pts[0].X - r, pts[0].Y - r, 2 * r, 2 * r);
            }
            np = 0;
        }
    }

    if (hover < 0) { return; }
    const size_t hi_ = static_cast<size_t>(hover);
    std::array<Row, kMaxChartSeries> rows{};
    size_t nr = 0;
    for (size_t k = 0; k < in.seriesCount; ++k) {
        const ChartSeries& cs = in.series[k];
        if (!cs.inTip) { continue; }
        const double v = cs.v[hi_];
        if (!cs.bars && !cs.tipOnly && !std::isnan(v)) {
            SolidBrush fill(cs.color);
            Pen ring(th.bg, 2.0f * s);
            const float r = 4.0f * s;
            g.FillEllipse(&fill, X(hi_) - r, Y(v) - r, 2 * r, 2 * r);
            g.DrawEllipse(&ring, X(hi_) - r, Y(v) - r, 2 * r, 2 * r);
        }
        rows[nr].swatch = cs.color;
        rows[nr].hasSwatch = !cs.tipOnly;
        rows[nr].label = cs.label;
        rows[nr].value = FmtNumber(v, cs.decimals >= 0 ? cs.decimals : in.decimals, cs.unit != nullptr ? cs.unit : in.unit);
        ++nr;
    }
    DrawTip(g, rc, X(hi_), plot.Y + 4.0f * s, SlotLabel(in, hi_), rows.data(), nr, th, s);
}

void DrawHeatmap(Graphics& g, const RectF& rc, const HeatmapInput& in, float s) {
    assert(in.theme != nullptr && in.values != nullptr);
    assert(in.nYears <= kMaxYears && in.limit > 0.0);
    const Theme& th = *in.theme;
    const float titleH = DrawTitleRow(g, rc, in.title, in.subtitle, th, s);
    const Font fAxis = MakeFont(8.0f, s);
    const Color* stops = in.diverging ? th.diverging.data() : th.sequential.data();
    const size_t nStops = in.diverging ? th.diverging.size() : th.sequential.size();

    // Colour scale, right of the title.
    {
        const float w = 120.0f * s;
        const float h = 8.0f * s;
        const std::wstring loT = in.diverging ? FmtNumber(-in.limit, 0, in.unit, true) : FmtNumber(0.0, 0, L"");
        const std::wstring hiT = FmtNumber(in.limit, 0, in.unit, in.diverging) + (in.diverging ? L"" : L"+");
        float x = rc.X + rc.Width - TextWidth(g, hiT, fAxis);
        const float y = rc.Y + 4.0f * s;
        Text(g, hiT, fAxis, th.textMuted, x, y - 2.0f * s);
        x -= w + 4.0f * s;
        for (int i = 0; i < 60; ++i) {
            SolidBrush b(Ramp(stops, nStops, i / 59.0));
            g.FillRectangle(&b, x + w * i / 60.0f, y + 2.0f * s, w / 60.0f + 0.6f, h);
        }
        Text(g, loT, fAxis, th.textMuted, x - 4.0f * s - TextWidth(g, loT, fAxis), y - 2.0f * s);
    }

    const RectF plot(rc.X + 34.0f * s, rc.Y + titleH + 4.0f * s, rc.Width - 38.0f * s, rc.Height - titleH - 26.0f * s);
    if (in.nYears == 0 || plot.Width < 40.0f || plot.Height < 24.0f) { return; }
    const float cw = plot.Width / static_cast<float>(in.nYears);
    const float ch = plot.Height / 12.0f;
    const float gap = cw >= 4.0f * s ? 1.0f : 0.0f;
    for (int m = 0; m < 12; ++m) {
        Text(g, kMonthShort[m], fAxis, th.textMuted, rc.X, plot.Y + ch * m + ch / 2.0f - FontPx(8.0f, s) * 0.7f);
    }
    for (size_t i = 0; i < in.nYears; ++i) {
        for (size_t m = 0; m < 12; ++m) {
            const double v = (*in.values)[i][m];
            if (std::isnan(v)) { continue; }
            const double t = in.diverging ? (v + in.limit) / (2.0 * in.limit) : v / in.limit;
            SolidBrush b(Ramp(stops, nStops, t));
            g.FillRectangle(&b, plot.X + cw * i + gap / 2.0f, plot.Y + ch * m + gap / 2.0f,
                            std::fmax(cw - gap, 0.6f), ch - gap);
        }
    }
    const int every = LabelEvery(cw, 46.0f * s);
    for (size_t i = 0; i < in.nYears; ++i) {
        const int y = in.firstYear + static_cast<int>(i);
        if (y % every != 0) { continue; }
        TextC(g, std::to_wstring(y), fAxis, th.textMuted, plot.X + cw * (i + 0.5f), plot.Y + plot.Height + 4.0f * s);
    }

    if (in.hoverX < plot.X || in.hoverY < plot.Y) { return; }
    const long i = static_cast<long>((in.hoverX - plot.X) / cw);
    const long m = static_cast<long>((in.hoverY - plot.Y) / ch);
    if (i < 0 || i >= static_cast<long>(in.nYears) || m < 0 || m > 11) { return; }
    const double v = (*in.values)[static_cast<size_t>(i)][static_cast<size_t>(m)];
    if (std::isnan(v)) { return; }
    Pen ring(th.text, 1.5f * s);
    g.DrawRectangle(&ring, plot.X + cw * i, plot.Y + ch * m, cw, ch);
    std::array<Row, 2> rows{};
    rows[0].label = in.valueLabel;
    rows[0].value = FmtNumber(v, in.decimals, in.unit, in.signedValues);
    size_t nr = 1;
    if (in.extra != nullptr) {
        rows[1].label = in.extraLabel;
        rows[1].value = FmtNumber((*in.extra)[static_cast<size_t>(i)][static_cast<size_t>(m)], 1, in.unit);
        nr = 2;
    }
    const std::wstring head = std::wstring(kMonthLong[m]) + L" " + std::to_wstring(in.firstYear + i);
    DrawTip(g, rc, plot.X + cw * (i + 1), plot.Y + ch * m - 8.0f * s, head, rows.data(), nr, th, s);
}

} // namespace wt
