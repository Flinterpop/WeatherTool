// Unit tests for the UI-free core. No framework: each CHECK prints the
// failing expression and the exe exits non-zero when any failed.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "climate.h"
#include "config.h"
#include "forecast.h"
#include "http.h"
#include "jsonscan.h"
#include "radar.h"
#include "slippy.h"
#include "units.h"
#include "update.h"
#include "webserver.h"

#include <cstring>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failed = 0;
int g_checks = 0;

#define CHECK(expr)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(expr)) {                                                                \
            ++g_failed;                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);               \
        }                                                                             \
    } while (0)

bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

using namespace wt;

int DaysIn(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m] + ((m == 1 && y % 4 == 0) ? 1 : 0);
}

// Daily archive reply for [y0, y1]: mean = month number, high = mean + 5,
// low = mean - 5, snow 1 cm a day in January only. `nullDays` days of
// March 2000's mean are null.
std::string MakeArchive(int y0, int y1, int nullDays, int stopAfterDayOfLastMonth = 0) {
    std::string t, mean, hi, lo, snow;
    for (int y = y0; y <= y1; ++y) {
        for (int m = 0; m < 12; ++m) {
            int days = DaysIn(y, m);
            if (y == y1 && m == 11 && stopAfterDayOfLastMonth > 0) { days = stopAfterDayOfLastMonth; }
            for (int d = 1; d <= days; ++d) {
                char b[32];
                std::snprintf(b, sizeof(b), "\"%04d-%02d-%02d\"", y, m + 1, d);
                const char* sep = t.empty() ? "" : ",";
                t += sep; t += b;
                const bool isNull = (y == 2000 && m == 2 && d <= nullDays);
                std::snprintf(b, sizeof(b), "%d", m + 1);
                mean += sep; mean += isNull ? "null" : b;
                std::snprintf(b, sizeof(b), "%d", m + 6);
                hi += sep; hi += b;
                std::snprintf(b, sizeof(b), "%d", m - 4);
                lo += sep; lo += b;
                snow += sep; snow += (m == 0) ? "1.0" : "0.0";
            }
        }
    }
    return std::string("{\"latitude\":45.4,\"longitude\":-75.7,\"elevation\":75.0,\"timezone\":\"America/Toronto\",") +
           "\"daily_units\":{\"time\":\"iso8601\",\"temperature_2m_mean\":\"\xC2\xB0" "C\"}," +
           "\"daily\":{\"time\":[" + t + "],\"temperature_2m_mean\":[" + mean + "],\"temperature_2m_max\":[" + hi +
           "],\"temperature_2m_min\":[" + lo + "],\"snowfall_sum\":[" + snow + "]}}";
}

void TestJsonScan() {
    const std::string j = R"({"a":{"name":"Montréal","n":-1.5e1,"x":null},"list":[1, 2 ,3],"b":true})";
    size_t p = json::FindKey(j, "name", 0, j.size());
    std::wstring s;
    CHECK(p != json::npos && json::ReadString(j, p, s) && s == L"Montréal");
    double v = 0;
    p = json::FindKey(j, "n", 0, j.size());
    CHECK(p != json::npos && json::ReadNumber(j, p, v) && v == -15.0);
    p = json::FindKey(j, "x", 0, j.size());
    CHECK(p != json::npos && json::ReadNumber(j, p, v) && std::isnan(v));
    json::ArrayReader r(j, json::FindKey(j, "list", 0, j.size()));
    double sum = 0;
    int n = 0;
    while (r.NextNumber(v)) { sum += v; ++n; }
    CHECK(r.ok() && n == 3 && sum == 6.0);
    // A key that only appears as a string value is not a key.
    const std::string k = R"({"v":"time","time":[1]})";
    p = json::FindKey(k, "time", 0, k.size());
    CHECK(p != json::npos && k[p] == '[');
    // Raw UTF-8 decodes too.
    const std::string u = "{\"n\":\"Z\xC3\xBCrich\"}";
    p = json::FindKey(u, "n", 0, u.size());
    CHECK(json::ReadString(u, p, s) && s == L"Zürich");
    // Malformed array is reported.
    const std::string bad = R"({"a":[1,2 3]})";
    json::ArrayReader rb(bad, json::FindKey(bad, "a", 0, bad.size()));
    while (rb.NextNumber(v)) {}
    CHECK(!rb.ok());
}

void TestParseArchive() {
    Climate c;
    std::wstring err;
    CHECK(ParseArchive(MakeArchive(2000, 2001, 1), c, err));
    CHECK(c.valid && c.firstYear == 2000 && c.yearCount == 2);
    CHECK(Near(c.lat, 45.4) && Near(c.elevation, 75.0) && c.timezone == L"America/Toronto");
    CHECK(Near(Cell(c, 2000, 0, Var::Mean), 1.0));
    CHECK(Near(Cell(c, 2001, 11, Var::High), 17.0));
    CHECK(Near(Cell(c, 2000, 0, Var::Snow), 31.0));          // a sum, not a mean
    CHECK(Near(Cell(c, 2000, 1, Var::Snow), 0.0));
    CHECK(Near(Cell(c, 2000, 2, Var::Mean), 3.0));           // one missing day is tolerated

    Climate c2;
    CHECK(ParseArchive(MakeArchive(2000, 2000, 2), c2, err));
    CHECK(std::isnan(Cell(c2, 2000, 2, Var::Mean)));         // two missing days are not
    CHECK(!std::isnan(Cell(c2, 2000, 2, Var::High)));

    // An unfinished last month is left out.
    Climate c3;
    CHECK(ParseArchive(MakeArchive(2000, 2000, 0, 10), c3, err));
    CHECK(std::isnan(Cell(c3, 2000, 11, Var::Mean)) && !std::isnan(Cell(c3, 2000, 10, Var::Mean)));
    CHECK(LastCompleteMonth(c3) == 2000 * 12 + 10);

    // Provider error objects surface their reason.
    Climate e;
    CHECK(!ParseArchive(R"({"error":true,"reason":"Daily API request limit exceeded"})", e, err));
    CHECK(err == L"Daily API request limit exceeded");
}

void TestMergeAndCache() {
    std::wstring err;
    Climate base;
    CHECK(ParseArchive(MakeArchive(2000, 2000, 0, 10), base, err));
    Climate upd;
    CHECK(ParseArchive(MakeArchive(2000, 2001, 0), upd, err));
    Cell(upd, 2000, 0, Var::Mean) = 99.0;
    CHECK(MergeClimate(base, upd, err));
    CHECK(base.firstYear == 2000 && base.yearCount == 2);
    CHECK(Near(Cell(base, 2000, 11, Var::Mean), 12.0));      // filled in
    CHECK(Near(Cell(base, 2000, 0, Var::Mean), 99.0));       // replaced
    CHECK(Near(Cell(base, 2001, 5, Var::Low), 1.0));

    // Merging an earlier span re-bases the grid.
    Climate early;
    CHECK(ParseArchive(MakeArchive(1998, 1998, 0), early, err));
    CHECK(MergeClimate(base, early, err));
    CHECK(base.firstYear == 1998 && base.yearCount == 4);
    CHECK(Near(Cell(base, 2000, 0, Var::Mean), 99.0) && Near(Cell(base, 1998, 3, Var::Mean), 4.0));
    CHECK(std::isnan(Cell(base, 1999, 3, Var::Mean)));

    base.fetched = 1700000000;
    const std::string csv = ClimateToCsv(base);
    Climate back;
    CHECK(ClimateFromCsv(csv, back, err));
    CHECK(back.firstYear == 1998 && back.yearCount == 4 && back.fetched == 1700000000);
    CHECK(Near(Cell(back, 2000, 0, Var::Mean), 99.0) && Near(Cell(back, 2001, 0, Var::Snow), 31.0));
    CHECK(std::isnan(Cell(back, 1999, 3, Var::Mean)));
    CHECK(!ClimateFromCsv("year,month\n2000,1,1,2,3,4\n", back, err));   // no signature line
}

void TestSeries() {
    std::wstring err;
    Climate c;
    CHECK(ParseArchive(MakeArchive(2000, 2002, 0, 10), c, err));
    const YearSeries a = AnnualTemp(c, Var::Mean);
    CHECK(Near(a.v[0], 6.5) && Near(a.v[1], 6.5) && std::isnan(a.v[2]));   // 2002 lacks December
    CHECK(LastValidYear(a) == 2001);
    const YearSeries s = SeasonSnow(c);
    CHECK(Near(s.v[0], 31.0) && Near(s.v[1], 31.0) && std::isnan(s.v[2]));  // 2002-03 not over
    const YearSeries m = MonthSeries(c, Var::High, 6);
    CHECK(Near(m.v[2], 12.0));
    const auto norms = MonthNormals(c, Var::Mean, 2000, 2001);
    CHECK(Near(norms[0], 1.0) && Near(norms[11], 12.0));

    YearSeries line;
    line.firstYear = 1950;
    line.n = 30;
    for (size_t i = 0; i < line.n; ++i) { line.v[i] = 2.0 + 0.1 * static_cast<double>(i); }
    line.v[5] = kNaN;
    const Trend t = LinearTrend(line, 1950);
    CHECK(t.valid && Near(t.slope, 0.1, 1e-9) && t.n == 29);
    CHECK(Near(t.intercept + t.slope * 1960, 3.0, 1e-9));
    CHECK(!LinearTrend(line, 1975).valid);                    // fewer than 10 points
    const YearSeries r = Rolling(line, 10, 8);
    CHECK(std::isnan(r.v[8]) && !std::isnan(r.v[9]) && std::isnan(r.v[5]));
    CHECK(Near(r.v[29], (4.9 + 4.0) / 2.0, 1e-9));
    const YearSeries tl = TrendLine(line, t, 1960);
    CHECK(std::isnan(tl.v[9]) && Near(tl.v[10], 3.0, 1e-9));
    int y0 = 0, y1 = 0;
    CHECK(LastValidSpan(line, 10, y0, y1) && y0 == 1970 && y1 == 1979);
    CHECK(Near(PeriodMean(line, 1950, 1951), 2.05, 1e-9));
}

void TestGeocode() {
    const std::string j =
        R"({"results":[{"id":1,"name":"Ottawa","latitude":45.41117,"longitude":-75.69812,"elevation":71.0,)"
        R"("country_code":"CA","admin1":"Ontario","postcodes":["K1A"]},)"
        R"({"id":2,"name":"Ottawa","latitude":41.34559,"longitude":-88.84258,"country_code":"US","admin1":"Illinois"}],)"
        R"("generationtime_ms":0.5})";
    std::array<GeoHit, kMaxHits> hits{};
    size_t n = 0;
    std::wstring err;
    CHECK(ParseGeocode(j, hits, n, err) && n == 2);
    CHECK(hits[0].admin1 == L"Ontario" && hits[0].country == L"CA" && Near(hits[0].lon, -75.69812));
    CHECK(hits[1].admin1 == L"Illinois" && std::isnan(hits[1].elevation));
    CHECK(ParseGeocode(R"({"generationtime_ms":0.3})", hits, n, err) && n == 0);
}

void TestUnits() {
    CHECK(FmtTemp(0.0, Units::Imperial) == L"32.0 °F");
    CHECK(FmtDelta(1.2, Units::Metric) == L"+1.20 °C");
    CHECK(FmtDelta(-0.001, Units::Metric) == L"±0.00 °C");
    CHECK(FmtSnow(254.0, Units::Imperial) == L"100.0 in");
    CHECK(FmtSnow(kNaN, Units::Metric) == L"-");
    CHECK(SeasonLabel(1999) == L"1999–00");
    CHECK(FmtTemp(-1.84, Units::Metric) == L"−1.8 °C");   // a real minus sign
    CHECK(FmtTemp(-0.04, Units::Metric) == L"0.0 °C");         // no "-0.0"
    CHECK(AgoText(-30) == L"just now" && AgoText(59) == L"just now");
    CHECK(AgoText(60) == L"1 minute ago" && AgoText(40 * 60 + 59) == L"40 minutes ago");
    CHECK(AgoText(3600) == L"1 hour ago" && AgoText(3600 + 5 * 60) == L"1 hour 5 minutes ago");
    CHECK(AgoText(2 * 3600 + 60) == L"2 hours 1 minute ago" && AgoText(23 * 3600 + 59 * 60) == L"23 hours 59 minutes ago");
    CHECK(AgoText(86400) == L"1 day ago" && AgoText(3 * 86400 + 2 * 3600 + 30 * 60) == L"3 days 2 hours ago");
}

// A forecast reply laid out as Open-Meteo sends it, *_units sections first
// (they repeat every key, so a lookup that strays out of its section would
// read a unit string instead of a number). Hourly from 2026-11-21T00:00 at
// UTC-5, as the API sends it without forecast_hours: temperature = hour
// index, snowfall 0.1 cm in hour 3 only, wind = 10, gusts = 20, cloud = 50
// (left out when !withWind, like a cached reply from 0.5.0). Daily from
// 2026-11-21: max = 10 + day, min = day - 10, snow 2 cm on day 1.
// "2026-11-21" + d days (Nov has 30), for d < 40.
std::string DateAfter(size_t d) {
    char b[16];
    std::snprintf(b, sizeof(b), "2026-%02d-%02zu", d < 10 ? 11 : 12, d < 10 ? 21 + d : d - 9);
    return b;
}

std::string MakeForecast(size_t hours, size_t days, bool shortHourly, bool withWind = true) {
    std::string ht, htemp, hfeel, hprob, hpre, hsnow, hcode, hwind, hgust, hcloud;
    for (size_t i = 0; i < hours; ++i) {
        char b[48];
        std::snprintf(b, sizeof(b), "\"%sT%02zu:00\"", DateAfter(i / 24).c_str(), i % 24);
        const char* sep = i == 0 ? "" : ",";
        ht += sep; ht += b;
        std::snprintf(b, sizeof(b), "%zu", i);
        htemp += sep; htemp += b;
        hfeel += sep; hfeel += b;
        hprob += sep; hprob += "40";
        hpre += sep; hpre += (i == 3) ? "0.1" : "0.0";
        hsnow += sep; hsnow += (i == 3) ? "0.1" : "0.0";
        hwind += sep; hwind += "10";
        hgust += sep; hgust += "20";
        hcloud += sep; hcloud += "50";
        if (!(shortHourly && i + 1 == hours)) { hcode += sep; hcode += (i == 3) ? "71" : "3"; }
    }
    const std::string windPart = withWind ? ",\"wind_speed_10m\":[" + hwind + "],\"wind_gusts_10m\":[" + hgust +
                                                "],\"cloud_cover\":[" + hcloud + "]"
                                          : std::string();
    std::string dt, dmax, dmin, dpre, dsnow, dprob, dcode;
    for (size_t i = 0; i < days; ++i) {
        const char* sep = i == 0 ? "" : ",";
        dt += sep; dt += "\"" + DateAfter(i) + "\"";
        dmax += sep; dmax += std::to_string(10 + i);
        dmin += sep; dmin += std::to_string(static_cast<int>(i) - 10);
        dpre += sep; dpre += (i == 1) ? "2.0" : "0.0";
        dsnow += sep; dsnow += (i == 1) ? "2.0" : "0.0";
        dprob += sep; dprob += "10";
        dcode += sep; dcode += (i == 1) ? "73" : "0";
    }
    return std::string("{\"latitude\":45.406376,\"longitude\":-75.71817,\"utc_offset_seconds\":-18000,") +
           "\"timezone\":\"America/Toronto\",\"timezone_abbreviation\":\"EST\",\"elevation\":74.0," +
           "\"current_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\xC2\xB0" "C\",\"snow_depth\":\"m\"}," +
           "\"current\":{\"time\":\"2026-11-21T16:15\",\"interval\":900,\"temperature_2m\":-3.4,\"apparent_temperature\":-8.1," +
           "\"relative_humidity_2m\":90,\"precipitation\":0.20,\"snowfall\":0.14,\"snow_depth\":0.12,\"weather_code\":71," +
           "\"wind_speed_10m\":20.5,\"wind_direction_10m\":225,\"wind_gusts_10m\":41.0,\"is_day\":0}," +
           "\"hourly_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\xC2\xB0" "C\"}," +
           "\"hourly\":{\"time\":[" + ht + "],\"temperature_2m\":[" + htemp + "],\"apparent_temperature\":[" + hfeel +
           "],\"precipitation_probability\":[" + hprob + "],\"precipitation\":[" + hpre + "],\"snowfall\":[" + hsnow +
           "],\"weather_code\":[" + hcode + "]" + windPart + "}," +
           "\"daily_units\":{\"time\":\"iso8601\",\"temperature_2m_max\":\"\xC2\xB0" "C\"}," +
           "\"daily\":{\"time\":[" + dt + "],\"weather_code\":[" + dcode + "],\"temperature_2m_max\":[" + dmax +
           "],\"temperature_2m_min\":[" + dmin + "],\"precipitation_sum\":[" + dpre + "],\"snowfall_sum\":[" + dsnow +
           "],\"precipitation_probability_max\":[" + dprob + "]}}";
}

void TestForecast() {
    auto fp = std::make_unique<Forecast>();   // 384 hours: kept off the stack
    Forecast& f = *fp;
    std::wstring err;
    CHECK(ParseForecast(MakeForecast(kForecastHours, 16, false), f, err) && f.valid);
    CHECK(f.timezone == L"America/Toronto" && f.tzAbbrev == L"EST" && f.utcOffset == -18000);
    CHECK(Near(f.temp, -3.4) && Near(f.feels, -8.1) && Near(f.humidity, 90.0));
    CHECK(Near(f.snowDepth, 12.0) && Near(f.snowfall, 0.14) && f.code == 71 && !f.isDay);
    CHECK(Near(f.wind, 20.5) && Near(f.windDir, 225.0) && Near(f.gust, 41.0));
    CHECK(f.nowTime.year == 2026 && f.nowTime.month == 11 && f.nowTime.day == 21 && f.nowTime.hour == 16 && f.nowTime.minute == 15);
    CHECK(f.hours == kForecastHours && f.days == 16);
    CHECK(Near(f.hTemp[383], 383.0) && f.hCode[3] == 71 && f.hCode[4] == 3 && Near(f.hSnow[3], 0.1));
    CHECK(Near(f.hWind[383], 10.0) && Near(f.hGust[0], 20.0) && Near(f.hCloud[100], 50.0));
    CHECK(f.hTime[24].day == 22 && f.hTime[24].hour == 0 && f.hTime[240].month == 12 && f.hTime[240].day == 1);
    CHECK(Near(f.dMax[15], 25.0) && Near(f.dMin[0], -10.0) && Near(f.dSnow[1], 2.0) && f.dCode[1] == 73);
    CHECK(f.dTime[10].month == 12 && f.dTime[10].day == 1);

    // Each day's hours, across the month end; none for a day past the hours.
    size_t first = 0, count = 0;
    HoursOfDay(f, 0, first, count);
    CHECK(first == 0 && count == 24);
    HoursOfDay(f, 10, first, count);   // 1 December
    CHECK(first == 240 && count == 24 && f.hTime[first].day == 1);
    HoursOfDay(f, 15, first, count);
    CHECK(first == 360 && count == 24);
    HoursOfDay(f, 16, first, count);   // no such day
    CHECK(count == 0);

    // More values than the fixed arrays hold: only the first 384 / 16 are kept.
    CHECK(ParseForecast(MakeForecast(kForecastHours + 30, 16, false), f, err) && f.hours == kForecastHours);
    // A series shorter than its time axis is malformed, not silently padded.
    CHECK(!ParseForecast(MakeForecast(48, 16, true), f, err) && !f.valid);
    // A cached reply from before wind, gusts and cloud were asked for still reads, with them unknown.
    CHECK(ParseForecast(MakeForecast(48, 16, false, false), f, err) && f.hours == 48);
    CHECK(std::isnan(f.hWind[0]) && std::isnan(f.hGust[47]) && std::isnan(f.hCloud[10]));
    HoursOfDay(f, 5, first, count);    // 48 hours cover two days only
    CHECK(count == 0);
    CHECK(!ParseForecast(R"({"error":true,"reason":"Latitude must be in range of -90 to 90°."})", f, err));
    CHECK(err.find(L"Latitude") == 0);
    CHECK(!ParseForecast("{\"latitude\":1}", f, err));

    // Which slots are still ahead: at 16:30 local (21:30 UTC) the 16:00 hour is
    // under way, at 17:00 it is over; the next morning the first day is over.
    CHECK(ParseForecast(MakeForecast(kForecastHours, 16, false), f, err));
    const int64_t t1630 = LocalSeconds({ 2026, 11, 21, 16, 30 }) + 18000;
    CHECK(FirstHourFrom(f, t1630) == 16);
    CHECK(FirstHourFrom(f, t1630 + 1800) == 17);
    CHECK(FirstDayFrom(f, t1630) == 0);
    CHECK(FirstDayFrom(f, LocalSeconds({ 2026, 11, 22, 8, 0 }) + 18000) == 1);
    CHECK(FirstHourFrom(f, t1630 + 20 * 86400) == kForecastHours);   // all over: an out-of-date forecast

    // Daily normals: exact at mid-month, halfway between at the boundary, and
    // wrapping from December into January.
    std::array<double, 12> monthly{};
    for (size_t m = 0; m < 12; ++m) { monthly[m] = static_cast<double>(m) * 10.0; }   // Jan 0 .. Dec 110
    CHECK(Near(DayNormal(monthly, 6, 15), 50.0, 0.2));                // mid-June
    CHECK(Near(DayNormal(monthly, 6, 30), 54.8, 0.2) && Near(DayNormal(monthly, 7, 1), 55.2, 0.2));
    CHECK(Near(DayNormal(monthly, 1, 1), 110.0 * 0.5 + 0.0 * 0.5, 2.0));   // 1 Jan: between Dec (110) and Jan (0)
    CHECK(Near(DayNormal(monthly, 12, 31), 55.0, 2.0));
    monthly[3] = kNaN;
    CHECK(std::isnan(DayNormal(monthly, 4, 1)) && Near(DayNormal(monthly, 2, 15), 10.0, 0.4));

    CHECK(LocalSeconds({ 1970, 1, 1, 0, 0 }) == 0);
    CHECK(LocalSeconds({ 2000, 3, 1, 0, 0 }) == 951868800);
    CHECK(Weekday(2026, 11, 21) == 6 && Weekday(2000, 1, 1) == 6 && Weekday(2024, 2, 29) == 4);
    CHECK(std::wstring(WeatherText(73)) == L"Snow" && std::wstring(WeatherText(42)).empty());
    CHECK(std::wstring(WeatherGlyph(0, true)) == L"☀" && std::wstring(WeatherGlyph(0, false)) == L"☾");
    CHECK(std::wstring(WeatherGlyph(85, true)) == L"❄" && std::wstring(WeatherGlyph(63, true)) == L"☂");
    CHECK(ForecastUrl(45.41117, -75.69812).find(L"latitude=45.4112&longitude=-75.6981") != std::wstring::npos);
    CHECK(ForecastUrl(0, 0).find(L"forecast_days=16") != std::wstring::npos &&
          ForecastUrl(0, 0).find(L"forecast_hours") == std::wstring::npos &&
          ForecastUrl(0, 0).find(L"wind_speed_10m,wind_gusts_10m,cloud_cover&daily") != std::wstring::npos);

    // Forecast units.
    CHECK(FmtWind(16.09344, Units::Imperial) == L"10 mph" && FmtWind(20.6, Units::Metric) == L"21 km/h");
    CHECK(FmtRain(25.4, Units::Imperial) == L"1.00 in" && FmtRain(2.44, Units::Metric) == L"2.4 mm");
    CHECK(FmtSnowAmount(0.44, Units::Metric) == L"0.4 cm");
    CHECK(std::wstring(CompassPoint(225)) == L"SW" && std::wstring(CompassPoint(359)) == L"N" &&
          std::wstring(CompassPoint(-90)) == L"W" && std::wstring(CompassPoint(kNaN)).empty());
}

// A latest-release reply laid out as GitHub's API sends it: top-level fields,
// then assets, each with a nested uploader object.
std::string MakeRelease(const char* tag, bool draft, bool withZip, bool withSetup, const char* host) {
    const std::string base = std::string(host) + "/Flinterpop/WeatherTool/releases/download/" + tag + "/";
    const std::string v = std::string(tag).substr(1);
    std::string assets;
    auto asset = [&](const std::string& name) {
        if (!assets.empty()) { assets += ","; }
        assets += "{\"url\":\"https://api.github.com/x\",\"id\":1,\"name\":\"" + name +
                  "\",\"label\":null,\"uploader\":{\"login\":\"someone\",\"id\":2,\"type\":\"User\"},"
                  "\"content_type\":\"application/octet-stream\",\"size\":2400000,"
                  "\"browser_download_url\":\"" + base + name + "\"}";
    };
    asset("WeatherTool-" + v + "-sha256.md");
    if (withZip) { asset("WeatherTool-" + v + "-portable.zip"); }
    if (withSetup) { asset("WeatherTool-" + v + "-setup.exe"); }
    return std::string("{\"url\":\"https://api.github.com/repos/Flinterpop/WeatherTool/releases/1\",\"tag_name\":\"") + tag +
           "\",\"name\":\"WeatherTool " + v + "\",\"draft\":" + (draft ? "true" : "false") +
           ",\"prerelease\":false,\"author\":{\"login\":\"someone\"},\"assets\":[" + assets +
           "],\"body\":\"A **Now** tab.\\r\\n\\r\\n| File | \\\"tag_name\\\": no |\\nMontr\\u00e9al\"}";
}

void TestUpdate() {
    using namespace wt::update;
    Version v;
    CHECK(ParseVersion(L"v0.4.0", v) && v.major == 0 && v.minor == 4 && v.patch == 0);
    CHECK(ParseVersion(L"12.3.45", v) && VersionText(v) == L"12.3.45");
    CHECK(!ParseVersion(L"v0.4", v) && !ParseVersion(L"v0.4.0-rc1", v) && !ParseVersion(L"", v) && !ParseVersion(L"v.1.2", v));
    CHECK(Newer({ 0, 5, 0 }, { 0, 4, 9 }) && Newer({ 1, 0, 0 }, { 0, 9, 9 }) && Newer({ 0, 4, 1 }, { 0, 4, 0 }));
    CHECK(!Newer({ 0, 4, 0 }, { 0, 4, 0 }) && !Newer({ 0, 3, 9 }, { 0, 4, 0 }));

    const std::wstring gh = kDownloadBase;
    Release r;
    std::wstring err;
    CHECK(ParseLatest(MakeRelease("v0.5.0", false, true, true, "https://github.com"), gh, r, err));
    CHECK(r.tag == L"v0.5.0" && r.version.minor == 5);
    CHECK(r.zipUrl == gh + L"v0.5.0/WeatherTool-0.5.0-portable.zip");
    CHECK(r.setupUrl == gh + L"v0.5.0/WeatherTool-0.5.0-setup.exe");
    CHECK(r.notes.find(L"A **Now** tab.") == 0 && r.notes.find(L"Montréal") != std::wstring::npos);
    CHECK(AssetFor(r, true, err) == r.setupUrl && AssetFor(r, false, err) == r.zipUrl);

    // Only the setup: an installed copy can update, a portable one is told why not.
    CHECK(ParseLatest(MakeRelease("v0.5.0", false, false, true, "https://github.com"), gh, r, err));
    CHECK(AssetFor(r, false, err).empty() && err.find(L"WeatherTool-0.5.0-portable.zip") != std::wstring::npos);
    // Links that do not lead to this repository's releases are ignored, so nothing is left.
    CHECK(!ParseLatest(MakeRelease("v0.5.0", false, true, true, "https://example.com"), gh, r, err));
    CHECK(err.find(L"has neither") != std::wstring::npos);
    // A test server's links pass when it is the base.
    CHECK(ParseLatest(MakeRelease("v0.5.0", false, true, false, "http://127.0.0.1:18765"), L"http://127.0.0.1:18765/", r, err));
    CHECK(!ParseLatest(MakeRelease("v0.5.0", true, true, true, "https://github.com"), gh, r, err));   // a draft
    CHECK(!ParseLatest(MakeRelease("latest", false, true, true, "https://github.com"), gh, r, err));
    CHECK(!ParseLatest(R"({"message":"API rate limit exceeded for 1.2.3.4.","documentation_url":"x"})", gh, r, err));
    CHECK(err.find(L"API rate limit exceeded") != std::wstring::npos);

    CHECK(ShortNotes(L"short", 100) == L"short");
    const std::wstring longNotes = std::wstring(60, L'a') + L"\n" + std::wstring(60, L'b');
    CHECK(ShortNotes(longNotes, 100) == std::wstring(60, L'a') + L"...");   // cut at the line
    CHECK(ShortNotes(std::wstring(9, L'x') + L"\U0001F600" + L"yyyy", 10) == std::wstring(9, L'x') + L"...");   // not half a pair
    CHECK(SetupArgs().find(L"/ALLUSERS") != std::wstring::npos && SetupArgs().find(L"/WEATHERTOOLRESTART=1") != std::wstring::npos);
}

void TestMap() {
    using namespace wt::slippy;
    CHECK(Near(WorldSize(0), 256.0) && Near(WorldSize(3), 2048.0));
    CHECK(Near(LonToX(0.0, 0), 128.0) && Near(LatToY(0.0, 0), 128.0) && Near(LonToX(-180.0, 2), 0.0));
    CHECK(Near(XToLon(LonToX(-75.6972, 10), 10), -75.6972, 1e-9) && Near(YToLat(LatToY(45.4215, 10), 10), 45.4215, 1e-9));
    CHECK(Near(LatToY(89.9, 4), 0.0, 1e-6) && Near(LatToY(-89.9, 4), WorldSize(4), 1e-6));   // clamped to the square world
    // Ottawa's tile at zoom 10, worked by hand from the wiki.openstreetmap.org formula:
    // x = (104.3028 / 360) * 1024 = 296.7; y = (1 - ln(tan + sec)(45.4215°) / pi) / 2 * 1024 = 366.4.
    CHECK(static_cast<int>(LonToX(-75.6972, 10) / kTile) == 296 && static_cast<int>(LatToY(45.4215, 10) / kTile) == 366);
    CHECK(WrapTile(-1, 3) == 7 && WrapTile(8, 3) == 0 && WrapTile(5, 3) == 5);
    const Parent p = ParentOf(5, 13, 22, 3);
    CHECK(p.x == 3 && p.y == 5 && Near(p.du, 0.25) && Near(p.u0, 0.25) && Near(p.v0, 0.5));
    const Parent same = ParentOf(4, 9, 6, 4);
    CHECK(same.x == 9 && same.y == 6 && Near(same.du, 1.0) && Near(same.u0, 0.0));
    CHECK(OsmTileUrl(10, 296, 372) == L"https://tile.openstreetmap.org/10/296/372.png");

    // RainViewer's frame list, as the public API sends it.
    const std::string j = R"({"version":"2.0","generated":1609402525,"host":"https://tilecache.rainviewer.com",)"
                          R"("radar":{"past":[{"time":1609401600,"path":"/v2/radar/1609401600"},)"
                          R"({"time":1609402200,"path":"/v2/radar/1609402200"}],"nowcast":[]},"satellite":{"infrared":[]}})";
    radar::Frames f;
    std::wstring err;
    CHECK(radar::ParseFrames(j, f, err) && f.count == 2 && f.host == L"https://tilecache.rainviewer.com");
    CHECK(f.frame[1].time == 1609402200 && f.frame[0].path == L"/v2/radar/1609401600");
    CHECK(radar::TileUrl(f, 1, 6, 18, 23) == L"https://tilecache.rainviewer.com/v2/radar/1609402200/512/6/18/23/2/1_1.png");
    CHECK(!radar::ParseFrames(R"({"host":"http://plain.example","radar":{"past":[{"time":1,"path":"/a"}]}})", f, err));
    CHECK(!radar::ParseFrames(R"({"host":"https://x","radar":{"past":[]}})", f, err) && err.find(L"no radar frames") != std::wstring::npos);
    CHECK(!radar::ParseFrames(R"({"host":"https://x","radar":{"past":[{"time":1,"path":"no-slash"}]}})", f, err));
    CHECK(f.source == radar::Source::World);

    // GeoMet: times, every 12 minutes back from the newest, oldest first.
    CHECK(radar::ParseIsoUtc("2026-11-22T01:54:00Z") == 1795312440);
    CHECK(radar::IsoUtc(1795312440) == L"2026-11-22T01:54:00Z");
    CHECK(radar::ParseIsoUtc("2026-11-22 01:54:00") == -1 && radar::ParseIsoUtc("2026-13-22T01:54:00Z") == -1);
    radar::Frames m;
    CHECK(radar::ParseMscTimes(R"(<Layer><Dimension name="time" units="ISO8601" default="2026-11-22T01:54:00Z" )"
                               R"(nearestValue="0">2026-11-21T22:54:00Z/2026-11-22T01:54:00Z/PT6M</Dimension></Layer>)", m, err));
    CHECK(m.source == radar::Source::NorthAmerica && m.count == 16);
    CHECK(m.frame[0].time == 1795301640 && m.frame[15].time == 1795312440 && m.frame[15].time - m.frame[14].time == 720);
    CHECK(radar::ParseMscTimes(R"(<Dimension name="time">2026-11-22T01:00:00Z,2026-11-22T01:06:00Z,2026-11-22T01:12:00Z</Dimension>)", m, err));
    CHECK(m.count == 2 && m.frame[1].time - m.frame[0].time == 720);
    CHECK(!radar::ParseMscTimes("<ServiceException>Layer not available</ServiceException>", m, err) && !err.empty());
    CHECK(!radar::ParseMscTimes(R"(<Dimension name="time">2026-11-22T01:00:00Z/2026-11-21T01:00:00Z/PT6M</Dimension>)", m, err));
    const std::wstring u = radar::MscTileUrl(radar::MscLayer::Radar, 1795312440, 0, 0, 0);
    CHECK(u.rfind(L"https://geo.weather.gc.ca/geomet?", 0) == 0);
    CHECK(u.find(L"&BBOX=-20037508.34,-20037508.34,20037508.34,20037508.34") != std::wstring::npos);
    CHECK(u.find(L"&LAYERS=Radar_1km_SfcPrecipType&STYLES=SfcPrecipType_Dis&TIME=2026-11-22T01:54:00Z") != std::wstring::npos);
    CHECK(radar::MscTileUrl(radar::MscLayer::Alerts, 0, 6, 18, 23).find(L"&LAYERS=Current-Alerts") != std::wstring::npos);
    CHECK(radar::MscCovers(radar::MscLayer::Radar, 6, 18, 23) && radar::MscCovers(radar::MscLayer::Alerts, 6, 18, 23));  // Ottawa
    CHECK(radar::MscCovers(radar::MscLayer::Radar, 6, 17, 27) && !radar::MscCovers(radar::MscLayer::Alerts, 6, 17, 27)); // Miami
    CHECK(!radar::MscCovers(radar::MscLayer::Radar, 6, 32, 21));                                                         // Europe
    CHECK(radar::MaxZoom(radar::Source::World) == 7 && radar::MaxZoom(radar::Source::NorthAmerica) == 8);

    // Satellite: the newest picture, which satellite sees a tile, which picture goes with a radar frame.
    CHECK(radar::MscLatest(R"(<Dimension name="time" units="ISO8601">2026-11-19T20:20:00Z/2026-11-22T02:20:00Z/PT10M</Dimension>)") ==
          radar::ParseIsoUtc("2026-11-22T02:20:00Z"));
    CHECK(radar::MscLatest("<nothing/>") == -1);
    radar::MscLayer which{};
    CHECK(radar::SatFor(6, 18, 23, which) && which == radar::MscLayer::SatEast);   // Ottawa
    CHECK(radar::SatFor(6, 10, 22, which) && which == radar::MscLayer::SatWest);   // Vancouver
    CHECK(radar::SatFor(6, 3, 27, which) && which == radar::MscLayer::SatWest);    // Hawaii
    CHECK(!radar::SatFor(6, 40, 21, which));                                       // central Asia: neither
    CHECK(!radar::SatFor(6, 18, 2, which));                                        // the far north: beyond both views
    const int64_t latest = radar::ParseIsoUtc("2026-11-22T02:20:00Z");
    CHECK(radar::SatTimeFor(radar::ParseIsoUtc("2026-11-22T01:54:00Z"), latest) == radar::ParseIsoUtc("2026-11-22T01:50:00Z"));
    CHECK(radar::SatTimeFor(radar::ParseIsoUtc("2026-11-22T02:30:00Z"), latest) == latest);   // never newer than there is
    CHECK(radar::MscTileUrl(radar::MscLayer::SatWest, latest, 6, 10, 22)
              .find(L"&LAYERS=GOES-West_1km_DayVis-NightIR&STYLES=&TIME=2026-11-22T02:20:00Z") != std::wstring::npos);
}

// The alerts at a clicked point: GeoMet's GetFeatureInfo reply, in the shape
// it had on 23 Nov 2026 (properties trimmed, geometry shortened).
void TestAlertInfo() {
    CHECK(radar::ParseIsoUtc("2026-11-22T01:54:00.422Z") == 1795312440);   // GeoMet's alert times carry milliseconds
    CHECK(radar::ParseIsoUtc("2026-11-22T01:54:00.Z") == -1 && radar::ParseIsoUtc("2026-11-22T01:54:00.4x2Z") == -1);
    CHECK(radar::AlertsCoverPoint(45.42, -75.70) && !radar::AlertsCoverPoint(25.76, -80.19));   // Ottawa, Miami
    const std::wstring u = radar::AlertInfoUrl(49.61, -81.69);
    CHECK(u.rfind(L"https://geo.weather.gc.ca/geomet?", 0) == 0 && u.find(L"&REQUEST=GetFeatureInfo") != std::wstring::npos);
    CHECK(u.find(L"&BBOX=49.60000,-81.70000,49.62000,-81.68000&WIDTH=21&HEIGHT=21&I=10&J=10") != std::wstring::npos);
    CHECK(u.find(L"&QUERY_LAYERS=Current-Alerts") != std::wstring::npos && u.find(L"&INFO_FORMAT=application/json") != std::wstring::npos);

    const char* reply = R"({
"type": "FeatureCollection", "name": "Current-Alerts",
"features": [
{ "type": "Feature", "properties": { "id": "1", "alert_type": "statement", "alert_name_en": "special weather statement",
  "alert_name_fr": "bulletin météorologique spécial", "publication_datetime": "2026-11-23T02:00:30.324Z",
  "event_end_datetime": "2026-11-23T23:00:00.000Z", "alert_text_en": "Heavy rainfall is expected.\n\nLocations: Nova Scotia.",
  "risk_colour_en": "", "feature_name_en": "Cumberland County - Minas Shore", "status_en": "continued" },
  "geometry": { "type": "Polygon", "coordinates": [ [ [ -64.1, 45.3 ], [ -64.0, 45.4 ], [ -64.1, 45.3 ] ] ] } },
{ "type": "Feature", "properties": { "id": "2", "alert_name_en": "snowfall warning", "publication_datetime": "2026-11-22T21:00:42.665Z",
  "event_end_datetime": "2026-11-23T10:00:00.000Z", "alert_text_en": "Heavy snowfall continues.",
  "risk_colour_en": "yellow", "feature_name_en": "Fraserdale - Pledger Lake", "status_en": "continued" },
  "geometry": { "type": "Polygon", "coordinates": [ [ [ -81.7, 49.6 ], [ -81.6, 49.7 ], [ -81.7, 49.6 ] ] ] } },
{ "type": "Feature", "properties": { "id": "3", "alert_name_en": "blizzard warning", "risk_colour_en": "red",
  "feature_name_en": "Fraserdale - Pledger Lake", "alert_text_en": "Whiteout." }, "geometry": null },
{ "type": "Feature", "properties": { "id": "4", "alert_name_en": "snowfall warning", "publication_datetime": "2026-11-22T21:00:42.665Z",
  "risk_colour_en": "yellow", "feature_name_en": "Fraserdale - Pledger Lake", "alert_text_en": "Heavy snowfall continues." } },
{ "type": "Feature", "properties": { "id": "5", "alert_name_en": "" } }
] })";
    radar::AlertInfo info;
    std::wstring err;
    CHECK(radar::ParseAlertInfo(reply, info, err) && err.empty());
    CHECK(info.count == 3);   // the repeated snowfall warning and the nameless one are dropped
    CHECK(info.alert[0].name == L"blizzard warning" && info.alert[0].colour == L"red" && info.alert[0].issued == -1);
    CHECK(info.alert[1].name == L"snowfall warning" && info.alert[1].area == L"Fraserdale - Pledger Lake");
    CHECK(info.alert[1].issued == radar::ParseIsoUtc("2026-11-22T21:00:42Z") && info.alert[1].ends == radar::ParseIsoUtc("2026-11-23T10:00:00Z"));
    CHECK(info.alert[2].name == L"special weather statement" && info.alert[2].colour.empty());   // statements last
    CHECK(info.alert[2].text == L"Heavy rainfall is expected.\n\nLocations: Nova Scotia." && info.alert[2].status == L"continued");

    CHECK(radar::ParseAlertInfo(R"({"type": "FeatureCollection", "name": "Current-Alerts", "features": [ ] })", info, err));
    CHECK(info.count == 0);   // nothing in effect there is an answer, not an error
    CHECK(!radar::ParseAlertInfo("<ServiceExceptionReport>msWMSFeatureInfo()</ServiceExceptionReport>", info, err) && !err.empty());
    CHECK(!radar::ParseAlertInfo(R"({"type": "FeatureCollection", "features": [ { "properties": { "alert_name_en": "x" )", info, err));

    // More alerts than the panel holds: kMaxAlerts of them, a late severe one displacing a mild one.
    std::string many = R"({"type": "FeatureCollection", "features": [)";
    for (int i = 0; i < 12; ++i) {
        many += std::string(i == 0 ? "" : ",") + R"({"properties": {"alert_name_en": "wind warning", "feature_name_en": "area )" +
                std::to_string(i) + R"(", "risk_colour_en": ")" + (i == 9 ? "orange" : "yellow") + R"("}})";
    }
    many += "]}";
    CHECK(radar::ParseAlertInfo(many, info, err) && info.count == radar::kMaxAlerts);
    CHECK(info.alert[0].colour == L"orange" && info.alert[0].area == L"area 9");   // the tenth, with the list already full
    CHECK(info.alert[1].area == L"area 0" && info.alert[radar::kMaxAlerts - 1].area == L"area 6");
}

void TestConfig() {
    Location loc;
    CHECK(ParseLocation(L"Ottawa, Ontario, CA|45.41117|-75.69812", loc));
    CHECK(loc.name == L"Ottawa, Ontario, CA" && Near(loc.lat, 45.41117));
    CHECK(!ParseLocation(L"Nowhere|95|0", loc));
    CHECK(!ParseLocation(L"|1|2", loc));
    CHECK(!ParseLocation(L"x|abc|2", loc));

    // Round trip through a scratch folder (never the operator's ini).
    wchar_t tmp[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH, tmp) > 0);
    const std::wstring dir = std::wstring(tmp) + L"weathertool_test_" + std::to_wstring(GetCurrentProcessId());
    CHECK(CreateDirectoryW(dir.c_str(), nullptr) != 0);
    CHECK(CreateDirectoryW((dir + L"\\cache").c_str(), nullptr) != 0);
    Config cfg;
    std::wstring err;
    CHECK(LoadConfig(dir, cfg, err) && cfg.locCount == 0 && cfg.units == Units::Metric);
    cfg.locs[0] = { L"Montréal, Quebec, CA", 45.50884, -73.58781 };
    cfg.locs[1] = { L"Ottawa, Ontario, CA", 45.41117, -75.69812 };
    cfg.locCount = 2;
    cfg.units = Units::Imperial;
    cfg.theme = ThemeMode::Dark;
    cfg.state.view = 2;
    cfg.state.tab = 1;
    cfg.state.selected = 1;
    CHECK(SaveLocations(cfg, err) && SaveSettings(cfg, err) && SaveState(cfg, err));
    Config back;
    CHECK(LoadConfig(dir, back, err));
    CHECK(back.locCount == 2 && back.locs[0].name == L"Montréal, Quebec, CA");
    CHECK(back.units == Units::Imperial && back.theme == ThemeMode::Dark);
    CHECK(back.state.view == 2 && back.state.selected == 1 && back.state.tab == 1);
    CHECK(!back.state.hasMap && back.state.mapZoom == 6 && back.state.radar);   // map defaults
    CHECK(back.state.radarSrc == 0 && !back.state.alerts && !back.state.satellite);
    back.state.radarSrc = 1;
    back.state.alerts = true;
    back.state.satellite = true;
    back.state.hasMap = true;
    back.state.mapLat = 45.5;
    back.state.mapLon = -73.6;
    back.state.mapZoom = 9;
    back.state.radar = false;
    back.state.tab = 2;
    CHECK(SaveState(back, err));
    Config mapBack;
    CHECK(LoadConfig(dir, mapBack, err) && mapBack.state.hasMap && Near(mapBack.state.mapLat, 45.5) &&
          Near(mapBack.state.mapLon, -73.6) && mapBack.state.mapZoom == 9 && !mapBack.state.radar && mapBack.state.tab == 2);
    CHECK(mapBack.state.radarSrc == 1 && mapBack.state.alerts && mapBack.state.satellite);
    CHECK(!back.serverEnabled && back.serverPort == 8765);   // defaults
    back.serverEnabled = true;
    back.serverPort = 9001;
    CHECK(SaveServer(back, err));
    Config srv;
    CHECK(LoadConfig(dir, srv, err) && srv.serverEnabled && srv.serverPort == 9001);
    CHECK(CacheFileFor(back, back.locs[1]) == dir + L"\\cache\\45.4112_-75.6981.csv");
    CHECK(ForecastFileFor(back, back.locs[1]) == dir + L"\\cache\\45.4112_-75.6981.forecast.json");

    std::string bytes;
    CHECK(WriteFileBytes(dir + L"\\x.bin", "abc", err) && ReadFileBytes(dir + L"\\x.bin", bytes, 16, err) && bytes == "abc");
    CHECK(!ReadFileBytes(dir + L"\\x.bin", bytes, 2, err));   // over the size cap
    DeleteFileW((dir + L"\\x.bin").c_str());
    DeleteFileW(cfg.path.c_str());
    RemoveDirectoryW((dir + L"\\cache").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestHttpReply() {
    const WebContent c{ "<html>hi</html>", "ICO" };
    int st = 0;
    std::string r = HttpReply("GET / HTTP/1.1\r\nHost: x\r\n\r\n", c, st);
    CHECK(st == 200 && r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(r.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
    CHECK(r.find("Content-Length: 15\r\n") != std::string::npos && r.size() > 15 && r.substr(r.size() - 15) == "<html>hi</html>");
    r = HttpReply("GET /index.html?x=1 HTTP/1.1\r\n\r\n", c, st);
    CHECK(st == 200);
    r = HttpReply("HEAD / HTTP/1.1\r\n\r\n", c, st);   // headers only
    CHECK(st == 200 && r.find("<html>") == std::string::npos && r.find("Content-Length: 15") != std::string::npos);
    r = HttpReply("GET /favicon.ico HTTP/1.1\r\n\r\n", c, st);
    CHECK(st == 200 && r.find("image/x-icon") != std::string::npos);
    CHECK((HttpReply("GET /favicon.ico HTTP/1.1\r\n\r\n", WebContent{ "x", "" }, st), st == 404));
    // Nothing else is reachable: no files, no traversal.
    CHECK((HttpReply("GET /../WeatherTool.ini HTTP/1.1\r\n\r\n", c, st), st == 404));
    CHECK((HttpReply("GET /cache/ HTTP/1.1\r\n\r\n", c, st), st == 404));
    CHECK((HttpReply("POST / HTTP/1.1\r\n\r\n", c, st), st == 405));
    CHECK((HttpReply("garbage", c, st), st == 400));
    CHECK((HttpReply("GET /\r\n\r\n", c, st), st == 400));
}

// A real server on a free loopback port: request, response, refusal of a
// second listener on the same port, and a clean stop.
std::string Fetch(uint16_t port, const char* request) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    InetPtonW(AF_INET, L"127.0.0.1", &a.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0) {
        send(s, request, static_cast<int>(std::strlen(request)), 0);
        char buf[4096];
        for (int i = 0; i < 100; ++i) {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) { break; }
            out.append(buf, static_cast<size_t>(n));
        }
    }
    closesocket(s);
    return out;
}

void TestWebServer() {
    WSADATA wd{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    const WebContent c{ "<html>served</html>", "" };
    WebServer srv;
    std::wstring err;
    CHECK(srv.Start(0, c, err) && srv.Running() && srv.Port() != 0);
    const std::string page = Fetch(srv.Port(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(page.rfind("HTTP/1.1 200 OK", 0) == 0 && page.find("<html>served</html>") != std::string::npos);
    CHECK(Fetch(srv.Port(), "GET /nope HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
    CHECK(srv.Requests() == 2);
    WebServer second;   // the same port again must be refused, with a readable reason
    CHECK(!second.Start(srv.Port(), c, err) && err.find(L"in use") != std::wstring::npos);
    const uint16_t port = srv.Port();
    srv.Stop();
    CHECK(!srv.Running() && Fetch(port, "GET / HTTP/1.1\r\n\r\n").empty());
    WSACleanup();
}

// Optional, network-dependent: one real archive download through the app's
// own HTTP client and parser. Not part of ctest (`--live` to run it).
int LiveCheck(const char* startDate) {
    HttpClient http;
    auto buf = std::make_unique<char[]>(kHttpBufSize);
    HttpResult hr;
    std::wstring err;
    const std::string start = startDate != nullptr ? startDate : "1940-01-01";
    const std::wstring url =
        L"https://archive-api.open-meteo.com/v1/archive?latitude=45.4112&longitude=-75.6981&start_date=" +
        std::wstring(start.begin(), start.end()) +
        L"&end_date=2026-11-01&daily=temperature_2m_mean,temperature_2m_max,temperature_2m_min,snowfall_sum&timezone=auto";
    if (!http.Get(url, buf.get(), kHttpBufSize, hr, err)) {
        std::printf("live: transport failed after %u ms: %ls\n", hr.elapsedMs, err.c_str());
        return 1;
    }
    std::printf("live: HTTP %u, %zu bytes in %u ms\n", hr.status, hr.length, hr.elapsedMs);
    Climate c;
    if (!ParseArchive(std::string_view(buf.get(), hr.length), c, err)) {
        std::printf("live: parse failed: %ls\n", err.c_str());
        return 1;
    }
    const YearSeries a = AnnualTemp(c, Var::Mean);
    const int last = LastValidYear(a);
    const double lastMean = last >= c.firstYear ? a.v[static_cast<size_t>(last - c.firstYear)] : kNaN;
    const int lcm = LastCompleteMonth(c);
    std::printf("live: %d-%d, last complete year %d mean %.2f C, last complete month %d-%02d\n", c.firstYear,
                c.firstYear + static_cast<int>(c.yearCount) - 1, last, lastMean, lcm / 12, lcm % 12 + 1);
    return 0;
}

// Optional: one real forecast request (`--live-forecast`).
int LiveForecast() {
    HttpClient http;
    auto buf = std::make_unique<char[]>(kHttpBufSize);
    HttpResult hr;
    std::wstring err;
    if (!http.Get(ForecastUrl(45.41117, -75.69812), buf.get(), kHttpBufSize, hr, err)) {
        std::printf("live: transport failed after %u ms: %ls\n", hr.elapsedMs, err.c_str());
        return 1;
    }
    std::printf("live: HTTP %u, %zu bytes in %u ms\n", hr.status, hr.length, hr.elapsedMs);
    Forecast f;
    if (!ParseForecast(std::string_view(buf.get(), hr.length), f, err)) {
        std::printf("live: parse failed: %ls\n", err.c_str());
        return 1;
    }
    std::printf("live: now %04d-%02d-%02d %02d:%02d %ls, %.1f C (feels %.1f), %ls, wind %.0f km/h; %zu hours, %zu days, "
                "today %.1f / %.1f C\n", f.nowTime.year, f.nowTime.month, f.nowTime.day, f.nowTime.hour, f.nowTime.minute,
                f.tzAbbrev.c_str(), f.temp, f.feels, WeatherText(f.code), f.wind, f.hours, f.days, f.dMax[0], f.dMin[0]);
    return f.hours == kForecastHours && f.days == kForecastDays ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--live") { return LiveCheck(argc > 2 ? argv[2] : nullptr); }
    if (argc > 1 && std::string(argv[1]) == "--live-forecast") { return LiveForecast(); }
    TestJsonScan();
    TestForecast();
    TestUpdate();
    TestMap();
    TestAlertInfo();
    TestParseArchive();
    TestMergeAndCache();
    TestSeries();
    TestGeocode();
    TestUnits();
    TestConfig();
    TestHttpReply();
    TestWebServer();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
