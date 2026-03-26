#include "providers/tool_runtime.h"

#include <sstream>
#include <iostream>
#include <unordered_map>

#include "infrastructure/process.h"
#include "infrastructure/platform_windows.h"

namespace jkm {

namespace {

std::string EscapePowerShellLiteral(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const auto ch : value) {
        if (ch == '\'') {
            escaped += "''";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::unordered_map<std::string, std::string> ParseKeyValueOutput(const std::string& output) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const auto split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }

        values[line.substr(0, split)] = line.substr(split + 1);
    }

    return values;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        parts.push_back(item);
    }
    return parts;
}

std::string BuildInstallLogHelperScript() {
    return
        "function Write-JkmLog {\n"
        "  param([string]$Message)\n"
        "  Write-Output ('LOG=' + $Message)\n"
        "}\n";
}

ProcessOutputLineHandler BuildInstallOutputLineHandler() {
    return [](const std::string& line) {
        if (line.rfind("LOG=", 0) == 0) {
            std::cout << line.substr(4) << std::endl;
        }
    };
}

bool ParseToolInstallResult(
    RuntimeType type,
    const std::string& distribution,
    const ProcessResult& process_result,
    ToolInstallResult* result,
    std::string* error) {
    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            *error = process_result.output.empty() ? "tool install script failed" : process_result.output;
        }
        return false;
    }

    const auto values = ParseKeyValueOutput(process_result.output);
    const auto name_it = values.find("name");
    const auto root_it = values.find("root");
    if (name_it == values.end() || root_it == values.end()) {
        if (error != nullptr) {
            *error = "tool installer returned incomplete metadata";
        }
        return false;
    }

    ToolInstallResult install_result;
    install_result.runtime = InstalledRuntime{
        type,
        values.contains("distribution") ? values.at("distribution") : distribution,
        name_it->second,
        PathFromUtf8(root_it->second),
        name_it->second,
        PathFromUtf8(root_it->second),
        false,
        false
    };
    install_result.already_installed = values.contains("status") && values.at("status") == "already_installed";
    if (values.contains("resolved_version")) {
        install_result.resolved_version = values.at("resolved_version");
    }
    if (values.contains("package_name")) {
        install_result.package_name = values.at("package_name");
    }
    if (values.contains("download_url")) {
        install_result.download_url = values.at("download_url");
    }
    if (values.contains("checksum")) {
        install_result.checksum = values.at("checksum");
    }
    if (values.contains("channel")) {
        install_result.channel = values.at("channel");
    }
    if (values.contains("published_at")) {
        install_result.published_at = values.at("published_at");
    }
    install_result.raw_output = process_result.output;

    if (result != nullptr) {
        *result = std::move(install_result);
    }
    return true;
}

bool ParseRemoteReleases(
    const ProcessResult& process_result,
    std::vector<ToolRemoteRelease>* result,
    std::string* error) {
    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            *error = process_result.output.empty() ? "tool remote release query failed" : process_result.output;
        }
        return false;
    }

    std::vector<ToolRemoteRelease> releases;
    std::istringstream stream(process_result.output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("ITEM\t", 0) != 0) {
            continue;
        }

        const auto fields = Split(line.substr(5), '\t');
        if (fields.size() < 6) {
            continue;
        }

        releases.push_back(ToolRemoteRelease{
            fields[0],
            fields[1],
            fields[2],
            fields[3],
            fields[4],
            fields[5]
        });
    }

    if (result != nullptr) {
        *result = std::move(releases);
    }
    return true;
}

std::string BuildNodeRemoteListScript(const std::string& selector, const std::string& arch, int limit) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$limit = " << limit << '\n'
        << "switch ($arch.ToLowerInvariant()) {\n"
        << "  'x64' { $packageToken = 'win-x64-zip' }\n"
        << "  default { throw 'only x64 is currently supported for Node.js installs' }\n"
        << "}\n"
        << "$releases = @((Invoke-RestMethod -Uri 'https://nodejs.org/dist/index.json' -Headers @{ 'User-Agent' = 'jkm-cpp' }) | Where-Object { $_.files -contains $packageToken })\n"
        << "$normalizedSelector = $selector.Trim().TrimStart('v', 'V')\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' {}\n"
        << "  'latest' {}\n"
        << "  'current' { $releases = @($releases | Where-Object { -not $_.lts }) }\n"
        << "  'lts' { $releases = @($releases | Where-Object { $_.lts }) }\n"
        << "  default {\n"
        << "    $releases = @($releases | Where-Object {\n"
        << "      $normalizedVersion = ([string]$_.version).TrimStart('v', 'V')\n"
        << "      $normalizedVersion -eq $normalizedSelector -or $normalizedVersion.StartsWith($normalizedSelector + '.')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "$selected = @($releases | Select-Object -First $limit)\n"
        << "foreach ($release in $selected) {\n"
        << "  $versionTag = [string]$release.version\n"
        << "  $version = $versionTag.TrimStart('v', 'V')\n"
        << "  $packageName = \"node-$versionTag-win-x64.zip\"\n"
        << "  $downloadUrl = \"https://nodejs.org/dist/$versionTag/$packageName\"\n"
        << "  $checksum = ''\n"
        << "  try {\n"
        << "    $shasums = (Invoke-WebRequest -Uri (\"https://nodejs.org/dist/$versionTag/SHASUMS256.txt\") -Headers @{ 'User-Agent' = 'jkm-cpp' }).Content\n"
        << "    $match = $shasums -split \"`n\" | Where-Object { $_ -match ('^(?<hash>[0-9a-fA-F]{64})\\s+' + [regex]::Escape($packageName) + '$') } | Select-Object -First 1\n"
        << "    if ($match) { $checksum = ([regex]::Match($match, '^(?<hash>[0-9a-fA-F]{64})')).Groups['hash'].Value.ToLowerInvariant() }\n"
        << "  } catch { $checksum = '' }\n"
        << "  $channel = if ($release.lts -and $release.lts -ne $false) { 'lts:' + [string]$release.lts } else { 'current' }\n"
        << "  $fields = @($version, $packageName, $downloadUrl, $checksum, $channel, [string]$release.date) | ForEach-Object { ($_ -replace \"`t\", ' ') -replace \"`r|`n\", ' ' }\n"
        << "  Write-Output ('ITEM' + \"`t\" + ($fields -join \"`t\"))\n"
        << "}\n";
    return script.str();
}

std::string BuildNodeInstallScript(const AppPaths& paths, const std::string& selector, const std::string& arch) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildInstallLogHelperScript()
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$downloads = '" << EscapePowerShellLiteral(PathToUtf8(paths.downloads)) << "'\n"
        << "$tempRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.temp)) << "'\n"
        << "$rootPath = '" << EscapePowerShellLiteral(PathToUtf8(paths.installs / "node" / "nodejs")) << "'\n"
        << "New-Item -ItemType Directory -Force -Path $downloads, $tempRoot, $rootPath | Out-Null\n"
        << "switch ($arch.ToLowerInvariant()) {\n"
        << "  'x64' { $packageToken = 'win-x64-zip' }\n"
        << "  default { throw 'only x64 is currently supported for Node.js installs' }\n"
        << "}\n"
        << "$releases = @((Invoke-RestMethod -Uri 'https://nodejs.org/dist/index.json' -Headers @{ 'User-Agent' = 'jkm-cpp' }) | Where-Object { $_.files -contains $packageToken })\n"
        << "$normalizedSelector = $selector.Trim().TrimStart('v', 'V')\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' { throw 'selector is required' }\n"
        << "  'latest' {}\n"
        << "  'current' { $releases = @($releases | Where-Object { -not $_.lts }) }\n"
        << "  'lts' { $releases = @($releases | Where-Object { $_.lts }) }\n"
        << "  default {\n"
        << "    $releases = @($releases | Where-Object {\n"
        << "      $normalizedVersion = ([string]$_.version).TrimStart('v', 'V')\n"
        << "      $normalizedVersion -eq $normalizedSelector -or $normalizedVersion.StartsWith($normalizedSelector + '.')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "Write-JkmLog ('Resolving Node.js release for selector ' + $selector)\n"
        << "$release = $releases | Select-Object -First 1\n"
        << "if (-not $release) { throw \"no Node.js release matched selector $selector\" }\n"
        << "$versionTag = [string]$release.version\n"
        << "$resolvedVersion = $versionTag.TrimStart('v', 'V')\n"
        << "Write-JkmLog ('Selected Node.js version ' + $resolvedVersion)\n"
        << "$packageName = \"node-$versionTag-win-x64.zip\"\n"
        << "$downloadUrl = \"https://nodejs.org/dist/$versionTag/$packageName\"\n"
        << "$shasums = (Invoke-WebRequest -Uri (\"https://nodejs.org/dist/$versionTag/SHASUMS256.txt\") -Headers @{ 'User-Agent' = 'jkm-cpp' }).Content\n"
        << "$checksumLine = $shasums -split \"`n\" | Where-Object { $_ -match ('^(?<hash>[0-9a-fA-F]{64})\\s+' + [regex]::Escape($packageName) + '$') } | Select-Object -First 1\n"
        << "if (-not $checksumLine) { throw 'unable to resolve Node.js package checksum' }\n"
        << "$checksum = ([regex]::Match($checksumLine, '^(?<hash>[0-9a-fA-F]{64})')).Groups['hash'].Value.ToLowerInvariant()\n"
        << "$channel = if ($release.lts -and $release.lts -ne $false) { 'lts:' + [string]$release.lts } else { 'current' }\n"
        << "$installName = '{0}-{1}' -f $resolvedVersion, $arch\n"
        << "$installPath = Join-Path $rootPath $installName\n"
        << "$nodeExe = Join-Path $installPath 'node.exe'\n"
        << "if (Test-Path $installPath) {\n"
        << "  if (Test-Path $nodeExe) {\n"
        << "    Write-JkmLog ('Runtime already exists at ' + $installPath)\n"
        << "    Write-Output ('status=already_installed')\n"
        << "    Write-Output ('distribution=nodejs')\n"
        << "    Write-Output ('name=' + $installName)\n"
        << "    Write-Output ('root=' + $installPath)\n"
        << "    Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "    Write-Output ('package_name=' + $packageName)\n"
        << "    Write-Output ('download_url=' + $downloadUrl)\n"
        << "    Write-Output ('checksum=' + $checksum)\n"
        << "    Write-Output ('channel=' + $channel)\n"
        << "    Write-Output ('published_at=' + [string]$release.date)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Write-JkmLog ('Cleaning incomplete install at ' + $installPath)\n"
        << "  Remove-Item -Recurse -Force $installPath\n"
        << "}\n"
        << "$archivePath = Join-Path $downloads $packageName\n"
        << "$needsDownload = $true\n"
        << "if (Test-Path $archivePath) {\n"
        << "  $existingHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "  if ($existingHash -eq $checksum) { Write-JkmLog ('Using cached archive ' + $packageName); $needsDownload = $false } else { Remove-Item -Force $archivePath }\n"
        << "}\n"
        << "if ($needsDownload) {\n"
        << "  Write-JkmLog ('Downloading ' + $packageName)\n"
        << "  Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "}\n"
        << "Write-JkmLog ('Verifying archive checksum')\n"
        << "$downloadedHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "if ($downloadedHash -ne $checksum) { throw 'downloaded Node.js archive checksum did not match the published SHASUMS256.txt entry' }\n"
        << "$extractDir = Join-Path $tempRoot ([Guid]::NewGuid().ToString('N'))\n"
        << "New-Item -ItemType Directory -Force -Path $extractDir | Out-Null\n"
        << "try {\n"
        << "  Write-JkmLog ('Extracting archive to ' + $installPath)\n"
        << "  Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force\n"
        << "  $childItems = Get-ChildItem -Path $extractDir -Force\n"
        << "  $singleDirectory = $null\n"
        << "  if ($childItems.Count -eq 1 -and $childItems[0].PSIsContainer) { $singleDirectory = $childItems[0].FullName }\n"
        << "  if ($singleDirectory) {\n"
        << "    Move-Item -Path $singleDirectory -Destination $installPath\n"
        << "  } else {\n"
        << "    New-Item -ItemType Directory -Force -Path $installPath | Out-Null\n"
        << "    Get-ChildItem -Path $extractDir -Force | Move-Item -Destination $installPath\n"
        << "  }\n"
        << "} finally {\n"
        << "  if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }\n"
        << "}\n"
        << "if (-not (Test-Path $nodeExe)) { throw 'installed Node.js runtime is missing node.exe after extraction' }\n"
        << "Write-JkmLog ('Install completed: ' + $installName)\n"
        << "Write-Output ('status=installed')\n"
        << "Write-Output ('distribution=nodejs')\n"
        << "Write-Output ('name=' + $installName)\n"
        << "Write-Output ('root=' + $installPath)\n"
        << "Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "Write-Output ('package_name=' + $packageName)\n"
        << "Write-Output ('download_url=' + $downloadUrl)\n"
        << "Write-Output ('checksum=' + $checksum)\n"
        << "Write-Output ('channel=' + $channel)\n"
        << "Write-Output ('published_at=' + [string]$release.date)\n";
    return script.str();
}

std::string BuildGoRemoteListScript(const std::string& selector, const std::string& arch, int limit) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$limit = " << limit << '\n'
        << "switch ($arch.ToLowerInvariant()) {\n"
        << "  'x64' { $targetArch = 'amd64' }\n"
        << "  default { throw 'only x64 is currently supported for Go installs' }\n"
        << "}\n"
        << "$releases = @((Invoke-RestMethod -Uri 'https://go.dev/dl/?mode=json&include=all' -Headers @{ 'User-Agent' = 'jkm-cpp' }) | Where-Object {\n"
        << "  $_.files | Where-Object { $_.os -eq 'windows' -and $_.arch -eq $targetArch -and $_.kind -eq 'archive' }\n"
        << "})\n"
        << "$normalizedSelector = $selector.Trim()\n"
        << "if ($normalizedSelector.StartsWith('go', [System.StringComparison]::OrdinalIgnoreCase)) { $normalizedSelector = $normalizedSelector.Substring(2) }\n"
        << "$allowPrerelease = $normalizedSelector -match '(?i)(beta|rc)'\n"
        << "if (-not $allowPrerelease) { $releases = @($releases | Where-Object { $_.stable }) }\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' {}\n"
        << "  'latest' {}\n"
        << "  'stable' { $releases = @($releases | Where-Object { $_.stable }) }\n"
        << "  default {\n"
        << "    $releases = @($releases | Where-Object {\n"
        << "      $normalizedVersion = ([string]$_.version)\n"
        << "      if ($normalizedVersion.StartsWith('go')) { $normalizedVersion = $normalizedVersion.Substring(2) }\n"
        << "      $normalizedVersion -eq $normalizedSelector -or $normalizedVersion.StartsWith($normalizedSelector + '.') -or $normalizedVersion.StartsWith($normalizedSelector + 'rc') -or $normalizedVersion.StartsWith($normalizedSelector + 'beta')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "$selected = @($releases | Select-Object -First $limit)\n"
        << "foreach ($release in $selected) {\n"
        << "  $file = $release.files | Where-Object { $_.os -eq 'windows' -and $_.arch -eq $targetArch -and $_.kind -eq 'archive' } | Select-Object -First 1\n"
        << "  if (-not $file) { continue }\n"
        << "  $version = [string]$release.version\n"
        << "  if ($version.StartsWith('go')) { $version = $version.Substring(2) }\n"
        << "  $channel = if ($release.stable) { 'stable' } else { 'prerelease' }\n"
        << "  $fields = @($version, [string]$file.filename, ('https://go.dev/dl/' + [string]$file.filename), [string]$file.sha256, $channel, [string]$release.version) | ForEach-Object { ($_ -replace \"`t\", ' ') -replace \"`r|`n\", ' ' }\n"
        << "  Write-Output ('ITEM' + \"`t\" + ($fields -join \"`t\"))\n"
        << "}\n";
    return script.str();
}

std::string BuildGoInstallScript(const AppPaths& paths, const std::string& selector, const std::string& arch) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildInstallLogHelperScript()
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$downloads = '" << EscapePowerShellLiteral(PathToUtf8(paths.downloads)) << "'\n"
        << "$tempRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.temp)) << "'\n"
        << "$rootPath = '" << EscapePowerShellLiteral(PathToUtf8(paths.installs / "go" / "golang")) << "'\n"
        << "New-Item -ItemType Directory -Force -Path $downloads, $tempRoot, $rootPath | Out-Null\n"
        << "switch ($arch.ToLowerInvariant()) {\n"
        << "  'x64' { $targetArch = 'amd64' }\n"
        << "  default { throw 'only x64 is currently supported for Go installs' }\n"
        << "}\n"
        << "$releases = @((Invoke-RestMethod -Uri 'https://go.dev/dl/?mode=json&include=all' -Headers @{ 'User-Agent' = 'jkm-cpp' }) | Where-Object {\n"
        << "  $_.files | Where-Object { $_.os -eq 'windows' -and $_.arch -eq $targetArch -and $_.kind -eq 'archive' }\n"
        << "})\n"
        << "$normalizedSelector = $selector.Trim()\n"
        << "if ($normalizedSelector.StartsWith('go', [System.StringComparison]::OrdinalIgnoreCase)) { $normalizedSelector = $normalizedSelector.Substring(2) }\n"
        << "$allowPrerelease = $normalizedSelector -match '(?i)(beta|rc)'\n"
        << "if (-not $allowPrerelease) { $releases = @($releases | Where-Object { $_.stable }) }\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' { throw 'selector is required' }\n"
        << "  'latest' {}\n"
        << "  'stable' { $releases = @($releases | Where-Object { $_.stable }) }\n"
        << "  default {\n"
        << "    $releases = @($releases | Where-Object {\n"
        << "      $normalizedVersion = [string]$_.version\n"
        << "      if ($normalizedVersion.StartsWith('go')) { $normalizedVersion = $normalizedVersion.Substring(2) }\n"
        << "      $normalizedVersion -eq $normalizedSelector -or $normalizedVersion.StartsWith($normalizedSelector + '.') -or $normalizedVersion.StartsWith($normalizedSelector + 'rc') -or $normalizedVersion.StartsWith($normalizedSelector + 'beta')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "Write-JkmLog ('Resolving Go release for selector ' + $selector)\n"
        << "$release = $releases | Select-Object -First 1\n"
        << "if (-not $release) { throw \"no Go release matched selector $selector\" }\n"
        << "$file = $release.files | Where-Object { $_.os -eq 'windows' -and $_.arch -eq $targetArch -and $_.kind -eq 'archive' } | Select-Object -First 1\n"
        << "if (-not $file) { throw 'selected Go release did not include a windows-amd64 zip package' }\n"
        << "$resolvedVersion = [string]$release.version\n"
        << "if ($resolvedVersion.StartsWith('go')) { $resolvedVersion = $resolvedVersion.Substring(2) }\n"
        << "Write-JkmLog ('Selected Go version ' + $resolvedVersion)\n"
        << "$packageName = [string]$file.filename\n"
        << "$downloadUrl = 'https://go.dev/dl/' + $packageName\n"
        << "$checksum = ([string]$file.sha256).ToLowerInvariant()\n"
        << "$channel = if ($release.stable) { 'stable' } else { 'prerelease' }\n"
        << "$installName = '{0}-{1}' -f $resolvedVersion, $arch\n"
        << "$installPath = Join-Path $rootPath $installName\n"
        << "$goExe = Join-Path $installPath 'bin\\go.exe'\n"
        << "if (Test-Path $installPath) {\n"
        << "  if (Test-Path $goExe) {\n"
        << "    Write-JkmLog ('Runtime already exists at ' + $installPath)\n"
        << "    Write-Output ('status=already_installed')\n"
        << "    Write-Output ('distribution=golang')\n"
        << "    Write-Output ('name=' + $installName)\n"
        << "    Write-Output ('root=' + $installPath)\n"
        << "    Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "    Write-Output ('package_name=' + $packageName)\n"
        << "    Write-Output ('download_url=' + $downloadUrl)\n"
        << "    Write-Output ('checksum=' + $checksum)\n"
        << "    Write-Output ('channel=' + $channel)\n"
        << "    Write-Output ('published_at=' + [string]$release.version)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Write-JkmLog ('Cleaning incomplete install at ' + $installPath)\n"
        << "  Remove-Item -Recurse -Force $installPath\n"
        << "}\n"
        << "$archivePath = Join-Path $downloads $packageName\n"
        << "$needsDownload = $true\n"
        << "if (Test-Path $archivePath) {\n"
        << "  $existingHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "  if ($existingHash -eq $checksum) { Write-JkmLog ('Using cached archive ' + $packageName); $needsDownload = $false } else { Remove-Item -Force $archivePath }\n"
        << "}\n"
        << "if ($needsDownload) {\n"
        << "  Write-JkmLog ('Downloading ' + $packageName)\n"
        << "  Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "}\n"
        << "Write-JkmLog ('Verifying archive checksum')\n"
        << "$downloadedHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "if ($downloadedHash -ne $checksum) { throw 'downloaded Go archive checksum did not match the official Go manifest' }\n"
        << "$extractDir = Join-Path $tempRoot ([Guid]::NewGuid().ToString('N'))\n"
        << "New-Item -ItemType Directory -Force -Path $extractDir | Out-Null\n"
        << "try {\n"
        << "  Write-JkmLog ('Extracting archive to ' + $installPath)\n"
        << "  Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force\n"
        << "  $goDir = Join-Path $extractDir 'go'\n"
        << "  if (-not (Test-Path $goDir)) { throw 'Go archive did not extract to a go directory' }\n"
        << "  Move-Item -Path $goDir -Destination $installPath\n"
        << "} finally {\n"
        << "  if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }\n"
        << "}\n"
        << "if (-not (Test-Path $goExe)) { throw 'installed Go runtime is missing bin\\go.exe after extraction' }\n"
        << "Write-JkmLog ('Install completed: ' + $installName)\n"
        << "Write-Output ('status=installed')\n"
        << "Write-Output ('distribution=golang')\n"
        << "Write-Output ('name=' + $installName)\n"
        << "Write-Output ('root=' + $installPath)\n"
        << "Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "Write-Output ('package_name=' + $packageName)\n"
        << "Write-Output ('download_url=' + $downloadUrl)\n"
        << "Write-Output ('checksum=' + $checksum)\n"
        << "Write-Output ('channel=' + $channel)\n"
        << "Write-Output ('published_at=' + [string]$release.version)\n";
    return script.str();
}

std::string BuildMavenRemoteListScript(const std::string& selector, int limit) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$limit = " << limit << '\n'
        << "[xml]$metadata = Invoke-WebRequest -Uri 'https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/maven-metadata.xml' -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "$versions = @($metadata.metadata.versioning.versions.version | ForEach-Object { [string]$_ })\n"
        << "$normalizedSelector = $selector.Trim()\n"
        << "$allowPrerelease = $normalizedSelector -match '(?i)(alpha|beta|rc|milestone|m\\d+)' -or $normalizedSelector.Contains('-')\n"
        << "$filtered = if ($allowPrerelease) { $versions } else { $versions | Where-Object { $_ -match '^\\d+(\\.\\d+)+$' } }\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' {}\n"
        << "  'latest' {}\n"
        << "  default {\n"
        << "    $filtered = @($filtered | Where-Object {\n"
        << "      $_ -eq $normalizedSelector -or $_.StartsWith($normalizedSelector + '.') -or $_.StartsWith($normalizedSelector + '-')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "$selected = @($filtered | Select-Object -Last $limit)\n"
        << "[array]::Reverse($selected)\n"
        << "foreach ($version in $selected) {\n"
        << "  $major = ($version -split '\\.')[0]\n"
        << "  $packageName = \"apache-maven-$version-bin.zip\"\n"
        << "  $downloadUrl = \"https://archive.apache.org/dist/maven/maven-$major/$version/binaries/$packageName\"\n"
        << "  $checksum = ''\n"
        << "  try { $checksum = ((Invoke-WebRequest -Uri ($downloadUrl + '.sha512') -Headers @{ 'User-Agent' = 'jkm-cpp' }).Content.Trim().Split(' ')[0]).ToLowerInvariant() } catch { $checksum = '' }\n"
        << "  $channel = if ($version -match '(?i)(alpha|beta|rc|milestone|m\\d+)') { 'prerelease' } else { 'stable' }\n"
        << "  $fields = @($version, $packageName, $downloadUrl, $checksum, $channel, 'n/a') | ForEach-Object { ($_ -replace \"`t\", ' ') -replace \"`r|`n\", ' ' }\n"
        << "  Write-Output ('ITEM' + \"`t\" + ($fields -join \"`t\"))\n"
        << "}\n";
    return script.str();
}

std::string BuildMavenInstallScript(const AppPaths& paths, const std::string& selector) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildInstallLogHelperScript()
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$downloads = '" << EscapePowerShellLiteral(PathToUtf8(paths.downloads)) << "'\n"
        << "$tempRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.temp)) << "'\n"
        << "$rootPath = '" << EscapePowerShellLiteral(PathToUtf8(paths.installs / "maven" / "apache")) << "'\n"
        << "New-Item -ItemType Directory -Force -Path $downloads, $tempRoot, $rootPath | Out-Null\n"
        << "[xml]$metadata = Invoke-WebRequest -Uri 'https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/maven-metadata.xml' -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "$versions = @($metadata.metadata.versioning.versions.version | ForEach-Object { [string]$_ })\n"
        << "$normalizedSelector = $selector.Trim()\n"
        << "$allowPrerelease = $normalizedSelector -match '(?i)(alpha|beta|rc|milestone|m\\d+)' -or $normalizedSelector.Contains('-')\n"
        << "$filtered = if ($allowPrerelease) { $versions } else { $versions | Where-Object { $_ -match '^\\d+(\\.\\d+)+$' } }\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' { throw 'selector is required' }\n"
        << "  'latest' {}\n"
        << "  default {\n"
        << "    $filtered = @($filtered | Where-Object {\n"
        << "      $_ -eq $normalizedSelector -or $_.StartsWith($normalizedSelector + '.') -or $_.StartsWith($normalizedSelector + '-')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "Write-JkmLog ('Resolving Maven release for selector ' + $selector)\n"
        << "$resolvedVersion = $filtered | Select-Object -Last 1\n"
        << "if (-not $resolvedVersion) { throw \"no Maven release matched selector $selector\" }\n"
        << "Write-JkmLog ('Selected Maven version ' + $resolvedVersion)\n"
        << "$major = ($resolvedVersion -split '\\.')[0]\n"
        << "$packageName = \"apache-maven-$resolvedVersion-bin.zip\"\n"
        << "$downloadUrl = \"https://archive.apache.org/dist/maven/maven-$major/$resolvedVersion/binaries/$packageName\"\n"
        << "$checksumResponse = (Invoke-WebRequest -Uri ($downloadUrl + '.sha512') -Headers @{ 'User-Agent' = 'jkm-cpp' }).Content.Trim()\n"
        << "$checksum = ($checksumResponse.Split(' ')[0]).ToLowerInvariant()\n"
        << "$channel = if ($resolvedVersion -match '(?i)(alpha|beta|rc|milestone|m\\d+)') { 'prerelease' } else { 'stable' }\n"
        << "$installPath = Join-Path $rootPath $resolvedVersion\n"
        << "$mvnCmd = Join-Path $installPath 'bin\\mvn.cmd'\n"
        << "if (Test-Path $installPath) {\n"
        << "  if (Test-Path $mvnCmd) {\n"
        << "    Write-JkmLog ('Runtime already exists at ' + $installPath)\n"
        << "    Write-Output ('status=already_installed')\n"
        << "    Write-Output ('distribution=apache')\n"
        << "    Write-Output ('name=' + $resolvedVersion)\n"
        << "    Write-Output ('root=' + $installPath)\n"
        << "    Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "    Write-Output ('package_name=' + $packageName)\n"
        << "    Write-Output ('download_url=' + $downloadUrl)\n"
        << "    Write-Output ('checksum=' + $checksum)\n"
        << "    Write-Output ('channel=' + $channel)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Write-JkmLog ('Cleaning incomplete install at ' + $installPath)\n"
        << "  Remove-Item -Recurse -Force $installPath\n"
        << "}\n"
        << "$archivePath = Join-Path $downloads $packageName\n"
        << "$needsDownload = $true\n"
        << "if (Test-Path $archivePath) {\n"
        << "  $existingHash = (Get-FileHash -Algorithm SHA512 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "  if ($existingHash -eq $checksum) { Write-JkmLog ('Using cached archive ' + $packageName); $needsDownload = $false } else { Remove-Item -Force $archivePath }\n"
        << "}\n"
        << "if ($needsDownload) {\n"
        << "  Write-JkmLog ('Downloading ' + $packageName)\n"
        << "  Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "}\n"
        << "Write-JkmLog ('Verifying archive checksum')\n"
        << "$downloadedHash = (Get-FileHash -Algorithm SHA512 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "if ($downloadedHash -ne $checksum) { throw 'downloaded Maven archive checksum did not match the published .sha512 file' }\n"
        << "$extractDir = Join-Path $tempRoot ([Guid]::NewGuid().ToString('N'))\n"
        << "New-Item -ItemType Directory -Force -Path $extractDir | Out-Null\n"
        << "try {\n"
        << "  Write-JkmLog ('Extracting archive to ' + $installPath)\n"
        << "  Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force\n"
        << "  $childItems = Get-ChildItem -Path $extractDir -Force\n"
        << "  $singleDirectory = $null\n"
        << "  if ($childItems.Count -eq 1 -and $childItems[0].PSIsContainer) { $singleDirectory = $childItems[0].FullName }\n"
        << "  if ($singleDirectory) {\n"
        << "    Move-Item -Path $singleDirectory -Destination $installPath\n"
        << "  } else {\n"
        << "    New-Item -ItemType Directory -Force -Path $installPath | Out-Null\n"
        << "    Get-ChildItem -Path $extractDir -Force | Move-Item -Destination $installPath\n"
        << "  }\n"
        << "} finally {\n"
        << "  if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }\n"
        << "}\n"
        << "if (-not (Test-Path $mvnCmd)) { throw 'installed Maven runtime is missing bin\\mvn.cmd after extraction' }\n"
        << "Write-JkmLog ('Install completed: ' + $resolvedVersion)\n"
        << "Write-Output ('status=installed')\n"
        << "Write-Output ('distribution=apache')\n"
        << "Write-Output ('name=' + $resolvedVersion)\n"
        << "Write-Output ('root=' + $installPath)\n"
        << "Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "Write-Output ('package_name=' + $packageName)\n"
        << "Write-Output ('download_url=' + $downloadUrl)\n"
        << "Write-Output ('checksum=' + $checksum)\n"
        << "Write-Output ('channel=' + $channel)\n";
    return script.str();
}

std::string BuildGradleRemoteListScript(const std::string& selector, int limit) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$limit = " << limit << '\n'
        << "$releases = @((Invoke-RestMethod -Uri 'https://services.gradle.org/versions/all' -Headers @{ 'User-Agent' = 'jkm-cpp' }) | Where-Object {\n"
        << "  -not $_.broken -and -not $_.snapshot -and -not $_.nightly -and -not $_.releaseNightly\n"
        << "})\n"
        << "$normalizedSelector = $selector.Trim()\n"
        << "$allowPrerelease = $normalizedSelector -match '(?i)(rc|milestone)' -or $normalizedSelector.Contains('-')\n"
        << "if (-not $allowPrerelease) {\n"
        << "  $releases = @($releases | Where-Object {\n"
        << "    -not $_.activeRc -and [string]$_.rcFor -eq '' -and [string]$_.milestoneFor -eq '' -and [string]$_.version -notmatch '(?i)(-rc-|milestone)'\n"
        << "  })\n"
        << "}\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' {}\n"
        << "  'latest' {}\n"
        << "  default {\n"
        << "    $releases = @($releases | Where-Object {\n"
        << "      $version = [string]$_.version\n"
        << "      $version -eq $normalizedSelector -or $version.StartsWith($normalizedSelector + '.') -or $version.StartsWith($normalizedSelector + '-')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "$selected = @($releases | Select-Object -First $limit)\n"
        << "foreach ($release in $selected) {\n"
        << "  $version = [string]$release.version\n"
        << "  $downloadUrl = [string]$release.downloadUrl\n"
        << "  $packageName = [System.IO.Path]::GetFileName($downloadUrl)\n"
        << "  $channel = if ($version -match '(?i)(rc|milestone)') { 'prerelease' } else { 'stable' }\n"
        << "  $fields = @($version, $packageName, $downloadUrl, [string]$release.checksum, $channel, [string]$release.buildTime) | ForEach-Object { ($_ -replace \"`t\", ' ') -replace \"`r|`n\", ' ' }\n"
        << "  Write-Output ('ITEM' + \"`t\" + ($fields -join \"`t\"))\n"
        << "}\n";
    return script.str();
}

std::string BuildGradleInstallScript(const AppPaths& paths, const std::string& selector) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildInstallLogHelperScript()
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$downloads = '" << EscapePowerShellLiteral(PathToUtf8(paths.downloads)) << "'\n"
        << "$tempRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.temp)) << "'\n"
        << "$rootPath = '" << EscapePowerShellLiteral(PathToUtf8(paths.installs / "gradle" / "gradle")) << "'\n"
        << "New-Item -ItemType Directory -Force -Path $downloads, $tempRoot, $rootPath | Out-Null\n"
        << "$releases = @((Invoke-RestMethod -Uri 'https://services.gradle.org/versions/all' -Headers @{ 'User-Agent' = 'jkm-cpp' }) | Where-Object {\n"
        << "  -not $_.broken -and -not $_.snapshot -and -not $_.nightly -and -not $_.releaseNightly\n"
        << "})\n"
        << "$normalizedSelector = $selector.Trim()\n"
        << "$allowPrerelease = $normalizedSelector -match '(?i)(rc|milestone)' -or $normalizedSelector.Contains('-')\n"
        << "if (-not $allowPrerelease) {\n"
        << "  $releases = @($releases | Where-Object {\n"
        << "    -not $_.activeRc -and [string]$_.rcFor -eq '' -and [string]$_.milestoneFor -eq '' -and [string]$_.version -notmatch '(?i)(-rc-|milestone)'\n"
        << "  })\n"
        << "}\n"
        << "switch ($normalizedSelector.ToLowerInvariant()) {\n"
        << "  '' { throw 'selector is required' }\n"
        << "  'latest' {}\n"
        << "  default {\n"
        << "    $releases = @($releases | Where-Object {\n"
        << "      $version = [string]$_.version\n"
        << "      $version -eq $normalizedSelector -or $version.StartsWith($normalizedSelector + '.') -or $version.StartsWith($normalizedSelector + '-')\n"
        << "    })\n"
        << "  }\n"
        << "}\n"
        << "Write-JkmLog ('Resolving Gradle release for selector ' + $selector)\n"
        << "$release = $releases | Select-Object -First 1\n"
        << "if (-not $release) { throw \"no Gradle release matched selector $selector\" }\n"
        << "$resolvedVersion = [string]$release.version\n"
        << "Write-JkmLog ('Selected Gradle version ' + $resolvedVersion)\n"
        << "$downloadUrl = [string]$release.downloadUrl\n"
        << "$packageName = [System.IO.Path]::GetFileName($downloadUrl)\n"
        << "$checksum = ([string]$release.checksum).ToLowerInvariant()\n"
        << "$channel = if ($resolvedVersion -match '(?i)(rc|milestone)') { 'prerelease' } else { 'stable' }\n"
        << "$installPath = Join-Path $rootPath $resolvedVersion\n"
        << "$gradleBat = Join-Path $installPath 'bin\\gradle.bat'\n"
        << "if (Test-Path $installPath) {\n"
        << "  if (Test-Path $gradleBat) {\n"
        << "    Write-JkmLog ('Runtime already exists at ' + $installPath)\n"
        << "    Write-Output ('status=already_installed')\n"
        << "    Write-Output ('distribution=gradle')\n"
        << "    Write-Output ('name=' + $resolvedVersion)\n"
        << "    Write-Output ('root=' + $installPath)\n"
        << "    Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "    Write-Output ('package_name=' + $packageName)\n"
        << "    Write-Output ('download_url=' + $downloadUrl)\n"
        << "    Write-Output ('checksum=' + $checksum)\n"
        << "    Write-Output ('channel=' + $channel)\n"
        << "    Write-Output ('published_at=' + [string]$release.buildTime)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Write-JkmLog ('Cleaning incomplete install at ' + $installPath)\n"
        << "  Remove-Item -Recurse -Force $installPath\n"
        << "}\n"
        << "$archivePath = Join-Path $downloads $packageName\n"
        << "$needsDownload = $true\n"
        << "if (Test-Path $archivePath) {\n"
        << "  $existingHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "  if ($existingHash -eq $checksum) { Write-JkmLog ('Using cached archive ' + $packageName); $needsDownload = $false } else { Remove-Item -Force $archivePath }\n"
        << "}\n"
        << "if ($needsDownload) {\n"
        << "  Write-JkmLog ('Downloading ' + $packageName)\n"
        << "  Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "}\n"
        << "Write-JkmLog ('Verifying archive checksum')\n"
        << "$downloadedHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "if ($downloadedHash -ne $checksum) { throw 'downloaded Gradle archive checksum did not match the official release metadata' }\n"
        << "$extractDir = Join-Path $tempRoot ([Guid]::NewGuid().ToString('N'))\n"
        << "New-Item -ItemType Directory -Force -Path $extractDir | Out-Null\n"
        << "try {\n"
        << "  Write-JkmLog ('Extracting archive to ' + $installPath)\n"
        << "  Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force\n"
        << "  $childItems = Get-ChildItem -Path $extractDir -Force\n"
        << "  $singleDirectory = $null\n"
        << "  if ($childItems.Count -eq 1 -and $childItems[0].PSIsContainer) { $singleDirectory = $childItems[0].FullName }\n"
        << "  if ($singleDirectory) {\n"
        << "    Move-Item -Path $singleDirectory -Destination $installPath\n"
        << "  } else {\n"
        << "    New-Item -ItemType Directory -Force -Path $installPath | Out-Null\n"
        << "    Get-ChildItem -Path $extractDir -Force | Move-Item -Destination $installPath\n"
        << "  }\n"
        << "} finally {\n"
        << "  if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }\n"
        << "}\n"
        << "if (-not (Test-Path $gradleBat)) { throw 'installed Gradle runtime is missing bin\\gradle.bat after extraction' }\n"
        << "Write-JkmLog ('Install completed: ' + $resolvedVersion)\n"
        << "Write-Output ('status=installed')\n"
        << "Write-Output ('distribution=gradle')\n"
        << "Write-Output ('name=' + $resolvedVersion)\n"
        << "Write-Output ('root=' + $installPath)\n"
        << "Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "Write-Output ('package_name=' + $packageName)\n"
        << "Write-Output ('download_url=' + $downloadUrl)\n"
        << "Write-Output ('checksum=' + $checksum)\n"
        << "Write-Output ('channel=' + $channel)\n"
        << "Write-Output ('published_at=' + [string]$release.buildTime)\n";
    return script.str();
}

}  // namespace

bool InstallNodeRuntime(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    ToolInstallResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildNodeInstallScript(paths, selector, arch), &process_result, error, BuildInstallOutputLineHandler())) {
        return false;
    }
    return ParseToolInstallResult(RuntimeType::Node, "nodejs", process_result, result, error);
}

bool QueryNodeRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildNodeRemoteListScript(selector, arch, limit), &process_result, error)) {
        return false;
    }
    return ParseRemoteReleases(process_result, result, error);
}

bool InstallGoRuntime(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    ToolInstallResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildGoInstallScript(paths, selector, arch), &process_result, error, BuildInstallOutputLineHandler())) {
        return false;
    }
    return ParseToolInstallResult(RuntimeType::Go, "golang", process_result, result, error);
}

bool QueryGoRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildGoRemoteListScript(selector, arch, limit), &process_result, error)) {
        return false;
    }
    return ParseRemoteReleases(process_result, result, error);
}

bool InstallMavenRuntime(
    const AppPaths& paths,
    const std::string& selector,
    ToolInstallResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildMavenInstallScript(paths, selector), &process_result, error, BuildInstallOutputLineHandler())) {
        return false;
    }
    return ParseToolInstallResult(RuntimeType::Maven, "apache", process_result, result, error);
}

bool QueryMavenRemoteReleases(
    const std::string& selector,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildMavenRemoteListScript(selector, limit), &process_result, error)) {
        return false;
    }
    return ParseRemoteReleases(process_result, result, error);
}

bool InstallGradleRuntime(
    const AppPaths& paths,
    const std::string& selector,
    ToolInstallResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildGradleInstallScript(paths, selector), &process_result, error, BuildInstallOutputLineHandler())) {
        return false;
    }
    return ParseToolInstallResult(RuntimeType::Gradle, "gradle", process_result, result, error);
}

bool QueryGradleRemoteReleases(
    const std::string& selector,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildGradleRemoteListScript(selector, limit), &process_result, error)) {
        return false;
    }
    return ParseRemoteReleases(process_result, result, error);
}

}  // namespace jkm
