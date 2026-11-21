#include "webserver.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <array>
#include <cassert>
#include <memory>
#include <system_error>

namespace wt {
namespace {

constexpr size_t   kMaxRequest   = 8192;   // request line + headers; anything longer is refused
constexpr DWORD    kRecvTimeoutMs = 5000;  // a silent client cannot hold the server for longer
constexpr long     kPollMs       = 250;    // how often the accept loop checks for Stop()

std::string Response(int code, const char* reason, const char* type, std::string_view body, bool head,
                     const char* extra = "") {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    r += "Content-Type: ";
    r += type;
    r += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Connection: close\r\nX-Content-Type-Options: nosniff\r\nServer: WeatherTool\r\n";
    r += extra;
    r += "\r\n";
    if (!head) { r.append(body.data(), body.size()); }
    return r;
}

std::wstring WsaText(const wchar_t* stage, int code) {
    if (code == WSAEADDRINUSE || code == WSAEACCES) {
        return L"the port is already in use by another program";
    }
    return std::wstring(stage) + L" failed (Winsock error " + std::to_wstring(code) + L")";
}

} // namespace

std::string HttpReply(std::string_view req, const WebContent& c, int& status) {
    const size_t eol = req.find("\r\n");
    const std::string_view line = req.substr(0, eol == std::string_view::npos ? req.size() : eol);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || line.substr(sp2 + 1, 5) != "HTTP/") {
        status = 400;
        return Response(400, "Bad Request", "text/plain; charset=utf-8", "Bad request\n", false);
    }
    const std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find_first_of("?#");
    if (q != std::string_view::npos) { target = target.substr(0, q); }
    const bool head = method == "HEAD";
    if (method != "GET" && !head) {
        status = 405;
        return Response(405, "Method Not Allowed", "text/plain; charset=utf-8", "Only GET and HEAD\n", false,
                        "Allow: GET, HEAD\r\n");
    }
    if (target == "/" || target == "/index.html") {
        status = 200;
        return Response(200, "OK", "text/html; charset=utf-8", c.html, head, "Cache-Control: no-cache\r\n");
    }
    if (target == "/favicon.ico" && !c.icon.empty()) {
        status = 200;
        return Response(200, "OK", "image/x-icon", c.icon, head, "Cache-Control: max-age=86400\r\n");
    }
    status = 404;
    return Response(404, "Not Found", "text/plain; charset=utf-8", "Not found\n", head);
}

WebServer::~WebServer() { Stop(); }

bool WebServer::Start(uint16_t port, const WebContent& content, std::wstring& err) {
    assert(!running_.load() && !worker_.joinable());
    assert(!content.html.empty());
    if (!wsa_) {
        WSADATA wd{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wd);
        if (rc != 0) {
            err = WsaText(L"WSAStartup", rc);
            return false;
        }
        wsa_ = true;
    }
    content_ = content;

    // Dual stack: one IPv6 socket that also accepts IPv4; plain IPv4 if IPv6 is off.
    SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool v6 = s != INVALID_SOCKET;
    if (!v6) { s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }
    if (s == INVALID_SOCKET) {
        err = WsaText(L"socket", WSAGetLastError());
        return false;
    }
    // Exclusive: nobody else can bind the same port and take the connections.
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    int bound = SOCKET_ERROR;
    if (v6) {
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr   = in6addr_any;
        a.sin6_port   = htons(port);
        bound = bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    } else {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons(port);
        bound = bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    }
    if (bound == SOCKET_ERROR || listen(s, 8) == SOCKET_ERROR) {
        err = WsaText(L"bind", WSAGetLastError());
        closesocket(s);
        return false;
    }
    sockaddr_storage got{};
    int len = sizeof(got);
    getsockname(s, reinterpret_cast<sockaddr*>(&got), &len);
    port_ = ntohs(got.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&got)->sin6_port
                                            : reinterpret_cast<sockaddr_in*>(&got)->sin_port);
    listen_ = static_cast<uintptr_t>(s);
    stop_ = false;
    requests_ = 0;
    try {
        worker_ = std::thread([this] { Run(); });
    } catch (const std::system_error&) {
        closesocket(s);
        listen_ = static_cast<uintptr_t>(INVALID_SOCKET);
        err = L"could not start the server thread";
        return false;
    }
    running_ = true;
    return true;
}

void WebServer::Stop() {
    stop_ = true;
    if (worker_.joinable()) { worker_.join(); }
    if (listen_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(listen_));
        listen_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
    running_ = false;
    if (wsa_) {
        WSACleanup();
        wsa_ = false;
    }
}

void WebServer::Run() {
    const SOCKET ls = static_cast<SOCKET>(listen_);
    while (!stop_.load()) {   // the service loop: ends on Stop()
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        timeval tv{ 0, kPollMs * 1000 };
        const int n = select(0, &rd, nullptr, nullptr, &tv);
        if (n <= 0) { continue; }
        const SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) { continue; }
        Serve(static_cast<uintptr_t>(c));
    }
}

void WebServer::Serve(uintptr_t client) {
    const SOCKET c = static_cast<SOCKET>(client);
    DWORD timeout = kRecvTimeoutMs;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::array<char, kMaxRequest> buf{};
    size_t have = 0;
    bool complete = false;
    for (int i = 0; i < 64 && have < buf.size() && !complete; ++i) {
        const int got = recv(c, buf.data() + have, static_cast<int>(buf.size() - have), 0);
        if (got <= 0) { break; }
        have += static_cast<size_t>(got);
        complete = std::string_view(buf.data(), have).find("\r\n\r\n") != std::string_view::npos;
    }
    if (have > 0) {
        int status = 0;
        const std::string reply = HttpReply(std::string_view(buf.data(), have), content_, status);
        size_t sent = 0;
        for (int i = 0; i < 4096 && sent < reply.size(); ++i) {
            const int n = send(c, reply.data() + sent, static_cast<int>(reply.size() - sent), 0);
            if (n <= 0) { break; }
            sent += static_cast<size_t>(n);
        }
        ++requests_;
    }
    shutdown(c, SD_SEND);
    closesocket(c);
}

std::vector<std::wstring> LocalIPv4Addresses(size_t maxCount) {
    std::vector<std::wstring> withGateway;
    std::vector<std::wstring> others;
    ULONG size = 32 * 1024;
    auto buf = std::make_unique<unsigned char[]>(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (rc == ERROR_BUFFER_OVERFLOW && size <= 1024u * 1024u) {   // `size` now holds what it needs
        buf = std::make_unique<unsigned char[]>(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (rc != NO_ERROR) { return {}; }
    for (auto* a = list; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) { continue; }
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const unsigned char first = sin->sin_addr.S_un.S_un_b.s_b1;
            const unsigned char second = sin->sin_addr.S_un.S_un_b.s_b2;
            if (first == 127 || (first == 169 && second == 254)) { continue; }
            std::array<wchar_t, 64> text{};
            if (InetNtopW(AF_INET, &sin->sin_addr, text.data(), text.size()) == nullptr) { continue; }
            (a->FirstGatewayAddress != nullptr ? withGateway : others).push_back(text.data());
        }
    }
    // Adapters without a gateway are usually virtual-machine host networks no
    // other device can reach; list them only when nothing better exists.
    std::vector<std::wstring>& best = withGateway.empty() ? others : withGateway;
    if (best.size() > maxCount) { best.resize(maxCount); }
    return best;
}

} // namespace wt
