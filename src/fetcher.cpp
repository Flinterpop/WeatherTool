#include "fetcher.h"

#include "climate.h"
#include "http.h"

#include <cassert>
#include <chrono>
#include <string_view>

namespace wt {

Fetcher::~Fetcher() { Stop(); }

bool Fetcher::Start(std::wstring& err) {
    assert(!worker_.joinable());
    buf_ = std::make_unique<char[]>(kHttpBufSize);
    try {
        worker_ = std::thread([this] { Run(); });
    } catch (const std::system_error&) {
        err = L"Could not start the download thread";
        return false;
    }
    return true;
}

void Fetcher::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) { worker_.join(); }
}

bool Fetcher::QueueArchive(HWND notify, const std::wstring& key, const std::wstring& url, bool full) {
    assert(notify != nullptr && !key.empty());
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (jobs_.size() >= kMaxJobs || stop_) { return false; }
        for (const Job& j : jobs_) {
            if (j.kind == Kind::Archive && j.key == key) { return true; }   // already queued
        }
        Job j;
        j.kind = Kind::Archive;
        j.notify = notify;
        j.key = key;
        j.url = url;
        j.full = full;
        jobs_.push_back(std::move(j));
    }
    cv_.notify_one();
    return true;
}

bool Fetcher::QueueSearch(HWND notify, uint64_t seq, const std::wstring& url) {
    assert(notify != nullptr);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (jobs_.size() >= kMaxJobs || stop_) { return false; }
        Job j;
        j.kind = Kind::Search;
        j.notify = notify;
        j.url = url;
        j.seq = seq;
        // A search jumps the queue: someone is waiting at the dialog.
        jobs_.push_front(std::move(j));
    }
    cv_.notify_one();
    return true;
}

bool Fetcher::TakeArchive(ArchiveResult& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (archives_.empty()) { return false; }
    out = std::move(archives_.front());
    archives_.pop_front();
    return true;
}

bool Fetcher::TakeSearch(SearchResult& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (searches_.empty()) { return false; }
    out = std::move(searches_.front());
    searches_.pop_front();
    return true;
}

bool Fetcher::IsPending(const std::wstring& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (inFlight_ == key) { return true; }
    for (const Job& j : jobs_) {
        if (j.kind == Kind::Archive && j.key == key) { return true; }
    }
    return false;
}

size_t Fetcher::PendingArchives() const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = inFlight_.empty() ? 0 : 1;
    for (const Job& j : jobs_) { n += (j.kind == Kind::Archive) ? 1 : 0; }
    return n;
}

void Fetcher::Run() {
    HttpClient http;
    for (;;) {   // the service loop: ends on Stop()
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) { return; }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            if (job.kind == Kind::Archive) { inFlight_ = job.key; }
        }
        if (job.kind == Kind::Archive) {
            RunArchive(job, http);
        } else {
            RunSearch(job, http);
        }
    }
}

bool Fetcher::WaitOrStop(uint32_t seconds) {
    std::unique_lock<std::mutex> lock(mu_);
    return !cv_.wait_for(lock, std::chrono::seconds(seconds), [this] { return stop_; });
}

void Fetcher::PushArchive(HWND notify, ArchiveResult&& r) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (archives_.size() >= kMaxResults) { archives_.pop_front(); }
        archives_.push_back(std::move(r));
    }
    PostMessageW(notify, WM_APP_ARCHIVE, 0, 0);
}

void Fetcher::RunArchive(const Job& job, HttpClient& http) {
    ArchiveResult r;
    r.key  = job.key;
    r.full = job.full;
    HttpResult hr;
    std::wstring err;
    bool got = false;
    // Open-Meteo rate-limits per minute, hour and day, and a whole-history
    // download counts as roughly 400 calls (measured 21 Nov 2026). A short
    // Retry-After (the per-minute limit, when several places are added at
    // once) is waited out here; a long one (the hourly or daily limit) blocks
    // every archive job until then, failing fast instead of hammering, and
    // the app's periodic update tries again later.
    const ULONGLONG now = GetTickCount64();
    if (now < blockedUntil_) {
        r.error = blockedNote_;
        std::lock_guard<std::mutex> lock(mu_);
        inFlight_.clear();
    } else {
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            err.clear();
            got = http.Get(job.url, buf_.get(), kHttpBufSize, hr, err);
            if (!got || hr.status != 429) { break; }
            const uint32_t wait = (hr.retryAfter == 0 ? 60u : hr.retryAfter) + 2u;
            if (wait > kMaxRetryWait || attempt + 1 == kMaxAttempts) {
                blockedUntil_ = GetTickCount64() + static_cast<ULONGLONG>(wait) * 1000u;
                SYSTEMTIME t{};
                GetLocalTime(&t);
                const unsigned mins = t.wHour * 60u + t.wMinute + (wait + 59u) / 60u;
                std::array<wchar_t, 96> b{};
                swprintf_s(b.data(), b.size(), L"Open-Meteo request limit reached; updates resume after %02u:%02u",
                           (mins / 60u) % 24u, mins % 60u);
                blockedNote_ = b.data();
                break;
            }
            ArchiveResult note;
            note.key = job.key;
            note.retrying = true;
            note.error = L"rate limited by Open-Meteo; retrying in " + std::to_wstring(wait) + L" s";
            PushArchive(job.notify, std::move(note));
            if (!WaitOrStop(wait)) { return; }
        }
    }
    if (got) {
        r.status    = hr.status;
        r.elapsedMs = hr.elapsedMs;
        r.bytes     = hr.length;
        auto data = std::make_unique<Climate>();
        const std::string_view body(buf_.get(), hr.length);
        // Parse even on an HTTP error: Open-Meteo explains itself in the body.
        if (ParseArchive(body, *data, err) && hr.status == 200) {
            r.ok   = true;
            r.data = std::move(data);
        } else if (hr.status != 200 && err.empty()) {
            err = L"HTTP " + std::to_wstring(hr.status);
        } else if (hr.status != 200) {
            err = L"HTTP " + std::to_wstring(hr.status) + L": " + err;
        }
    }
    if (got && hr.status == 429 && !blockedNote_.empty()) { err = blockedNote_; }
    if (!err.empty()) { r.error = err; }
    {
        std::lock_guard<std::mutex> lock(mu_);
        inFlight_.clear();
    }
    PushArchive(job.notify, std::move(r));
}

void Fetcher::RunSearch(const Job& job, HttpClient& http) {
    SearchResult r;
    r.seq = job.seq;
    HttpResult hr;
    std::wstring err;
    if (http.Get(job.url, buf_.get(), kHttpBufSize, hr, err)) {
        const std::string_view body(buf_.get(), hr.length);
        r.ok = ParseGeocode(body, r.hits, r.count, err) && hr.status == 200;
        if (hr.status != 200 && err.empty()) { err = L"HTTP " + std::to_wstring(hr.status); }
    }
    r.error = err;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (searches_.size() >= 8) { searches_.pop_front(); }
        searches_.push_back(std::move(r));
    }
    // The dialog may have closed meanwhile; then the result just waits.
    PostMessageW(job.notify, WM_APP_SEARCH, 0, 0);
}

} // namespace wt
