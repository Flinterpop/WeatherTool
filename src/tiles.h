// The Map tab's tiles: one worker thread that downloads, caches on disk and
// decodes map and radar tiles, and a bounded in-memory cache of decoded
// bitmaps the UI thread draws from. Only tiles the map asked for in its last
// two paints are fetched (no prefetching, as OpenStreetMap's tile policy
// requires); map tiles are kept on disk for 7 days, radar tiles for 3 hours;
// radar requests stay under RainViewer's 100 a minute.
#pragma once

#include "gdiplus_inc.h"
#include "radar.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace wt {

constexpr UINT WM_APP_TILES  = WM_APP + 5;   // decoded tiles are waiting: call TakeDone
constexpr UINT WM_APP_FRAMES = WM_APP + 6;   // the radar frame list arrived: call TakeFrames

class TileStore {
public:
    TileStore() = default;
    ~TileStore();
    TileStore(const TileStore&) = delete;
    TileStore& operator=(const TileStore&) = delete;

    bool Start(HWND notify, const std::wstring& cacheDir, std::wstring& err);
    void Stop();

    // UI thread. Call NewFrame at the start of each paint, then Get for each
    // tile the paint draws: the decoded bitmap, or nullptr after queuing it
    // (unless it is known to be missing). `key` names the tile and its cache
    // file ("osm_z_x_y", "radar_t_z_x_y").
    void NewFrame();
    Gdiplus::Bitmap* Get(const std::wstring& key, const std::wstring& url, bool radar);
    Gdiplus::Bitmap* Peek(const std::wstring& key);   // cached only, never queues
    void TakeDone();                                    // on WM_APP_TILES

    void RequestFrames();                               // the radar frame list
    bool TakeFrames(radar::Frames& out, std::wstring& err);

    size_t Queued() const;
    std::wstring LastError() const;

private:
    struct Job {
        std::wstring key, url;
        bool         radar = false;
        uint64_t     gen = 0;
    };
    struct Done {
        std::wstring key;
        std::unique_ptr<Gdiplus::Bitmap> bmp;   // null: missing
    };
    struct Cached {
        std::unique_ptr<Gdiplus::Bitmap> bmp;
        uint64_t used = 0;
    };
    static constexpr size_t kMaxJobs   = 256;
    static constexpr size_t kMaxCached = 240;   // ~60 MB of 256 px tiles at most

    void Run();
    bool Fetch(const Job& job, std::string& bytes, std::wstring& err);
    bool RadarSlot();                            // worker: wait for a free radar request slot; false on Stop
    void Evict();
    void CleanOldRadar();

    HWND         notify_ = nullptr;
    std::wstring dir_;                           // cacheDir\tiles
    std::thread  worker_;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    bool                    stop_ = false;
    uint64_t                gen_ = 0;
    std::deque<Job>         jobs_;
    std::unordered_set<std::wstring> pending_;   // queued or in flight
    std::unordered_set<std::wstring> missing_;   // 404s and failures this session
    std::deque<Done>        done_;
    bool                    posted_ = false;
    bool                    framesWanted_ = false;
    bool                    framesReady_ = false;
    radar::Frames           frames_;
    std::wstring            framesErr_;
    std::wstring            lastErr_;
    std::deque<ULONGLONG>   radarTimes_;         // worker only

    std::unordered_map<std::wstring, Cached> cache_;   // UI thread only
    uint64_t useClock_ = 0;
};

} // namespace wt
