#pragma once

#include <filesystem>
#include <string>

namespace jkm {

struct AppPaths {
    std::filesystem::path root;
    std::filesystem::path cache;
    std::filesystem::path downloads;
    std::filesystem::path temp;
    std::filesystem::path logs;
    std::filesystem::path state;
    std::filesystem::path installs;
    std::filesystem::path current;
    std::filesystem::path current_java;
    std::filesystem::path current_python;
    std::filesystem::path current_node;
    std::filesystem::path current_go;
    std::filesystem::path current_maven;
    std::filesystem::path current_gradle;
    std::filesystem::path settings_store;
    std::filesystem::path active_runtime_store;
    std::filesystem::path environment_snapshot_store;
    std::filesystem::path operations_audit_store;
};

AppPaths DetectAppPaths();
bool EnsureAppDirectories(const AppPaths& paths, std::string* error);

}  // namespace jkm
