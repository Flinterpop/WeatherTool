// Current weather and the forecast: Open-Meteo's forecast reply -> a fixed-
// size Forecast (conditions now, the next 48 hours, the next 16 days), and
// the WMO weather-code names. UI-free and unit-tested. The reply itself is
// what the app keeps on disk, so a cached forecast is re-read with the same
// parser.
#pragma once

#include "common.h"

#include <string>
#include <string_view>

namespace wt {

constexpr size_t kForecastHours = 48;
constexpr size_t kForecastDays  = 16;

// A local date and time at the place, as the provider gives it.
struct LocalTime {
    int year = 0, month = 0, day = 0;   // month 1..12
    int hour = 0, minute = 0;
};

struct Forecast {
    bool         valid = false;
    int64_t      fetched = 0;          // Unix time the reply arrived
    double       lat = 0.0, lon = 0.0, elevation = kNaN;
    std::wstring timezone;             // "America/Toronto"
    std::wstring tzAbbrev;             // "EST"
    int          utcOffset = 0;        // seconds

    // Now. Temperatures deg C, wind km/h, rain mm, snowfall and snow depth cm.
    LocalTime    nowTime;
    double       temp = kNaN, feels = kNaN, humidity = kNaN;
    double       precip = kNaN, snowfall = kNaN, snowDepth = kNaN;
    double       wind = kNaN, windDir = kNaN, gust = kNaN;
    int          code = -1;            // WMO weather code, -1 = unknown
    bool         isDay = true;

    // Hourly, from the current hour on.
    size_t hours = 0;
    std::array<LocalTime, kForecastHours> hTime{};
    std::array<double, kForecastHours>    hTemp{}, hFeels{}, hProb{}, hPrecip{}, hSnow{};
    std::array<int, kForecastHours>       hCode{};

    // Daily, from today on.
    size_t days = 0;
    std::array<LocalTime, kForecastDays> dTime{};
    std::array<double, kForecastDays>    dMax{}, dMin{}, dPrecip{}, dSnow{}, dProb{};
    std::array<int, kForecastDays>       dCode{};
};

// The request for one place (all quantities metric; units convert on screen).
std::wstring ForecastUrl(double lat, double lon);

// Reply -> Forecast. `err` carries the provider's "reason" on an error reply.
bool ParseForecast(std::string_view json, Forecast& out, std::wstring& err);

// "Light snow", "Overcast"...; L"" for an unknown code.
const wchar_t* WeatherText(int code);
// A one-glyph symbol for the code (sun, cloud, rain, snow, storm, fog).
const wchar_t* WeatherGlyph(int code, bool isDay);

// Day of the week for a date, 0 = Sunday.
int Weekday(int year, int month, int day);

// Seconds since 1970-01-01 00:00 of a local time read as if it were UTC.
// Add a place's utcOffset to a real Unix time to compare with it.
int64_t LocalSeconds(const LocalTime& t);

// The first hourly slot not yet over, and the first day not yet over, at
// Unix time `now` (a cached forecast can be hours old). `hours` / `days`
// when every slot is in the past.
size_t FirstHourFrom(const Forecast& f, int64_t now);
size_t FirstDayFrom(const Forecast& f, int64_t now);

} // namespace wt
