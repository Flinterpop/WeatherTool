#include "climate.h"

#include "jsonscan.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace wt {
namespace {

int DaysInMonth(int y, int m) {
    static constexpr int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    assert(m >= 0 && m < 12);
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return kDays[m] + ((m == 1 && leap) ? 1 : 0);
}

bool ParseDate(const std::wstring& s, int& y, int& m) {
    if (s.size() < 7 || s[4] != L'-') { return false; }
    y = _wtoi(s.substr(0, 4).c_str());
    m = _wtoi(s.substr(5, 2).c_str()) - 1;
    return y >= kFirstYear && y < kFirstYear + static_cast<int>(kMaxYears) && m >= 0 && m < 12;
}

bool InRange(const Climate& c, int y) {
    return y >= c.firstYear && y < c.firstYear + static_cast<int>(c.yearCount);
}

double CellOr(const Climate& c, int y, int m, Var v) {
    if (!InRange(c, y)) { return kNaN; }
    return c.cells[static_cast<size_t>(y - c.firstYear)][static_cast<size_t>(m)].v[static_cast<size_t>(v)];
}

bool HasAny(const MonthCell& cell) {
    for (double v : cell.v) {
        if (!std::isnan(v)) { return true; }
    }
    return false;
}

YearSeries EmptySeries(const Climate& c) {
    YearSeries s;
    s.firstYear = c.firstYear;
    s.n         = c.yearCount;
    s.v.fill(kNaN);
    return s;
}

// Daily accumulators for one variable over the reply's months.
struct Acc {
    std::array<std::array<double, 12>, kMaxYears> sum{};
    std::array<std::array<int, 12>, kMaxYears>    n{};
};

// Reads one named daily array into `acc`, indexed by the parsed dates.
bool Accumulate(std::string_view j, size_t dailyPos, size_t dailyEnd, const char* key,
                const std::array<int, kMaxYears * 12 * 31 + 1>& dayMonth, size_t days, Acc& acc) {
    const size_t at = json::FindKey(j, key, dailyPos, dailyEnd);
    if (at == json::npos) { return false; }
    json::ArrayReader r(j, at);
    double v = 0.0;
    size_t i = 0;
    while (i < days && r.NextNumber(v)) {
        const int ym = dayMonth[i];
        ++i;
        if (std::isnan(v) || ym < 0) { continue; }
        const size_t yi = static_cast<size_t>(ym / 12);
        const size_t mi = static_cast<size_t>(ym % 12);
        acc.sum[yi][mi] += v;
        acc.n[yi][mi]   += 1;
    }
    return r.ok() && i == days;
}

} // namespace

bool ParseArchive(std::string_view j, Climate& out, std::wstring& err) {
    bool isErr = false;
    size_t p = json::FindKey(j, "error", 0, j.size());
    if (p != json::npos && json::ReadBool(j, p, isErr) && isErr) {
        size_t r = json::FindKey(j, "reason", 0, j.size());
        std::wstring reason;
        err = (r != json::npos && json::ReadString(j, r, reason)) ? reason : L"provider returned an error";
        return false;
    }
    const size_t daily = json::FindKey(j, "daily", 0, j.size());
    const size_t dailyEnd = (daily == json::npos) ? json::npos : json::SkipContainer(j, daily);
    if (daily == json::npos || dailyEnd == json::npos) {
        err = L"reply has no daily data";
        return false;
    }

    // Dates first: map each day to a (year - kFirstYear)*12 + month slot.
    auto dayMonth = std::make_unique<std::array<int, kMaxYears * 12 * 31 + 1>>();
    size_t days = 0;
    int minY = 99999;
    int maxY = -1;
    {
        const size_t t = json::FindKey(j, "time", daily, dailyEnd);
        json::ArrayReader r(j, t == json::npos ? j.size() : t);
        std::wstring s;
        while (days < dayMonth->size() && r.NextString(s)) {
            int y = 0;
            int m = 0;
            if (!ParseDate(s, y, m)) {
                err = L"bad date in reply: " + s;
                return false;
            }
            (*dayMonth)[days++] = (y - kFirstYear) * 12 + m;
            minY = (y < minY) ? y : minY;
            maxY = (y > maxY) ? y : maxY;
        }
        if (!r.ok() || days == 0) {
            err = L"reply has no dates";
            return false;
        }
    }

    static const char* const kKeys[kVarCount] = { "temperature_2m_mean", "temperature_2m_max",
                                                 "temperature_2m_min", "snowfall_sum" };
    auto acc = std::make_unique<std::array<Acc, kVarCount>>();
    for (size_t k = 0; k < kVarCount; ++k) {
        if (!Accumulate(j, daily, dailyEnd, kKeys[k], *dayMonth, days, (*acc)[k])) {
            err = L"reply is missing or has a malformed daily series";
            return false;
        }
    }
    // Days actually present per month in the reply (the first/last month of
    // an incremental reply may be partial on purpose).
    std::array<std::array<int, 12>, kMaxYears> present{};
    for (size_t i = 0; i < days; ++i) {
        const int ym = (*dayMonth)[i];
        present[static_cast<size_t>(ym / 12)][static_cast<size_t>(ym % 12)] += 1;
    }

    out = Climate{};
    out.firstYear = minY;
    out.yearCount = static_cast<size_t>(maxY - minY + 1);
    for (auto& row : out.cells) { row.fill(MonthCell{}); }
    for (int y = minY; y <= maxY; ++y) {
        const size_t yi = static_cast<size_t>(y - kFirstYear);
        for (int m = 0; m < 12; ++m) {
            const int need = DaysInMonth(y, m) - 1;
            if (present[yi][static_cast<size_t>(m)] < need) { continue; }   // month not wholly in this reply
            for (size_t k = 0; k < kVarCount; ++k) {
                const int n = (*acc)[k].n[yi][static_cast<size_t>(m)];
                if (n < need) { continue; }
                const double sum = (*acc)[k].sum[yi][static_cast<size_t>(m)];
                Cell(out, y, m, static_cast<Var>(k)) = (k == static_cast<size_t>(Var::Snow)) ? sum : sum / n;
            }
        }
    }
    double v = 0.0;
    if ((p = json::FindKey(j, "latitude", 0, daily)) != json::npos && json::ReadNumber(j, p, v)) { out.lat = v; }
    if ((p = json::FindKey(j, "longitude", 0, daily)) != json::npos && json::ReadNumber(j, p, v)) { out.lon = v; }
    if ((p = json::FindKey(j, "elevation", 0, daily)) != json::npos && json::ReadNumber(j, p, v)) { out.elevation = v; }
    std::wstring tz;
    if ((p = json::FindKey(j, "timezone", 0, daily)) != json::npos && json::ReadString(j, p, tz)) { out.timezone = tz; }
    out.valid = true;
    return true;
}

bool MergeClimate(Climate& dst, const Climate& src, std::wstring& err) {
    if (!src.valid) {
        err = L"nothing to merge";
        return false;
    }
    if (!dst.valid) {
        dst = src;
        return true;
    }
    assert(dst.firstYear == kFirstYear || dst.yearCount == 0 || dst.firstYear >= kFirstYear);
    // Re-base dst so both ranges fit.
    const int lo = (src.firstYear < dst.firstYear) ? src.firstYear : dst.firstYear;
    const int hiSrc = src.firstYear + static_cast<int>(src.yearCount);
    const int hiDst = dst.firstYear + static_cast<int>(dst.yearCount);
    const int hi = (hiSrc > hiDst) ? hiSrc : hiDst;
    if (lo < kFirstYear || hi - lo > static_cast<int>(kMaxYears)) {
        err = L"years out of range";
        return false;
    }
    if (lo != dst.firstYear) {
        const size_t shift = static_cast<size_t>(dst.firstYear - lo);
        for (size_t i = kMaxYears; i-- > 0;) {
            dst.cells[i] = (i >= shift) ? dst.cells[i - shift] : std::array<MonthCell, 12>{};
        }
        dst.firstYear = lo;
    }
    for (size_t i = static_cast<size_t>(hiDst - lo); i < kMaxYears; ++i) { dst.cells[i].fill(MonthCell{}); }
    dst.yearCount = static_cast<size_t>(hi - lo);
    for (int y = src.firstYear; y < hiSrc; ++y) {
        for (int m = 0; m < 12; ++m) {
            const MonthCell& s = src.cells[static_cast<size_t>(y - src.firstYear)][static_cast<size_t>(m)];
            if (HasAny(s)) { dst.cells[static_cast<size_t>(y - lo)][static_cast<size_t>(m)] = s; }
        }
    }
    dst.lat       = src.lat;
    dst.lon       = src.lon;
    dst.elevation = src.elevation;
    if (!src.timezone.empty()) { dst.timezone = src.timezone; }
    return true;
}

bool ParseGeocode(std::string_view j, std::array<GeoHit, kMaxHits>& hits, size_t& count, std::wstring& err) {
    count = 0;
    bool isErr = false;
    size_t p = json::FindKey(j, "error", 0, j.size());
    if (p != json::npos && json::ReadBool(j, p, isErr) && isErr) {
        size_t r = json::FindKey(j, "reason", 0, j.size());
        std::wstring reason;
        err = (r != json::npos && json::ReadString(j, r, reason)) ? reason : L"provider returned an error";
        return false;
    }
    const size_t res = json::FindKey(j, "results", 0, j.size());
    if (res == json::npos) { return true; }   // no matches: the key is simply absent
    json::ArrayReader r(j, res);
    size_t obj = 0;
    while (count < hits.size() && r.NextElement(obj)) {
        const size_t end = json::SkipContainer(j, obj);
        if (end == json::npos) { break; }
        GeoHit h;
        double v = 0.0;
        size_t q = 0;
        if ((q = json::FindKey(j, "name", obj, end)) == json::npos || !json::ReadString(j, q, h.name)) { continue; }
        if ((q = json::FindKey(j, "latitude", obj, end)) == json::npos || !json::ReadNumber(j, q, h.lat)) { continue; }
        if ((q = json::FindKey(j, "longitude", obj, end)) == json::npos || !json::ReadNumber(j, q, h.lon)) { continue; }
        if ((q = json::FindKey(j, "elevation", obj, end)) != json::npos && json::ReadNumber(j, q, v)) { h.elevation = v; }
        if ((q = json::FindKey(j, "admin1", obj, end)) != json::npos) { (void)json::ReadString(j, q, h.admin1); }
        if ((q = json::FindKey(j, "country_code", obj, end)) != json::npos) { (void)json::ReadString(j, q, h.country); }
        hits[count++] = h;
    }
    if (!r.ok()) {
        err = L"malformed search reply";
        return false;
    }
    return true;
}

int LastCompleteMonth(const Climate& c) {
    for (size_t i = c.yearCount; i-- > 0;) {
        for (int m = 11; m >= 0; --m) {
            const MonthCell& cell = c.cells[i][static_cast<size_t>(m)];
            bool all = true;
            for (double v : cell.v) { all = all && !std::isnan(v); }
            if (all) { return (c.firstYear + static_cast<int>(i)) * 12 + m; }
        }
    }
    return -1;
}

// ---- Cache ------------------------------------------------------------------

std::string ClimateToCsv(const Climate& c) {
    std::string out;
    out.reserve(64 + c.yearCount * 12 * 48);
    char line[256];
    std::snprintf(line, sizeof(line), "# weathertool 1,%.5f,%.5f,%.1f,%lld\n", c.lat, c.lon,
                  std::isnan(c.elevation) ? -9999.0 : c.elevation, static_cast<long long>(c.fetched));
    out += line;
    out += "year,month,mean_c,high_c,low_c,snow_cm\n";
    for (size_t i = 0; i < c.yearCount; ++i) {
        for (size_t m = 0; m < 12; ++m) {
            const MonthCell& cell = c.cells[i][m];
            if (!HasAny(cell)) { continue; }
            int n = std::snprintf(line, sizeof(line), "%d,%zu", c.firstYear + static_cast<int>(i), m + 1);
            for (double v : cell.v) {
                n += std::isnan(v) ? std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), ",")
                                   : std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), ",%.2f", v);
            }
            out += line;
            out += '\n';
        }
    }
    return out;
}

bool ClimateFromCsv(std::string_view text, Climate& out, std::wstring& err) {
    out = Climate{};
    for (auto& row : out.cells) { row.fill(MonthCell{}); }
    size_t pos = 0;
    int lineNo = 0;
    int minY = 99999;
    int maxY = -1;
    while (pos < text.size() && lineNo < 20000) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) { eol = text.size(); }
        std::string line(text.substr(pos, eol - pos));
        pos = eol + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (lineNo == 1) {
            double lat = 0, lon = 0, elev = 0;
            long long fetched = 0;
            if (sscanf_s(line.c_str(), "# weathertool 1,%lf,%lf,%lf,%lld", &lat, &lon, &elev, &fetched) != 4) {
                err = L"not a WeatherTool cache file";
                return false;
            }
            out.lat = lat;
            out.lon = lon;
            out.elevation = (elev <= -9998.0) ? kNaN : elev;
            out.fetched = fetched;
            continue;
        }
        if (line.empty() || line[0] < '0' || line[0] > '9') { continue; }   // header / blank
        // year,month then four optional numbers
        const char* s = line.c_str();
        char* e = nullptr;
        const long y = std::strtol(s, &e, 10);
        if (*e != ',') { continue; }
        const long m = std::strtol(e + 1, &e, 10) - 1;
        if (y < kFirstYear || y >= kFirstYear + static_cast<long>(kMaxYears) || m < 0 || m > 11) {
            err = L"cache row out of range";
            return false;
        }
        MonthCell cell;
        for (size_t k = 0; k < kVarCount && *e == ','; ++k) {
            const char* f = e + 1;
            if (*f == ',' || *f == '\0') { e = const_cast<char*>(f); continue; }
            cell.v[k] = std::strtod(f, &e);
        }
        out.cells[static_cast<size_t>(y - kFirstYear)][static_cast<size_t>(m)] = cell;
        minY = (y < minY) ? static_cast<int>(y) : minY;
        maxY = (y > maxY) ? static_cast<int>(y) : maxY;
    }
    if (maxY < 0) {
        err = L"cache file has no data";
        return false;
    }
    // Rows were stored at kFirstYear-based slots; trim to the span present.
    const size_t shift = static_cast<size_t>(minY - kFirstYear);
    for (size_t i = 0; i + shift < kMaxYears; ++i) { out.cells[i] = out.cells[i + shift]; }
    out.firstYear = minY;
    out.yearCount = static_cast<size_t>(maxY - minY + 1);
    for (size_t i = out.yearCount; i < kMaxYears; ++i) { out.cells[i].fill(MonthCell{}); }
    out.valid = true;
    return true;
}

// ---- Series -------------------------------------------------------------------

YearSeries AnnualTemp(const Climate& c, Var var) {
    assert(var != Var::Snow);
    YearSeries s = EmptySeries(c);
    for (size_t i = 0; i < c.yearCount; ++i) {
        double sum = 0.0;
        int n = 0;
        for (size_t m = 0; m < 12; ++m) {
            const double v = c.cells[i][m].v[static_cast<size_t>(var)];
            if (!std::isnan(v)) { sum += v; ++n; }
        }
        s.v[i] = (n == 12) ? sum / 12.0 : kNaN;
    }
    return s;
}

YearSeries SeasonSnow(const Climate& c) {
    YearSeries s = EmptySeries(c);
    for (size_t i = 0; i < c.yearCount; ++i) {
        const int y = c.firstYear + static_cast<int>(i);
        double sum = 0.0;
        bool all = true;
        for (int k = 0; k < 12 && all; ++k) {
            const int m  = (6 + k) % 12;
            const int yy = (k < 6) ? y : y + 1;
            const double v = CellOr(c, yy, m, Var::Snow);
            all = !std::isnan(v);
            sum += all ? v : 0.0;
        }
        s.v[i] = all ? sum : kNaN;
    }
    return s;
}

YearSeries MonthSeries(const Climate& c, Var var, int month) {
    assert(month >= 0 && month < 12);
    YearSeries s = EmptySeries(c);
    for (size_t i = 0; i < c.yearCount; ++i) {
        s.v[i] = c.cells[i][static_cast<size_t>(month)].v[static_cast<size_t>(var)];
    }
    return s;
}

std::array<double, 12> MonthNormals(const Climate& c, Var var, int y0, int y1) {
    std::array<double, 12> out{};
    for (int m = 0; m < 12; ++m) {
        out[static_cast<size_t>(m)] = PeriodMean(MonthSeries(c, var, m), y0, y1);
    }
    return out;
}

double PeriodMean(const YearSeries& s, int y0, int y1) {
    double sum = 0.0;
    int n = 0;
    for (size_t i = 0; i < s.n; ++i) {
        const int y = s.firstYear + static_cast<int>(i);
        if (y < y0 || y > y1 || std::isnan(s.v[i])) { continue; }
        sum += s.v[i];
        ++n;
    }
    return n > 0 ? sum / n : kNaN;
}

int LastValidYear(const YearSeries& s) {
    for (size_t i = s.n; i-- > 0;) {
        if (!std::isnan(s.v[i])) { return s.firstYear + static_cast<int>(i); }
    }
    return -1;
}

bool LastValidSpan(const YearSeries& s, size_t count, int& y0, int& y1) {
    assert(count > 0);
    size_t found = 0;
    for (size_t i = s.n; i-- > 0 && found < count;) {
        if (std::isnan(s.v[i])) { continue; }
        const int y = s.firstYear + static_cast<int>(i);
        if (found == 0) { y1 = y; }
        y0 = y;
        ++found;
    }
    return found == count;
}

Trend LinearTrend(const YearSeries& s, int fromYear) {
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < s.n; ++i) {
        const int y = s.firstYear + static_cast<int>(i);
        if (y < fromYear || std::isnan(s.v[i])) { continue; }
        const double x = y;
        n += 1; sx += x; sy += s.v[i]; sxx += x * x; sxy += x * s.v[i];
    }
    Trend t;
    const double den = n * sxx - sx * sx;
    if (n < 10 || den == 0.0) { return t; }
    t.slope     = (n * sxy - sx * sy) / den;
    t.intercept = (sy - t.slope * sx) / n;
    t.n         = static_cast<size_t>(n);
    t.valid     = true;
    return t;
}

YearSeries Rolling(const YearSeries& s, size_t window, size_t minCount) {
    assert(window > 0 && minCount <= window);
    YearSeries out = s;
    for (size_t i = 0; i < s.n; ++i) {
        out.v[i] = kNaN;
        if (std::isnan(s.v[i]) || i + 1 < window) { continue; }
        double sum = 0.0;
        size_t n = 0;
        for (size_t k = i + 1 - window; k <= i; ++k) {
            if (!std::isnan(s.v[k])) { sum += s.v[k]; ++n; }
        }
        out.v[i] = (n >= minCount) ? sum / static_cast<double>(n) : kNaN;
    }
    return out;
}

YearSeries TrendLine(const YearSeries& s, const Trend& t, int fromYear) {
    YearSeries out = s;
    out.v.fill(kNaN);
    if (!t.valid) { return out; }
    int lo = -1;
    for (size_t i = 0; i < s.n; ++i) {
        if (s.firstYear + static_cast<int>(i) >= fromYear && !std::isnan(s.v[i])) { lo = static_cast<int>(i); break; }
    }
    const int hiYear = LastValidYear(s);
    if (lo < 0 || hiYear < 0) { return out; }
    for (size_t i = static_cast<size_t>(lo); i < s.n; ++i) {
        const int y = s.firstYear + static_cast<int>(i);
        if (y > hiYear) { break; }
        out.v[i] = t.intercept + t.slope * y;
    }
    return out;
}

} // namespace wt
