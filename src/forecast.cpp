#include "forecast.h"

#include "jsonscan.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>

namespace wt {
namespace {

// "2026-11-21T16:00" or "2026-11-21".
bool ParseLocalTime(const std::wstring& s, LocalTime& t) {
    t = LocalTime{};
    if (s.size() < 10 || s[4] != L'-' || s[7] != L'-') { return false; }
    t.year  = _wtoi(s.substr(0, 4).c_str());
    t.month = _wtoi(s.substr(5, 2).c_str());
    t.day   = _wtoi(s.substr(8, 2).c_str());
    if (s.size() >= 16 && s[10] == L'T' && s[13] == L':') {
        t.hour   = _wtoi(s.substr(11, 2).c_str());
        t.minute = _wtoi(s.substr(14, 2).c_str());
    }
    return t.year > 1900 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 &&
           t.hour >= 0 && t.hour < 24 && t.minute >= 0 && t.minute < 60;
}

// The bounds [from, to) of the object or array value of `key`.
bool Section(std::string_view j, const char* key, size_t& from, size_t& to) {
    from = json::FindKey(j, key, 0, j.size());
    to = from == json::npos ? json::npos : json::SkipContainer(j, from);
    return from != json::npos && to != json::npos;
}

double NumberIn(std::string_view j, size_t from, size_t to, const char* key) {
    size_t p = json::FindKey(j, key, from, to);
    double v = kNaN;
    if (p == json::npos || !json::ReadNumber(j, p, v)) { return kNaN; }
    return v;
}

// A flat number array, at most `max` values; `n` is how many were read.
bool NumbersIn(std::string_view j, size_t from, size_t to, const char* key, double* out, size_t max, size_t& n) {
    assert(out != nullptr && max > 0);
    n = 0;
    const size_t p = json::FindKey(j, key, from, to);
    if (p == json::npos) { return false; }
    json::ArrayReader r(j, p);
    double v = 0.0;
    while (n < max && r.NextNumber(v)) { out[n++] = v; }
    return r.ok();
}

bool TimesIn(std::string_view j, size_t from, size_t to, LocalTime* out, size_t max, size_t& n) {
    assert(out != nullptr && max > 0);
    n = 0;
    const size_t p = json::FindKey(j, "time", from, to);
    if (p == json::npos) { return false; }
    json::ArrayReader r(j, p);
    std::wstring s;
    while (n < max && r.NextString(s)) {
        if (!ParseLocalTime(s, out[n])) { return false; }
        ++n;
    }
    return r.ok() && n > 0;
}

int CodeOf(double v) { return std::isnan(v) ? -1 : static_cast<int>(std::lround(v)); }

bool ParseCurrent(std::string_view j, Forecast& f) {
    size_t a = 0, b = 0;
    if (!Section(j, "current", a, b)) { return false; }
    size_t p = json::FindKey(j, "time", a, b);
    std::wstring s;
    if (p == json::npos || !json::ReadString(j, p, s) || !ParseLocalTime(s, f.nowTime)) { return false; }
    f.temp      = NumberIn(j, a, b, "temperature_2m");
    f.feels     = NumberIn(j, a, b, "apparent_temperature");
    f.humidity  = NumberIn(j, a, b, "relative_humidity_2m");
    f.precip    = NumberIn(j, a, b, "precipitation");
    f.snowfall  = NumberIn(j, a, b, "snowfall");
    f.snowDepth = NumberIn(j, a, b, "snow_depth") * 100.0;   // m -> cm
    f.wind      = NumberIn(j, a, b, "wind_speed_10m");
    f.windDir   = NumberIn(j, a, b, "wind_direction_10m");
    f.gust      = NumberIn(j, a, b, "wind_gusts_10m");
    f.code      = CodeOf(NumberIn(j, a, b, "weather_code"));
    f.isDay     = NumberIn(j, a, b, "is_day") != 0.0;
    return !std::isnan(f.temp);
}

bool ParseHourly(std::string_view j, Forecast& f) {
    size_t a = 0, b = 0;
    if (!Section(j, "hourly", a, b) || !TimesIn(j, a, b, f.hTime.data(), kForecastHours, f.hours)) { return false; }
    auto code = std::make_unique<std::array<double, kForecastHours>>();
    size_t n[6] = {};
    const bool ok = NumbersIn(j, a, b, "temperature_2m", f.hTemp.data(), kForecastHours, n[0]) &&
                    NumbersIn(j, a, b, "apparent_temperature", f.hFeels.data(), kForecastHours, n[1]) &&
                    NumbersIn(j, a, b, "precipitation_probability", f.hProb.data(), kForecastHours, n[2]) &&
                    NumbersIn(j, a, b, "precipitation", f.hPrecip.data(), kForecastHours, n[3]) &&
                    NumbersIn(j, a, b, "snowfall", f.hSnow.data(), kForecastHours, n[4]) &&
                    NumbersIn(j, a, b, "weather_code", code->data(), kForecastHours, n[5]);
    for (size_t k = 0; k < 6; ++k) {
        if (n[k] != f.hours) { return false; }
    }
    for (size_t i = 0; i < f.hours; ++i) { f.hCode[i] = CodeOf((*code)[i]); }
    // Optional (added in 0.6.0): a series that is missing or the wrong length stays NaN.
    struct Opt { const char* key; std::array<double, kForecastHours>* out; };
    const Opt opts[3] = { { "wind_speed_10m", &f.hWind }, { "wind_gusts_10m", &f.hGust }, { "cloud_cover", &f.hCloud } };
    for (const Opt& o : opts) {
        size_t got = 0;
        if (!NumbersIn(j, a, b, o.key, o.out->data(), kForecastHours, got) || got != f.hours) { o.out->fill(kNaN); }
    }
    return ok;
}

bool ParseDaily(std::string_view j, Forecast& f) {
    size_t a = 0, b = 0;
    if (!Section(j, "daily", a, b) || !TimesIn(j, a, b, f.dTime.data(), kForecastDays, f.days)) { return false; }
    std::array<double, kForecastDays> code{};
    size_t n[6] = {};
    const bool ok = NumbersIn(j, a, b, "temperature_2m_max", f.dMax.data(), kForecastDays, n[0]) &&
                    NumbersIn(j, a, b, "temperature_2m_min", f.dMin.data(), kForecastDays, n[1]) &&
                    NumbersIn(j, a, b, "precipitation_sum", f.dPrecip.data(), kForecastDays, n[2]) &&
                    NumbersIn(j, a, b, "snowfall_sum", f.dSnow.data(), kForecastDays, n[3]) &&
                    NumbersIn(j, a, b, "precipitation_probability_max", f.dProb.data(), kForecastDays, n[4]) &&
                    NumbersIn(j, a, b, "weather_code", code.data(), kForecastDays, n[5]);
    for (size_t k = 0; k < 6; ++k) {
        if (n[k] != f.days) { return false; }
    }
    for (size_t i = 0; i < f.days; ++i) { f.dCode[i] = CodeOf(code[i]); }
    return ok;
}

} // namespace

std::wstring ForecastUrl(double lat, double lon) {
    assert(std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0);
    std::array<wchar_t, 768> url{};
    swprintf_s(url.data(), url.size(),
               L"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
               L"&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,snowfall,snow_depth,"
               L"weather_code,wind_speed_10m,wind_direction_10m,wind_gusts_10m,is_day"
               L"&hourly=temperature_2m,apparent_temperature,precipitation_probability,precipitation,snowfall,weather_code,"
               L"wind_speed_10m,wind_gusts_10m,cloud_cover"
               L"&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,snowfall_sum,"
               L"precipitation_probability_max"
               L"&timezone=auto&forecast_days=%u",
               lat, lon, static_cast<unsigned>(kForecastDays));
    return url.data();
}

bool ParseForecast(std::string_view j, Forecast& out, std::wstring& err) {
    out = Forecast{};
    bool isErr = false;
    size_t p = json::FindKey(j, "error", 0, j.size());
    if (p != json::npos && json::ReadBool(j, p, isErr) && isErr) {
        size_t r = json::FindKey(j, "reason", 0, j.size());
        std::wstring reason;
        err = (r != json::npos && json::ReadString(j, r, reason)) ? reason : L"provider returned an error";
        return false;
    }
    out.lat       = NumberIn(j, 0, j.size(), "latitude");
    out.lon       = NumberIn(j, 0, j.size(), "longitude");
    out.elevation = NumberIn(j, 0, j.size(), "elevation");
    const double off = NumberIn(j, 0, j.size(), "utc_offset_seconds");
    out.utcOffset = std::isnan(off) ? 0 : static_cast<int>(off);
    p = json::FindKey(j, "timezone", 0, j.size());
    if (p != json::npos) { (void)json::ReadString(j, p, out.timezone); }
    p = json::FindKey(j, "timezone_abbreviation", 0, j.size());
    if (p != json::npos) { (void)json::ReadString(j, p, out.tzAbbrev); }
    if (!ParseCurrent(j, out)) {
        err = L"reply has no current conditions";
        return false;
    }
    if (!ParseHourly(j, out) || !ParseDaily(j, out)) {
        err = L"reply has a missing or malformed hourly or daily series";
        return false;
    }
    out.valid = true;
    return true;
}

const wchar_t* WeatherText(int code) {
    switch (code) {
    case 0:  return L"Clear sky";
    case 1:  return L"Mainly clear";
    case 2:  return L"Partly cloudy";
    case 3:  return L"Overcast";
    case 45: return L"Fog";
    case 48: return L"Freezing fog";
    case 51: return L"Light drizzle";
    case 53: return L"Drizzle";
    case 55: return L"Heavy drizzle";
    case 56: return L"Light freezing drizzle";
    case 57: return L"Freezing drizzle";
    case 61: return L"Light rain";
    case 63: return L"Rain";
    case 65: return L"Heavy rain";
    case 66: return L"Light freezing rain";
    case 67: return L"Freezing rain";
    case 71: return L"Light snow";
    case 73: return L"Snow";
    case 75: return L"Heavy snow";
    case 77: return L"Snow grains";
    case 80: return L"Light showers";
    case 81: return L"Showers";
    case 82: return L"Heavy showers";
    case 85: return L"Light snow showers";
    case 86: return L"Snow showers";
    case 95: return L"Thunderstorm";
    case 96: return L"Thunderstorm, hail";
    case 99: return L"Thunderstorm, heavy hail";
    default: return L"";
    }
}

const wchar_t* WeatherGlyph(int code, bool isDay) {
    if (code == 0 || code == 1) { return isDay ? L"☀" : L"☾"; }   // sun, moon
    if (code == 2) { return isDay ? L"⛅" : L"☁"; }                 // sun behind cloud, cloud
    if (code == 3) { return L"☁"; }
    if (code == 45 || code == 48) { return L"≡"; }                      // fog: three bars
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) { return L"❄"; }
    if (code >= 95) { return L"⚡"; }
    if (code >= 51 && code <= 82) { return L"☂"; }                      // umbrella: drizzle, rain, showers
    return L"";
}

int64_t LocalSeconds(const LocalTime& t) {
    assert(t.month >= 1 && t.month <= 12);
    // Days from the civil date (Howard Hinnant's algorithm), no tables.
    const int64_t y = t.month <= 2 ? t.year - 1 : t.year;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (t.month + 9) % 12;
    const int64_t doy = (153 * mp + 2) / 5 + t.day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + t.hour * 3600 + t.minute * 60;
}

size_t FirstHourFrom(const Forecast& f, int64_t now) {
    assert(f.hours <= kForecastHours);
    const int64_t local = now + f.utcOffset;
    size_t i = 0;
    while (i < f.hours && LocalSeconds(f.hTime[i]) + 3600 <= local) { ++i; }
    return i;
}

size_t FirstDayFrom(const Forecast& f, int64_t now) {
    assert(f.days <= kForecastDays);
    const int64_t local = now + f.utcOffset;
    size_t i = 0;
    while (i < f.days && LocalSeconds(f.dTime[i]) + 86400 <= local) { ++i; }
    return i;
}

void HoursOfDay(const Forecast& f, size_t day, size_t& first, size_t& count) {
    assert(f.hours <= kForecastHours && f.days <= kForecastDays);
    first = 0;
    count = 0;
    if (day >= f.days) { return; }
    const LocalTime& d = f.dTime[day];
    size_t i = 0;
    while (i < f.hours && (f.hTime[i].year != d.year || f.hTime[i].month != d.month || f.hTime[i].day != d.day)) { ++i; }
    first = i;
    while (i < f.hours && f.hTime[i].year == d.year && f.hTime[i].month == d.month && f.hTime[i].day == d.day) { ++i; }
    count = i - first;
    assert(count <= 25);   // 23 to 25 across a clock change
}

int Weekday(int y, int m, int d) {
    assert(m >= 1 && m <= 12 && d >= 1 && d <= 31);
    // Sakamoto's method.
    static constexpr int kT[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    const int yy = m < 3 ? y - 1 : y;
    const int w = (yy + yy / 4 - yy / 100 + yy / 400 + kT[m - 1] + d) % 7;
    assert(w >= 0 && w < 7);
    return w;
}

} // namespace wt
