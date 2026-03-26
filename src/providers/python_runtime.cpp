#include "providers/python_runtime.h"

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

std::string BuildInstallScript(const AppPaths& paths, const std::string& selector, const std::string& arch) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << BuildInstallLogHelperScript()
        << "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$downloads = '" << EscapePowerShellLiteral(PathToUtf8(paths.downloads)) << "'\n"
        << "$tempRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.temp)) << "'\n"
        << "$pythonRoot = '" << EscapePowerShellLiteral(PathToUtf8(paths.installs / "python" / "cpython")) << "'\n"
        << "New-Item -ItemType Directory -Force -Path $downloads | Out-Null\n"
        << "New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null\n"
        << "New-Item -ItemType Directory -Force -Path $pythonRoot | Out-Null\n"
        << "switch ($arch.ToLowerInvariant()) {\n"
        << "  'x64' { $packageId = 'python' }\n"
        << "  default { throw 'only x64 is currently supported for Python installs' }\n"
        << "}\n"
        << "Write-JkmLog ('Resolving Python package for selector ' + $selector)\n"
        << "$indexUri = \"https://api.nuget.org/v3-flatcontainer/$packageId/index.json\"\n"
        << "$index = Invoke-RestMethod -Uri $indexUri -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "$versions = @($index.versions | ForEach-Object { [string]$_ })\n"
        << "if (-not $versions -or $versions.Count -eq 0) { throw 'the Python package index returned no versions' }\n"
        << "$selector = $selector.Trim()\n"
        << "$dotCount = ($selector.ToCharArray() | Where-Object { $_ -eq '.' }).Count\n"
        << "$allowPrerelease = $selector.Contains('-')\n"
        << "$filtered = if ($allowPrerelease) { $versions } else { $versions | Where-Object { $_ -notmatch '-' } }\n"
        << "$resolvedVersion = $null\n"
        << "if ($dotCount -ge 2) {\n"
        << "  if ($filtered -contains $selector) { $resolvedVersion = $selector }\n"
        << "} else {\n"
        << "  $prefix = \"$selector.\"\n"
        << "  $candidates = @($filtered | Where-Object { $_.StartsWith($prefix) })\n"
        << "  if ($candidates.Count -gt 0) { $resolvedVersion = $candidates[-1] }\n"
        << "}\n"
        << "if (-not $resolvedVersion) { throw \"no Python package matched selector $selector\" }\n"
        << "Write-JkmLog ('Selected Python version ' + $resolvedVersion)\n"
        << "$normalizedVersion = $resolvedVersion.ToLowerInvariant()\n"
        << "$packageName = \"$packageId.$normalizedVersion.nupkg\"\n"
        << "$packageUri = \"https://api.nuget.org/v3-flatcontainer/$packageId/$normalizedVersion/$packageName\"\n"
        << "$checksumUri = \"$packageUri.sha512\"\n"
        << "$expectedChecksum = ''\n"
        << "$expectedChecksumHex = ''\n"
        << "try {\n"
        << "  $expectedChecksum = ((Invoke-WebRequest -Uri $checksumUri -Headers @{ 'User-Agent' = 'jkm-cpp' }).Content.Trim())\n"
        << "  if ($expectedChecksum) {\n"
        << "    $expectedChecksumHex = ([System.BitConverter]::ToString([System.Convert]::FromBase64String($expectedChecksum))).Replace('-', '').ToLowerInvariant()\n"
        << "  }\n"
        << "} catch {\n"
        << "  $expectedChecksum = ''\n"
        << "  $expectedChecksumHex = ''\n"
        << "}\n"
        << "$installName = '{0}-{1}' -f $resolvedVersion, $arch\n"
        << "$installPath = Join-Path $pythonRoot $installName\n"
        << "$pythonExe = Join-Path $installPath 'python.exe'\n"
        << "if (Test-Path $installPath) {\n"
        << "  if (Test-Path $pythonExe) {\n"
        << "    Write-JkmLog ('Runtime already exists at ' + $installPath)\n"
        << "    Write-Output ('status=already_installed')\n"
        << "    Write-Output ('distribution=cpython')\n"
        << "    Write-Output ('name=' + $installName)\n"
        << "    Write-Output ('root=' + $installPath)\n"
        << "    Write-Output ('base_name=' + $installName)\n"
        << "    Write-Output ('base_root=' + $installPath)\n"
        << "    Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "    Write-Output ('package_id=' + $packageId)\n"
        << "    Write-Output ('package_name=' + $packageName)\n"
        << "    Write-Output ('download_url=' + $packageUri)\n"
        << "    Write-Output ('checksum_sha512=' + $expectedChecksum)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Write-JkmLog ('Cleaning incomplete install at ' + $installPath)\n"
        << "  Remove-Item -Recurse -Force $installPath\n"
        << "}\n"
        << "$archivePath = Join-Path $downloads $packageName\n"
        << "$needsDownload = $true\n"
        << "if (Test-Path $archivePath) {\n"
        << "  if ($expectedChecksumHex) {\n"
        << "    $existingHash = (Get-FileHash -Algorithm SHA512 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "    if ($existingHash -eq $expectedChecksumHex) { Write-JkmLog ('Using cached archive ' + $packageName); $needsDownload = $false } else { Remove-Item -Force $archivePath }\n"
        << "  } else {\n"
        << "    Write-JkmLog ('Using cached archive ' + $packageName)\n"
        << "    $needsDownload = $false\n"
        << "  }\n"
        << "}\n"
        << "if ($needsDownload) {\n"
        << "  Write-JkmLog ('Downloading ' + $packageName)\n"
        << "  Invoke-WebRequest -Uri $packageUri -OutFile $archivePath -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "}\n"
        << "if ($expectedChecksumHex) {\n"
        << "  Write-JkmLog ('Verifying package checksum')\n"
        << "  $downloadedHash = (Get-FileHash -Algorithm SHA512 -Path $archivePath).Hash.ToLowerInvariant()\n"
        << "  if ($downloadedHash -ne $expectedChecksumHex) { throw 'downloaded Python package checksum did not match the NuGet metadata' }\n"
        << "}\n"
        << "$extractDir = Join-Path $tempRoot ([Guid]::NewGuid().ToString('N'))\n"
        << "New-Item -ItemType Directory -Force -Path $extractDir | Out-Null\n"
        << "try {\n"
        << "  Write-JkmLog ('Extracting package to ' + $installPath)\n"
        << "  [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $extractDir)\n"
        << "  $toolsDir = Join-Path $extractDir 'tools'\n"
        << "  if (-not (Test-Path (Join-Path $toolsDir 'python.exe'))) { throw 'the Python package did not contain tools\\\\python.exe' }\n"
        << "  New-Item -ItemType Directory -Force -Path $installPath | Out-Null\n"
        << "  Get-ChildItem -Path $toolsDir -Force | Move-Item -Destination $installPath\n"
        << "  New-Item -ItemType Directory -Force -Path (Join-Path $installPath 'envs') | Out-Null\n"
        << "} finally {\n"
        << "  if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }\n"
        << "}\n"
        << "if (-not (Test-Path $pythonExe)) { throw 'installed Python runtime is missing python.exe after extraction' }\n"
        << "Write-JkmLog ('Install completed: ' + $installName)\n"
        << "Write-Output ('status=installed')\n"
        << "Write-Output ('distribution=cpython')\n"
        << "Write-Output ('name=' + $installName)\n"
        << "Write-Output ('root=' + $installPath)\n"
        << "Write-Output ('base_name=' + $installName)\n"
        << "Write-Output ('base_root=' + $installPath)\n"
        << "Write-Output ('resolved_version=' + $resolvedVersion)\n"
        << "Write-Output ('package_id=' + $packageId)\n"
        << "Write-Output ('package_name=' + $packageName)\n"
        << "Write-Output ('download_url=' + $packageUri)\n"
        << "Write-Output ('checksum_sha512=' + $expectedChecksum)\n";

    return script.str();
}

std::string BuildRemoteListScript(const std::string& selector, const std::string& arch, int limit) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << "$selector = '" << EscapePowerShellLiteral(selector) << "'\n"
        << "$arch = '" << EscapePowerShellLiteral(arch) << "'\n"
        << "$limit = " << limit << '\n'
        << "switch ($arch.ToLowerInvariant()) {\n"
        << "  'x64' { $packageId = 'python' }\n"
        << "  default { throw 'only x64 is currently supported for Python remote queries' }\n"
        << "}\n"
        << "$indexUri = \"https://api.nuget.org/v3-flatcontainer/$packageId/index.json\"\n"
        << "$index = Invoke-RestMethod -Uri $indexUri -Headers @{ 'User-Agent' = 'jkm-cpp' }\n"
        << "$versions = @($index.versions | ForEach-Object { [string]$_ })\n"
        << "if (-not $versions -or $versions.Count -eq 0) { throw 'the Python package index returned no versions' }\n"
        << "$selector = $selector.Trim()\n"
        << "$allowPrerelease = $selector.Contains('-')\n"
        << "$filtered = if ($allowPrerelease) { $versions } else { $versions | Where-Object { $_ -notmatch '-' } }\n"
        << "if ($selector) {\n"
        << "  $prefix = if ($selector.EndsWith('.')) { $selector } else { \"$selector\" }\n"
        << "  $filtered = $filtered | Where-Object {\n"
        << "    $_ -eq $selector -or $_.StartsWith($selector + '.') -or $_.StartsWith($selector + '-')\n"
        << "  }\n"
        << "}\n"
        << "$selected = @($filtered | Select-Object -Last $limit)\n"
        << "[array]::Reverse($selected)\n"
        << "foreach ($version in $selected) {\n"
        << "  $normalizedVersion = $version.ToLowerInvariant()\n"
        << "  $packageName = \"$packageId.$normalizedVersion.nupkg\"\n"
        << "  $packageUri = \"https://api.nuget.org/v3-flatcontainer/$packageId/$normalizedVersion/$packageName\"\n"
        << "  $isPrerelease = if ($version.Contains('-')) { 'true' } else { 'false' }\n"
        << "  Write-Output ('ITEM' + \"`t\" + $version + \"`t\" + $packageId + \"`t\" + $packageName + \"`t\" + $packageUri + \"`t\" + $isPrerelease)\n"
        << "}\n";
    return script.str();
}

std::string BuildCreateEnvironmentScript(const InstalledRuntime& base_runtime, const std::string& env_name) {
    std::ostringstream script;
    script
        << "$ErrorActionPreference = 'Stop'\n"
        << "$ProgressPreference = 'SilentlyContinue'\n"
        << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n"
        << "$basePath = '" << EscapePowerShellLiteral(PathToUtf8(base_runtime.root)) << "'\n"
        << "$baseName = '" << EscapePowerShellLiteral(base_runtime.name) << "'\n"
        << "$distribution = '" << EscapePowerShellLiteral(base_runtime.distribution) << "'\n"
        << "$envName = '" << EscapePowerShellLiteral(env_name) << "'\n"
        << "$pythonExe = Join-Path $basePath 'python.exe'\n"
        << "$envPath = Join-Path (Join-Path $basePath 'envs') $envName\n"
        << "$envPython = Join-Path $envPath 'Scripts\\python.exe'\n"
        << "if (-not (Test-Path $pythonExe)) { throw 'selected base Python runtime is missing python.exe' }\n"
        << "if (Test-Path $envPath) {\n"
        << "  if (Test-Path $envPython) {\n"
        << "    Write-Output ('status=already_created')\n"
        << "    Write-Output ('distribution=' + $distribution)\n"
        << "    Write-Output ('name=' + $envName)\n"
        << "    Write-Output ('root=' + $envPath)\n"
        << "    Write-Output ('base_name=' + $baseName)\n"
        << "    Write-Output ('base_root=' + $basePath)\n"
        << "    exit 0\n"
        << "  }\n"
        << "  Remove-Item -Recurse -Force $envPath\n"
        << "}\n"
        << "New-Item -ItemType Directory -Force -Path (Split-Path -Parent $envPath) | Out-Null\n"
        << "& $pythonExe -m venv $envPath\n"
        << "if ($LASTEXITCODE -ne 0) { throw 'python -m venv failed' }\n"
        << "if (-not (Test-Path $envPython)) { throw 'created environment is missing Scripts\\\\python.exe' }\n"
        << "Write-Output ('status=created')\n"
        << "Write-Output ('distribution=' + $distribution)\n"
        << "Write-Output ('name=' + $envName)\n"
        << "Write-Output ('root=' + $envPath)\n"
        << "Write-Output ('base_name=' + $baseName)\n"
        << "Write-Output ('base_root=' + $basePath)\n";

    return script.str();
}

}  // namespace

bool InstallPythonRuntime(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    PythonInstallResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(
            BuildInstallScript(paths, selector, arch),
            &process_result,
            error,
            [](const std::string& line) {
                if (line.rfind("LOG=", 0) == 0) {
                    std::cout << line.substr(4) << std::endl;
                }
            })) {
        return false;
    }

    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            *error = process_result.output.empty() ? "Python install script failed" : process_result.output;
        }
        return false;
    }

    const auto values = ParseKeyValueOutput(process_result.output);
    const auto name_it = values.find("name");
    const auto root_it = values.find("root");
    if (name_it == values.end() || root_it == values.end()) {
        if (error != nullptr) {
            *error = "Python installer returned incomplete metadata";
        }
        return false;
    }

    PythonInstallResult install_result;
    install_result.runtime = InstalledRuntime{
        RuntimeType::Python,
        values.contains("distribution") ? values.at("distribution") : "cpython",
        name_it->second,
        PathFromUtf8(root_it->second),
        values.contains("base_name") ? values.at("base_name") : name_it->second,
        values.contains("base_root") ? PathFromUtf8(values.at("base_root")) : PathFromUtf8(root_it->second),
        false,
        false
    };
    install_result.already_installed = values.contains("status") && values.at("status") == "already_installed";
    if (values.contains("resolved_version")) {
        install_result.resolved_version = values.at("resolved_version");
    }
    if (values.contains("package_id")) {
        install_result.package_id = values.at("package_id");
    }
    if (values.contains("package_name")) {
        install_result.package_name = values.at("package_name");
    }
    if (values.contains("download_url")) {
        install_result.download_url = values.at("download_url");
    }
    if (values.contains("checksum_sha512")) {
        install_result.checksum_sha512_base64 = values.at("checksum_sha512");
    }
    install_result.raw_output = process_result.output;

    if (result != nullptr) {
        *result = std::move(install_result);
    }
    return true;
}

bool QueryPythonRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<PythonRemoteRelease>* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildRemoteListScript(selector, arch, limit), &process_result, error)) {
        return false;
    }

    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            *error = process_result.output.empty() ? "Python remote release query failed" : process_result.output;
        }
        return false;
    }

    std::vector<PythonRemoteRelease> releases;
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
        if (fields.size() < 5) {
            continue;
        }

        releases.push_back(PythonRemoteRelease{
            fields[0],
            fields[1],
            fields[2],
            fields[3],
            fields[4] == "true"
        });
    }

    if (result != nullptr) {
        *result = std::move(releases);
    }
    return true;
}

bool CreatePythonEnvironment(
    const InstalledRuntime& base_runtime,
    const std::string& env_name,
    PythonEnvironmentResult* result,
    std::string* error) {
    ProcessResult process_result;
    if (!RunPowerShellScript(BuildCreateEnvironmentScript(base_runtime, env_name), &process_result, error)) {
        return false;
    }

    if (process_result.exit_code != 0) {
        if (error != nullptr) {
            *error = process_result.output.empty() ? "Python environment creation failed" : process_result.output;
        }
        return false;
    }

    const auto values = ParseKeyValueOutput(process_result.output);
    const auto name_it = values.find("name");
    const auto root_it = values.find("root");
    if (name_it == values.end() || root_it == values.end()) {
        if (error != nullptr) {
            *error = "Python environment creation returned incomplete metadata";
        }
        return false;
    }

    PythonEnvironmentResult environment_result;
    environment_result.base_runtime = base_runtime;
    environment_result.runtime = InstalledRuntime{
        RuntimeType::Python,
        values.contains("distribution") ? values.at("distribution") : base_runtime.distribution,
        name_it->second,
        PathFromUtf8(root_it->second),
        values.contains("base_name") ? values.at("base_name") : base_runtime.name,
        values.contains("base_root") ? PathFromUtf8(values.at("base_root")) : base_runtime.root,
        true,
        false
    };
    environment_result.already_created = values.contains("status") && values.at("status") == "already_created";
    environment_result.raw_output = process_result.output;

    if (result != nullptr) {
        *result = std::move(environment_result);
    }
    return true;
}

}  // namespace jkm
