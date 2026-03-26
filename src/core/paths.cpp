#include "core/paths.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace jkm {

namespace {

#ifdef _WIN32
std::optional<std::wstring> ReadWideEnvironment(const wchar_t* name) {
    wchar_t* buffer = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&buffer, &length, name) != 0 || buffer == nullptr || length == 0 || buffer[0] == L'\0') {
        if (buffer != nullptr) {
            free(buffer);
        }
        return std::nullopt;
    }

    std::wstring value(buffer);
    free(buffer);
    return value;
}
#endif

}  // namespace

AppPaths DetectAppPaths() {
    fs::path root;

#ifdef _WIN32
    if (const auto explicit_home = ReadWideEnvironment(L"JDKM_HOME"); explicit_home.has_value()) {
        root = fs::path(*explicit_home);
    } else if (const auto local_app_data = ReadWideEnvironment(L"LOCALAPPDATA"); local_app_data.has_value()) {
        root = fs::path(*local_app_data) / "JdkManagement";
    } else {
        root = fs::current_path() / ".jkm";
    }
#else
    if (const auto* explicit_home = std::getenv("JDKM_HOME"); explicit_home != nullptr && explicit_home[0] != '\0') {
        root = fs::path(explicit_home);
    } else {
        root = fs::current_path() / ".jkm";
    }
#endif

    return AppPaths{
        root,
        root / "cache",
        root / "cache" / "downloads",
        root / "cache" / "temp",
        root / "logs",
        root / "state",
        root / "installs",
        root / "current",
        root / "current" / "java",
        root / "current" / "python",
        root / "current" / "node",
        root / "current" / "go",
        root / "current" / "maven",
        root / "current" / "gradle",
        root / "state" / "active_runtimes.tsv",
        root / "state" / "environment_snapshot.tsv",
        root / "state" / "operations.ndjson"
    };
}

bool EnsureAppDirectories(const AppPaths& paths, std::string* error) {
    std::error_code ec;

    const fs::path directories[] = {
        paths.root,
        paths.cache,
        paths.downloads,
        paths.temp,
        paths.logs,
        paths.state,
        paths.installs / "java",
        paths.installs / "python",
        paths.installs / "node",
        paths.installs / "go",
        paths.installs / "maven",
        paths.installs / "gradle",
        paths.current
    };

    for (const auto& directory : directories) {
        fs::create_directories(directory, ec);
        if (ec) {
            if (error != nullptr) {
                *error = "failed to create directory: " + directory.string();
            }
            return false;
        }
    }

    return true;
}

}  // namespace jkm
