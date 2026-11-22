#include "radar.h"

#include "jsonscan.h"

#include <cassert>
#include <cmath>

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

} // namespace wt::radar
