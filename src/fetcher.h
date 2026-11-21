// Worker thread: runs HTTP jobs one at a time off the UI thread and posts a
// message to the requesting window when a result is ready. Jobs carry a
// fully built URL and the caller's key, and results echo the key, so the
// worker never reads configuration and a removed place cannot receive data
// meant for another.
#pragma once

#include "common.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace wt {

class HttpClient;

constexpr UINT WM_APP_ARCHIVE = WM_APP + 1;   // an ArchiveResult is ready
constexpr UINT WM_APP_SEARCH  = WM_APP + 2;   // a SearchResult is ready

struct ArchiveResult {
    std::wstring key;                 // the place's cache key
    bool         ok = false;
    bool         full = false;        // a whole-history download (not an update)
    bool         retrying = false;    // progress note only: rate limited, will retry
    std::wstring error;
    uint32_t     status = 0;
    uint32_t     elapsedMs = 0;
    size_t       bytes = 0;
    std::unique_ptr<Climate> data;    // the months in the reply (ok only)
};

struct SearchResult {
    uint64_t     seq = 0;             // echoes QueueSearch's seq
    bool         ok = false;
    std::wstring error;
    std::array<GeoHit, kMaxHits> hits{};
    size_t       count = 0;
};

class Fetcher {
public:
    Fetcher() = default;
    ~Fetcher();
    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;

    bool Start(std::wstring& err);
    void Stop();   // waits for the job in flight, if any

    bool QueueArchive(HWND notify, const std::wstring& key, const std::wstring& url, bool full);
    bool QueueSearch(HWND notify, uint64_t seq, const std::wstring& url);
    bool TakeArchive(ArchiveResult& out);
    bool TakeSearch(SearchResult& out);

    bool   IsPending(const std::wstring& key) const;   // queued or in flight
    size_t PendingArchives() const;

private:
    enum class Kind { Archive, Search };
    struct Job {
        Kind         kind = Kind::Archive;
        HWND         notify = nullptr;
        std::wstring key;
        std::wstring url;
        uint64_t     seq = 0;
        bool         full = false;
    };
    static constexpr size_t kMaxJobs    = 64;
    static constexpr size_t kMaxResults = 64;
    static constexpr int    kMaxAttempts = 4;    // first try + 3 retries after HTTP 429
    static constexpr uint32_t kMaxRetryWait = 90; // longer Retry-After: stop asking until then

    void Run();
    bool WaitOrStop(uint32_t seconds);   // false when Stop() was called meanwhile
    void PushArchive(HWND notify, ArchiveResult&& r);
    void RunArchive(const Job& job, HttpClient& http);
    void RunSearch(const Job& job, HttpClient& http);

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::deque<Job>         jobs_;
    std::deque<ArchiveResult> archives_;
    std::deque<SearchResult>  searches_;
    std::wstring            inFlight_;    // key of the archive job running now
    bool                    stop_ = false;
    std::thread             worker_;
    std::unique_ptr<char[]> buf_;
    // Worker thread only: archive requests are refused until this tick count.
    ULONGLONG               blockedUntil_ = 0;
    std::wstring            blockedNote_;
};

} // namespace wt
