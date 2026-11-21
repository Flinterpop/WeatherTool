# End-to-end check of a WeatherTool setup exe: install, inspect, run, uninstall. Modelled on
# BriskMail's tools/check_installer.ps1. Needs an ELEVATED PowerShell (a machine-level install
# needs admin). It refuses to start if WeatherTool is installed or %ProgramData%\WeatherTool
# exists, so it never touches real settings, and at the end it removes what it made.
#
#   powershell -ExecutionPolicy Bypass -File tools\check_installer.ps1 -Setup dist\WeatherTool-<version>-setup.exe
#
# Checks, each printed PASS or FAIL (and written to -Log when given):
#   1. a silent install succeeds;
#   2. the installed WeatherTool.exe is byte for byte build\Release\WeatherTool.exe, README.md is
#      there, and nothing else is;
#   3. %ProgramData%\WeatherTool exists and BUILTIN\Users can modify it, with inheritance;
#   4. Users cannot write to the program folder;
#   5. the Start menu shortcut exists;
#   6. the installed exe starts and writes WeatherTool.ini to %ProgramData%\WeatherTool, never
#      beside itself (no places are configured, so it makes no network request);
#   7. a silent uninstall removes the program folder, the uninstall entry, the shortcuts and
#      cache\, and leaves WeatherTool.ini.
# Exit code 0 when every check passed.

param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$lines = New-Object System.Collections.Generic.List[string]
function Say([string]$s, [string]$colour = 'Gray') {
    Write-Host $s -ForegroundColor $colour
    $lines.Add($s)
    if ($Log) { [IO.File]::WriteAllLines($Log, $lines.ToArray()) }
}
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Say "PASS $what" 'Green' } else { Say "FAIL $what" 'Red'; $script:failed++ }
}

try {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'run this from an elevated PowerShell' }
    $Setup = (Resolve-Path $Setup).Path
    $appDir = Join-Path $env:ProgramFiles 'WeatherTool'
    $appExe = Join-Path $appDir 'WeatherTool.exe'
    $dataDir = Join-Path $env:ProgramData 'WeatherTool'
    $appId = [regex]::Match((Get-Content (Join-Path $root 'installer\WeatherTool.iss') -Raw), 'AppId=\{\{([0-9A-F-]+)\}').Groups[1].Value
    $uninstKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{$appId}_is1"
    $startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\WeatherTool'
    if (Test-Path $appDir) { throw "$appDir already exists; uninstall WeatherTool first so this check starts clean" }
    if (Test-Path $dataDir) { throw "$dataDir already exists; it may hold real settings, so this check will not run" }
    if (Get-Process WeatherTool -ErrorAction SilentlyContinue) { throw 'close every WeatherTool first' }

    # 1. install
    $p = Start-Process $Setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    Check ($p.ExitCode -eq 0 -and (Test-Path $appExe)) "silent install (exit $($p.ExitCode))"

    # 2. payload
    $built = Join-Path $root 'build\Release\WeatherTool.exe'
    Check ((Get-FileHash $appExe).Hash -eq (Get-FileHash $built).Hash) 'installed exe is the built exe'
    $names = @(Get-ChildItem $appDir -File | ForEach-Object Name | Where-Object { $_ -notmatch '^unins\d+\.(exe|dat)$' } | Sort-Object)
    Check (($names -join ',') -eq 'README.md,WeatherTool.exe') "program folder holds only the app ($($names -join ', '))"

    # 3. data folder rights
    $users = (Get-Acl $dataDir).Access | Where-Object { $_.IdentityReference.Value -match '(^|\\)Users$' -and $_.AccessControlType -eq 'Allow' }
    $modify = $users | Where-Object { ($_.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Modify) -eq [Security.AccessControl.FileSystemRights]::Modify -and
                                      ($_.InheritanceFlags -band [Security.AccessControl.InheritanceFlags]::ObjectInherit) }
    Check ($null -ne $modify) "$dataDir exists and Users can modify it, inherited"

    # 4. program folder is not writable by Users
    $progUsers = (Get-Acl $appDir).Access | Where-Object { $_.IdentityReference.Value -match '(^|\\)Users$' -and $_.AccessControlType -eq 'Allow' -and
                                                         ($_.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Write) }
    Check ($null -eq $progUsers) 'Users cannot write to the program folder'

    # 5. shortcut
    Check (Test-Path (Join-Path $startMenu 'WeatherTool.lnk')) 'Start menu shortcut'

    # 6. run it: settings go to ProgramData, nothing beside the exe
    $run = Start-Process $appExe -PassThru
    $ini = Join-Path $dataDir 'WeatherTool.ini'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 10000 -and -not (Test-Path $ini)) { Start-Sleep -Milliseconds 200 }
    Start-Sleep 1
    [void]$run.CloseMainWindow()
    if (-not $run.WaitForExit(10000)) { $run.Kill() }
    Check (Test-Path $ini) 'installed copy keeps WeatherTool.ini in %ProgramData%\WeatherTool'
    Check (Test-Path (Join-Path $dataDir 'cache')) 'and its cache folder there'
    Check (-not (Test-Path (Join-Path $appDir 'WeatherTool.ini'))) 'nothing written beside the installed exe'
    $state = Select-String -Path $ini -Pattern '^\[state\]' -Encoding Unicode -Quiet
    Check $state 'closing the window saved its state'

    # 7. uninstall
    $unins = Join-Path $appDir 'unins000.exe'
    $p = Start-Process $unins -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 15000 -and (Test-Path $appDir)) { Start-Sleep -Milliseconds 300 }
    Check (-not (Test-Path $appDir)) "silent uninstall removes the program folder (exit $($p.ExitCode))"
    Check (-not (Test-Path $uninstKey)) 'uninstall entry removed'
    Check (-not (Test-Path $startMenu)) 'Start menu shortcuts removed'
    Check (-not (Test-Path (Join-Path $dataDir 'cache'))) 'cache removed'
    Check (Test-Path $ini) 'WeatherTool.ini kept for a reinstall'
} catch {
    Say "ERROR $($_.Exception.Message)" 'Red'
    $failed++
} finally {
    # This check created %ProgramData%\WeatherTool (it refused to start if it existed): remove it.
    if ($dataDir -and (Test-Path $dataDir) -and -not (Test-Path (Join-Path $env:ProgramFiles 'WeatherTool'))) {
        Remove-Item $dataDir -Recurse -Force
        Say "cleaned up $dataDir"
    }
}
Say ($(if ($failed -eq 0) { 'ALL PASSED' } else { "$failed FAILED" }))
exit $failed
