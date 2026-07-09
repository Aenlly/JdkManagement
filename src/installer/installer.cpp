#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "resources/version_info.h"

#define JKM_WIDEN2(value) L##value
#define JKM_WIDEN(value) JKM_WIDEN2(value)

namespace fs = std::filesystem;

namespace {

std::wstring ModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring EnvVar(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(written);
    return value;
}

std::wstring DefaultInstallRoot() {
    const auto local_app_data = EnvVar(L"LOCALAPPDATA");
    if (!local_app_data.empty()) {
        return (fs::path(local_app_data) / L"Programs" / L"JdkManagement").wstring();
    }
    return (fs::path(EnvVar(L"USERPROFILE")) / L"AppData" / L"Local" / L"Programs" / L"JdkManagement").wstring();
}

std::wstring NormalizePath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::vector<std::wstring> SplitPath(const std::wstring& value) {
    std::vector<std::wstring> entries;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(L';', start);
        auto entry = value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!entry.empty()) {
            entries.push_back(entry);
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return entries;
}

bool ReadUserEnvString(const wchar_t* name, std::wstring* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    auto status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        return false;
    }
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    *value = buffer;
    return true;
}

bool WriteUserEnvString(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const auto status = RegSetValueExW(key, name, 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    SetEnvironmentVariableW(name, value.c_str());
    return status == ERROR_SUCCESS;
}

bool EnsureUserPathEntry(const fs::path& entry) {
    std::wstring current;
    ReadUserEnvString(L"Path", &current);
    const auto normalized_entry = NormalizePath(entry.wstring());
    std::vector<std::wstring> output{entry.wstring()};
    for (const auto& item : SplitPath(current)) {
        if (NormalizePath(item) != normalized_entry) {
            output.push_back(item);
        }
    }
    std::wstring joined;
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (i > 0) joined += L';';
        joined += output[i];
    }
    return WriteUserEnvString(L"Path", joined);
}

bool RemoveUserPathEntry(const fs::path& entry) {
    std::wstring current;
    if (!ReadUserEnvString(L"Path", &current)) {
        return true;
    }
    const auto normalized_entry = NormalizePath(entry.wstring());
    std::vector<std::wstring> output;
    for (const auto& item : SplitPath(current)) {
        if (NormalizePath(item) != normalized_entry) {
            output.push_back(item);
        }
    }
    std::wstring joined;
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (i > 0) joined += L';';
        joined += output[i];
    }
    return WriteUserEnvString(L"Path", joined);
}

void BroadcastEnvironmentChange() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000, &result);
}

bool WriteUninstallRegistry(const fs::path& uninstall_exe, const fs::path& install_root) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\JdkManagement", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const auto set = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    const auto uninstall_string = L"\"" + uninstall_exe.wstring() + L"\"";
    set(L"DisplayName", L"JdkManagement");
    set(L"DisplayVersion", JKM_WIDEN(JKM_VERSION_STRING));
    set(L"Publisher", JKM_WIDEN(JKM_COMPANY_NAME));
    set(L"InstallLocation", install_root.wstring());
    set(L"DisplayIcon", (install_root / L"bin" / L"jkm.exe").wstring());
    set(L"UninstallString", uninstall_string);
    set(L"QuietUninstallString", uninstall_string + L" --quiet");
    DWORD no_modify = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&no_modify), sizeof(no_modify));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&no_modify), sizeof(no_modify));
    RegCloseKey(key);
    return true;
}

void DeleteUninstallRegistry() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\JdkManagement");
}

bool HasArg(int argc, wchar_t** argv, const std::wstring& arg) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], arg.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<std::wstring> ArgValue(int argc, wchar_t** argv, const std::wstring& arg) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], arg.c_str()) == 0) {
            return argv[i + 1];
        }
    }
    return std::nullopt;
}

bool CopyFileChecked(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

int Install(int argc, wchar_t** argv) {
    const auto explicit_root = ArgValue(argc, argv, L"--install-root");
    const fs::path install_root = explicit_root.has_value() ? fs::path(*explicit_root) : fs::path(DefaultInstallRoot());
    const fs::path bin_dir = install_root / L"bin";
    const fs::path self_dir = fs::path(ModulePath()).parent_path();
    const fs::path source_jkm = self_dir / L"jkm.exe";
    fs::path source_uninstaller = self_dir / L"jkm-uninstall.exe";
    if (!fs::exists(source_uninstaller)) {
        source_uninstaller = self_dir / L"JdkManagement-Uninstall-windows-x64.exe";
    }
    if (!fs::exists(source_jkm)) {
        std::wcerr << L"jkm.exe was not found next to the installer: " << source_jkm.wstring() << L"\n";
        return 1;
    }
    if (!fs::exists(source_uninstaller)) {
        std::wcerr << L"uninstaller executable was not found next to the installer.\n";
        return 1;
    }

    const auto installed_jkm = bin_dir / L"jkm.exe";
    const auto installed_uninstaller = install_root / L"jkm-uninstall.exe";
    if (!CopyFileChecked(source_jkm, installed_jkm) || !CopyFileChecked(source_uninstaller, installed_uninstaller)) {
        std::wcerr << L"failed to copy files into " << install_root.wstring() << L"\n";
        return 1;
    }
    if (!EnsureUserPathEntry(bin_dir)) {
        std::wcerr << L"failed to update user PATH\n";
        return 1;
    }
    WriteUninstallRegistry(installed_uninstaller, install_root);
    BroadcastEnvironmentChange();

    std::wcout << L"Installed JdkManagement\n";
    std::wcout << L"  Executable: " << installed_jkm.wstring() << L"\n";
    std::wcout << L"  Uninstall:  " << installed_uninstaller.wstring() << L"\n";
    std::wcout << L"Open a new terminal and run: jkm doctor\n";
    return 0;
}

bool RelaunchFromTempIfInsideInstallRoot(int argc, wchar_t** argv, const fs::path& install_root) {
    const auto self = fs::path(ModulePath());
    const auto normalized_self = NormalizePath(self.wstring());
    auto normalized_root = NormalizePath(install_root.wstring());
    if (!normalized_root.empty() && normalized_root.back() != L'\\') normalized_root += L'\\';
    if (normalized_self.rfind(normalized_root, 0) != 0 || HasArg(argc, argv, L"--from-temp")) {
        return false;
    }
    const auto temp = fs::temp_directory_path() / (L"jkm-uninstall-" + std::to_wstring(GetTickCount64()) + L".exe");
    std::error_code ec;
    fs::copy_file(self, temp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    std::wstring command = L"\"" + temp.wstring() + L"\" --from-temp --install-root \"" + install_root.wstring() + L"\"";
    if (HasArg(argc, argv, L"--purge-data")) command += L" --purge-data";
    if (HasArg(argc, argv, L"--quiet")) command += L" --quiet";
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int Uninstall(int argc, wchar_t** argv) {
    const auto explicit_root = ArgValue(argc, argv, L"--install-root");
    const fs::path install_root = explicit_root.has_value() ? fs::path(*explicit_root) : fs::path(DefaultInstallRoot());
    if (RelaunchFromTempIfInsideInstallRoot(argc, argv, install_root)) {
        return 0;
    }
    if (HasArg(argc, argv, L"--from-temp")) {
        Sleep(1500);
    }

    const fs::path bin_dir = install_root / L"bin";
    RemoveUserPathEntry(bin_dir);
    DeleteUninstallRegistry();
    std::error_code ec;
    fs::remove_all(install_root, ec);
    if (HasArg(argc, argv, L"--purge-data")) {
        fs::remove_all(fs::path(EnvVar(L"LOCALAPPDATA")) / L"JdkManagement", ec);
    }
    BroadcastEnvironmentChange();
    if (HasArg(argc, argv, L"--from-temp")) {
        MoveFileExW(ModulePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (!HasArg(argc, argv, L"--quiet")) {
        std::wcout << L"Uninstalled JdkManagement\n";
        std::wcout << L"  InstallRoot: " << install_root.wstring() << L"\n";
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef JKM_UNINSTALLER
    return Uninstall(argc, argv);
#else
    if (HasArg(argc, argv, L"--uninstall")) {
        return Uninstall(argc, argv);
    }
    return Install(argc, argv);
#endif
}
