// Shapefiles drawn on the Map tab over the radar and satellite: a reader for
// the main file (.shp) of an ESRI shapefile, written from ESRI's "Shapefile
// Technical Description" (July 1998), and the check of its .prj. Only the
// geometry is read (no attributes from the .dbf), and only shapefiles in
// longitude and latitude: NAD83 and WGS 84 differ by a metre or two, far
// under a pixel, so any geographic datum is drawn as it is; a projected one
// is refused. Points are kept as float (about a metre at 180°). UI-free and
// unit-tested.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wt::shapes {

constexpr size_t kMaxLayers    = 16;
constexpr size_t kMaxFileBytes = 256u * 1024u * 1024u;   // one .shp
constexpr size_t kMaxPoints    = 24u * 1024u * 1024u;    // one layer: 192 MB as float pairs
constexpr size_t kMaxRecords   = 4u * 1024u * 1024u;

enum class Kind { Point, Line, Polygon };

// Longitude and latitude as read; Web Mercator world fractions after
// ToMercator. Either way x0 <= x1 and y0 <= y1.
struct Box {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// One shape. A line or polygon has `partCount` parts (polygon rings: the
// outer ones and the holes, drawn together so the holes stay holes); a point
// or multipoint has one part holding its points.
struct Record {
    uint32_t firstPart = 0;
    uint32_t partCount = 0;
    Box      box;
};

struct Part {
    uint32_t first = 0;   // index of its first point
    uint32_t count = 0;
};

struct Layer {
    Kind kind = Kind::Line;
    std::vector<float>  xy;        // lon, lat, lon, lat, ...
    std::vector<Part>   parts;
    std::vector<Record> records;
    Box    box;                    // of everything
    size_t Points() const { return xy.size() / 2; }
};

// The .shp's bytes. Null shapes are skipped; Z and M values are ignored.
// False, with why, when it is not a shapefile, is a MultiPatch, is cut short
// or exceeds the limits above.
bool Parse(std::string_view shp, Layer& out, std::wstring& error);

// The .prj's text: true for a geographic one (GEOGCS), false, with why, for a
// projected one (PROJCS) or one that is neither.
bool PrjIsGeographic(std::string_view prj, std::wstring& error);

// Reads path (a .shp) and its .prj beside it. Without a .prj the shapefile
// is taken to be in degrees if its extent fits (-180..180, -90..90).
bool Load(const std::wstring& path, Layer& out, std::wstring& error);

// The points and boxes from degrees to Web Mercator world fractions (x 0..1
// east from 180°W, y 0..1 south from 85.05°N, latitudes beyond clamped), so
// a paint only scales them: map pixels at zoom z are 256 * 2^z times these.
void ToMercator(Layer& layer);

// "15M_shoreline_1" from "C:\\maps\\15M_shoreline_1.shp".
std::wstring NameOf(const std::wstring& path);

} // namespace wt::shapes
