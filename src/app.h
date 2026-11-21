// Main window: places list, view toolbar, header figures, two chart panels
// (temperature above, snowfall below) and a status line.
#pragma once

#include "chart.h"
#include "climate.h"
#include "config.h"
#include "dialogs.h"
#include "fetcher.h"
#include "theme.h"

#include <memory>

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

    // commands
    void OnRemovePlace();
    void OnMovePlace(int delta);
    void OnExportCsv();
    void SelectPlace(size_t index);
    void SetView(int view);

    // data
    void LoadCaches();
    void ScheduleUpdates(bool force);
    bool QueueUpdate(size_t index, bool full);
    std::wstring KeyFor(const Location& loc) const;
    int  FromYear(const Climate& c) const;    // first year shown under the current range
    bool HasData(size_t index) const;

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
    void EnsureBackBuffer(HDC hdc, int w, int h);
    void FreeBackBuffer();

    HINSTANCE hInst_ = nullptr;
    HWND      hwnd_  = nullptr;
    HWND      hList_ = nullptr;
    HWND      hAdd_ = nullptr;
    HWND      hRemove_ = nullptr;
    HWND      hUpdate_ = nullptr;
    HWND      hUnits_ = nullptr;
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

    RECT headerRect_{};
    RECT topRect_{};      // temperature panel
    RECT bottomRect_{};   // snowfall panel
    RECT statusRect_{};

    Config       cfg_;
    std::wstring dataNote_;
    const Theme* theme_ = nullptr;
    Fetcher      fetcher_;
    std::unique_ptr<std::array<Climate, kMaxLocations>>      climates_;
    std::array<std::wstring, kMaxLocations>                  errors_{};
    std::wstring status_;
    float scale_  = 1.0f;
    int   hoverX_ = -1;
    int   hoverY_ = -1;
    bool  tracking_ = false;
};

} // namespace wt
