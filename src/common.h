// Shared types and compile-time limits for WeatherTool.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace wt {

// Fixed capacities (Power of 10: no unbounded growth).
constexpr int    kFirstYear    = 1940;           // ERA5 starts here
constexpr size_t kMaxYears     = 120;            // 1940..2059
constexpr size_t kMaxLocations = 32;
constexpr size_t kMaxHits      = 10;             // place-search results
constexpr size_t kHttpBufSize  = 8u * 1024u * 1024u;
constexpr double kNaN          = std::numeric_limits<double>::quiet_NaN();

// The four monthly quantities. Temperatures are in deg C, snowfall in cm.
enum class Var : uint8_t { Mean = 0, High = 1, Low = 2, Snow = 3 };
constexpr size_t kVarCount = 4;

// One calendar month at one place. NaN = missing (more than one day absent).
struct MonthCell {
    std::array<double, kVarCount> v{ kNaN, kNaN, kNaN, kNaN };
};

using MonthGrid = std::array<std::array<MonthCell, 12>, kMaxYears>;

// Monthly history for one place, year index 0 = firstYear.
struct Climate {
    bool         valid     = false;
    double       lat       = 0.0;   // grid cell actually used by the provider
    double       lon       = 0.0;
    double       elevation = kNaN;  // metres
    std::wstring timezone;
    int64_t      fetched   = 0;     // Unix time of the last successful update
    int          firstYear = kFirstYear;
    size_t       yearCount = 0;
    MonthGrid    cells{};
};

// A saved place.
struct Location {
    std::wstring name;
    double       lat = 0.0;
    double       lon = 0.0;
};

// One row from the place search.
struct GeoHit {
    std::wstring name;
    std::wstring admin1;     // province / state
    std::wstring country;    // ISO code
    double       lat       = 0.0;
    double       lon       = 0.0;
    double       elevation = kNaN;
};

inline double& Cell(Climate& c, int year, int month, Var v) {
    return c.cells[static_cast<size_t>(year - c.firstYear)][static_cast<size_t>(month)].v[static_cast<size_t>(v)];
}

} // namespace wt
