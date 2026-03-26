#pragma once

#include <string>
#include <vector>

#include "core/paths.h"
#include "core/store.h"

namespace jkm {

struct JavaInstallResult {
    InstalledRuntime runtime;
    bool already_installed{false};
    std::string release_name;
    std::string package_name;
    std::string download_url;
    std::string checksum;
    std::string raw_output;
};

struct TemurinAvailableReleases {
    std::vector<int> available_releases;
    std::vector<int> lts_releases;
    int most_recent_feature_release{0};
    int most_recent_lts{0};
};

struct JavaRemoteRelease {
    std::string release_name;
    std::string openjdk_version;
    std::string semver;
    std::string package_name;
    std::string download_url;
    std::string checksum;
    std::string updated_at;
};

bool InstallTemurinJdk(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    JavaInstallResult* result,
    std::string* error);

bool QueryTemurinAvailableReleases(
    TemurinAvailableReleases* result,
    std::string* error);

bool QueryTemurinRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<JavaRemoteRelease>* result,
    std::string* error);

}  // namespace jkm
