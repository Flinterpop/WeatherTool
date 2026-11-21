#include "updater.h"

#include "common.h"
#include "config.h"
#include "http.h"

#include <sddl.h>
#include <shellapi.h>

#include <array>
#include <cassert>
#include <memory>

namespace wt::updater {
namespace {

bool g_restart = false;
std::wstring g_setup;

constexpr size_t kMinExe = 100u * 1024u;
constexpr size_t kMaxDownload = 32u * 1024u * 1024u;   // the setup is ~2.4 MB
constexpr size_t kMaxApiReply = 1024u * 1024u;
constexpr int    kMaxStagedFiles = 64;
constexpr DWORD  kUnpackTimeoutMs = 60000;

std::wstring Env(const wchar_t* name) {
    std::array<wchar_t, 2048> buf{};
    const DWORD n = GetEnvironmentVariableW(name, buf.data(), static_cast<DWORD>(buf.size()));
    return n > 0 && n < buf.size() ? std::wstring(buf.data(), n) : std::wstring();
}

bool IsFile(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring Dir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

std::wstring Quote(const std::wstring& a) { return L"\"" + a + L"\""; }   // paths only: no quotes inside

// This Windows user's SID as text ("S-1-5-21-..."); empty if it cannot be read.
std::wstring UserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { return {}; }
    std::array<char, 256> buf{};
    DWORD got = 0;
    const BOOL ok = GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &got);
    CloseHandle(token);
    wchar_t* text = nullptr;
    if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) { return {}; }
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}

// Removes the files in dir (not below it) and then dir, if it is empty.
void RemoveStaged(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) { return; }
    for (int i = 0; i < kMaxStagedFiles; ++i) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); }
        if (!FindNextFileW(find, &fd)) { break; }
    }
    FindClose(find);
    RemoveDirectoryW(dir.c_str());
}

// A new, empty staging folder. For an installed copy it is writable by this
// user, SYSTEM and Administrators only (no inherited Users access), so no
// other account can replace the setup between the download and the
// elevated run.
bool MakeStagingDir(const std::wstring& dir, bool installed, std::wstring& error) {
    RemoveStaged(dir);   // a download left from an earlier try
    BOOL made = FALSE;
    if (!installed) {
        made = CreateDirectoryW(dir.c_str(), nullptr);
    } else {
        const std::wstring sid = UserSid();
        PSECURITY_DESCRIPTOR sd = nullptr;
        const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + sid + L")";
        if (!sid.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
            made = CreateDirectoryW(dir.c_str(), &sa);
            LocalFree(sd);
        }
    }
    if (!made) {
        error = L"WeatherTool cannot make " + dir + L" (error " + std::to_wstring(GetLastError()) +
                L"), so it cannot update this copy. Download the new release from GitHub instead.";
    }
    return made != FALSE;
}

// The file is there, of a plausible size, and starts like a Windows program.
bool LooksLikeProgram(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) { return false; }
    const unsigned long long size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { return false; }
    char mz[2] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(f, mz, 2, &read, nullptr);
    CloseHandle(f);
    return ok && read == 2 && mz[0] == 'M' && mz[1] == 'Z' && size >= kMinExe && size <= kMaxDownload;
}

// One GET into memory; false, with why, unless the answer is 200.
bool Fetch(const std::wstring& url, size_t cap, std::unique_ptr<char[]>& buf, size_t& len, std::wstring& error) {
    assert(cap > 0);
    buf = std::make_unique<char[]>(cap);
    HttpClient http;
    HttpResult hr;
    if (!http.Get(url, buf.get(), cap, hr, error)) {
        error = L"GitHub could not be reached: " + error;
        return false;
    }
    len = hr.length;
    if (hr.status != 200) {
        error = L"GitHub answered HTTP " + std::to_wstring(hr.status) +
                (hr.status == 403 || hr.status == 429 ? L" (too many requests from this network; try again in an hour)." : L".");
        return false;
    }
    return true;
}

// Runs a program with no window and waits for it; its exit code, or -1.
int RunHidden(const std::wstring& exe, std::wstring args, DWORD timeoutMs) {
    std::wstring cmd = Quote(exe) + L" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

} // namespace

std::wstring LatestUrl() {
    const std::wstring over = Env(L"WEATHERTOOL_UPDATE_URL");
    return over.empty() ? std::wstring(update::kLatestApi) : over;
}

std::wstring DownloadBase() {
    const std::wstring over = Env(L"WEATHERTOOL_UPDATE_URL");
    if (over.empty()) { return update::kDownloadBase; }
    const size_t host = over.find(L"://");
    const size_t path = host == std::wstring::npos ? std::wstring::npos : over.find(L'/', host + 3);
    return path == std::wstring::npos ? over + L"/" : over.substr(0, path + 1);
}

std::wstring ExePath() {
    static const std::wstring path = [] {
        std::array<wchar_t, MAX_PATH * 4> buf{};
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        return n > 0 && n < buf.size() ? std::wstring(buf.data(), n) : std::wstring();
    }();
    return path;
}

std::wstring StagingDir(bool installed) {
    if (!installed) { return Dir(ExePath()) + L"\\Update"; }
    const std::wstring sid = UserSid();
    return ProgramDataFolder() + L"\\WeatherTool\\Update-" + (sid.empty() ? std::wstring(L"unknown") : sid);
}

bool CheckLatest(update::Release& out, std::wstring& error) {
    std::unique_ptr<char[]> buf;
    size_t len = 0;
    if (!Fetch(LatestUrl(), kMaxApiReply, buf, len, error)) { return false; }
    return update::ParseLatest(std::string_view(buf.get(), len), DownloadBase(), out, error);
}

bool Stage(const update::Release& r, bool installed, const std::atomic<bool>& cancel, std::wstring& staged,
           std::wstring& error) {
    const std::wstring url = update::AssetFor(r, installed, error);
    if (url.empty()) { return false; }
    const std::wstring name = installed ? update::SetupName(r.version) : update::ZipName(r.version);
    const std::wstring dir = StagingDir(installed);
    if (!MakeStagingDir(dir, installed, error)) { return false; }
    std::unique_ptr<char[]> buf;
    size_t len = 0;
    if (!Fetch(url, kMaxDownload, buf, len, error)) { return false; }
    if (cancel.load()) {
        error = L"Cancelled.";
        return false;
    }
    const std::wstring file = dir + L"\\" + name;
    if (!WriteFileBytes(file, std::string(buf.get(), len), error)) { return false; }
    if (installed) {
        staged = file;
        if (!LooksLikeProgram(staged)) {
            error = name + L" does not look like a program; nothing was changed.";
            return false;
        }
        return true;
    }
    // Windows' own tar (bsdtar, in System32 since Windows 10 1803) reads zip files.
    const std::wstring tar = Env(L"SystemRoot") + L"\\System32\\tar.exe";
    const int rc = RunHidden(tar, L"-xf " + Quote(file) + L" -C " + Quote(dir) + L" WeatherTool.exe", kUnpackTimeoutMs);
    staged = dir + L"\\WeatherTool.exe";
    if (rc != 0 || !IsFile(staged)) {
        error = name + L" could not be unpacked (tar exit " + std::to_wstring(rc) + L"); nothing was changed.";
        return false;
    }
    if (!LooksLikeProgram(staged)) {
        error = L"The WeatherTool.exe in " + name + L" does not look like a program; nothing was changed.";
        return false;
    }
    return true;
}

bool Swap(const std::wstring& newExe, std::wstring& error) {
    const std::wstring exe = ExePath();
    assert(!exe.empty() && IsFile(newExe));
    const std::wstring old = exe + L".old";
    DeleteFileW(old.c_str());   // from an earlier update; fails harmlessly if there is none
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"The running WeatherTool.exe could not be set aside (error " + std::to_wstring(GetLastError()) +
                L"); nothing was changed.";
        return false;
    }
    if (!MoveFileExW(newExe.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        const DWORD why = GetLastError();
        const bool back = MoveFileExW(old.c_str(), exe.c_str(), 0) != FALSE;
        error = L"The new WeatherTool.exe could not be put in place (error " + std::to_wstring(why) + L")" +
                (back ? std::wstring(L"; the running one is back where it was.")
                      : L", and the running one is now " + old + L": rename it back to WeatherTool.exe.");
        return false;
    }
    return true;
}

void CleanUp(bool installed) {
    const std::wstring exe = ExePath();
    if (exe.empty() || !g_setup.empty()) { return; }   // a downloaded setup waiting for WeatherTool to close
    // Fails while the old copy (or a setup) is still running; the next start tries again.
    if (!installed) { DeleteFileW((exe + L".old").c_str()); }
    RemoveStaged(StagingDir(installed));
}

void RequestRestart(bool on) { g_restart = on; }
void RequestSetup(const std::wstring& setup) { g_setup = setup; }

bool AfterExit() {
    if (!g_setup.empty()) {
        // Started as an ordinary program: Setup raises its own UAC prompt, since it
        // installs into Program Files, and starts WeatherTool again when it is done.
        const std::wstring args = update::SetupArgs();
        SHELLEXECUTEINFOW ex{};
        ex.cbSize = sizeof(ex);
        ex.fMask = SEE_MASK_NOASYNC;
        ex.lpVerb = L"open";
        ex.lpFile = g_setup.c_str();
        ex.lpParameters = args.c_str();
        ex.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&ex) != FALSE;
    }
    if (!g_restart) { return true; }
    const std::wstring exe = ExePath();
    if (exe.empty()) { return false; }
    std::wstring cmd = Quote(exe);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, Dir(exe).c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace wt::updater
