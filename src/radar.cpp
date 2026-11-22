#include "radar.h"

#include "jsonscan.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ctime>
#include <cwchar>

namespace wt::radar {

bool ParseFrames(std::string_view j, Frames& out, std::wstring& error) {
    Frames f;
    size_t p = json::FindKey(j, "host", 0, j.size());
    if (p == json::npos || !json::ReadString(j, p, f.host) || f.host.rfind(L"https://", 0) != 0) {
        error = L"RainViewer's frame list has no usable host.";
        return false;
    }
    const size_t r = json::FindKey(j, "radar", 0, j.size());
    const size_t rEnd = r == json::npos ? json::npos : json::SkipContainer(j, r);
    const size_t past = r == json::npos || rEnd == json::npos ? json::npos : json::FindKey(j, "past", r, rEnd);
    if (past == json::npos) {
        error = L"RainViewer's frame list has no radar frames.";
        return false;
    }
    json::ArrayReader a(j, past);
    size_t el = 0;
    for (size_t i = 0; i < 64 && f.count < kMaxFrames && a.NextElement(el); ++i) {
        const size_t end = json::SkipContainer(j, el);
        if (end == json::npos) { break; }
        size_t t = json::FindKey(j, "time", el, end);
        size_t q = json::FindKey(j, "path", el, end);
        double time = 0.0;
        Frame fr;
        if (t == json::npos || q == json::npos || !json::ReadNumber(j, t, time) || !json::ReadString(j, q, fr.path)) { continue; }
        if (std::isnan(time) || fr.path.empty() || fr.path[0] != L'/') { continue; }
        fr.time = static_cast<int64_t>(time);
        f.frame[f.count++] = fr;
    }
    if (f.count == 0) {
        error = L"RainViewer's frame list has no radar frames.";
        return false;
    }
    out = f;
    return true;
}

std::wstring TileUrl(const Frames& f, size_t frame, int z, int x, int y) {
    assert(frame < f.count && z >= 0 && z <= kMaxZoom);
    return f.host + f.frame[frame].path + L"/" + std::to_wstring(kTileSize) + L"/" + std::to_wstring(z) + L"/" +
           std::to_wstring(x) + L"/" + std::to_wstring(y) + L"/2/1_1.png";
}

int MaxZoom(Source s) { return s == Source::NorthAmerica ? kMscMaxZoom : kMaxZoom; }

namespace {

bool Digits(std::string_view s, size_t at, size_t n, int& out) {
    if (at + n > s.size()) { return false; }
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') { return false; }
        v = v * 10 + (s[i] - '0');
    }
    out = v;
    return true;
}

// "PT6M", "PT1H", "PT10M": seconds; 0 when not one of those shapes.
int64_t ParsePeriod(std::string_view p) {
    if (p.size() < 4 || p.substr(0, 2) != "PT") { return 0; }
    int n = 0;
    if (!Digits(p, 2, p.size() - 3, n) || n <= 0) { return 0; }
    const char unit = p.back();
    return unit == 'M' ? n * 60LL : unit == 'H' ? n * 3600LL : unit == 'S' ? static_cast<int64_t>(n) : 0;
}

struct Box { double w, s, e, n; };   // degrees
// The layers' own bounding boxes from GetCapabilities, 22 Nov 2026, widened a little.
constexpr Box kRadarBox  = { -171.0, 16.0, -49.0, 68.0 };
constexpr Box kAlertsBox = { -142.0, 41.0, -52.0, 84.0 };

constexpr double kHalfWorld = 20037508.342789244;   // metres, EPSG:3857

} // namespace

int64_t ParseIsoUtc(std::string_view s) {
    // YYYY-MM-DDTHH:MM:SSZ
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') { return -1; }
    if (!Digits(s, 0, 4, Y) || !Digits(s, 5, 2, M) || !Digits(s, 8, 2, D) || !Digits(s, 11, 2, h) || !Digits(s, 14, 2, m) ||
        !Digits(s, 17, 2, sec)) {
        return -1;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) { return -1; }
    tm t{};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = sec;
    const __time64_t u = _mkgmtime64(&t);
    return u < 0 ? -1 : static_cast<int64_t>(u);
}

std::wstring IsoUtc(int64_t t) {
    tm g{};
    const __time64_t u = t;
    if (_gmtime64_s(&g, &u) != 0) { return L""; }
    std::array<wchar_t, 32> b{};
    swprintf_s(b.data(), b.size(), L"%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour,
               g.tm_min, g.tm_sec);
    return b.data();
}

bool ParseMscTimes(std::string_view xml, Frames& out, std::wstring& error) {
    error = L"GeoMet's radar layer lists no times.";
    const size_t d = xml.find("<Dimension name=\"time\"");
    const size_t gt = d == std::string_view::npos ? d : xml.find('>', d);
    const size_t lt = gt == std::string_view::npos ? gt : xml.find('<', gt);
    if (lt == std::string_view::npos) { return false; }
    std::string_view v = xml.substr(gt + 1, lt - gt - 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\n' || v.front() == '\r')) { v.remove_prefix(1); }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r')) { v.remove_suffix(1); }

    // The times on offer, newest first, at most 64.
    std::array<int64_t, 64> have{};
    size_t n = 0;
    const size_t s1 = v.find('/');
    if (s1 != std::string_view::npos) {   // start/end/period
        const size_t s2 = v.find('/', s1 + 1);
        if (s2 == std::string_view::npos) { return false; }
        const int64_t start = ParseIsoUtc(v.substr(0, s1));
        const int64_t end = ParseIsoUtc(v.substr(s1 + 1, s2 - s1 - 1));
        const int64_t period = ParsePeriod(v.substr(s2 + 1));
        if (start < 0 || end < start || period <= 0) { return false; }
        for (int64_t t = end; t >= start && n < have.size(); t -= period) { have[n++] = t; }
    } else {                              // a comma list, oldest first
        std::array<int64_t, 64> list{};
        size_t k = 0;
        size_t p = 0;
        for (int guard = 0; guard < 4096 && p <= v.size(); ++guard) {
            const size_t c = std::min(v.find(',', p), v.size());
            const int64_t t = ParseIsoUtc(v.substr(p, c - p));
            if (t >= 0) {
                if (k == list.size()) {   // keep the newest 64
                    for (size_t i = 1; i < k; ++i) { list[i - 1] = list[i]; }
                    --k;
                }
                list[k++] = t;
            }
            p = c + 1;
        }
        for (size_t i = 0; i < k; ++i) { have[n++] = list[k - 1 - i]; }
    }
    if (n == 0) { return false; }

    // Every kMscStep back from the newest, using only times the layer has.
    Frames f;
    f.source = Source::NorthAmerica;
    std::array<int64_t, kMaxFrames> pick{};
    size_t m = 0;
    for (size_t i = 0; i < n && m < kMaxFrames; ++i) {
        if ((have[0] - have[i]) % kMscStep == 0) { pick[m++] = have[i]; }
    }
    assert(m >= 1);
    for (size_t i = 0; i < m; ++i) { f.frame[i].time = pick[m - 1 - i]; }
    f.count = m;
    out = f;
    error.clear();
    return true;
}

bool MscCovers(MscLayer layer, int z, int x, int y) {
    assert(z >= 0 && z <= 20);
    const Box& b = layer == MscLayer::Radar ? kRadarBox : kAlertsBox;
    const double n = static_cast<double>(1 << z);
    const double w = x / n * 360.0 - 180.0;
    const double e = (x + 1) / n * 360.0 - 180.0;
    auto lat = [n](double ty) { return std::atan(std::sinh(3.14159265358979323846 * (1.0 - 2.0 * ty / n))) * 180.0 / 3.14159265358979323846; };
    const double north = lat(y);
    const double south = lat(y + 1.0);
    return e > b.w && w < b.e && north > b.s && south < b.n;
}

std::wstring MscTileUrl(MscLayer layer, int64_t time, int z, int x, int y) {
    assert(z >= 0 && z <= 20 && x >= 0 && y >= 0);
    const double size = 2.0 * kHalfWorld / static_cast<double>(1 << z);
    const double minx = -kHalfWorld + x * size;
    const double maxy = kHalfWorld - y * size;
    std::array<wchar_t, 128> bbox{};
    swprintf_s(bbox.data(), bbox.size(), L"%.2f,%.2f,%.2f,%.2f", minx, maxy - size, minx + size, maxy);
    std::wstring u = L"https://geo.weather.gc.ca/geomet?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&FORMAT=image/png"
                     L"&TRANSPARENT=TRUE&CRS=EPSG:3857&WIDTH=512&HEIGHT=512&BBOX=";
    u += bbox.data();
    if (layer == MscLayer::Radar) {
        u += L"&LAYERS=Radar_1km_SfcPrecipType&STYLES=SfcPrecipType_Dis&TIME=" + IsoUtc(time);
    } else {
        u += L"&LAYERS=Current-Alerts&STYLES=Current-Alerts";
    }
    return u;
}

} // namespace wt::radar
