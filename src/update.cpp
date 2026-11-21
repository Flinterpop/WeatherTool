#include "update.h"

#include "jsonscan.h"

#include <cassert>

namespace wt::update {
namespace {

constexpr int    kMaxPart   = 9999;
constexpr size_t kMaxAssets = 100;
constexpr size_t kMaxNotes  = 64u * 1024u;

// One number of a version, up to the next '.' or the end; false on anything but digits.
bool ReadPart(std::wstring_view s, size_t& at, int& out) {
    const size_t start = at;
    out = 0;
    while (at < s.size() && s[at] >= L'0' && s[at] <= L'9' && at - start < 5) {
        out = out * 10 + (s[at] - L'0');
        ++at;
    }
    return at > start && out <= kMaxPart;
}

bool StringAt(std::string_view j, size_t from, size_t to, const char* key, std::wstring& out, size_t maxBytes = 4096) {
    size_t p = json::FindKey(j, key, from, to);
    return p != json::npos && json::ReadString(j, p, out, maxBytes);
}

bool BoolAt(std::string_view j, const char* key) {
    size_t p = json::FindKey(j, key, 0, j.size());
    bool v = false;
    return p != json::npos && json::ReadBool(j, p, v) && v;
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    return !prefix.empty() && s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

bool ParseVersion(std::wstring_view s, Version& out) {
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) { s.remove_prefix(1); }
    size_t at = 0;
    Version v;
    if (!ReadPart(s, at, v.major) || at >= s.size() || s[at++] != L'.') { return false; }
    if (!ReadPart(s, at, v.minor) || at >= s.size() || s[at++] != L'.') { return false; }
    if (!ReadPart(s, at, v.patch) || at != s.size()) { return false; }
    out = v;
    return true;
}

bool Newer(const Version& c, const Version& cur) {
    if (c.major != cur.major) { return c.major > cur.major; }
    if (c.minor != cur.minor) { return c.minor > cur.minor; }
    return c.patch > cur.patch;
}

std::wstring VersionText(const Version& v) {
    return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
}

std::wstring ZipName(const Version& v)   { return L"WeatherTool-" + VersionText(v) + L"-portable.zip"; }
std::wstring SetupName(const Version& v) { return L"WeatherTool-" + VersionText(v) + L"-setup.exe"; }

bool ParseLatest(std::string_view j, const std::wstring& downloadBase, Release& out, std::wstring& error) {
    assert(!downloadBase.empty());
    Release r;
    if (!StringAt(j, 0, j.size(), "tag_name", r.tag)) {
        // GitHub explains a refusal (rate limit, not found) in "message".
        std::wstring why;
        error = StringAt(j, 0, j.size(), "message", why) ? L"GitHub answered: " + why
                                                          : std::wstring(L"GitHub's answer was not a release.");
        return false;
    }
    if (BoolAt(j, "draft") || BoolAt(j, "prerelease")) {
        error = L"The latest release is not a finished one.";
        return false;
    }
    if (!ParseVersion(r.tag, r.version)) {
        error = L"The latest release's tag (" + r.tag + L") is not a version number.";
        return false;
    }
    const std::wstring zip = ZipName(r.version), setup = SetupName(r.version);
    const size_t at = json::FindKey(j, "assets", 0, j.size());
    if (at != json::npos) {
        json::ArrayReader assets(j, at);
        size_t el = 0;
        for (size_t i = 0; i < kMaxAssets && assets.NextElement(el); ++i) {
            const size_t end = json::SkipContainer(j, el);
            if (end == json::npos) { break; }
            std::wstring name, url;
            if (!StringAt(j, el, end, "name", name) || !StringAt(j, el, end, "browser_download_url", url)) { continue; }
            if (!StartsWith(url, downloadBase)) { continue; }   // only from this repository's releases
            if (name == zip) { r.zipUrl = url; }
            if (name == setup) { r.setupUrl = url; }
        }
    }
    if (r.zipUrl.empty() && r.setupUrl.empty()) {
        error = L"The latest release (" + r.tag + L") has neither " + zip + L" nor " + setup + L".";
        return false;
    }
    (void)StringAt(j, 0, j.size(), "body", r.notes, kMaxNotes);   // no notes is fine
    assert(!r.tag.empty());
    out = r;
    return true;
}

std::wstring AssetFor(const Release& r, bool installed, std::wstring& error) {
    const std::wstring& url = installed ? r.setupUrl : r.zipUrl;
    if (url.empty()) {
        error = L"The latest release (" + r.tag + L") has no " +
                (installed ? SetupName(r.version) + L", which an installed copy updates from."
                           : ZipName(r.version) + L", which a portable copy updates from.");
    }
    return url;
}

std::wstring ShortNotes(std::wstring_view notes, size_t maxChars) {
    assert(maxChars > 3);
    std::wstring s(notes.substr(0, maxChars));
    if (notes.size() <= maxChars) { return s; }
    const size_t line = s.find_last_of(L'\n');
    if (line != std::wstring::npos && line > maxChars / 2) { s.resize(line); }
    if (!s.empty() && s.back() >= 0xD800 && s.back() < 0xDC00) { s.pop_back(); }   // half a pair
    return s + L"...";
}

// /SILENT shows Setup's progress, so the update is seen happening; /ALLUSERS
// is the house rule for a self-updater (the installer is machine-level only,
// so it changes nothing, but never per user); /WEATHERTOOLRESTART=1 has
// installer/WeatherTool.iss start WeatherTool again when it is done.
std::wstring SetupArgs() {
    return L"/SILENT /SUPPRESSMSGBOXES /NORESTART /ALLUSERS /WEATHERTOOLRESTART=1";
}

} // namespace wt::update
