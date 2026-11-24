// The Map tab's two radar sources and the weather-alert overlay: frame lists
// and tile addresses. UI-free and unit-tested.
//
// World: RainViewer's free weather-radar API. Free for personal and
// educational use with credit to RainViewer; since January 2026 it serves only
// the past 2 hours (10-minute frames), zoom 7 at most, one colour scheme (2,
// "Universal Blue"), and at most 100 requests a minute per address
// (rainviewer.com/api, read 21 Nov 2026).
//
// North America: the Meteorological Service of Canada's GeoMet WMS, read
// 22 Nov 2026. Open licence (ECCC Data Services End-use Licence), anonymous,
// credit "Data Source: Environment and Climate Change Canada". Its usage
// policy asks for a meaningful User-Agent, no no-cache headers, no bulk or
// batch tile retrieval, and contact above 86,400 requests a day (about one a
// second) for an application as a whole. So frames are taken 12 minutes apart
// (the layer has one every 6), tiles only for what is on screen, and none
// outside the layer's own bounding box. The radar layer is the 1 km North
// American composite of surface precipitation type (rain, snow, mixed,
// freezing rain, each light to heavy), the last 3 hours; the alerts layer has
// no time and shows the alerts in effect. Satellite: NOAA's GOES-East and
// GOES-West "Day Visible / Night IR" 1 km pictures as GeoMet serves them,
// every 10 minutes; each tile comes from the satellite that sees it better.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace wt::radar {

constexpr wchar_t kFramesUrl[] = L"https://api.rainviewer.com/public/weather-maps.json";
constexpr int     kMaxZoom     = 7;     // RainViewer's free limit
constexpr int     kTileSize    = 512;   // one 512 px tile at zoom z-1 covers four 256 px map tiles at z
constexpr size_t  kMaxFrames   = 16;    // 2 hours at 10 minutes is 13

enum class Source : int { World = 0, NorthAmerica = 1 };

constexpr wchar_t kMscCapsUrl[] =
    L"https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=Radar_1km_SfcPrecipType";
constexpr int     kMscMaxZoom  = 8;      // the radar is 1 km: closer than this, tiles are only scaled up
constexpr int64_t kMscStep     = 720;    // seconds between frames shown (the layer has one every 6 minutes)
constexpr wchar_t kSatCapsUrl[] =
    L"https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=GOES-East_1km_DayVis-NightIR";
constexpr int64_t kSatStep     = 600;    // the satellite layers have a picture every 10 minutes

struct Frame {
    int64_t      time = 0;   // Unix time of the radar picture
    std::wstring path;       // RainViewer: "/v2/radar/1609401600"; GeoMet: empty
};

struct Frames {
    Source       source = Source::World;
    std::wstring host;       // RainViewer: "https://tilecache.rainviewer.com"; GeoMet: empty
    std::array<Frame, kMaxFrames> frame{};
    size_t count = 0;        // oldest first
    int64_t satLatest = 0;   // the newest satellite picture, 0 when not asked for or not known
};

// The highest radar tile zoom the source serves.
int MaxZoom(Source s);

// The frame list. False, with why, when it has no host or no past frames.
bool ParseFrames(std::string_view json, Frames& out, std::wstring& error);

// One radar tile: scheme 2, smoothed, snow shown in its own colours. Only a
// host that starts with "https://" and a path that starts with "/" are used.
std::wstring TileUrl(const Frames& f, size_t frame, int z, int x, int y);

// GeoMet: the radar layer's time dimension from its GetCapabilities
// ("start/end/PT6M", or a comma list), as up to kMaxFrames frames kMscStep
// apart ending at the latest. False, with why, when there is none.
bool ParseMscTimes(std::string_view xml, Frames& out, std::wstring& error);

// "2026-11-22T01:54:00Z" (or with fractional seconds, "...:00.422Z") to Unix
// time; -1 when it is not that shape.
int64_t ParseIsoUtc(std::string_view s);
std::wstring IsoUtc(int64_t t);

// The newest time a GeoMet layer's GetCapabilities lists; -1 when none.
int64_t MscLatest(std::string_view xml);

enum class MscLayer { Radar, Alerts, SatEast, SatWest };

// A 512 px GetMap of one Web Mercator tile (z, x, y) of the layer; `time` is
// the picture's (ignored for alerts).
std::wstring MscTileUrl(MscLayer layer, int64_t time, int z, int x, int y);

// Whether the tile touches the layer's bounding box (no request otherwise).
bool MscCovers(MscLayer layer, int z, int x, int y);

// What the alerts layer has at one point, from a WMS GetFeatureInfo (JSON).
// The reply carries each alert's polygon too, so it can run to tens of kB.
constexpr size_t kMaxAlerts         = 8;
constexpr size_t kMaxAlertInfoBytes = 1024u * 1024u;

struct Alert {
    std::wstring name;     // "snowfall warning" (alert_name_en)
    std::wstring colour;   // "yellow", "orange" or "red"; empty for a statement
    std::wstring area;     // "Fraserdale - Pledger Lake"
    std::wstring status;   // "issued", "continued", "updated", ...
    std::wstring text;     // the bulletin, paragraphs separated by blank lines
    int64_t issued = -1;   // publication time (Unix), -1 when not given
    int64_t ends   = -1;   // when the event is expected to end, -1 when not given
};

struct AlertInfo {
    std::array<Alert, kMaxAlerts> alert{};
    size_t count = 0;      // most severe first: red, orange, yellow, then statements
};

// Whether (lat, lon) is inside the alerts layer's bounding box.
bool AlertsCoverPoint(double lat, double lon);

// The GetFeatureInfo for the alerts in effect at (lat, lon).
std::wstring AlertInfoUrl(double lat, double lon);

// The reply to AlertInfoUrl, in English. No alert there is a success with
// count 0; false, with why, when the reply is not a feature collection.
bool ParseAlertInfo(std::string_view json, AlertInfo& out, std::wstring& error);

// Which satellite to ask for the tile: GOES-East east of 106°W, GOES-West
// west of it, each within its own view. False when neither sees the tile.
bool SatFor(int z, int x, int y, MscLayer& out);

// The satellite picture to show with a radar picture taken at `t`: the
// newest at or before it, but none newer than `latest`.
int64_t SatTimeFor(int64_t t, int64_t latest);

} // namespace wt::radar
