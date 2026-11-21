#include "app.h"

#include "resource.h"
#include "units.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ctime>

namespace wt {
namespace {

using namespace Gdiplus;

#define WT_WIDE_(s) L##s
#define WT_WIDE(s)  WT_WIDE_(s)
constexpr wchar_t kVersion[]   = WT_WIDE(WEATHERTOOL_VERSION);
constexpr wchar_t kClassName[] = L"WeatherToolMain";
constexpr wchar_t kAppName[]   = L"WeatherTool";
constexpr wchar_t kCredit[]    = L"Data: Open-Meteo.com, ERA5 reanalysis (CC BY 4.0)";
constexpr wchar_t kCreditNow[] = L"Data: Open-Meteo.com forecast, national weather models (CC BY 4.0)";

// Menu and control identifiers.
enum : int {
    IDM_EXPORT = 100, IDM_OPENDIR, IDM_EXIT, IDM_WEBSERVER, IDM_WEBOPEN,
    IDM_ADD = 110, IDM_REMOVE, IDM_MOVEUP, IDM_MOVEDOWN, IDM_UPDATE, IDM_REDOWNLOAD,
    IDM_VIEW0 = 120,                                  // + View
    IDM_METRIC = 130, IDM_IMPERIAL,
    IDM_THEME_SYSTEM = 140, IDM_THEME_LIGHT, IDM_THEME_DARK,
    IDM_ABOUT = 150,
    IDM_TAB0 = 160, IDM_TAB1, IDM_TABNEXT,             // + Tab
    IDC_LIST = 200, IDC_ADD, IDC_REMOVE, IDC_UPDATE, IDC_UNITS,
    IDC_VIEW0 = 210, IDC_RANGE0 = 220, IDC_MEASURE0 = 230, IDC_MONTH0 = 240, IDC_TAB0 = 250,
};

constexpr UINT_PTR kUpdateTimer   = 1;
constexpr UINT     kUpdateEveryMs = 30u * 60u * 1000u;
constexpr int64_t  kStaleSeconds  = 6 * 3600;
// Open-Meteo refreshes current conditions every 15 minutes; asking more
// often returns the same values. Forecasts are fetched only while the Now
// tab is showing.
constexpr UINT_PTR kForecastTimer   = 2;
constexpr UINT     kForecastEveryMs = 15u * 60u * 1000u;
constexpr int64_t  kForecastStale   = 14 * 60;
constexpr size_t   kMaxForecastBytes = 256u * 1024u;

// Layout in device-independent pixels.
constexpr int kMargin   = 10;
constexpr int kListW    = 270;
constexpr int kItemH    = 46;
constexpr int kButtonH  = 26;
constexpr int kGap      = 4;
constexpr int kViewBtnW = 78;
constexpr int kMonthW   = 36;
constexpr int kRangeW   = 64;
constexpr int kMeasureW = 70;
constexpr int kHeaderH  = 72;
constexpr int kStatusH  = 20;
constexpr int kMinW     = 1180;
constexpr int kMinH     = 720;
constexpr int kTabH     = 30;
constexpr int kTabW     = 96;

constexpr const wchar_t* kTabNames[kTabCount]   = { L"History", L"Now" };
constexpr const wchar_t* kViewNames[kViewCount] = { L"Trends", L"Month", L"Heatmap", L"Climate" };
constexpr const wchar_t* kDayShort[7] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
constexpr const wchar_t* kRangeNames[]          = { L"All years", L"50 years", L"30 years", L"20 years" };
constexpr const wchar_t* kMeasureNames[3]       = { L"Mean", L"Daily high", L"Daily low" };
constexpr const wchar_t* kMonthShort[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                             L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
constexpr const wchar_t* kMonthLong[12]  = { L"January", L"February", L"March", L"April", L"May", L"June",
                                             L"July", L"August", L"September", L"October", L"November", L"December" };
static_assert(sizeof(kRangeNames) / sizeof(kRangeNames[0]) == kRangeYears.size(), "range names and presets differ");

constexpr int kNormal0 = 1991, kNormal1 = 2020;   // WMO current normal
constexpr int kOld0 = 1961, kOld1 = 1990;         // WMO reference period

int64_t UnixNow() { return static_cast<int64_t>(_time64(nullptr)); }

std::wstring IsoDateDaysAgo(int days) {
    const __time64_t t = _time64(nullptr) - static_cast<__time64_t>(days) * 86400;
    tm g{};
    _gmtime64_s(&g, &t);
    std::array<wchar_t, 16> b{};
    swprintf_s(b.data(), b.size(), L"%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return b.data();
}

std::wstring LocalDate(int64_t t) {
    if (t <= 0) { return L"never"; }
    tm l{};
    const __time64_t tt = t;
    _localtime64_s(&l, &tt);
    std::array<wchar_t, 32> b{};
    wcsftime(b.data(), b.size(), L"%d %b %Y %H:%M", &l);
    return b.data();
}

std::wstring ClockNow() {   // "16:03", this PC's time
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::array<wchar_t, 8> b{};
    swprintf_s(b.data(), b.size(), L"%02u:%02u", static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute));
    return b.data();
}

int64_t FileUnixTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) { return 0; }
    ULARGE_INTEGER u{};
    u.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    constexpr ULONGLONG kEpoch = 116444736000000000ULL;   // 1970-01-01 in FILETIME units
    return u.QuadPart > kEpoch ? static_cast<int64_t>((u.QuadPart - kEpoch) / 10000000ULL) : 0;
}

std::wstring HourLabel(const LocalTime& t) {
    std::array<wchar_t, 8> b{};
    swprintf_s(b.data(), b.size(), L"%02d:%02d", t.hour, t.minute);
    return b.data();
}

std::wstring DayLabel(const LocalTime& t) {   // "Sat 22"
    return std::wstring(kDayShort[Weekday(t.year, t.month, t.day)]) + L" " + std::to_wstring(t.day);
}

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top),
                 static_cast<REAL>(r.right - r.left), static_cast<REAL>(r.bottom - r.top));
}

Font UiFont(float pt, float s, INT style = FontStyleRegular) {
    return Font(L"Segoe UI", FontPx(pt, s), style, UnitPixel);
}

// GDI+ does no font fallback, and Segoe UI lacks most weather symbols.
Font GlyphFont(float pt, float s) {
    return Font(L"Segoe UI Symbol", FontPx(pt, s), FontStyleRegular, UnitPixel);
}

void PaintText(Graphics& g, const std::wstring& t, const Font& f, const Color& c, float x, float y) {
    SolidBrush b(c);
    g.DrawString(t.c_str(), static_cast<INT>(t.size()), &f, PointF(x, y), &b);
}

float TextW(Graphics& g, const std::wstring& t, const Font& f) {
    RectF box;
    g.MeasureString(t.c_str(), static_cast<INT>(t.size()), &f, PointF(0, 0), &box);
    return box.Width;
}

// Text clipped to a width, with an ellipsis.
void DrawTextClip(Graphics& g, const std::wstring& t, const Font& f, const Color& c, const RectF& r) {
    SolidBrush b(c);
    StringFormat fmt;
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(t.c_str(), static_cast<INT>(t.size()), &f, r, &fmt, &b);
}

const wchar_t* MeasureTitle(int measure) {
    return measure == 1 ? L"average daily high" : measure == 2 ? L"average daily low" : L"mean temperature";
}

std::wstring PerDecade(const Trend& t, bool snow, Units u) {
    if (!t.valid) { return L"no trend (too few years)"; }
    const std::wstring v = snow ? FmtSnowDelta(t.slope * 10.0, u) : FmtDelta(t.slope * 10.0, u);
    return L"trend " + v + L" per decade";
}

// Series converted to display units and sliced to start at `from`.
struct Slice {
    int    firstYear = 0;
    size_t n = 0;
    std::array<double, kMaxYears> v{};
};

enum class Conv { Temp, Snow };

Slice MakeSlice(const YearSeries& s, int from, Conv conv, Units u) {
    Slice out;
    const int off = from - s.firstYear;
    const size_t start = off > 0 ? static_cast<size_t>(off) : 0;
    out.firstYear = s.firstYear + static_cast<int>(start);
    out.n = start < s.n ? s.n - start : 0;
    for (size_t i = 0; i < out.n; ++i) {
        const double v = s.v[start + i];
        out.v[i] = conv == Conv::Temp ? ToTemp(v, u) : ToSnow(v, u);
    }
    // Drop trailing years with no value at all (the current, unfinished one).
    while (out.n > 0 && std::isnan(out.v[out.n - 1])) { --out.n; }
    return out;
}

void SetCheck(HWND h, bool on) {
    if (h != nullptr) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
}

} // namespace

App::~App() {
    fetcher_.Stop();
    FreeBackBuffer();
    if (hUiFont_ != nullptr)    { DeleteObject(hUiFont_); }
    if (hBgBrush_ != nullptr)   { DeleteObject(hBgBrush_); }
    if (hListBrush_ != nullptr) { DeleteObject(hListBrush_); }
    if (hAccel_ != nullptr)     { DestroyAcceleratorTable(hAccel_); }
}

int App::Px(int dip) const { return static_cast<int>(std::lround(dip * scale_)); }

std::wstring App::KeyFor(const Location& loc) const { return CacheFileFor(cfg_, loc); }
std::wstring App::ForecastKeyFor(const Location& loc) const { return ForecastFileFor(cfg_, loc); }

bool App::HasData(size_t index) const {
    return index < cfg_.locCount && (*climates_)[index].valid && (*climates_)[index].yearCount > 0;
}

bool App::HasForecast(size_t index) const {
    return index < cfg_.locCount && (*forecasts_)[index].valid;
}

int App::FromYear(const Climate& c) const {
    const int years = kRangeYears[static_cast<size_t>(cfg_.state.range)];
    const int last = c.firstYear + static_cast<int>(c.yearCount) - 1;
    if (years <= 0) { return c.firstYear; }
    const int from = last - years + 1;
    return from > c.firstYear ? from : c.firstYear;
}

// ---------------------------------------------------------------------------
// Creation

bool App::Create(HINSTANCE hInst, int nCmdShow, std::wstring& err) {
    hInst_ = hInst;
    climates_ = std::make_unique<std::array<Climate, kMaxLocations>>();
    forecasts_ = std::make_unique<std::array<Forecast, kMaxLocations>>();
    std::wstring dir;
    if (!ResolveDataDir(dir, dataNote_, err)) { return false; }
    if (!LoadConfig(dir, cfg_, err)) { return false; }
    LoadCaches();
    if (!fetcher_.Start(err)) { return false; }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // Large icon (Alt+Tab, taskbar) and small one (title bar), each picked
    // from the .ico at the size the system asks for.
    wc.hIcon   = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    assert(wc.hIcon != nullptr && wc.hIconSm != nullptr);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
        err = L"RegisterClassEx failed";
        return false;
    }
    const std::wstring title = std::wstring(kAppName) + L" " + kVersion;
    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, nullptr, nullptr, hInst, this);
    if (hwnd_ == nullptr) {
        err = L"CreateWindowEx failed";
        return false;
    }
    if (cfg_.state.hasPlacement) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        wp.showCmd = cfg_.state.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        wp.rcNormalPosition = cfg_.state.placement;
        SetWindowPlacement(hwnd_, &wp);
        if (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINNOACTIVE) {
            ShowWindow(hwnd_, nCmdShow);
        }
    } else {
        ShowWindow(hwnd_, nCmdShow);
    }
    UpdateWindow(hwnd_);
    ScheduleUpdates(false);
    ScheduleForecasts(false);
    SetTimer(hwnd_, kUpdateTimer, kUpdateEveryMs, nullptr);
    SetTimer(hwnd_, kForecastTimer, kForecastEveryMs, nullptr);
    return true;
}

int App::Run() {
    MSG msg{};
    for (;;) {   // the message pump: ends on WM_QUIT
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) { break; }
        if (TranslateAcceleratorW(hwnd_, hAccel_, &msg) != 0) { continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = nullptr;
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app != nullptr ? app->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:      OnCreate(); return 0;
    case WM_CLOSE:       OnClose(); return 0;
    case WM_DESTROY:     PostQuitMessage(0); return 0;
    case WM_SIZE:        Layout(); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_ERASEBKGND:  return 1;
    case WM_PAINT:       OnPaint(); return 0;
    case WM_COMMAND:     OnCommand(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_MOUSEMOVE:   OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSELEAVE:  OnMouseLeave(); return 0;
    case WM_DPICHANGED:  OnDpiChanged(wp, lp); return 0;
    case WM_MEASUREITEM: OnMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp)); return TRUE;
    case WM_DRAWITEM:    OnDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_TIMER:
        if (wp == kUpdateTimer) { ScheduleUpdates(false); }
        if (wp == kForecastTimer) { ScheduleForecasts(false); }
        return 0;
    case WM_APP_ARCHIVE: OnArchiveReady(); return 0;
    case WM_APP_FORECAST: OnForecastReady(); return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = Px(kMinW);
        mm->ptMinTrackSize.y = Px(kMinH);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp != 0 && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 &&
            cfg_.theme == ThemeMode::System) {
            ApplyTheme();
        }
        return 0;
    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wp), theme_->listBgRef);
        return reinterpret_cast<LRESULT>(hListBrush_);
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(hBgBrush_);
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

void App::OnCreate() {
    theme_ = &ThemeFor(false);   // replaced by ApplyTheme below; controls may ask for colours first
    scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    CreateFonts();
    CreateControls();

    hMenu_ = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_EXPORT, L"&Export monthly data (CSV)...");
    AppendMenuW(file, MF_STRING, IDM_OPENDIR, L"Open &data folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    const std::wstring webItem = L"&Web server (port " + std::to_wstring(cfg_.serverPort) + L")";
    AppendMenuW(file, MF_STRING, IDM_WEBSERVER, webItem.c_str());
    AppendMenuW(file, MF_STRING, IDM_WEBOPEN, L"&Open web page in browser");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU place = CreatePopupMenu();
    AppendMenuW(place, MF_STRING, IDM_ADD, L"&Add...\tCtrl+N");
    AppendMenuW(place, MF_STRING, IDM_REMOVE, L"&Remove\tDel");
    AppendMenuW(place, MF_STRING, IDM_MOVEUP, L"Move &up\tCtrl+Up");
    AppendMenuW(place, MF_STRING, IDM_MOVEDOWN, L"Move &down\tCtrl+Down");
    AppendMenuW(place, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(place, MF_STRING, IDM_UPDATE, L"&Update all now\tF5");
    AppendMenuW(place, MF_STRING, IDM_REDOWNLOAD, L"Re-download &full history");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_TAB0, L"&History tab");
    AppendMenuW(view, MF_STRING, IDM_TAB1, L"&Now tab (current weather and forecast)");
    AppendMenuW(view, MF_STRING, IDM_TABNEXT, L"Switch ta&b\tCtrl+Tab");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    for (int v = 0; v < kViewCount; ++v) {
        const std::wstring t = std::wstring(L"&") + kViewNames[v] + L"\tCtrl+" + std::to_wstring(v + 1);
        AppendMenuW(view, MF_STRING, static_cast<UINT_PTR>(IDM_VIEW0 + v), t.c_str());
    }
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_METRIC, L"&Metric (°C, cm)");
    AppendMenuW(view, MF_STRING, IDM_IMPERIAL, L"&Imperial (°F, in)");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_THEME_SYSTEM, L"Theme: follow &system");
    AppendMenuW(view, MF_STRING, IDM_THEME_LIGHT, L"Theme: &light");
    AppendMenuW(view, MF_STRING, IDM_THEME_DARK, L"Theme: &dark");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About WeatherTool");
    AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(place), L"&Place");
    AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(hMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    SetMenu(hwnd_, hMenu_);

    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_ADD },
        { FVIRTKEY, VK_DELETE, IDM_REMOVE },
        { FVIRTKEY | FCONTROL, VK_UP, IDM_MOVEUP },
        { FVIRTKEY | FCONTROL, VK_DOWN, IDM_MOVEDOWN },
        { FVIRTKEY, VK_F5, IDM_UPDATE },
        { FVIRTKEY | FCONTROL, '1', IDM_VIEW0 + 0 },
        { FVIRTKEY | FCONTROL, '2', IDM_VIEW0 + 1 },
        { FVIRTKEY | FCONTROL, '3', IDM_VIEW0 + 2 },
        { FVIRTKEY | FCONTROL, '4', IDM_VIEW0 + 3 },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_TABNEXT },
    };
    hAccel_ = CreateAcceleratorTableW(acc, static_cast<int>(sizeof(acc) / sizeof(acc[0])));

    // The browser version, compiled in as resources (see WeatherTool.rc).
    auto resource = [this](int id) {
        HRSRC r = FindResourceW(hInst_, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL g = r != nullptr ? LoadResource(hInst_, r) : nullptr;
        const void* p = g != nullptr ? LockResource(g) : nullptr;
        return p != nullptr ? std::string_view(static_cast<const char*>(p), SizeofResource(hInst_, r)) : std::string_view();
    };
    webContent_.html = resource(IDR_WEBPAGE);
    webContent_.icon = resource(IDR_FAVICON);
    assert(!webContent_.html.empty());

    ApplyTheme();
    RebuildList();
    SyncControls();
    Layout();
    status_ = cfg_.locCount == 0 ? L"Add a place to begin (Place > Add, or the Add button)." : L"Ready.";
    if (!dataNote_.empty()) { status_ += L" (" + dataNote_ + L")"; }
    if (cfg_.serverEnabled) { (void)StartWebServer(false); }   // remembered from last time
}

// ---------------------------------------------------------------------------
// Web server (File > Web server)

std::wstring App::WebUrls() const {
    const std::wstring port = std::to_wstring(web_.Port());
    std::wstring s;
    for (const std::wstring& a : LocalIPv4Addresses(3)) {
        s += (s.empty() ? L"" : L"  or  ") + (L"http://" + a + L":" + port + L"/");
    }
    return s.empty() ? L"http://localhost:" + port + L"/" : s;
}

bool App::StartWebServer(bool interactive) {
    if (web_.Running()) { return true; }
    std::wstring err;
    if (webContent_.html.empty()) {
        err = L"the web page is missing from the exe";
    } else if (web_.Start(static_cast<uint16_t>(cfg_.serverPort), webContent_, err)) {
        SetStatus(L"Web server on " + WebUrls());
        SyncControls();
        return true;
    }
    const std::wstring msg = L"Could not start the web server on port " + std::to_wstring(cfg_.serverPort) + L": " + err +
                             L". The port can be changed with [server] port= in " + cfg_.path + L".";
    SetStatus(msg);
    if (interactive) { MessageBoxW(hwnd_, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING); }
    SyncControls();
    return false;
}

void App::StopWebServer() {
    if (!web_.Running()) { return; }
    const uint64_t served = web_.Requests();
    web_.Stop();
    SetStatus(L"Web server stopped (" + std::to_wstring(served) + (served == 1 ? L" request" : L" requests") + L" served).");
    SyncControls();
}

void App::OnToggleWebServer() {
    const bool turnOn = !web_.Running();
    if (turnOn && !StartWebServer(true)) { return; }   // stays off; the setting is not changed
    if (!turnOn) { StopWebServer(); }
    cfg_.serverEnabled = turnOn;
    std::wstring err;
    if (!SaveServer(cfg_, err)) { SetStatus(err); }
}

void App::CreateFonts() {
    if (hUiFont_ != nullptr) { DeleteObject(hUiFont_); }
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    const UINT dpi = GetDpiForWindow(hwnd_);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        hUiFont_ = CreateFontIndirectW(&ncm.lfMessageFont);
    } else {
        hUiFont_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
}

void App::CreateControls() {
    auto button = [this](const wchar_t* text, int id, bool toggle) {
        const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (toggle ? (BS_CHECKBOX | BS_PUSHLIKE) : BS_PUSHBUTTON);
        HWND h = CreateWindowExW(0, L"BUTTON", text, style, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE);
        return h;
    };
    hList_ = CreateWindowExW(0, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                                 LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hInst_, nullptr);
    // The tabs are drawn here (WM_DRAWITEM), so they follow the app's theme.
    for (int t = 0; t < kTabCount; ++t) {
        hTabs_[static_cast<size_t>(t)] = CreateWindowExW(0, L"BUTTON", kTabNames[t], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                                         0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB0 + t)),
                                                         hInst_, nullptr);
        assert(hTabs_[static_cast<size_t>(t)] != nullptr);
    }
    hAdd_    = button(L"Add...", IDC_ADD, false);
    hRemove_ = button(L"Remove", IDC_REMOVE, false);
    hUpdate_ = button(L"Update", IDC_UPDATE, false);
    hUnits_  = button(L"°F / in", IDC_UNITS, true);
    for (int v = 0; v < kViewCount; ++v) { hViews_[static_cast<size_t>(v)] = button(kViewNames[v], IDC_VIEW0 + v, true); }
    for (size_t r = 0; r < hRanges_.size(); ++r) { hRanges_[r] = button(kRangeNames[r], IDC_RANGE0 + static_cast<int>(r), true); }
    for (size_t m = 0; m < hMeasures_.size(); ++m) { hMeasures_[m] = button(kMeasureNames[m], IDC_MEASURE0 + static_cast<int>(m), true); }
    for (size_t m = 0; m < 12; ++m) { hMonths_[m] = button(kMonthShort[m], IDC_MONTH0 + static_cast<int>(m), true); }
}

void App::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int m = Px(kMargin);
    const int bh = Px(kButtonH);
    const int gap = Px(kGap);
    const int w = rc.right;
    const int h = rc.bottom;

    // Left column: list, then Add / Remove / Update.
    const int listR = m + Px(kListW);
    const int btnY = h - m - bh;
    MoveWindow(hList_, m, m, Px(kListW), btnY - gap - m, TRUE);
    const int bw = (Px(kListW) - 2 * gap) / 3;
    MoveWindow(hAdd_, m, btnY, bw, bh, TRUE);
    MoveWindow(hRemove_, m + bw + gap, btnY, bw, bh, TRUE);
    MoveWindow(hUpdate_, listR - bw, btnY, bw, bh, TRUE);

    // Tab row: History / Now on the left, units at the right, a rule under it.
    const int x0 = listR + Px(16);
    const int r0 = w - m;
    for (size_t t = 0; t < hTabs_.size(); ++t) {
        MoveWindow(hTabs_[t], x0 + static_cast<int>(t) * Px(kTabW), m, Px(kTabW), Px(kTabH), TRUE);
    }
    MoveWindow(hUnits_, r0 - Px(80), m + (Px(kTabH) - bh) / 2 - Px(2), Px(80), bh, TRUE);
    tabLine_ = { x0, m + Px(kTabH), r0, m + Px(kTabH) + 1 };
    const int row1 = tabLine_.bottom + Px(10);

    // History, row 1: views (+ months in Month view). The Now tab has none
    // of these, so its header moves up into their place.
    const bool history = !NowTab();
    const int show = history ? SW_SHOW : SW_HIDE;
    int x = x0;
    for (HWND hv : hViews_) {
        ShowWindow(hv, show);
        MoveWindow(hv, x, row1, Px(kViewBtnW), bh, TRUE);
        x += Px(kViewBtnW) + gap;
    }
    x += Px(12);
    const bool months = history && cfg_.state.view == static_cast<int>(View::Month);
    for (HWND hm : hMonths_) {
        ShowWindow(hm, months ? SW_SHOW : SW_HIDE);
        MoveWindow(hm, x, row1, Px(kMonthW), bh, TRUE);
        x += Px(kMonthW) + 2;
    }
    const int headTop = history ? row1 + bh + Px(8) : row1;
    headerRect_ = { x0, headTop, r0, headTop + Px(kHeaderH) };

    // History, row 2: range and measure.
    const int row2 = headerRect_.bottom + Px(4);
    x = x0;
    for (HWND hr : hRanges_) {
        ShowWindow(hr, show);
        MoveWindow(hr, x, row2, Px(kRangeW), bh, TRUE);
        x += Px(kRangeW) + gap;
    }
    x += Px(16);
    for (HWND hm : hMeasures_) {
        ShowWindow(hm, show);
        MoveWindow(hm, x, row2, Px(kMeasureW), bh, TRUE);
        x += Px(kMeasureW) + gap;
    }

    statusRect_ = { x0, h - m - Px(kStatusH), r0, h - m };
    const int top = history ? row2 + bh + Px(12) : headerRect_.bottom + Px(12);
    const int bottom = statusRect_.top - Px(8);
    const int mid = top + (bottom - top) / 2;
    topRect_    = { x0, top, r0, mid - Px(8) };
    bottomRect_ = { x0, mid + Px(8), r0, bottom };
}

void App::SyncControls() {
    const ViewState& s = cfg_.state;
    for (HWND ht : hTabs_) { InvalidateRect(ht, nullptr, FALSE); }
    for (int v = 0; v < kViewCount; ++v) { SetCheck(hViews_[static_cast<size_t>(v)], v == s.view); }
    for (size_t r = 0; r < hRanges_.size(); ++r) {
        SetCheck(hRanges_[r], static_cast<int>(r) == s.range);
        EnableWindow(hRanges_[r], s.view != static_cast<int>(View::Climate));
    }
    for (size_t m = 0; m < hMeasures_.size(); ++m) { SetCheck(hMeasures_[m], static_cast<int>(m) == s.measure); }
    for (size_t m = 0; m < 12; ++m) { SetCheck(hMonths_[m], static_cast<int>(m) == s.month); }
    SetCheck(hUnits_, cfg_.units == Units::Imperial);
    const bool any = cfg_.locCount > 0;
    EnableWindow(hRemove_, any);
    EnableWindow(hUpdate_, any);
    if (hMenu_ != nullptr) {
        auto check = [this](int id, bool on) {
            CheckMenuItem(hMenu_, static_cast<UINT>(id), MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        };
        check(IDM_TAB0, s.tab == static_cast<int>(Tab::History));
        check(IDM_TAB1, s.tab == static_cast<int>(Tab::Now));
        for (int v = 0; v < kViewCount; ++v) { check(IDM_VIEW0 + v, v == s.view && !NowTab()); }
        check(IDM_METRIC, cfg_.units == Units::Metric);
        check(IDM_IMPERIAL, cfg_.units == Units::Imperial);
        check(IDM_THEME_SYSTEM, cfg_.theme == ThemeMode::System);
        check(IDM_THEME_LIGHT, cfg_.theme == ThemeMode::Light);
        check(IDM_THEME_DARK, cfg_.theme == ThemeMode::Dark);
        check(IDM_WEBSERVER, web_.Running());
        EnableMenuItem(hMenu_, IDM_WEBOPEN, MF_BYCOMMAND | (web_.Running() ? MF_ENABLED : MF_GRAYED));
    }
}

void App::ApplyTheme() {
    const bool dark = cfg_.theme == ThemeMode::Dark || (cfg_.theme == ThemeMode::System && SystemPrefersDark());
    theme_ = &ThemeFor(dark);
    if (hBgBrush_ != nullptr)   { DeleteObject(hBgBrush_); }
    if (hListBrush_ != nullptr) { DeleteObject(hListBrush_); }
    hBgBrush_   = CreateSolidBrush(theme_->bgRef);
    hListBrush_ = CreateSolidBrush(theme_->listBgRef);
    const BOOL useDark = dark ? TRUE : FALSE;
    const HRESULT hr = DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDark, sizeof(useDark));
    (void)hr;   // older builds ignore it; the client area is still themed
    const wchar_t* sub = dark ? L"DarkMode_Explorer" : L"Explorer";
    HWND singles[] = { hList_, hAdd_, hRemove_, hUpdate_, hUnits_ };
    for (HWND h : singles) { SetWindowTheme(h, sub, nullptr); }
    for (HWND h : hViews_) { SetWindowTheme(h, sub, nullptr); }
    for (HWND h : hRanges_) { SetWindowTheme(h, sub, nullptr); }
    for (HWND h : hMeasures_) { SetWindowTheme(h, sub, nullptr); }
    for (HWND h : hMonths_) { SetWindowTheme(h, sub, nullptr); }
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void App::RebuildList() {
    SendMessageW(hList_, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < cfg_.locCount; ++i) { SendMessageW(hList_, LB_ADDSTRING, 0, static_cast<LPARAM>(i)); }
    if (cfg_.locCount > 0) {
        if (cfg_.state.selected >= cfg_.locCount) { cfg_.state.selected = 0; }
        SendMessageW(hList_, LB_SETCURSEL, cfg_.state.selected, 0);
    }
}

void App::SetStatus(const std::wstring& text) {
    status_ = text;
    InvalidateRect(hwnd_, &statusRect_, FALSE);
}

// ---------------------------------------------------------------------------
// Data

void App::LoadCaches() {
    for (size_t i = 0; i < cfg_.locCount; ++i) {
        std::string bytes;
        std::wstring err;
        Climate& c = (*climates_)[i];
        if (ReadFileBytes(KeyFor(cfg_.locs[i]), bytes, 4u * 1024u * 1024u, err) && !ClimateFromCsv(bytes, c, err)) {
            errors_[i] = L"Cache unreadable (" + err + L"); downloading again.";
            c = Climate{};
        }
        // The last forecast reply, so the Now tab opens with something; its
        // age is the file's.
        const std::wstring fk = ForecastKeyFor(cfg_.locs[i]);
        Forecast& f = (*forecasts_)[i];
        if (ReadFileBytes(fk, bytes, kMaxForecastBytes, err) && ParseForecast(bytes, f, err)) {
            f.fetched = FileUnixTime(fk);
        } else {
            f = Forecast{};
        }
    }
}

void App::ScheduleForecasts(bool force) {
    if (!NowTab() && !force) { return; }   // only fetched while someone can see them
    const int64_t now = UnixNow();
    for (size_t i = 0; i < cfg_.locCount; ++i) {
        const Forecast& f = (*forecasts_)[i];
        const bool stale = !f.valid || now - f.fetched > kForecastStale;
        const Location& loc = cfg_.locs[i];
        if ((force || stale) && !fetcher_.IsPending(ForecastKeyFor(loc))) {
            if (!fetcher_.QueueForecast(hwnd_, ForecastKeyFor(loc), ForecastUrl(loc.lat, loc.lon))) {
                SetStatus(L"The download queue is full; the forecast will be fetched later.");
                break;
            }
        }
    }
    InvalidateRect(hList_, nullptr, FALSE);
}

void App::OnForecastReady() {
    ForecastResult r;
    size_t updated = 0;
    while (fetcher_.TakeForecast(r)) {
        size_t idx = kMaxLocations;
        for (size_t i = 0; i < cfg_.locCount; ++i) {
            if (ForecastKeyFor(cfg_.locs[i]) == r.key) { idx = i; break; }
        }
        if (idx == kMaxLocations) { continue; }   // removed while downloading
        if (!r.ok) {
            fcErrors_[idx] = r.error;
            SetStatus(L"Forecast failed for " + cfg_.locs[idx].name + L": " + r.error);
            continue;
        }
        assert(r.data != nullptr);
        (*forecasts_)[idx] = *r.data;
        (*forecasts_)[idx].fetched = UnixNow();
        fcErrors_[idx].clear();
        std::wstring err;
        if (!WriteFileBytes(r.key, r.body, err)) { SetStatus(L"Forecast updated, but it could not be saved: " + err); }
        ++updated;
    }
    // Results arrive in batches, so name no count: the last one wins the line.
    if (updated > 0 && fetcher_.PendingArchives() == 0) { SetStatus(L"Forecast updated at " + ClockNow() + L"."); }
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(hList_, nullptr, FALSE);
}

bool App::QueueUpdate(size_t index, bool full) {
    assert(index < cfg_.locCount);
    const Location& loc = cfg_.locs[index];
    const Climate& c = (*climates_)[index];
    std::wstring start = std::to_wstring(kFirstYear) + L"-01-01";
    const int last = c.valid ? LastCompleteMonth(c) : -1;
    if (!full && last >= 0) {
        // Re-fetch the last two complete months too: ERA5's preliminary
        // values are revised for a while after they first appear.
        const int from = last - 2;
        std::array<wchar_t, 16> b{};
        swprintf_s(b.data(), b.size(), L"%04d-%02d-01", from / 12, from % 12 + 1);
        start = b.data();
    } else {
        full = true;
    }
    std::array<wchar_t, 512> url{};
    swprintf_s(url.data(), url.size(),
               L"https://archive-api.open-meteo.com/v1/archive?latitude=%.4f&longitude=%.4f&start_date=%s&end_date=%s"
               L"&daily=temperature_2m_mean,temperature_2m_max,temperature_2m_min,snowfall_sum&timezone=auto",
               loc.lat, loc.lon, start.c_str(), IsoDateDaysAgo(1).c_str());
    return fetcher_.QueueArchive(hwnd_, KeyFor(loc), url.data(), full);
}

void App::ScheduleUpdates(bool force) {
    const int64_t now = UnixNow();
    size_t queued = 0;
    for (size_t i = 0; i < cfg_.locCount; ++i) {
        const Climate& c = (*climates_)[i];
        const bool stale = !c.valid || now - c.fetched > kStaleSeconds;
        if ((force || stale) && !fetcher_.IsPending(KeyFor(cfg_.locs[i]))) {
            queued += QueueUpdate(i, false) ? 1 : 0;
        }
    }
    if (queued > 0) {
        SetStatus(L"Updating " + std::to_wstring(queued) + (queued == 1 ? L" place…" : L" places…"));
    } else if (force) {
        SetStatus(L"Updates already in progress.");
    }
    InvalidateRect(hList_, nullptr, FALSE);
}

void App::OnArchiveReady() {
    ArchiveResult r;
    while (fetcher_.TakeArchive(r)) {
        size_t idx = kMaxLocations;
        for (size_t i = 0; i < cfg_.locCount; ++i) {
            if (KeyFor(cfg_.locs[i]) == r.key) { idx = i; break; }
        }
        if (idx == kMaxLocations) { continue; }   // removed while downloading
        const std::wstring& name = cfg_.locs[idx].name;
        if (r.retrying) {
            SetStatus(name + L": " + r.error);
            continue;
        }
        if (!r.ok) {
            errors_[idx] = r.error;
            SetStatus(L"Download failed for " + name + L": " + r.error);
            continue;
        }
        Climate& c = (*climates_)[idx];
        if (r.full) { c = Climate{}; }
        std::wstring err;
        if (!MergeClimate(c, *r.data, err)) {
            errors_[idx] = err;
            SetStatus(L"Could not use the data for " + name + L": " + err);
            continue;
        }
        c.fetched = UnixNow();
        errors_[idx].clear();
        if (!WriteFileBytes(KeyFor(cfg_.locs[idx]), ClimateToCsv(c), err)) {
            SetStatus(L"Updated " + name + L", but the cache could not be saved: " + err);
        } else {
            std::array<wchar_t, 160> b{};
            swprintf_s(b.data(), b.size(), L" (%s, %.0f KB in %.1f s).", r.full ? L"full history" : L"update",
                       static_cast<double>(r.bytes) / 1024.0, r.elapsedMs / 1000.0);
            const size_t left = fetcher_.PendingArchives();
            SetStatus(L"Updated " + name + b.data() +
                      (left > 0 ? L" " + std::to_wstring(left) + L" still to go." : std::wstring()));
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(hList_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Commands

bool App::AddPlace(const Location& loc, std::wstring& err) {
    if (cfg_.locCount >= kMaxLocations) {
        err = L"The list is full (" + std::to_wstring(kMaxLocations) + L" places).";
        return false;
    }
    const std::wstring key = KeyFor(loc);
    for (size_t i = 0; i < cfg_.locCount; ++i) {
        if (KeyFor(cfg_.locs[i]) == key) {
            err = cfg_.locs[i].name + L" is already in the list.";
            return false;
        }
    }
    const size_t idx = cfg_.locCount++;
    cfg_.locs[idx] = loc;
    (*climates_)[idx] = Climate{};
    (*forecasts_)[idx] = Forecast{};
    errors_[idx].clear();
    fcErrors_[idx].clear();
    std::string bytes;
    std::wstring ignored;
    if (ReadFileBytes(key, bytes, 4u * 1024u * 1024u, ignored)) {
        (void)ClimateFromCsv(bytes, (*climates_)[idx], ignored);   // re-added: reuse the old cache
    }
    if (!SaveLocations(cfg_, err)) { return false; }
    RebuildList();
    SelectPlace(idx);
    ScheduleForecasts(false);   // on the Now tab, fetch its forecast first
    if (!QueueUpdate(idx, false)) {
        err = L"Added, but the download queue is full; press Update later.";
        return false;
    }
    SetStatus(L"Downloading " + loc.name + L"…");
    SyncControls();
    return true;
}

void App::OnRemovePlace() {
    const size_t i = cfg_.state.selected;
    if (i >= cfg_.locCount) { return; }
    const std::wstring q = L"Remove " + cfg_.locs[i].name + L" and its downloaded data?";
    if (MessageBoxW(hwnd_, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) { return; }
    DeleteFileW(KeyFor(cfg_.locs[i]).c_str());
    DeleteFileW(ForecastKeyFor(cfg_.locs[i]).c_str());
    for (size_t k = i; k + 1 < cfg_.locCount; ++k) {
        cfg_.locs[k] = cfg_.locs[k + 1];
        (*climates_)[k] = (*climates_)[k + 1];
        (*forecasts_)[k] = (*forecasts_)[k + 1];
        errors_[k] = errors_[k + 1];
        fcErrors_[k] = fcErrors_[k + 1];
    }
    --cfg_.locCount;
    cfg_.locs[cfg_.locCount] = Location{};
    (*climates_)[cfg_.locCount] = Climate{};
    (*forecasts_)[cfg_.locCount] = Forecast{};
    errors_[cfg_.locCount].clear();
    fcErrors_[cfg_.locCount].clear();
    std::wstring err;
    if (!SaveLocations(cfg_, err)) { SetStatus(err); }
    if (cfg_.state.selected >= cfg_.locCount && cfg_.locCount > 0) { cfg_.state.selected = cfg_.locCount - 1; }
    RebuildList();
    SyncControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnMovePlace(int delta) {
    const size_t i = cfg_.state.selected;
    const long j = static_cast<long>(i) + delta;
    if (i >= cfg_.locCount || j < 0 || j >= static_cast<long>(cfg_.locCount)) { return; }
    const size_t k = static_cast<size_t>(j);
    std::swap(cfg_.locs[i], cfg_.locs[k]);
    std::swap((*climates_)[i], (*climates_)[k]);
    std::swap((*forecasts_)[i], (*forecasts_)[k]);
    std::swap(errors_[i], errors_[k]);
    std::swap(fcErrors_[i], fcErrors_[k]);
    cfg_.state.selected = k;
    std::wstring err;
    if (!SaveLocations(cfg_, err)) { SetStatus(err); }
    RebuildList();
}

void App::SelectPlace(size_t index) {
    if (index >= cfg_.locCount) { return; }
    cfg_.state.selected = index;
    SendMessageW(hList_, LB_SETCURSEL, index, 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetView(int view) {
    assert(view >= 0 && view < kViewCount);
    cfg_.state.view = view;
    if (NowTab()) {
        SetTab(static_cast<int>(Tab::History));   // Ctrl+1..4 from the Now tab
        return;
    }
    SyncControls();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetTab(int tab) {
    assert(tab >= 0 && tab < kTabCount);
    cfg_.state.tab = tab;
    hoverX_ = hoverY_ = -1;
    SyncControls();
    Layout();
    ScheduleForecasts(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateRect(hList_, nullptr, FALSE);
}

void App::OnExportCsv() {
    const size_t i = cfg_.state.selected;
    if (!HasData(i)) {
        SetStatus(L"Nothing to export yet.");
        return;
    }
    std::array<wchar_t, MAX_PATH> path{};
    std::wstring suggested = cfg_.locs[i].name.substr(0, cfg_.locs[i].name.find(L',')) + L"_monthly.csv";
    for (wchar_t& c : suggested) {
        if (wcschr(L"\\/:*?\"<>|", c) != nullptr) { c = L'_'; }
    }
    wcsncpy_s(path.data(), path.size(), suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile   = path.data();
    ofn.nMaxFile    = static_cast<DWORD>(path.size());
    ofn.lpstrDefExt = L"csv";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) { return; }
    // The cache format minus its signature line is already a plain CSV.
    std::string csv = ClimateToCsv((*climates_)[i]);
    csv.erase(0, csv.find('\n') + 1);
    std::wstring err;
    SetStatus(WriteFileBytes(path.data(), csv, err) ? L"Exported " + std::wstring(path.data()) : err);
}

void App::OnCommand(int id, int code) {
    if (id == IDC_LIST) {
        if (code == LBN_SELCHANGE) {
            const LRESULT sel = SendMessageW(hList_, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) { SelectPlace(static_cast<size_t>(sel)); }
        }
        return;
    }
    if (id >= IDC_TAB0 && id < IDC_TAB0 + kTabCount) { SetTab(id - IDC_TAB0); return; }
    if (id == IDM_TAB0 || id == IDM_TAB1) { SetTab(id - IDM_TAB0); return; }
    if (id == IDM_TABNEXT) { SetTab((cfg_.state.tab + 1) % kTabCount); return; }
    if (id >= IDC_VIEW0 && id < IDC_VIEW0 + kViewCount) { SetView(id - IDC_VIEW0); return; }
    if (id >= IDM_VIEW0 && id < IDM_VIEW0 + kViewCount) { SetView(id - IDM_VIEW0); return; }
    if (id >= IDC_RANGE0 && id < IDC_RANGE0 + static_cast<int>(kRangeYears.size())) {
        cfg_.state.range = id - IDC_RANGE0;
        SyncControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= IDC_MEASURE0 && id < IDC_MEASURE0 + 3) {
        cfg_.state.measure = id - IDC_MEASURE0;
        SyncControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= IDC_MONTH0 && id < IDC_MONTH0 + 12) {
        cfg_.state.month = id - IDC_MONTH0;
        SyncControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    std::wstring err;
    switch (id) {
    case IDC_ADD:
    case IDM_ADD:      ShowAddPlaceDialog(hInst_, hwnd_, fetcher_, *this); break;
    case IDC_REMOVE:
    case IDM_REMOVE:   OnRemovePlace(); break;
    case IDM_MOVEUP:   OnMovePlace(-1); break;
    case IDM_MOVEDOWN: OnMovePlace(+1); break;
    case IDC_UPDATE:
    case IDM_UPDATE:
        if (NowTab()) {
            ScheduleForecasts(true);
            SetStatus(L"Fetching the forecast…");
        } else {
            ScheduleUpdates(true);
        }
        break;
    case IDM_REDOWNLOAD:
        if (cfg_.state.selected < cfg_.locCount && QueueUpdate(cfg_.state.selected, true)) {
            SetStatus(L"Downloading the full history of " + cfg_.locs[cfg_.state.selected].name + L"…");
        }
        break;
    case IDC_UNITS:
    case IDM_METRIC:
    case IDM_IMPERIAL:
        cfg_.units = (id == IDM_METRIC) ? Units::Metric
                   : (id == IDM_IMPERIAL) ? Units::Imperial
                   : (cfg_.units == Units::Metric ? Units::Imperial : Units::Metric);
        if (!SaveSettings(cfg_, err)) { SetStatus(err); }
        SyncControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateRect(hList_, nullptr, FALSE);
        break;
    case IDM_THEME_SYSTEM:
    case IDM_THEME_LIGHT:
    case IDM_THEME_DARK:
        cfg_.theme = id == IDM_THEME_LIGHT ? ThemeMode::Light : id == IDM_THEME_DARK ? ThemeMode::Dark : ThemeMode::System;
        if (!SaveSettings(cfg_, err)) { SetStatus(err); }
        ApplyTheme();
        SyncControls();
        break;
    case IDM_EXPORT:   OnExportCsv(); break;
    case IDM_OPENDIR:  ShellExecuteW(hwnd_, L"open", cfg_.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_WEBSERVER: OnToggleWebServer(); break;
    case IDM_WEBOPEN:
        if (web_.Running()) {
            const std::wstring url = L"http://localhost:" + std::to_wstring(web_.Port()) + L"/";
            ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case IDM_EXIT:     OnClose(); break;
    case IDM_ABOUT: {
        const std::wstring t = std::wstring(kAppName) + L" " + kVersion +
            L"\n\nHistorical temperature and snowfall for the places you choose, and their weather now.\n\n"
            L"Weather data by Open-Meteo.com, licensed CC BY 4.0. History: ERA5 and ERA5-Land reanalysis from the "
            L"Copernicus Climate Change Service. Now: Open-Meteo's forecast from national weather services' models. "
            L"Values describe a model grid cell, not a weather station; snowfall in particular can differ from a "
            L"local gauge.\n\nData folder:\n" + cfg_.dir +
            L"\n\nWeb server: " + (web_.Running() ? WebUrls() : std::wstring(L"off (File > Web server)"));
        MessageBoxW(hwnd_, t.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
        break;
    }
    default: break;
    }
}

void App::OnClose() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        cfg_.state.hasPlacement = true;
        cfg_.state.placement = wp.rcNormalPosition;
        cfg_.state.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    std::wstring err;
    if (!SaveState(cfg_, err)) {
        MessageBoxW(hwnd_, (L"Could not save the window state:\n" + err).c_str(), kAppName, MB_OK | MB_ICONWARNING);
    }
    ShowWindow(hwnd_, SW_HIDE);   // a download in flight may take a moment to finish
    web_.Stop();
    fetcher_.Stop();
    DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------
// Mouse and DPI

void App::OnMouseMove(int x, int y) {
    if (!tracking_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        tracking_ = TrackMouseEvent(&tme) != 0;
    }
    const POINT p{ x, y };
    const bool inPanels = PtInRect(&topRect_, p) || PtInRect(&bottomRect_, p);
    const bool wasIn = hoverX_ >= 0;
    if (!inPanels && !wasIn) { return; }
    hoverX_ = inPanels ? x : -1;
    hoverY_ = inPanels ? y : -1;
    InvalidateRect(hwnd_, &topRect_, FALSE);
    InvalidateRect(hwnd_, &bottomRect_, FALSE);
}

void App::OnMouseLeave() {
    tracking_ = false;
    if (hoverX_ < 0) { return; }
    hoverX_ = -1;
    hoverY_ = -1;
    InvalidateRect(hwnd_, &topRect_, FALSE);
    InvalidateRect(hwnd_, &bottomRect_, FALSE);
}

void App::OnDpiChanged(WPARAM wp, LPARAM lp) {
    scale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
    CreateFonts();
    auto setFont = [this](HWND h) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE); };
    HWND singles[] = { hAdd_, hRemove_, hUpdate_, hUnits_ };
    for (HWND h : singles) { setFont(h); }
    for (HWND h : hViews_) { setFont(h); }
    for (HWND h : hRanges_) { setFont(h); }
    for (HWND h : hMeasures_) { setFont(h); }
    for (HWND h : hMonths_) { setFont(h); }
    SendMessageW(hList_, LB_SETITEMHEIGHT, 0, Px(kItemH));
    const RECT* r = reinterpret_cast<const RECT*>(lp);
    SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// The list

void App::OnMeasureItem(MEASUREITEMSTRUCT* mis) const {
    mis->itemHeight = static_cast<UINT>(Px(kItemH));
}

void App::OnDrawItem(const DRAWITEMSTRUCT* dis) {
    if (dis->CtlID >= IDC_TAB0 && dis->CtlID < static_cast<UINT>(IDC_TAB0 + kTabCount)) {
        DrawTab(dis);
        return;
    }
    if (dis->CtlID != IDC_LIST || dis->itemID == static_cast<UINT>(-1)) { return; }
    const size_t i = dis->itemID;
    if (i >= cfg_.locCount) { return; }
    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const RectF r = ToRectF(dis->rcItem);
    const bool sel = (dis->itemState & ODS_SELECTED) != 0;
    SolidBrush bg(sel ? theme_->listSel : theme_->listBg);
    g.FillRectangle(&bg, r);
    Pen div(theme_->listDivider, 1.0f);
    g.DrawLine(&div, r.X, r.Y + r.Height - 1, r.X + r.Width, r.Y + r.Height - 1);

    const float s = scale_;
    const float pad = 10.0f * s;
    const Font fName = UiFont(9.5f, s, FontStyleBold);
    DrawTextClip(g, cfg_.locs[i].name, fName, theme_->text, RectF(r.X + pad, r.Y + 5.0f * s, r.Width - 2 * pad, 20.0f * s));
    if (NowTab()) {
        DrawListRowNow(g, r, i);
    } else {
        DrawListRow(g, r, i);
    }
}

// A tab: label, and an accent bar under the selected one.
void App::DrawTab(const DRAWITEMSTRUCT* dis) {
    const int t = static_cast<int>(dis->CtlID) - IDC_TAB0;
    assert(t >= 0 && t < kTabCount);
    Graphics g(dis->hDC);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    const RectF r = ToRectF(dis->rcItem);
    SolidBrush bg(theme_->bg);
    g.FillRectangle(&bg, r);
    const bool on = t == cfg_.state.tab;
    const Font f = UiFont(10.5f, scale_, on ? FontStyleBold : FontStyleRegular);
    const std::wstring label = kTabNames[t];
    const float w = TextW(g, label, f);
    PaintText(g, label, f, on ? theme_->text : theme_->textSecondary, r.X + (r.Width - w) / 2.0f, r.Y + 4.0f * scale_);
    if (on) {
        SolidBrush accent(theme_->series1);
        g.FillRectangle(&accent, r.X + 8.0f * scale_, r.Y + r.Height - 3.0f * scale_, r.Width - 16.0f * scale_, 3.0f * scale_);
    }
    if ((dis->itemState & ODS_FOCUS) != 0 && (dis->itemState & ODS_NOFOCUSRECT) == 0) {
        Pen focus(theme_->textMuted, 1.0f);
        focus.SetDashStyle(DashStyleDot);
        g.DrawRectangle(&focus, r.X + 2.0f, r.Y + 2.0f, r.Width - 5.0f, r.Height - 8.0f * scale_);
    }
}

// Now tab: the temperature and conditions now, and today's high and low.
void App::DrawListRowNow(Graphics& g, const RectF& r, size_t i) {
    const float s = scale_;
    const float pad = 10.0f * s;
    const float y2 = r.Y + 25.0f * s;
    const Font fSmall = UiFont(8.5f, s);
    if (!HasForecast(i)) {
        const bool busy = fetcher_.IsPending(ForecastKeyFor(cfg_.locs[i]));
        const std::wstring t = busy ? L"Fetching the forecast…" : fcErrors_[i].empty() ? L"No forecast yet" : fcErrors_[i];
        DrawTextClip(g, t, fSmall, theme_->textMuted, RectF(r.X + pad, y2, r.Width - 2 * pad, 18.0f * s));
        return;
    }
    const Forecast& f = (*forecasts_)[i];
    const Units u = cfg_.units;
    const Font fSym = GlyphFont(9.0f, s);
    const std::wstring glyph = WeatherGlyph(f.code, f.isDay);
    float x = r.X + pad;
    if (!glyph.empty()) {
        PaintText(g, glyph, fSym, theme_->textSecondary, x, y2 - 2.0f * s);
        x += TextW(g, glyph, fSym) + 2.0f * s;
    }
    const size_t d0 = FirstDayFrom(f, UnixNow());
    std::wstring hl;
    if (d0 < f.days) { hl = L"H " + FmtNumber(ToTemp(f.dMax[d0], u), 0, L"") + L"  L " + FmtNumber(ToTemp(f.dMin[d0], u), 0, L""); }
    const float hlW = hl.empty() ? 0.0f : TextW(g, hl, fSmall);
    const std::wstring t = FmtTemp(f.temp, u) + L"  " + WeatherText(f.code);
    DrawTextClip(g, t, fSmall, theme_->textSecondary, RectF(x, y2, r.X + r.Width - pad - hlW - 6.0f * s - x, 18.0f * s));
    if (!hl.empty()) { PaintText(g, hl, fSmall, theme_->textMuted, r.X + r.Width - pad - hlW, y2); }
}

// History tab: last year's mean against the normal, and last season's snow.
void App::DrawListRow(Graphics& g, const RectF& r, size_t i) {
    const float s = scale_;
    const float pad = 10.0f * s;
    const float y2 = r.Y + 25.0f * s;
    const Font fSmall = UiFont(8.5f, s);
    const Climate& c = (*climates_)[i];
    if (!HasData(i)) {
        const bool busy = fetcher_.IsPending(KeyFor(cfg_.locs[i]));
        const std::wstring t = busy ? L"Downloading history…" : errors_[i].empty() ? L"No data yet" : errors_[i];
        DrawTextClip(g, t, fSmall, theme_->textMuted, RectF(r.X + pad, y2, r.Width - 2 * pad, 18.0f * s));
        return;
    }
    const YearSeries a = AnnualTemp(c, Var::Mean);
    const int ya = LastValidYear(a);
    float x = r.X + pad;
    if (ya > 0) {
        const double v = a.v[static_cast<size_t>(ya - a.firstYear)];
        const std::wstring t = std::to_wstring(ya) + L": " + FmtTemp(v, cfg_.units) + L" ";
        PaintText(g, t, fSmall, theme_->textSecondary, x, y2);
        x += TextW(g, t, fSmall);
        const double normal = PeriodMean(a, kNormal0, kNormal1);
        if (!std::isnan(normal)) {
            const double d = v - normal;
            PaintText(g, FmtDelta(d, cfg_.units, 1), fSmall, d >= 0 ? theme_->warm : theme_->cool, x - 3.0f * s, y2);
        }
    }
    const YearSeries sn = SeasonSnow(c);
    const int ys = LastValidYear(sn);
    if (ys > 0) {
        const std::wstring t = L"❄ " + FmtSnow(sn.v[static_cast<size_t>(ys - sn.firstYear)], cfg_.units);
        PaintText(g, t, fSmall, theme_->textSecondary, r.X + r.Width - pad - TextW(g, t, fSmall), y2);
    }
}

// ---------------------------------------------------------------------------
// Painting

void App::EnsureBackBuffer(HDC hdc, int w, int h) {
    if (memDC_ != nullptr && w <= bufW_ && h <= bufH_) { return; }
    FreeBackBuffer();
    memDC_  = CreateCompatibleDC(hdc);
    memBmp_ = CreateCompatibleBitmap(hdc, w, h);
    oldBmp_ = SelectObject(memDC_, memBmp_);
    bufW_ = w;
    bufH_ = h;
}

void App::FreeBackBuffer() {
    if (memDC_ == nullptr) { return; }
    SelectObject(memDC_, oldBmp_);
    DeleteObject(memBmp_);
    DeleteDC(memDC_);
    memDC_ = nullptr;
    memBmp_ = nullptr;
    bufW_ = bufH_ = 0;
}

void App::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        EnsureBackBuffer(hdc, rc.right, rc.bottom);
        {
            Graphics g(memDC_);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            SolidBrush bg(theme_->bg);
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
            PaintTabLine(g);
            if (NowTab()) {
                PaintNowHeader(g);
                PaintNow(g);
            } else {
                PaintHeader(g);
                PaintPanels(g);
            }
            PaintStatus(g);
        }
        BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, memDC_, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

void App::PaintHeader(Graphics& g) {
    const RectF r = ToRectF(headerRect_);
    const float s = scale_;
    const size_t i = cfg_.state.selected;
    if (i >= cfg_.locCount) { return; }
    const Location& loc = cfg_.locs[i];
    const Climate& c = (*climates_)[i];
    const Units u = cfg_.units;

    // Figures on the right: four blocks, narrowing so the name keeps ~290 dip.
    const float blockW = std::fmax(128.0f * s, std::fmin(168.0f * s, (r.Width - 290.0f * s) / 4.0f));
    const size_t blocks = HasData(i) ? 4 : 0;
    const float nameW = r.Width - blockW * static_cast<float>(blocks) - 12.0f * s;
    DrawTextClip(g, loc.name, UiFont(15.0f, s, FontStyleBold), theme_->text, RectF(r.X, r.Y + 4.0f * s, nameW, 30.0f * s));
    std::array<wchar_t, 256> line{};
    if (HasData(i)) {
        const std::wstring elev = std::isnan(c.elevation) ? std::wstring(L"?") : std::to_wstring(std::lround(c.elevation));
        swprintf_s(line.data(), line.size(), L"%.2f°%c, %.2f°%c · grid cell at %s m · %d–%d · updated %s",
                   std::fabs(c.lat), c.lat >= 0 ? L'N' : L'S', std::fabs(c.lon), c.lon >= 0 ? L'E' : L'W', elev.c_str(),
                   c.firstYear, c.firstYear + static_cast<int>(c.yearCount) - 1, LocalDate(c.fetched).c_str());
    } else {
        swprintf_s(line.data(), line.size(), L"%.2f°, %.2f° · no data downloaded yet", loc.lat, loc.lon);
    }
    DrawTextClip(g, line.data(), UiFont(8.5f, s), theme_->textSecondary, RectF(r.X, r.Y + 38.0f * s, nameW, 18.0f * s));
    if (blocks == 0) { return; }

    const YearSeries a = AnnualTemp(c, Var::Mean);
    const YearSeries sn = SeasonSnow(c);
    const int from = FromYear(c);
    const Trend ta = LinearTrend(a, from);
    const Trend ts = LinearTrend(sn, from);
    int r0 = 0, r1 = 0, s0 = 0, s1 = 0;
    const bool haveRecent = LastValidSpan(a, 10, r0, r1);
    const bool haveSnow = LastValidSpan(sn, 10, s0, s1);
    const double oldT = PeriodMean(a, kOld0, kOld1);
    const double newT = haveRecent ? PeriodMean(a, r0, r1) : kNaN;
    const double oldS = PeriodMean(sn, kOld0, kOld1);
    const double newS = haveSnow ? PeriodMean(sn, s0, s1) : kNaN;

    struct Block { std::wstring k, v, d; };
    std::array<Block, 4> b;
    // Kept short: at the minimum window width a block is ~130 dip wide.
    b[0] = { L"Temp. per decade", ta.valid ? FmtDelta(ta.slope * 10.0, u) : L"-",
             L"annual mean, " + std::to_wstring(from) + L"–" };
    b[1] = { L"Last 10 yr vs 1961–90", FmtDelta(newT - oldT, u),
             haveRecent ? FmtNumber(ToTemp(newT, u), 1, L"") + L" vs " + FmtTemp(oldT, u) : std::wstring(L"not enough years") };
    b[2] = { L"Snow per decade", ts.valid ? FmtSnowDelta(ts.slope * 10.0, u) : L"-",
             L"season total, " + std::to_wstring(from) + L"–" };
    std::wstring pct;
    if (!std::isnan(oldS) && oldS > 0.0 && !std::isnan(newS)) {
        pct = L", " + FmtNumber((newS / oldS - 1.0) * 100.0, 0, L"", true) + L"%";
    }
    b[3] = { L"Snow, last 10 seasons", FmtSnow(newS, u), L"1961–90: " + FmtSnow(oldS, u) + pct };

    const Font fk = UiFont(8.5f, s);
    const Font fv = UiFont(14.0f, s, FontStyleBold);
    const Font fd = UiFont(8.0f, s);
    Pen sep(theme_->grid, 1.0f);
    for (size_t k = 0; k < 4; ++k) {
        const float x = r.X + r.Width - blockW * static_cast<float>(4 - k);
        g.DrawLine(&sep, x, r.Y + 6.0f * s, x, r.Y + r.Height - 8.0f * s);
        const float tx = x + 10.0f * s;
        const float tw = blockW - 14.0f * s;
        DrawTextClip(g, b[k].k, fk, theme_->textSecondary, RectF(tx, r.Y + 4.0f * s, tw, 16.0f * s));
        DrawTextClip(g, b[k].v, fv, theme_->text, RectF(tx, r.Y + 19.0f * s, tw, 26.0f * s));
        DrawTextClip(g, b[k].d, fd, theme_->textMuted, RectF(tx, r.Y + 46.0f * s, tw, 16.0f * s));
    }
}

void App::PaintEmpty(Graphics& g, const std::wstring& text) {
    RECT all = { topRect_.left, topRect_.top, bottomRect_.right, bottomRect_.bottom };
    const RectF r = ToRectF(all);
    const Font f = UiFont(11.0f, scale_);
    const float w = TextW(g, text, f);
    PaintText(g, text, f, theme_->textMuted, r.X + (r.Width - w) / 2.0f, r.Y + r.Height / 2.0f - 20.0f * scale_);
}

void App::PaintPanels(Graphics& g) {
    const size_t i = cfg_.state.selected;
    if (cfg_.locCount == 0) {
        PaintEmpty(g, L"No places yet. Press Add... and search for a town.");
        return;
    }
    if (!HasData(i)) {
        const bool busy = fetcher_.IsPending(KeyFor(cfg_.locs[i]));
        PaintEmpty(g, busy ? L"Downloading weather history since 1940…"
                           : errors_[i].empty() ? L"No data yet. Press Update." : L"Download failed: " + errors_[i]);
        return;
    }
    const Climate& c = (*climates_)[i];
    switch (static_cast<View>(cfg_.state.view)) {
    case View::Trends:  PaintTrends(g, c, false); break;
    case View::Month:   PaintTrends(g, c, true); break;
    case View::Heatmap: PaintHeatmaps(g, c); break;
    case View::Climate: PaintClimate(g, c); break;
    }
}

// Trends (annual) and Month (one calendar month): temperature line chart
// above, snowfall bars below, each with a 10-year mean and a trend.
void App::PaintTrends(Graphics& g, const Climate& c, bool monthly) {
    const Units u = cfg_.units;
    const Var var = static_cast<Var>(cfg_.state.measure);
    const int month = cfg_.state.month;
    const int from = FromYear(c);
    const YearSeries t = monthly ? MonthSeries(c, var, month) : AnnualTemp(c, var);
    const YearSeries sn = monthly ? MonthSeries(c, Var::Snow, month) : SeasonSnow(c);
    const Trend tt = LinearTrend(t, from);
    const Trend ts = LinearTrend(sn, from);

    const Slice tv = MakeSlice(t, from, Conv::Temp, u);
    const Slice tr = MakeSlice(Rolling(t, 10, 8), from, Conv::Temp, u);
    Slice tl = MakeSlice(TrendLine(t, tt, from), from, Conv::Temp, u);
    tl.n = tv.n;
    const Slice sv = MakeSlice(sn, from, Conv::Snow, u);
    const Slice sr = MakeSlice(Rolling(sn, 10, 8), from, Conv::Snow, u);
    Slice sl = MakeSlice(TrendLine(sn, ts, from), from, Conv::Snow, u);
    sl.n = sv.n;

    BandChartInput top;
    top.title = monthly ? std::wstring(kMonthLong[month]) + L" " + MeasureTitle(cfg_.state.measure)
                        : std::wstring(L"Annual ") + MeasureTitle(cfg_.state.measure);
    top.subtitle = PerDecade(tt, false, u);
    top.axis = XAxis::Years;
    top.firstYear = tv.firstYear;
    top.n = tv.n;
    top.unit = TempUnit(u);
    top.decimals = 1;
    top.theme = theme_;
    top.hoverX = hoverX_;
    top.hoverY = hoverY_;
    top.series[0] = { tv.v.data(), theme_->series2Soft, 1.5f, false, false, true, monthly ? L"Month" : L"Year" };
    top.series[1] = { tr.v.data(), theme_->series2, 3.0f, false, false, true, L"10-year mean" };
    top.series[2] = { tl.v.data(), theme_->trend, 1.5f, false, true, true, L"Trend" };
    top.seriesCount = 3;
    DrawBandChart(g, ToRectF(topRect_), top, scale_);

    BandChartInput bot;
    bot.title = monthly ? std::wstring(kMonthLong[month]) + L" snowfall" : std::wstring(L"Snowfall per winter season");
    bot.subtitle = (monthly ? std::wstring() : std::wstring(L"July to June · ")) + PerDecade(ts, true, u);
    bot.axis = monthly ? XAxis::Years : XAxis::Seasons;
    bot.firstYear = sv.firstYear;
    bot.n = sv.n;
    bot.unit = SnowUnit(u);
    bot.decimals = SnowDecimals(u);
    bot.theme = theme_;
    bot.hoverX = hoverX_;
    bot.hoverY = hoverY_;
    bot.series[0] = { sv.v.data(), theme_->series1, 0.0f, true, false, true, monthly ? L"Snowfall" : L"Season total" };
    bot.series[1] = { sr.v.data(), theme_->text, 2.0f, false, false, true, L"10-year mean" };
    bot.series[2] = { sl.v.data(), theme_->trend, 1.5f, false, true, true, L"Trend" };
    bot.seriesCount = 3;
    DrawBandChart(g, ToRectF(bottomRect_), bot, scale_);
}

void App::PaintHeatmaps(Graphics& g, const Climate& c) {
    const Units u = cfg_.units;
    const Var var = static_cast<Var>(cfg_.state.measure);
    const int from = FromYear(c);
    const size_t start = static_cast<size_t>(from - c.firstYear);
    const size_t n = c.yearCount - start;
    const std::array<double, 12> normal = MonthNormals(c, var, kNormal0, kNormal1);

    auto anom  = std::make_unique<YearMonthGrid>();
    auto temp  = std::make_unique<YearMonthGrid>();
    auto snow  = std::make_unique<YearMonthGrid>();
    std::array<double, kMaxYears * 12> positives{};
    size_t np = 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t m = 0; m < 12; ++m) {
            const MonthCell& cell = c.cells[start + i][m];
            const double t = cell.v[static_cast<size_t>(var)];
            (*anom)[i][m] = ToDelta(t - normal[m], u);
            (*temp)[i][m] = ToTemp(t, u);
            (*snow)[i][m] = ToSnow(cell.v[static_cast<size_t>(Var::Snow)], u);
            if (cell.v[static_cast<size_t>(Var::Snow)] > 0.0) { positives[np++] = (*snow)[i][m]; }
        }
    }
    // Snow scale tops out at the 95th percentile so one huge month does not wash out the rest.
    std::sort(positives.begin(), positives.begin() + static_cast<long>(np));
    const double cap = np > 0 ? std::fmax(positives[static_cast<size_t>(np * 0.95)], 1.0) : 1.0;

    HeatmapInput top;
    top.title = std::wstring(L"Monthly ") + MeasureTitle(cfg_.state.measure) + L" anomaly";
    top.subtitle = L"against each month’s 1991–2020 average · red warmer, blue colder";
    top.firstYear = from;
    top.nYears = n;
    top.values = anom.get();
    top.extra = temp.get();
    top.valueLabel = L"Anomaly";
    top.extraLabel = L"Value";
    top.diverging = true;
    top.limit = u == Units::Metric ? 5.0 : 9.0;
    top.decimals = 1;
    top.unit = TempUnit(u);
    top.signedValues = true;
    top.hoverX = hoverX_;
    top.hoverY = hoverY_;
    top.theme = theme_;
    DrawHeatmap(g, ToRectF(topRect_), top, scale_);

    HeatmapInput bot;
    bot.title = L"Monthly snowfall";
    bot.subtitle = L"darker is more snow";
    bot.firstYear = from;
    bot.nYears = n;
    bot.values = snow.get();
    bot.valueLabel = L"Snowfall";
    bot.limit = cap;
    bot.decimals = SnowDecimals(u);
    bot.unit = SnowUnit(u);
    bot.hoverX = hoverX_;
    bot.hoverY = hoverY_;
    bot.theme = theme_;
    DrawHeatmap(g, ToRectF(bottomRect_), bot, scale_);
}

void App::PaintClimate(Graphics& g, const Climate& c) {
    const Units u = cfg_.units;
    const Var var = static_cast<Var>(cfg_.state.measure);
    struct Period { int y0, y1; std::wstring label; };
    std::array<Period, 3> periods{};
    size_t np = 0;
    periods[np++] = { kOld0, kOld1, L"1961–1990" };
    periods[np++] = { kNormal0, kNormal1, L"1991–2020" };
    int r0 = 0, r1 = 0;
    if (LastValidSpan(AnnualTemp(c, Var::Mean), 10, r0, r1)) {
        periods[np++] = { r0, r1, std::to_wstring(r0) + L"–" + std::to_wstring(r1) };
    }
    const Color colors[3] = { theme_->series1, theme_->series2, theme_->series3 };
    std::array<std::array<double, kMaxYears>, 3> tv{};
    std::array<std::array<double, kMaxYears>, 3> sv{};
    BandChartInput top;
    BandChartInput bot;
    for (size_t k = 0; k < np; ++k) {
        const auto t = MonthNormals(c, var, periods[k].y0, periods[k].y1);
        const auto s = MonthNormals(c, Var::Snow, periods[k].y0, periods[k].y1);
        for (size_t m = 0; m < 12; ++m) {
            tv[k][m] = ToTemp(t[m], u);
            sv[k][m] = ToSnow(s[m], u);
        }
        top.series[k] = { tv[k].data(), colors[k], 2.5f, false, false, true, periods[k].label };
        bot.series[k] = { sv[k].data(), colors[k], 0.0f, true, false, true, periods[k].label };
    }
    top.seriesCount = bot.seriesCount = np;
    top.title = std::wstring(L"Monthly ") + MeasureTitle(cfg_.state.measure) + L" by period";
    top.subtitle = L"each month averaged over the period";
    top.axis = bot.axis = XAxis::Months;
    top.n = bot.n = 12;
    top.unit = TempUnit(u);
    top.decimals = 1;
    bot.title = L"Monthly snowfall by period";
    bot.subtitle = L"average snowfall in each month";
    bot.unit = SnowUnit(u);
    bot.decimals = SnowDecimals(u) + 1;
    top.theme = bot.theme = theme_;
    top.hoverX = bot.hoverX = hoverX_;
    top.hoverY = bot.hoverY = hoverY_;
    DrawBandChart(g, ToRectF(topRect_), top, scale_);
    DrawBandChart(g, ToRectF(bottomRect_), bot, scale_);
}

// ---------------------------------------------------------------------------
// The Now tab

void App::PaintTabLine(Graphics& g) {
    SolidBrush b(theme_->grid);
    g.FillRectangle(&b, ToRectF(tabLine_));
}

void App::PaintNowHeader(Graphics& g) {
    const RectF r = ToRectF(headerRect_);
    const float s = scale_;
    const size_t i = cfg_.state.selected;
    if (i >= cfg_.locCount) { return; }
    const Units u = cfg_.units;
    const bool have = HasForecast(i);
    const float blockW = std::fmax(128.0f * s, std::fmin(168.0f * s, (r.Width - 290.0f * s) / 4.0f));
    const size_t blocks = have ? 4 : 0;
    const float nameW = r.Width - blockW * static_cast<float>(blocks) - 12.0f * s;
    DrawTextClip(g, cfg_.locs[i].name, UiFont(15.0f, s, FontStyleBold), theme_->text, RectF(r.X, r.Y + 4.0f * s, nameW, 30.0f * s));
    const Font fLine = UiFont(8.5f, s);
    if (!have) {
        DrawTextClip(g, L"no forecast yet", fLine, theme_->textSecondary, RectF(r.X, r.Y + 38.0f * s, nameW, 18.0f * s));
        return;
    }
    const Forecast& f = (*forecasts_)[i];

    // "☁ Overcast · 16:00 EST at the place · grid cell at 74 m · updated 21 Nov 2026 16:03"
    float x = r.X;
    const std::wstring glyph = WeatherGlyph(f.code, f.isDay);
    if (!glyph.empty()) {
        const Font fSym = GlyphFont(10.0f, s);
        PaintText(g, glyph, fSym, theme_->textSecondary, x, r.Y + 36.0f * s);
        x += TextW(g, glyph, fSym) + 2.0f * s;
    }
    std::wstring line = WeatherText(f.code);
    line += (line.empty() ? L"" : L" · ") + HourLabel(f.nowTime) + (f.tzAbbrev.empty() ? L"" : L" " + f.tzAbbrev) + L" there";
    line += L" · updated " + LocalDate(f.fetched);
    if (!std::isnan(f.elevation)) { line += L" · grid cell at " + std::to_wstring(std::lround(f.elevation)) + L" m"; }
    DrawTextClip(g, line, fLine, theme_->textSecondary, RectF(x, r.Y + 38.0f * s, nameW - (x - r.X), 18.0f * s));

    // Today, against the month's 1991–2020 normal when the history is here.
    const size_t d0 = FirstDayFrom(f, UnixNow());
    const bool today = d0 < f.days;
    std::wstring normal = L"normal: needs the history";
    if (today && HasData(i)) {
        const LocalTime& t = f.dTime[d0];
        const Climate& c = (*climates_)[i];
        const double nh = DayNormal(MonthNormals(c, Var::High, kNormal0, kNormal1), t.month, t.day);
        const double nl = DayNormal(MonthNormals(c, Var::Low, kNormal0, kNormal1), t.month, t.day);
        normal = L"normal " + FmtNumber(ToTemp(nh, u), 0, L"") + L" / " + FmtNumber(ToTemp(nl, u), 0, TempUnit(u));
    }
    std::wstring precipV = L"-";
    std::wstring precipD;
    if (today) {
        const bool snow = f.dSnow[d0] >= 0.1;
        precipV = snow ? FmtSnowAmount(f.dSnow[d0], u) : FmtRain(f.dPrecip[d0], u);
        precipD = std::isnan(f.dProb[d0]) ? std::wstring() : L"chance " + FmtNumber(f.dProb[d0], 0, L"%");
    }
    const std::wstring cover = f.snowDepth >= 0.5 ? L"snow cover " + FmtSnow(f.snowDepth, u)
                                                  : L"humidity " + FmtNumber(f.humidity, 0, L"%");
    precipD += (precipD.empty() ? L"" : L" · ") + cover;

    struct Block { std::wstring k, v, d; };
    std::array<Block, 4> b;
    b[0] = { L"Temperature now", FmtTemp(f.temp, u), L"feels like " + FmtTemp(f.feels, u) };
    b[1] = { L"Today high / low",
             today ? FmtNumber(ToTemp(f.dMax[d0], u), 0, L"") + L" / " + FmtNumber(ToTemp(f.dMin[d0], u), 0, TempUnit(u)) : L"-",
             normal };
    b[2] = { L"Wind", FmtWind(f.wind, u),
             std::wstring(L"from ") + CompassPoint(f.windDir) + L", gusts " + FmtWind(f.gust, u) };
    b[3] = { today && f.dSnow[d0] >= 0.1 ? L"Snow today" : L"Precipitation today", precipV, precipD };

    const Font fk = UiFont(8.5f, s);
    const Font fv = UiFont(14.0f, s, FontStyleBold);
    const Font fd = UiFont(8.0f, s);
    Pen sep(theme_->grid, 1.0f);
    for (size_t k = 0; k < 4; ++k) {
        const float bx = r.X + r.Width - blockW * static_cast<float>(4 - k);
        g.DrawLine(&sep, bx, r.Y + 6.0f * s, bx, r.Y + r.Height - 8.0f * s);
        const float tx = bx + 10.0f * s;
        const float tw = blockW - 14.0f * s;
        DrawTextClip(g, b[k].k, fk, theme_->textSecondary, RectF(tx, r.Y + 4.0f * s, tw, 16.0f * s));
        DrawTextClip(g, b[k].v, fv, theme_->text, RectF(tx, r.Y + 19.0f * s, tw, 26.0f * s));
        DrawTextClip(g, b[k].d, fd, theme_->textMuted, RectF(tx, r.Y + 46.0f * s, tw, 16.0f * s));
    }
}

void App::PaintNow(Graphics& g) {
    const size_t i = cfg_.state.selected;
    if (cfg_.locCount == 0) {
        PaintEmpty(g, L"No places yet. Press Add... and search for a town.");
        return;
    }
    if (!HasForecast(i)) {
        const bool busy = fetcher_.IsPending(ForecastKeyFor(cfg_.locs[i]));
        PaintEmpty(g, busy ? L"Fetching the forecast…"
                           : fcErrors_[i].empty() ? L"No forecast yet. Press Update." : L"Forecast failed: " + fcErrors_[i]);
        return;
    }
    const Forecast& f = (*forecasts_)[i];
    PaintHours(g, f);
    PaintDays(g, f, i);
}

// The next 48 hours: temperature and feels-like, with precipitation in the tooltip.
void App::PaintHours(Graphics& g, const Forecast& f) {
    const Units u = cfg_.units;
    const size_t h0 = FirstHourFrom(f, UnixNow());
    const size_t n = f.hours - h0;
    std::array<double, kForecastHours> temp{}, feels{}, prob{}, rain{}, snow{};
    std::array<std::wstring, kForecastHours> heads{}, ticks{};
    for (size_t k = 0; k < n; ++k) {
        const size_t j = h0 + k;
        const LocalTime& t = f.hTime[j];
        temp[k]  = ToTemp(f.hTemp[j], u);
        feels[k] = ToTemp(f.hFeels[j], u);
        prob[k]  = f.hProb[j];
        rain[k]  = ToRain(f.hPrecip[j], u);
        snow[k]  = ToSnow(f.hSnow[j], u);
        const std::wstring what = WeatherText(f.hCode[j]);
        heads[k] = DayLabel(t) + L" " + HourLabel(t) + (what.empty() ? L"" : L" · " + what);
        if (t.hour % 6 == 0) { ticks[k] = t.hour == 0 ? std::wstring(kDayShort[Weekday(t.year, t.month, t.day)]) : HourLabel(t); }
    }
    BandChartInput in;
    in.title = L"Next 48 hours";
    in.subtitle = L"hourly, local time there" + (f.tzAbbrev.empty() ? std::wstring() : L" (" + f.tzAbbrev + L")");
    in.axis = XAxis::Custom;
    in.n = n;
    in.heads = heads.data();
    in.ticks = ticks.data();
    in.unit = TempUnit(u);
    in.decimals = 1;
    in.theme = theme_;
    in.hoverX = hoverX_;
    in.hoverY = hoverY_;
    in.empty = L"This forecast is out of date. Press Update.";
    in.series[0] = { temp.data(), theme_->series2, 2.5f, false, false, true, L"Temperature" };
    in.series[1] = { feels.data(), theme_->series2Soft, 1.5f, false, false, true, L"Feels like" };
    in.series[2] = { prob.data(), theme_->text, 0.0f, false, false, true, L"Chance of precipitation", true, L"%", 0 };
    in.series[3] = { rain.data(), theme_->text, 0.0f, false, false, true, L"Precipitation", true, RainUnit(u), RainDecimals(u) };
    in.series[4] = { snow.data(), theme_->text, 0.0f, false, false, true, L"Snowfall", true, SnowUnit(u), SnowDecimals(u) + 1 };
    in.seriesCount = 5;
    DrawBandChart(g, ToRectF(topRect_), in, scale_);
}

// The next 16 days: high and low against the month's 1991–2020 normal, with
// the day's rain or snow and its weather symbol under the axis.
void App::PaintDays(Graphics& g, const Forecast& f, size_t index) {
    const Units u = cfg_.units;
    const size_t d0 = FirstDayFrom(f, UnixNow());
    const size_t n = f.days - d0;
    std::array<double, 12> normHi{}, normLo{};
    normHi.fill(kNaN);
    normLo.fill(kNaN);
    if (HasData(index)) {
        normHi = MonthNormals((*climates_)[index], Var::High, kNormal0, kNormal1);
        normLo = MonthNormals((*climates_)[index], Var::Low, kNormal0, kNormal1);
    }
    std::array<double, kForecastDays> hi{}, lo{}, nhi{}, nlo{}, rain{}, snow{}, prob{};
    std::array<std::wstring, kForecastDays> heads{}, ticks{}, ticks2{};
    for (size_t k = 0; k < n; ++k) {
        const size_t j = d0 + k;
        const LocalTime& t = f.dTime[j];
        const size_t m = static_cast<size_t>(t.month - 1);
        hi[k]   = ToTemp(f.dMax[j], u);
        lo[k]   = ToTemp(f.dMin[j], u);
        nhi[k]  = ToTemp(DayNormal(normHi, t.month, t.day), u);
        nlo[k]  = ToTemp(DayNormal(normLo, t.month, t.day), u);
        rain[k] = ToRain(f.dPrecip[j], u);
        snow[k] = ToSnow(f.dSnow[j], u);
        prob[k] = f.dProb[j];
        const std::wstring what = WeatherText(f.dCode[j]);
        heads[k] = (k == 0 ? std::wstring(L"Today, ") : std::wstring()) + DayLabel(t) + L" " + kMonthShort[m] +
                   (what.empty() ? L"" : L" · " + what);
        ticks[k] = DayLabel(t);
        // Under the axis: snow if any fell, else rain, one decimal below 10.
        std::wstring amount;
        if (f.dSnow[j] >= 0.1) {
            amount = FmtNumber(snow[k], u == Units::Metric ? (snow[k] < 10.0 ? 1 : 0) : 1, L"");
        } else if (f.dPrecip[j] >= 0.1) {
            amount = FmtNumber(rain[k], u == Units::Metric ? (rain[k] < 10.0 ? 1 : 0) : 2, L"");
        }
        ticks2[k] = std::wstring(WeatherGlyph(f.dCode[j], true)) + (amount.empty() ? L"" : L" " + amount);
    }
    BandChartInput in;
    in.title = L"Next 16 days";
    // Kept short: the legend shares this row.
    in.subtitle = HasData(index) ? std::wstring(L"dashed: 1991–2020 normal · ") : std::wstring();
    in.subtitle += u == Units::Metric ? L"rain mm, snow cm" : L"rain, snow in";
    in.axis = XAxis::Custom;
    in.n = n;
    in.heads = heads.data();
    in.ticks = ticks.data();
    in.ticks2 = ticks2.data();
    in.highlight = n > 0 ? 0 : -1;
    in.unit = TempUnit(u);
    in.decimals = 1;
    in.theme = theme_;
    in.hoverX = hoverX_;
    in.hoverY = hoverY_;
    in.empty = L"This forecast is out of date. Press Update.";
    size_t ns = 0;
    in.series[ns++] = { hi.data(), theme_->series2, 2.5f, false, false, true, L"High" };
    in.series[ns++] = { lo.data(), theme_->series1, 2.5f, false, false, true, L"Low" };
    if (HasData(index)) {
        in.series[ns++] = { nhi.data(), theme_->series2, 1.5f, false, true, true, L"Normal high" };
        in.series[ns++] = { nlo.data(), theme_->series1, 1.5f, false, true, true, L"Normal low" };
    }
    in.series[ns++] = { prob.data(), theme_->text, 0.0f, false, false, true, L"Chance of precipitation", true, L"%", 0 };
    in.series[ns++] = { rain.data(), theme_->text, 0.0f, false, false, true, L"Precipitation", true, RainUnit(u), RainDecimals(u) };
    in.series[ns++] = { snow.data(), theme_->text, 0.0f, false, false, true, L"Snowfall", true, SnowUnit(u), SnowDecimals(u) + 1 };
    assert(ns <= kMaxChartSeries);
    in.seriesCount = ns;
    DrawBandChart(g, ToRectF(bottomRect_), in, scale_);
}

void App::PaintStatus(Graphics& g) {
    const RectF r = ToRectF(statusRect_);
    const Font f = UiFont(8.5f, scale_);
    const wchar_t* credit = NowTab() ? kCreditNow : kCredit;
    const float cw = TextW(g, credit, f);
    PaintText(g, credit, f, theme_->textMuted, r.X + r.Width - cw, r.Y + 2.0f * scale_);
    DrawTextClip(g, status_, f, theme_->textSecondary, RectF(r.X, r.Y + 2.0f * scale_, r.Width - cw - 16.0f * scale_, r.Height));
}

} // namespace wt
