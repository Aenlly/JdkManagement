#pragma once

#include <string>
#include <vector>

#include "core/store.h"

namespace jkm {

struct PythonInstallResult {
    InstalledRuntime runtime;
    bool already_installed{false};
    std::string resolved_version;
    std::string package_id;
    std::string package_name;
    std::string download_url;
    std::string checksum_sha512_base64;
    std::string raw_output;
};

struct PythonRemoteRelease {
    std::string version;
    std::string package_id;
    std::string package_name;
    std::string download_url;
    bool prerelease{false};
};

struct PythonEnvironmentResult {
    InstalledRuntime runtime;
    InstalledRuntime base_runtime;
    bool already_created{false};
    std::string raw_output;
};

bool InstallPythonRuntime(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    PythonInstallResult* result,
    std::string* error);

bool QueryPythonRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<PythonRemoteRelease>* result,
    std::string* error);

bool CreatePythonEnvironment(
    const InstalledRuntime& base_runtime,
    const std::string& env_name,
    PythonEnvironmentResult* result,
    std::string* error);

}  // namespace jkm
