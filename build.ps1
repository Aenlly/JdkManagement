Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$workspace = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install Visual Studio or Build Tools first."
}

$installationPath = & $vswhere -latest -products * -property installationPath
if (-not $installationPath) {
    throw "Visual Studio installation was not found."
}

$vcVars64 = Join-Path $installationPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcVars64)) {
    throw "vcvars64.bat was not found at $vcVars64"
}

$msvcRoot = Get-ChildItem (Join-Path $installationPath "VC\Tools\MSVC") -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvcRoot) {
    throw "MSVC toolset was not found. Install the C++ build tools workload first."
}

$msvcInclude = Join-Path $msvcRoot.FullName "include"
if (-not (Test-Path $msvcInclude)) {
    throw "MSVC include headers are missing at $msvcInclude. The Visual Studio C++ workload appears incomplete."
}

$windowsSdkInclude = "C:\Program Files (x86)\Windows Kits\10\Include"
if (-not (Test-Path $windowsSdkInclude)) {
    throw "Windows SDK headers were not found at $windowsSdkInclude. Install the Windows SDK to build this project."
}

$buildDir = Join-Path $workspace "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$sources = @(
    "src\main.cpp",
    "src\app\app.cpp",
    "src\core\models.cpp",
    "src\core\paths.cpp",
    "src\core\store.cpp",
    "src\infrastructure\audit.cpp",
    "src\infrastructure\logger.cpp",
    "src\infrastructure\process.cpp",
    "src\infrastructure\platform_windows.cpp",
    "src\providers\temurin.cpp",
    "src\providers\python_runtime.cpp",
    "src\providers\tool_runtime.cpp"
)

$quotedSources = ($sources | ForEach-Object { "`"$_`"" }) -join " "
$resourceSource = "src\\resources\\jkm.rc"
$resourceObject = "build\\jkm.res"
$linkLibraries = "advapi32.lib user32.lib"
$resourceCommand = "rc /nologo /I . /I src /fo $resourceObject $resourceSource"
$compileCommand = "cl /std:c++20 /EHsc /nologo /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I src /Fe:build\\jkm.exe $quotedSources $resourceObject $linkLibraries"
$driverScript = Join-Path $buildDir "invoke_build.cmd"

@(
    "@echo off",
    "call `"$vcVars64`"",
    $resourceCommand,
    $compileCommand
) | Set-Content -Encoding ascii $driverScript

Push-Location $workspace
try {
    cmd.exe /c $driverScript
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
