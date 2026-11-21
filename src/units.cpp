#include "units.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cwchar>

namespace wt {

double ToTemp(double c, Units u)    { return u == Units::Metric ? c : c * 1.8 + 32.0; }
double ToDelta(double dc, Units u)  { return u == Units::Metric ? dc : dc * 1.8; }
double ToSnow(double cm, Units u)   { return u == Units::Metric ? cm : cm / 2.54; }

const wchar_t* TempUnit(Units u) { return u == Units::Metric ? L"°C" : L"°F"; }
const wchar_t* SnowUnit(Units u) { return u == Units::Metric ? L"cm" : L"in"; }
int            SnowDecimals(Units u) { return u == Units::Metric ? 0 : 1; }

std::wstring FmtNumber(double v, int decimals, const wchar_t* unit, bool sign) {
    assert(unit != nullptr);
    assert(decimals >= 0 && decimals <= 6);
    if (std::isnan(v)) { return L"-"; }
    std::array<wchar_t, 64> b{};
    const double mag = sign ? std::fabs(v) : v;
    swprintf_s(b.data(), b.size(), L"%.*f", decimals, mag);
    std::wstring s = b.data();
    // "-0" after rounding reads oddly; treat it as zero. Negatives use a
    // real minus sign, matching the signed form.
    const bool zero = std::wcstod(s.c_str(), nullptr) == 0.0;
    if (!s.empty() && s[0] == L'-') { s = zero ? s.substr(1) : L"−" + s.substr(1); }
    if (sign) { s = (zero ? L"±" : (v > 0 ? L"+" : L"−")) + s; }
    if (*unit != L'\0') { s += L' '; s += unit; }
    return s;
}

std::wstring FmtTemp(double c, Units u)  { return FmtNumber(ToTemp(c, u), 1, TempUnit(u)); }
std::wstring FmtDelta(double dc, Units u, int decimals) { return FmtNumber(ToDelta(dc, u), decimals, TempUnit(u), true); }
std::wstring FmtSnow(double cm, Units u) { return FmtNumber(ToSnow(cm, u), SnowDecimals(u), SnowUnit(u)); }
std::wstring FmtSnowDelta(double cm, Units u) { return FmtNumber(ToSnow(cm, u), SnowDecimals(u) + 1, SnowUnit(u), true); }

std::wstring SeasonLabel(int startYear) {
    std::array<wchar_t, 16> b{};
    swprintf_s(b.data(), b.size(), L"%d–%02d", startYear, (startYear + 1) % 100);
    return b.data();
}

} // namespace wt
