#include "config.h"

#include <shlobj.h>

#include <cassert>
#include <cmath>
#include <cwchar>
#include <memory>

namespace wt {
namespace {

constexpr wchar_t kIniName[] = L"WeatherTool.ini";
constexpr DWORD   kSectionChars = 32768;

std::wstring ExeDir() {
    std::array<wchar_t, MAX_PATH * 2> buf{};
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return L"."; }
    std::wstring p(buf.data(), n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw != nullptr) { out = raw; }
    CoTaskMemFree(raw);
    return out;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.empty() || s.size() < prefix.size()) { return false; }
    if (_wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) != 0) { return false; }
    return s.size() == prefix.size() || s[prefix.size()] == L'\\';
}

bool EnsureDir(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr)) { return true; }
    const DWORD e = GetLastError();
    const DWORD attr = GetFileAttributesW(dir.c_str());
    return e == ERROR_ALREADY_EXISTS && attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// The profile API reads and writes Unicode only when the file starts with
// a UTF-16LE BOM, so the file is created that way (place names such as
// Montréal then survive).
bool EnsureUnicodeIni(const std::wstring& path, std::wstring& err) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) { return true; }
    const std::string bom = "\xFF\xFE";
    return WriteFileBytes(path, bom, err);
}

std::wstring ReadStr(const Config& c, const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    std::array<wchar_t, 512> b{};
    GetPrivateProfileStringW(sec, key, def, b.data(), static_cast<DWORD>(b.size()), c.path.c_str());
    return b.data();
}

int ReadInt(const Config& c, const wchar_t* sec, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(sec, key, def, c.path.c_str()));
}

bool WriteStr(const Config& c, const wchar_t* sec, const wchar_t* key, const std::wstring& v, std::wstring& err) {
    if (!WritePrivateProfileStringW(sec, key, v.c_str(), c.path.c_str())) {
        err = L"Could not write " + c.path + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

bool ResolveDataDir(std::wstring& dir, std::wstring& note, std::wstring& err) {
    note.clear();
    std::array<wchar_t, MAX_PATH> env{};
    const DWORD n = GetEnvironmentVariableW(L"WEATHERTOOL_DIR", env.data(), static_cast<DWORD>(env.size()));
    if (n > 0 && n < env.size()) {
        dir = env.data();
        note = L"data folder from WEATHERTOOL_DIR";
    } else {
        const std::wstring exe = ExeDir();
        const bool installed = StartsWithI(exe, KnownFolder(FOLDERID_ProgramFiles)) ||
                               StartsWithI(exe, KnownFolder(FOLDERID_ProgramFilesX86));
        if (installed) {
            const std::wstring pd = KnownFolder(FOLDERID_ProgramData);
            dir = pd.empty() ? exe : pd + L"\\WeatherTool";
        } else {
            dir = exe;
        }
    }
    if (!EnsureDir(dir)) {
        err = L"Cannot create data folder " + dir;
        return false;
    }
    if (!EnsureDir(dir + L"\\cache")) {
        err = L"Cannot create cache folder in " + dir;
        return false;
    }
    return true;
}

bool ParseLocation(const std::wstring& line, Location& out) {
    const size_t a = line.find(L'|');
    const size_t b = (a == std::wstring::npos) ? a : line.find(L'|', a + 1);
    if (a == std::wstring::npos || b == std::wstring::npos || a == 0) { return false; }
    wchar_t* end = nullptr;
    const std::wstring latS = line.substr(a + 1, b - a - 1);
    const std::wstring lonS = line.substr(b + 1);
    const double lat = std::wcstod(latS.c_str(), &end);
    if (end == latS.c_str()) { return false; }
    const double lon = std::wcstod(lonS.c_str(), &end);
    if (end == lonS.c_str()) { return false; }
    if (!(std::fabs(lat) <= 90.0) || !(std::fabs(lon) <= 180.0)) { return false; }
    out.name = line.substr(0, a);
    out.lat  = lat;
    out.lon  = lon;
    return true;
}

std::wstring FormatLocation(const Location& loc) {
    assert(loc.name.find(L'|') == std::wstring::npos);
    std::array<wchar_t, 64> b{};
    swprintf_s(b.data(), b.size(), L"|%.5f|%.5f", loc.lat, loc.lon);
    return loc.name + b.data();
}

std::wstring CacheFileFor(const Config& cfg, const Location& loc) {
    std::array<wchar_t, 64> b{};
    swprintf_s(b.data(), b.size(), L"\\%.4f_%.4f.csv", loc.lat, loc.lon);
    return cfg.cacheDir + b.data();
}

bool LoadConfig(const std::wstring& dir, Config& cfg, std::wstring& err) {
    assert(!dir.empty());
    cfg = Config{};
    cfg.dir      = dir;
    cfg.path     = dir + L"\\" + kIniName;
    cfg.cacheDir = dir + L"\\cache";
    if (!EnsureUnicodeIni(cfg.path, err)) { return false; }

    cfg.units = (ReadStr(cfg, L"settings", L"units", L"metric") == L"imperial") ? Units::Imperial : Units::Metric;
    const std::wstring th = ReadStr(cfg, L"settings", L"theme", L"system");
    cfg.theme = (th == L"light") ? ThemeMode::Light : (th == L"dark") ? ThemeMode::Dark : ThemeMode::System;

    auto buf = std::make_unique<wchar_t[]>(kSectionChars);
    const DWORD got = GetPrivateProfileSectionW(L"locations", buf.get(), kSectionChars, cfg.path.c_str());
    for (DWORD p = 0; p < got && buf[p] != L'\0' && cfg.locCount < kMaxLocations;) {
        const std::wstring entry(buf.get() + p);
        p += static_cast<DWORD>(entry.size()) + 1;
        const size_t eq = entry.find(L'=');
        Location loc;
        if (eq != std::wstring::npos && ParseLocation(entry.substr(eq + 1), loc)) { cfg.locs[cfg.locCount++] = loc; }
    }

    ViewState& s = cfg.state;
    s.selected = static_cast<size_t>(Clamp(ReadInt(cfg, L"state", L"selected", 0), 0, kMaxLocations - 1));
    s.view     = Clamp(ReadInt(cfg, L"state", L"view", 0), 0, kViewCount - 1);
    s.month    = Clamp(ReadInt(cfg, L"state", L"month", 0), 0, 11);
    s.measure  = Clamp(ReadInt(cfg, L"state", L"measure", 0), 0, 2);
    s.range    = Clamp(ReadInt(cfg, L"state", L"range", 0), 0, static_cast<int>(kRangeYears.size()) - 1);
    s.maximized = ReadInt(cfg, L"state", L"maximized", 0) != 0;
    const int w = ReadInt(cfg, L"state", L"w", 0);
    const int h = ReadInt(cfg, L"state", L"h", 0);
    if (w >= 400 && h >= 300) {
        s.hasPlacement = true;
        s.placement.left   = ReadInt(cfg, L"state", L"x", 0);
        s.placement.top    = ReadInt(cfg, L"state", L"y", 0);
        s.placement.right  = s.placement.left + w;
        s.placement.bottom = s.placement.top + h;
    }
    return true;
}

bool SaveSettings(const Config& cfg, std::wstring& err) {
    const wchar_t* th = cfg.theme == ThemeMode::Light ? L"light" : cfg.theme == ThemeMode::Dark ? L"dark" : L"system";
    return WriteStr(cfg, L"settings", L"units", cfg.units == Units::Imperial ? L"imperial" : L"metric", err) &&
           WriteStr(cfg, L"settings", L"theme", th, err);
}

bool SaveLocations(const Config& cfg, std::wstring& err) {
    std::wstring section;
    for (size_t i = 0; i < cfg.locCount; ++i) {
        section += std::to_wstring(i + 1) + L"=" + FormatLocation(cfg.locs[i]);
        section += L'\0';
    }
    section += L'\0';
    if (!WritePrivateProfileSectionW(L"locations", section.c_str(), cfg.path.c_str())) {
        err = L"Could not write " + cfg.path;
        return false;
    }
    return true;
}

bool SaveState(const Config& cfg, std::wstring& err) {
    const ViewState& s = cfg.state;
    bool ok = WriteStr(cfg, L"state", L"selected", std::to_wstring(s.selected), err) &&
              WriteStr(cfg, L"state", L"view", std::to_wstring(s.view), err) &&
              WriteStr(cfg, L"state", L"month", std::to_wstring(s.month), err) &&
              WriteStr(cfg, L"state", L"measure", std::to_wstring(s.measure), err) &&
              WriteStr(cfg, L"state", L"range", std::to_wstring(s.range), err) &&
              WriteStr(cfg, L"state", L"maximized", s.maximized ? L"1" : L"0", err);
    if (ok && s.hasPlacement) {
        ok = WriteStr(cfg, L"state", L"x", std::to_wstring(s.placement.left), err) &&
             WriteStr(cfg, L"state", L"y", std::to_wstring(s.placement.top), err) &&
             WriteStr(cfg, L"state", L"w", std::to_wstring(s.placement.right - s.placement.left), err) &&
             WriteStr(cfg, L"state", L"h", std::to_wstring(s.placement.bottom - s.placement.top), err);
    }
    return ok;
}

bool ReadFileBytes(const std::wstring& path, std::string& out, size_t maxBytes, std::wstring& err) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"Cannot open " + path;
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= maxBytes;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    if (!ok) { err = L"Cannot read " + path; }
    return ok;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes, std::wstring& err) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = L"Cannot write " + tmp;
        return false;
    }
    DWORD put = 0;
    const bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    const bool closed = CloseHandle(f) != 0;
    if (!ok || !closed || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = L"Cannot write " + path;
        return false;
    }
    return true;
}

} // namespace wt
