# WeatherTool

[![Release](https://img.shields.io/github/v/release/Flinterpop/WeatherTool)](https://github.com/Flinterpop/WeatherTool/releases/latest)
[![Release date](https://img.shields.io/github/release-date/Flinterpop/WeatherTool)](https://github.com/Flinterpop/WeatherTool/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Flinterpop/WeatherTool/total)](https://github.com/Flinterpop/WeatherTool/releases)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2B%20x64-0078d4)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599c)

*Last updated: 21 Nov 2026*

A Win32 C++ desktop app that shows **historical temperature and snowfall** for the places you choose, from 1940 to last month, as annual and monthly trends. It follows StockTool's layout: a list of places on the left, view buttons and a header with the headline figures across the top, and two charts (temperature above, snowfall below) with a hover tooltip. Plain Win32, GDI+ and WinHTTP; no frameworks and no third-party libraries.

## Installing

Each [release](https://github.com/Flinterpop/WeatherTool/releases/latest) ships two ways. Both need Windows 10 or later, 64-bit, and nothing else.

- **`WeatherTool-<version>-setup.exe`** installs into Program Files for everyone on the PC (it asks for administrator rights), with a Start menu entry, an optional desktop shortcut and an uninstaller in Apps & Features. Settings and downloaded data are kept in `%ProgramData%\WeatherTool`, which Setup makes writable for every user. Uninstalling deletes the downloaded data but keeps `WeatherTool.ini`, so a reinstall still has your places. `/VERYSILENT` installs without questions.
- **`WeatherTool-<version>-portable.zip`** holds `WeatherTool.exe` and this README: unzip it anywhere and run it. Everything it keeps is in files beside the exe, so moving the folder moves them.

Each release's notes list the SHA-256 of every file.

## Views

| View | Temperature (top) | Snowfall (bottom) |
|---|---|---|
| **Trends** (Ctrl+1) | Annual mean, average daily high or average daily low, with a trailing 10-year mean and a least-squares trend | Total per winter season (July to June, so each winter counts once), 10-season mean and trend |
| **Month** (Ctrl+2) | The chosen month in every year, same three lines | That month's snowfall in every year |
| **Heatmap** (Ctrl+3) | Every month of every year as its anomaly against that month's 1991–2020 average (blue colder, red warmer, capped at ±5 °C) | Every month's snowfall (scale tops out at the 95th percentile) |
| **Climate** (Ctrl+4) | Each month's average over 1961–1990, 1991–2020 and the last 10 complete years | The same periods' monthly snowfall |

- **All years / 50 / 30 / 20 years** limits the Trends, Month and Heatmap views, and the trend figures in the header are computed over that span.
- **Mean / Daily high / Daily low** picks the temperature measure in every view.
- The header shows the temperature change per decade, the last 10 years against 1961–1990, the snowfall change per decade, and the average of the last 10 seasons against 1961–1990.
- Each row in the list shows the last complete year's mean temperature, its difference from the 1991–2020 normal, and the last complete season's snowfall.
- **°F / in** (or View > Imperial) switches units; View > Theme picks system, light or dark.
- File > Export writes the selected place's monthly data as CSV (°C and cm).

## Web server (for a phone or tablet)

**File > Web server** serves a browser version of WeatherTool to other devices on your network, such as an iPad. The browser version is one HTML page with the same views, compiled into the exe. Tick the menu item and the status line shows the address to open, such as `http://192.168.1.10:8765/`; **File > Open web page in browser** opens it on this PC. The tick is remembered, so the server starts with the app next time.

- The port is 8765 unless `WeatherTool.ini` says otherwise (`[server]` `port=`). If another program already uses the port, WeatherTool says so and leaves the server off.
- It serves that page and the icon and nothing else: no files from disk, none of the app's settings or cached data, and only GET and HEAD. The page fetches its own data from Open-Meteo, in the browser that opens it. Each browser remembers its own places, in its local storage.
- Plain HTTP on your local network, so a browser will not offer "use my location" there; search for the town instead. If Windows Firewall is on, Windows asks the first time whether to let WeatherTool accept connections.

## Data

Data comes from the [Open-Meteo](https://open-meteo.com/) historical weather API: ERA5 and ERA5-Land reanalysis from the Copernicus Climate Change Service, licensed CC BY 4.0. Place search uses Open-Meteo's geocoding API. **These are model values for a grid cell of roughly 10 to 25 km, not station readings**: the header shows the cell's elevation, and snowfall in particular can differ from a local gauge.

- A month counts only when at most one day is missing, so the current month and the current year are left out until they are complete.
- The first download for a place fetches every day since 1940 (about 1 MB). After that only the last few months are fetched, re-taking the two latest complete months because ERA5's preliminary values are revised for a while. The app checks for updates at start and every 30 minutes, for places not updated in the last 6 hours; F5 forces it.
- **Rate limits.** Open-Meteo's free tier allows 600 calls a minute, 5,000 an hour and 10,000 a day, and a whole-history download counts as roughly 400 calls (measured 21 Nov 2026). Adding several places at once therefore hits the per-minute limit: the app waits out a short `Retry-After` and tries again, up to three times. When the server asks for a longer wait (the hourly or daily limit), the app stops calling Open-Meteo until then, shows the time it will resume, and the next periodic update picks up from there.

## Where it keeps things

Files only, never the registry:

- A portable copy keeps `WeatherTool.ini` (settings, places, window state) and `cache\` (one CSV of monthly values per place) **beside the exe**.
- A copy under Program Files keeps them in **`%ProgramData%\WeatherTool`**.
- `WEATHERTOOL_DIR` overrides both; the tests and test runs use it so they never touch a real configuration.

The INI is UTF-16 so place names such as Montréal survive. Removing a place deletes its cache file.

## Building and testing

Requires Visual Studio 2026 (MSVC) and CMake 3.25+. No vcpkg packages.

```powershell
cmake --preset default
cmake --build --preset release
build\Release\WeatherTool.exe
build\Release\weathertool_tests.exe          # unit tests (also: ctest --test-dir build -C Release)
build\Release\weathertool_tests.exe --live   # one real download through the app's HTTP client and parser
```

The build uses `/W4 /WX /permissive-` and the static CRT, so the exe needs no VC++ redistributable. The UI-free parts (JSON scanning, climate parsing and statistics, cache format, config, units, themes) are a static library, `weathertool_core`, shared by the app and the tests. `--live` is not part of ctest because it needs the network and spends rate-limit allowance.

## Releasing

```powershell
powershell -ExecutionPolicy Bypass -File tools\release.ps1
```

The script configures, builds and runs the tests. It then checks the exe: it must not need the VC++ runtime, its version resource must match `project(VERSION)` in `CMakeLists.txt`, and it must carry no build-machine paths (the repo is public). Then it writes the following to `dist\`:

- `WeatherTool-<version>-portable.zip`, checked to hold exactly the exe and the README with `/` entry names;
- `WeatherTool-<version>-setup.exe`, built from `installer\WeatherTool.iss` by Inno Setup 6 (`ISCC.exe`, looked for in `C:\bin\InnoSetup6` and then Program Files);
- `WeatherTool-<version>-sha256.md`, for the release notes.

The version lives only in `project(VERSION)`; the README badge reads the latest GitHub release.

`tools\check_installer.ps1 -Setup dist\WeatherTool-<version>-setup.exe`, run from an elevated PowerShell, installs the setup silently and checks the following:

- the installed exe is the built one;
- `%ProgramData%\WeatherTool` is writable by Users, and the program folder is not;
- the shortcut exists;
- the installed copy keeps its settings in ProgramData;
- an uninstall removes the app and the cache but keeps the INI.

It refuses to run over an existing install or data folder, and cleans up after itself.

## Layout of the code

| File | Role |
|---|---|
| `src/main.cpp` | Entry point: DPI awareness, GDI+ start/stop, creates `App`. |
| `src/app.*` | Main window: list, buttons, menus, layout, header figures, the four views, status line, update scheduling. |
| `src/chart.*` | GDI+ renderers: the banded chart (bars and lines per year or month, legend, axes, hover tooltip) and the year-by-month heatmap. |
| `src/climate.*` | Archive reply to monthly grid, merging updates, the cache CSV, and the series and statistics (annual and seasonal series, month normals, trends, rolling means). |
| `src/jsonscan.*` | A small non-recursive JSON scanner for the two Open-Meteo replies. |
| `src/fetcher.*` | Worker thread with a bounded job queue; archive downloads and place searches; rate-limit handling. |
| `src/http.*` | Blocking HTTPS GET on WinHTTP. Copied from StockTool, with three differences: UTF-8 URL encoding, gzip only, and `Retry-After`. |
| `src/dialogs.*` | The Add place dialog (search runs on the worker thread). |
| `src/webserver.*` | File > Web server: a small Winsock HTTP server (IPv6 and IPv4, one connection at a time) that serves the compiled-in page and icon, and the LAN addresses to show. |
| `web/index.html` | The browser version: one self-contained page (plain JavaScript and SVG), compiled into the exe as a resource. |
| `src/config.*` | Data folder resolution and `WeatherTool.ini`. |
| `src/units.*` | Metric and imperial conversion and number formatting. |
| `src/theme.*` | Light and dark palettes and the heatmap colour ramps. |
| `src/WeatherTool.rc`, `src/resource.h`, `src/WeatherTool.ico` | Version resource, app icon and the Add place dialog. |
| `tools/make_icon.py` | Regenerates the multi-size `.ico` with Pillow: `python tools/make_icon.py`. |
| `installer/WeatherTool.iss` | Inno Setup script: machine-level install, `%ProgramData%\WeatherTool` with Users modify. |
| `tools/release.ps1`, `tools/check_installer.ps1` | Build-test-package, and the elevated end-to-end installer check. |
| `tests/test_main.cpp` | Unit tests, plus the `--live` check. |

**Why gzip only:** offered deflate, Open-Meteo sends it, and WinHTTP aborts the read with "Operation aborted". This was confirmed both ways on 21 Nov 2026 with the same small request: it fails with deflate allowed and succeeds with gzip only.
