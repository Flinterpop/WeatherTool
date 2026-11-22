#include "tiles.h"

#include "config.h"
#include "http.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <cassert>
#include <chrono>
#include <ctime>
#include <cwchar>

namespace wt {
namespace {

constexpr int64_t  kMapMaxAge     = 7 * 86400;   // OpenStreetMap: keep tiles at least 7 days
constexpr int64_t  kRadarMaxAge   = 3 * 3600;    // radar frames are 2 hours deep
constexpr size_t   kRadarPerMin   = 90;          // RainViewer allows 100 a minute per address
constexpr size_t   kMaxTileBytes  = 2u * 1024u * 1024u;
constexpr int      kMaxCleanFiles = 20000;

int64_t NowUnix() { return static_cast<int64_t>(_time64(nullptr)); }

// Seconds since the file was written; -1 when there is no such file.
int64_t FileAge(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) { return -1; }
    ULARGE_INTEGER u{};
    u.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    constexpr ULONGLONG kEpoch = 116444736000000000ULL;
    const int64_t written = u.QuadPart > kEpoch ? static_cast<int64_t>((u.QuadPart - kEpoch) / 10000000ULL) : 0;
    return NowUnix() - written;
}

// PNG or JPEG bytes to a premultiplied 32-bit bitmap that owns its pixels
// (a bitmap made straight from a stream needs the stream to outlive it).
std::unique_ptr<Gdiplus::Bitmap> Decode(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > kMaxTileBytes) { return nullptr; }
    IStream* s = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
    if (s == nullptr) { return nullptr; }
    std::unique_ptr<Gdiplus::Bitmap> out;
    {
        std::unique_ptr<Gdiplus::Bitmap> src(Gdiplus::Bitmap::FromStream(s));
        if (src && src->GetLastStatus() == Gdiplus::Ok && src->GetWidth() > 0 && src->GetWidth() <= 1024) {
            const INT w = static_cast<INT>(src->GetWidth());
            const INT h = static_cast<INT>(src->GetHeight());
            out = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
            Gdiplus::Graphics g(out.get());
            if (out->GetLastStatus() != Gdiplus::Ok || g.DrawImage(src.get(), 0, 0, w, h) != Gdiplus::Ok) { out.reset(); }
        }
    }
    s->Release();
    return out;
}

} // namespace

TileStore::~TileStore() { Stop(); }

bool TileStore::Start(HWND notify, const std::wstring& cacheDir, std::wstring& err) {
    assert(notify != nullptr && !worker_.joinable());
    notify_ = notify;
    dir_ = cacheDir + L"\\tiles";
    const int rc = SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
        err = L"Cannot create " + dir_;
        return false;
    }
    CleanOldRadar();
    try {
        worker_ = std::thread([this] { Run(); });
    } catch (const std::system_error&) {
        err = L"Could not start the map download thread";
        return false;
    }
    return true;
}

void TileStore::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
}

void TileStore::NewFrame() {
    std::lock_guard<std::mutex> lock(mu_);
    ++gen_;
}

Gdiplus::Bitmap* TileStore::Peek(const std::wstring& key) {
    const auto it = cache_.find(key);
    if (it == cache_.end()) { return nullptr; }
    it->second.used = ++useClock_;
    return it->second.bmp.get();
}

Gdiplus::Bitmap* TileStore::Get(const std::wstring& key, const std::wstring& url, bool radarTile) {
    if (Gdiplus::Bitmap* b = Peek(key)) { return b; }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_ || missing_.count(key) != 0) { return nullptr; }
        if (pending_.count(key) == 0) {
            if (jobs_.size() >= kMaxJobs) {   // the oldest request is the least likely still on screen
                pending_.erase(jobs_.front().key);
                jobs_.pop_front();
            }
            jobs_.push_back(Job{ key, url, radarTile, gen_ });
            pending_.insert(key);
        } else {
            for (Job& j : jobs_) {   // still wanted: keep it from going stale
                if (j.key == key) { j.gen = gen_; break; }
            }
        }
    }
    cv_.notify_one();
    return nullptr;
}

void TileStore::TakeDone() {
    std::deque<Done> got;
    {
        std::lock_guard<std::mutex> lock(mu_);
        got.swap(done_);
        posted_ = false;
    }
    for (Done& d : got) {
        if (d.bmp) { cache_[d.key] = Cached{ std::move(d.bmp), ++useClock_ }; }
    }
    Evict();
}

void TileStore::Evict() {
    // Least recently drawn first; runs between paints, so no pointer from Get is live.
    for (int guard = 0; guard < 1000 && cache_.size() > kMaxCached; ++guard) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.used < oldest->second.used) { oldest = it; }
        }
        cache_.erase(oldest);
    }
}

void TileStore::RequestFrames() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        framesWanted_ = true;
        missing_.clear();   // a tile that failed earlier gets another chance
    }
    cv_.notify_one();
}

bool TileStore::TakeFrames(radar::Frames& out, std::wstring& err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!framesReady_) { return false; }
    framesReady_ = false;
    if (frames_.count == 0) {
        err = framesErr_;
        return false;
    }
    out = frames_;
    return true;
}

size_t TileStore::Queued() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
}

std::wstring TileStore::LastError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lastErr_;
}

bool TileStore::RadarSlot() {
    for (int guard = 0; guard < 600; ++guard) {   // at most ~10 minutes
        const ULONGLONG now = GetTickCount64();
        while (!radarTimes_.empty() && now - radarTimes_.front() > 60000) { radarTimes_.pop_front(); }
        if (radarTimes_.size() < kRadarPerMin) {
            radarTimes_.push_back(now);
            return true;
        }
        std::unique_lock<std::mutex> lock(mu_);
        if (cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) { return false; }
    }
    return false;
}

bool TileStore::Fetch(const Job& job, std::string& bytes, std::wstring& err) {
    const std::wstring file = dir_ + L"\\" + job.key + L".png";
    const int64_t age = FileAge(file);
    if (age >= 0 && age < (job.radar ? kRadarMaxAge : kMapMaxAge) && ReadFileBytes(file, bytes, kMaxTileBytes, err)) {
        return true;
    }
    if (job.radar && !RadarSlot()) { return false; }
    static thread_local HttpClient http;   // the worker's own session
    static thread_local std::unique_ptr<char[]> buf;
    if (!buf) {
        http.UseAppIdentity();
        buf = std::make_unique<char[]>(kMaxTileBytes);
    }
    HttpResult hr;
    if (http.Get(job.url, buf.get(), kMaxTileBytes, hr, err) && hr.status == 200) {
        bytes.assign(buf.get(), hr.length);
        std::wstring ignored;
        (void)WriteFileBytes(file, bytes, ignored);   // a cache that cannot be written only costs a download
        return true;
    }
    if (err.empty()) { err = L"HTTP " + std::to_wstring(hr.status) + L" for " + job.url; }
    // Offline or refused: an expired copy is better than nothing.
    std::wstring ignored;
    return age >= 0 && ReadFileBytes(file, bytes, kMaxTileBytes, ignored);
}

void TileStore::Run() {
    for (;;) {   // the service loop: ends on Stop()
        Job job;
        bool frames = false;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || framesWanted_ || !jobs_.empty(); });
            if (stop_) { return; }
            if (framesWanted_) {
                framesWanted_ = false;
                frames = true;
            } else {
                job = std::move(jobs_.back());   // newest first: what is on screen now
                jobs_.pop_back();
                if (job.gen + 1 < gen_) {        // not asked for in the last two paints: scrolled away
                    pending_.erase(job.key);
                    continue;
                }
            }
        }
        if (frames) {
            HttpClient http;
            auto buf = std::make_unique<char[]>(256 * 1024);
            HttpResult hr;
            std::wstring err;
            radar::Frames f;
            const bool ok = http.Get(radar::kFramesUrl, buf.get(), 256 * 1024, hr, err) && hr.status == 200 &&
                            radar::ParseFrames(std::string_view(buf.get(), hr.length), f, err);
            if (err.empty() && !ok) { err = L"RainViewer answered HTTP " + std::to_wstring(hr.status); }
            {
                std::lock_guard<std::mutex> lock(mu_);
                frames_ = ok ? f : radar::Frames{};
                framesErr_ = err;
                framesReady_ = true;
            }
            PostMessageW(notify_, WM_APP_FRAMES, 0, 0);
            continue;
        }
        std::string bytes;
        std::wstring err;
        std::unique_ptr<Gdiplus::Bitmap> bmp;
        if (Fetch(job, bytes, err)) { bmp = Decode(bytes); }
        bool post = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_.erase(job.key);
            if (stop_) { return; }
            if (!bmp) {
                missing_.insert(job.key);
                lastErr_ = err.empty() ? L"could not decode " + job.key : err;
            }
            done_.push_back(Done{ job.key, std::move(bmp) });
            if (!posted_) {
                posted_ = true;
                post = true;
            }
        }
        if (post) { PostMessageW(notify_, WM_APP_TILES, 0, 0); }
    }
}

void TileStore::CleanOldRadar() {
    // Radar tiles are named radar_<unix time>_z_x_y; frames older than kRadarMaxAge are no longer listed.
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir_ + L"\\radar_*.png").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) { return; }
    const int64_t cutoff = NowUnix() - kRadarMaxAge;
    for (int i = 0; i < kMaxCleanFiles; ++i) {
        const int64_t t = _wtoi64(fd.cFileName + 6);
        if (t > 0 && t < cutoff) { DeleteFileW((dir_ + L"\\" + fd.cFileName).c_str()); }
        if (!FindNextFileW(find, &fd)) { break; }
    }
    FindClose(find);
}

} // namespace wt
