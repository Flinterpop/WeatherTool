// WeatherTool.ini: settings, saved places and window state. Files only,
// never the registry. The data folder is the exe's own folder for a
// portable copy, %ProgramData%\WeatherTool for a copy under Program Files,
// or WEATHERTOOL_DIR when set (tests use that).
#pragma once

#include "common.h"
#include "units.h"

#include <windows.h>

namespace wt {

enum class ThemeMode { System, Light, Dark };

enum class View : int { Trends = 0, Month = 1, Heatmap = 2, Climate = 3 };
constexpr int kViewCount = 4;

// Range presets: how many years the trend views show (0 = everything).
constexpr std::array<int, 4> kRangeYears = { 0, 50, 30, 20 };

struct ViewState {
    size_t selected  = 0;
    int    view      = 0;     // View
    int    month     = 0;     // 0..11, for View::Month
    int    measure   = 0;     // Var::Mean / High / Low
    int    range     = 0;     // index into kRangeYears
    bool   hasPlacement = false;
    RECT   placement{};       // normal (restored) window rectangle
    bool   maximized = false;
};

struct Config {
    std::wstring dir;        // data folder
    std::wstring path;       // dir\WeatherTool.ini
    std::wstring cacheDir;   // dir\cache
    Units        units = Units::Metric;
    ThemeMode    theme = ThemeMode::System;
    std::array<Location, kMaxLocations> locs{};
    size_t       locCount = 0;
    ViewState    state;
};

// Picks the data folder as described above and creates it (and cache\).
// `note` explains a fallback, for the status line.
bool ResolveDataDir(std::wstring& dir, std::wstring& note, std::wstring& err);

bool LoadConfig(const std::wstring& dir, Config& cfg, std::wstring& err);
bool SaveSettings(const Config& cfg, std::wstring& err);
bool SaveLocations(const Config& cfg, std::wstring& err);
bool SaveState(const Config& cfg, std::wstring& err);

// "Ottawa, Ontario, CA|45.41117|-75.69812"
bool         ParseLocation(const std::wstring& line, Location& out);
std::wstring FormatLocation(const Location& loc);

// Cache file for a place: cacheDir\<lat>_<lon>.csv
std::wstring CacheFileFor(const Config& cfg, const Location& loc);

// Whole-file helpers (binary).
bool ReadFileBytes(const std::wstring& path, std::string& out, size_t maxBytes, std::wstring& err);
bool WriteFileBytes(const std::wstring& path, const std::string& bytes, std::wstring& err);   // via temp + rename

} // namespace wt
