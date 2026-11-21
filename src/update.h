// Help > Check for updates: what the latest GitHub release is, and whether it
// is newer than this build. Modelled on BriskMail's src/core/Update.h, with
// one difference: WeatherTool's repository is public, so the app asks
// GitHub's API directly over WinHTTP instead of through the GitHub CLI, and
// downloads the asset from its public link. This half is UI-free and only
// reads GitHub's answers; src/updater.* puts a release in place.
#pragma once

#include <string>
#include <string_view>

namespace wt::update {

constexpr wchar_t kLatestApi[]    = L"https://api.github.com/repos/Flinterpop/WeatherTool/releases/latest";
// Every asset link must start with this; anything else is refused.
constexpr wchar_t kDownloadBase[] = L"https://github.com/Flinterpop/WeatherTool/releases/download/";

struct Version {
    int major = 0, minor = 0, patch = 0;
};
// "0.4.0" or "v0.4.0"; false for anything else (a suffix such as "-rc1" included).
bool ParseVersion(std::wstring_view s, Version& out);
bool Newer(const Version& candidate, const Version& current);
std::wstring VersionText(const Version& v);   // "0.4.0"

struct Release {
    std::wstring tag;        // "v0.5.0"
    Version      version;
    std::wstring zipUrl;     // WeatherTool-0.5.0-portable.zip's download link, if the release has it
    std::wstring setupUrl;   // WeatherTool-0.5.0-setup.exe's, if it has it
    std::wstring notes;      // the release's description, as written
};
// The asset names a release of this version carries.
std::wstring ZipName(const Version& v);
std::wstring SetupName(const Version& v);
// GitHub's answer for the latest release. False, with why, when it is not one
// this app can install: not a release, a draft or prerelease, a tag that is
// not a version, or neither asset. An asset whose link does not start with
// `downloadBase` (kDownloadBase, or a test server's) is ignored.
bool ParseLatest(std::string_view json, const std::wstring& downloadBase, Release& out, std::wstring& error);
// The link this copy updates from: the setup for an installed copy, the zip
// for a portable one. Empty, with why, when the release lacks it.
std::wstring AssetFor(const Release& r, bool installed, std::wstring& error);

// The notes cut to fit a message box: at most maxChars, ending at a line
// where possible, never between the halves of a surrogate pair.
std::wstring ShortNotes(std::wstring_view notes, size_t maxChars);

// What an installed copy passes the new setup: silent, machine-wide, and
// WeatherTool started again when it is done (installer/WeatherTool.iss).
std::wstring SetupArgs();

} // namespace wt::update
