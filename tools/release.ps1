# Builds, tests and packages a release, both ways it ships:
#   dist/WeatherTool-<version>-portable.zip   WeatherTool.exe and README.md, to unpack anywhere
#   dist/WeatherTool-<version>-setup.exe      the machine-level installer (installer/WeatherTool.iss)
# plus dist/WeatherTool-<version>-sha256.md, the files' SHA-256 for the release notes.
# Modelled on BriskMail's tools/release.ps1. The version lives only in CMakeLists.txt
# project(VERSION); the exe's version resource must match it. The README badge reads the
# latest GitHub release, so it needs no bump.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1     (5.1 or pwsh: the zip is
#   written with System.IO.Compression and '/' entry names, never Compress-Archive)
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
# Only a copy running from this build tree blocks the build; an installed copy does not.
$buildExe = Join-Path $root 'build\Release\WeatherTool.exe'
if (Get-Process WeatherTool -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $buildExe }) {
    throw "Close the WeatherTool running from $buildExe first: the build cannot replace a running exe."
}

$version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(WeatherTool VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'No project(WeatherTool VERSION x.y.z) in CMakeLists.txt.' }
"WeatherTool $version"

function Run([string] $what, [scriptblock] $step) {
    "== $what"
    $ErrorActionPreference = 'Continue'
    & $step
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "$what failed (exit $code)." }
}

# cmake and ctest are not on PATH on every machine: find them inside Visual Studio if need be.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'CMake\\bin\\cmake\.exe$' } | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw 'cmake.exe not found on PATH or under C:\Program Files\Microsoft Visual Studio.' }
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'

$build = Join-Path $root 'build'
$exe = Join-Path $build 'Release\WeatherTool.exe'
Run 'configure' { & $cmake -S $root -B $build -G 'Visual Studio 18 2026' -A x64 }
Run 'build' { & $cmake --build $build --config Release }
Run 'unit tests' { & $ctest --test-dir $build -C Release --output-on-failure }

# Static CRT: the exe must not need the VC++ redistributable.
$dumpbin = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin.exe not found under C:\Program Files\Microsoft Visual Studio.' }
$deps = & $dumpbin.FullName /nologo /dependents $exe | Where-Object { $_ -match '\.dll' } | ForEach-Object { $_.Trim() }
"   needs: $($deps -join ', ')"
$bad = $deps | Where-Object { $_ -match '^(vcruntime|msvcp|api-ms-win-crt)' }
if ($bad) { throw "The exe needs the VC++ runtime: $($bad -join ', ')" }

$pv = (Get-Item $exe).VersionInfo.ProductVersion
if ($pv -ne $version) { throw "$exe reports version '$pv', not $version." }

# The repo is public: no build-machine paths or debug-file names may be compiled in.
$bytes = [IO.File]::ReadAllBytes($exe)
$text = [Text.Encoding]::ASCII.GetString($bytes) + [Text.Encoding]::Unicode.GetString($bytes)
$leak = [regex]::Matches($text, '(?i)[a-z]:\\source\\|\\users\\[^\\]+\\|\.pdb\b|claude-scratch') | ForEach-Object Value | Sort-Object -Unique
if ($leak) { throw "The exe carries build-machine strings: $($leak -join ', ')" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "WeatherTool-$version-portable.zip"
if (Test-Path $zip) { [IO.File]::Delete($zip) }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($f in @($exe, (Join-Path $root 'README.md'))) {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f, (Split-Path $f -Leaf), 'Optimal')
    }
} finally { $z.Dispose() }
$z = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($z.Entries | ForEach-Object { $_.FullName })
    if ($entries | Where-Object { $_ -match '\\' }) { throw 'A zip entry name holds a backslash.' }
    if ((($entries | Sort-Object) -join ',') -ne 'README.md,WeatherTool.exe') { throw "Unexpected zip contents: $($entries -join ', ')" }
} finally { $z.Dispose() }
"== wrote $zip ($([int]((Get-Item $zip).Length / 1KB)) KB): $($entries -join ', ')"

# The installer. ISCC is not on PATH and never under AppData: resolve it from the candidates.
$isccCandidates = @('C:\bin\InnoSetup6\ISCC.exe', "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                    "$env:ProgramFiles\Inno Setup 6\ISCC.exe")
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { throw "ISCC.exe not found; tried: $($isccCandidates -join '; ')" }
$setup = Join-Path $dist "WeatherTool-$version-setup.exe"
if (Test-Path $setup) { [IO.File]::Delete($setup) }
Run "installer ($iscc)" { & $iscc /Q "/DAppVersion=$version" "/DAppExe=$exe" "/DSourceDir=$root" "/DOutDir=$dist" (Join-Path $root 'installer\WeatherTool.iss') }
if (-not (Test-Path $setup)) { throw "ISCC reported success but $setup is not there." }
$spv = "$((Get-Item $setup).VersionInfo.ProductVersion)".Trim()   # Inno pads it with spaces
if ($spv -notin @($version, "$version.0")) { throw "$setup reports version '$spv', not $version." }
"== wrote $setup ($([int]((Get-Item $setup).Length / 1KB)) KB)"

# SHA-256 of what ships, the exe's read back from inside the zip so the table describes exactly
# what was packed. Paste it at the end of the release notes.
$exeHash = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLowerInvariant()
$z = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $s = $z.GetEntry('WeatherTool.exe').Open()
    try { $inZip = (Get-FileHash -Algorithm SHA256 -InputStream $s).Hash.ToLowerInvariant() } finally { $s.Dispose() }
} finally { $z.Dispose() }
if ($inZip -ne $exeHash) { throw 'WeatherTool.exe inside the zip is not the built exe.' }
$rows = @([pscustomobject]@{ Name = 'WeatherTool.exe'; Hash = $exeHash }) +
        @($zip, $setup | ForEach-Object { [pscustomobject]@{ Name = (Split-Path $_ -Leaf); Hash = (Get-FileHash -Algorithm SHA256 $_).Hash.ToLowerInvariant() } })
$hashes = Join-Path $dist "WeatherTool-$version-sha256.md"
$md = @('## SHA-256', '', '| File | SHA-256 |', '|---|---|') + @($rows | ForEach-Object { "| ``$($_.Name)`` | ``$($_.Hash)`` |" }) +
      @('', 'Check a copy with `Get-FileHash <file> -Algorithm SHA256` or `certutil -hashfile <file> SHA256`.')
[IO.File]::WriteAllLines($hashes, [string[]]$md)
"== wrote $hashes"
$rows | ForEach-Object { "   $($_.Hash)  $($_.Name)" }
"   The setup's own install check (elevated): powershell -ExecutionPolicy Bypass -File tools\check_installer.ps1 -Setup $setup"
