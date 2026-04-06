#include "providers/temurin.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "infrastructure/process.h"
#include "infrastructure/platform_windows.h"
#include "providers/download_helpers.h"

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

        const auto key = line.substr(0, split);
        const auto value = line.substr(split + 1);
        values[key] = value;
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

std::vector<int> ParseCommaSeparatedInts(const std::string& value) {
    std::vector<int> numbers;
    for (const auto& part : Split(value, ',')) {
        if (part.empty()) {
            continue;
        }
        numbers.push_back(std::stoi(part));
    }
    return numbers;
}

std::string ExtractFriendlyPowerShellError(const std::string& output) {
    const auto values = ParseKeyValueOutput(output);
    const auto error_it = values.find("ERROR");
    if (error_it != values.end() && !error_it->second.empty()) {
        return error_it->second;
    }

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

std::string WrapPowerShellScript(const std::string& body) {
    return
        "try {\n" +
        body +
        "} catch {\n"
        "  $message = [string]$_.Exception.Message\n"
        "  if ([string]::IsNullOrWhiteSpace($message)) { $message = ($_ | Out-String) }\n"
        "  Write-Output ('ERROR=' + $message.Trim())\n"
        "  exit 1\n"
        "}\n";
}

// Temurin uses a richer selector matcher than the other providers, so its install/query scripts share one helper block.
std::string BuildTemurinSelectorHelperScript() {
    return
        BuildDownloadHelperScript() +
        "function Get-JkmTemurinFeatureMajor {\n"
        "  param([string]$Selector)\n"
        "  $normalized = $Selector.Trim().ToLowerInvariant()\n"
        "  if ($normalized -match '^(?:jdk-?)?(?:1\\.)?(?<major>\\d+)') { return [int]$Matches['major'] }\n"
        "  throw 'selector must start with a Java feature version'\n"
        "}\n"
        "function Get-JkmTemurinSelectorVariants {\n"
        "  param([string]$Selector)\n"
        "  $normalized = $Selector.Trim().ToLowerInvariant()\n"
        "  $withoutPrefix = $normalized -replace '^jdk-?', ''\n"
        "  $withoutLegacy = $withoutPrefix -replace '^1\\.(?=\\d)', ''\n"
        "  $variants = @(\n"
        "    $normalized,\n"
        "    $withoutPrefix,\n"
        "    $withoutLegacy,\n"
        "    ($normalized -replace '_', '+'),\n"
        "    ($withoutPrefix -replace '_', '+'),\n"
        "    ($withoutLegacy -replace '_', '+'),\n"
        "    ($normalized -replace '\\+', '_'),\n"
        "    ($withoutPrefix -replace '\\+', '_'),\n"
        "    ($withoutLegacy -replace '\\+', '_')\n"
        "  ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }\n"
        "  return $variants | Select-Object -Unique\n"
        "}\n"
        "function Get-JkmTemurinAssetVariants {\n"
        "  param($Asset)\n"
        "  $releaseName = ([string]$Asset.release_name).ToLowerInvariant()\n"
        "  $openjdkVersion = ([string]$Asset.version_data.openjdk_version).ToLowerInvariant()\n"
        "  $semver = ([string]$Asset.version_data.semver).ToLowerInvariant()\n"
        "  $packageName = ''\n"
        "  if ($Asset.binaries -and $Asset.binaries.Count -gt 0 -and $Asset.binaries[0].package) {\n"
        "    $packageName = ([string]$Asset.binaries[0].package.name).ToLowerInvariant()\n"
        "  }\n"
        "  $variants = @(\n"
        "    $releaseName,\n"
        "    ($releaseName -replace '^jdk-?', ''),\n"
        "    $openjdkVersion,\n"
        "    ($openjdkVersion -replace '-lts$', ''),\n"
        "    (($openjdkVersion -replace '-lts$', '') -replace '^1\\.(?=\\d)', ''),\n"
        "    $semver,\n"
        "    ($semver -replace '\\.0\\.lts$', ''),\n"
        "    $packageName,\n"
        "    ($packageName -replace '^openjdk', '')\n"
        "  ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }\n"
        "  return $variants | Select-Object -Unique\n"
        "}\n"
        "function Test-JkmTemurinAssetMatch {\n"
        "  param($Asset, [string[]]$SelectorVariants)\n"
        "  $assetVariants = Get-JkmTemurinAssetVariants $Asset\n"
        "  foreach ($selectorVariant in $SelectorVariants) {\n"
        "    foreach ($assetVariant in $assetVariants) {\n"
        "      if ($assetVariant.StartsWith($selectorVariant, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }\n"
        "    }\n"
        "  }\n"
        "  return $false\n"
        "}\n";
}

std::string BuildInstallScript(const AppPaths& paths, const std::string& selector, const std::string& arch) {
    std::ostringstream body;
    body
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildTemurinSelectorHelperScript()
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$root = '" << EscapePowerShellLiteral(PathToUtf8(paths.root)) << "'\n"
        << "$downloads = '" << EscapePowerShellLiteral(PathToUtf8(paths.downloads)) << "'\n"
        << "$tempRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.temp)) << "'\n"
        << "$javaRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.installs / "java" / "temurin")) << "'\n"
        << "New-Item -ItemType Directory -Force -Path $downloads | Out-Null\n"
        << "New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null\n"
        << "New-Item -ItemType Directory -Force -Path $javaRoot | Out-Null\n"
        << "Write-JkmLog ('Resolving Temurin release for selector ' + $selector)\n"
        << "$major = Get-JkmTemurinFeatureMajor $selector\n"
        << "$selectorVariants = Get-JkmTemurinSelectorVariants $selector\n"
        << "$pageSize = 100\n"
        << "$temurinBaseUrl = Get-JkmSourceBaseUrl -EnvironmentVariableName 'JDKM_SOURCE_TEMURIN_BASE_URL' -DefaultBaseUrl 'https://api.adoptium.net'\n"
        << "$uri = Join-JkmUri $temurinBaseUrl \"/v3/assets/feature_releases/$major/ga?architecture=$arch&os=windows&image_type=jdk&jvm_impl=hotspot&heap_size=normal&project=jdk&sort_method=DATE&sort_order=DESC&page_size=$pageSize\"\n"
        << "$releases = Invoke-JkmRestMethod -Uri $uri\n"
        << "if (-not $releases) { throw 'no Temurin releases were returned by the Adoptium API' }\n"
        << "$releases = @($releases | Where-Object { Test-JkmTemurinAssetMatch $_ $selectorVariants })\n"
        << "$asset = $releases[0]\n"
        << "if (-not $asset) { throw \"no Temurin release matched selector $selector\" }\n"
        << "$package = $asset.binaries[0].package\n"
        << "if (-not $package) { throw 'selected Temurin release did not include a downloadable zip package' }\n"
        << "$releaseName = ($asset.release_name -replace '^jdk-', '')\n"
        << "Write-JkmLog ('Selected Temurin release ' + $releaseName)\n"
        << "$installName = '{0}-{1}' -f $releaseName, $arch\n"
        << "$installPath = Join-Path $javaRoot $installName\n"
        << "$javaExe = Join-Path $installPath 'bin\\java.exe'\n"
        << "if (Test-Path $installPath) {\n"
        << "  if (Test-Path $javaExe) {\n"
        << "    Write-JkmLog ('Runtime already exists at ' + $installPath)\n"
        << "    Write-Output ('status=already_installed')\n"
        << "    Write-Output ('distribution=temurin')\n"
        << "    Write-Output ('name=' + $installName)\n"
        << "    Write-Output ('root=' + $installPath)\n"
        << "    Write-Output ('release_name=' + $releaseName)\n"
        << "    Write-Output ('package_name=' + $package.name)\n"
        << "    Write-Output ('download_url=' + $package.link)\n"
        << "    Write-Output ('checksum=' + $package.checksum)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Write-JkmLog ('Cleaning incomplete install at ' + $installPath)\n"
        << "  Remove-Item -Recurse -Force $installPath\n"
        << "}\n"
        << "$archivePath = Join-Path $downloads $package.name\n"
        << "$expectedHash = ([string]$package.checksum).ToLowerInvariant()\n"
        << "$needsDownload = $true\n"
        << "if (Test-Path $archivePath) {\n"
        << "  $existingHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "  if ($existingHash -eq $expectedHash) { Write-JkmLog ('Using cached archive ' + $package.name); $needsDownload = $false }\n"
        << "  else { Remove-Item -Force $archivePath }\n"
        << "}\n"
        << "if ($needsDownload) {\n"
        << "  Write-JkmLog ('Downloading ' + $package.name)\n"
        << "  Download-JkmFile -Uri $package.link -Destination $archivePath -Label $package.name\n"
        << "}\n"
        << "Write-JkmLog ('Verifying archive checksum')\n"
        << "$downloadedHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "if ($downloadedHash -ne $expectedHash) { throw 'downloaded archive checksum did not match the API metadata' }\n"
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
        << "if (-not (Test-Path $javaExe)) { throw 'installed JDK is missing bin\\java.exe after extraction' }\n"
        << "Write-JkmLog ('Install completed: ' + $installName)\n"
        << "Write-Output ('status=installed')\n"
        << "Write-Output ('distribution=temurin')\n"
        << "Write-Output ('name=' + $installName)\n"
        << "Write-Output ('root=' + $installPath)\n"
        << "Write-Output ('release_name=' + $releaseName)\n"
        << "Write-Output ('package_name=' + $package.name)\n"
        << "Write-Output ('download_url=' + $package.link)\n"
        << "Write-Output ('checksum=' + $package.checksum)\n";

    return WrapPowerShellScript(body.str());
}

std::string BuildAvailableReleasesScript() {
    return WrapPowerShellScript(
        std::string(
            "$ErrorActionPreference = 'Stop'\n"
            "$ProgressPreference = 'SilentlyContinue'\n"
            "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n") +
        BuildDownloadHelperScript() +
        "$temurinBaseUrl = Get-JkmSourceBaseUrl -EnvironmentVariableName 'JDKM_SOURCE_TEMURIN_BASE_URL' -DefaultBaseUrl 'https://api.adoptium.net'\n"
        "$info = Invoke-JkmRestMethod -Uri (Join-JkmUri $temurinBaseUrl '/v3/info/available_releases')\n"
        "Write-Output ('MOST_RECENT_FEATURE_RELEASE=' + $info.most_recent_feature_release)\n"
        "Write-Output ('MOST_RECENT_LTS=' + $info.most_recent_lts)\n"
        "Write-Output ('AVAILABLE_RELEASES=' + (($info.available_releases | ForEach-Object { [string]$_ }) -join ','))\n"
        "Write-Output ('AVAILABLE_LTS_RELEASES=' + (($info.available_lts_releases | ForEach-Object { [string]$_ }) -join ','))\n");
}

std::string BuildRemoteListScript(const std::string& selector, const std::string& arch, int limit) {
    std::ostringstream body;
    body
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildTemurinSelectorHelperScript()
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$limit = " << limit << '\n'
        << "$major = Get-JkmTemurinFeatureMajor $selector\n"
        << "$selectorVariants = Get-JkmTemurinSelectorVariants $selector\n"
        << "$pageSize = [Math]::Max($limit, 100)\n"
        << "$temurinBaseUrl = Get-JkmSourceBaseUrl -EnvironmentVariableName 'JDKM_SOURCE_TEMURIN_BASE_URL' -DefaultBaseUrl 'https://api.adoptium.net'\n"
        << "$uri = Join-JkmUri $temurinBaseUrl \"/v3/assets/feature_releases/$major/ga?architecture=$arch&os=windows&image_type=jdk&jvm_impl=hotspot&heap_size=normal&project=jdk&sort_method=DATE&sort_order=DESC&page_size=$pageSize\"\n"
        << "$releases = Invoke-JkmRestMethod -Uri $uri\n"
        << "if (-not $releases) { throw 'no Temurin releases were returned by the Adoptium API' }\n"
        << "$releases = @($releases | Where-Object { Test-JkmTemurinAssetMatch $_ $selectorVariants })\n"
        << "$selected = if ($releases.Count -gt $limit) { $releases[0..($limit - 1)] } else { $releases }\n"
        << "foreach ($asset in $selected) {\n"
        << "  $package = $asset.binaries[0].package\n"
        << "  if (-not $package) { continue }\n"
        << "  $fields = @(\n"
        << "    ($asset.release_name -replace '^jdk-', ''),\n"
        << "    [string]$asset.version_data.openjdk_version,\n"
        << "    [string]$asset.version_data.semver,\n"
        << "    [string]$package.name,\n"
        << "    [string]$package.link,\n"
        << "    [string]$package.checksum,\n"
        << "    [string]$asset.updated_at\n"
        << "  ) | ForEach-Object { ($_ -replace \"`t\", ' ') -replace \"`r|`n\", ' ' }\n"
        << "  Write-Output ('ITEM' + \"`t\" + ($fields -join \"`t\"))\n"
        << "}\n";
    return WrapPowerShellScript(body.str());
}

}  // namespace

bool InstallTemurinJdk(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    JavaInstallResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(
            BuildInstallScript(paths, selector, arch),
            &process_result,
            error,
            BuildDownloadOutputLineHandler())) {
        return false;
    }

    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            const auto friendly = ExtractFriendlyPowerShellError(process_result.output);
            *error = friendly.empty() ? "Temurin installer script failed" : friendly;
        }
        return false;
    }

    const auto values = ParseKeyValueOutput(process_result.output);
    const auto name_it = values.find("name");
    const auto root_it = values.find("root");
    if (name_it == values.end() || root_it == values.end()) {
        if (error != nullptr) {
            *error = "Temurin installer returned incomplete metadata";
        }
        return false;
    }

    JavaInstallResult install_result;
    install_result.runtime = InstalledRuntime{
        RuntimeType::Java,
        values.contains("distribution") ? values.at("distribution") : "temurin",
        name_it->second,
        PathFromUtf8(root_it->second),
        name_it->second,
        PathFromUtf8(root_it->second),
        false,
        false
    };
    install_result.already_installed = values.contains("status") && values.at("status") == "already_installed";
    if (values.contains("release_name")) {
        install_result.release_name = values.at("release_name");
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
    install_result.raw_output = process_result.output;

    if (result != nullptr) {
        *result = std::move(install_result);
    }
    return true;
}

bool QueryTemurinAvailableReleases(
    TemurinAvailableReleases* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildAvailableReleasesScript(), &process_result, error)) {
        return false;
    }

    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            const auto friendly = ExtractFriendlyPowerShellError(process_result.output);
            *error = friendly.empty() ? "Temurin available releases query failed" : friendly;
        }
        return false;
    }

    const auto values = ParseKeyValueOutput(process_result.output);
    TemurinAvailableReleases parsed;
    if (values.contains("AVAILABLE_RELEASES")) {
        parsed.available_releases = ParseCommaSeparatedInts(values.at("AVAILABLE_RELEASES"));
    }
    if (values.contains("AVAILABLE_LTS_RELEASES")) {
        parsed.lts_releases = ParseCommaSeparatedInts(values.at("AVAILABLE_LTS_RELEASES"));
    }
    if (values.contains("MOST_RECENT_FEATURE_RELEASE")) {
        parsed.most_recent_feature_release = std::stoi(values.at("MOST_RECENT_FEATURE_RELEASE"));
    }
    if (values.contains("MOST_RECENT_LTS")) {
        parsed.most_recent_lts = std::stoi(values.at("MOST_RECENT_LTS"));
    }

    if (result != nullptr) {
        *result = std::move(parsed);
    }
    return true;
}

bool QueryTemurinRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<JavaRemoteRelease>* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildRemoteListScript(selector, arch, limit), &process_result, error)) {
        return false;
    }

    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            const auto friendly = ExtractFriendlyPowerShellError(process_result.output);
            *error = friendly.empty() ? "Temurin remote release query failed" : friendly;
        }
        return false;
    }

    std::vector<JavaRemoteRelease> releases;
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
        if (fields.size() < 7) {
            continue;
        }

        releases.push_back(JavaRemoteRelease{
            fields[0],
            fields[1],
            fields[2],
            fields[3],
            fields[4],
            fields[5],
            fields[6]
        });
    }

    if (result != nullptr) {
        *result = std::move(releases);
    }
    return true;
}

}  // namespace jkm
