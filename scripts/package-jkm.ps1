param(
    [string]$SourceExe,
    [string]$SetupExe,
    [string]$UninstallExe,
    [string]$OutputRoot = (Join-Path $PSScriptRoot "..\out\package\JdkManagement")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Artifact {
    param(
        [string]$ExplicitPath,
        [string[]]$Candidates,
        [string]$Name
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        return (Resolve-Path $ExplicitPath).Path
    }

    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Unable to locate $Name. Build the project first or pass the explicit path."
}

$resolvedSourceExe = Resolve-Artifact $SourceExe @(
    (Join-Path $PSScriptRoot "..\build\jkm.exe"),
    (Join-Path $PSScriptRoot "..\build-cmake\Release\jkm.exe")
) "jkm.exe"

$resolvedSetupExe = Resolve-Artifact $SetupExe @(
    (Join-Path $PSScriptRoot "..\build\jkm-setup.exe"),
    (Join-Path $PSScriptRoot "..\build-cmake\Release\jkm-setup.exe")
) "jkm-setup.exe"

$resolvedUninstallExe = Resolve-Artifact $UninstallExe @(
    (Join-Path $PSScriptRoot "..\build\jkm-uninstall.exe"),
    (Join-Path $PSScriptRoot "..\build-cmake\Release\jkm-uninstall.exe")
) "jkm-uninstall.exe"

$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$zipPath = Join-Path (Split-Path -Parent $resolvedOutputRoot) "JdkManagement-windows-x64.zip"
$outputDocs = Join-Path $resolvedOutputRoot "docs"

Remove-Item -Recurse -Force $resolvedOutputRoot -ErrorAction SilentlyContinue
Remove-Item -Force $zipPath -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $resolvedOutputRoot | Out-Null
New-Item -ItemType Directory -Force -Path $outputDocs | Out-Null

Copy-Item -Path $resolvedSourceExe -Destination (Join-Path $resolvedOutputRoot "jkm.exe") -Force
Copy-Item -Path $resolvedSetupExe -Destination (Join-Path $resolvedOutputRoot "JdkManagement-Setup-windows-x64.exe") -Force
Copy-Item -Path $resolvedUninstallExe -Destination (Join-Path $resolvedOutputRoot "JdkManagement-Uninstall-windows-x64.exe") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "..\README.md") -Destination (Join-Path $resolvedOutputRoot "README.md") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "..\README.zh-CN.md") -Destination (Join-Path $resolvedOutputRoot "README.zh-CN.md") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "..\docs\download.md") -Destination (Join-Path $resolvedOutputRoot "DOWNLOAD.md") -Force
Copy-Item -Path (Join-Path $PSScriptRoot "..\docs\download.md") -Destination (Join-Path $outputDocs "download.md") -Force

@(
    "JdkManagement Windows x64 package",
    "",
    "Install:",
    "  1. Run JdkManagement-Setup-windows-x64.exe",
    "  2. Open a new terminal",
    "  3. Run: jkm doctor",
    "",
    "Uninstall:",
    "  - Run JdkManagement-Uninstall-windows-x64.exe from this package, or",
    "  - Use Windows Settings > Apps > JdkManagement > Uninstall",
    "",
    "Do not run PowerShell scripts for normal installation.",
    "See DOWNLOAD.md for details."
) | Set-Content -Path (Join-Path $resolvedOutputRoot "INSTALL.txt") -Encoding utf8

Compress-Archive -Path (Join-Path $resolvedOutputRoot "*") -DestinationPath $zipPath -Force

Write-Host "Packaged JdkManagement" -ForegroundColor Green
Write-Host ("  Output:    {0}" -f $resolvedOutputRoot)
Write-Host ("  Zip:       {0}" -f $zipPath)
Write-Host ("  Installer: {0}" -f (Join-Path $resolvedOutputRoot "JdkManagement-Setup-windows-x64.exe"))
Write-Host ("  Uninstall: {0}" -f (Join-Path $resolvedOutputRoot "JdkManagement-Uninstall-windows-x64.exe"))
