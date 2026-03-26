param(
    [string]$SourceExe,
    [string]$OutputRoot = (Join-Path $PSScriptRoot "..\out\package\JdkManagement")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-SourceExe {
    param([string]$ExplicitSource)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitSource)) {
        return (Resolve-Path $ExplicitSource).Path
    }

    $candidates = @(
        (Join-Path $PSScriptRoot "..\build\jkm.exe"),
        (Join-Path $PSScriptRoot "..\build-cmake\Release\jkm.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Unable to locate jkm.exe. Build the project first or pass -SourceExe explicitly."
}

$resolvedSourceExe = Resolve-SourceExe $SourceExe
$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$outputScripts = Join-Path $resolvedOutputRoot "scripts"

Remove-Item -Recurse -Force $resolvedOutputRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $outputScripts | Out-Null

Copy-Item -Path $resolvedSourceExe -Destination (Join-Path $resolvedOutputRoot "jkm.exe") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "install-jkm.ps1") -Destination (Join-Path $outputScripts "install-jkm.ps1") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "uninstall-jkm.ps1") -Destination (Join-Path $outputScripts "uninstall-jkm.ps1") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "..\README.md") -Destination (Join-Path $resolvedOutputRoot "README.md") -Force

@(
    "JdkManagement package",
    "Developer: Aenlly",
    "",
    "1. Run .\scripts\install-jkm.ps1",
    "2. Use jkm init / install / use commands from a new terminal"
) | Set-Content -Path (Join-Path $resolvedOutputRoot "INSTALL.txt") -Encoding ascii

Write-Host "Packaged JdkManagement" -ForegroundColor Green
Write-Host ("  Output: {0}" -f $resolvedOutputRoot)
Write-Host ("  Binary: {0}" -f (Join-Path $resolvedOutputRoot "jkm.exe"))
Write-Host ("  Install: {0}" -f (Join-Path $outputScripts "install-jkm.ps1"))
