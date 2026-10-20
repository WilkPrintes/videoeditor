<#
.SYNOPSIS
    Downloads the Windows dependencies that are not installed system-wide.

.DESCRIPTION
    - FFmpeg 9.0 LGPL shared build (github.com/BtbN/FFmpeg-Builds), SHA-256 verified,
      extracted to external/ffmpeg (ignored by git).
    - With -Qt: Qt 6.12.0 for MSVC (download.qt.io archives, SHA-1 verified) into C:\Qt,
      via scripts/fetch_qt.py in a private Python venv.
    Visual Studio Build Tools (compiler, CMake, Ninja) are installed separately; see README.

.EXAMPLE
    ./scripts/fetch-deps-windows.ps1
    ./scripts/fetch-deps-windows.ps1 -Qt
#>
param(
    [switch]$Qt,
    [string]$QtRoot = "C:\Qt",
    [string]$QtVersion = "6.12.0",
    [string]$FFmpegBranch = "9.0"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$external = Join-Path $root "external"
$downloads = Join-Path $external "_downloads"
New-Item -ItemType Directory -Force $downloads | Out-Null

# ---- FFmpeg (LGPL, shared libraries + import libs + headers)
$ffmpegDir = Join-Path $external "ffmpeg"
if (Test-Path (Join-Path $ffmpegDir "include\libavcodec\avcodec.h")) {
    Write-Host "FFmpeg already present in $ffmpegDir"
} else {
    $name = "ffmpeg-n$FFmpegBranch-latest-win64-lgpl-shared-$FFmpegBranch"
    $base = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest"
    $zip = Join-Path $downloads "$name.zip"
    Write-Host "Downloading $name.zip"
    Invoke-WebRequest "$base/$name.zip" -OutFile $zip
    $sumsFile = Join-Path $downloads "checksums.sha256"
    Invoke-WebRequest "$base/checksums.sha256" -OutFile $sumsFile
    $line = Get-Content $sumsFile | Where-Object { $_ -match [regex]::Escape("$name.zip") } | Select-Object -First 1
    $expected = if ($line) { ($line -split "\s+")[0] } else { "" }
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if (-not $expected -or $actual -ne $expected.ToLower()) { throw "FFmpeg checksum mismatch ($actual vs $expected)" }
    Expand-Archive $zip -DestinationPath $external -Force
    Move-Item (Join-Path $external $name) $ffmpegDir
    Write-Host "FFmpeg installed in $ffmpegDir"
}

# ---- Qt (optional; the official online installer works as well)
if ($Qt) {
    $qtDir = Join-Path $QtRoot "$QtVersion\msvc2022_64"
    if (Test-Path (Join-Path $qtDir "lib\cmake\Qt6\Qt6Config.cmake")) {
        Write-Host "Qt already present in $qtDir"
    } else {
        $venv = Join-Path $root ".tools\qt-venv"
        if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
            python -m venv $venv
            & (Join-Path $venv "Scripts\python.exe") -m pip install --quiet --disable-pip-version-check py7zr
        }
        & (Join-Path $venv "Scripts\python.exe") -I (Join-Path $PSScriptRoot "fetch_qt.py") `
            --version $QtVersion --arch msvc2022_64 --modules qtshadertools --out $QtRoot `
            --cache (Join-Path $downloads "qt")
        if ($LASTEXITCODE) { throw "Qt download failed" }
    }
}
