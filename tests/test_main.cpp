// Unit tests for the UI-free core. No framework: each CHECK prints the
// failing expression and the exe exits non-zero when any failed.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "climate.h"
#include "config.h"
#include "http.h"
#include "jsonscan.h"
#include "units.h"
#include "webserver.h"

#include <cstring>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failed = 0;
int g_checks = 0;

#define CHECK(expr)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(expr)) {                                                                \
            ++g_failed;                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);               \
        }                                                                             \
    } while (0)

bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

using namespace wt;

int DaysIn(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m] + ((m == 1 && y % 4 == 0) ? 1 : 0);
}

// Daily archive reply for [y0, y1]: mean = month number, high = mean + 5,
// low = mean - 5, snow 1 cm a day in January only. `nullDays` days of
// March 2000's mean are null.
std::string MakeArchive(int y0, int y1, int nullDays, int stopAfterDayOfLastMonth = 0) {
    std::string t, mean, hi, lo, snow;
    for (int y = y0; y <= y1; ++y) {
        for (int m = 0; m < 12; ++m) {
            int days = DaysIn(y, m);
            if (y == y1 && m == 11 && stopAfterDayOfLastMonth > 0) { days = stopAfterDayOfLastMonth; }
            for (int d = 1; d <= days; ++d) {
                char b[32];
                std::snprintf(b, sizeof(b), "\"%04d-%02d-%02d\"", y, m + 1, d);
                const char* sep = t.empty() ? "" : ",";
                t += sep; t += b;
                const bool isNull = (y == 2000 && m == 2 && d <= nullDays);
                std::snprintf(b, sizeof(b), "%d", m + 1);
                mean += sep; mean += isNull ? "null" : b;
                std::snprintf(b, sizeof(b), "%d", m + 6);
                hi += sep; hi += b;
                std::snprintf(b, sizeof(b), "%d", m - 4);
                lo += sep; lo += b;
                snow += sep; snow += (m == 0) ? "1.0" : "0.0";
            }
        }
    }
    return std::string("{\"latitude\":45.4,\"longitude\":-75.7,\"elevation\":75.0,\"timezone\":\"America/Toronto\",") +
           "\"daily_units\":{\"time\":\"iso8601\",\"temperature_2m_mean\":\"\xC2\xB0" "C\"}," +
           "\"daily\":{\"time\":[" + t + "],\"temperature_2m_mean\":[" + mean + "],\"temperature_2m_max\":[" + hi +
           "],\"temperature_2m_min\":[" + lo + "],\"snowfall_sum\":[" + snow + "]}}";
}

void TestJsonScan() {
    const std::string j = R"({"a":{"name":"Montréal","n":-1.5e1,"x":null},"list":[1, 2 ,3],"b":true})";
    size_t p = json::FindKey(j, "name", 0, j.size());
    std::wstring s;
    CHECK(p != json::npos && json::ReadString(j, p, s) && s == L"Montréal");
    double v = 0;
    p = json::FindKey(j, "n", 0, j.size());
    CHECK(p != json::npos && json::ReadNumber(j, p, v) && v == -15.0);
    p = json::FindKey(j, "x", 0, j.size());
    CHECK(p != json::npos && json::ReadNumber(j, p, v) && std::isnan(v));
    json::ArrayReader r(j, json::FindKey(j, "list", 0, j.size()));
    double sum = 0;
    int n = 0;
    while (r.NextNumber(v)) { sum += v; ++n; }
    CHECK(r.ok() && n == 3 && sum == 6.0);
    // A key that only appears as a string value is not a key.
    const std::string k = R"({"v":"time","time":[1]})";
    p = json::FindKey(k, "time", 0, k.size());
    CHECK(p != json::npos && k[p] == '[');
    // Raw UTF-8 decodes too.
    const std::string u = "{\"n\":\"Z\xC3\xBCrich\"}";
    p = json::FindKey(u, "n", 0, u.size());
    CHECK(json::ReadString(u, p, s) && s == L"Zürich");
    // Malformed array is reported.
    const std::string bad = R"({"a":[1,2 3]})";
    json::ArrayReader rb(bad, json::FindKey(bad, "a", 0, bad.size()));
    while (rb.NextNumber(v)) {}
    CHECK(!rb.ok());
}

void TestParseArchive() {
    Climate c;
    std::wstring err;
    CHECK(ParseArchive(MakeArchive(2000, 2001, 1), c, err));
    CHECK(c.valid && c.firstYear == 2000 && c.yearCount == 2);
    CHECK(Near(c.lat, 45.4) && Near(c.elevation, 75.0) && c.timezone == L"America/Toronto");
    CHECK(Near(Cell(c, 2000, 0, Var::Mean), 1.0));
    CHECK(Near(Cell(c, 2001, 11, Var::High), 17.0));
    CHECK(Near(Cell(c, 2000, 0, Var::Snow), 31.0));          // a sum, not a mean
    CHECK(Near(Cell(c, 2000, 1, Var::Snow), 0.0));
    CHECK(Near(Cell(c, 2000, 2, Var::Mean), 3.0));           // one missing day is tolerated

    Climate c2;
    CHECK(ParseArchive(MakeArchive(2000, 2000, 2), c2, err));
    CHECK(std::isnan(Cell(c2, 2000, 2, Var::Mean)));         // two missing days are not
    CHECK(!std::isnan(Cell(c2, 2000, 2, Var::High)));

    // An unfinished last month is left out.
    Climate c3;
    CHECK(ParseArchive(MakeArchive(2000, 2000, 0, 10), c3, err));
    CHECK(std::isnan(Cell(c3, 2000, 11, Var::Mean)) && !std::isnan(Cell(c3, 2000, 10, Var::Mean)));
    CHECK(LastCompleteMonth(c3) == 2000 * 12 + 10);

    // Provider error objects surface their reason.
    Climate e;
    CHECK(!ParseArchive(R"({"error":true,"reason":"Daily API request limit exceeded"})", e, err));
    CHECK(err == L"Daily API request limit exceeded");
}

void TestMergeAndCache() {
    std::wstring err;
    Climate base;
    CHECK(ParseArchive(MakeArchive(2000, 2000, 0, 10), base, err));
    Climate upd;
    CHECK(ParseArchive(MakeArchive(2000, 2001, 0), upd, err));
    Cell(upd, 2000, 0, Var::Mean) = 99.0;
    CHECK(MergeClimate(base, upd, err));
    CHECK(base.firstYear == 2000 && base.yearCount == 2);
    CHECK(Near(Cell(base, 2000, 11, Var::Mean), 12.0));      // filled in
    CHECK(Near(Cell(base, 2000, 0, Var::Mean), 99.0));       // replaced
    CHECK(Near(Cell(base, 2001, 5, Var::Low), 1.0));

    // Merging an earlier span re-bases the grid.
    Climate early;
    CHECK(ParseArchive(MakeArchive(1998, 1998, 0), early, err));
    CHECK(MergeClimate(base, early, err));
    CHECK(base.firstYear == 1998 && base.yearCount == 4);
    CHECK(Near(Cell(base, 2000, 0, Var::Mean), 99.0) && Near(Cell(base, 1998, 3, Var::Mean), 4.0));
    CHECK(std::isnan(Cell(base, 1999, 3, Var::Mean)));

    base.fetched = 1700000000;
    const std::string csv = ClimateToCsv(base);
    Climate back;
    CHECK(ClimateFromCsv(csv, back, err));
    CHECK(back.firstYear == 1998 && back.yearCount == 4 && back.fetched == 1700000000);
    CHECK(Near(Cell(back, 2000, 0, Var::Mean), 99.0) && Near(Cell(back, 2001, 0, Var::Snow), 31.0));
    CHECK(std::isnan(Cell(back, 1999, 3, Var::Mean)));
    CHECK(!ClimateFromCsv("year,month\n2000,1,1,2,3,4\n", back, err));   // no signature line
}

void TestSeries() {
    std::wstring err;
    Climate c;
    CHECK(ParseArchive(MakeArchive(2000, 2002, 0, 10), c, err));
    const YearSeries a = AnnualTemp(c, Var::Mean);
    CHECK(Near(a.v[0], 6.5) && Near(a.v[1], 6.5) && std::isnan(a.v[2]));   // 2002 lacks December
    CHECK(LastValidYear(a) == 2001);
    const YearSeries s = SeasonSnow(c);
    CHECK(Near(s.v[0], 31.0) && Near(s.v[1], 31.0) && std::isnan(s.v[2]));  // 2002-03 not over
    const YearSeries m = MonthSeries(c, Var::High, 6);
    CHECK(Near(m.v[2], 12.0));
    const auto norms = MonthNormals(c, Var::Mean, 2000, 2001);
    CHECK(Near(norms[0], 1.0) && Near(norms[11], 12.0));

    YearSeries line;
    line.firstYear = 1950;
    line.n = 30;
    for (size_t i = 0; i < line.n; ++i) { line.v[i] = 2.0 + 0.1 * static_cast<double>(i); }
    line.v[5] = kNaN;
    const Trend t = LinearTrend(line, 1950);
    CHECK(t.valid && Near(t.slope, 0.1, 1e-9) && t.n == 29);
    CHECK(Near(t.intercept + t.slope * 1960, 3.0, 1e-9));
    CHECK(!LinearTrend(line, 1975).valid);                    // fewer than 10 points
    const YearSeries r = Rolling(line, 10, 8);
    CHECK(std::isnan(r.v[8]) && !std::isnan(r.v[9]) && std::isnan(r.v[5]));
    CHECK(Near(r.v[29], (4.9 + 4.0) / 2.0, 1e-9));
    const YearSeries tl = TrendLine(line, t, 1960);
    CHECK(std::isnan(tl.v[9]) && Near(tl.v[10], 3.0, 1e-9));
    int y0 = 0, y1 = 0;
    CHECK(LastValidSpan(line, 10, y0, y1) && y0 == 1970 && y1 == 1979);
    CHECK(Near(PeriodMean(line, 1950, 1951), 2.05, 1e-9));
}

void TestGeocode() {
    const std::string j =
        R"({"results":[{"id":1,"name":"Ottawa","latitude":45.41117,"longitude":-75.69812,"elevation":71.0,)"
        R"("country_code":"CA","admin1":"Ontario","postcodes":["K1A"]},)"
        R"({"id":2,"name":"Ottawa","latitude":41.34559,"longitude":-88.84258,"country_code":"US","admin1":"Illinois"}],)"
        R"("generationtime_ms":0.5})";
    std::array<GeoHit, kMaxHits> hits{};
    size_t n = 0;
    std::wstring err;
    CHECK(ParseGeocode(j, hits, n, err) && n == 2);
    CHECK(hits[0].admin1 == L"Ontario" && hits[0].country == L"CA" && Near(hits[0].lon, -75.69812));
    CHECK(hits[1].admin1 == L"Illinois" && std::isnan(hits[1].elevation));
    CHECK(ParseGeocode(R"({"generationtime_ms":0.3})", hits, n, err) && n == 0);
}

void TestUnits() {
    CHECK(FmtTemp(0.0, Units::Imperial) == L"32.0 °F");
    CHECK(FmtDelta(1.2, Units::Metric) == L"+1.20 °C");
    CHECK(FmtDelta(-0.001, Units::Metric) == L"±0.00 °C");
    CHECK(FmtSnow(254.0, Units::Imperial) == L"100.0 in");
    CHECK(FmtSnow(kNaN, Units::Metric) == L"-");
    CHECK(SeasonLabel(1999) == L"1999–00");
    CHECK(FmtTemp(-1.84, Units::Metric) == L"−1.8 °C");   // a real minus sign
    CHECK(FmtTemp(-0.04, Units::Metric) == L"0.0 °C");         // no "-0.0"
}

void TestConfig() {
    Location loc;
    CHECK(ParseLocation(L"Ottawa, Ontario, CA|45.41117|-75.69812", loc));
    CHECK(loc.name == L"Ottawa, Ontario, CA" && Near(loc.lat, 45.41117));
    CHECK(!ParseLocation(L"Nowhere|95|0", loc));
    CHECK(!ParseLocation(L"|1|2", loc));
    CHECK(!ParseLocation(L"x|abc|2", loc));

    // Round trip through a scratch folder (never the operator's ini).
    wchar_t tmp[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH, tmp) > 0);
    const std::wstring dir = std::wstring(tmp) + L"weathertool_test_" + std::to_wstring(GetCurrentProcessId());
    CHECK(CreateDirectoryW(dir.c_str(), nullptr) != 0);
    CHECK(CreateDirectoryW((dir + L"\\cache").c_str(), nullptr) != 0);
    Config cfg;
    std::wstring err;
    CHECK(LoadConfig(dir, cfg, err) && cfg.locCount == 0 && cfg.units == Units::Metric);
    cfg.locs[0] = { L"Montréal, Quebec, CA", 45.50884, -73.58781 };
    cfg.locs[1] = { L"Ottawa, Ontario, CA", 45.41117, -75.69812 };
    cfg.locCount = 2;
    cfg.units = Units::Imperial;
    cfg.theme = ThemeMode::Dark;
    cfg.state.view = 2;
    cfg.state.selected = 1;
    CHECK(SaveLocations(cfg, err) && SaveSettings(cfg, err) && SaveState(cfg, err));
    Config back;
    CHECK(LoadConfig(dir, back, err));
    CHECK(back.locCount == 2 && back.locs[0].name == L"Montréal, Quebec, CA");
    CHECK(back.units == Units::Imperial && back.theme == ThemeMode::Dark);
    CHECK(back.state.view == 2 && back.state.selected == 1);
    CHECK(!back.serverEnabled && back.serverPort == 8765);   // defaults
    back.serverEnabled = true;
    back.serverPort = 9001;
    CHECK(SaveServer(back, err));
    Config srv;
    CHECK(LoadConfig(dir, srv, err) && srv.serverEnabled && srv.serverPort == 9001);
    CHECK(CacheFileFor(back, back.locs[1]) == dir + L"\\cache\\45.4112_-75.6981.csv");

    std::string bytes;
    CHECK(WriteFileBytes(dir + L"\\x.bin", "abc", err) && ReadFileBytes(dir + L"\\x.bin", bytes, 16, err) && bytes == "abc");
    CHECK(!ReadFileBytes(dir + L"\\x.bin", bytes, 2, err));   // over the size cap
    DeleteFileW((dir + L"\\x.bin").c_str());
    DeleteFileW(cfg.path.c_str());
    RemoveDirectoryW((dir + L"\\cache").c_str());
    RemoveDirectoryW(dir.c_str());
}

void TestHttpReply() {
    const WebContent c{ "<html>hi</html>", "ICO" };
    int st = 0;
    std::string r = HttpReply("GET / HTTP/1.1\r\nHost: x\r\n\r\n", c, st);
    CHECK(st == 200 && r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(r.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
    CHECK(r.find("Content-Length: 15\r\n") != std::string::npos && r.size() > 15 && r.substr(r.size() - 15) == "<html>hi</html>");
    r = HttpReply("GET /index.html?x=1 HTTP/1.1\r\n\r\n", c, st);
    CHECK(st == 200);
    r = HttpReply("HEAD / HTTP/1.1\r\n\r\n", c, st);   // headers only
    CHECK(st == 200 && r.find("<html>") == std::string::npos && r.find("Content-Length: 15") != std::string::npos);
    r = HttpReply("GET /favicon.ico HTTP/1.1\r\n\r\n", c, st);
    CHECK(st == 200 && r.find("image/x-icon") != std::string::npos);
    CHECK((HttpReply("GET /favicon.ico HTTP/1.1\r\n\r\n", WebContent{ "x", "" }, st), st == 404));
    // Nothing else is reachable: no files, no traversal.
    CHECK((HttpReply("GET /../WeatherTool.ini HTTP/1.1\r\n\r\n", c, st), st == 404));
    CHECK((HttpReply("GET /cache/ HTTP/1.1\r\n\r\n", c, st), st == 404));
    CHECK((HttpReply("POST / HTTP/1.1\r\n\r\n", c, st), st == 405));
    CHECK((HttpReply("garbage", c, st), st == 400));
    CHECK((HttpReply("GET /\r\n\r\n", c, st), st == 400));
}

// A real server on a free loopback port: request, response, refusal of a
// second listener on the same port, and a clean stop.
std::string Fetch(uint16_t port, const char* request) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    InetPtonW(AF_INET, L"127.0.0.1", &a.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0) {
        send(s, request, static_cast<int>(std::strlen(request)), 0);
        char buf[4096];
        for (int i = 0; i < 100; ++i) {
            const int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) { break; }
            out.append(buf, static_cast<size_t>(n));
        }
    }
    closesocket(s);
    return out;
}

void TestWebServer() {
    WSADATA wd{};
    CHECK(WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    const WebContent c{ "<html>served</html>", "" };
    WebServer srv;
    std::wstring err;
    CHECK(srv.Start(0, c, err) && srv.Running() && srv.Port() != 0);
    const std::string page = Fetch(srv.Port(), "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(page.rfind("HTTP/1.1 200 OK", 0) == 0 && page.find("<html>served</html>") != std::string::npos);
    CHECK(Fetch(srv.Port(), "GET /nope HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
    CHECK(srv.Requests() == 2);
    WebServer second;   // the same port again must be refused, with a readable reason
    CHECK(!second.Start(srv.Port(), c, err) && err.find(L"in use") != std::wstring::npos);
    const uint16_t port = srv.Port();
    srv.Stop();
    CHECK(!srv.Running() && Fetch(port, "GET / HTTP/1.1\r\n\r\n").empty());
    WSACleanup();
}

// Optional, network-dependent: one real archive download through the app's
// own HTTP client and parser. Not part of ctest (`--live` to run it).
int LiveCheck(const char* startDate) {
    HttpClient http;
    auto buf = std::make_unique<char[]>(kHttpBufSize);
    HttpResult hr;
    std::wstring err;
    const std::string start = startDate != nullptr ? startDate : "1940-01-01";
    const std::wstring url =
        L"https://archive-api.open-meteo.com/v1/archive?latitude=45.4112&longitude=-75.6981&start_date=" +
        std::wstring(start.begin(), start.end()) +
        L"&end_date=2026-11-01&daily=temperature_2m_mean,temperature_2m_max,temperature_2m_min,snowfall_sum&timezone=auto";
    if (!http.Get(url, buf.get(), kHttpBufSize, hr, err)) {
        std::printf("live: transport failed after %u ms: %ls\n", hr.elapsedMs, err.c_str());
        return 1;
    }
    std::printf("live: HTTP %u, %zu bytes in %u ms\n", hr.status, hr.length, hr.elapsedMs);
    Climate c;
    if (!ParseArchive(std::string_view(buf.get(), hr.length), c, err)) {
        std::printf("live: parse failed: %ls\n", err.c_str());
        return 1;
    }
    const YearSeries a = AnnualTemp(c, Var::Mean);
    const int last = LastValidYear(a);
    const double lastMean = last >= c.firstYear ? a.v[static_cast<size_t>(last - c.firstYear)] : kNaN;
    const int lcm = LastCompleteMonth(c);
    std::printf("live: %d-%d, last complete year %d mean %.2f C, last complete month %d-%02d\n", c.firstYear,
                c.firstYear + static_cast<int>(c.yearCount) - 1, last, lastMean, lcm / 12, lcm % 12 + 1);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--live") { return LiveCheck(argc > 2 ? argv[2] : nullptr); }
    TestJsonScan();
    TestParseArchive();
    TestMergeAndCache();
    TestSeries();
    TestGeocode();
    TestUnits();
    TestConfig();
    TestHttpReply();
    TestWebServer();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
