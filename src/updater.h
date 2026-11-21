// The Windows half of Help > Check for updates (see update.h for the rest),
// modelled on BriskMail's src/app/Updater.*. A portable copy unpacks the new
// exe beside itself, swaps the two and starts the new one when this one has
// closed. An installed copy (under Program Files, which it cannot write)
// downloads the new setup instead and runs it when this one has closed;
// Setup asks for elevation and starts WeatherTool again.
#pragma once

#include "update.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace wt::updater {

// GitHub's latest-release address: WEATHERTOOL_UPDATE_URL if set (tests point
// it at a local server), else update::kLatestApi. DownloadBase is what every
// asset link must start with: the GitHub releases address, or the test
// server's own scheme and host.
std::wstring LatestUrl();
std::wstring DownloadBase();

// Learned once, at start: after Swap the running file is WeatherTool.exe.old,
// and what is wanted is still the name it was started as.
std::wstring ExePath();

// Where a download waits: "Update" beside a portable exe; for an installed
// copy a folder in %ProgramData%\WeatherTool named for this Windows user,
// writable by that user, SYSTEM and Administrators only, since the setup in it
// is about to be run elevated.
std::wstring StagingDir(bool installed);

// Asks GitHub for the latest release (worker thread).
bool CheckLatest(update::Release& out, std::wstring& error);

// Downloads this copy's asset of the release into StagingDir. Portable: the
// zip, then unpacks WeatherTool.exe from it. Installed: the setup exe. On
// success `staged` is that program, checked to look like one. `cancel` is
// looked at between steps.
bool Stage(const update::Release& r, bool installed, const std::atomic<bool>& cancel, std::wstring& staged,
           std::wstring& error);

// Portable only: puts newExe in the running exe's place, keeping the running
// one as WeatherTool.exe.old (Windows lets a running program be renamed, not
// overwritten). On failure nothing has moved.
bool Swap(const std::wstring& newExe, std::wstring& error);

// Removes what an update leaves: WeatherTool.exe.old (once the old copy has
// exited) and the staging folder. Does nothing while a downloaded setup
// waits to run.
void CleanUp(bool installed);

// What wWinMain does once the window has closed: start the swapped-in exe,
// or run the downloaded setup.
void RequestRestart(bool on);
void RequestSetup(const std::wstring& setup);   // empty: none
bool AfterExit();   // false when the one requested could not be started

} // namespace wt::updater
