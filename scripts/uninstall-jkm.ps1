param(
    [string]$InstallRoot = $PSScriptRoot,
    [switch]$PurgeData
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

function Start-DelayedDirectoryRemoval {
    param([string]$TargetDirectory)

    $cleanupScript = Join-Path $env:TEMP ("jkm-cleanup-" + [guid]::NewGuid().ToString("N") + ".cmd")
    @(
        "@echo off"
        "ping 127.0.0.1 -n 3 > nul"
        ('rmdir /s /q "{0}"' -f $TargetDirectory)
        'del /q "%~f0"'
    ) | Set-Content -Path $cleanupScript -Encoding ascii

    Start-Process -FilePath "cmd.exe" -ArgumentList "/c `"$cleanupScript`"" -WindowStyle Hidden
}

$resolvedInstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$binDirectory = Join-Path $resolvedInstallRoot "bin"
$installedExe = Join-Path $binDirectory "jkm.exe"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\JdkManagement"
$dataRoot = Join-Path $env:LOCALAPPDATA "JdkManagement"

if (Test-Path $installedExe) {
    $arguments = @("uninstall")
    if ($PurgeData) {
        $arguments += "--purge-data"
    }

    & $installedExe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "jkm uninstall failed with exit code $LASTEXITCODE"
    }
} elseif ($PurgeData -and (Test-Path $dataRoot)) {
    Remove-Item -Recurse -Force $dataRoot
}

Remove-UserPathEntry $binDirectory
Remove-Item -Path $uninstallKey -Recurse -Force -ErrorAction SilentlyContinue
Publish-EnvironmentChange
Start-DelayedDirectoryRemoval $resolvedInstallRoot

Write-Host "Uninstalled JdkManagement" -ForegroundColor Green
Write-Host ("  InstallRoot: {0}" -f $resolvedInstallRoot)
if ($PurgeData) {
    Write-Host ("  DataRoot:    {0}" -f $dataRoot)
}
Write-Host "  Note:        open a new terminal to refresh environment variables."
