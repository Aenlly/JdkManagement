#pragma once

#include <string>
#include <vector>

#include "core/store.h"

namespace jkm {

struct ToolInstallResult {
    InstalledRuntime runtime;
    bool already_installed{false};
    std::string resolved_version;
    std::string package_name;
    std::string download_url;
    std::string checksum;
    std::string channel;
    std::string published_at;
    std::string raw_output;
};

struct ToolRemoteRelease {
    std::string version;
    std::string package_name;
    std::string download_url;
    std::string checksum;
    std::string channel;
    std::string published_at;
};

bool InstallNodeRuntime(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    ToolInstallResult* result,
    std::string* error);

bool QueryNodeRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error);

bool InstallGoRuntime(
    const AppPaths& paths,
    const std::string& selector,
    const std::string& arch,
    ToolInstallResult* result,
    std::string* error);

bool QueryGoRemoteReleases(
    const std::string& selector,
    const std::string& arch,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error);

bool InstallMavenRuntime(
    const AppPaths& paths,
    const std::string& selector,
    ToolInstallResult* result,
    std::string* error);

bool QueryMavenRemoteReleases(
    const std::string& selector,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error);

bool InstallGradleRuntime(
    const AppPaths& paths,
    const std::string& selector,
    ToolInstallResult* result,
    std::string* error);

bool QueryGradleRemoteReleases(
    const std::string& selector,
    int limit,
    std::vector<ToolRemoteRelease>* result,
    std::string* error);

}  // namespace jkm
