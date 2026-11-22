// Minimal blocking HTTPS GET/POST on top of WinHTTP. Copied from StockTool
// (src/http.*), which stays the upstream. Differences here: UrlEncode
// encodes UTF-8, only gzip is accepted (see Request), and Retry-After is read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wt {

struct HttpResult {
    uint32_t status    = 0;   // HTTP status code
    size_t   length    = 0;   // bytes written to the caller's buffer
    uint32_t elapsedMs = 0;   // wall time of the whole request
    uint32_t retryAfter = 0;  // Retry-After header in seconds (0 = absent)
};

// One WinHTTP session, reused across requests. Not thread-safe: owned and
// used by the worker thread only.
class HttpClient {
public:
    HttpClient() = default;
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Fetches `url` into `buf` (capacity `cap`). Returns false on transport
    // failure or if the body does not fit; `err` describes why. Any HTTP
    // status counts as success here; the caller inspects `out.status`.
    bool Get(const std::wstring& url, char* buf, size_t cap, HttpResult& out, std::wstring& err);

    // POSTs `body` (UTF-8) with the given Content-Type and optional extra
    // headers ("Name: value\r\n" lines). Same contract as Get otherwise.
    bool Post(const std::wstring& url, const std::wstring& contentType, const std::wstring& extraHeaders,
              const std::string& body, char* buf, size_t cap, HttpResult& out, std::wstring& err);

    // Sends exactly "WeatherTool/<version> (+repository URL)" as the
    // User-Agent instead of the browser-like default. OpenStreetMap's tile
    // policy requires a distinct, honest one and blocks browser imitations.
    void UseAppIdentity() { appIdentity_ = true; }

private:
    bool appIdentity_ = false;
    bool Request(const wchar_t* method, const std::wstring& url, const std::wstring& extraHeaders,
                 const std::string& body, char* buf, size_t cap, HttpResult& out, std::wstring& err);
    bool EnsureSession(std::wstring& err);
    void* session_ = nullptr;   // HINTERNET
};

// Percent-encodes the UTF-8 form of `s`, keeping unreserved characters
// (place names such as "Montréal" must reach the search intact).
std::wstring UrlEncode(const std::wstring& s);

} // namespace wt
