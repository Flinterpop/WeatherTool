// Web Mercator ("slippy map") arithmetic for the Map tab: the standard
// 256-pixel tile scheme OpenStreetMap and RainViewer use, written from the
// published formulas (wiki.openstreetmap.org/wiki/Slippy_map_tilenames).
// Map pixels: at zoom z the world is 256 * 2^z map pixels wide, x east from
// 180°W, y south from the top edge (about 85.05°N). UI-free and unit-tested.
#pragma once

#include <cstdint>
#include <string>

namespace wt::slippy {

constexpr int    kTile    = 256;
constexpr double kMaxLat  = 85.0511287798;   // the square world's edge
constexpr int    kMinZoom = 3;
constexpr int    kMaxZoom = 12;

double WorldSize(int z);                       // map pixels across the world at zoom z
double LonToX(double lon, int z);
double LatToY(double lat, int z);              // lat is clamped to +-kMaxLat
double XToLon(double x, int z);
double YToLat(double y, int z);
int    WrapTile(int x, int z);                 // tile column into [0, 2^z)

// The tile at zoom `zr` (<= z) that contains tile (z, x, y), and the part of
// it that (z, x, y) covers, as fractions of its width: [u0, u0 + du).
struct Parent {
    int    x = 0, y = 0;
    double u0 = 0.0, v0 = 0.0, du = 1.0;
};
Parent ParentOf(int z, int x, int y, int zr);

// The map tile and radar tile addresses (relative to their host).
std::wstring OsmTileUrl(int z, int x, int y);   // "https://tile.openstreetmap.org/z/x/y.png"

} // namespace wt::slippy
