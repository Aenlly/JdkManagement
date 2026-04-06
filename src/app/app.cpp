#include "app/app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "core/metadata.h"
#include "core/models.h"
#include "infrastructure/process.h"
#include "infrastructure/platform_windows.h"
#include "providers/python_runtime.h"
#include "providers/temurin.h"
#include "providers/tool_runtime.h"

#include <windows.h>

namespace fs = std::filesystem;

namespace jkm {

namespace {

constexpr const char* kProjectStateDirectoryName = ".jkm";
constexpr const char* kProjectActiveRuntimeStoreName = "local_runtimes.tsv";
constexpr const char* kProjectLockFileName = "project.lock.tsv";
constexpr const char* kShellHookStartMarker = "# >>> JKM Auto Hook >>>";
constexpr const char* kShellHookEndMarker = "# <<< JKM Auto Hook <<<";

struct ConfigKeyDefinition {
    const char* canonical_key;
    const char* env_name;
    bool is_path{false};
};

constexpr ConfigKeyDefinition kConfigKeyDefinitions[] = {
    {"network.proxy", nullptr, false},
    {"network.http_proxy", nullptr, false},
    {"network.https_proxy", nullptr, false},
    {"network.ca_cert", "JDKM_CA_CERT_PATH", true},
    {"mirror.temurin", "JDKM_SOURCE_TEMURIN_BASE_URL", false},
    {"mirror.python", "JDKM_SOURCE_PYTHON_BASE_URL", false},
    {"mirror.node", "JDKM_SOURCE_NODE_BASE_URL", false},
    {"mirror.go", "JDKM_SOURCE_GO_BASE_URL", false},
    {"mirror.maven.metadata", "JDKM_SOURCE_MAVEN_METADATA_BASE_URL", false},
    {"mirror.maven.archive", "JDKM_SOURCE_MAVEN_ARCHIVE_BASE_URL", false},
    {"mirror.gradle", "JDKM_SOURCE_GRADLE_BASE_URL", false},
};

struct CacheFileEntry {
    std::string area;
    fs::path path;
    std::uintmax_t size_bytes{0};
    fs::file_time_type last_write_time{};
};

std::optional<std::string> TryOptionValue(const std::vector<std::string>& args, const std::string& name);
std::optional<fs::path> DefaultShellProfilePath(const std::string& shell, std::string* error = nullptr);
std::string QuotePowerShellString(const std::string& value);
std::optional<std::string> ReadProcessEnvironmentUtf8(const wchar_t* name);

std::string ScopeFromArgs(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "--local") {
            return "local";
        }
    }
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--scope" && index + 1 < args.size()) {
            return ToLowerAscii(args[index + 1]);
        }
    }
    return "user";
}

std::string OptionValue(const std::vector<std::string>& args, const std::string& name, const std::string& fallback) {
    for (std::size_t index = 0; index + 1 < args.size(); ++index) {
        if (args[index] == name) {
            return args[index + 1];
        }
    }
    return fallback;
}

std::string DefaultDistribution(RuntimeType type) {
    switch (type) {
        case RuntimeType::Java:
            return "temurin";
        case RuntimeType::Python:
            return "cpython";
        case RuntimeType::Node:
            return "nodejs";
        case RuntimeType::Go:
            return "golang";
        case RuntimeType::Maven:
            return "apache";
        case RuntimeType::Gradle:
            return "gradle";
        default:
            return {};
    }
}

fs::path CurrentLinkPath(const AppPaths& paths, RuntimeType type) {
    switch (type) {
        case RuntimeType::Java:
            return paths.current_java;
        case RuntimeType::Python:
            return paths.current_python;
        case RuntimeType::Node:
            return paths.current_node;
        case RuntimeType::Go:
            return paths.current_go;
        case RuntimeType::Maven:
            return paths.current_maven;
        case RuntimeType::Gradle:
            return paths.current_gradle;
        default:
            return paths.current;
    }
}

std::vector<fs::path> ManagedPathEntries(const AppPaths& paths);

std::optional<fs::path> CurrentWorkingDirectory(std::string* error = nullptr) {
    std::error_code ec;
    const auto cwd = fs::current_path(ec);
    if (ec) {
        if (error != nullptr) {
            *error = ec.message();
        }
        return std::nullopt;
    }
    return cwd;
}

std::string TrimWhitespace(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

std::string NormalizeConfigKey(std::string value) {
    value = ToLowerAscii(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](char ch) {
        return ch == '.' || ch == '_' || ch == '-' || ch == ' ';
    }), value.end());
    return value;
}

const ConfigKeyDefinition* FindConfigKeyDefinition(const std::string& key) {
    const auto normalized = NormalizeConfigKey(key);
    for (const auto& definition : kConfigKeyDefinitions) {
        if (NormalizeConfigKey(definition.canonical_key) == normalized) {
            return &definition;
        }
    }
    return nullptr;
}

std::optional<fs::path> FindProjectStateDirectory(const fs::path& start_directory) {
    std::error_code ec;
    auto current = start_directory.lexically_normal();
    while (true) {
        const auto candidate = current / kProjectStateDirectoryName;
        if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
            return candidate;
        }

        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }

    return std::nullopt;
}

std::optional<fs::path> FindProjectLockFilePath(const fs::path& start_directory) {
    std::error_code ec;
    auto current = start_directory.lexically_normal();
    while (true) {
        const auto candidate = current / kProjectStateDirectoryName / kProjectLockFileName;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }

        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }

    return std::nullopt;
}

fs::path DefaultProjectStateDirectory(const fs::path& start_directory) {
    if (const auto existing = FindProjectStateDirectory(start_directory); existing.has_value()) {
        return *existing;
    }
    return start_directory / kProjectStateDirectoryName;
}

std::optional<fs::path> ResolveProjectLockFilePath(
    const std::vector<std::string>& args,
    bool require_existing,
    std::string* error = nullptr) {
    const auto explicit_path = TryOptionValue(args, "--lock-file");
    const auto cwd = CurrentWorkingDirectory(error);
    if (!cwd.has_value()) {
        return std::nullopt;
    }

    if (explicit_path.has_value()) {
        auto lock_path = PathFromUtf8(*explicit_path);
        if (lock_path.is_relative()) {
            lock_path = (*cwd / lock_path).lexically_normal();
        }
        if (require_existing) {
            std::error_code ec;
            if (!fs::exists(lock_path, ec)) {
                if (error != nullptr) {
                    *error = "project lock file was not found at " + PathToUtf8(lock_path);
                }
                return std::nullopt;
            }
        }
        return lock_path;
    }

    if (require_existing) {
        if (const auto detected = FindProjectLockFilePath(*cwd); detected.has_value()) {
            return *detected;
        }
        if (error != nullptr) {
            *error = "project lock file was not found; expected " + PathToUtf8(DefaultProjectStateDirectory(*cwd) / kProjectLockFileName);
        }
        return std::nullopt;
    }

    return DefaultProjectStateDirectory(*cwd) / kProjectLockFileName;
}

bool SetProcessEnvironmentVariableUtf8(
    const std::string& name,
    const std::optional<std::string>& value,
    std::string* error = nullptr) {
    const auto wide_name = WideFromUtf8(name);
    std::wstring wide_value;
    const wchar_t* value_pointer = nullptr;
    if (value.has_value()) {
        wide_value = WideFromUtf8(*value);
        value_pointer = wide_value.c_str();
    }

    if (!SetEnvironmentVariableW(wide_name.c_str(), value_pointer)) {
        if (error != nullptr) {
            *error = "failed to update process environment for " + name;
        }
        return false;
    }
    return true;
}

std::optional<std::string> SettingValue(
    const std::map<std::string, std::string>& settings,
    const std::string& key) {
    const auto iterator = settings.find(key);
    if (iterator == settings.end()) {
        return std::nullopt;
    }

    const auto value = TrimWhitespace(iterator->second);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> ReadProcessEnvironmentUtf8(const std::string& name) {
    const auto wide_name = WideFromUtf8(name);
    return ReadProcessEnvironmentUtf8(wide_name.c_str());
}

bool EnsureProcessEnvironmentValue(
    const std::string& name,
    const std::optional<std::string>& value,
    std::string* error = nullptr) {
    if (!value.has_value() || value->empty()) {
        return true;
    }
    if (ReadProcessEnvironmentUtf8(name).has_value()) {
        return true;
    }
    return SetProcessEnvironmentVariableUtf8(name, value, error);
}

std::string FormatByteSize(std::uintmax_t size_bytes) {
    std::ostringstream stream;
    stream << std::fixed;
    if (size_bytes >= (1024ull * 1024ull * 1024ull)) {
        stream << std::setprecision(2) << (static_cast<double>(size_bytes) / (1024.0 * 1024.0 * 1024.0)) << " GB";
    } else if (size_bytes >= (1024ull * 1024ull)) {
        stream << std::setprecision(1) << (static_cast<double>(size_bytes) / (1024.0 * 1024.0)) << " MB";
    } else if (size_bytes >= 1024ull) {
        stream << std::setprecision(1) << (static_cast<double>(size_bytes) / 1024.0) << " KB";
    } else {
        stream.unsetf(std::ios::floatfield);
        stream << size_bytes << " B";
    }
    return stream.str();
}

std::vector<std::pair<std::string, fs::path>> CacheRoots(const AppPaths& paths, const std::string& selector) {
    const auto normalized = ToLowerAscii(selector);
    if (normalized.empty() || normalized == "all") {
        return {
            {"downloads", paths.downloads},
            {"temp", paths.temp}
        };
    }
    if (normalized == "downloads") {
        return {{"downloads", paths.downloads}};
    }
    if (normalized == "temp") {
        return {{"temp", paths.temp}};
    }
    return {};
}

std::vector<CacheFileEntry> CollectCacheFileEntries(
    const AppPaths& paths,
    const std::string& selector) {
    std::vector<CacheFileEntry> entries;
    const auto roots = CacheRoots(paths, selector);
    for (const auto& [area, root] : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) {
            continue;
        }

        const auto options = fs::directory_options::skip_permission_denied;
        for (fs::recursive_directory_iterator iterator(root, options, ec), end; iterator != end; iterator.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }

            if (!iterator->is_regular_file(ec)) {
                continue;
            }

            const auto size = iterator->file_size(ec);
            if (ec) {
                ec.clear();
                continue;
            }

            const auto last_write = iterator->last_write_time(ec);
            if (ec) {
                ec.clear();
                continue;
            }

            entries.push_back(CacheFileEntry{
                area,
                iterator->path(),
                static_cast<std::uintmax_t>(size),
                last_write
            });
        }
    }

    std::sort(entries.begin(), entries.end(), [](const CacheFileEntry& left, const CacheFileEntry& right) {
        if (left.area != right.area) {
            return left.area < right.area;
        }
        return PathToUtf8(left.path) < PathToUtf8(right.path);
    });
    return entries;
}

std::uintmax_t TotalCacheSize(const std::vector<CacheFileEntry>& entries) {
    std::uintmax_t total = 0;
    for (const auto& entry : entries) {
        total += entry.size_bytes;
    }
    return total;
}

void RemoveEmptyDirectoriesUnder(const fs::path& root) {
    std::vector<fs::path> directories;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }

    const auto options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator iterator(root, options, ec), end; iterator != end; iterator.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (iterator->is_directory(ec)) {
            directories.push_back(iterator->path());
        }
    }

    std::sort(directories.begin(), directories.end(), [](const fs::path& left, const fs::path& right) {
        return left.native().size() > right.native().size();
    });

    for (const auto& directory : directories) {
        std::error_code remove_ec;
        if (fs::is_empty(directory, remove_ec)) {
            fs::remove(directory, remove_ec);
        }
    }
}

bool IsManagedRuntimeSelection(const ActiveRuntime& runtime) {
    return ToLowerAscii(runtime.selected_name) != "original" && ToLowerAscii(runtime.distribution) != "external";
}

std::optional<std::pair<std::string, std::string>> ParsePythonEnvironmentSelector(const std::string& selector) {
    const auto separator = selector.find('/');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= selector.size()) {
        return std::nullopt;
    }
    return std::make_pair(selector.substr(0, separator), selector.substr(separator + 1));
}

std::string StripArchSuffix(std::string selector, const std::string& arch) {
    if (selector.empty() || arch.empty()) {
        return selector;
    }

    const auto normalized_selector = ToLowerAscii(selector);
    const auto suffix = "-" + ToLowerAscii(arch);
    if (normalized_selector.size() > suffix.size() &&
        normalized_selector.compare(normalized_selector.size() - suffix.size(), suffix.size(), suffix) == 0) {
        selector.erase(selector.size() - suffix.size());
    }
    return selector;
}

std::string InstallSelectorForLockEntry(const ProjectLockEntry& entry) {
    if (entry.type == RuntimeType::Python) {
        if (const auto env_selector = ParsePythonEnvironmentSelector(entry.selector); env_selector.has_value()) {
            return StripArchSuffix(env_selector->first, entry.arch);
        }
        return StripArchSuffix(entry.selector, entry.arch);
    }

    if (entry.type == RuntimeType::Node || entry.type == RuntimeType::Go) {
        return StripArchSuffix(entry.selector, entry.arch);
    }

    return entry.selector;
}

bool ApplyPersistedNetworkSettings(const SettingsStore& settings_store, std::string* error = nullptr) {
    std::string load_error;
    const auto settings = settings_store.Load(&load_error);
    if (!load_error.empty()) {
        if (error != nullptr) {
            *error = load_error;
        }
        return false;
    }

    const auto shared_proxy = SettingValue(settings, "network.proxy");
    auto http_proxy = SettingValue(settings, "network.http_proxy");
    auto https_proxy = SettingValue(settings, "network.https_proxy");
    if (!http_proxy.has_value()) {
        http_proxy = shared_proxy;
    }
    if (!https_proxy.has_value()) {
        https_proxy = shared_proxy;
    }

    if (!EnsureProcessEnvironmentValue("JDKM_HTTP_PROXY", http_proxy, error) ||
        !EnsureProcessEnvironmentValue("JDKM_HTTPS_PROXY", https_proxy, error) ||
        !EnsureProcessEnvironmentValue("HTTP_PROXY", http_proxy, error) ||
        !EnsureProcessEnvironmentValue("HTTPS_PROXY", https_proxy, error)) {
        return false;
    }

    const auto ca_cert = SettingValue(settings, "network.ca_cert");
    if (!EnsureProcessEnvironmentValue("JDKM_CA_CERT_PATH", ca_cert, error) ||
        !EnsureProcessEnvironmentValue("SSL_CERT_FILE", ca_cert, error)) {
        return false;
    }

    for (const auto& definition : kConfigKeyDefinitions) {
        if (definition.env_name == nullptr || std::string(definition.canonical_key) == "network.ca_cert") {
            continue;
        }
        if (!EnsureProcessEnvironmentValue(definition.env_name, SettingValue(settings, definition.canonical_key), error)) {
            return false;
        }
    }

    return true;
}

std::optional<fs::path> FindProjectRuntimeStorePath(const fs::path& start_directory) {
    std::error_code ec;
    auto current = start_directory.lexically_normal();
    while (true) {
        const auto candidate = current / kProjectStateDirectoryName / kProjectActiveRuntimeStoreName;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }

        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }

    return std::nullopt;
}

std::optional<fs::path> DetectProjectRuntimeStorePath(std::string* error = nullptr) {
    const auto cwd = CurrentWorkingDirectory(error);
    if (!cwd.has_value()) {
        return std::nullopt;
    }
    return FindProjectRuntimeStorePath(*cwd);
}

std::optional<fs::path> DetectProjectStateDirectoryPath(std::string* error = nullptr) {
    const auto store_path = DetectProjectRuntimeStorePath(error);
    if (!store_path.has_value()) {
        return std::nullopt;
    }
    return store_path->parent_path();
}

std::optional<ActiveRuntime> LoadProjectActiveRuntime(RuntimeType type) {
    std::string error;
    const auto store_path = DetectProjectRuntimeStorePath(&error);
    if (!store_path.has_value()) {
        return std::nullopt;
    }

    ActiveRuntimeStore store(*store_path);
    return store.Get(type);
}

std::vector<ActiveRuntime> LoadProjectActiveRuntimes() {
    std::string error;
    const auto store_path = DetectProjectRuntimeStorePath(&error);
    if (!store_path.has_value()) {
        return {};
    }

    ActiveRuntimeStore store(*store_path);
    return store.Load();
}

bool SaveProjectActiveRuntime(const ActiveRuntime& runtime, fs::path* store_path, std::string* error) {
    const auto cwd = CurrentWorkingDirectory(error);
    if (!cwd.has_value()) {
        return false;
    }

    const auto state_dir = *cwd / kProjectStateDirectoryName;
    std::error_code ec;
    fs::create_directories(state_dir, ec);
    if (ec) {
        if (error != nullptr) {
            *error = ec.message();
        }
        return false;
    }

    const auto file_path = state_dir / kProjectActiveRuntimeStoreName;
    ActiveRuntimeStore store(file_path);
    if (!store.Upsert(runtime, error)) {
        return false;
    }

    if (store_path != nullptr) {
        *store_path = file_path;
    }
    return true;
}

bool DeleteProjectActiveRuntime(RuntimeType type, fs::path* store_path, std::string* error) {
    const auto file_path = DetectProjectRuntimeStorePath(error);
    if (!file_path.has_value()) {
        if (store_path != nullptr) {
            *store_path = fs::path{};
        }
        return true;
    }

    ActiveRuntimeStore store(*file_path);
    if (!store.Remove(type, error)) {
        return false;
    }

    std::error_code ec;
    const auto state_dir = file_path->parent_path();
    if (fs::exists(state_dir, ec) && fs::is_empty(state_dir, ec)) {
        fs::remove(state_dir, ec);
    }

    if (store_path != nullptr) {
        *store_path = *file_path;
    }
    return true;
}

std::optional<ActiveRuntime> EffectiveActiveRuntime(const ActiveRuntimeStore& global_store, RuntimeType type) {
    const auto project_runtime = LoadProjectActiveRuntime(type);
    if (project_runtime.has_value()) {
        return project_runtime;
    }
    return global_store.Get(type);
}

std::map<RuntimeType, ActiveRuntime> EffectiveActiveRuntimeMap(const ActiveRuntimeStore& global_store) {
    std::map<RuntimeType, ActiveRuntime> runtimes;
    for (const auto& runtime : global_store.Load()) {
        runtimes[runtime.type] = runtime;
    }
    for (const auto& runtime : LoadProjectActiveRuntimes()) {
        runtimes[runtime.type] = runtime;
    }
    return runtimes;
}

std::map<RuntimeType, ActiveRuntime> GlobalActiveRuntimeMap(const ActiveRuntimeStore& global_store) {
    std::map<RuntimeType, ActiveRuntime> runtimes;
    for (const auto& runtime : global_store.Load()) {
        runtimes[runtime.type] = runtime;
    }
    return runtimes;
}

std::map<RuntimeType, ActiveRuntime> RuntimeContextAfterUse(
    const ActiveRuntimeStore& global_store,
    const ActiveRuntime& runtime) {
    auto runtimes = runtime.scope == "local"
        ? EffectiveActiveRuntimeMap(global_store)
        : GlobalActiveRuntimeMap(global_store);
    runtimes[runtime.type] = runtime;
    return runtimes;
}

std::string ScopeDisplayName(const std::string& scope) {
    return scope == "local" ? "this project scope" : "the user scope";
}

std::vector<DoctorCheck> BuildRuntimeDependencyChecks(const std::map<RuntimeType, ActiveRuntime>& runtimes) {
    std::vector<DoctorCheck> checks;
    const auto java_it = runtimes.find(RuntimeType::Java);

    const auto append_tool_check = [&checks, &java_it, &runtimes](RuntimeType type, const std::string& label, const std::string& command_name) {
        const auto tool_it = runtimes.find(type);
        if (tool_it == runtimes.end()) {
            return;
        }

        if (java_it == runtimes.end()) {
            checks.push_back({
                "WARN",
                label + " Java dependency",
                label + " `" + tool_it->second.selected_name + "` is effective but no Java runtime is selected; `"
                    + command_name + "` usually requires `JAVA_HOME` or `java` on PATH"
            });
            return;
        }

        checks.push_back({
            "OK",
            label + " Java dependency",
            label + " `" + tool_it->second.selected_name + "` will use Java `" + java_it->second.selected_name + "`"
        });
    };

    append_tool_check(RuntimeType::Maven, "maven", "mvn");
    append_tool_check(RuntimeType::Gradle, "gradle", "gradle");
    return checks;
}

std::vector<std::string> BuildUseDependencyNotes(
    const std::map<RuntimeType, ActiveRuntime>& runtimes,
    const ActiveRuntime& changed_runtime) {
    std::vector<std::string> notes;
    const auto java_it = runtimes.find(RuntimeType::Java);
    const auto scope_name = ScopeDisplayName(changed_runtime.scope);

    const auto append_tool_note = [&notes, &java_it, &runtimes, &scope_name](RuntimeType type, const std::string& label) {
        const auto tool_it = runtimes.find(type);
        if (tool_it == runtimes.end()) {
            return;
        }

        if (java_it == runtimes.end()) {
            notes.push_back("[WARN] " + label + " requires Java. No effective Java runtime is selected for " + scope_name + " yet.");
            return;
        }

        notes.push_back("Dependency: " + label + " will use Java `" + java_it->second.selected_name + "` in " + scope_name + ".");
    };

    switch (changed_runtime.type) {
        case RuntimeType::Maven:
            append_tool_note(RuntimeType::Maven, "Maven");
            break;
        case RuntimeType::Gradle:
            append_tool_note(RuntimeType::Gradle, "Gradle");
            break;
        case RuntimeType::Java:
            if (const auto maven_it = runtimes.find(RuntimeType::Maven); maven_it != runtimes.end()) {
                notes.push_back("Dependency: Maven `" + maven_it->second.selected_name + "` now resolves against Java `"
                    + changed_runtime.selected_name + "` in " + scope_name + ".");
            }
            if (const auto gradle_it = runtimes.find(RuntimeType::Gradle); gradle_it != runtimes.end()) {
                notes.push_back("Dependency: Gradle `" + gradle_it->second.selected_name + "` now resolves against Java `"
                    + changed_runtime.selected_name + "` in " + scope_name + ".");
            }
            break;
        default:
            break;
    }

    return notes;
}

ActiveRuntime ActiveRuntimeFromInstalledRuntime(const InstalledRuntime& runtime, const std::string& scope) {
    return ActiveRuntime{
        runtime.type,
        PreferredInstalledRuntimeSelector(runtime),
        runtime.distribution,
        runtime.root,
        scope,
        NowUtcIso8601()
    };
}

std::map<RuntimeType, ActiveRuntime> RuntimeContextForInstalledRuntimeProbe(
    const ActiveRuntimeStore& global_store,
    const InstalledRuntime& runtime) {
    auto runtimes = EffectiveActiveRuntimeMap(global_store);
    runtimes[runtime.type] = ActiveRuntimeFromInstalledRuntime(runtime, "probe");
    return runtimes;
}

std::vector<std::string> VersionArguments(RuntimeType type) {
    switch (type) {
        case RuntimeType::Java:
        case RuntimeType::Maven:
            return {"-version"};
        case RuntimeType::Go:
            return {"version"};
        case RuntimeType::Python:
        case RuntimeType::Node:
        case RuntimeType::Gradle:
            return {"--version"};
        default:
            return {};
    }
}

std::string BuildPowerShellProcessInvocation(
    const fs::path& executable,
    const std::vector<std::string>& arguments) {
    std::ostringstream script;
    script << "$command = " << QuotePowerShellString(PathToUtf8(executable)) << "\n";
    script << "$arguments = @(";
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index > 0) {
            script << ", ";
        }
        script << QuotePowerShellString(arguments[index]);
    }
    script << ")\n";
    script << "& $command @arguments\n";
    script << "if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE } else { exit 0 }\n";
    return script.str();
}

std::vector<fs::path> RuntimePathEntries(const ActiveRuntime& runtime) {
    switch (runtime.type) {
        case RuntimeType::Java:
            return {runtime.root / "bin"};
        case RuntimeType::Python:
            return {runtime.root, runtime.root / "Scripts"};
        case RuntimeType::Node:
            return {runtime.root};
        case RuntimeType::Go:
            return {runtime.root / "bin"};
        case RuntimeType::Maven:
            return {runtime.root / "bin"};
        case RuntimeType::Gradle:
            return {runtime.root / "bin"};
        default:
            return {};
    }
}

std::vector<RuntimeType> AllRuntimeTypes() {
    return {
        RuntimeType::Java,
        RuntimeType::Python,
        RuntimeType::Node,
        RuntimeType::Go,
        RuntimeType::Maven,
        RuntimeType::Gradle
    };
}

bool ClearUserRuntimeSelection(
    const AppPaths& paths,
    const ActiveRuntimeStore& store,
    const EnvironmentSnapshotStore& snapshot_store,
    RuntimeType type,
    std::string* error) {
    const auto snapshot = snapshot_store.Load(error);
    const auto restore_or_delete = [error](const std::string& name, const std::optional<std::string>& value) {
        return value.has_value() ? SetUserEnvironmentVariable(name, *value, error)
                                 : DeleteUserEnvironmentVariable(name, error);
    };

    switch (type) {
        case RuntimeType::Java:
            if (!restore_or_delete("JAVA_HOME", snapshot.has_value() ? snapshot->java_home : std::nullopt) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_JAVA", error)) {
                return false;
            }
            break;
        case RuntimeType::Python:
            if (!restore_or_delete("PYTHON_HOME", snapshot.has_value() ? snapshot->python_home : std::nullopt) ||
                !restore_or_delete("CONDA_PREFIX", snapshot.has_value() ? snapshot->conda_prefix : std::nullopt) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_PYTHON", error) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_PYTHON_BASE", error) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_PYTHON_ENV", error)) {
                return false;
            }
            break;
        case RuntimeType::Node:
            if (!restore_or_delete("NODE_HOME", snapshot.has_value() ? snapshot->node_home : std::nullopt) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_NODE", error)) {
                return false;
            }
            break;
        case RuntimeType::Go:
            if (!restore_or_delete("GOROOT", snapshot.has_value() ? snapshot->go_root : std::nullopt) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_GO", error)) {
                return false;
            }
            break;
        case RuntimeType::Maven:
            if (!restore_or_delete("MAVEN_HOME", snapshot.has_value() ? snapshot->maven_home : std::nullopt) ||
                !restore_or_delete("M2_HOME", snapshot.has_value() ? snapshot->m2_home : std::nullopt) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_MAVEN", error)) {
                return false;
            }
            break;
        case RuntimeType::Gradle:
            if (!restore_or_delete("GRADLE_HOME", snapshot.has_value() ? snapshot->gradle_home : std::nullopt) ||
                !DeleteUserEnvironmentVariable("JDKM_CURRENT_GRADLE", error)) {
                return false;
            }
            break;
    }

    if (!RemoveDirectoryJunction(CurrentLinkPath(paths, type), error)) {
        return false;
    }
    return store.Remove(type, error);
}

std::string QuotePowerShellString(const std::string& value) {
    std::string escaped = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            escaped += "''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped += "'";
    return escaped;
}

std::string QuoteCmdArgument(const std::string& value) {
    std::string quoted = "\"";
    for (const auto ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += "\"";
    return quoted;
}

std::vector<fs::path> EffectiveManagedPathEntries(const std::map<RuntimeType, ActiveRuntime>& runtimes) {
    std::vector<fs::path> entries;
    for (const auto& [type, runtime] : runtimes) {
        (void)type;
        const auto runtime_entries = RuntimePathEntries(runtime);
        entries.insert(entries.end(), runtime_entries.begin(), runtime_entries.end());
    }
    return entries;
}

std::string BuildPowerShellActivationScript(
    const AppPaths& paths,
    const std::map<RuntimeType, ActiveRuntime>& runtimes) {
    std::ostringstream script;
    script << "$__jkmManaged = @(";
    const auto managed_entries = ManagedPathEntries(paths);
    for (std::size_t index = 0; index < managed_entries.size(); ++index) {
        if (index > 0) {
            script << ", ";
        }
        script << QuotePowerShellString(PathToUtf8(managed_entries[index].lexically_normal()));
    }
    script << ")\n";
    script << "$__jkmCurrentPath = @(($env:PATH -split ';') | Where-Object { $_ -and ($__jkmManaged -notcontains $_) })\n";
    script << "$__jkmEffective = @(";
    const auto effective_entries = EffectiveManagedPathEntries(runtimes);
    for (std::size_t index = 0; index < effective_entries.size(); ++index) {
        if (index > 0) {
            script << ", ";
        }
        script << QuotePowerShellString(PathToUtf8(effective_entries[index].lexically_normal()));
    }
    script << ")\n";
    script << "$env:PATH = (($__jkmEffective + $__jkmCurrentPath) | Where-Object { $_ } | Select-Object -Unique) -join ';'\n";
    script << "$env:JDKM_HOME = " << QuotePowerShellString(PathToUtf8(paths.root)) << "\n";

    const auto set_or_clear = [&script](const std::string& name, const std::optional<std::string>& value) {
        script << "if (" << (value.has_value() ? "$true" : "$false") << ") { $env:" << name << " = "
               << (value.has_value() ? QuotePowerShellString(*value) : "''")
               << " } else { Remove-Item Env:" << name << " -ErrorAction SilentlyContinue }\n";
    };

    const auto java_it = runtimes.find(RuntimeType::Java);
    set_or_clear("JAVA_HOME", java_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(java_it->second.root)));
    set_or_clear("JDKM_CURRENT_JAVA", java_it == runtimes.end() ? std::nullopt : std::optional<std::string>(java_it->second.selected_name));

    const auto python_it = runtimes.find(RuntimeType::Python);
    set_or_clear("PYTHON_HOME", python_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(python_it->second.root)));
    set_or_clear("CONDA_PREFIX", python_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(python_it->second.root)));
    set_or_clear("JDKM_CURRENT_PYTHON", python_it == runtimes.end() ? std::nullopt : std::optional<std::string>(python_it->second.selected_name));
    if (python_it == runtimes.end()) {
        set_or_clear("JDKM_CURRENT_PYTHON_BASE", std::nullopt);
        set_or_clear("JDKM_CURRENT_PYTHON_ENV", std::nullopt);
    } else {
        const auto selector = python_it->second.selected_name;
        const auto separator = selector.find('/');
        if (separator == std::string::npos) {
            set_or_clear("JDKM_CURRENT_PYTHON_BASE", selector == "original" ? std::optional<std::string>("original") : std::optional<std::string>(selector));
            set_or_clear("JDKM_CURRENT_PYTHON_ENV", selector == "original" ? std::optional<std::string>("original") : std::optional<std::string>("base"));
        } else {
            set_or_clear("JDKM_CURRENT_PYTHON_BASE", selector.substr(0, separator));
            set_or_clear("JDKM_CURRENT_PYTHON_ENV", selector.substr(separator + 1));
        }
    }

    const auto node_it = runtimes.find(RuntimeType::Node);
    set_or_clear("NODE_HOME", node_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(node_it->second.root)));
    set_or_clear("JDKM_CURRENT_NODE", node_it == runtimes.end() ? std::nullopt : std::optional<std::string>(node_it->second.selected_name));

    const auto go_it = runtimes.find(RuntimeType::Go);
    set_or_clear("GOROOT", go_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(go_it->second.root)));
    set_or_clear("JDKM_CURRENT_GO", go_it == runtimes.end() ? std::nullopt : std::optional<std::string>(go_it->second.selected_name));

    const auto maven_it = runtimes.find(RuntimeType::Maven);
    set_or_clear("MAVEN_HOME", maven_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(maven_it->second.root)));
    set_or_clear("M2_HOME", maven_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(maven_it->second.root)));
    set_or_clear("JDKM_CURRENT_MAVEN", maven_it == runtimes.end() ? std::nullopt : std::optional<std::string>(maven_it->second.selected_name));

    const auto gradle_it = runtimes.find(RuntimeType::Gradle);
    set_or_clear("GRADLE_HOME", gradle_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(gradle_it->second.root)));
    set_or_clear("JDKM_CURRENT_GRADLE", gradle_it == runtimes.end() ? std::nullopt : std::optional<std::string>(gradle_it->second.selected_name));
    return script.str();
}

std::string BuildCmdActivationScript(
    const AppPaths& paths,
    const std::map<RuntimeType, ActiveRuntime>& runtimes) {
    std::ostringstream script;
    script << "@echo off\r\n";
    script << "set \"JDKM_HOME=" << PathToUtf8(paths.root) << "\"\r\n";
    script << "set \"PATH=";
    const auto effective_entries = EffectiveManagedPathEntries(runtimes);
    for (std::size_t index = 0; index < effective_entries.size(); ++index) {
        if (index > 0) {
            script << ';';
        }
        script << PathToUtf8(effective_entries[index].lexically_normal());
    }
    script << ";%PATH%\"\r\n";

    const auto set_or_clear = [&script](const std::string& name, const std::optional<std::string>& value) {
        if (value.has_value()) {
            script << "set \"" << name << '=' << *value << "\"\r\n";
        } else {
            script << "set \"" << name << "=\"\r\n";
        }
    };

    const auto java_it = runtimes.find(RuntimeType::Java);
    set_or_clear("JAVA_HOME", java_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(java_it->second.root)));
    set_or_clear("JDKM_CURRENT_JAVA", java_it == runtimes.end() ? std::nullopt : std::optional<std::string>(java_it->second.selected_name));

    const auto python_it = runtimes.find(RuntimeType::Python);
    set_or_clear("PYTHON_HOME", python_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(python_it->second.root)));
    set_or_clear("CONDA_PREFIX", python_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(python_it->second.root)));
    set_or_clear("JDKM_CURRENT_PYTHON", python_it == runtimes.end() ? std::nullopt : std::optional<std::string>(python_it->second.selected_name));
    if (python_it == runtimes.end()) {
        set_or_clear("JDKM_CURRENT_PYTHON_BASE", std::nullopt);
        set_or_clear("JDKM_CURRENT_PYTHON_ENV", std::nullopt);
    } else {
        const auto selector = python_it->second.selected_name;
        const auto separator = selector.find('/');
        if (separator == std::string::npos) {
            set_or_clear("JDKM_CURRENT_PYTHON_BASE", selector == "original" ? std::optional<std::string>("original") : std::optional<std::string>(selector));
            set_or_clear("JDKM_CURRENT_PYTHON_ENV", selector == "original" ? std::optional<std::string>("original") : std::optional<std::string>("base"));
        } else {
            set_or_clear("JDKM_CURRENT_PYTHON_BASE", selector.substr(0, separator));
            set_or_clear("JDKM_CURRENT_PYTHON_ENV", selector.substr(separator + 1));
        }
    }

    const auto node_it = runtimes.find(RuntimeType::Node);
    set_or_clear("NODE_HOME", node_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(node_it->second.root)));
    set_or_clear("JDKM_CURRENT_NODE", node_it == runtimes.end() ? std::nullopt : std::optional<std::string>(node_it->second.selected_name));

    const auto go_it = runtimes.find(RuntimeType::Go);
    set_or_clear("GOROOT", go_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(go_it->second.root)));
    set_or_clear("JDKM_CURRENT_GO", go_it == runtimes.end() ? std::nullopt : std::optional<std::string>(go_it->second.selected_name));

    const auto maven_it = runtimes.find(RuntimeType::Maven);
    set_or_clear("MAVEN_HOME", maven_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(maven_it->second.root)));
    set_or_clear("M2_HOME", maven_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(maven_it->second.root)));
    set_or_clear("JDKM_CURRENT_MAVEN", maven_it == runtimes.end() ? std::nullopt : std::optional<std::string>(maven_it->second.selected_name));

    const auto gradle_it = runtimes.find(RuntimeType::Gradle);
    set_or_clear("GRADLE_HOME", gradle_it == runtimes.end() ? std::nullopt : std::optional<std::string>(PathToUtf8(gradle_it->second.root)));
    set_or_clear("JDKM_CURRENT_GRADLE", gradle_it == runtimes.end() ? std::nullopt : std::optional<std::string>(gradle_it->second.selected_name));
    return script.str();
}

std::string BuildPowerShellHookScript(const fs::path& executable_path) {
    std::ostringstream script;
    const auto exe = PathToUtf8(executable_path);
    script
        << "function Invoke-JkmAutoEnv {\n"
        << "    $output = & " << QuotePowerShellString(exe) << " env activate --shell powershell 2>$null\n"
        << "    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace(($output | Out-String))) {\n"
        << "        Invoke-Expression ($output | Out-String)\n"
        << "    }\n"
        << "}\n"
        << "if (-not (Test-Path Function:\\global:__jkm_original_prompt)) {\n"
        << "    Copy-Item Function:\\prompt Function:\\global:__jkm_original_prompt\n"
        << "}\n"
        << "function global:prompt {\n"
        << "    Invoke-JkmAutoEnv\n"
        << "    & __jkm_original_prompt\n"
        << "}\n"
        << "Invoke-JkmAutoEnv\n";
    return script.str();
}

std::optional<fs::path> ResolveShellProfilePath(const std::vector<std::string>& args, std::size_t shell_arg_index, std::string* error) {
    if (const auto explicit_profile = TryOptionValue(args, "--profile"); explicit_profile.has_value()) {
        return PathFromUtf8(*explicit_profile);
    }

    if (args.size() <= shell_arg_index) {
        if (error != nullptr) {
            *error = "shell type is required";
        }
        return std::nullopt;
    }

    return DefaultShellProfilePath(ToLowerAscii(args[shell_arg_index]), error);
}

std::optional<fs::path> CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return std::nullopt;
    }
    buffer.resize(length);
    return fs::path(buffer);
}

std::optional<std::string> ReadProcessEnvironmentUtf8(const wchar_t* name) {
    wchar_t* buffer = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&buffer, &length, name) != 0 || buffer == nullptr || length == 0 || buffer[0] == L'\0') {
        if (buffer != nullptr) {
            free(buffer);
        }
        return std::nullopt;
    }

    const std::wstring value(buffer);
    free(buffer);
    return Utf8FromWide(value);
}

bool ReadTextFile(const fs::path& path, std::string* content, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        if (error != nullptr) {
            *error = "unable to open file: " + PathToUtf8(path);
        }
        return false;
    }

    std::ostringstream stream;
    stream << input.rdbuf();
    if (content != nullptr) {
        *content = stream.str();
    }
    return true;
}

bool WriteTextFile(const fs::path& path, const std::string& content, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = ec.message();
        }
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "unable to open file for writing: " + PathToUtf8(path);
        }
        return false;
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

std::optional<fs::path> DefaultShellProfilePath(const std::string& shell, std::string* error) {
    const auto user_profile = ReadProcessEnvironmentUtf8(L"USERPROFILE");
    if (!user_profile.has_value()) {
        if (error != nullptr) {
            *error = "USERPROFILE is not available";
        }
        return std::nullopt;
    }

    const auto documents = PathFromUtf8(*user_profile) / "Documents";
    if (shell == "powershell") {
        return documents / "WindowsPowerShell" / "Microsoft.PowerShell_profile.ps1";
    }
    if (shell == "pwsh") {
        return documents / "PowerShell" / "Microsoft.PowerShell_profile.ps1";
    }

    if (error != nullptr) {
        *error = "unsupported shell: " + shell;
    }
    return std::nullopt;
}

bool IsSupportedHookShell(const std::string& shell) {
    return shell == "powershell" || shell == "pwsh";
}

std::string NormalizeNewlines(std::string content) {
    std::string normalized;
    normalized.reserve(content.size());
    for (std::size_t index = 0; index < content.size(); ++index) {
        if (content[index] == '\r') {
            continue;
        }
        normalized.push_back(content[index]);
    }
    return normalized;
}

bool HookBlockInstalled(const std::string& content) {
    const auto normalized = NormalizeNewlines(content);
    const auto start = normalized.find(kShellHookStartMarker);
    const auto end = normalized.find(kShellHookEndMarker);
    return start != std::string::npos && end != std::string::npos && end >= start;
}

std::string RemoveHookBlock(std::string content) {
    auto normalized = NormalizeNewlines(std::move(content));
    const auto start = normalized.find(kShellHookStartMarker);
    if (start == std::string::npos) {
        return normalized;
    }

    const auto end_marker = normalized.find(kShellHookEndMarker, start);
    if (end_marker == std::string::npos) {
        return normalized;
    }

    auto erase_end = end_marker + std::strlen(kShellHookEndMarker);
    if (erase_end < normalized.size() && normalized[erase_end] == '\n') {
        ++erase_end;
    }
    while (erase_end < normalized.size() && normalized[erase_end] == '\n') {
        ++erase_end;
    }
    normalized.erase(start, erase_end - start);

    while (normalized.find("\n\n\n") != std::string::npos) {
        normalized.replace(normalized.find("\n\n\n"), 3, "\n\n");
    }
    return normalized;
}

std::string WrapHookBlock(const std::string& content) {
    std::ostringstream stream;
    stream << kShellHookStartMarker << '\n'
           << content
           << kShellHookEndMarker << '\n';
    return stream.str();
}

std::string UpsertHookBlock(const std::string& existing_content, const std::string& hook_script) {
    auto stripped = RemoveHookBlock(existing_content);
    while (!stripped.empty() && (stripped.back() == '\n' || stripped.back() == '\r')) {
        stripped.pop_back();
    }

    if (!stripped.empty()) {
        stripped += "\n\n";
    }
    stripped += WrapHookBlock(hook_script);
    return stripped;
}

std::vector<fs::path> ManagedPathEntries(const AppPaths& paths) {
    return {
        paths.current_java / "bin",
        paths.current_python,
        paths.current_python / "Scripts",
        paths.current_node,
        paths.current_go / "bin",
        paths.current_maven / "bin",
        paths.current_gradle / "bin"
    };
}

std::optional<std::string> TryOptionValue(const std::vector<std::string>& args, const std::string& name) {
    for (std::size_t index = 0; index + 1 < args.size(); ++index) {
        if (args[index] == name) {
            return args[index + 1];
        }
    }
    return std::nullopt;
}

bool HasFlag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

std::wstring QuoteCommandArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (const auto ch : value) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::string JoinArgs(const std::vector<std::string>& args) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (index > 0) {
            stream << ' ';
        }
        stream << args[index];
    }
    return stream.str();
}

std::string InferRuntimeTypeForAudit(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {};
    }

    if (args[0] == "remote") {
        if (args.size() >= 3) {
            const auto type = ParseRuntimeType(args[2]);
            return type.has_value() ? ToString(*type) : std::string{};
        }
        return {};
    }

    if (args[0] == "search" || args[0] == "version" || args[0] == "status") {
        if (args.size() >= 2) {
            const auto type = ParseRuntimeType(args[1]);
            return type.has_value() ? ToString(*type) : std::string{};
        }
        return {};
    }

    if (args[0] == "logs") {
        return {};
    }

    if (args.size() >= 2) {
        const auto type = ParseRuntimeType(args[1]);
        return type.has_value() ? ToString(*type) : std::string{};
    }
    return {};
}

std::string InferSelectorForAudit(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {};
    }

    if (args[0] == "remote") {
        if (args.size() >= 4 && !args[3].empty() && args[3][0] != '-') {
            return args[3];
        }
        return {};
    }

    if (args[0] == "search" || args[0] == "version" || args[0] == "status") {
        if (args.size() >= 3 && !args[2].empty() && args[2][0] != '-') {
            return args[2];
        }
        return {};
    }

    if (args[0] == "install" || args[0] == "remove" || args[0] == "use" || args[0] == "unuse") {
        return args.size() >= 3 ? args[2] : std::string{};
    }

    return {};
}

bool HasPathEntry(const std::string& path_value, const fs::path& expected) {
    const auto normalized_expected = ToLowerAscii(PathToUtf8(expected.lexically_normal()));

    std::size_t start = 0;
    while (start <= path_value.size()) {
        const auto end = path_value.find(';', start);
        const auto entry = path_value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (ToLowerAscii(entry) == normalized_expected) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    return false;
}

bool RestoreUserEnvironmentVariable(const std::string& name, const std::optional<std::string>& value, std::string* error) {
    return value.has_value()
        ? SetUserEnvironmentVariable(name, *value, error)
        : DeleteUserEnvironmentVariable(name, error);
}

std::string RemoveManagedPathEntries(const std::string& path_value, const AppPaths& paths) {
    std::vector<std::string> managed_entries;
    for (const auto& entry : ManagedPathEntries(paths)) {
        managed_entries.push_back(ToLowerAscii(PathToUtf8(entry.lexically_normal())));
    }

    std::vector<std::string> retained;
    std::size_t start = 0;
    while (start <= path_value.size()) {
        const auto end = path_value.find(';', start);
        auto entry = path_value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto normalized = ToLowerAscii(entry);
        const auto managed = std::find(managed_entries.begin(), managed_entries.end(), normalized);
        if (!entry.empty() && managed == managed_entries.end()) {
            retained.push_back(entry);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::ostringstream output;
    for (std::size_t index = 0; index < retained.size(); ++index) {
        if (index > 0) {
            output << ';';
        }
        output << retained[index];
    }
    return output.str();
}

bool PathStartsWith(const fs::path& candidate, const fs::path& prefix) {
    const auto normalized_candidate = candidate.lexically_normal();
    const auto normalized_prefix = prefix.lexically_normal();

    auto candidate_it = normalized_candidate.begin();
    auto prefix_it = normalized_prefix.begin();
    for (; prefix_it != normalized_prefix.end(); ++prefix_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end()) {
            return false;
        }
        if (ToLowerAscii(candidate_it->string()) != ToLowerAscii(prefix_it->string())) {
            return false;
        }
    }

    return true;
}

bool PathsOverlapForRemoval(const fs::path& selected_root, const fs::path& active_root) {
    return PathStartsWith(selected_root, active_root) || PathStartsWith(active_root, selected_root);
}

bool IsValidPythonEnvironmentName(const std::string& name) {
    if (name.empty() || ToLowerAscii(name) == "base") {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
}

fs::path ResolvePythonBaseRoot(const fs::path& runtime_root) {
    const auto normalized = runtime_root.lexically_normal();
    if (ToLowerAscii(normalized.parent_path().filename().string()) == "envs") {
        return normalized.parent_path().parent_path();
    }
    return normalized;
}

std::vector<InstalledRuntime> FilterPythonBaseRuntimes(const std::vector<InstalledRuntime>& runtimes) {
    std::vector<InstalledRuntime> filtered;
    for (const auto& runtime : runtimes) {
        if (runtime.type == RuntimeType::Python && !runtime.is_environment) {
            filtered.push_back(runtime);
        }
    }
    return filtered;
}

std::vector<InstalledRuntime> FilterPythonEnvironmentRuntimes(const std::vector<InstalledRuntime>& runtimes) {
    std::vector<InstalledRuntime> filtered;
    for (const auto& runtime : runtimes) {
        if (runtime.type == RuntimeType::Python && runtime.is_environment) {
            filtered.push_back(runtime);
        }
    }
    return filtered;
}

std::optional<fs::path> ResolveRuntimeExecutable(const InstalledRuntime& runtime) {
    fs::path candidates[2];
    std::size_t candidate_count = 0;

    switch (runtime.type) {
        case RuntimeType::Java:
            candidates[candidate_count++] = runtime.root / "bin" / "java.exe";
            break;
        case RuntimeType::Python:
            candidates[candidate_count++] = runtime.root / "python.exe";
            candidates[candidate_count++] = runtime.root / "Scripts" / "python.exe";
            break;
        case RuntimeType::Node:
            candidates[candidate_count++] = runtime.root / "node.exe";
            break;
        case RuntimeType::Go:
            candidates[candidate_count++] = runtime.root / "bin" / "go.exe";
            break;
        case RuntimeType::Maven:
            candidates[candidate_count++] = runtime.root / "bin" / "mvn.cmd";
            candidates[candidate_count++] = runtime.root / "bin" / "mvn.bat";
            break;
        case RuntimeType::Gradle:
            candidates[candidate_count++] = runtime.root / "bin" / "gradle.bat";
            candidates[candidate_count++] = runtime.root / "bin" / "gradle.cmd";
            break;
    }

    for (std::size_t index = 0; index < candidate_count; ++index) {
        const auto& candidate = candidates[index];
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::string TrimOutput(std::string output) {
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n' || output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    return output;
}

std::string PadTableCell(const std::string& value, std::size_t width) {
    if (value.size() >= width) {
        return value;
    }
    return value + std::string(width - value.size(), ' ');
}

void PrintTable(
    const std::vector<std::string>& headers,
    const std::vector<std::vector<std::string>>& rows,
    bool show_headers = true) {
    if (headers.empty()) {
        return;
    }

    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t index = 0; index < headers.size(); ++index) {
        widths[index] = headers[index].size();
    }

    for (const auto& row : rows) {
        for (std::size_t index = 0; index < headers.size() && index < row.size(); ++index) {
            widths[index] = std::max(widths[index], row[index].size());
        }
    }

    if (show_headers) {
        for (std::size_t index = 0; index < headers.size(); ++index) {
            if (index > 0) {
                std::cout << " | ";
            }
            std::cout << PadTableCell(headers[index], widths[index]);
        }
        std::cout << '\n';

        for (std::size_t index = 0; index < headers.size(); ++index) {
            if (index > 0) {
                std::cout << "-+-";
            }
            std::cout << std::string(widths[index], '-');
        }
        std::cout << '\n';
    }

    for (const auto& row : rows) {
        for (std::size_t index = 0; index < headers.size(); ++index) {
            if (index > 0) {
                std::cout << " | ";
            }
            const auto cell = index < row.size() ? row[index] : std::string{};
            std::cout << PadTableCell(cell, widths[index]);
        }
        std::cout << '\n';
    }
}

std::string EscapeJsonString(const std::string& value) {
    std::ostringstream stream;
    for (const auto ch : value) {
        switch (ch) {
            case '\\':
                stream << "\\\\";
                break;
            case '"':
                stream << "\\\"";
                break;
            case '\b':
                stream << "\\b";
                break;
            case '\f':
                stream << "\\f";
                break;
            case '\n':
                stream << "\\n";
                break;
            case '\r':
                stream << "\\r";
                break;
            case '\t':
                stream << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    stream << "\\u"
                           << std::hex
                           << std::uppercase
                           << std::setw(4)
                           << std::setfill('0')
                           << static_cast<int>(static_cast<unsigned char>(ch))
                           << std::dec
                           << std::nouppercase
                           << std::setfill(' ');
                } else {
                    stream << ch;
                }
                break;
        }
    }
    return stream.str();
}

std::string JsonString(const std::string& value) {
    return "\"" + EscapeJsonString(value) + "\"";
}

std::string JsonBoolean(bool value) {
    return value ? "true" : "false";
}

std::string JsonNumber(int value) {
    return std::to_string(value);
}

std::string JsonIntArray(const std::vector<int>& values) {
    std::ostringstream stream;
    stream << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << values[index];
    }
    stream << "]";
    return stream.str();
}

using JsonFields = std::vector<std::pair<std::string, std::string>>;

struct SortOption {
    std::string field;
    bool descending{false};
};

struct FilterCondition {
    std::string field;
    std::string value;
};

template <typename T>
struct OutputColumn {
    std::string key;
    std::string json_key;
    std::string header;
    std::function<std::string(const T&)> table_value;
    std::function<std::string(const T&)> json_value;
};

std::string JsonObject(const JsonFields& fields) {
    std::ostringstream stream;
    stream << "{";
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << JsonString(fields[index].first) << ": " << fields[index].second;
    }
    stream << "}";
    return stream.str();
}

void PrintJsonArray(const std::vector<JsonFields>& items) {
    std::cout << "[\n";
    for (std::size_t index = 0; index < items.size(); ++index) {
        std::cout << "  " << JsonObject(items[index]);
        if (index + 1 < items.size()) {
            std::cout << ",";
        }
        std::cout << '\n';
    }
    std::cout << "]\n";
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    if (begin == value.end()) {
        return {};
    }

    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    return std::string(begin, end);
}

// Remote query fields accept multiple spellings, so normalize everything to a compact lookup key.
std::string NormalizeColumnKey(const std::string& value) {
    const auto trimmed = TrimAscii(value);
    std::string normalized;
    normalized.reserve(trimmed.size());
    for (const auto ch : trimmed) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte)) {
            normalized.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    return normalized;
}

std::vector<std::string> SplitCommaSeparated(const std::string& raw) {
    std::vector<std::string> values;
    std::stringstream stream(raw);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = NormalizeColumnKey(item);
        if (!item.empty()) {
            values.push_back(item);
        }
    }
    return values;
}

std::string JoinReleaseNumbers(const std::vector<int>& values) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << ',';
        }
        stream << values[index];
    }
    return stream.str();
}

// This keeps version-like strings intuitive without pulling in a dedicated semver parser.
int CompareNaturalStrings(const std::string& left, const std::string& right) {
    std::size_t left_index = 0;
    std::size_t right_index = 0;

    while (left_index < left.size() && right_index < right.size()) {
        const unsigned char left_char = static_cast<unsigned char>(left[left_index]);
        const unsigned char right_char = static_cast<unsigned char>(right[right_index]);

        if (std::isdigit(left_char) && std::isdigit(right_char)) {
            std::size_t left_end = left_index;
            while (left_end < left.size() && std::isdigit(static_cast<unsigned char>(left[left_end]))) {
                ++left_end;
            }

            std::size_t right_end = right_index;
            while (right_end < right.size() && std::isdigit(static_cast<unsigned char>(right[right_end]))) {
                ++right_end;
            }

            std::size_t left_trimmed = left_index;
            while (left_trimmed < left_end && left[left_trimmed] == '0') {
                ++left_trimmed;
            }

            std::size_t right_trimmed = right_index;
            while (right_trimmed < right_end && right[right_trimmed] == '0') {
                ++right_trimmed;
            }

            const auto left_digits = left.substr(left_trimmed, left_end - left_trimmed);
            const auto right_digits = right.substr(right_trimmed, right_end - right_trimmed);
            const auto normalized_left = left_digits.empty() ? std::string("0") : left_digits;
            const auto normalized_right = right_digits.empty() ? std::string("0") : right_digits;

            if (normalized_left.size() != normalized_right.size()) {
                return normalized_left.size() < normalized_right.size() ? -1 : 1;
            }

            if (normalized_left != normalized_right) {
                return normalized_left < normalized_right ? -1 : 1;
            }

            if ((left_end - left_index) != (right_end - right_index)) {
                return (left_end - left_index) < (right_end - right_index) ? -1 : 1;
            }

            left_index = left_end;
            right_index = right_end;
            continue;
        }

        const auto normalized_left = static_cast<unsigned char>(std::tolower(left_char));
        const auto normalized_right = static_cast<unsigned char>(std::tolower(right_char));
        if (normalized_left != normalized_right) {
            return normalized_left < normalized_right ? -1 : 1;
        }

        ++left_index;
        ++right_index;
    }

    if (left_index == left.size() && right_index == right.size()) {
        return 0;
    }
    return left_index == left.size() ? -1 : 1;
}

template <typename T>
std::string JoinColumnKeys(const std::vector<OutputColumn<T>>& columns) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << columns[index].json_key;
    }
    return stream.str();
}

bool ParseSortOption(const std::optional<std::string>& raw_value, SortOption* option, std::string* error) {
    if (!raw_value.has_value()) {
        if (option != nullptr) {
            *option = SortOption{};
        }
        return true;
    }

    const auto trimmed = TrimAscii(*raw_value);
    if (trimmed.empty()) {
        if (error != nullptr) {
            *error = "--sort cannot be empty";
        }
        return false;
    }

    SortOption parsed;
    const auto separator = trimmed.find(':');
    if (separator == std::string::npos) {
        parsed.field = NormalizeColumnKey(trimmed);
    } else {
        parsed.field = NormalizeColumnKey(trimmed.substr(0, separator));
        const auto direction = ToLowerAscii(TrimAscii(trimmed.substr(separator + 1)));
        if (direction.empty() || direction == "asc") {
            parsed.descending = false;
        } else if (direction == "desc") {
            parsed.descending = true;
        } else {
            if (error != nullptr) {
                *error = "invalid sort direction `" + direction + "`. Use asc or desc";
            }
            return false;
        }
    }

    if (parsed.field.empty()) {
        if (error != nullptr) {
            *error = "--sort must include a field name";
        }
        return false;
    }

    if (option != nullptr) {
        *option = parsed;
    }
    return true;
}

bool ParseFilterConditions(
    const std::optional<std::string>& raw_value,
    std::vector<FilterCondition>* conditions,
    std::string* error) {
    if (conditions == nullptr) {
        return false;
    }

    conditions->clear();
    if (!raw_value.has_value()) {
        return true;
    }

    std::stringstream stream(*raw_value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = TrimAscii(item);
        if (item.empty()) {
            continue;
        }

        const auto separator = item.find('=');
        if (separator == std::string::npos) {
            if (error != nullptr) {
                *error = "filter `" + item + "` must use the form field=value";
            }
            return false;
        }

        const auto field = NormalizeColumnKey(item.substr(0, separator));
        const auto value = TrimAscii(item.substr(separator + 1));
        if (field.empty()) {
            if (error != nullptr) {
                *error = "filter `" + item + "` is missing a field name";
            }
            return false;
        }
        if (value.empty()) {
            if (error != nullptr) {
                *error = "filter `" + item + "` is missing a value";
            }
            return false;
        }

        conditions->push_back({field, value});
    }

    if (conditions->empty()) {
        if (error != nullptr) {
            *error = "--filter cannot be empty";
        }
        return false;
    }
    return true;
}

template <typename T>
// Resolve user-facing column requests once so table and JSON output stay in sync.
bool ResolveRequestedColumns(
    const std::vector<OutputColumn<T>>& available_columns,
    const std::optional<std::string>& raw_columns,
    std::vector<OutputColumn<T>>* selected_columns,
    std::string* error) {
    if (selected_columns == nullptr) {
        return false;
    }

    if (!raw_columns.has_value()) {
        *selected_columns = available_columns;
        return true;
    }

    const auto requested_keys = SplitCommaSeparated(*raw_columns);
    if (requested_keys.empty()) {
        if (error != nullptr) {
            *error = "--columns cannot be empty";
        }
        return false;
    }

    std::vector<OutputColumn<T>> resolved;
    for (const auto& key : requested_keys) {
        const auto match = std::find_if(available_columns.begin(), available_columns.end(), [&key](const auto& column) {
            return column.key == key;
        });
        if (match == available_columns.end()) {
            if (error != nullptr) {
                *error = "unknown column `" + key + "`. Supported values: " + JoinColumnKeys(available_columns);
            }
            return false;
        }

        const auto duplicate = std::find_if(resolved.begin(), resolved.end(), [&key](const auto& column) {
            return column.key == key;
        });
        if (duplicate == resolved.end()) {
            resolved.push_back(*match);
        }
    }

    *selected_columns = resolved;
    return true;
}

template <typename T>
bool SortRowsByColumn(
    std::vector<T>* rows,
    const std::vector<OutputColumn<T>>& available_columns,
    const std::optional<SortOption>& sort_option,
    std::string* error) {
    if (!sort_option.has_value()) {
        return true;
    }

    const auto match = std::find_if(available_columns.begin(), available_columns.end(), [&sort_option](const auto& column) {
        return column.key == sort_option->field;
    });
    if (match == available_columns.end()) {
        if (error != nullptr) {
            *error = "unknown sort field `" + sort_option->field + "`. Supported values: " + JoinColumnKeys(available_columns);
        }
        return false;
    }

    std::stable_sort(rows->begin(), rows->end(), [&match, &sort_option](const T& left, const T& right) {
        const auto comparison = CompareNaturalStrings(match->table_value(left), match->table_value(right));
        if (comparison == 0) {
            return false;
        }
        return sort_option->descending ? comparison > 0 : comparison < 0;
    });
    return true;
}

template <typename T>
// Filters are applied against the same display values used by sorting so CLI behavior stays predictable.
bool ApplyFilters(
    std::vector<T>* rows,
    const std::vector<OutputColumn<T>>& available_columns,
    const std::vector<FilterCondition>& conditions,
    std::string* error) {
    if (conditions.empty()) {
        return true;
    }

    std::vector<const OutputColumn<T>*> resolved_columns;
    resolved_columns.reserve(conditions.size());
    for (const auto& condition : conditions) {
        const auto match = std::find_if(available_columns.begin(), available_columns.end(), [&condition](const auto& column) {
            return column.key == condition.field;
        });
        if (match == available_columns.end()) {
            if (error != nullptr) {
                *error = "unknown filter field `" + condition.field + "`. Supported values: " + JoinColumnKeys(available_columns);
            }
            return false;
        }
        resolved_columns.push_back(&(*match));
    }

    rows->erase(std::remove_if(rows->begin(), rows->end(), [&conditions, &resolved_columns](const T& row) {
        for (std::size_t index = 0; index < conditions.size(); ++index) {
            if (ToLowerAscii(resolved_columns[index]->table_value(row)) != ToLowerAscii(conditions[index].value)) {
                return true;
            }
        }
        return false;
    }), rows->end());
    return true;
}

template <typename T>
void ApplyOutputLimit(std::vector<T>* rows, std::size_t limit) {
    if (rows->size() > limit) {
        rows->resize(limit);
    }
}

template <typename T>
void PrintTableRows(
    const std::vector<OutputColumn<T>>& columns,
    const std::vector<T>& rows,
    bool show_headers) {
    std::vector<std::string> headers;
    headers.reserve(columns.size());
    for (const auto& column : columns) {
        headers.push_back(column.header);
    }

    std::vector<std::vector<std::string>> cells;
    cells.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<std::string> values;
        values.reserve(columns.size());
        for (const auto& column : columns) {
            values.push_back(column.table_value(row));
        }
        cells.push_back(std::move(values));
    }

    PrintTable(headers, cells, show_headers);
}

template <typename T>
void PrintJsonRows(
    const std::vector<OutputColumn<T>>& columns,
    const std::vector<T>& rows) {
    std::vector<JsonFields> items;
    items.reserve(rows.size());
    for (const auto& row : rows) {
        JsonFields fields;
        fields.reserve(columns.size());
        for (const auto& column : columns) {
            fields.push_back({column.json_key, column.json_value(row)});
        }
        items.push_back(std::move(fields));
    }
    PrintJsonArray(items);
}

template <typename T>
void PrintJsonObjectRow(
    const std::vector<OutputColumn<T>>& columns,
    const T& row) {
    JsonFields fields;
    fields.reserve(columns.size());
    for (const auto& column : columns) {
        fields.push_back({column.json_key, column.json_value(row)});
    }
    std::cout << JsonObject(fields) << '\n';
}

}  // namespace

Application::Application(AppPaths paths)
    : paths_(std::move(paths)),
      logger_(paths_.logs),
      audit_store_(paths_.operations_audit_store),
      settings_store_(paths_.settings_store),
      active_store_(paths_.active_runtime_store),
      snapshot_store_(paths_.environment_snapshot_store) {}

int Application::Run(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        PrintHelp();
        return 0;
    }

    std::string ensure_error;
    if (!EnsureAppDirectories(paths_, &ensure_error)) {
        std::cerr << "Failed to initialize application directories: " << ensure_error << '\n';
        return 1;
    }

    std::string settings_error;
    if (!ApplyPersistedNetworkSettings(settings_store_, &settings_error)) {
        std::cerr << "Failed to apply persisted network configuration: " << settings_error << '\n';
        return 1;
    }

    const auto operation_id = GenerateOperationId();
    const auto started_at = NowUtcIso8601();
    const bool suppress_persisted_completion = args[0] == "uninstall" && HasFlag(args, "--purge-data");
    logger_.Info(operation_id, "command started", {{"command", args[0]}, {"developer", kDeveloper}});

    int exit_code = 1;
    if (args[0] == "init") {
        exit_code = HandleInit(operation_id);
    } else if (args[0] == "deinit") {
        exit_code = HandleDeinit(operation_id);
    } else if (args[0] == "uninstall") {
        exit_code = HandleUninstall(args, operation_id);
    } else if (args[0] == "install") {
        exit_code = HandleInstall(args, operation_id);
    } else if (args[0] == "remove") {
        exit_code = HandleRemove(args, operation_id);
    } else if (args[0] == "remote") {
        exit_code = HandleRemote(args, operation_id);
    } else if (args[0] == "search") {
        std::vector<std::string> remote_args{"remote", "list"};
        remote_args.insert(remote_args.end(), args.begin() + 1, args.end());
        exit_code = HandleRemote(remote_args, operation_id);
    } else if (args[0] == "version") {
        exit_code = HandleVersion(args, operation_id);
    } else if (args[0] == "doctor") {
        exit_code = HandleDoctor(operation_id);
    } else if (args[0] == "status") {
        exit_code = HandleStatus(args, operation_id);
    } else if (args[0] == "list") {
        exit_code = HandleList(args, operation_id);
    } else if (args[0] == "current") {
        exit_code = HandleCurrent(args, operation_id);
    } else if (args[0] == "use") {
        exit_code = HandleUse(args, operation_id);
    } else if (args[0] == "unuse") {
        exit_code = HandleUnuse(args, operation_id);
    } else if (args[0] == "exec") {
        exit_code = HandleExec(args, operation_id);
    } else if (args[0] == "shell") {
        exit_code = HandleShell(args, operation_id);
    } else if (args[0] == "logs") {
        exit_code = HandleLogs(args);
    } else if (args[0] == "env") {
        exit_code = HandleEnv(args, operation_id);
    } else if (args[0] == "config") {
        exit_code = HandleConfig(args, operation_id);
    } else if (args[0] == "cache") {
        exit_code = HandleCache(args, operation_id);
    } else if (args[0] == "lock") {
        exit_code = HandleLock(args, operation_id);
    } else if (args[0] == "sync") {
        exit_code = HandleSync(args, operation_id);
    } else {
        logger_.Error(operation_id, "unknown command", {{"command", args[0]}});
        std::cerr << "Unknown command: " << args[0] << "\n\n";
        PrintHelp();
        exit_code = 1;
    }

    const auto ended_at = NowUtcIso8601();
    const OperationRecord record{
        operation_id,
        args[0],
        JoinArgs(args),
        kDeveloper,
        InferRuntimeTypeForAudit(args),
        InferSelectorForAudit(args),
        exit_code,
        exit_code == 0 ? "succeeded" : "failed",
        started_at,
        ended_at,
        exit_code == 0 ? "completed" : "see operational logs for details"
    };

    if (!suppress_persisted_completion) {
        std::string audit_error;
        if (!audit_store_.Append(record, &audit_error)) {
            logger_.Warning(operation_id, "failed to append operation audit record", {{"error", audit_error}});
        }

        logger_.Info(operation_id,
                     exit_code == 0 ? "command completed" : "command failed",
                     {{"command", args[0]}, {"exitCode", std::to_string(exit_code)}, {"developer", kDeveloper}});
    }
    return exit_code;
}

std::vector<InstalledRuntime> Application::LoadKnownRuntimes(std::optional<RuntimeType> filter) const {
    std::string error;
    const auto snapshot = snapshot_store_.Load(&error);
    return ScanInstalledRuntimes(paths_, filter, snapshot.has_value() ? &(*snapshot) : nullptr);
}

bool Application::EnsureEnvironmentSnapshotCaptured(const std::string& operation_id) {
    if (snapshot_store_.Exists()) {
        return true;
    }

    EnvironmentSnapshot snapshot;
    std::string error;
    if (!CaptureEnvironmentSnapshot(paths_, &snapshot, &error)) {
        logger_.Error(operation_id, "failed to capture environment snapshot", {{"error", error}});
        std::cerr << "Failed to capture original environment snapshot: " << error << '\n';
        return false;
    }

    if (!snapshot_store_.Save(snapshot, &error)) {
        logger_.Error(operation_id, "failed to persist environment snapshot", {{"error", error}});
        std::cerr << "Failed to persist original environment snapshot: " << error << '\n';
        return false;
    }

    logger_.Info(operation_id, "captured original environment snapshot",
                 {{"snapshot", PathToUtf8(snapshot_store_.FilePath())},
                  {"javaOriginal", snapshot.external_java_root.has_value() ? PathToUtf8(*snapshot.external_java_root) : ""},
                  {"pythonOriginal", snapshot.external_python_root.has_value() ? PathToUtf8(*snapshot.external_python_root) : ""}});
    return true;
}

std::optional<EnvironmentSnapshot> Application::LoadEnvironmentSnapshot() const {
    std::string error;
    return snapshot_store_.Load(&error);
}

int Application::HandleInstall(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 3) {
        std::cerr << "Usage: jkm install <java|python|node|go|maven|gradle> <selector> [--distribution <name>] [--arch x64]\n";
        return 1;
    }

    const auto runtime_type = ParseRuntimeType(args[1]);
    if (!runtime_type.has_value()) {
        std::cerr << "Unknown runtime type: " << args[1] << '\n';
        return 1;
    }

    const auto distribution = ToLowerAscii(OptionValue(args, "--distribution", DefaultDistribution(*runtime_type)));
    const auto arch = ToLowerAscii(OptionValue(args, "--arch", "x64"));

    if (arch != "x64") {
        std::cerr << "Only --arch x64 is supported right now.\n";
        return 1;
    }

    if (*runtime_type == RuntimeType::Python) {
        if (distribution != "cpython") {
            std::cerr << "Only --distribution cpython is supported right now for Python installs.\n";
            return 1;
        }

        logger_.Info(operation_id, "starting Python install",
                     {{"selector", args[2]}, {"distribution", distribution}, {"arch", arch}});
        std::cout << "Installing Python " << args[2] << " from " << distribution << "..." << std::endl;

        PythonInstallResult install_result;
        std::string error;
        if (!InstallPythonRuntime(paths_, args[2], arch, &install_result, &error)) {
            logger_.Error(operation_id, "Python install failed", {{"selector", args[2]}, {"error", error}});
            std::cerr << "Failed to install Python " << args[2] << ": " << error << '\n';
            return 1;
        }

        logger_.Debug(operation_id, "Python install output", {{"output", install_result.raw_output}});
        logger_.Info(operation_id, "Python install completed",
                     {{"selector", args[2]},
                      {"name", install_result.runtime.name},
                      {"path", PathToUtf8(install_result.runtime.root)},
                      {"version", install_result.resolved_version},
                      {"status", install_result.already_installed ? "already_installed" : "installed"}});

        if (install_result.already_installed) {
            std::cout << "Python runtime is already installed: " << install_result.runtime.name << '\n';
        } else {
            std::cout << "Installed Python runtime: " << install_result.runtime.name << '\n';
        }
        std::cout << "Path:         " << PathToUtf8(install_result.runtime.root) << '\n';
        std::cout << "Version:      " << install_result.resolved_version << '\n';
        std::cout << "Package:      " << install_result.package_name << '\n';
        std::cout << "Distribution: " << install_result.runtime.distribution << '\n';
        std::cout << "Next step:    jkm use python " << PreferredInstalledRuntimeSelector(install_result.runtime) << '\n';
        std::cout << "Env create:   jkm env create python <env-name> --python " << install_result.runtime.name << '\n';
        return 0;
    }

    if (*runtime_type == RuntimeType::Java) {
        if (distribution != "temurin") {
            std::cerr << "Only --distribution temurin is supported right now for Java installs.\n";
            return 1;
        }

        logger_.Info(operation_id, "starting Temurin install",
                     {{"selector", args[2]}, {"distribution", distribution}, {"arch", arch}});
        std::cout << "Installing Java " << args[2] << " from " << distribution << "..." << std::endl;

        JavaInstallResult install_result;
        std::string error;
        if (!InstallTemurinJdk(paths_, args[2], arch, &install_result, &error)) {
            logger_.Error(operation_id, "Temurin install failed", {{"selector", args[2]}, {"error", error}});
            std::cerr << "Failed to install Java " << args[2] << ": " << error << '\n';
            return 1;
        }

        logger_.Debug(operation_id, "Temurin install output", {{"output", install_result.raw_output}});
        logger_.Info(operation_id, "Temurin install completed",
                     {{"selector", args[2]},
                      {"name", install_result.runtime.name},
                      {"path", PathToUtf8(install_result.runtime.root)},
                      {"status", install_result.already_installed ? "already_installed" : "installed"}});

        if (install_result.already_installed) {
            std::cout << "Java runtime is already installed: " << install_result.runtime.name << '\n';
        } else {
            std::cout << "Installed Java runtime: " << install_result.runtime.name << '\n';
        }
        std::cout << "Path:        " << PathToUtf8(install_result.runtime.root) << '\n';
        std::cout << "Package:     " << install_result.package_name << '\n';
        std::cout << "Release:     " << install_result.release_name << '\n';
        std::cout << "Distribution:" << ' ' << install_result.runtime.distribution << '\n';
        std::cout << "Next step:   jkm use java " << install_result.runtime.name << '\n';
        return 0;
    }

    ToolInstallResult install_result;
    std::string error;
    bool installed = false;

    switch (*runtime_type) {
        case RuntimeType::Node:
            if (distribution != "nodejs") {
                std::cerr << "Only --distribution nodejs is supported right now for Node.js installs.\n";
                return 1;
            }
            logger_.Info(operation_id, "starting Node.js install",
                         {{"selector", args[2]}, {"distribution", distribution}, {"arch", arch}});
            std::cout << "Installing Node.js " << args[2] << " from " << distribution << "..." << std::endl;
            installed = InstallNodeRuntime(paths_, args[2], arch, &install_result, &error);
            break;
        case RuntimeType::Go:
            if (distribution != "golang") {
                std::cerr << "Only --distribution golang is supported right now for Go installs.\n";
                return 1;
            }
            logger_.Info(operation_id, "starting Go install",
                         {{"selector", args[2]}, {"distribution", distribution}, {"arch", arch}});
            std::cout << "Installing Go " << args[2] << " from " << distribution << "..." << std::endl;
            installed = InstallGoRuntime(paths_, args[2], arch, &install_result, &error);
            break;
        case RuntimeType::Maven:
            if (distribution != "apache") {
                std::cerr << "Only --distribution apache is supported right now for Maven installs.\n";
                return 1;
            }
            logger_.Info(operation_id, "starting Maven install",
                         {{"selector", args[2]}, {"distribution", distribution}});
            std::cout << "Installing Maven " << args[2] << " from " << distribution << "..." << std::endl;
            installed = InstallMavenRuntime(paths_, args[2], &install_result, &error);
            break;
        case RuntimeType::Gradle:
            if (distribution != "gradle") {
                std::cerr << "Only --distribution gradle is supported right now for Gradle installs.\n";
                return 1;
            }
            logger_.Info(operation_id, "starting Gradle install",
                         {{"selector", args[2]}, {"distribution", distribution}});
            std::cout << "Installing Gradle " << args[2] << " from " << distribution << "..." << std::endl;
            installed = InstallGradleRuntime(paths_, args[2], &install_result, &error);
            break;
        default:
            break;
    }

    if (!installed) {
        logger_.Error(operation_id, "tool install failed",
                      {{"type", ToString(*runtime_type)}, {"selector", args[2]}, {"error", error}});
        std::cerr << "Failed to install " << ToString(*runtime_type) << ' ' << args[2] << ": " << error << '\n';
        return 1;
    }

    logger_.Debug(operation_id, "tool install output", {{"output", install_result.raw_output}});
    logger_.Info(operation_id, "tool install completed",
                 {{"type", ToString(*runtime_type)},
                  {"selector", args[2]},
                  {"name", install_result.runtime.name},
                  {"path", PathToUtf8(install_result.runtime.root)},
                  {"status", install_result.already_installed ? "already_installed" : "installed"}});

    if (install_result.already_installed) {
        std::cout << ToString(*runtime_type) << " runtime is already installed: " << install_result.runtime.name << '\n';
    } else {
        std::cout << "Installed " << ToString(*runtime_type) << " runtime: " << install_result.runtime.name << '\n';
    }
    std::cout << "Path:         " << PathToUtf8(install_result.runtime.root) << '\n';
    std::cout << "Version:      " << install_result.resolved_version << '\n';
    std::cout << "Package:      " << install_result.package_name << '\n';
    std::cout << "Distribution: " << install_result.runtime.distribution << '\n';
    if (!install_result.channel.empty()) {
        std::cout << "Channel:      " << install_result.channel << '\n';
    }
    if (!install_result.published_at.empty()) {
        std::cout << "Published:    " << install_result.published_at << '\n';
    }
    std::cout << "Next step:    jkm use " << ToString(*runtime_type) << ' '
              << PreferredInstalledRuntimeSelector(install_result.runtime) << '\n';
    return 0;
}

int Application::HandleRemove(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 3) {
        std::cerr << "Usage: jkm remove <java|python|node|go|maven|gradle> <selector>\n";
        return 1;
    }

    const auto runtime_type = ParseRuntimeType(args[1]);
    if (!runtime_type.has_value()) {
        std::cerr << "Unknown runtime type: " << args[1] << '\n';
        return 1;
    }

    const auto installed = LoadKnownRuntimes(*runtime_type);
    const auto matches = FindInstalledRuntimeMatches(installed, *runtime_type, args[2]);

    if (matches.empty()) {
        std::cerr << "No installed runtime matched selector `" << args[2] << "`.\n";
        return 1;
    }

    if (matches.size() > 1) {
        std::cerr << "Selector `" << args[2] << "` is ambiguous. Candidates:\n";
        for (const auto& match : matches) {
            PrintInstalledRuntime(match);
        }
        return 1;
    }

    const auto& selected = matches.front();
    if (selected.is_external) {
        std::cerr << "The preserved original runtime is read-only and cannot be removed.\n";
        return 1;
    }

    const auto active_runtime = active_store_.Get(*runtime_type);
    if (active_runtime.has_value() && PathsOverlapForRemoval(selected.root, active_runtime->root)) {
        std::cerr << "Cannot remove the active " << ToString(*runtime_type)
                  << " runtime or one of its parent directories. Switch first.\n";
        return 1;
    }

    const auto local_active_runtime = LoadProjectActiveRuntime(*runtime_type);
    if (local_active_runtime.has_value() && PathsOverlapForRemoval(selected.root, local_active_runtime->root)) {
        std::cerr << "Cannot remove the current project's local " << ToString(*runtime_type)
                  << " runtime. Change the local selection first.\n";
        return 1;
    }

    std::error_code ec;
    const auto removed_count = fs::remove_all(selected.root, ec);
    if (ec) {
        logger_.Error(operation_id, "failed to remove runtime directory",
                      {{"selector", args[2]}, {"path", PathToUtf8(selected.root)}, {"error", ec.message()}});
        std::cerr << "Failed to remove " << PathToUtf8(selected.root) << ": " << ec.message() << '\n';
        return 1;
    }

    logger_.Info(operation_id, "runtime removed",
                 {{"selector", args[2]},
                  {"path", PathToUtf8(selected.root)},
                  {"removed_count", std::to_string(static_cast<unsigned long long>(removed_count))}});

    std::cout << "Removed " << ToString(*runtime_type) << " runtime: " << selected.name << '\n';
    std::cout << "Path: " << PathToUtf8(selected.root) << '\n';
    return 0;
}

int Application::HandleRemote(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 3 || ToLowerAscii(args[1]) != "list") {
        std::cerr << "Usage: jkm remote list <java|python|node|go|maven|gradle> [selector] "
                  << "[--distribution <name>] [--arch x64] [--limit 10] [--latest] "
                  << "[--format table|json] [--no-headers] [--columns <field,field>] "
                  << "[--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
        return 1;
    }

    const auto runtime_type = ParseRuntimeType(args[2]);
    if (!runtime_type.has_value()) {
        std::cerr << "Unknown runtime type: " << args[2] << '\n';
        return 1;
    }

    std::string selector;
    if (args.size() >= 4 && !args[3].empty() && args[3][0] != '-') {
        selector = args[3];
    }

    const auto distribution = ToLowerAscii(OptionValue(args, "--distribution", DefaultDistribution(*runtime_type)));
    const auto arch = ToLowerAscii(OptionValue(args, "--arch", "x64"));
    const auto limit_raw = OptionValue(args, "--limit", "10");
    const auto format = ToLowerAscii(OptionValue(args, "--format", "table"));
    const auto columns_raw = TryOptionValue(args, "--columns");
    const auto sort_raw = TryOptionValue(args, "--sort");
    const auto filter_raw = TryOptionValue(args, "--filter");
    const bool latest_only = HasFlag(args, "--latest");
    const bool show_headers = !HasFlag(args, "--no-headers");
    if (arch != "x64") {
        std::cerr << "Only --arch x64 is supported right now.\n";
        return 1;
    }
    if (format != "table" && format != "json") {
        std::cerr << "Invalid --format value: " << format << ". Supported values: table, json\n";
        return 1;
    }
    if (format == "json" && !show_headers) {
        logger_.Debug(operation_id, "ignoring --no-headers for JSON output");
    }

    std::optional<SortOption> sort_option;
    if (sort_raw.has_value()) {
        SortOption parsed_sort;
        std::string sort_error;
        if (!ParseSortOption(sort_raw, &parsed_sort, &sort_error)) {
            std::cerr << "Invalid --sort value: " << sort_error << '\n';
            return 1;
        }
        sort_option = parsed_sort;
    }

    std::vector<FilterCondition> filter_conditions;
    if (filter_raw.has_value()) {
        std::string filter_error;
        if (!ParseFilterConditions(filter_raw, &filter_conditions, &filter_error)) {
            std::cerr << "Invalid --filter value: " << filter_error << '\n';
            return 1;
        }
    }

    int requested_limit = 10;
    try {
        requested_limit = std::stoi(limit_raw);
    } catch (...) {
        std::cerr << "Invalid --limit value: " << limit_raw << '\n';
        return 1;
    }
    if (requested_limit <= 0) {
        std::cerr << "--limit must be greater than 0.\n";
        return 1;
    }
    const int output_limit = latest_only ? 1 : requested_limit;
    const int query_limit = (latest_only || sort_option.has_value() || !filter_conditions.empty())
        ? std::max(output_limit, 50)
        : output_limit;

    if (*runtime_type == RuntimeType::Python) {
        if (distribution != "cpython") {
            std::cerr << "Only --distribution cpython is supported right now for Python remote queries.\n";
            return 1;
        }

        std::vector<PythonRemoteRelease> releases;
        std::string error;
        if (!QueryPythonRemoteReleases(selector, arch, query_limit, &releases, &error)) {
            logger_.Error(operation_id, "failed to query Python remote releases",
                          {{"selector", selector}, {"error", error}});
            std::cerr << "Failed to query Python releases"
                      << (selector.empty() ? std::string{} : " for " + selector) << ": " << error << '\n';
            return 1;
        }

        const std::vector<OutputColumn<PythonRemoteRelease>> available_columns{
            {"version", "version", "VERSION",
             [](const PythonRemoteRelease& release) { return release.version; },
             [](const PythonRemoteRelease& release) { return JsonString(release.version); }},
            {"packageid", "packageId", "PACKAGE_ID",
             [](const PythonRemoteRelease& release) { return release.package_id; },
             [](const PythonRemoteRelease& release) { return JsonString(release.package_id); }},
            {"packagename", "packageName", "PACKAGE_NAME",
             [](const PythonRemoteRelease& release) { return release.package_name; },
             [](const PythonRemoteRelease& release) { return JsonString(release.package_name); }},
            {"channel", "channel", "CHANNEL",
             [](const PythonRemoteRelease& release) { return release.prerelease ? "prerelease" : "stable"; },
             [](const PythonRemoteRelease& release) { return JsonString(release.prerelease ? "prerelease" : "stable"); }},
            {"downloadurl", "downloadUrl", "DOWNLOAD_URL",
             [](const PythonRemoteRelease& release) { return release.download_url; },
             [](const PythonRemoteRelease& release) { return JsonString(release.download_url); }},
            {"prerelease", "prerelease", "PRERELEASE",
             [](const PythonRemoteRelease& release) { return release.prerelease ? "true" : "false"; },
             [](const PythonRemoteRelease& release) { return JsonBoolean(release.prerelease); }}
        };

        std::vector<OutputColumn<PythonRemoteRelease>> selected_columns;
        if (!ResolveRequestedColumns(available_columns, columns_raw, &selected_columns, &error)) {
            std::cerr << "Invalid --columns value: " << error << '\n';
            return 1;
        }
        if (!ApplyFilters(&releases, available_columns, filter_conditions, &error)) {
            std::cerr << "Invalid --filter value: " << error << '\n';
            return 1;
        }
        if (!SortRowsByColumn(&releases, available_columns, sort_option, &error)) {
            std::cerr << "Invalid --sort value: " << error << '\n';
            return 1;
        }
        ApplyOutputLimit(&releases, static_cast<std::size_t>(output_limit));

        if (releases.empty()) {
            std::cout << "No Python releases matched selector `" << selector << "`.\n";
            return 0;
        }

        logger_.Info(operation_id, "queried Python remote releases",
                     {{"selector", selector},
                      {"count", std::to_string(releases.size())},
                      {"format", format},
                      {"latestOnly", latest_only ? "true" : "false"},
                      {"sort", sort_option.has_value() ? sort_option->field + (sort_option->descending ? ":desc" : ":asc") : ""},
                      {"columns", columns_raw.has_value() ? *columns_raw : ""},
                      {"filter", filter_raw.has_value() ? *filter_raw : ""}});

        if (format == "json") {
            PrintJsonRows(selected_columns, releases);
            return 0;
        }

        PrintTableRows(selected_columns, releases, show_headers);
        return 0;
    }

    if (*runtime_type == RuntimeType::Java) {
        if (distribution != "temurin") {
            std::cerr << "Only --distribution temurin is supported right now for Java remote queries.\n";
            return 1;
        }

        if (selector.empty()) {
            TemurinAvailableReleases releases;
            std::string error;
            if (!QueryTemurinAvailableReleases(&releases, &error)) {
                logger_.Error(operation_id, "failed to query Temurin available releases", {{"error", error}});
                std::cerr << "Failed to query Temurin releases: " << error << '\n';
                return 1;
            }

            struct TemurinSummaryRow {
                std::string distribution;
                std::vector<int> available_releases;
                std::vector<int> lts_releases;
                int most_recent_feature_release{0};
                int most_recent_lts_release{0};
            };

            TemurinSummaryRow summary{
                "temurin",
                releases.available_releases,
                releases.lts_releases,
                releases.most_recent_feature_release,
                releases.most_recent_lts
            };

            const std::vector<OutputColumn<TemurinSummaryRow>> available_columns{
                {"distribution", "distribution", "DISTRIBUTION",
                 [](const TemurinSummaryRow& row) { return row.distribution; },
                 [](const TemurinSummaryRow& row) { return JsonString(row.distribution); }},
                {"availablereleases", "availableReleases", "AVAILABLE_RELEASES",
                 [](const TemurinSummaryRow& row) { return JoinReleaseNumbers(row.available_releases); },
                 [](const TemurinSummaryRow& row) { return JsonIntArray(row.available_releases); }},
                {"ltsreleases", "ltsReleases", "LTS_RELEASES",
                 [](const TemurinSummaryRow& row) { return JoinReleaseNumbers(row.lts_releases); },
                 [](const TemurinSummaryRow& row) { return JsonIntArray(row.lts_releases); }},
                {"mostrecentfeaturerelease", "mostRecentFeatureRelease", "MOST_RECENT_FEATURE",
                 [](const TemurinSummaryRow& row) { return std::to_string(row.most_recent_feature_release); },
                 [](const TemurinSummaryRow& row) { return JsonNumber(row.most_recent_feature_release); }},
                {"mostrecentltsrelease", "mostRecentLtsRelease", "MOST_RECENT_LTS",
                 [](const TemurinSummaryRow& row) { return std::to_string(row.most_recent_lts_release); },
                 [](const TemurinSummaryRow& row) { return JsonNumber(row.most_recent_lts_release); }}
            };

            std::vector<OutputColumn<TemurinSummaryRow>> selected_columns;
            if (!ResolveRequestedColumns(available_columns, columns_raw, &selected_columns, &error)) {
                std::cerr << "Invalid --columns value: " << error << '\n';
                return 1;
            }

            std::vector<TemurinSummaryRow> rows{summary};
            if (!ApplyFilters(&rows, available_columns, filter_conditions, &error)) {
                std::cerr << "Invalid --filter value: " << error << '\n';
                return 1;
            }
            if (rows.empty()) {
                std::cout << "No Temurin feature release summary matched the current filter.\n";
                return 0;
            }

            logger_.Info(operation_id, "queried Temurin feature releases",
                         {{"format", format},
                          {"latestOnly", latest_only ? "true" : "false"},
                          {"columns", columns_raw.has_value() ? *columns_raw : ""},
                          {"filter", filter_raw.has_value() ? *filter_raw : ""}});

            if (format == "json") {
                PrintJsonObjectRow(selected_columns, rows.front());
                return 0;
            }

            PrintTableRows(selected_columns, rows, show_headers);
            std::cout << "Tip: run `jkm remote list java 21` to see concrete Temurin builds.\n";
            return 0;
        }

        std::vector<JavaRemoteRelease> releases;
        std::string error;
        if (!QueryTemurinRemoteReleases(selector, arch, query_limit, &releases, &error)) {
            logger_.Error(operation_id, "failed to query Temurin remote releases",
                          {{"selector", selector}, {"error", error}});
            std::cerr << "Failed to query Temurin releases for " << selector << ": " << error << '\n';
            return 1;
        }

        const std::vector<OutputColumn<JavaRemoteRelease>> available_columns{
            {"release", "release", "RELEASE",
             [](const JavaRemoteRelease& release) { return release.release_name; },
             [](const JavaRemoteRelease& release) { return JsonString(release.release_name); }},
            {"openjdkversion", "openjdkVersion", "OPENJDK_VERSION",
             [](const JavaRemoteRelease& release) { return release.openjdk_version; },
             [](const JavaRemoteRelease& release) { return JsonString(release.openjdk_version); }},
            {"semver", "semver", "SEMVER",
             [](const JavaRemoteRelease& release) { return release.semver; },
             [](const JavaRemoteRelease& release) { return JsonString(release.semver); }},
            {"updatedat", "updatedAt", "UPDATED_AT",
             [](const JavaRemoteRelease& release) { return release.updated_at; },
             [](const JavaRemoteRelease& release) { return JsonString(release.updated_at); }},
            {"packagename", "packageName", "PACKAGE_NAME",
             [](const JavaRemoteRelease& release) { return release.package_name; },
             [](const JavaRemoteRelease& release) { return JsonString(release.package_name); }},
            {"downloadurl", "downloadUrl", "DOWNLOAD_URL",
             [](const JavaRemoteRelease& release) { return release.download_url; },
             [](const JavaRemoteRelease& release) { return JsonString(release.download_url); }},
            {"checksum", "checksum", "CHECKSUM",
             [](const JavaRemoteRelease& release) { return release.checksum; },
             [](const JavaRemoteRelease& release) { return JsonString(release.checksum); }}
        };

        std::vector<OutputColumn<JavaRemoteRelease>> selected_columns;
        if (!ResolveRequestedColumns(available_columns, columns_raw, &selected_columns, &error)) {
            std::cerr << "Invalid --columns value: " << error << '\n';
            return 1;
        }
        if (!ApplyFilters(&releases, available_columns, filter_conditions, &error)) {
            std::cerr << "Invalid --filter value: " << error << '\n';
            return 1;
        }
        if (!SortRowsByColumn(&releases, available_columns, sort_option, &error)) {
            std::cerr << "Invalid --sort value: " << error << '\n';
            return 1;
        }
        ApplyOutputLimit(&releases, static_cast<std::size_t>(output_limit));

        if (releases.empty()) {
            std::cout << "No Temurin releases matched selector `" << selector << "`.\n";
            return 0;
        }

        logger_.Info(operation_id, "queried Temurin remote releases",
                     {{"selector", selector},
                      {"count", std::to_string(releases.size())},
                      {"format", format},
                      {"latestOnly", latest_only ? "true" : "false"},
                      {"sort", sort_option.has_value() ? sort_option->field + (sort_option->descending ? ":desc" : ":asc") : ""},
                      {"columns", columns_raw.has_value() ? *columns_raw : ""},
                      {"filter", filter_raw.has_value() ? *filter_raw : ""}});

        if (format == "json") {
            PrintJsonRows(selected_columns, releases);
            return 0;
        }

        PrintTableRows(selected_columns, releases, show_headers);
        return 0;
    }

    std::vector<ToolRemoteRelease> releases;
    std::string error;
    bool queried = false;

    switch (*runtime_type) {
        case RuntimeType::Node:
            if (distribution != "nodejs") {
                std::cerr << "Only --distribution nodejs is supported right now for Node.js remote queries.\n";
                return 1;
            }
            queried = QueryNodeRemoteReleases(selector, arch, query_limit, &releases, &error);
            break;
        case RuntimeType::Go:
            if (distribution != "golang") {
                std::cerr << "Only --distribution golang is supported right now for Go remote queries.\n";
                return 1;
            }
            queried = QueryGoRemoteReleases(selector, arch, query_limit, &releases, &error);
            break;
        case RuntimeType::Maven:
            if (distribution != "apache") {
                std::cerr << "Only --distribution apache is supported right now for Maven remote queries.\n";
                return 1;
            }
            queried = QueryMavenRemoteReleases(selector, query_limit, &releases, &error);
            break;
        case RuntimeType::Gradle:
            if (distribution != "gradle") {
                std::cerr << "Only --distribution gradle is supported right now for Gradle remote queries.\n";
                return 1;
            }
            queried = QueryGradleRemoteReleases(selector, query_limit, &releases, &error);
            break;
        default:
            break;
    }

    if (!queried) {
        logger_.Error(operation_id, "failed to query tool remote releases",
                      {{"type", ToString(*runtime_type)}, {"selector", selector}, {"error", error}});
        std::cerr << "Failed to query " << ToString(*runtime_type)
                  << " releases" << (selector.empty() ? std::string{} : " for " + selector) << ": " << error << '\n';
        return 1;
    }

    if (releases.empty()) {
        std::cout << "No " << ToString(*runtime_type) << " releases matched selector `" << selector << "`.\n";
        return 0;
    }

    const std::vector<OutputColumn<ToolRemoteRelease>> available_columns{
        {"version", "version", "VERSION",
         [](const ToolRemoteRelease& release) { return release.version; },
         [](const ToolRemoteRelease& release) { return JsonString(release.version); }},
        {"packagename", "packageName", "PACKAGE_NAME",
         [](const ToolRemoteRelease& release) { return release.package_name; },
         [](const ToolRemoteRelease& release) { return JsonString(release.package_name); }},
        {"channel", "channel", "CHANNEL",
         [](const ToolRemoteRelease& release) { return release.channel.empty() ? "stable" : release.channel; },
         [](const ToolRemoteRelease& release) { return JsonString(release.channel.empty() ? "stable" : release.channel); }},
        {"publishedat", "publishedAt", "PUBLISHED_AT",
         [](const ToolRemoteRelease& release) { return release.published_at; },
         [](const ToolRemoteRelease& release) { return JsonString(release.published_at); }},
        {"downloadurl", "downloadUrl", "DOWNLOAD_URL",
         [](const ToolRemoteRelease& release) { return release.download_url; },
         [](const ToolRemoteRelease& release) { return JsonString(release.download_url); }},
        {"checksum", "checksum", "CHECKSUM",
         [](const ToolRemoteRelease& release) { return release.checksum; },
         [](const ToolRemoteRelease& release) { return JsonString(release.checksum); }}
    };

    std::vector<OutputColumn<ToolRemoteRelease>> selected_columns;
    if (!ResolveRequestedColumns(available_columns, columns_raw, &selected_columns, &error)) {
        std::cerr << "Invalid --columns value: " << error << '\n';
        return 1;
    }
    if (!ApplyFilters(&releases, available_columns, filter_conditions, &error)) {
        std::cerr << "Invalid --filter value: " << error << '\n';
        return 1;
    }
    if (!SortRowsByColumn(&releases, available_columns, sort_option, &error)) {
        std::cerr << "Invalid --sort value: " << error << '\n';
        return 1;
    }
    ApplyOutputLimit(&releases, static_cast<std::size_t>(output_limit));

    logger_.Info(operation_id, "queried tool remote releases",
                 {{"type", ToString(*runtime_type)},
                  {"selector", selector},
                  {"count", std::to_string(releases.size())},
                  {"format", format},
                  {"latestOnly", latest_only ? "true" : "false"},
                  {"sort", sort_option.has_value() ? sort_option->field + (sort_option->descending ? ":desc" : ":asc") : ""},
                  {"columns", columns_raw.has_value() ? *columns_raw : ""},
                  {"filter", filter_raw.has_value() ? *filter_raw : ""}});

    if (format == "json") {
        PrintJsonRows(selected_columns, releases);
        return 0;
    }

    PrintTableRows(selected_columns, releases, show_headers);
    return 0;
}

int Application::HandleVersion(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 2) {
        std::cerr << "Usage: jkm version <java|python|node|go|maven|gradle> [selector]\n";
        return 1;
    }

    const auto runtime_type = ParseRuntimeType(args[1]);
    if (!runtime_type.has_value()) {
        std::cerr << "Unknown runtime type: " << args[1] << '\n';
        return 1;
    }

    std::optional<InstalledRuntime> selected_runtime;
    if (args.size() >= 3 && !args[2].empty() && args[2][0] != '-') {
        const auto installed = LoadKnownRuntimes(*runtime_type);
        const auto matches = FindInstalledRuntimeMatches(installed, *runtime_type, args[2]);
        if (matches.empty()) {
            std::cerr << "No installed runtime matched selector `" << args[2] << "`.\n";
            return 1;
        }
        if (matches.size() > 1) {
            std::cerr << "Selector `" << args[2] << "` is ambiguous. Candidates:\n";
            for (const auto& match : matches) {
                PrintInstalledRuntime(match);
            }
            return 1;
        }
        selected_runtime = matches.front();
    } else {
        const auto active_runtime = EffectiveActiveRuntime(active_store_, *runtime_type);
        if (!active_runtime.has_value()) {
            std::cerr << "No active " << ToString(*runtime_type)
                      << " runtime is recorded. Pass a selector or switch one first.\n";
            return 1;
        }

        const auto installed = LoadKnownRuntimes(*runtime_type);
        const auto matches = FindInstalledRuntimeMatches(installed, *runtime_type, active_runtime->selected_name);
        if (!matches.empty()) {
            selected_runtime = matches.front();
        } else {
            selected_runtime = InstalledRuntime{
                *runtime_type,
                active_runtime->distribution,
                active_runtime->selected_name,
                active_runtime->root,
                active_runtime->selected_name,
                active_runtime->root,
                false,
                false
            };
        }
    }

    const auto executable = ResolveRuntimeExecutable(*selected_runtime);
    if (!executable.has_value()) {
        std::cerr << "Unable to locate the executable for selector `"
                  << PreferredInstalledRuntimeSelector(*selected_runtime) << "`.\n";
        return 1;
    }

    ProcessResult process_result;
    std::string error;
    std::ostringstream script;
    script << "$ErrorActionPreference = 'Stop'\n";
    script << "$ProgressPreference = 'SilentlyContinue'\n";
    script << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n";
    script << BuildPowerShellActivationScript(paths_, RuntimeContextForInstalledRuntimeProbe(active_store_, *selected_runtime));
    script << BuildPowerShellProcessInvocation(*executable, VersionArguments(selected_runtime->type));

    if (!RunPowerShellScript(script.str(), &process_result, &error)) {
        logger_.Error(operation_id, "runtime version probe failed",
                      {{"type", ToString(*runtime_type)},
                       {"selector", PreferredInstalledRuntimeSelector(*selected_runtime)},
                       {"error", error}});
        std::cerr << "Failed to probe runtime version: " << error << '\n';
        return 1;
    }

    if (process_result.exit_code != 0) {
        logger_.Error(operation_id, "runtime version probe returned non-zero",
                      {{"type", ToString(*runtime_type)},
                       {"selector", PreferredInstalledRuntimeSelector(*selected_runtime)},
                       {"exitCode", std::to_string(process_result.exit_code)},
                       {"output", TrimOutput(process_result.output)}});
        std::cerr << "Version probe exited with code " << process_result.exit_code << '\n';
        std::cerr << TrimOutput(process_result.output) << '\n';
        return 1;
    }

    const auto output = TrimOutput(process_result.output);
    logger_.Info(operation_id, "runtime version probed",
                 {{"type", ToString(*runtime_type)},
                  {"selector", PreferredInstalledRuntimeSelector(*selected_runtime)},
                  {"executable", PathToUtf8(*executable)}});

    std::cout << ToString(*runtime_type)
              << " | selector=" << PreferredInstalledRuntimeSelector(*selected_runtime)
              << " | distribution=" << selected_runtime->distribution << '\n';
    std::cout << "Path:       " << PathToUtf8(selected_runtime->root) << '\n';
    std::cout << "Executable: " << PathToUtf8(*executable) << '\n';
    std::cout << "Version:\n" << output << '\n';
    return 0;
}

int Application::HandleInit(const std::string& operation_id) {
    if (!EnsureEnvironmentSnapshotCaptured(operation_id)) {
        return 1;
    }

    std::string error;
    if (!SetUserEnvironmentVariable("JDKM_HOME", PathToUtf8(paths_.root), &error)) {
        logger_.Error(operation_id, "failed to set JDKM_HOME", {{"error", error}});
        std::cerr << "Failed to set JDKM_HOME: " << error << '\n';
        return 1;
    }

    for (const auto& entry : ManagedPathEntries(paths_)) {
        if (!EnsureUserPathEntry(entry, &error)) {
            logger_.Error(operation_id, "failed to update user PATH",
                          {{"error", error}, {"entry", PathToUtf8(entry)}});
            std::cerr << "Failed to update user PATH: " << error << '\n';
            return 1;
        }
    }

    if (!BroadcastEnvironmentChanged(&error)) {
        logger_.Warning(operation_id, "environment change broadcast failed", {{"error", error}});
    }

    logger_.Info(operation_id, "initialization completed",
                 {{"root", PathToUtf8(paths_.root)}});

    std::cout << "Initialization complete.\n";
    std::cout << "JDKM_HOME: " << PathToUtf8(paths_.root) << '\n';
    std::cout << "PATH entries ensured:\n";
    for (const auto& entry : ManagedPathEntries(paths_)) {
        std::cout << "  " << PathToUtf8(entry) << '\n';
    }
    std::cout << "Open a new terminal to pick up persistent environment variable changes.\n";
    return 0;
}

int Application::HandleDeinit(const std::string& operation_id) {
    std::string error;
    const auto snapshot = snapshot_store_.Load(&error);

    if (snapshot.has_value()) {
        if (!RestoreUserEnvironmentVariable("Path", snapshot->path_value, &error) ||
            !RestoreUserEnvironmentVariable("JAVA_HOME", snapshot->java_home, &error) ||
            !RestoreUserEnvironmentVariable("PYTHON_HOME", snapshot->python_home, &error) ||
            !RestoreUserEnvironmentVariable("CONDA_PREFIX", snapshot->conda_prefix, &error) ||
            !RestoreUserEnvironmentVariable("NODE_HOME", snapshot->node_home, &error) ||
            !RestoreUserEnvironmentVariable("GOROOT", snapshot->go_root, &error) ||
            !RestoreUserEnvironmentVariable("MAVEN_HOME", snapshot->maven_home, &error) ||
            !RestoreUserEnvironmentVariable("M2_HOME", snapshot->m2_home, &error) ||
            !RestoreUserEnvironmentVariable("GRADLE_HOME", snapshot->gradle_home, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_HOME", snapshot->jdkm_home, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_JAVA", snapshot->jdkm_current_java, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_PYTHON", snapshot->jdkm_current_python, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_PYTHON_BASE", snapshot->jdkm_current_python_base, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_PYTHON_ENV", snapshot->jdkm_current_python_env, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_NODE", snapshot->jdkm_current_node, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_GO", snapshot->jdkm_current_go, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_MAVEN", snapshot->jdkm_current_maven, &error) ||
            !RestoreUserEnvironmentVariable("JDKM_CURRENT_GRADLE", snapshot->jdkm_current_gradle, &error)) {
            logger_.Error(operation_id, "failed to restore original environment", {{"error", error}});
            std::cerr << "Failed to restore original environment: " << error << '\n';
            return 1;
        }
    } else {
        logger_.Warning(operation_id, "environment snapshot missing during deinit", {{"error", error}});

        const auto path_value = ReadUserEnvironmentVariable("Path", &error);
        if (path_value.has_value()) {
            const auto cleaned_path = RemoveManagedPathEntries(*path_value, paths_);
            if (cleaned_path.empty()) {
                if (!DeleteUserEnvironmentVariable("Path", &error)) {
                    std::cerr << "Failed to clean user PATH: " << error << '\n';
                    return 1;
                }
            } else if (!SetUserEnvironmentVariable("Path", cleaned_path, &error)) {
                std::cerr << "Failed to clean user PATH: " << error << '\n';
                return 1;
            }
        }

        if (!DeleteUserEnvironmentVariable("JAVA_HOME", &error) ||
            !DeleteUserEnvironmentVariable("PYTHON_HOME", &error) ||
            !DeleteUserEnvironmentVariable("CONDA_PREFIX", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_HOME", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_JAVA", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_PYTHON", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_PYTHON_BASE", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_PYTHON_ENV", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_NODE", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_GO", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_MAVEN", &error) ||
            !DeleteUserEnvironmentVariable("JDKM_CURRENT_GRADLE", &error) ||
            !DeleteUserEnvironmentVariable("NODE_HOME", &error) ||
            !DeleteUserEnvironmentVariable("GOROOT", &error) ||
            !DeleteUserEnvironmentVariable("MAVEN_HOME", &error) ||
            !DeleteUserEnvironmentVariable("M2_HOME", &error) ||
            !DeleteUserEnvironmentVariable("GRADLE_HOME", &error)) {
            std::cerr << "Failed to remove JDKM variables: " << error << '\n';
            return 1;
        }
    }

    std::string remove_error;
    for (const auto type : {RuntimeType::Java, RuntimeType::Python, RuntimeType::Node, RuntimeType::Go, RuntimeType::Maven, RuntimeType::Gradle}) {
        if (!RemoveDirectoryJunction(CurrentLinkPath(paths_, type), &remove_error)) {
            logger_.Warning(operation_id, "failed to remove current runtime link",
                            {{"type", ToString(type)}, {"error", remove_error}});
        }
    }

    if (!BroadcastEnvironmentChanged(&error)) {
        logger_.Warning(operation_id, "environment change broadcast failed", {{"error", error}});
    }

    if (!active_store_.Clear(&error)) {
        logger_.Warning(operation_id, "failed to clear active runtime store", {{"error", error}});
    }

    logger_.Info(operation_id, "environment restored for deinit",
                 {{"snapshot", snapshot.has_value() ? PathToUtf8(snapshot_store_.FilePath()) : "missing"}});

    std::cout << "Deinitialization complete.\n";
    if (snapshot.has_value()) {
        std::cout << "Original environment was restored from " << PathToUtf8(snapshot_store_.FilePath()) << '\n';
    } else {
        std::cout << "Environment snapshot was missing; only JDKM-managed variables and PATH entries were removed.\n";
    }
    std::cout << "Open a new terminal to refresh environment variables.\n";
    return 0;
}

int Application::HandleUninstall(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() >= 3) {
        const auto runtime_type = ParseRuntimeType(args[1]);
        if (runtime_type.has_value()) {
            std::vector<std::string> remove_args{"remove"};
            remove_args.insert(remove_args.end(), args.begin() + 1, args.end());
            return HandleRemove(remove_args, operation_id);
        }
    }

    const bool purge_data = HasFlag(args, "--purge-data");
    if (args.size() > 1 && !purge_data) {
        std::cerr << "Usage: jkm uninstall [--purge-data]\n";
        std::cerr << "   or: jkm uninstall <java|python|node|go|maven|gradle> <selector>\n";
        return 1;
    }

    const auto deinit_exit_code = HandleDeinit(operation_id);
    if (deinit_exit_code != 0) {
        return deinit_exit_code;
    }

    if (!purge_data) {
        logger_.Info(operation_id, "uninstall restore completed without data purge",
                     {{"root", PathToUtf8(paths_.root)}});
        std::cout << "JDKM environment hooks were removed.\n";
        std::cout << "Managed data was kept at " << PathToUtf8(paths_.root) << '\n';
        std::cout << "Run `jkm uninstall --purge-data` if you also want to delete managed data.\n";
        return 0;
    }

    logger_.Info(operation_id, "starting managed data purge",
                 {{"root", PathToUtf8(paths_.root)}});

    std::error_code ec;
    const auto removed_count = fs::remove_all(paths_.root, ec);
    if (ec) {
        std::cerr << "Environment was restored, but failed to purge managed data at "
                  << PathToUtf8(paths_.root) << ": " << ec.message() << '\n';
        return 1;
    }

    std::cout << "JDKM environment hooks were removed.\n";
    std::cout << "Managed data was deleted from " << PathToUtf8(paths_.root) << '\n';
    std::cout << "Removed entries: " << static_cast<unsigned long long>(removed_count) << '\n';
    return 0;
}

std::vector<DoctorCheck> Application::BuildDoctorChecks() const {
    std::vector<DoctorCheck> checks;

    const auto add_directory_check = [&checks](const std::string& label, const fs::path& path) {
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        checks.push_back({
            exists ? "OK" : "FAIL",
            label,
            exists ? PathToUtf8(path) : "missing: " + PathToUtf8(path)
        });
    };

    add_directory_check("root directory", paths_.root);
    add_directory_check("installs directory", paths_.installs);
    add_directory_check("logs directory", paths_.logs);
    add_directory_check("state directory", paths_.state);

    std::string snapshot_error;
    const auto snapshot = snapshot_store_.Load(&snapshot_error);
    checks.push_back({
        snapshot.has_value() ? "OK" : "WARN",
        "original environment snapshot",
        snapshot.has_value()
            ? "captured at " + snapshot->created_at_utc
            : "no original environment snapshot is recorded yet"
    });

    std::string read_error;
    const auto path_value = ReadUserEnvironmentVariable("Path", &read_error);
    if (!path_value.has_value()) {
        checks.push_back({"WARN", "user PATH", "unable to read persistent user PATH: " + read_error});
    } else {
        bool all_entries_ok = true;
        for (const auto& entry : ManagedPathEntries(paths_)) {
            if (!HasPathEntry(*path_value, entry)) {
                all_entries_ok = false;
                break;
            }
        }
        checks.push_back({
            all_entries_ok ? "OK" : "WARN",
            "persistent PATH entries",
            all_entries_ok ? "runtime shims are registered in the user PATH"
                           : "run `jkm init` to register current runtime paths in the user PATH"
        });
    }

    const auto active_runtimes = active_store_.Load();
    if (active_runtimes.empty()) {
        checks.push_back({"WARN", "active runtimes", "no active runtime is recorded yet"});
    } else {
        for (const auto& runtime : active_runtimes) {
            const auto current_link = CurrentLinkPath(paths_, runtime.type);
            std::error_code ec;
            const bool root_exists = fs::exists(runtime.root, ec);
            const bool link_exists = fs::exists(current_link, ec);
            std::ostringstream detail;
            detail << "recorded=" << PathToUtf8(runtime.root) << ", link=" << PathToUtf8(current_link);
            checks.push_back({
                root_exists && link_exists ? "OK" : "WARN",
                std::string("active ") + ToString(runtime.type),
                detail.str()
            });
        }
    }

    std::string project_error;
    const auto project_store_path = DetectProjectRuntimeStorePath(&project_error);
    const auto project_runtimes = LoadProjectActiveRuntimes();
    checks.push_back({
        project_error.empty() ? "OK" : "WARN",
        "project local runtime overrides",
        project_runtimes.empty()
            ? (project_store_path.has_value()
                ? "project store exists at " + PathToUtf8(*project_store_path) + " but no local runtime override is active"
                : "no local .jkm runtime override was found from the current directory upward")
            : "local project overrides are active for " + std::to_string(project_runtimes.size()) + " runtime type(s)"
              + (project_store_path.has_value() ? " at " + PathToUtf8(*project_store_path) : std::string{})
    });

    for (const auto shell : {"powershell", "pwsh"}) {
        std::string profile_error;
        const auto profile_path = DefaultShellProfilePath(shell, &profile_error);
        if (!profile_path.has_value()) {
            checks.push_back({"WARN", std::string(shell) + " shell hook", profile_error});
            continue;
        }

        std::error_code ec;
        const bool exists = fs::exists(*profile_path, ec);
        std::string content;
        const bool readable = exists ? ReadTextFile(*profile_path, &content, &profile_error) : true;
        const bool installed = readable && exists && HookBlockInstalled(content);
        const auto status = (!project_runtimes.empty() && !installed) ? "WARN" : "OK";
        checks.push_back({
            status,
            std::string(shell) + " shell hook",
            installed
                ? "installed in " + PathToUtf8(*profile_path)
                : "not installed in " + PathToUtf8(*profile_path) + "; local overrides require `jkm exec`, `jkm env activate`, or `jkm shell install " + shell + "`"
        });
    }

    const auto effective_runtimes = EffectiveActiveRuntimeMap(active_store_);
    const auto dependency_checks = BuildRuntimeDependencyChecks(effective_runtimes);
    checks.insert(checks.end(), dependency_checks.begin(), dependency_checks.end());

    return checks;
}

int Application::HandleDoctor(const std::string& operation_id) {
    const auto checks = BuildDoctorChecks();

    for (const auto& check : checks) {
        std::cout << "[" << check.status << "] " << check.label << ": " << check.detail << '\n';
    }

    logger_.Info(operation_id, "doctor completed", {{"checks", std::to_string(checks.size())}});

    const auto failed = std::count_if(checks.begin(), checks.end(), [](const DoctorCheck& check) {
        return check.status == "FAIL";
    });
    return failed == 0 ? 0 : 1;
}

int Application::HandleStatus(const std::vector<std::string>& args, const std::string& operation_id) {
    std::optional<RuntimeType> filter;
    if (args.size() > 1) {
        filter = ParseRuntimeType(args[1]);
        if (!filter.has_value()) {
            std::cerr << "Unknown runtime type: " << args[1] << '\n';
            return 1;
        }
    }

    std::string cwd_error;
    const auto cwd = CurrentWorkingDirectory(&cwd_error);
    const auto project_store_path = DetectProjectRuntimeStorePath(&cwd_error);
    const auto project_runtimes = LoadProjectActiveRuntimes();
    const auto global_runtimes = active_store_.Load();

    std::map<RuntimeType, ActiveRuntime> global_map;
    std::map<RuntimeType, ActiveRuntime> local_map;
    for (const auto& runtime : global_runtimes) {
        global_map[runtime.type] = runtime;
    }
    for (const auto& runtime : project_runtimes) {
        local_map[runtime.type] = runtime;
    }

    if (cwd.has_value()) {
        std::cout << "cwd:          " << PathToUtf8(*cwd) << '\n';
    }
    std::cout << "project store: "
              << (project_store_path.has_value() ? PathToUtf8(*project_store_path) : std::string("none"))
              << '\n';

    bool printed_any = false;
    for (const auto type : AllRuntimeTypes()) {
        if (filter.has_value() && *filter != type) {
            continue;
        }

        const auto global_it = global_map.find(type);
        const auto local_it = local_map.find(type);
        const auto effective = EffectiveActiveRuntime(active_store_, type);

        if (!effective.has_value() && global_it == global_map.end() && local_it == local_map.end()) {
            continue;
        }

        printed_any = true;
        std::cout << ToString(type) << '\n';

        if (effective.has_value()) {
            std::cout << "  effective: " << effective->selected_name
                      << " [" << effective->distribution << "] "
                      << "(scope=" << effective->scope << ")\n";
            std::cout << "  path:      " << PathToUtf8(effective->root) << '\n';
        } else {
            std::cout << "  effective: none\n";
        }

        if (local_it != local_map.end()) {
            std::cout << "  local:     " << local_it->second.selected_name
                      << " [" << local_it->second.distribution << "]\n";
        } else {
            std::cout << "  local:     none\n";
        }

        if (global_it != global_map.end()) {
            std::cout << "  global:    " << global_it->second.selected_name
                      << " [" << global_it->second.distribution << "]\n";
        } else {
            std::cout << "  global:    none\n";
        }
    }

    if (!printed_any) {
        std::cout << "No local or global active runtimes are recorded for the current view.\n";
    }

    logger_.Info(operation_id, "displayed runtime status",
                 {{"filter", filter.has_value() ? ToString(*filter) : "all"},
                  {"projectStore", project_store_path.has_value() ? PathToUtf8(*project_store_path) : "none"}});
    return 0;
}

int Application::HandleList(const std::vector<std::string>& args, const std::string& operation_id) {
    std::optional<RuntimeType> filter;
    if (args.size() > 1) {
        filter = ParseRuntimeType(args[1]);
        if (!filter.has_value()) {
            std::cerr << "Unknown runtime type: " << args[1] << '\n';
            return 1;
        }
    }

    const auto installed = LoadKnownRuntimes(filter);
    if (installed.empty()) {
        std::cout << "No installed runtimes were found under " << PathToUtf8(paths_.installs) << '\n';
        return 0;
    }

    for (const auto& runtime : installed) {
        PrintInstalledRuntime(runtime);
    }

    logger_.Info(operation_id, "listed installed runtimes", {{"count", std::to_string(installed.size())}});
    return 0;
}

int Application::HandleCurrent(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() > 1) {
        const auto runtime_type = ParseRuntimeType(args[1]);
        if (!runtime_type.has_value()) {
            std::cerr << "Unknown runtime type: " << args[1] << '\n';
            return 1;
        }

        const auto runtime = EffectiveActiveRuntime(active_store_, *runtime_type);
        if (!runtime.has_value()) {
            std::cout << "No active " << ToString(*runtime_type) << " runtime is recorded.\n";
            return 0;
        }

        PrintActiveRuntime(*runtime);
        logger_.Info(operation_id, "displayed active runtime", {{"type", ToString(*runtime_type)}});
        return 0;
    }

    const auto project_runtimes = LoadProjectActiveRuntimes();
    const auto global_runtimes = active_store_.Load();

    std::map<RuntimeType, ActiveRuntime> effective_runtimes;
    for (const auto& runtime : global_runtimes) {
        effective_runtimes[runtime.type] = runtime;
    }
    for (const auto& runtime : project_runtimes) {
        effective_runtimes[runtime.type] = runtime;
    }

    if (effective_runtimes.empty()) {
        std::cout << "No active runtimes are recorded.\n";
        return 0;
    }

    for (const auto& [type, runtime] : effective_runtimes) {
        (void)type;
        PrintActiveRuntime(runtime);
    }

    logger_.Info(operation_id, "displayed all active runtimes", {{"count", std::to_string(effective_runtimes.size())}});
    return 0;
}

int Application::HandleUse(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 3) {
        std::cerr << "Usage: jkm use <java|python|node|go|maven|gradle> <selector> [--scope user]\n";
        return 1;
    }

    const auto runtime_type = ParseRuntimeType(args[1]);
    if (!runtime_type.has_value()) {
        std::cerr << "Unknown runtime type: " << args[1] << '\n';
        return 1;
    }

    const auto scope = ScopeFromArgs(args);
    if (scope != "user" && scope != "local") {
        std::cerr << "Only --scope user and --scope local are supported right now.\n";
        return 1;
    }

    if (scope == "user" && !EnsureEnvironmentSnapshotCaptured(operation_id)) {
        return 1;
    }

    const auto installed = LoadKnownRuntimes(*runtime_type);
    const auto matches = FindInstalledRuntimeMatches(installed, *runtime_type, args[2]);

    if (matches.empty()) {
        std::cerr << "No installed runtime matched selector `" << args[2] << "`.\n";
        return 1;
    }

    if (matches.size() > 1) {
        std::cerr << "Selector `" << args[2] << "` is ambiguous. Candidates:\n";
        for (const auto& match : matches) {
            PrintInstalledRuntime(match);
        }
        return 1;
    }

    const auto& selected = matches.front();
    const ActiveRuntime active_runtime{
        selected.type,
        PreferredInstalledRuntimeSelector(selected),
        selected.distribution,
        selected.root,
        scope,
        NowUtcIso8601()
    };

    std::string error;
    if (scope == "local") {
        fs::path store_path;
        if (!SaveProjectActiveRuntime(active_runtime, &store_path, &error)) {
            logger_.Error(operation_id, "failed to persist local active runtime", {{"error", error}});
            std::cerr << "Failed to persist local runtime state: " << error << '\n';
            return 1;
        }

        logger_.Info(operation_id, "runtime switched locally",
                     {{"type", ToString(selected.type)},
                      {"selector", args[2]},
                      {"path", PathToUtf8(selected.root)},
                      {"distribution", selected.distribution},
                      {"store", PathToUtf8(store_path)}});

        std::cout << "Switched " << ToString(selected.type) << " locally to " << selected.name << '\n';
        std::cout << "Project store: " << PathToUtf8(store_path) << '\n';
        std::cout << "Target path:   " << PathToUtf8(selected.root) << '\n';
        std::cout << "Note: local scope affects JKM project resolution, not the global user PATH.\n";
        for (const auto& note : BuildUseDependencyNotes(RuntimeContextAfterUse(active_store_, active_runtime), active_runtime)) {
            std::cout << note << '\n';
        }
        return 0;
    }

    const auto current_link = CurrentLinkPath(paths_, selected.type);
    const auto snapshot = selected.is_external ? LoadEnvironmentSnapshot() : std::optional<EnvironmentSnapshot>{};
    if (selected.is_external && !snapshot.has_value()) {
        std::cerr << "The original runtime snapshot is missing, so `original` cannot be restored safely.\n";
        return 1;
    }

    if (!RepointDirectoryJunction(current_link, selected.root, &error)) {
        logger_.Error(operation_id, "failed to repoint current runtime link", {{"error", error}});
        std::cerr << "Failed to update current runtime link: " << error << '\n';
        return 1;
    }

    switch (selected.type) {
        case RuntimeType::Java: {
            const auto java_home_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("JAVA_HOME", snapshot->java_home, &error)
                : SetUserEnvironmentVariable("JAVA_HOME", PathToUtf8(selected.root), &error);
            if (!java_home_ok ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_JAVA", PreferredInstalledRuntimeSelector(selected), &error)) {
                logger_.Error(operation_id, "failed to update JAVA_HOME", {{"error", error}});
                std::cerr << "Failed to update Java environment variables: " << error << '\n';
                return 1;
            }
            break;
        }
        case RuntimeType::Python: {
            const auto python_home_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("PYTHON_HOME", snapshot->python_home, &error)
                : SetUserEnvironmentVariable("PYTHON_HOME", PathToUtf8(selected.root), &error);
            const auto conda_prefix_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("CONDA_PREFIX", snapshot->conda_prefix, &error)
                : SetUserEnvironmentVariable("CONDA_PREFIX", PathToUtf8(selected.root), &error);
            if (!python_home_ok ||
                !conda_prefix_ok ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_PYTHON", PreferredInstalledRuntimeSelector(selected), &error) ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_PYTHON_BASE", selected.is_external ? "original" : selected.base_name, &error) ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_PYTHON_ENV", selected.is_external ? "original" : (selected.is_environment ? selected.name : "base"), &error)) {
                logger_.Error(operation_id, "failed to update Python environment variables", {{"error", error}});
                std::cerr << "Failed to update Python environment variables: " << error << '\n';
                return 1;
            }
            break;
        }
        case RuntimeType::Node: {
            const auto node_home_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("NODE_HOME", snapshot->node_home, &error)
                : SetUserEnvironmentVariable("NODE_HOME", PathToUtf8(selected.root), &error);
            if (!node_home_ok ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_NODE", PreferredInstalledRuntimeSelector(selected), &error)) {
                logger_.Error(operation_id, "failed to update Node.js environment variables", {{"error", error}});
                std::cerr << "Failed to update Node.js environment variables: " << error << '\n';
                return 1;
            }
            break;
        }
        case RuntimeType::Go: {
            const auto go_root_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("GOROOT", snapshot->go_root, &error)
                : SetUserEnvironmentVariable("GOROOT", PathToUtf8(selected.root), &error);
            if (!go_root_ok ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_GO", PreferredInstalledRuntimeSelector(selected), &error)) {
                logger_.Error(operation_id, "failed to update Go environment variables", {{"error", error}});
                std::cerr << "Failed to update Go environment variables: " << error << '\n';
                return 1;
            }
            break;
        }
        case RuntimeType::Maven: {
            const auto maven_home_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("MAVEN_HOME", snapshot->maven_home, &error)
                : SetUserEnvironmentVariable("MAVEN_HOME", PathToUtf8(selected.root), &error);
            const auto m2_home_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("M2_HOME", snapshot->m2_home, &error)
                : SetUserEnvironmentVariable("M2_HOME", PathToUtf8(selected.root), &error);
            if (!maven_home_ok ||
                !m2_home_ok ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_MAVEN", PreferredInstalledRuntimeSelector(selected), &error)) {
                logger_.Error(operation_id, "failed to update Maven environment variables", {{"error", error}});
                std::cerr << "Failed to update Maven environment variables: " << error << '\n';
                return 1;
            }
            break;
        }
        case RuntimeType::Gradle: {
            const auto gradle_home_ok = selected.is_external
                ? RestoreUserEnvironmentVariable("GRADLE_HOME", snapshot->gradle_home, &error)
                : SetUserEnvironmentVariable("GRADLE_HOME", PathToUtf8(selected.root), &error);
            if (!gradle_home_ok ||
                !SetUserEnvironmentVariable("JDKM_CURRENT_GRADLE", PreferredInstalledRuntimeSelector(selected), &error)) {
                logger_.Error(operation_id, "failed to update Gradle environment variables", {{"error", error}});
                std::cerr << "Failed to update Gradle environment variables: " << error << '\n';
                return 1;
            }
            break;
        }
    }

    if (!BroadcastEnvironmentChanged(&error)) {
        logger_.Warning(operation_id, "environment change broadcast failed", {{"error", error}});
    }

    if (!active_store_.Upsert(active_runtime, &error)) {
        logger_.Error(operation_id, "failed to persist active runtime", {{"error", error}});
        std::cerr << "Runtime switched, but failed to persist active state: " << error << '\n';
        return 1;
    }

    logger_.Info(operation_id, "runtime switched",
                 {{"type", ToString(selected.type)},
                  {"selector", args[2]},
                  {"path", PathToUtf8(selected.root)},
                  {"distribution", selected.distribution}});

    std::cout << "Switched " << ToString(selected.type) << " to " << selected.name << '\n';
    std::cout << "Current link: " << PathToUtf8(current_link) << '\n';
    std::cout << "Target path:  " << PathToUtf8(selected.root) << '\n';
    std::cout << "Open a new terminal to refresh persistent environment variables in the shell session.\n";
    for (const auto& note : BuildUseDependencyNotes(RuntimeContextAfterUse(active_store_, active_runtime), active_runtime)) {
        std::cout << note << '\n';
    }
    return 0;
}

int Application::HandleUnuse(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 2) {
        std::cerr << "Usage: jkm unuse <java|python|node|go|maven|gradle> [--scope user|local] [--local]\n";
        return 1;
    }

    const auto runtime_type = ParseRuntimeType(args[1]);
    if (!runtime_type.has_value()) {
        std::cerr << "Unknown runtime type: " << args[1] << '\n';
        return 1;
    }

    const auto scope = ScopeFromArgs(args);
    if (scope != "user" && scope != "local") {
        std::cerr << "Only --scope user and --scope local are supported right now.\n";
        return 1;
    }

    std::string error;
    if (scope == "local") {
        const auto local_runtime = LoadProjectActiveRuntime(*runtime_type);
        if (!local_runtime.has_value()) {
            std::cout << "No local " << ToString(*runtime_type) << " override is recorded for this project tree.\n";
            return 0;
        }

        fs::path store_path;
        if (!DeleteProjectActiveRuntime(*runtime_type, &store_path, &error)) {
            logger_.Error(operation_id, "failed to remove local runtime override",
                          {{"type", ToString(*runtime_type)}, {"error", error}});
            std::cerr << "Failed to remove local runtime override: " << error << '\n';
            return 1;
        }

        const auto fallback = active_store_.Get(*runtime_type);
        logger_.Info(operation_id, "removed local runtime override",
                     {{"type", ToString(*runtime_type)},
                      {"selector", local_runtime->selected_name},
                      {"store", store_path.empty() ? "none" : PathToUtf8(store_path)}});

        std::cout << "Removed local " << ToString(*runtime_type) << " override.\n";
        if (fallback.has_value()) {
            std::cout << "Effective selection now falls back to global: " << fallback->selected_name << '\n';
        } else {
            std::cout << "No global fallback is recorded for this runtime type.\n";
        }
        return 0;
    }

    const auto global_runtime = active_store_.Get(*runtime_type);
    if (!global_runtime.has_value()) {
        std::cout << "No global " << ToString(*runtime_type) << " runtime is recorded.\n";
        return 0;
    }

    if (!ClearUserRuntimeSelection(paths_, active_store_, snapshot_store_, *runtime_type, &error)) {
        logger_.Error(operation_id, "failed to clear global runtime selection",
                      {{"type", ToString(*runtime_type)}, {"error", error}});
        std::cerr << "Failed to clear global runtime selection: " << error << '\n';
        return 1;
    }

    if (!BroadcastEnvironmentChanged(&error)) {
        logger_.Warning(operation_id, "environment change broadcast failed", {{"error", error}});
    }

    const auto local_runtime = LoadProjectActiveRuntime(*runtime_type);
    logger_.Info(operation_id, "cleared global runtime selection",
                 {{"type", ToString(*runtime_type)}, {"selector", global_runtime->selected_name}});

    std::cout << "Cleared global " << ToString(*runtime_type) << " selection.\n";
    if (local_runtime.has_value()) {
        std::cout << "A local project override still exists: " << local_runtime->selected_name << '\n';
    } else {
        std::cout << "No local override is active for this runtime type in the current project tree.\n";
    }
    return 0;
}

int Application::HandleExec(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 2) {
        std::cerr << "Usage: jkm exec <command> [args...]\n";
        return 1;
    }

    const auto effective_runtimes = EffectiveActiveRuntimeMap(active_store_);
    std::ostringstream script;
    script << "$ErrorActionPreference = 'Stop'\n";
    script << "$ProgressPreference = 'SilentlyContinue'\n";
    script << "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)\n";
    script << BuildPowerShellActivationScript(paths_, effective_runtimes);
    script << "$command = " << QuotePowerShellString(args[1]) << "\n";
    script << "$arguments = @(";
    for (std::size_t index = 2; index < args.size(); ++index) {
        if (index > 2) {
            script << ", ";
        }
        script << QuotePowerShellString(args[index]);
    }
    script << ")\n";
    script << "& $command @arguments\n";
    script << "if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE } else { exit 0 }\n";

    ProcessResult process_result;
    std::string error;
    if (!RunPowerShellScript(script.str(), &process_result, &error)) {
        logger_.Error(operation_id, "failed to run command with effective runtime environment",
                      {{"command", args[1]}, {"error", error}});
        std::cerr << "Failed to run command: " << error << '\n';
        return 1;
    }

    if (!process_result.output.empty()) {
        std::cout << process_result.output;
        if (process_result.output.back() != '\n') {
            std::cout << '\n';
        }
    }

    logger_.Info(operation_id, "executed command with effective runtime environment",
                 {{"command", args[1]},
                  {"argc", std::to_string(args.size() - 1)},
                  {"runtimeCount", std::to_string(effective_runtimes.size())},
                  {"exitCode", std::to_string(process_result.exit_code)}});
    return process_result.exit_code;
}

int Application::HandleShell(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() >= 3 && args[1] == "hook") {
        const auto shell = ToLowerAscii(args[2]);
        if (!IsSupportedHookShell(shell)) {
            std::cerr << "Unsupported shell: " << shell << ". Supported values: powershell, pwsh\n";
            return 1;
        }

        const auto executable = CurrentExecutablePath();
        if (!executable.has_value()) {
            std::cerr << "Unable to resolve the jkm executable path for shell hook generation.\n";
            return 1;
        }

        std::cout << WrapHookBlock(BuildPowerShellHookScript(*executable));
        logger_.Info(operation_id, "generated shell hook",
                     {{"shell", shell}, {"path", PathToUtf8(*executable)}});
        return 0;
    }

    if (args.size() >= 3 && args[1] == "install") {
        const auto shell = ToLowerAscii(args[2]);
        if (!IsSupportedHookShell(shell)) {
            std::cerr << "Unsupported shell: " << shell << ". Supported values: powershell, pwsh\n";
            return 1;
        }

        const auto executable = CurrentExecutablePath();
        if (!executable.has_value()) {
            std::cerr << "Unable to resolve the jkm executable path for shell hook installation.\n";
            return 1;
        }

        std::string error;
        const auto profile_path = ResolveShellProfilePath(args, 2, &error);
        if (!profile_path.has_value()) {
            std::cerr << "Failed to resolve shell profile path: " << error << '\n';
            return 1;
        }

        std::string existing_content;
        std::error_code ec;
        if (fs::exists(*profile_path, ec) && !ReadTextFile(*profile_path, &existing_content, &error)) {
            std::cerr << "Failed to read shell profile: " << error << '\n';
            return 1;
        }

        const auto updated_content = UpsertHookBlock(existing_content, BuildPowerShellHookScript(*executable));
        if (!WriteTextFile(*profile_path, updated_content, &error)) {
            std::cerr << "Failed to write shell profile: " << error << '\n';
            return 1;
        }

        logger_.Info(operation_id, "installed shell hook",
                     {{"shell", shell}, {"profile", PathToUtf8(*profile_path)}});
        std::cout << "Installed " << shell << " shell hook in " << PathToUtf8(*profile_path) << '\n';
        return 0;
    }

    if (args.size() >= 3 && args[1] == "uninstall") {
        const auto shell = ToLowerAscii(args[2]);
        if (!IsSupportedHookShell(shell)) {
            std::cerr << "Unsupported shell: " << shell << ". Supported values: powershell, pwsh\n";
            return 1;
        }

        std::string error;
        const auto profile_path = ResolveShellProfilePath(args, 2, &error);
        if (!profile_path.has_value()) {
            std::cerr << "Failed to resolve shell profile path: " << error << '\n';
            return 1;
        }

        std::error_code ec;
        if (!fs::exists(*profile_path, ec)) {
            std::cout << "No " << shell << " profile exists at " << PathToUtf8(*profile_path) << '\n';
            return 0;
        }

        std::string existing_content;
        if (!ReadTextFile(*profile_path, &existing_content, &error)) {
            std::cerr << "Failed to read shell profile: " << error << '\n';
            return 1;
        }

        if (!HookBlockInstalled(existing_content)) {
            std::cout << "No JKM shell hook is installed in " << PathToUtf8(*profile_path) << '\n';
            return 0;
        }

        const auto updated_content = RemoveHookBlock(existing_content);
        if (updated_content.empty()) {
            fs::remove(*profile_path, ec);
            if (ec) {
                std::cerr << "Failed to remove empty shell profile: " << ec.message() << '\n';
                return 1;
            }
        } else if (!WriteTextFile(*profile_path, updated_content, &error)) {
            std::cerr << "Failed to update shell profile: " << error << '\n';
            return 1;
        }

        logger_.Info(operation_id, "uninstalled shell hook",
                     {{"shell", shell}, {"profile", PathToUtf8(*profile_path)}});
        std::cout << "Removed " << shell << " shell hook from " << PathToUtf8(*profile_path) << '\n';
        return 0;
    }

    if (args.size() >= 2 && args[1] == "status") {
        std::vector<std::string> shells;
        const auto explicit_profile = TryOptionValue(args, "--profile");
        const bool shell_provided = args.size() >= 3 && !args[2].empty() && args[2][0] != '-';
        if (shell_provided) {
            shells.push_back(ToLowerAscii(args[2]));
        } else if (explicit_profile.has_value()) {
            shells = {"powershell"};
        } else {
            shells = {"powershell", "pwsh"};
        }

        for (const auto& shell : shells) {
            if (!IsSupportedHookShell(shell)) {
                std::cerr << "Unsupported shell: " << shell << ". Supported values: powershell, pwsh\n";
                return 1;
            }

            std::string error;
            std::optional<fs::path> profile_path;
            if (explicit_profile.has_value()) {
                profile_path = PathFromUtf8(*explicit_profile);
            } else {
                profile_path = DefaultShellProfilePath(shell, &error);
            }
            if (!profile_path.has_value()) {
                std::cerr << "Failed to resolve shell profile path: " << error << '\n';
                return 1;
            }

            std::error_code ec;
            const bool exists = fs::exists(*profile_path, ec);
            std::string content;
            const bool installed = exists && ReadTextFile(*profile_path, &content, &error) && HookBlockInstalled(content);

            std::cout << shell
                      << " | profile=" << PathToUtf8(*profile_path)
                      << " | exists=" << (exists ? "true" : "false")
                      << " | hook=" << (installed ? "installed" : "missing") << '\n';
        }

        logger_.Info(operation_id, "displayed shell hook status");
        return 0;
    }

    std::cerr << "Usage: jkm shell hook <powershell|pwsh>\n";
    std::cerr << "   or: jkm shell install <powershell|pwsh> [--profile <path>]\n";
    std::cerr << "   or: jkm shell uninstall <powershell|pwsh> [--profile <path>]\n";
    std::cerr << "   or: jkm shell status [powershell|pwsh] [--profile <path>]\n";
    return 1;
}

int Application::HandleLogs(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[1] == "path") {
        std::cout << "logs:     " << PathToUtf8(paths_.logs) << '\n';
        std::cout << "settings: " << PathToUtf8(settings_store_.FilePath()) << '\n';
        std::cout << "audit:    " << PathToUtf8(audit_store_.FilePath()) << '\n';
        std::cout << "active:   " << PathToUtf8(active_store_.FilePath()) << '\n';
        std::cout << "snapshot: " << PathToUtf8(snapshot_store_.FilePath()) << '\n';
        return 0;
    }

    if (args.size() >= 2 && args[1] == "recent") {
        const auto limit_raw = OptionValue(args, "--limit", "20");
        int limit = 20;
        try {
            limit = std::stoi(limit_raw);
        } catch (...) {
            std::cerr << "Invalid --limit value: " << limit_raw << '\n';
            return 1;
        }
        if (limit <= 0) {
            std::cerr << "--limit must be greater than 0.\n";
            return 1;
        }

        const auto command_filter = TryOptionValue(args, "--command");
        const auto runtime_filter = TryOptionValue(args, "--runtime");
        const auto status_filter = TryOptionValue(args, "--status");

        if (runtime_filter.has_value() && !runtime_filter->empty() && !ParseRuntimeType(*runtime_filter).has_value()) {
            std::cerr << "Unknown runtime type for --runtime: " << *runtime_filter << '\n';
            return 1;
        }

        std::string error;
        const auto records = audit_store_.ListRecent(
            static_cast<std::size_t>(limit),
            command_filter.has_value() ? std::optional<std::string>(ToLowerAscii(*command_filter)) : std::nullopt,
            runtime_filter.has_value() ? std::optional<std::string>(ToLowerAscii(*runtime_filter)) : std::nullopt,
            status_filter.has_value() ? std::optional<std::string>(ToLowerAscii(*status_filter)) : std::nullopt,
            &error);
        if (!error.empty()) {
            std::cerr << "Failed to read audit records: " << error << '\n';
            return 1;
        }

        if (records.empty()) {
            std::cout << "No audit records matched the current filters.\n";
            return 0;
        }

        for (const auto& record : records) {
            std::cout << record.started_at_utc
                      << " | " << record.status
                      << " | " << record.command_name;
            if (!record.runtime_type.empty()) {
                std::cout << " | runtime=" << record.runtime_type;
            }
            if (!record.selector.empty()) {
                std::cout << " | selector=" << record.selector;
            }
            std::cout << " | exit=" << record.exit_code
                      << " | op=" << record.operation_id << '\n';
            if (!record.note.empty()) {
                std::cout << "  note: " << record.note << '\n';
            }
        }
        return 0;
    }

    if (args.size() >= 4 && args[1] == "show" && args[2] == "--operation") {
        const auto record = audit_store_.FindByOperationId(args[3]);
        if (!record.has_value()) {
            std::cerr << "No audit record was found for operation `" << args[3] << "`.\n";
            return 1;
        }

        std::cout << "operationId: " << record->operation_id << '\n';
        std::cout << "command:     " << record->command_line << '\n';
        std::cout << "commandName: " << record->command_name << '\n';
        std::cout << "developer:   " << record->developer << '\n';
        if (!record->runtime_type.empty()) {
            std::cout << "runtimeType: " << record->runtime_type << '\n';
        }
        if (!record->selector.empty()) {
            std::cout << "selector:    " << record->selector << '\n';
        }
        std::cout << "status:      " << record->status << '\n';
        std::cout << "exitCode:    " << record->exit_code << '\n';
        std::cout << "startedAt:   " << record->started_at_utc << '\n';
        std::cout << "endedAt:     " << record->ended_at_utc << '\n';
        std::cout << "note:        " << record->note << '\n';
        return 0;
    }

    std::cerr << "Usage: jkm logs path\n";
    std::cerr << "   or: jkm logs recent [--limit 20] [--command <name>] [--runtime <type>] [--status <succeeded|failed>]\n";
    std::cerr << "   or: jkm logs show --operation <id>\n";
    return 1;
}

int Application::HandleConfig(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() >= 2 && args[1] == "path") {
        std::cout << PathToUtf8(settings_store_.FilePath()) << '\n';
        return 0;
    }

    if (args.size() >= 2 && args[1] == "list") {
        std::string error;
        const auto settings = settings_store_.Load(&error);
        if (!error.empty()) {
            std::cerr << "Failed to read settings: " << error << '\n';
            return 1;
        }

        if (settings.empty()) {
            std::cout << "No persisted configuration values are recorded.\n";
            return 0;
        }

        auto remaining = settings;
        for (const auto& definition : kConfigKeyDefinitions) {
            const auto iterator = remaining.find(definition.canonical_key);
            if (iterator == remaining.end()) {
                continue;
            }

            std::cout << definition.canonical_key << " = " << iterator->second << '\n';
            remaining.erase(iterator);
        }

        for (const auto& [key, value] : remaining) {
            std::cout << key << " = " << value << '\n';
        }

        logger_.Info(operation_id, "listed persisted configuration", {{"count", std::to_string(settings.size())}});
        return 0;
    }

    if (args.size() >= 3 && args[1] == "get") {
        const auto* definition = FindConfigKeyDefinition(args[2]);
        if (definition == nullptr) {
            std::cerr << "Unknown configuration key: " << args[2] << '\n';
            return 1;
        }

        std::string error;
        const auto value = settings_store_.Get(definition->canonical_key, &error);
        if (!error.empty()) {
            std::cerr << "Failed to read settings: " << error << '\n';
            return 1;
        }

        if (!value.has_value()) {
            std::cerr << "No persisted value is recorded for `" << definition->canonical_key << "`.\n";
            return 1;
        }

        std::cout << definition->canonical_key << " = " << *value << '\n';
        return 0;
    }

    if (args.size() >= 4 && args[1] == "set") {
        const auto* definition = FindConfigKeyDefinition(args[2]);
        if (definition == nullptr) {
            std::cerr << "Unknown configuration key: " << args[2] << '\n';
            return 1;
        }

        auto value = TrimWhitespace(args[3]);
        if (value.empty()) {
            std::cerr << "Configuration values cannot be empty.\n";
            return 1;
        }

        if (definition->is_path) {
            std::string cwd_error;
            const auto cwd = CurrentWorkingDirectory(&cwd_error);
            if (!cwd.has_value()) {
                std::cerr << "Failed to resolve current directory: " << cwd_error << '\n';
                return 1;
            }

            auto path = PathFromUtf8(value);
            if (path.is_relative()) {
                path = (*cwd / path).lexically_normal();
            }

            std::error_code ec;
            if (!fs::exists(path, ec)) {
                std::cerr << "Certificate path does not exist: " << PathToUtf8(path) << '\n';
                return 1;
            }

            value = PathToUtf8(path);
        }

        if (std::string(definition->canonical_key).rfind("mirror.", 0) == 0) {
            while (value.size() > 1 && value.back() == '/') {
                value.pop_back();
            }
        }

        std::string error;
        if (!settings_store_.Upsert(definition->canonical_key, value, &error)) {
            std::cerr << "Failed to persist configuration: " << error << '\n';
            return 1;
        }

        logger_.Info(operation_id, "saved configuration value",
                     {{"key", definition->canonical_key}, {"value", value}});
        std::cout << "Saved " << definition->canonical_key << " = " << value << '\n';
        return 0;
    }

    if (args.size() >= 3 && args[1] == "unset") {
        const auto* definition = FindConfigKeyDefinition(args[2]);
        if (definition == nullptr) {
            std::cerr << "Unknown configuration key: " << args[2] << '\n';
            return 1;
        }

        std::string error;
        if (!settings_store_.Remove(definition->canonical_key, &error)) {
            std::cerr << "Failed to remove configuration: " << error << '\n';
            return 1;
        }

        logger_.Info(operation_id, "removed configuration value", {{"key", definition->canonical_key}});
        std::cout << "Removed " << definition->canonical_key << '\n';
        return 0;
    }

    std::cerr << "Usage: jkm config path\n";
    std::cerr << "   or: jkm config list\n";
    std::cerr << "   or: jkm config get <key>\n";
    std::cerr << "   or: jkm config set <key> <value>\n";
    std::cerr << "   or: jkm config unset <key>\n";
    return 1;
}

int Application::HandleCache(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 2) {
        std::cerr << "Usage: jkm cache list [downloads|temp|all]\n";
        std::cerr << "   or: jkm cache clear [downloads|temp|all]\n";
        std::cerr << "   or: jkm cache prune [downloads|temp|all] [--max-size-mb <n>] [--max-age-days <n>] [--dry-run]\n";
        return 1;
    }

    const auto subcommand = ToLowerAscii(args[1]);
    const auto target = args.size() >= 3 && !args[2].empty() && args[2][0] != '-' ? ToLowerAscii(args[2]) : "all";
    const auto roots = CacheRoots(paths_, target);
    if (roots.empty()) {
        std::cerr << "Unknown cache target: " << target << ". Supported values: downloads, temp, all\n";
        return 1;
    }

    if (subcommand == "list") {
        const auto entries = CollectCacheFileEntries(paths_, target);
        if (entries.empty()) {
            std::cout << "No cached files were found in " << target << ".\n";
            return 0;
        }

        for (const auto& entry : entries) {
            std::cout << entry.area
                      << " | " << FormatByteSize(entry.size_bytes)
                      << " | " << PathToUtf8(entry.path) << '\n';
        }

        std::cout << "Total: " << entries.size() << " file(s), " << FormatByteSize(TotalCacheSize(entries)) << '\n';
        logger_.Info(operation_id, "listed cache entries",
                     {{"target", target}, {"count", std::to_string(entries.size())}});
        return 0;
    }

    if (subcommand == "clear") {
        const auto entries = CollectCacheFileEntries(paths_, target);
        for (const auto& [area, root] : roots) {
            (void)area;
            std::error_code ec;
            if (!fs::exists(root, ec)) {
                continue;
            }

            for (fs::directory_iterator iterator(root, ec), end; iterator != end; iterator.increment(ec)) {
                if (ec) {
                    std::cerr << "Failed to enumerate cache path " << PathToUtf8(root) << ": " << ec.message() << '\n';
                    return 1;
                }

                std::error_code remove_ec;
                fs::remove_all(iterator->path(), remove_ec);
                if (remove_ec) {
                    std::cerr << "Failed to remove cache entry " << PathToUtf8(iterator->path()) << ": " << remove_ec.message() << '\n';
                    return 1;
                }
            }

            fs::create_directories(root, ec);
            if (ec) {
                std::cerr << "Failed to recreate cache directory " << PathToUtf8(root) << ": " << ec.message() << '\n';
                return 1;
            }
        }

        std::cout << "Removed " << entries.size() << " cached file(s), freeing "
                  << FormatByteSize(TotalCacheSize(entries)) << ".\n";
        logger_.Info(operation_id, "cleared cache",
                     {{"target", target}, {"count", std::to_string(entries.size())}});
        return 0;
    }

    if (subcommand == "prune") {
        const auto max_size_raw = TryOptionValue(args, "--max-size-mb");
        const auto max_age_raw = TryOptionValue(args, "--max-age-days");
        const bool dry_run = HasFlag(args, "--dry-run");

        std::optional<std::uintmax_t> max_size_bytes;
        std::optional<int> max_age_days;

        if (max_size_raw.has_value()) {
            try {
                const auto size_mb = std::stoll(*max_size_raw);
                if (size_mb <= 0) {
                    throw std::invalid_argument("non-positive");
                }
                max_size_bytes = static_cast<std::uintmax_t>(size_mb) * 1024ull * 1024ull;
            } catch (...) {
                std::cerr << "Invalid --max-size-mb value: " << *max_size_raw << '\n';
                return 1;
            }
        }

        if (max_age_raw.has_value()) {
            try {
                const auto age_days = std::stoi(*max_age_raw);
                if (age_days <= 0) {
                    throw std::invalid_argument("non-positive");
                }
                max_age_days = age_days;
            } catch (...) {
                std::cerr << "Invalid --max-age-days value: " << *max_age_raw << '\n';
                return 1;
            }
        }

        if (!max_size_bytes.has_value() && !max_age_days.has_value()) {
            std::cerr << "Prune requires --max-size-mb, --max-age-days, or both.\n";
            return 1;
        }

        const auto entries = CollectCacheFileEntries(paths_, target);
        if (entries.empty()) {
            std::cout << "No cached files were found in " << target << ".\n";
            return 0;
        }

        std::vector<bool> marked(entries.size(), false);
        if (max_age_days.has_value()) {
            const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24LL * static_cast<long long>(*max_age_days));
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (entries[index].last_write_time <= cutoff) {
                    marked[index] = true;
                }
            }
        }

        std::uintmax_t remaining_size = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (!marked[index]) {
                remaining_size += entries[index].size_bytes;
            }
        }

        if (max_size_bytes.has_value() && remaining_size > *max_size_bytes) {
            std::vector<std::size_t> candidates;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (!marked[index]) {
                    candidates.push_back(index);
                }
            }

            std::sort(candidates.begin(), candidates.end(), [&entries](std::size_t left, std::size_t right) {
                return entries[left].last_write_time < entries[right].last_write_time;
            });

            for (const auto index : candidates) {
                if (remaining_size <= *max_size_bytes) {
                    break;
                }
                marked[index] = true;
                remaining_size -= entries[index].size_bytes;
            }
        }

        std::vector<CacheFileEntry> removed_entries;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (marked[index]) {
                removed_entries.push_back(entries[index]);
            }
        }

        if (removed_entries.empty()) {
            std::cout << "No cache entries matched the current prune policy.\n";
            return 0;
        }

        const auto removed_size = TotalCacheSize(removed_entries);
        if (dry_run) {
            for (const auto& entry : removed_entries) {
                std::cout << "Would remove " << entry.area
                          << " | " << FormatByteSize(entry.size_bytes)
                          << " | " << PathToUtf8(entry.path) << '\n';
            }
            std::cout << "Dry run: " << removed_entries.size() << " file(s), "
                      << FormatByteSize(removed_size) << " would be removed.\n";
            return 0;
        }

        for (const auto& entry : removed_entries) {
            std::error_code ec;
            fs::remove(entry.path, ec);
            if (ec) {
                std::cerr << "Failed to remove cache entry " << PathToUtf8(entry.path) << ": " << ec.message() << '\n';
                return 1;
            }
        }

        for (const auto& [area, root] : roots) {
            (void)area;
            RemoveEmptyDirectoriesUnder(root);
        }

        const auto remaining_entries = CollectCacheFileEntries(paths_, target);
        std::cout << "Pruned " << removed_entries.size() << " file(s), freeing "
                  << FormatByteSize(removed_size) << ". Remaining cache size: "
                  << FormatByteSize(TotalCacheSize(remaining_entries)) << ".\n";
        logger_.Info(operation_id, "pruned cache",
                     {{"target", target}, {"removedCount", std::to_string(removed_entries.size())}});
        return 0;
    }

    std::cerr << "Unknown cache subcommand: " << args[1] << '\n';
    std::cerr << "Usage: jkm cache list [downloads|temp|all]\n";
    std::cerr << "   or: jkm cache clear [downloads|temp|all]\n";
    std::cerr << "   or: jkm cache prune [downloads|temp|all] [--max-size-mb <n>] [--max-age-days <n>] [--dry-run]\n";
    return 1;
}

int Application::HandleLock(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() < 2) {
        std::cerr << "Usage: jkm lock path [--lock-file <path>]\n";
        std::cerr << "   or: jkm lock show [--lock-file <path>]\n";
        std::cerr << "   or: jkm lock write [--lock-file <path>]\n";
        return 1;
    }

    const auto subcommand = ToLowerAscii(args[1]);
    if (subcommand == "path") {
        std::string error;
        const auto lock_path = ResolveProjectLockFilePath(args, false, &error);
        if (!lock_path.has_value()) {
            std::cerr << "Failed to resolve lock file path: " << error << '\n';
            return 1;
        }

        std::cout << PathToUtf8(*lock_path) << '\n';
        return 0;
    }

    if (subcommand == "show") {
        std::string error;
        const auto lock_path = ResolveProjectLockFilePath(args, true, &error);
        if (!lock_path.has_value()) {
            std::cerr << error << '\n';
            return 1;
        }

        ProjectLockStore lock_store(*lock_path);
        const auto lock_file = lock_store.Load(&error);
        if (!lock_file.has_value()) {
            std::cerr << "Failed to read project lock file: " << error << '\n';
            return 1;
        }

        std::cout << "path:       " << PathToUtf8(lock_store.FilePath()) << '\n';
        std::cout << "version:    " << lock_file->format_version << '\n';
        std::cout << "createdAt:  " << lock_file->created_at_utc << '\n';
        std::cout << "entryCount: " << lock_file->entries.size() << '\n';
        for (const auto& entry : lock_file->entries) {
            std::cout << ToString(entry.type)
                      << " | " << entry.distribution
                      << " | selector=" << entry.selector;
            if (!entry.arch.empty()) {
                std::cout << " | arch=" << entry.arch;
            }
            std::cout << '\n';
        }

        logger_.Info(operation_id, "displayed project lock file",
                     {{"path", PathToUtf8(lock_store.FilePath())},
                      {"entryCount", std::to_string(lock_file->entries.size())}});
        return 0;
    }

    if (subcommand == "write") {
        std::string error;
        const auto lock_path = ResolveProjectLockFilePath(args, false, &error);
        if (!lock_path.has_value()) {
            std::cerr << "Failed to resolve lock file path: " << error << '\n';
            return 1;
        }

        const auto effective_runtimes = EffectiveActiveRuntimeMap(active_store_);
        const auto installed = LoadKnownRuntimes();
        const std::array<RuntimeType, 6> runtime_order{
            RuntimeType::Java,
            RuntimeType::Python,
            RuntimeType::Node,
            RuntimeType::Go,
            RuntimeType::Maven,
            RuntimeType::Gradle
        };

        std::vector<ProjectLockEntry> entries;
        std::vector<std::string> skipped;
        for (const auto runtime_type : runtime_order) {
            const auto iterator = effective_runtimes.find(runtime_type);
            if (iterator == effective_runtimes.end()) {
                continue;
            }

            const auto& active_runtime = iterator->second;
            if (!IsManagedRuntimeSelection(active_runtime)) {
                skipped.push_back(ToString(runtime_type));
                continue;
            }

            auto selector = active_runtime.selected_name;
            const auto matches = FindInstalledRuntimeMatches(installed, runtime_type, active_runtime.selected_name);
            if (!matches.empty()) {
                selector = PreferredInstalledRuntimeSelector(matches.front());
            }

            auto arch = std::string{"x64"};
            if (runtime_type == RuntimeType::Maven || runtime_type == RuntimeType::Gradle) {
                arch.clear();
            }

            entries.push_back(ProjectLockEntry{
                runtime_type,
                active_runtime.distribution.empty() ? DefaultDistribution(runtime_type) : active_runtime.distribution,
                selector,
                arch
            });
        }

        if (entries.empty()) {
            std::cerr << "No managed active runtimes are available to write into the project lock file.\n";
            return 1;
        }

        ProjectLockStore lock_store(*lock_path);
        ProjectLockFile lock_file;
        lock_file.created_at_utc = NowUtcIso8601();
        lock_file.entries = std::move(entries);
        if (!lock_store.Save(lock_file, &error)) {
            std::cerr << "Failed to write project lock file: " << error << '\n';
            return 1;
        }

        std::cout << "Wrote project lock file: " << PathToUtf8(lock_store.FilePath()) << '\n';
        std::cout << "Entries: " << lock_file.entries.size() << '\n';
        if (!skipped.empty()) {
            std::cout << "Skipped preserved external selections:";
            for (const auto& item : skipped) {
                std::cout << ' ' << item;
            }
            std::cout << '\n';
        }

        logger_.Info(operation_id, "wrote project lock file",
                     {{"path", PathToUtf8(lock_store.FilePath())},
                      {"entryCount", std::to_string(lock_file.entries.size())}});
        return 0;
    }

    std::cerr << "Unknown lock subcommand: " << args[1] << '\n';
    std::cerr << "Usage: jkm lock path [--lock-file <path>]\n";
    std::cerr << "   or: jkm lock show [--lock-file <path>]\n";
    std::cerr << "   or: jkm lock write [--lock-file <path>]\n";
    return 1;
}

int Application::HandleSync(const std::vector<std::string>& args, const std::string& operation_id) {
    const auto scope = HasFlag(args, "--local") ? "local" : ToLowerAscii(OptionValue(args, "--scope", "local"));
    if (scope != "user" && scope != "local") {
        std::cerr << "Only --scope user and --scope local are supported right now.\n";
        return 1;
    }

    std::string error;
    const auto lock_path = ResolveProjectLockFilePath(args, true, &error);
    if (!lock_path.has_value()) {
        std::cerr << error << '\n';
        return 1;
    }

    ProjectLockStore lock_store(*lock_path);
    const auto lock_file = lock_store.Load(&error);
    if (!lock_file.has_value()) {
        std::cerr << "Failed to read project lock file: " << error << '\n';
        return 1;
    }

    if (lock_file->entries.empty()) {
        std::cout << "The project lock file does not contain any runtime entries.\n";
        return 0;
    }

    std::optional<fs::path> original_cwd;
    bool changed_directory = false;
    if (scope == "local") {
        original_cwd = CurrentWorkingDirectory(&error);
        if (!original_cwd.has_value()) {
            std::cerr << "Failed to resolve current directory: " << error << '\n';
            return 1;
        }

        const auto project_root = lock_store.FilePath().parent_path().parent_path();
        std::error_code ec;
        fs::current_path(project_root, ec);
        if (ec) {
            std::cerr << "Failed to switch to project root " << PathToUtf8(project_root) << ": " << ec.message() << '\n';
            return 1;
        }
        changed_directory = true;
    }

    auto restore_cwd = [&]() {
        if (!changed_directory || !original_cwd.has_value()) {
            return;
        }
        std::error_code ec;
        fs::current_path(*original_cwd, ec);
    };

    auto install_runtime = [&](const ProjectLockEntry& entry, const std::string& selector) -> int {
        std::vector<std::string> install_args{
            "install",
            ToString(entry.type),
            selector,
            "--distribution",
            entry.distribution.empty() ? DefaultDistribution(entry.type) : entry.distribution
        };

        if (entry.type == RuntimeType::Java ||
            entry.type == RuntimeType::Python ||
            entry.type == RuntimeType::Node ||
            entry.type == RuntimeType::Go) {
            install_args.push_back("--arch");
            install_args.push_back(entry.arch.empty() ? "x64" : entry.arch);
        }

        return HandleInstall(install_args, operation_id);
    };

    auto use_runtime = [&](RuntimeType type, const std::string& selector) -> int {
        return HandleUse({"use", ToString(type), selector, "--scope", scope}, operation_id);
    };

    int synced_count = 0;
    int exit_code = 0;
    for (const auto& entry : lock_file->entries) {
        if (entry.type == RuntimeType::Python) {
            if (const auto env_selector = ParsePythonEnvironmentSelector(entry.selector); env_selector.has_value()) {
                const auto base_selector = InstallSelectorForLockEntry(entry);
                exit_code = install_runtime(entry, base_selector);
                if (exit_code != 0) {
                    break;
                }

                auto installed = LoadKnownRuntimes(RuntimeType::Python);
                auto environment_matches = FilterPythonEnvironmentRuntimes(
                    FindInstalledRuntimeMatches(installed, RuntimeType::Python, entry.selector));
                if (environment_matches.empty()) {
                    exit_code = HandleEnv({"env", "create", "python", env_selector->second, "--python", base_selector}, operation_id);
                    if (exit_code != 0) {
                        break;
                    }
                }

                exit_code = use_runtime(entry.type, entry.selector);
                if (exit_code != 0) {
                    break;
                }

                ++synced_count;
                continue;
            }
        }

        const auto install_selector = InstallSelectorForLockEntry(entry);
        exit_code = install_runtime(entry, install_selector);
        if (exit_code != 0) {
            break;
        }

        exit_code = use_runtime(entry.type, entry.selector);
        if (exit_code != 0) {
            break;
        }

        ++synced_count;
    }

    restore_cwd();
    if (exit_code != 0) {
        return exit_code;
    }

    std::cout << "Synchronized " << synced_count << " runtime entr"
                 "ies from " << PathToUtf8(lock_store.FilePath())
              << " using scope " << scope << ".\n";
    logger_.Info(operation_id, "synchronized project lock file",
                 {{"path", PathToUtf8(lock_store.FilePath())},
                  {"scope", scope},
                  {"entryCount", std::to_string(synced_count)}});
    return 0;
}

int Application::HandleEnv(const std::vector<std::string>& args, const std::string& operation_id) {
    if (args.size() >= 2 && args[1] == "activate") {
        const auto shell = ToLowerAscii(OptionValue(args, "--shell", "powershell"));
        const auto effective_runtimes = EffectiveActiveRuntimeMap(active_store_);
        if (shell == "powershell") {
            std::cout << BuildPowerShellActivationScript(paths_, effective_runtimes);
            logger_.Info(operation_id, "generated powershell activation script",
                         {{"runtimeCount", std::to_string(effective_runtimes.size())}});
            return 0;
        }
        if (shell == "cmd") {
            std::cout << BuildCmdActivationScript(paths_, effective_runtimes);
            logger_.Info(operation_id, "generated cmd activation script",
                         {{"runtimeCount", std::to_string(effective_runtimes.size())}});
            return 0;
        }

        std::cerr << "Unsupported shell: " << shell << ". Supported values: powershell, cmd\n";
        return 1;
    }

    if (args.size() >= 3 && args[1] == "list" && ToLowerAscii(args[2]) == "python") {
        const auto installed = LoadKnownRuntimes(RuntimeType::Python);
        auto environments = FilterPythonEnvironmentRuntimes(installed);
        const auto base_selector = TryOptionValue(args, "--python");

        if (base_selector.has_value()) {
            auto base_matches = FilterPythonBaseRuntimes(FindInstalledRuntimeMatches(installed, RuntimeType::Python, *base_selector));
            if (base_matches.empty()) {
                std::cerr << "No Python base runtime matched selector `" << *base_selector << "`.\n";
                return 1;
            }
            if (base_matches.size() > 1) {
                std::cerr << "Base selector `" << *base_selector << "` is ambiguous. Candidates:\n";
                for (const auto& match : base_matches) {
                    PrintInstalledRuntime(match);
                }
                return 1;
            }

            const auto base_root = base_matches.front().root.lexically_normal();
            environments.erase(std::remove_if(environments.begin(), environments.end(), [&base_root](const InstalledRuntime& runtime) {
                return runtime.base_root.lexically_normal() != base_root;
            }), environments.end());
        }

        if (environments.empty()) {
            std::cout << "No Python environments were found.\n";
            return 0;
        }

        for (const auto& runtime : environments) {
            PrintInstalledRuntime(runtime);
        }

        logger_.Info(operation_id, "listed python environments", {{"count", std::to_string(environments.size())}});
        return 0;
    }

    if (args.size() >= 4 && args[1] == "create" && ToLowerAscii(args[2]) == "python") {
        const auto env_name = args[3];
        if (!IsValidPythonEnvironmentName(env_name)) {
            std::cerr << "Invalid Python environment name. Use letters, digits, '.', '-' or '_' and avoid `base`.\n";
            return 1;
        }

        const auto installed = LoadKnownRuntimes(RuntimeType::Python);
        auto base_candidates = FilterPythonBaseRuntimes(installed);
        const auto base_selector = TryOptionValue(args, "--python");

        if (base_selector.has_value()) {
            base_candidates = FilterPythonBaseRuntimes(FindInstalledRuntimeMatches(installed, RuntimeType::Python, *base_selector));
            if (base_candidates.empty()) {
                std::cerr << "No Python base runtime matched selector `" << *base_selector << "`.\n";
                return 1;
            }
            if (base_candidates.size() > 1) {
                std::cerr << "Base selector `" << *base_selector << "` is ambiguous. Candidates:\n";
                for (const auto& match : base_candidates) {
                    PrintInstalledRuntime(match);
                }
                return 1;
            }
        } else {
            const auto active_python = active_store_.Get(RuntimeType::Python);
            if (!active_python.has_value()) {
                std::cerr << "No active Python runtime is recorded. Use `--python <base-selector>` or activate a Python base first.\n";
                return 1;
            }

            const auto desired_base_root = ResolvePythonBaseRoot(active_python->root);
            base_candidates.erase(std::remove_if(base_candidates.begin(), base_candidates.end(), [&desired_base_root](const InstalledRuntime& runtime) {
                return runtime.root.lexically_normal() != desired_base_root;
            }), base_candidates.end());
        }

        if (base_candidates.empty()) {
            std::cerr << "No Python base runtime was available to create the environment.\n";
            return 1;
        }

        base_candidates.erase(std::remove_if(base_candidates.begin(), base_candidates.end(), [](const InstalledRuntime& runtime) {
            return runtime.is_external;
        }), base_candidates.end());
        if (base_candidates.empty()) {
            std::cerr << "The preserved original Python runtime is read-only and cannot host managed environments.\n";
            return 1;
        }

        const auto& base_runtime = base_candidates.front();
        PythonEnvironmentResult environment_result;
        std::string error;
        if (!CreatePythonEnvironment(base_runtime, env_name, &environment_result, &error)) {
            logger_.Error(operation_id, "failed to create Python environment",
                          {{"base", base_runtime.name}, {"name", env_name}, {"error", error}});
            std::cerr << "Failed to create Python environment `" << env_name << "`: " << error << '\n';
            return 1;
        }

        logger_.Debug(operation_id, "Python environment create output", {{"output", environment_result.raw_output}});
        logger_.Info(operation_id, "Python environment created",
                     {{"base", base_runtime.name},
                      {"name", env_name},
                      {"path", PathToUtf8(environment_result.runtime.root)},
                      {"status", environment_result.already_created ? "already_created" : "created"}});

        if (environment_result.already_created) {
            std::cout << "Python environment already exists: " << PreferredInstalledRuntimeSelector(environment_result.runtime) << '\n';
        } else {
            std::cout << "Created Python environment: " << PreferredInstalledRuntimeSelector(environment_result.runtime) << '\n';
        }
        std::cout << "Base:       " << base_runtime.name << '\n';
        std::cout << "Path:       " << PathToUtf8(environment_result.runtime.root) << '\n';
        std::cout << "Next step:  jkm use python " << PreferredInstalledRuntimeSelector(environment_result.runtime) << '\n';
        return 0;
    }

    if (args.size() >= 4 && args[1] == "remove" && ToLowerAscii(args[2]) == "python") {
        const auto env_selector = args[3];
        const auto installed = LoadKnownRuntimes(RuntimeType::Python);
        auto matches = FilterPythonEnvironmentRuntimes(FindInstalledRuntimeMatches(installed, RuntimeType::Python, env_selector));

        matches.erase(std::remove_if(matches.begin(), matches.end(), [](const InstalledRuntime& runtime) {
            return ToLowerAscii(runtime.name) == "base";
        }), matches.end());

        const auto base_selector = TryOptionValue(args, "--python");
        if (base_selector.has_value()) {
            const auto base_matches = FilterPythonBaseRuntimes(FindInstalledRuntimeMatches(installed, RuntimeType::Python, *base_selector));
            if (base_matches.empty()) {
                std::cerr << "No Python base runtime matched selector `" << *base_selector << "`.\n";
                return 1;
            }
            if (base_matches.size() > 1) {
                std::cerr << "Base selector `" << *base_selector << "` is ambiguous. Candidates:\n";
                for (const auto& match : base_matches) {
                    PrintInstalledRuntime(match);
                }
                return 1;
            }

            const auto base_root = base_matches.front().root.lexically_normal();
            matches.erase(std::remove_if(matches.begin(), matches.end(), [&base_root](const InstalledRuntime& runtime) {
                return runtime.base_root.lexically_normal() != base_root;
            }), matches.end());
        }

        if (matches.empty()) {
            std::cerr << "No Python environment matched selector `" << env_selector << "`.\n";
            return 1;
        }

        if (matches.size() > 1) {
            std::cerr << "Environment selector `" << env_selector << "` is ambiguous. Candidates:\n";
            for (const auto& match : matches) {
                PrintInstalledRuntime(match);
            }
            return 1;
        }

        const auto& selected = matches.front();
        const auto active_runtime = active_store_.Get(RuntimeType::Python);
        if (active_runtime.has_value() && PathsOverlapForRemoval(selected.root, active_runtime->root)) {
            std::cerr << "Cannot remove the active Python environment. Switch first.\n";
            return 1;
        }

        std::error_code ec;
        const auto removed_count = fs::remove_all(selected.root, ec);
        if (ec) {
            logger_.Error(operation_id, "failed to remove Python environment",
                          {{"selector", env_selector}, {"path", PathToUtf8(selected.root)}, {"error", ec.message()}});
            std::cerr << "Failed to remove " << PathToUtf8(selected.root) << ": " << ec.message() << '\n';
            return 1;
        }

        logger_.Info(operation_id, "Python environment removed",
                     {{"selector", env_selector},
                      {"path", PathToUtf8(selected.root)},
                      {"removed_count", std::to_string(static_cast<unsigned long long>(removed_count))}});

        std::cout << "Removed Python environment: " << PreferredInstalledRuntimeSelector(selected) << '\n';
        std::cout << "Path: " << PathToUtf8(selected.root) << '\n';
        return 0;
    }

    std::cerr << "Usage: jkm env activate [--shell powershell|cmd]\n";
    std::cerr << "   or: jkm env list python [--python <base-selector>]\n";
    std::cerr << "   or: jkm env create python <env-name> [--python <base-selector>]\n";
    std::cerr << "   or: jkm env remove python <env-selector> [--python <base-selector>]\n";
    return 1;
}

int Application::HandlePlaceholder(const std::string& name, const std::string& operation_id) {
    logger_.Warning(operation_id, "command not implemented", {{"command", name}});
    std::cout << "Command `" << name << "` is planned but not implemented yet in this first C++ skeleton.\n";
    return 0;
}

void Application::PrintHelp() const {
    std::cout << kAppName << ' ' << kVersion << " (C++ runtime manager prototype)\n";
    std::cout << "Developer: " << kDeveloper << "\n\n";
    std::cout << "Commands:\n";
    std::cout << "  jkm init\n";
    std::cout << "  jkm deinit\n";
    std::cout << "  jkm uninstall [--purge-data]\n";
    std::cout << "  jkm uninstall <java|python|node|go|maven|gradle> <selector>\n";
    std::cout << "  jkm install <java|python|node|go|maven|gradle> <selector> [--distribution <name>] [--arch x64]\n";
    std::cout << "  jkm remove <java|python|node|go|maven|gradle> <selector>\n";
    std::cout << "  jkm search <java|python|node|go|maven|gradle> [selector] [--distribution <name>] [--arch x64] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm version <java|python|node|go|maven|gradle> [selector]\n";
    std::cout << "  jkm doctor\n";
    std::cout << "  jkm status [java|python|node|go|maven|gradle]\n";
    std::cout << "  jkm list [java|python|node|go|maven|gradle]\n";
    std::cout << "  jkm current [java|python|node|go|maven|gradle]\n";
    std::cout << "  jkm use <java|python|node|go|maven|gradle> <selector> [--scope user|local] [--local]\n";
    std::cout << "  jkm unuse <java|python|node|go|maven|gradle> [--scope user|local] [--local]\n";
    std::cout << "  jkm exec <command> [args...]\n";
    std::cout << "  jkm shell hook <powershell|pwsh>\n";
    std::cout << "  jkm shell install <powershell|pwsh> [--profile <path>]\n";
    std::cout << "  jkm shell uninstall <powershell|pwsh> [--profile <path>]\n";
    std::cout << "  jkm shell status [powershell|pwsh] [--profile <path>]\n";
    std::cout << "  jkm remote list java [selector] [--distribution temurin] [--arch x64] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm remote list python [selector] [--distribution cpython] [--arch x64] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm remote list node [selector] [--distribution nodejs] [--arch x64] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm remote list go [selector] [--distribution golang] [--arch x64] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm remote list maven [selector] [--distribution apache] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm remote list gradle [selector] [--distribution gradle] [--limit 10] [--latest] [--format table|json] [--no-headers] [--columns <field,field>] [--sort <field[:asc|desc]>] [--filter <field=value[,field=value]>]\n";
    std::cout << "  jkm env list python [--python <base-selector>]\n";
    std::cout << "  jkm env create python <env-name> [--python <base-selector>]\n";
    std::cout << "  jkm env remove python <env-selector> [--python <base-selector>]\n";
    std::cout << "  jkm env activate [--shell powershell|cmd]\n";
    std::cout << "  jkm config path\n";
    std::cout << "  jkm config list\n";
    std::cout << "  jkm config get <key>\n";
    std::cout << "  jkm config set <key> <value>\n";
    std::cout << "  jkm config unset <key>\n";
    std::cout << "  jkm cache list [downloads|temp|all]\n";
    std::cout << "  jkm cache clear [downloads|temp|all]\n";
    std::cout << "  jkm cache prune [downloads|temp|all] [--max-size-mb <n>] [--max-age-days <n>] [--dry-run]\n";
    std::cout << "  jkm lock path [--lock-file <path>]\n";
    std::cout << "  jkm lock show [--lock-file <path>]\n";
    std::cout << "  jkm lock write [--lock-file <path>]\n";
    std::cout << "  jkm sync [--scope local|user] [--lock-file <path>]\n";
    std::cout << "  jkm logs path\n";
    std::cout << "  jkm logs recent [--limit 20] [--command <name>] [--runtime <type>] [--status <succeeded|failed>]\n";
    std::cout << "  jkm logs show --operation <id>\n";
    std::cout << '\n';
    std::cout << "Directory layout defaults to %LOCALAPPDATA%\\JdkManagement unless JDKM_HOME is set.\n";
    std::cout << "Original external runtimes are preserved as selector `original` when detected.\n";
    std::cout << "Local project scope is stored in .jkm\\local_runtimes.tsv and overrides global JKM resolution in that directory tree.\n";
    std::cout << "Project lock files default to .jkm\\project.lock.tsv and can be replayed with `jkm sync`.\n";
    std::cout << "Persisted mirror, proxy, and certificate settings are managed with `jkm config` and stored under state\\settings.tsv.\n";
    std::cout << "Remote query fields accept camelCase, snake_case, or kebab-case spellings such as `packageName`, `package_name`, or `package-name`.\n";
    std::cout << "Use `jkm shell install powershell` or `jkm shell install pwsh` to enable automatic local activation on prompt refresh.\n";
}

void Application::PrintActiveRuntime(const ActiveRuntime& runtime) const {
    std::cout << ToString(runtime.type)
              << " -> " << runtime.selected_name
              << " [" << runtime.distribution << "] "
              << PathToUtf8(runtime.root)
              << " (scope=" << runtime.scope << ", updated=" << runtime.updated_at_utc << ")\n";
}

void Application::PrintInstalledRuntime(const InstalledRuntime& runtime) const {
    std::cout << ToString(runtime.type)
              << " | " << runtime.distribution
              << " | " << runtime.name;

    if (runtime.is_environment) {
        std::cout << " | env";
    }

    std::cout << " | selector=" << PreferredInstalledRuntimeSelector(runtime);

    if (runtime.is_external) {
        std::cout << " | preserved";
    }

    if (runtime.type == RuntimeType::Python) {
        if (runtime.is_environment) {
            std::cout << " | base=" << runtime.base_name;
        }
    }

    std::cout << " | " << PathToUtf8(runtime.root) << '\n';
}

}  // namespace jkm
