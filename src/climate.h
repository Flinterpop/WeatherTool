// Climate data: Open-Meteo replies -> monthly grid, the on-disk cache, and
// the statistics the charts draw (annual and seasonal series, trends,
// rolling means, period normals). UI-free and unit-tested.
#pragma once

#include "common.h"

#include <string>
#include <string_view>

namespace wt {

// ---- Provider replies -------------------------------------------------------

// Daily archive reply -> a fresh monthly grid covering the reply's years.
// A month is kept only when at most one day is missing, so the current,
// unfinished month never drags a mean down. `err` carries the provider's
// own "reason" when it returned an error object.
bool ParseArchive(std::string_view json, Climate& out, std::wstring& err);

// Copies every month that `src` has data for into `dst`, growing `dst`'s
// year range as needed, and takes `src`'s grid-cell metadata.
bool MergeClimate(Climate& dst, const Climate& src, std::wstring& err);

// Geocoding reply -> up to kMaxHits places.
bool ParseGeocode(std::string_view json, std::array<GeoHit, kMaxHits>& hits, size_t& count, std::wstring& err);

// Last month with every variable present, as year*12+month; -1 when none.
int LastCompleteMonth(const Climate& c);

// ---- Cache file (one CSV per place) -------------------------------------

std::string ClimateToCsv(const Climate& c);
bool        ClimateFromCsv(std::string_view text, Climate& out, std::wstring& err);

// ---- Series and statistics -------------------------------------------------

// One value per year; v[i] belongs to year firstYear + i. NaN = no value.
struct YearSeries {
    int    firstYear = kFirstYear;
    size_t n         = 0;
    std::array<double, kMaxYears> v{};
};

// Calendar-year mean of a temperature variable (all 12 months required).
YearSeries AnnualTemp(const Climate& c, Var var);
// Snowfall per winter season: v[i] is July of year i to June of year i+1.
YearSeries SeasonSnow(const Climate& c);
// One month's value in every year.
YearSeries MonthSeries(const Climate& c, Var var, int month);

// Mean of each calendar month over the years [y0, y1].
std::array<double, 12> MonthNormals(const Climate& c, Var var, int y0, int y1);

// Mean of the values for years [y0, y1]; NaN when there are none.
double PeriodMean(const YearSeries& s, int y0, int y1);

// The year span covered by the last `count` years that have a value.
bool LastValidSpan(const YearSeries& s, size_t count, int& y0, int& y1);
int  LastValidYear(const YearSeries& s);   // -1 when none

// Least-squares line through the values from `fromYear` on (x = year).
struct Trend {
    bool   valid     = false;
    double slope     = 0.0;   // per year
    double intercept = 0.0;
    size_t n         = 0;
};
Trend LinearTrend(const YearSeries& s, int fromYear);

// Trailing mean over `window` years, needing `minCount` values; NaN where
// the year itself has no value.
YearSeries Rolling(const YearSeries& s, size_t window, size_t minCount);

// The trend line evaluated over the span that has values, NaN outside it.
YearSeries TrendLine(const YearSeries& s, const Trend& t, int fromYear);

} // namespace wt
