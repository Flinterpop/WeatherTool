#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cassert>
#include <cwchar>

namespace wt {
namespace {

// Widen the CMake-supplied version string for the User-Agent.
#define WT_WIDE_(s) L##s
#define WT_WIDE(s)  WT_WIDE_(s)
constexpr wchar_t kAgent[] = L"WeatherTool/" WT_WIDE(WEATHERTOOL_VERSION);

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h != nullptr) { WinHttpCloseHandle(h); } }
};

std::wstring ErrorText(const wchar_t* stage, DWORD code) {
    assert(stage != nullptr);
    std::array<wchar_t, 256> msg{};
    const HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             mod, code, 0, msg.data(), static_cast<DWORD>(msg.size()), nullptr);
    if (n == 0) {
        swprintf_s(msg.data(), msg.size(), L"error %lu", code);
        n = static_cast<DWORD>(wcslen(msg.data()));
    }
    // Strip the trailing CR/LF FormatMessage appends.
    while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n')) { msg[--n] = L'\0'; }
    return std::wstring(stage) + L": " + msg.data();
}

bool ReadBody(HINTERNET req, char* buf, size_t cap, size_t& len, std::wstring& err) {
    assert(req != nullptr);
    assert(buf != nullptr);
    len = 0;
    constexpr int kMaxChunks = 4096;
    for (int i = 0; i < kMaxChunks; ++i) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            err = ErrorText(L"WinHttpQueryDataAvailable", GetLastError());
            return false;
        }
        if (avail == 0) { return true; }
        if (len + avail > cap) {
            err = L"Response exceeds receive buffer";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + len, avail, &got)) {
            err = ErrorText(L"WinHttpReadData", GetLastError());
            return false;
        }
        if (got == 0) { return true; }
        len += got;
        assert(len <= cap);
    }
    err = L"Response too fragmented";
    return false;
}

} // namespace

HttpClient::~HttpClient() {
    if (session_ != nullptr) { WinHttpCloseHandle(static_cast<HINTERNET>(session_)); }
}

bool HttpClient::EnsureSession(std::wstring& err) {
    if (session_ != nullptr) { return true; }
    HINTERNET s = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s == nullptr) {
        err = ErrorText(L"WinHttpOpen", GetLastError());
        return false;
    }
    // resolve, connect, send, receive (ms)
    if (!WinHttpSetTimeouts(s, 10000, 10000, 10000, 15000)) {
        err = ErrorText(L"WinHttpSetTimeouts", GetLastError());
        WinHttpCloseHandle(s);
        return false;
    }
    session_ = s;
    return true;
}

bool HttpClient::Get(const std::wstring& url, char* buf, size_t cap,
                     HttpResult& out, std::wstring& err) {
    return Request(L"GET", url, L"", std::string(), buf, cap, out, err);
}

bool HttpClient::Post(const std::wstring& url, const std::wstring& contentType, const std::wstring& extraHeaders,
                      const std::string& body, char* buf, size_t cap, HttpResult& out, std::wstring& err) {
    assert(!contentType.empty());
    if (body.size() > 65536) {
        err = L"POST body too large";
        return false;
    }
    return Request(L"POST", url, L"Content-Type: " + contentType + L"\r\n" + extraHeaders, body, buf, cap, out, err);
}

bool HttpClient::Request(const wchar_t* method, const std::wstring& url, const std::wstring& extraHeaders,
                         const std::string& body, char* buf, size_t cap, HttpResult& out, std::wstring& err) {
    assert(method != nullptr);
    assert(buf != nullptr);
    assert(cap > 0);
    out = HttpResult{};
    if (url.empty() || url.size() > 4096) {
        err = L"Bad URL";
        return false;
    }
    if (!EnsureSession(err)) { return false; }
    const ULONGLONG started = GetTickCount64();
    // Records the latency whichever way the function returns.
    struct Timer {
        ULONGLONG start;
        HttpResult& res;
        ~Timer() { res.elapsedMs = static_cast<uint32_t>(GetTickCount64() - start); }
    } timer{ started, out };

    std::array<wchar_t, 256>  host{};
    std::array<wchar_t, 2048> path{};
    std::array<wchar_t, 2048> extra{};
    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host.data();
    uc.dwHostNameLength  = static_cast<DWORD>(host.size());
    uc.lpszUrlPath       = path.data();
    uc.dwUrlPathLength   = static_cast<DWORD>(path.size());
    uc.lpszExtraInfo     = extra.data();
    uc.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        err = ErrorText(L"WinHttpCrackUrl", GetLastError());
        return false;
    }
    const std::wstring target = std::wstring(path.data()) + extra.data();

    Handle conn;
    conn.h = WinHttpConnect(static_cast<HINTERNET>(session_), host.data(), uc.nPort, 0);
    if (conn.h == nullptr) {
        err = ErrorText(L"WinHttpConnect", GetLastError());
        return false;
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    Handle req;
    req.h = WinHttpOpenRequest(conn.h, method, target.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (req.h == nullptr) {
        err = ErrorText(L"WinHttpOpenRequest", GetLastError());
        return false;
    }

    // Transparent gzip only (best effort, unsupported on very old builds).
    // Not deflate, unlike StockTool: offered deflate, Open-Meteo sends it and
    // WinHTTP aborts the read part-way (WinHttpQueryDataAvailable "Operation
    // aborted" on a ~1 MB archive reply, 21 Nov 2026).
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP;
    const BOOL decompOk = WinHttpSetOption(req.h, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
    (void)decompOk;

    const std::wstring headers =
        std::wstring(L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) ") + kAgent +
        L"\r\nAccept: application/json, text/plain, */*\r\n" + extraHeaders;
    if (!WinHttpAddRequestHeaders(req.h, headers.c_str(), static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        err = ErrorText(L"WinHttpAddRequestHeaders", GetLastError());
        return false;
    }
    // WinHTTP does not copy the body: `body` outlives the call.
    void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD bodyLen = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, bodyLen, bodyLen, 0)) {
        err = ErrorText(L"WinHttpSendRequest", GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        err = ErrorText(L"WinHttpReceiveResponse", GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD size   = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        err = ErrorText(L"WinHttpQueryHeaders", GetLastError());
        return false;
    }
    out.status = status;
    DWORD retry = 0;
    size = sizeof(retry);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &retry, &size, WINHTTP_NO_HEADER_INDEX)) {
        out.retryAfter = retry;   // absent header: stays 0
    }
    return ReadBody(req.h, buf, cap, out.length, err);
}

std::wstring UrlEncode(const std::wstring& s) {
    std::wstring out;
    if (s.empty() || s.size() > 1024) { return out; }
    std::array<char, 4096> utf8{};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, nullptr);
    if (n <= 0) { return out; }
    out.reserve(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[static_cast<size_t>(i)]);
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) {
            out += static_cast<wchar_t>(c);
        } else {
            std::array<wchar_t, 8> hex{};
            swprintf_s(hex.data(), hex.size(), L"%%%02X", static_cast<unsigned>(c));
            out += hex.data();
        }
    }
    return out;
}

} // namespace wt
