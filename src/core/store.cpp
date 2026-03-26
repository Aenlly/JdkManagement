#include "core/store.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <sstream>

#include "infrastructure/process.h"
#include "infrastructure/platform_windows.h"

namespace fs = std::filesystem;

namespace jkm {

namespace {

std::vector<std::string> Split(const std::string& line, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        parts.push_back(item);
    }
    return parts;
}

std::string TrimCopy(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }

    return value.substr(start);
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

std::optional<std::string> FirstOutputLine(const std::string& output) {
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line = TrimCopy(line);
        if (!line.empty()) {
            return line;
        }
    }
    return std::nullopt;
}

std::optional<fs::path> ExistingPath(const fs::path& path) {
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
        return std::nullopt;
    }
    return path.lexically_normal();
}

std::optional<fs::path> DetectJavaRootFromHome(const std::optional<std::string>& java_home) {
    if (!java_home.has_value() || java_home->empty()) {
        return std::nullopt;
    }

    const auto home = PathFromUtf8(*java_home).lexically_normal();
    return ExistingPath(home / "bin" / "java.exe").has_value() ? std::optional<fs::path>(home) : std::nullopt;
}

std::optional<fs::path> DetectPythonRootFromHome(const std::optional<std::string>& python_home) {
    if (!python_home.has_value() || python_home->empty()) {
        return std::nullopt;
    }

    auto home = PathFromUtf8(*python_home).lexically_normal();
    if (ToLowerAscii(home.filename().string()) == "scripts") {
        home = home.parent_path();
    }

    if (ExistingPath(home / "python.exe").has_value() || ExistingPath(home / "Scripts" / "python.exe").has_value()) {
        return home;
    }

    return std::nullopt;
}

std::optional<fs::path> DetectNodeRootFromHome(const std::optional<std::string>& node_home) {
    if (!node_home.has_value() || node_home->empty()) {
        return std::nullopt;
    }

    const auto home = PathFromUtf8(*node_home).lexically_normal();
    return ExistingPath(home / "node.exe").has_value() ? std::optional<fs::path>(home) : std::nullopt;
}

std::optional<fs::path> DetectGoRootFromHome(const std::optional<std::string>& go_root) {
    if (!go_root.has_value() || go_root->empty()) {
        return std::nullopt;
    }

    const auto home = PathFromUtf8(*go_root).lexically_normal();
    return ExistingPath(home / "bin" / "go.exe").has_value() ? std::optional<fs::path>(home) : std::nullopt;
}

std::optional<fs::path> DetectToolRootFromHome(
    const std::optional<std::string>& home_value,
    const std::string& executable_name) {
    if (!home_value.has_value() || home_value->empty()) {
        return std::nullopt;
    }

    auto home = PathFromUtf8(*home_value).lexically_normal();
    if (ToLowerAscii(home.filename().string()) == "bin") {
        home = home.parent_path();
    }

    const fs::path candidates[] = {
        home / executable_name,
        home / "bin" / executable_name
    };

    for (const auto& candidate : candidates) {
        if (ExistingPath(candidate).has_value()) {
            return home;
        }
    }

    return std::nullopt;
}

std::optional<fs::path> DetectRootFromWhere(const AppPaths& paths, RuntimeType type, const std::string& command_name) {
    ProcessResult process_result;
    std::string error;
    const auto command = L"cmd.exe /d /c where.exe " + WideFromUtf8(command_name);
    if (!RunProcess(command, &process_result, &error) || process_result.exit_code != 0) {
        return std::nullopt;
    }

    const auto first_line = FirstOutputLine(process_result.output);
    if (!first_line.has_value()) {
        return std::nullopt;
    }

    auto executable = PathFromUtf8(*first_line).lexically_normal();
    if (!ExistingPath(executable).has_value()) {
        return std::nullopt;
    }
    if (PathStartsWith(executable, paths.root) || PathStartsWith(executable, paths.current)) {
        return std::nullopt;
    }

    const auto filename = ToLowerAscii(executable.filename().string());
    auto parent = executable.parent_path();

    switch (type) {
        case RuntimeType::Java:
            if (filename == "java.exe") {
                if (ToLowerAscii(parent.filename().string()) == "bin") {
                    return parent.parent_path();
                }
                return parent;
            }
            break;
        case RuntimeType::Python:
            if (filename == "python.exe") {
                if (ToLowerAscii(parent.filename().string()) == "scripts") {
                    return parent.parent_path();
                }
                return parent;
            }
            break;
        case RuntimeType::Node:
            if (filename == "node.exe") {
                return parent;
            }
            break;
        case RuntimeType::Go:
            if (filename == "go.exe") {
                if (ToLowerAscii(parent.filename().string()) == "bin") {
                    return parent.parent_path();
                }
                return parent;
            }
            break;
        case RuntimeType::Maven:
            if (filename == "mvn.cmd" || filename == "mvn.bat" || filename == "mvn") {
                if (ToLowerAscii(parent.filename().string()) == "bin") {
                    return parent.parent_path();
                }
                return parent;
            }
            break;
        case RuntimeType::Gradle:
            if (filename == "gradle.bat" || filename == "gradle.cmd" || filename == "gradle") {
                if (ToLowerAscii(parent.filename().string()) == "bin") {
                    return parent.parent_path();
                }
                return parent;
            }
            break;
    }

    return std::nullopt;
}

void AppendExternalRuntimes(
    const EnvironmentSnapshot& snapshot,
    std::optional<RuntimeType> filter,
    std::vector<InstalledRuntime>* runtimes) {
    if ((!filter.has_value() || *filter == RuntimeType::Java) && snapshot.external_java_root.has_value()) {
        const auto java_root = snapshot.external_java_root->lexically_normal();
        if (ExistingPath(java_root / "bin" / "java.exe").has_value()) {
            runtimes->push_back(InstalledRuntime{
                RuntimeType::Java,
                "external",
                "original",
                java_root,
                "original",
                java_root,
                false,
                true
            });
        }
    }

    if ((!filter.has_value() || *filter == RuntimeType::Python) && snapshot.external_python_root.has_value()) {
        const auto python_root = snapshot.external_python_root->lexically_normal();
        if (ExistingPath(python_root / "python.exe").has_value() || ExistingPath(python_root / "Scripts" / "python.exe").has_value()) {
            runtimes->push_back(InstalledRuntime{
                RuntimeType::Python,
                "external",
                "original",
                python_root,
                "original",
                python_root,
                false,
                true
            });
        }
    }

    if ((!filter.has_value() || *filter == RuntimeType::Node) && snapshot.external_node_root.has_value()) {
        const auto node_root = snapshot.external_node_root->lexically_normal();
        if (ExistingPath(node_root / "node.exe").has_value()) {
            runtimes->push_back(InstalledRuntime{
                RuntimeType::Node,
                "external",
                "original",
                node_root,
                "original",
                node_root,
                false,
                true
            });
        }
    }

    if ((!filter.has_value() || *filter == RuntimeType::Go) && snapshot.external_go_root.has_value()) {
        const auto go_root = snapshot.external_go_root->lexically_normal();
        if (ExistingPath(go_root / "bin" / "go.exe").has_value()) {
            runtimes->push_back(InstalledRuntime{
                RuntimeType::Go,
                "external",
                "original",
                go_root,
                "original",
                go_root,
                false,
                true
            });
        }
    }

    if ((!filter.has_value() || *filter == RuntimeType::Maven) && snapshot.external_maven_root.has_value()) {
        const auto maven_root = snapshot.external_maven_root->lexically_normal();
        if (ExistingPath(maven_root / "bin" / "mvn.cmd").has_value()) {
            runtimes->push_back(InstalledRuntime{
                RuntimeType::Maven,
                "external",
                "original",
                maven_root,
                "original",
                maven_root,
                false,
                true
            });
        }
    }

    if ((!filter.has_value() || *filter == RuntimeType::Gradle) && snapshot.external_gradle_root.has_value()) {
        const auto gradle_root = snapshot.external_gradle_root->lexically_normal();
        if (ExistingPath(gradle_root / "bin" / "gradle.bat").has_value()) {
            runtimes->push_back(InstalledRuntime{
                RuntimeType::Gradle,
                "external",
                "original",
                gradle_root,
                "original",
                gradle_root,
                false,
                true
            });
        }
    }
}

void WriteOptionalField(std::ofstream* output, const std::string& key, const std::optional<std::string>& value) {
    *output << key << '\t' << (value.has_value() ? '1' : '0') << '\t';
    if (value.has_value()) {
        *output << *value;
    }
    *output << '\n';
}

std::optional<std::string> ParseOptionalField(const std::vector<std::string>& parts) {
    if (parts.size() < 2 || parts[1] != "1") {
        return std::nullopt;
    }
    return parts.size() >= 3 ? std::optional<std::string>(parts[2]) : std::optional<std::string>(std::string{});
}

void CollectJavaRuntimes(const fs::path& java_root, std::vector<InstalledRuntime>* runtimes) {
    std::error_code ec;
    if (!fs::exists(java_root, ec)) {
        return;
    }

    for (const auto& distribution_dir : fs::directory_iterator(java_root, ec)) {
        if (ec || !distribution_dir.is_directory()) {
            continue;
        }

        for (const auto& version_dir : fs::directory_iterator(distribution_dir.path(), ec)) {
            if (ec || !version_dir.is_directory()) {
                continue;
            }

            runtimes->push_back(InstalledRuntime{
                RuntimeType::Java,
                distribution_dir.path().filename().string(),
                version_dir.path().filename().string(),
                version_dir.path(),
                version_dir.path().filename().string(),
                version_dir.path(),
                false,
                false
            });
        }
    }
}

void CollectVersionedRuntimes(const fs::path& runtime_root, RuntimeType type, std::vector<InstalledRuntime>* runtimes) {
    std::error_code ec;
    if (!fs::exists(runtime_root, ec)) {
        return;
    }

    for (const auto& distribution_dir : fs::directory_iterator(runtime_root, ec)) {
        if (ec || !distribution_dir.is_directory()) {
            continue;
        }

        for (const auto& version_dir : fs::directory_iterator(distribution_dir.path(), ec)) {
            if (ec || !version_dir.is_directory()) {
                continue;
            }

            runtimes->push_back(InstalledRuntime{
                type,
                distribution_dir.path().filename().string(),
                version_dir.path().filename().string(),
                version_dir.path(),
                version_dir.path().filename().string(),
                version_dir.path(),
                false,
                false
            });
        }
    }
}

void CollectPythonRuntimes(const fs::path& python_root, std::vector<InstalledRuntime>* runtimes) {
    std::error_code ec;
    if (!fs::exists(python_root, ec)) {
        return;
    }

    for (const auto& distribution_dir : fs::directory_iterator(python_root, ec)) {
        if (ec || !distribution_dir.is_directory()) {
            continue;
        }

        for (const auto& install_dir : fs::directory_iterator(distribution_dir.path(), ec)) {
            if (ec || !install_dir.is_directory()) {
                continue;
            }

            runtimes->push_back(InstalledRuntime{
                RuntimeType::Python,
                distribution_dir.path().filename().string(),
                install_dir.path().filename().string(),
                install_dir.path(),
                install_dir.path().filename().string(),
                install_dir.path(),
                false,
                false
            });

            const auto env_root = install_dir.path() / "envs";
            if (!fs::exists(env_root, ec)) {
                continue;
            }

            runtimes->push_back(InstalledRuntime{
                RuntimeType::Python,
                distribution_dir.path().filename().string(),
                "base",
                install_dir.path(),
                install_dir.path().filename().string(),
                install_dir.path(),
                true,
                false
            });

            for (const auto& env_dir : fs::directory_iterator(env_root, ec)) {
                if (ec || !env_dir.is_directory()) {
                    continue;
                }

                runtimes->push_back(InstalledRuntime{
                    RuntimeType::Python,
                    distribution_dir.path().filename().string(),
                    env_dir.path().filename().string(),
                    env_dir.path(),
                    install_dir.path().filename().string(),
                    install_dir.path(),
                    true,
                    false
                });
            }
        }
    }
}

std::string SortKey(const InstalledRuntime& runtime) {
    return (runtime.is_external ? "0" : "1") + std::string("|") + ToLowerAscii(runtime.distribution) + "|" + ToLowerAscii(runtime.base_name) + "|" + ToLowerAscii(runtime.name) + "|" +
           ToLowerAscii(runtime.root.filename().string());
}

std::vector<std::string> UniqueSelectors(std::initializer_list<std::string> values) {
    std::vector<std::string> selectors;
    for (const auto& value : values) {
        if (value.empty()) {
            continue;
        }

        const auto normalized = ToLowerAscii(value);
        const auto duplicate = std::find_if(selectors.begin(), selectors.end(), [&normalized](const std::string& existing) {
            return ToLowerAscii(existing) == normalized;
        });
        if (duplicate == selectors.end()) {
            selectors.push_back(value);
        }
    }
    return selectors;
}

}  // namespace

ActiveRuntimeStore::ActiveRuntimeStore(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

EnvironmentSnapshotStore::EnvironmentSnapshotStore(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

std::vector<ActiveRuntime> ActiveRuntimeStore::Load() const {
    std::vector<ActiveRuntime> runtimes;

    std::ifstream input(file_path_, std::ios::binary);
    if (!input.is_open()) {
        return runtimes;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        const auto parts = Split(line, '\t');
        if (parts.size() < 6) {
            continue;
        }

        const auto runtime_type = ParseRuntimeType(parts[0]);
        if (!runtime_type.has_value()) {
            continue;
        }

        runtimes.push_back(ActiveRuntime{
            *runtime_type,
            parts[1],
            parts[2],
            PathFromUtf8(parts[3]),
            parts[4],
            parts[5]
        });
    }

    return runtimes;
}

std::optional<ActiveRuntime> ActiveRuntimeStore::Get(RuntimeType type) const {
    const auto runtimes = Load();
    const auto iterator = std::find_if(runtimes.begin(), runtimes.end(), [type](const ActiveRuntime& runtime) {
        return runtime.type == type;
    });

    if (iterator == runtimes.end()) {
        return std::nullopt;
    }
    return *iterator;
}

bool ActiveRuntimeStore::Upsert(const ActiveRuntime& runtime, std::string* error) const {
    auto runtimes = Load();
    auto iterator = std::find_if(runtimes.begin(), runtimes.end(), [&runtime](const ActiveRuntime& candidate) {
        return candidate.type == runtime.type;
    });

    if (iterator == runtimes.end()) {
        runtimes.push_back(runtime);
    } else {
        *iterator = runtime;
    }

    std::ofstream output(file_path_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "unable to open active runtime store for writing";
        }
        return false;
    }

    for (const auto& item : runtimes) {
        output << ToString(item.type) << '\t'
               << item.selected_name << '\t'
               << item.distribution << '\t'
               << PathToUtf8(item.root) << '\t'
               << item.scope << '\t'
               << item.updated_at_utc << '\n';
    }

    return true;
}

bool ActiveRuntimeStore::Remove(RuntimeType type, std::string* error) const {
    auto runtimes = Load();
    runtimes.erase(std::remove_if(runtimes.begin(), runtimes.end(), [type](const ActiveRuntime& runtime) {
        return runtime.type == type;
    }), runtimes.end());

    if (runtimes.empty()) {
        std::error_code ec;
        fs::remove(file_path_, ec);
        if (ec) {
            if (error != nullptr) {
                *error = "unable to remove active runtime store";
            }
            return false;
        }
        return true;
    }

    std::ofstream output(file_path_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "unable to open active runtime store for writing";
        }
        return false;
    }

    for (const auto& item : runtimes) {
        output << ToString(item.type) << '\t'
               << item.selected_name << '\t'
               << item.distribution << '\t'
               << PathToUtf8(item.root) << '\t'
               << item.scope << '\t'
               << item.updated_at_utc << '\n';
    }

    return true;
}

bool ActiveRuntimeStore::Clear(std::string* error) const {
    std::error_code ec;
    fs::remove(file_path_, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "unable to remove active runtime store";
        }
        return false;
    }
    return true;
}

const std::filesystem::path& ActiveRuntimeStore::FilePath() const {
    return file_path_;
}

bool EnvironmentSnapshotStore::Exists() const {
    std::error_code ec;
    return fs::exists(file_path_, ec);
}

std::optional<EnvironmentSnapshot> EnvironmentSnapshotStore::Load(std::string* error) const {
    std::ifstream input(file_path_, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }

    EnvironmentSnapshot snapshot;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        const auto parts = Split(line, '\t');
        if (parts.empty()) {
            continue;
        }

        if (parts[0] == "created_at_utc") {
            if (parts.size() >= 2) {
                snapshot.created_at_utc = parts[1];
            }
            continue;
        }

        const auto value = ParseOptionalField(parts);
        if (parts[0] == "path_value") {
            snapshot.path_value = value;
        } else if (parts[0] == "java_home") {
            snapshot.java_home = value;
        } else if (parts[0] == "python_home") {
            snapshot.python_home = value;
        } else if (parts[0] == "conda_prefix") {
            snapshot.conda_prefix = value;
        } else if (parts[0] == "node_home") {
            snapshot.node_home = value;
        } else if (parts[0] == "go_root") {
            snapshot.go_root = value;
        } else if (parts[0] == "maven_home") {
            snapshot.maven_home = value;
        } else if (parts[0] == "m2_home") {
            snapshot.m2_home = value;
        } else if (parts[0] == "gradle_home") {
            snapshot.gradle_home = value;
        } else if (parts[0] == "jdkm_home") {
            snapshot.jdkm_home = value;
        } else if (parts[0] == "jdkm_current_java") {
            snapshot.jdkm_current_java = value;
        } else if (parts[0] == "jdkm_current_python") {
            snapshot.jdkm_current_python = value;
        } else if (parts[0] == "jdkm_current_python_base") {
            snapshot.jdkm_current_python_base = value;
        } else if (parts[0] == "jdkm_current_python_env") {
            snapshot.jdkm_current_python_env = value;
        } else if (parts[0] == "jdkm_current_node") {
            snapshot.jdkm_current_node = value;
        } else if (parts[0] == "jdkm_current_go") {
            snapshot.jdkm_current_go = value;
        } else if (parts[0] == "jdkm_current_maven") {
            snapshot.jdkm_current_maven = value;
        } else if (parts[0] == "jdkm_current_gradle") {
            snapshot.jdkm_current_gradle = value;
        } else if (parts[0] == "external_java_root") {
            snapshot.external_java_root = value.has_value() ? std::optional<fs::path>(PathFromUtf8(*value)) : std::nullopt;
        } else if (parts[0] == "external_python_root") {
            snapshot.external_python_root = value.has_value() ? std::optional<fs::path>(PathFromUtf8(*value)) : std::nullopt;
        } else if (parts[0] == "external_node_root") {
            snapshot.external_node_root = value.has_value() ? std::optional<fs::path>(PathFromUtf8(*value)) : std::nullopt;
        } else if (parts[0] == "external_go_root") {
            snapshot.external_go_root = value.has_value() ? std::optional<fs::path>(PathFromUtf8(*value)) : std::nullopt;
        } else if (parts[0] == "external_maven_root") {
            snapshot.external_maven_root = value.has_value() ? std::optional<fs::path>(PathFromUtf8(*value)) : std::nullopt;
        } else if (parts[0] == "external_gradle_root") {
            snapshot.external_gradle_root = value.has_value() ? std::optional<fs::path>(PathFromUtf8(*value)) : std::nullopt;
        }
    }

    if (snapshot.created_at_utc.empty()) {
        if (error != nullptr) {
            *error = "environment snapshot is missing created_at_utc";
        }
        return std::nullopt;
    }

    return snapshot;
}

bool EnvironmentSnapshotStore::Save(const EnvironmentSnapshot& snapshot, std::string* error) const {
    std::ofstream output(file_path_, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "unable to open environment snapshot store for writing";
        }
        return false;
    }

    output << "created_at_utc" << '\t' << snapshot.created_at_utc << '\n';
    WriteOptionalField(&output, "path_value", snapshot.path_value);
    WriteOptionalField(&output, "java_home", snapshot.java_home);
    WriteOptionalField(&output, "python_home", snapshot.python_home);
    WriteOptionalField(&output, "conda_prefix", snapshot.conda_prefix);
    WriteOptionalField(&output, "node_home", snapshot.node_home);
    WriteOptionalField(&output, "go_root", snapshot.go_root);
    WriteOptionalField(&output, "maven_home", snapshot.maven_home);
    WriteOptionalField(&output, "m2_home", snapshot.m2_home);
    WriteOptionalField(&output, "gradle_home", snapshot.gradle_home);
    WriteOptionalField(&output, "jdkm_home", snapshot.jdkm_home);
    WriteOptionalField(&output, "jdkm_current_java", snapshot.jdkm_current_java);
    WriteOptionalField(&output, "jdkm_current_python", snapshot.jdkm_current_python);
    WriteOptionalField(&output, "jdkm_current_python_base", snapshot.jdkm_current_python_base);
    WriteOptionalField(&output, "jdkm_current_python_env", snapshot.jdkm_current_python_env);
    WriteOptionalField(&output, "jdkm_current_node", snapshot.jdkm_current_node);
    WriteOptionalField(&output, "jdkm_current_go", snapshot.jdkm_current_go);
    WriteOptionalField(&output, "jdkm_current_maven", snapshot.jdkm_current_maven);
    WriteOptionalField(&output, "jdkm_current_gradle", snapshot.jdkm_current_gradle);
    WriteOptionalField(&output, "external_java_root",
                       snapshot.external_java_root.has_value() ? std::optional<std::string>(PathToUtf8(*snapshot.external_java_root)) : std::nullopt);
    WriteOptionalField(&output, "external_python_root",
                       snapshot.external_python_root.has_value() ? std::optional<std::string>(PathToUtf8(*snapshot.external_python_root)) : std::nullopt);
    WriteOptionalField(&output, "external_node_root",
                       snapshot.external_node_root.has_value() ? std::optional<std::string>(PathToUtf8(*snapshot.external_node_root)) : std::nullopt);
    WriteOptionalField(&output, "external_go_root",
                       snapshot.external_go_root.has_value() ? std::optional<std::string>(PathToUtf8(*snapshot.external_go_root)) : std::nullopt);
    WriteOptionalField(&output, "external_maven_root",
                       snapshot.external_maven_root.has_value() ? std::optional<std::string>(PathToUtf8(*snapshot.external_maven_root)) : std::nullopt);
    WriteOptionalField(&output, "external_gradle_root",
                       snapshot.external_gradle_root.has_value() ? std::optional<std::string>(PathToUtf8(*snapshot.external_gradle_root)) : std::nullopt);
    return true;
}

const std::filesystem::path& EnvironmentSnapshotStore::FilePath() const {
    return file_path_;
}

bool CaptureEnvironmentSnapshot(
    const AppPaths& paths,
    EnvironmentSnapshot* result,
    std::string* error) {
    EnvironmentSnapshot snapshot;
    snapshot.created_at_utc = NowUtcIso8601();

    std::string ignored_error;
    snapshot.path_value = ReadUserEnvironmentVariable("Path", &ignored_error);
    snapshot.java_home = ReadUserEnvironmentVariable("JAVA_HOME", &ignored_error);
    snapshot.python_home = ReadUserEnvironmentVariable("PYTHON_HOME", &ignored_error);
    snapshot.conda_prefix = ReadUserEnvironmentVariable("CONDA_PREFIX", &ignored_error);
    snapshot.node_home = ReadUserEnvironmentVariable("NODE_HOME", &ignored_error);
    snapshot.go_root = ReadUserEnvironmentVariable("GOROOT", &ignored_error);
    snapshot.maven_home = ReadUserEnvironmentVariable("MAVEN_HOME", &ignored_error);
    snapshot.m2_home = ReadUserEnvironmentVariable("M2_HOME", &ignored_error);
    snapshot.gradle_home = ReadUserEnvironmentVariable("GRADLE_HOME", &ignored_error);
    snapshot.jdkm_home = ReadUserEnvironmentVariable("JDKM_HOME", &ignored_error);
    snapshot.jdkm_current_java = ReadUserEnvironmentVariable("JDKM_CURRENT_JAVA", &ignored_error);
    snapshot.jdkm_current_python = ReadUserEnvironmentVariable("JDKM_CURRENT_PYTHON", &ignored_error);
    snapshot.jdkm_current_python_base = ReadUserEnvironmentVariable("JDKM_CURRENT_PYTHON_BASE", &ignored_error);
    snapshot.jdkm_current_python_env = ReadUserEnvironmentVariable("JDKM_CURRENT_PYTHON_ENV", &ignored_error);
    snapshot.jdkm_current_node = ReadUserEnvironmentVariable("JDKM_CURRENT_NODE", &ignored_error);
    snapshot.jdkm_current_go = ReadUserEnvironmentVariable("JDKM_CURRENT_GO", &ignored_error);
    snapshot.jdkm_current_maven = ReadUserEnvironmentVariable("JDKM_CURRENT_MAVEN", &ignored_error);
    snapshot.jdkm_current_gradle = ReadUserEnvironmentVariable("JDKM_CURRENT_GRADLE", &ignored_error);

    snapshot.external_java_root = DetectJavaRootFromHome(snapshot.java_home);
    if (!snapshot.external_java_root.has_value()) {
        snapshot.external_java_root = DetectRootFromWhere(paths, RuntimeType::Java, "java");
    }

    snapshot.external_python_root = DetectPythonRootFromHome(snapshot.python_home);
    if (!snapshot.external_python_root.has_value()) {
        snapshot.external_python_root = DetectPythonRootFromHome(snapshot.conda_prefix);
    }
    if (!snapshot.external_python_root.has_value()) {
        snapshot.external_python_root = DetectRootFromWhere(paths, RuntimeType::Python, "python");
    }

    snapshot.external_node_root = DetectNodeRootFromHome(snapshot.node_home);
    if (!snapshot.external_node_root.has_value()) {
        snapshot.external_node_root = DetectRootFromWhere(paths, RuntimeType::Node, "node");
    }

    snapshot.external_go_root = DetectGoRootFromHome(snapshot.go_root);
    if (!snapshot.external_go_root.has_value()) {
        snapshot.external_go_root = DetectRootFromWhere(paths, RuntimeType::Go, "go");
    }

    snapshot.external_maven_root = DetectToolRootFromHome(snapshot.maven_home, "mvn.cmd");
    if (!snapshot.external_maven_root.has_value()) {
        snapshot.external_maven_root = DetectToolRootFromHome(snapshot.m2_home, "mvn.cmd");
    }
    if (!snapshot.external_maven_root.has_value()) {
        snapshot.external_maven_root = DetectRootFromWhere(paths, RuntimeType::Maven, "mvn");
    }

    snapshot.external_gradle_root = DetectToolRootFromHome(snapshot.gradle_home, "gradle.bat");
    if (!snapshot.external_gradle_root.has_value()) {
        snapshot.external_gradle_root = DetectRootFromWhere(paths, RuntimeType::Gradle, "gradle");
    }

    if (result != nullptr) {
        *result = std::move(snapshot);
    }
    (void)error;
    return true;
}

std::vector<InstalledRuntime> ScanInstalledRuntimes(const AppPaths& paths, std::optional<RuntimeType> filter, const EnvironmentSnapshot* snapshot) {
    std::vector<InstalledRuntime> runtimes;

    if (!filter.has_value() || *filter == RuntimeType::Java) {
        CollectJavaRuntimes(paths.installs / "java", &runtimes);
    }
    if (!filter.has_value() || *filter == RuntimeType::Python) {
        CollectPythonRuntimes(paths.installs / "python", &runtimes);
    }
    if (!filter.has_value() || *filter == RuntimeType::Node) {
        CollectVersionedRuntimes(paths.installs / "node", RuntimeType::Node, &runtimes);
    }
    if (!filter.has_value() || *filter == RuntimeType::Go) {
        CollectVersionedRuntimes(paths.installs / "go", RuntimeType::Go, &runtimes);
    }
    if (!filter.has_value() || *filter == RuntimeType::Maven) {
        CollectVersionedRuntimes(paths.installs / "maven", RuntimeType::Maven, &runtimes);
    }
    if (!filter.has_value() || *filter == RuntimeType::Gradle) {
        CollectVersionedRuntimes(paths.installs / "gradle", RuntimeType::Gradle, &runtimes);
    }
    if (snapshot != nullptr) {
        AppendExternalRuntimes(*snapshot, filter, &runtimes);
    }

    std::sort(runtimes.begin(), runtimes.end(), [](const InstalledRuntime& left, const InstalledRuntime& right) {
        if (left.type != right.type) {
            return ToString(left.type) < ToString(right.type);
        }
        return SortKey(left) < SortKey(right);
    });

    return runtimes;
}

std::vector<InstalledRuntime> FindInstalledRuntimeMatches(
    const std::vector<InstalledRuntime>& installed,
    RuntimeType type,
    const std::string& selector) {
    const auto normalized_selector = ToLowerAscii(selector);

    std::vector<InstalledRuntime> exact_matches;
    for (const auto& runtime : installed) {
        if (runtime.type != type) {
            continue;
        }

        const auto selectors = InstalledRuntimeSelectors(runtime);
        const auto exact_match = std::find_if(selectors.begin(), selectors.end(), [&normalized_selector](const std::string& candidate) {
            return ToLowerAscii(candidate) == normalized_selector;
        });
        if (exact_match != selectors.end()) {
            exact_matches.push_back(runtime);
        }
    }

    if (!exact_matches.empty()) {
        return exact_matches;
    }

    std::vector<InstalledRuntime> prefix_matches;
    for (const auto& runtime : installed) {
        if (runtime.type != type) {
            continue;
        }

        const auto selectors = InstalledRuntimeSelectors(runtime);
        const auto prefix_match = std::find_if(selectors.begin(), selectors.end(), [&normalized_selector](const std::string& candidate) {
            const auto normalized_candidate = ToLowerAscii(candidate);
            return normalized_candidate.rfind(normalized_selector, 0) == 0;
        });
        if (prefix_match != selectors.end()) {
            prefix_matches.push_back(runtime);
        }
    }

    return prefix_matches;
}

std::vector<std::string> InstalledRuntimeSelectors(const InstalledRuntime& runtime) {
    if (runtime.is_external) {
        switch (runtime.type) {
            case RuntimeType::Java:
                return UniqueSelectors({"original", "external", "external/original", "java-home"});
            case RuntimeType::Python:
                return UniqueSelectors({"original", "external", "external/original", "python-home"});
            case RuntimeType::Node:
                return UniqueSelectors({"original", "external", "external/original", "node-home"});
            case RuntimeType::Go:
                return UniqueSelectors({"original", "external", "external/original", "go-root"});
            case RuntimeType::Maven:
                return UniqueSelectors({"original", "external", "external/original", "maven-home", "m2-home"});
            case RuntimeType::Gradle:
                return UniqueSelectors({"original", "external", "external/original", "gradle-home"});
        }
        return UniqueSelectors({"original", "external"});
    }

    if (runtime.is_environment) {
        return UniqueSelectors({
            runtime.name,
            runtime.base_name + "/" + runtime.name,
            runtime.distribution + "/" + runtime.base_name + "/" + runtime.name
        });
    }

    return UniqueSelectors({
        runtime.name,
        runtime.root.filename().string(),
        runtime.distribution + "/" + runtime.name
    });
}

std::string PreferredInstalledRuntimeSelector(const InstalledRuntime& runtime) {
    if (runtime.is_external) {
        return "original";
    }
    if (runtime.type == RuntimeType::Python && runtime.is_environment) {
        return runtime.base_name + "/" + runtime.name;
    }
    return runtime.name;
}

}  // namespace jkm
