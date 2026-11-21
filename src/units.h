// Display units and number formatting. Data is stored in deg C and cm;
// conversion happens only on the way to the screen.
#pragma once

#include <string>

namespace wt {

enum class Units { Metric, Imperial };

double ToTemp(double c, Units u);    // absolute temperature
double ToDelta(double dc, Units u);  // temperature difference
double ToSnow(double cm, Units u);

const wchar_t* TempUnit(Units u);    // L"°C" / L"°F"
const wchar_t* SnowUnit(Units u);    // L"cm" / L"in"
int            SnowDecimals(Units u);

// All take stored (metric) values; "-" for NaN.
std::wstring FmtTemp(double c, Units u);              // "7.3 °C"
std::wstring FmtDelta(double dc, Units u, int decimals = 2);   // "+1.20 °C"
std::wstring FmtSnow(double cm, Units u);              // "154 cm"
std::wstring FmtSnowDelta(double cm, Units u);         // "-1.1 cm"

// Forecast quantities: wind in km/h (mph), rain in mm (in), snow on the
// ground in cm (in), each formatted the same way.
double         ToWind(double kmh, Units u);
double         ToRain(double mm, Units u);
const wchar_t* WindUnit(Units u);    // L"km/h" / L"mph"
const wchar_t* RainUnit(Units u);    // L"mm" / L"in"
int            RainDecimals(Units u);
std::wstring   FmtWind(double kmh, Units u);   // "15 km/h"
std::wstring   FmtRain(double mm, Units u);    // "2.4 mm"
std::wstring   FmtSnowAmount(double cm, Units u);   // one more decimal than FmtSnow: "0.4 cm"
const wchar_t* CompassPoint(double degrees);  // 0 -> L"N", 225 -> L"SW"; L"" for NaN

// Generic: value already in display units.
std::wstring FmtNumber(double v, int decimals, const wchar_t* unit, bool sign = false);

std::wstring SeasonLabel(int startYear);              // 1998 -> "1998–99"

} // namespace wt
