#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/models.h"
#include "core/paths.h"

namespace jkm {

struct InstalledRuntime {
    RuntimeType type{RuntimeType::Java};
    std::string distribution;
    std::string name;
    std::filesystem::path root;
    std::string base_name;
    std::filesystem::path base_root;
    bool is_environment{false};
    bool is_external{false};
};

struct ActiveRuntime {
    RuntimeType type{RuntimeType::Java};
    std::string selected_name;
    std::string distribution;
    std::filesystem::path root;
    std::string scope;
    std::string updated_at_utc;
};

struct EnvironmentSnapshot {
    std::string created_at_utc;
    std::optional<std::string> path_value;
    std::optional<std::string> java_home;
    std::optional<std::string> python_home;
    std::optional<std::string> conda_prefix;
    std::optional<std::string> node_home;
    std::optional<std::string> go_root;
    std::optional<std::string> maven_home;
    std::optional<std::string> m2_home;
    std::optional<std::string> gradle_home;
    std::optional<std::string> jdkm_home;
    std::optional<std::string> jdkm_current_java;
    std::optional<std::string> jdkm_current_python;
    std::optional<std::string> jdkm_current_python_base;
    std::optional<std::string> jdkm_current_python_env;
    std::optional<std::string> jdkm_current_node;
    std::optional<std::string> jdkm_current_go;
    std::optional<std::string> jdkm_current_maven;
    std::optional<std::string> jdkm_current_gradle;
    std::optional<std::filesystem::path> external_java_root;
    std::optional<std::filesystem::path> external_python_root;
    std::optional<std::filesystem::path> external_node_root;
    std::optional<std::filesystem::path> external_go_root;
    std::optional<std::filesystem::path> external_maven_root;
    std::optional<std::filesystem::path> external_gradle_root;
};

class ActiveRuntimeStore {
public:
    explicit ActiveRuntimeStore(std::filesystem::path file_path);

    std::vector<ActiveRuntime> Load() const;
    std::optional<ActiveRuntime> Get(RuntimeType type) const;
    bool Upsert(const ActiveRuntime& runtime, std::string* error) const;
    bool Remove(RuntimeType type, std::string* error) const;
    bool Clear(std::string* error) const;
    const std::filesystem::path& FilePath() const;

private:
    std::filesystem::path file_path_;
};

class EnvironmentSnapshotStore {
public:
    explicit EnvironmentSnapshotStore(std::filesystem::path file_path);

    bool Exists() const;
    std::optional<EnvironmentSnapshot> Load(std::string* error = nullptr) const;
    bool Save(const EnvironmentSnapshot& snapshot, std::string* error) const;
    const std::filesystem::path& FilePath() const;

private:
    std::filesystem::path file_path_;
};

std::vector<InstalledRuntime> ScanInstalledRuntimes(
    const AppPaths& paths,
    std::optional<RuntimeType> filter = std::nullopt,
    const EnvironmentSnapshot* snapshot = nullptr);

bool CaptureEnvironmentSnapshot(
    const AppPaths& paths,
    EnvironmentSnapshot* result,
    std::string* error);

std::vector<InstalledRuntime> FindInstalledRuntimeMatches(
    const std::vector<InstalledRuntime>& installed,
    RuntimeType type,
    const std::string& selector);

std::vector<std::string> InstalledRuntimeSelectors(const InstalledRuntime& runtime);
std::string PreferredInstalledRuntimeSelector(const InstalledRuntime& runtime);

}  // namespace jkm
