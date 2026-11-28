cmake_minimum_required(VERSION 3.25)
# Single source of truth for the version: it flows into the code
# (WEATHERTOOL_VERSION*) and the exe's VERSIONINFO resource from here.
project(WeatherTool VERSION 0.9.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the exe needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WT_DEFINES
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    $<$<COMPILE_LANGUAGE:CXX>:WEATHERTOOL_VERSION="${PROJECT_VERSION}">
    WEATHERTOOL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WEATHERTOOL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WEATHERTOOL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Power of 10: strictest warnings, treated as errors.
set(WT_OPTIONS /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)

# ---- UI-free core: parsing, climate statistics, config, formatting ---------
add_library(weathertool_core STATIC
    src/climate.cpp
    src/config.cpp
    src/forecast.cpp
    src/http.cpp
    src/jsonscan.cpp
    src/radar.cpp
    src/shapes.cpp
    src/slippy.cpp
    src/theme.cpp
    src/units.cpp
    src/update.cpp
    src/webserver.cpp
)
target_include_directories(weathertool_core PUBLIC src)
target_compile_definitions(weathertool_core PUBLIC ${WT_DEFINES})
target_compile_options(weathertool_core PRIVATE ${WT_OPTIONS})
target_link_libraries(weathertool_core PUBLIC winhttp gdiplus shell32 advapi32 ole32 ws2_32 iphlpapi)

# ---- Application -----------------------------------------------------------
add_executable(WeatherTool WIN32
    src/main.cpp
    src/app.cpp
    src/chart.cpp
    src/dialogs.cpp
    src/fetcher.cpp
    src/tiles.cpp
    src/updater.cpp
    src/WeatherTool.rc
)
target_compile_options(WeatherTool PRIVATE ${WT_OPTIONS})
target_link_libraries(WeatherTool PRIVATE weathertool_core comctl32 comdlg32 dwmapi uxtheme shlwapi)

# ---- Tests (no framework: one exe, non-zero exit on failure) ---------------
enable_testing()
add_executable(weathertool_tests tests/test_main.cpp)
target_compile_options(weathertool_tests PRIVATE ${WT_OPTIONS})
target_link_libraries(weathertool_tests PRIVATE weathertool_core)
add_test(NAME weathertool_tests COMMAND weathertool_tests)
