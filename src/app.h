// Main window: places list, History / Now tabs, view toolbar, header
// figures, two chart panels (History: temperature above, snowfall below;
// Now: the next 48 hours above, the next 16 days below) and a status line.
#pragma once

#include "chart.h"
#include "climate.h"
#include "config.h"
#include "dialogs.h"
#include "fetcher.h"
#include "update.h"
#include "webserver.h"
#include "theme.h"

#include <atomic>
#include <memory>
#include <thread>

namespace wt {

class App final : public PlaceSink {
public:
    App() = default;
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Create(HINSTANCE hInst, int nCmdShow, std::wstring& err);
    int  Run();

    bool AddPlace(const Location& loc, std::wstring& err) override;   // from the Add dialog

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // messages
    void OnCreate();
    void OnClose();
    void OnCommand(int id, int code);
    void OnPaint();
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnDpiChanged(WPARAM wp, LPARAM lp);
    void OnMeasureItem(MEASUREITEMSTRUCT* mis) const;
    void OnDrawItem(const DRAWITEMSTRUCT* dis);
    void OnArchiveReady();
    void OnForecastReady();
    void OnUpdateResult(LPARAM lp);

    // Help > Check for updates (update.h, updater.h): a check on a worker
    // thread, then, if the user agrees, a download on another.
    void CheckForUpdates();
    void StopUpdateThread();

    // commands
    void OnRemovePlace();
    void OnMovePlace(int delta);
    void OnExportCsv();
    bool StartWebServer(bool interactive);   // false (and says why) when it cannot listen
    void StopWebServer();
    void OnToggleWebServer();
    std::wstring WebUrls() const;            // "http://192.168.1.10:8765/" etc.
    void SelectPlace(size_t index);
    void SetView(int view);
    void SetTab(int tab);
    bool NowTab() const { return cfg_.state.tab == static_cast<int>(Tab::Now); }

    // data
    void LoadCaches();
    void ScheduleUpdates(bool force);
    bool QueueUpdate(size_t index, bool full);
    void ScheduleForecasts(bool force);
    std::wstring KeyFor(const Location& loc) const;
    std::wstring ForecastKeyFor(const Location& loc) const;
    int  FromYear(const Climate& c) const;    // first year shown under the current range
    bool HasData(size_t index) const;
    bool HasForecast(size_t index) const;

    // ui
    void CreateControls();
    void CreateFonts();
    void Layout();
    void SyncControls();
    void ApplyTheme();
    void RebuildList();
    void SetStatus(const std::wstring& text);
    int  Px(int dip) const;

    // painting
    void PaintHeader(Gdiplus::Graphics& g);
    void PaintPanels(Gdiplus::Graphics& g);
    void PaintStatus(Gdiplus::Graphics& g);
    void PaintTrends(Gdiplus::Graphics& g, const Climate& c, bool monthly);
    void PaintHeatmaps(Gdiplus::Graphics& g, const Climate& c);
    void PaintClimate(Gdiplus::Graphics& g, const Climate& c);
    void PaintEmpty(Gdiplus::Graphics& g, const std::wstring& text);
    void PaintNowHeader(Gdiplus::Graphics& g);
    void PaintNow(Gdiplus::Graphics& g);
    void PaintHours(Gdiplus::Graphics& g, const Forecast& f);
    void PaintDays(Gdiplus::Graphics& g, const Forecast& f, size_t index);
    void PaintTabLine(Gdiplus::Graphics& g);
    void DrawTab(const DRAWITEMSTRUCT* dis);
    void DrawListRow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, size_t index);
    void DrawListRowNow(Gdiplus::Graphics& g, const Gdiplus::RectF& r, size_t index);
    void EnsureBackBuffer(HDC hdc, int w, int h);
    void FreeBackBuffer();

    HINSTANCE hInst_ = nullptr;
    HWND      hwnd_  = nullptr;
    HWND      hList_ = nullptr;
    HWND      hAdd_ = nullptr;
    HWND      hRemove_ = nullptr;
    HWND      hUpdate_ = nullptr;
    HWND      hUnits_ = nullptr;
    std::array<HWND, kTabCount> hTabs_{};
    std::array<HWND, kViewCount> hViews_{};
    std::array<HWND, kRangeYears.size()> hRanges_{};
    std::array<HWND, 3> hMeasures_{};
    std::array<HWND, 12> hMonths_{};
    HMENU     hMenu_ = nullptr;
    HACCEL    hAccel_ = nullptr;
    HFONT     hUiFont_ = nullptr;
    HBRUSH    hBgBrush_ = nullptr;
    HBRUSH    hListBrush_ = nullptr;

    HDC     memDC_  = nullptr;
    HBITMAP memBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int     bufW_ = 0;
    int     bufH_ = 0;

    RECT tabLine_{};      // the rule under the History / Now tabs
    RECT headerRect_{};
    RECT topRect_{};      // temperature panel
    RECT bottomRect_{};   // snowfall panel
    RECT statusRect_{};

    Config       cfg_;
    std::wstring dataNote_;
    const Theme* theme_ = nullptr;
    Fetcher      fetcher_;
    WebServer    web_;
    WebContent   webContent_;   // views into the exe's resources
    std::unique_ptr<std::array<Climate, kMaxLocations>>      climates_;
    std::array<std::wstring, kMaxLocations>                  errors_{};
    std::unique_ptr<std::array<Forecast, kMaxLocations>>     forecasts_;
    std::array<std::wstring, kMaxLocations>                  fcErrors_{};
    std::wstring status_;
    std::thread       updateThread_;
    std::atomic<bool> updateBusy_{ false };
    std::atomic<bool> updateCancel_{ false };
    float scale_  = 1.0f;
    int   hoverX_ = -1;
    int   hoverY_ = -1;
    bool  tracking_ = false;
};

} // namespace wt
