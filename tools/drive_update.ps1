# Drives Help > Check for updates in a live WeatherTool.exe against a local web server standing in
# for GitHub (through WEATHERTOOL_UPDATE_URL), so nothing reaches GitHub. Modelled on BriskMail's
# tools/drive_update.ps1. The exe is copied into a scratch folder first, since an update replaces
# the exe it runs from, and the data folder is an empty scratch one (WEATHERTOOL_DIR), so the app
# makes no weather requests. Four cases, each checked at the app and at the server's log:
#   up to date            one request, no download, "is the latest version"
#   server unreachable    the error shown, nothing changed
#   a zip without an exe  refused, and the exe beside it unchanged
#   a newer release       downloaded, swapped in, the app restarts as the new exe, and what the
#                         update left (WeatherTool.exe.old, Update\) is gone afterwards
#   powershell -ExecutionPolicy Bypass -File tools\drive_update.ps1 -Exe build\Release\WeatherTool.exe
param(
    [Parameter(Mandatory)] [string] $Exe,
    [string] $Python = 'C:\Python314\python.exe',
    [int] $Port = 18765
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$scratch = Join-Path $repo 'build\claude-scratch\update'
if (Test-Path $scratch) { Remove-Item $scratch -Recurse -Force }
$appDir = Join-Path $scratch 'app'; $www = Join-Path $scratch 'www'; $data = Join-Path $scratch 'data'
New-Item -ItemType Directory -Force $appDir, $www, $data | Out-Null
$appExe = Join-Path $appDir 'WeatherTool.exe'
Copy-Item (Resolve-Path $Exe) $appExe
$current = (Get-Item $appExe).VersionInfo.ProductVersion
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class D {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    static string Cls(IntPtr h) { var sb = new StringBuilder(64); GetClassNameW(h, sb, 64); return sb.ToString(); }
    public static List<IntPtr> Boxes(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h) && Cls(h) == "#32770") list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string BoxText(IntPtr box) {
        var parts = new List<string>();
        EnumChildWindows(box, (h, l) => { if (Cls(h) == "Static") { var sb = new StringBuilder(4096); GetWindowTextW(h, sb, 4096);
            if (sb.Length > 0) parts.Add(sb.ToString()); } return true; }, IntPtr.Zero);
        return string.Join(" | ", parts);
    }
}
'@

$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "PASS $what" -ForegroundColor Green } else { Write-Host "FAIL $what" -ForegroundColor Red; $script:failed++ }
}
function Hash($p) { (Get-FileHash $p).Hash }

# A latest-release reply in GitHub's layout, its assets on the local server.
function Publish([string]$tag, [bool]$exeInZip) {
    $v = $tag.Substring(1)
    $dl = Join-Path $www "Flinterpop\WeatherTool\releases\download\$tag"
    New-Item -ItemType Directory -Force $dl | Out-Null
    $zip = Join-Path $dl "WeatherTool-$v-portable.zip"
    if (Test-Path $zip) { [IO.File]::Delete($zip) }
    $z = [IO.Compression.ZipFile]::Open($zip, 'Create')
    try {
        if ($exeInZip) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $script:newExe, 'WeatherTool.exe') }
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, (Join-Path $repo 'README.md'), 'README.md')
    } finally { $z.Dispose() }
    $url = "http://127.0.0.1:$Port/Flinterpop/WeatherTool/releases/download/$tag/WeatherTool-$v-portable.zip"
    $json = '{"tag_name":"' + $tag + '","draft":false,"prerelease":false,"assets":[{"name":"WeatherTool-' + $v +
            '-portable.zip","uploader":{"login":"x"},"browser_download_url":"' + $url + '"}],"body":"Test release ' + $tag + '."}'
    [IO.File]::WriteAllText((Join-Path $www 'latest.json'), $json)
}

# The "new" exe: the same program with marker bytes after it, so it runs and its hash differs.
$script:newExe = Join-Path $scratch 'new.exe'
Copy-Item $appExe $script:newExe
[IO.File]::AppendAllText($script:newExe, 'WEATHERTOOL-UPDATE-TEST-MARKER')
$oldHash = Hash $appExe; $newHash = Hash $script:newExe

# Starts the copy, picks Help > Check for updates, waits for its message box; returns its text.
function CheckOnce([int]$waitSec = 20) {
    $env:WEATHERTOOL_DIR = $data
    $env:WEATHERTOOL_UPDATE_URL = "http://127.0.0.1:$Port/latest.json"
    $p = Start-Process $appExe -PassThru
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($p.MainWindowHandle -eq [IntPtr]::Zero -and $sw.Elapsed.TotalSeconds -lt 15) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    [void][D]::PostMessageW($p.MainWindowHandle, 0x111, [IntPtr]151, [IntPtr]::Zero)   # IDM_CHECKUPDATE
    $sw.Restart()
    while ($sw.Elapsed.TotalSeconds -lt $waitSec) {
        $b = [D]::Boxes([uint32]$p.Id)
        if ($b.Count -gt 0) { return @{ Proc = $p; Box = $b[0]; Text = [D]::BoxText($b[0]) } }
        Start-Sleep -Milliseconds 200
    }
    return @{ Proc = $p; Box = [IntPtr]::Zero; Text = '' }
}
# IDYES 6 / IDNO 7. An MB_OK box's one button answers to IDCANCEL (2), not IDOK: posting IDOK
# leaves it open, the copy keeps running, and (once swapped) holds WeatherTool.exe.old.
function Answer($r, [int]$id) { [void][D]::PostMessageW($r.Box, 0x111, [IntPtr]$id, [IntPtr]::Zero) }
function Quit($p, [string]$what) {
    if (-not $p.HasExited) { Start-Sleep -Milliseconds 300; [void]$p.CloseMainWindow(); [void]$p.WaitForExit(10000) }
    Check $p.HasExited "$what`: that copy exited"
}

$log = Join-Path $scratch 'server.log'
$server = $null
function StartServer { $script:server = Start-Process $Python -ArgumentList '-u', '-m', 'http.server', $Port, '--bind', '127.0.0.1', '--directory', $www -PassThru -WindowStyle Hidden -RedirectStandardError $log; Start-Sleep 2 }
function Requests { if (Test-Path $log) { @(Get-Content $log | Where-Object { $_ -match '"GET ' }) } else { @() } }

try {
    # 1. Unreachable: no server yet.
    $r = CheckOnce
    Check ($r.Text -match 'could not be reached') "server down: error shown ($($r.Text))"
    Answer $r 2; Quit $r.Proc 'server down'
    Check ((Hash $appExe) -eq $oldHash) 'server down: exe unchanged'

    StartServer
    # 2. Up to date.
    Publish "v$current" $true
    $r = CheckOnce
    Check ($r.Text -match 'is the latest version') "up to date: told so ($($r.Text))"
    Answer $r 2; Quit $r.Proc 'up to date'
    $g = @(Requests)   # @(): a one-line answer would otherwise come back as a bare string
    Check ($g.Count -eq 1 -and $g[0] -match 'latest\.json') "up to date: one request, no download ($($g.Count))"

    # 3. A zip without an exe.
    Publish 'v99.0.0' $false
    $r = CheckOnce
    Check ($r.Text -match '99\.0\.0 is available') "bad zip: offered ($($r.Text.Substring(0, [Math]::Min(60, $r.Text.Length))))"
    Answer $r 6
    Start-Sleep 1
    $sw = [Diagnostics.Stopwatch]::StartNew(); $t = ''
    while ($sw.Elapsed.TotalSeconds -lt 20 -and -not $t) { $b = [D]::Boxes([uint32]$r.Proc.Id); if ($b.Count) { $t = [D]::BoxText($b[0]); Answer @{ Box = $b[0] } 2 } else { Start-Sleep -Milliseconds 200 } }
    Check ($t -match 'could not be unpacked') "bad zip: refused ($t)"
    Quit $r.Proc 'bad zip'
    Check ((Hash $appExe) -eq $oldHash -and -not (Test-Path "$appExe.old")) 'bad zip: exe unchanged, nothing set aside'

    # 4. A newer release.
    Publish 'v99.0.1' $true
    $before = Requests
    $r = CheckOnce
    Check ($r.Text -match '99\.0\.1 is available') 'update: offered'
    Answer $r 6
    Check ($r.Proc.WaitForExit(30000)) 'update: the old copy closed'
    $sw = [Diagnostics.Stopwatch]::StartNew(); $new = $null
    while ($sw.Elapsed.TotalSeconds -lt 20 -and -not $new) {
        $new = Get-Process WeatherTool -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $appExe -and $_.Id -ne $r.Proc.Id } | Select-Object -First 1
        Start-Sleep -Milliseconds 300
    }
    Check ($null -ne $new) 'update: the new copy started'
    Check ((Hash $appExe) -eq $newHash) 'update: WeatherTool.exe is the new exe'
    $added = @(Requests).Count - @($before).Count
    Check ($added -eq 2) "update: two requests, the check and the zip ($added)"
    # The new copy removes them at start if it can, else on its 15 s clean-up timer.
    $sw.Restart()
    while ($sw.Elapsed.TotalSeconds -lt 40 -and ((Test-Path "$appExe.old") -or (Test-Path (Join-Path $appDir 'Update')))) { Start-Sleep -Milliseconds 500 }
    Check (-not (Test-Path "$appExe.old") -and -not (Test-Path (Join-Path $appDir 'Update'))) "update: .old and Update\ removed (after $([int]$sw.Elapsed.TotalSeconds) s)"
    if (Test-Path "$appExe.old") {
        try { [IO.File]::Delete("$appExe.old"); Write-Host '  (the test itself could delete .old)' }
        catch { Write-Host "  (.old cannot be deleted: $($_.Exception.InnerException.Message))" }
        Get-Process | Where-Object { $_.Path -like "$appDir*" } | ForEach-Object { Write-Host "  running: $($_.Id) $($_.Path)" }
    }
    if ($new) { Quit $new 'update' }
} finally {
    Get-Process WeatherTool -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$appDir*" } | Stop-Process -Force
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
if ($failed -eq 0) { Write-Host 'ALL PASSED' -ForegroundColor Green } else { Write-Host "$failed FAILED" -ForegroundColor Red }
exit $failed
