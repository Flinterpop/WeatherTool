// A tiny HTTP server for the browser version of WeatherTool (web/index.html,
// compiled into the exe). It serves that page and the icon, nothing else:
// no files from disk, no uploads, no data of the app's own. One worker
// thread, one connection at a time, Connection: close. The page itself
// fetches its data from Open-Meteo, in the browser that opens it.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wt {

// What the server hands out. The views must outlive the server
// (they point into the exe's resources).
struct WebContent {
    std::string_view html;
    std::string_view icon;   // favicon.ico; empty = 404
};

// The complete response to one raw request (request line and headers).
// Pure, so it is unit-tested without a socket. `status` gets the code.
std::string HttpReply(std::string_view request, const WebContent& content, int& status);

class WebServer {
public:
    WebServer() = default;
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Listens on every interface (IPv6 and IPv4) at `port`; 0 picks a free
    // one (tests). False with a readable reason, e.g. the port is in use.
    bool Start(uint16_t port, const WebContent& content, std::wstring& err);
    void Stop();   // waits for the worker; idempotent

    bool     Running() const { return running_.load(); }
    uint16_t Port() const { return port_; }
    uint64_t Requests() const { return requests_.load(); }

private:
    void Run();
    void Serve(uintptr_t client);

    uintptr_t             listen_ = static_cast<uintptr_t>(~0ull);   // SOCKET
    std::thread           worker_;
    std::atomic<bool>     stop_{ false };
    std::atomic<bool>     running_{ false };
    std::atomic<uint64_t> requests_{ 0 };
    uint16_t              port_ = 0;
    WebContent            content_;
    bool                  wsa_ = false;
};

// This machine's IPv4 addresses that other devices can reach: adapters that
// are up, not loopback and not link-local, and have a default gateway (or,
// when none has, all the others).
std::vector<std::wstring> LocalIPv4Addresses(size_t maxCount);

} // namespace wt
