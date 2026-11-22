// RainViewer's free weather-radar API for the Map tab: the frame list and the
// tile addresses. Free for personal and educational use with credit to
// RainViewer; since January 2026 it serves only the past 2 hours (10-minute
// frames), zoom 7 at most, one colour scheme (2, "Universal Blue"), and at
// most 100 requests a minute per address (rainviewer.com/api, read
// 21 Nov 2026). UI-free and unit-tested.
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

struct Frame {
    int64_t      time = 0;   // Unix time of the radar picture
    std::wstring path;       // "/v2/radar/1609401600"
};

struct Frames {
    std::wstring host;       // "https://tilecache.rainviewer.com"
    std::array<Frame, kMaxFrames> frame{};
    size_t count = 0;        // oldest first
};

// The frame list. False, with why, when it has no host or no past frames.
bool ParseFrames(std::string_view json, Frames& out, std::wstring& error);

// One radar tile: scheme 2, smoothed, snow shown in its own colours. Only a
// host that starts with "https://" and a path that starts with "/" are used.
std::wstring TileUrl(const Frames& f, size_t frame, int z, int x, int y);

} // namespace wt::radar
