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

// Generic: value already in display units.
std::wstring FmtNumber(double v, int decimals, const wchar_t* unit, bool sign = false);

std::wstring SeasonLabel(int startYear);              // 1998 -> "1998–99"

} // namespace wt
