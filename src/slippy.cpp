#include "slippy.h"

#include <cassert>
#include <cmath>

namespace wt::slippy {
namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

double WorldSize(int z) {
    assert(z >= 0 && z <= 22);
    return static_cast<double>(kTile) * std::ldexp(1.0, z);
}

double LonToX(double lon, int z) {
    return (lon + 180.0) / 360.0 * WorldSize(z);
}

double LatToY(double lat, int z) {
    const double c = std::fmax(-kMaxLat, std::fmin(kMaxLat, lat)) * kPi / 180.0;
    return (1.0 - std::log(std::tan(c) + 1.0 / std::cos(c)) / kPi) / 2.0 * WorldSize(z);
}

double XToLon(double x, int z) {
    return x / WorldSize(z) * 360.0 - 180.0;
}

double YToLat(double y, int z) {
    const double n = kPi - 2.0 * kPi * y / WorldSize(z);
    return 180.0 / kPi * std::atan(std::sinh(n));
}

int WrapTile(int x, int z) {
    assert(z >= 0 && z <= 22);
    const int n = 1 << z;
    return ((x % n) + n) % n;
}

Parent ParentOf(int z, int x, int y, int zr) {
    assert(zr >= 0 && zr <= z);
    const int shift = z - zr;
    const int k = 1 << shift;
    Parent p;
    p.x = x >> shift;
    p.y = y >> shift;
    p.du = 1.0 / k;
    p.u0 = (x - p.x * k) * p.du;
    p.v0 = (y - p.y * k) * p.du;
    return p;
}

std::wstring OsmTileUrl(int z, int x, int y) {
    assert(z >= 0 && x >= 0 && y >= 0);
    return L"https://tile.openstreetmap.org/" + std::to_wstring(z) + L"/" + std::to_wstring(x) + L"/" +
           std::to_wstring(y) + L".png";
}

} // namespace wt::slippy
