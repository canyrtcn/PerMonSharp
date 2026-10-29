param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

# Locate Visual Studio / BuildTools vcvars64.bat via vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Please install Visual Studio 2022+ or C++ Build Tools."
}

$vsInstall = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw "Visual Studio C++ x64 build tools were not found."
}

$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}

$BuildDir = Join-Path $RepoRoot "build"
$DistDir  = Join-Path $RepoRoot "dist"

Write-Host "==> Configuring and building PerMonSharp ($Configuration)..." -ForegroundColor Cyan

$cmdLine = "`"$vcvars`" >nul && cmake -S `"$RepoRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration && cmake --build `"$BuildDir`" --config $Configuration"
cmd /c $cmdLine
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

$BuiltExe = Join-Path $BuildDir "PerMonSharp.exe"
$TestsExe = Join-Path $BuildDir "PerMonSharpTests.exe"

if (-not (Test-Path $BuiltExe)) {
    throw "Expected output executable not found: $BuiltExe"
}

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
$DistExe = Join-Path $DistDir "PerMonSharp.exe"
Copy-Item -Force $BuiltExe $DistExe

$exeSizeKB = [Math]::Round(((Get-Item $DistExe).Length / 1024.0), 1)
Write-Host "==> Built distributable: $DistExe ($exeSizeKB KB)" -ForegroundColor Green

if (-not $SkipTests -and (Test-Path $TestsExe)) {
    Write-Host "==> Running automated unit & GPU test suite..." -ForegroundColor Cyan
    & $TestsExe
    if ($LASTEXITCODE -ne 0) {
        throw "Test suite failed with exit code $LASTEXITCODE"
    }
}
