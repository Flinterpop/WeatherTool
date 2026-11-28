#include "shapes.h"

#include "config.h"
#include "slippy.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace wt::shapes {

namespace {

constexpr size_t  kHeaderBytes = 100;
constexpr int32_t kFileCode    = 9994;

// The header's file code and lengths are big-endian, everything else little.
uint32_t Be32(std::string_view b, size_t at) {
    assert(at + 4 <= b.size());
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
    return (uint32_t{ p[0] } << 24) | (uint32_t{ p[1] } << 16) | (uint32_t{ p[2] } << 8) | uint32_t{ p[3] };
}

int32_t Le32(std::string_view b, size_t at) {
    assert(at + 4 <= b.size());
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);   // x64 is little-endian
    return static_cast<int32_t>(v);
}

double LeF64(std::string_view b, size_t at) {
    assert(at + 8 <= b.size());
    double v = 0.0;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}

// The shape type with Z (+10) or M (+20) folded onto the plain one.
int BaseType(int32_t t) {
    if (t == 0) { return 0; }
    if (t > 30) { return -1; }        // 31 MultiPatch, or not a shape type
    const int b = static_cast<int>(t % 10);
    return (b == 1 || b == 3 || b == 5 || b == 8) ? b : -1;
}

Kind KindOf(int base) {
    assert(base == 1 || base == 3 || base == 5 || base == 8);
    return base == 3 ? Kind::Line : base == 5 ? Kind::Polygon : Kind::Point;
}

void Grow(Box& b, float lon, float lat, bool first) {
    if (first) {
        b = { lon, lat, lon, lat };
        return;
    }
    b.x0 = std::min(b.x0, lon);
    b.x1 = std::max(b.x1, lon);
    b.y0 = std::min(b.y0, lat);
    b.y1 = std::max(b.y1, lat);
}

// Appends `n` points read at `at` as one part, growing the record's box.
bool AddPoints(std::string_view c, size_t at, size_t n, Layer& out, Record& rec, std::wstring& error) {
    assert(at + n * 16 <= c.size());
    if (out.Points() + n > kMaxPoints) {
        error = L"it has more than " + std::to_wstring(kMaxPoints) + L" points";
        return false;
    }
    const bool firstOfRecord = rec.partCount == 0;
    out.parts.push_back({ static_cast<uint32_t>(out.Points()), static_cast<uint32_t>(n) });
    ++rec.partCount;
    for (size_t k = 0; k < n; ++k) {
        const double x = LeF64(c, at + 16 * k);
        const double y = LeF64(c, at + 16 * k + 8);
        if (!std::isfinite(x) || !std::isfinite(y)) {
            error = L"it has a point that is not a number";
            return false;
        }
        const float lon = static_cast<float>(x), lat = static_cast<float>(y);
        out.xy.push_back(lon);
        out.xy.push_back(lat);
        Grow(rec.box, lon, lat, firstOfRecord && k == 0);
    }
    return true;
}

// A PolyLine or Polygon record's content after its shape type: box (32),
// part count, point count, the parts' first indexes, the points.
bool ParsePoly(std::string_view c, Layer& out, Record& rec, std::wstring& error) {
    assert(c.size() >= 4);
    if (c.size() < 44) { error = L"a record is cut short"; return false; }
    const int32_t np = Le32(c, 36), npt = Le32(c, 40);
    const size_t pts = 44 + 4 * static_cast<size_t>(std::max(np, 0));
    if (np <= 0 || npt <= 0 || np > npt || pts + 16 * static_cast<size_t>(npt) > c.size()) {
        if (np == 0 && npt == 0) { return true; }   // empty: drawn as nothing
        error = L"a record's part or point count does not fit it";
        return false;
    }
    for (int32_t k = 0; k < np; ++k) {
        const int32_t a = Le32(c, 44 + 4 * static_cast<size_t>(k));
        const int32_t e = k + 1 < np ? Le32(c, 48 + 4 * static_cast<size_t>(k)) : npt;
        if (a < 0 || e < a || e > npt) { error = L"a record's parts are out of order"; return false; }
        if (e - a == 0) { continue; }
        if (!AddPoints(c, pts + 16 * static_cast<size_t>(a), static_cast<size_t>(e - a), out, rec, error)) { return false; }
    }
    return true;
}

// One record's content (from its shape type on) into `out`.
bool ParseRecord(std::string_view c, int base, Layer& out, std::wstring& error) {
    assert(base == 1 || base == 3 || base == 5 || base == 8);
    if (c.size() < 4) { error = L"a record is cut short"; return false; }
    const int got = BaseType(Le32(c, 0));
    if (got == 0) { return true; }   // a null shape
    if (got != base) { error = L"a record's shape type differs from the file's"; return false; }
    Record rec;
    rec.firstPart = static_cast<uint32_t>(out.parts.size());
    bool ok = true;
    if (base == 1) {
        ok = c.size() >= 20;
        if (!ok) { error = L"a point record is cut short"; }
        if (ok) { ok = AddPoints(c, 4, 1, out, rec, error); }
    } else if (base == 8) {
        const int32_t n = c.size() >= 40 ? Le32(c, 36) : -1;
        ok = n >= 0 && 40 + 16 * static_cast<size_t>(n) <= c.size();
        if (!ok) { error = L"a multipoint record's point count does not fit it"; }
        if (ok && n > 0) { ok = AddPoints(c, 40, static_cast<size_t>(n), out, rec, error); }
    } else {
        ok = ParsePoly(c, out, rec, error);
    }
    if (ok && rec.partCount > 0) {
        Grow(out.box, rec.box.x0, rec.box.y0, out.records.empty());
        Grow(out.box, rec.box.x1, rec.box.y1, false);
        out.records.push_back(rec);
    }
    return ok;
}

} // namespace

bool Parse(std::string_view shp, Layer& out, std::wstring& error) {
    out = Layer{};
    if (shp.size() < kHeaderBytes || Be32(shp, 0) != static_cast<uint32_t>(kFileCode)) {
        error = L"it is not a shapefile (.shp)";
        return false;
    }
    const int base = BaseType(Le32(shp, 32));
    if (base <= 0) {
        error = base == 0 ? L"it holds only null shapes" : L"its shape type (" + std::to_wstring(Le32(shp, 32)) + L") is not one drawn here";
        return false;
    }
    out.kind = KindOf(base);
    const size_t end = std::min(shp.size(), size_t{ Be32(shp, 24) } * 2);   // the length is in 16-bit words
    size_t at = kHeaderBytes;
    for (size_t n = 0; at + 8 <= end; ++n) {
        if (n >= kMaxRecords) { error = L"it has more than " + std::to_wstring(kMaxRecords) + L" shapes"; return false; }
        const size_t len = size_t{ Be32(shp, at + 4) } * 2;
        if (at + 8 + len > end) { error = L"it is cut short"; return false; }
        if (!ParseRecord(shp.substr(at + 8, len), base, out, error)) { return false; }
        at += 8 + len;
    }
    if (out.records.empty()) { error = L"it holds no shapes"; return false; }
    assert(out.xy.size() % 2 == 0);
    return true;
}

bool PrjIsGeographic(std::string_view prj, std::wstring& error) {
    const size_t start = prj.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    const std::string_view t = start == std::string_view::npos ? std::string_view() : prj.substr(start);
    if (t.starts_with("GEOGCS[")) { return true; }
    error = t.starts_with("PROJCS[") ? L"it is in a projected coordinate system; only longitude and latitude are drawn"
                                     : L"its .prj names no coordinate system WeatherTool knows";
    return false;
}

bool Load(const std::wstring& path, Layer& out, std::wstring& error) {
    assert(!path.empty());
    std::string bytes;
    if (!ReadFileBytes(path, bytes, kMaxFileBytes, error)) { return false; }
    if (!Parse(bytes, out, error)) { return false; }
    const size_t dot = path.find_last_of(L'.');
    const std::wstring prjPath = (dot == std::wstring::npos ? path : path.substr(0, dot)) + L".prj";
    std::string prj;
    if (GetFileAttributesW(prjPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!ReadFileBytes(prjPath, prj, 64u * 1024u, error)) { return false; }
        if (!PrjIsGeographic(prj, error)) { out = Layer{}; return false; }
    } else if (out.box.x0 < -180.5f || out.box.x1 > 180.5f || out.box.y0 < -90.5f || out.box.y1 > 90.5f) {
        error = L"it has no .prj and its coordinates are not longitude and latitude";
        out = Layer{};
        return false;
    }
    return true;
}

void ToMercator(Layer& layer) {
    assert(layer.xy.size() % 2 == 0);
    const double w = slippy::WorldSize(0);
    auto fx = [w](float lon) { return static_cast<float>(slippy::LonToX(lon, 0) / w); };
    auto fy = [w](float lat) { return static_cast<float>(slippy::LatToY(lat, 0) / w); };
    for (size_t k = 0; k + 1 < layer.xy.size(); k += 2) {
        layer.xy[k]     = fx(layer.xy[k]);
        layer.xy[k + 1] = fy(layer.xy[k + 1]);
    }
    auto box = [&](Box& b) { b = { fx(b.x0), fy(b.y1), fx(b.x1), fy(b.y0) }; };   // north is the smaller y
    for (Record& r : layer.records) { box(r.box); }
    box(layer.box);
    assert(layer.box.x0 <= layer.box.x1 && layer.box.y0 <= layer.box.y1);
}

std::wstring NameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring n = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = n.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) { n.resize(dot); }
    return n;
}

} // namespace wt::shapes
