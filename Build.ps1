# Single entry point: clean clone -> .\Build.ps1 -> working binaries.
# Finds Visual Studio, uses its bundled CMake+Ninja when they are not on PATH,
# checks prerequisites with readable errors instead of compiler noise.
#   .\Build.ps1                 - configure (if needed) + build everything
#   .\Build.ps1 -Target XrGame  - build one target
#   .\Build.ps1 -Clean          - wipe the cmake binary dir first
#   .\Build.ps1 -Debug          - Debug preset instead of Release
param(
    [string]$Target = "",
    [switch]$Clean,
    [switch]$Debug
)

$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$preset = if ($Debug) { 'debug' } else { 'release' }

function Fail([string]$msg) { Write-Host "`nBUILD SETUP ERROR: $msg" -ForegroundColor Red; exit 1 }

# --- locate Visual Studio (2022+) with C++ tools --------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "Visual Studio not found. Install VS 2022+ with the 'Desktop development with C++' workload." }
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail "No VS installation with C++ build tools. Add the 'Desktop development with C++' workload." }

# --- enter the dev environment (cl/link/rc on PATH) -----------------------------
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64' | Out-Null
}

# --- cmake + ninja: PATH first, VS-bundled as fallback --------------------------
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $cmake)) { Fail "cmake not found. Add the 'C++ CMake tools for Windows' VS component." }
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $ninja_dir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
    if (Test-Path (Join-Path $ninja_dir 'ninja.exe')) { $env:PATH = "$ninja_dir;$env:PATH" }
    else { Fail "ninja not found. Add the 'C++ CMake tools for Windows' VS component." }
}

$bin = Join-Path $root "Intermediate\cmake-$preset"
if ($Clean -and (Test-Path $bin)) { Remove-Item -Recurse -Force $bin }

& $cmake --preset $preset -S $root
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$build_args = @('--build', '--preset', $preset)
if ($Target) { $build_args += @('--target', $Target) }
& $cmake @build_args
exit $LASTEXITCODE
