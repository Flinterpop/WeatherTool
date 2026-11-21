#include "theme.h"

#include <cassert>
#include <cmath>

namespace wt {
namespace {

using Gdiplus::Color;

Color Hex(unsigned rgb, BYTE a = 255) {
    return Color(a, static_cast<BYTE>(rgb >> 16), static_cast<BYTE>(rgb >> 8), static_cast<BYTE>(rgb));
}

COLORREF Ref(unsigned rgb) {
    return RGB(static_cast<BYTE>(rgb >> 16), static_cast<BYTE>(rgb >> 8), static_cast<BYTE>(rgb));
}

// Values from the validated reference data-viz palette (categorical slots
// 1-3 validate all-pairs for colour-vision deficiency in both modes).
Theme MakeLight() {
    Theme t;
    t.dark          = false;
    t.bg            = Hex(0xfcfcfb);
    t.text          = Hex(0x0b0b0b);
    t.textSecondary = Hex(0x52514e);
    t.textMuted     = Hex(0x7a7974);
    t.grid          = Hex(0xebeae6);
    t.guide         = Hex(0x000000, 16);
    t.series1       = Hex(0x2a78d6);
    t.series2       = Hex(0xeb6834);
    t.series2Soft   = Hex(0xf5b496);
    t.series3       = Hex(0x1baf7a);
    t.trend         = Hex(0x7a7974);
    t.warm          = Hex(0xc0392b);
    t.cool          = Hex(0x1c5cab);
    t.listBg        = Hex(0xffffff);
    t.listSel       = Hex(0xe2ecfc);
    t.listDivider   = Hex(0xeeeeec);
    t.tipBg         = Hex(0xffffff, 245);
    t.tipBorder     = Hex(0xd6d5d0);
    t.tipText       = Hex(0x0b0b0b);
    t.diverging     = { Hex(0x1c5cab), Hex(0x5598e7), Hex(0xb7d3f6), Hex(0xf0efec),
                        Hex(0xf4bcb8), Hex(0xe34948), Hex(0xa8302f) };
    t.sequential    = { Hex(0xf0efec), Hex(0xb7d3f6), Hex(0x5598e7), Hex(0x1c5cab), Hex(0x0d366b) };
    t.bgRef         = Ref(0xfcfcfb);
    t.listBgRef     = Ref(0xffffff);
    t.textRef       = Ref(0x0b0b0b);
    return t;
}

Theme MakeDark() {
    Theme t;
    t.dark          = true;
    t.bg            = Hex(0x1a1a19);
    t.text          = Hex(0xffffff);
    t.textSecondary = Hex(0xc3c2b7);
    t.textMuted     = Hex(0x8e8d85);
    t.grid          = Hex(0x2e2e2b);
    t.guide         = Hex(0xffffff, 20);
    t.series1       = Hex(0x3987e5);
    t.series2       = Hex(0xd95926);
    t.series2Soft   = Hex(0x7d3f24);
    t.series3       = Hex(0x199e70);
    t.trend         = Hex(0x8e8d85);
    t.warm          = Hex(0xf08a87);
    t.cool          = Hex(0x6da7ec);
    t.listBg        = Hex(0x222221);
    t.listSel       = Hex(0x36445c);
    t.listDivider   = Hex(0x2e2e2b);
    t.tipBg         = Hex(0x2a2a28, 245);
    t.tipBorder     = Hex(0x45453f);
    t.tipText       = Hex(0xffffff);
    t.diverging     = { Hex(0x6da7ec), Hex(0x2a78d6), Hex(0x1c4a85), Hex(0x383835),
                        Hex(0x8a2f2e), Hex(0xd44544), Hex(0xf08a87) };
    t.sequential    = { Hex(0x2a2a28), Hex(0x184f95), Hex(0x2a78d6), Hex(0x6da7ec), Hex(0xcde2fb) };
    t.bgRef         = Ref(0x1a1a19);
    t.listBgRef     = Ref(0x222221);
    t.textRef       = Ref(0xffffff);
    return t;
}

} // namespace

const Theme& ThemeFor(bool dark) {
    static const Theme light = MakeLight();
    static const Theme darkTheme = MakeDark();
    return dark ? darkTheme : light;
}

bool SystemPrefersDark() {
    // Reading Windows' own preference; the app stores nothing in the registry.
    DWORD value = 1;
    DWORD size  = sizeof(value);
    const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER,
                                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                    L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return rc == ERROR_SUCCESS && value == 0;
}

Color Ramp(const Color* stops, size_t count, double t) {
    assert(stops != nullptr && count >= 2);
    if (std::isnan(t)) { t = 0.0; }
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double p = t * static_cast<double>(count - 1);
    size_t k = static_cast<size_t>(p);
    if (k >= count - 1) { k = count - 2; }
    const double f = p - static_cast<double>(k);
    const Color& a = stops[k];
    const Color& b = stops[k + 1];
    auto mix = [f](BYTE x, BYTE y) {
        return static_cast<BYTE>(std::lround(x + (static_cast<double>(y) - x) * f));
    };
    return Color(255, mix(a.GetR(), b.GetR()), mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

} // namespace wt
