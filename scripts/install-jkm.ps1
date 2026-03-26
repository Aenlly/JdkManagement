param(
    [string]$SourceExe,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA "Programs\JdkManagement"),
    [switch]$SkipInit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RawUserPath {
    $environmentKey = Get-ItemProperty -Path "HKCU:\Environment" -ErrorAction SilentlyContinue
    if ($null -eq $environmentKey) {
        return ""
    }

    $pathProperty = $environmentKey.PSObject.Properties["Path"]
    if ($null -eq $pathProperty) {
        return ""
    }

    return [string]$pathProperty.Value
}

function Set-RawUserPath {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        Remove-ItemProperty -Path "HKCU:\Environment" -Name "Path" -ErrorAction SilentlyContinue
        [Environment]::SetEnvironmentVariable("Path", $null, "User")
        return
    }

    New-Item -Path "HKCU:\Environment" -Force | Out-Null
    New-ItemProperty -Path "HKCU:\Environment" -Name "Path" -Value $Value -PropertyType ExpandString -Force | Out-Null
    [Environment]::SetEnvironmentVariable("Path", $Value, "User")
}

function Normalize-PathString {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    return $Value.Replace("/", "\").TrimEnd("\").ToLowerInvariant()
}

function Ensure-UserPathEntry {
    param([string]$Entry)

    $currentPath = Get-RawUserPath
    $normalizedEntry = Normalize-PathString $Entry
    $entries = @()
    if (-not [string]::IsNullOrWhiteSpace($currentPath)) {
        $entries = @($currentPath -split ';')
    }

    foreach ($existing in $entries) {
        if ((Normalize-PathString $existing) -eq $normalizedEntry) {
            return
        }
    }

    if ([string]::IsNullOrWhiteSpace($currentPath)) {
        Set-RawUserPath $Entry
    } else {
        Set-RawUserPath ($currentPath.TrimEnd(';') + ';' + $Entry)
    }
}

function Remove-UserPathEntry {
    param([string]$Entry)

    $currentPath = Get-RawUserPath
    if ([string]::IsNullOrWhiteSpace($currentPath)) {
        return
    }

    $normalizedEntry = Normalize-PathString $Entry
    $retained = New-Object System.Collections.Generic.List[string]
    foreach ($item in ($currentPath -split ';')) {
        if ([string]::IsNullOrWhiteSpace($item)) {
            continue
        }
        if ((Normalize-PathString $item) -ne $normalizedEntry) {
            $retained.Add($item)
        }
    }

    Set-RawUserPath ($retained -join ';')
}

function Publish-EnvironmentChange {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class JkmEnvironmentNotifier {
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SendMessageTimeout(
        IntPtr hWnd,
        uint Msg,
        UIntPtr wParam,
        string lParam,
        uint fuFlags,
        uint uTimeout,
        out UIntPtr lpdwResult);
}
"@ -ErrorAction SilentlyContinue | Out-Null

    $hwndBroadcast = [IntPtr]0xffff
    $wmSettingChange = 0x1A
    $smtoAbortIfHung = 0x0002
    $result = [UIntPtr]::Zero
    [void][JkmEnvironmentNotifier]::SendMessageTimeout(
        $hwndBroadcast,
        $wmSettingChange,
        [UIntPtr]::Zero,
        "Environment",
        $smtoAbortIfHung,
        5000,
        [ref]$result)
}

function Get-ReleaseMetadata {
    $manifestCandidates = @(
        (Join-Path $PSScriptRoot "..\release-manifest.json"),
        (Join-Path $PSScriptRoot "release-manifest.json")
    )

    foreach ($candidate in $manifestCandidates) {
        if (Test-Path $candidate) {
            return Get-Content -Raw -Path $candidate | ConvertFrom-Json
        }
    }

    $headerCandidates = @(
        (Join-Path $PSScriptRoot "..\src\resources\version_info.h"),
        (Join-Path $PSScriptRoot "..\..\src\resources\version_info.h")
    )

    foreach ($headerPath in $headerCandidates) {
        if (-not (Test-Path $headerPath)) {
            continue
        }

        $content = Get-Content -Raw -Path $headerPath
        $version = [regex]::Match($content, '#define\s+JKM_VERSION_STRING\s+"([^"]+)"').Groups[1].Value
        $company = [regex]::Match($content, '#define\s+JKM_COMPANY_NAME\s+"([^"]+)"').Groups[1].Value
        $product = [regex]::Match($content, '#define\s+JKM_PRODUCT_NAME\s+"([^"]+)"').Groups[1].Value

        return [pscustomobject]@{
            appName = if ($product) { $product } else { "JdkManagement" }
            developer = if ($company) { $company } else { "Aenlly" }
            companyName = if ($company) { $company } else { "Aenlly" }
            version = if ($version) { $version } else { "0.0.0-dev" }
            architecture = "windows-x64"
        }
    }

    return [pscustomobject]@{
        appName = "JdkManagement"
        developer = "Aenlly"
        companyName = "Aenlly"
        version = "0.0.0-dev"
        architecture = "windows-x64"
    }
}

function Resolve-SourceExe {
    param([string]$ExplicitSource)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitSource)) {
        return (Resolve-Path $ExplicitSource).Path
    }

    $candidates = @(
        (Join-Path $PSScriptRoot "..\bin\jkm.exe"),
        (Join-Path $PSScriptRoot "..\jkm.exe"),
        (Join-Path $PSScriptRoot "..\build\jkm.exe"),
        (Join-Path $PSScriptRoot "..\build-cmake\Release\jkm.exe"),
        (Join-Path $PSScriptRoot "..\jkm.exe"),
        (Join-Path $PSScriptRoot "..\bin\jkm.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "Unable to locate jkm.exe. Pass -SourceExe explicitly."
}

$metadata = Get-ReleaseMetadata
$resolvedSourceExe = Resolve-SourceExe $SourceExe
$resolvedInstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$binDirectory = Join-Path $resolvedInstallRoot "bin"
$installedExe = Join-Path $binDirectory "jkm.exe"
$installedUninstallScript = Join-Path $resolvedInstallRoot "uninstall-jkm.ps1"
$installInfoPath = Join-Path $resolvedInstallRoot "install-info.json"
$uninstallScriptSource = Join-Path $PSScriptRoot "uninstall-jkm.ps1"

if (-not (Test-Path $uninstallScriptSource)) {
    throw "The uninstall script template was not found at $uninstallScriptSource"
}

$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\JdkManagement"
$uninstallCommand = 'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $installedUninstallScript
$quietUninstallCommand = 'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}" -PurgeData' -f $installedUninstallScript

try {
    New-Item -ItemType Directory -Force -Path $binDirectory | Out-Null
    Copy-Item -Path $resolvedSourceExe -Destination $installedExe -Force
    Copy-Item -Path $uninstallScriptSource -Destination $installedUninstallScript -Force

    $installInfo = [ordered]@{
        appName = $metadata.appName
        developer = $metadata.developer
        version = $metadata.version
        installedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        installRoot = $resolvedInstallRoot
        binaryPath = $installedExe
        dataRoot = (Join-Path $env:LOCALAPPDATA "JdkManagement")
        architecture = $metadata.architecture
    }
    $installInfo | ConvertTo-Json -Depth 4 | Set-Content -Path $installInfoPath -Encoding utf8

    Ensure-UserPathEntry $binDirectory

    New-Item -Path $uninstallKey -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "DisplayName" -Value $metadata.appName -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "DisplayVersion" -Value $metadata.version -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "Publisher" -Value $metadata.companyName -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "InstallLocation" -Value $resolvedInstallRoot -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "DisplayIcon" -Value $installedExe -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "UninstallString" -Value $uninstallCommand -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "QuietUninstallString" -Value $quietUninstallCommand -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "InstallDate" -Value (Get-Date -Format "yyyyMMdd") -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "NoModify" -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name "NoRepair" -Value 1 -PropertyType DWord -Force | Out-Null

    if (-not $SkipInit) {
        & $installedExe init
        if ($LASTEXITCODE -ne 0) {
            throw "jkm init failed with exit code $LASTEXITCODE"
        }
    }

    Publish-EnvironmentChange

    Write-Host ("Installed {0} {1}" -f $metadata.appName, $metadata.version) -ForegroundColor Green
    Write-Host ("  Binary:      {0}" -f $installedExe)
    Write-Host ("  InstallRoot: {0}" -f $resolvedInstallRoot)
    Write-Host ("  User PATH:   {0}" -f $binDirectory)
    Write-Host ("  Uninstall:   {0}" -f $uninstallCommand)
    if ($SkipInit) {
        Write-Host "  Note:        init was skipped; run `jkm init` manually before switching runtimes."
    }
} catch {
    Remove-Item -Path $uninstallKey -Recurse -Force -ErrorAction SilentlyContinue
    Remove-UserPathEntry $binDirectory
    Remove-Item -Recurse -Force $resolvedInstallRoot -ErrorAction SilentlyContinue
    throw
}
