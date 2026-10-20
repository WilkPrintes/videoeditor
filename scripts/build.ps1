<#
.SYNOPSIS
    Configures and builds VideoEditor with MSVC (Visual Studio / Build Tools 2022 or 2026).

.DESCRIPTION
    Loads the x64 developer environment (cl, link, CMake and Ninja from Visual Studio) and runs
    the CMake preset. Works from a plain PowerShell window.

.EXAMPLE
    ./scripts/build.ps1                          # windows-release (RelWithDebInfo)
    ./scripts/build.ps1 -Preset windows-debug -Test
    ./scripts/build.ps1 -Fresh -CMakeArgs "-DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64"
#>
param(
    [string]$Preset = "windows-release",
    [switch]$Test,
    [switch]$Fresh,
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $env:VSCMD_VER) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "Visual Studio Build Tools not found (vswhere.exe is missing)." }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "No Visual Studio installation with the C++ x64 toolset." }
    $env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH" # VsDevCmd calls vswhere itself
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

# CMake prints warnings on stderr; with redirected output Windows PowerShell would turn them into
# terminating errors. Exit codes are checked explicitly instead.
$ErrorActionPreference = "Continue"
Push-Location $root
try {
    $configure = @("--preset", $Preset)
    if ($Fresh) { $configure += "--fresh" }
    $configure += $CMakeArgs
    cmake @configure
    if ($LASTEXITCODE) { throw "CMake configure failed" }

    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "Build failed" }

    if ($Test) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "Tests failed" }
    }
}
finally {
    Pop-Location
}
