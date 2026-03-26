#include "infrastructure/platform_windows.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <vector>

#include <windows.h>

namespace fs = std::filesystem;

namespace jkm {

namespace {

std::wstring NormalizeWindowsPath(const std::wstring& path) {
    std::wstring normalized = path;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t ch) {
        if (ch == L'/') {
            return L'\\';
        }
        return static_cast<wchar_t>(std::towlower(ch));
    });

    while (!normalized.empty() && (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }

    return normalized;
}

bool ReadRegistryString(HKEY root, const wchar_t* sub_key, const wchar_t* value_name, std::wstring* value, std::string* error) {
    HKEY key = nullptr;
    auto result = RegOpenKeyExW(root, sub_key, 0, KEY_READ, &key);
    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "unable to open registry key";
        }
        return false;
    }

    DWORD type = 0;
    DWORD size = 0;
    result = RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return false;
    }
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        if (error != nullptr) {
            *error = "unable to query registry value";
        }
        RegCloseKey(key);
        return false;
    }

    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(
        key,
        value_name,
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(buffer.data()),
        &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "unable to read registry value";
        }
        return false;
    }

    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }

    *value = buffer;
    return true;
}

bool WriteRegistryString(HKEY root, const wchar_t* sub_key, const wchar_t* value_name, const std::wstring& value, std::string* error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const auto result = RegCreateKeyExW(
        root,
        sub_key,
        0,
        nullptr,
        0,
        KEY_SET_VALUE,
        nullptr,
        &key,
        &disposition);

    if (result != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "unable to create registry key";
        }
        return false;
    }

    const auto status = RegSetValueExW(
        key,
        value_name,
        0,
        REG_EXPAND_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "unable to persist registry value";
        }
        return false;
    }

    return true;
}

bool DeleteRegistryValue(HKEY root, const wchar_t* sub_key, const wchar_t* value_name, std::string* error) {
    HKEY key = nullptr;
    const auto open_status = RegOpenKeyExW(root, sub_key, 0, KEY_SET_VALUE, &key);
    if (open_status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (open_status != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "unable to open registry key for delete";
        }
        return false;
    }

    const auto delete_status = RegDeleteValueW(key, value_name);
    RegCloseKey(key);

    if (delete_status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (delete_status != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "unable to delete registry value";
        }
        return false;
    }

    return true;
}

bool RemoveExistingLink(const fs::path& link_path, std::string* error) {
    DWORD attributes = GetFileAttributesW(link_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        if (error != nullptr) {
            *error = "refusing to overwrite a non-link path: " + PathToUtf8(link_path);
        }
        return false;
    }

    std::wstring command = L"cmd.exe /c rmdir \"" + link_path.wstring() + L"\" >nul 2>nul";
    const auto exit_code = _wsystem(command.c_str());
    if (exit_code != 0) {
        if (error != nullptr) {
            *error = "failed to remove existing directory link";
        }
        return false;
    }

    return true;
}

std::vector<std::wstring> SplitPath(const std::wstring& path_value) {
    std::vector<std::wstring> entries;
    std::size_t start = 0;
    while (start <= path_value.size()) {
        const auto end = path_value.find(L';', start);
        entries.push_back(path_value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return entries;
}

}  // namespace

std::string Utf8FromWide(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    return Utf8FromWide(path.wstring());
}

std::filesystem::path PathFromUtf8(const std::string& path) {
    return fs::path(WideFromUtf8(path));
}

bool RepointDirectoryJunction(const std::filesystem::path& link_path, const std::filesystem::path& target_path, std::string* error) {
    std::error_code ec;
    if (!fs::exists(target_path, ec)) {
        if (error != nullptr) {
            *error = "target path does not exist: " + PathToUtf8(target_path);
        }
        return false;
    }

    fs::create_directories(link_path.parent_path(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = "failed to create link parent directory";
        }
        return false;
    }

    if (!RemoveExistingLink(link_path, error)) {
        return false;
    }

    std::wstring command = L"cmd.exe /c mklink /J \"" + link_path.wstring() + L"\" \"" + target_path.wstring() + L"\" >nul";
    const auto exit_code = _wsystem(command.c_str());
    if (exit_code != 0) {
        if (error != nullptr) {
            *error = "mklink /J failed";
        }
        return false;
    }

    return true;
}

bool SetUserEnvironmentVariable(const std::string& name, const std::string& value, std::string* error) {
    const auto wide_name = WideFromUtf8(name);
    const auto wide_value = WideFromUtf8(value);

    if (!WriteRegistryString(HKEY_CURRENT_USER, L"Environment", wide_name.c_str(), wide_value, error)) {
        return false;
    }

    if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str())) {
        if (error != nullptr) {
            *error = "failed to update process environment";
        }
        return false;
    }

    return true;
}

bool DeleteUserEnvironmentVariable(const std::string& name, std::string* error) {
    const auto wide_name = WideFromUtf8(name);

    if (!DeleteRegistryValue(HKEY_CURRENT_USER, L"Environment", wide_name.c_str(), error)) {
        return false;
    }

    if (!SetEnvironmentVariableW(wide_name.c_str(), nullptr)) {
        if (error != nullptr) {
            *error = "failed to update process environment";
        }
        return false;
    }

    return true;
}

std::optional<std::string> ReadUserEnvironmentVariable(const std::string& name, std::string* error) {
    std::wstring value;
    if (!ReadRegistryString(HKEY_CURRENT_USER, L"Environment", WideFromUtf8(name).c_str(), &value, error)) {
        return std::nullopt;
    }
    return Utf8FromWide(value);
}

bool EnsureUserPathEntry(const std::filesystem::path& entry, std::string* error) {
    std::wstring current_path;
    std::string ignored_error;
    ReadRegistryString(HKEY_CURRENT_USER, L"Environment", L"Path", &current_path, &ignored_error);

    const auto normalized_entry = NormalizeWindowsPath(entry.lexically_normal().wstring());
    const auto entries = SplitPath(current_path);
    for (const auto& existing : entries) {
        if (NormalizeWindowsPath(existing) == normalized_entry) {
            return true;
        }
    }

    if (!current_path.empty() && current_path.back() != L';') {
        current_path += L';';
    }
    current_path += entry.lexically_normal().wstring();

    return SetUserEnvironmentVariable("Path", Utf8FromWide(current_path), error);
}

bool RemoveDirectoryJunction(const std::filesystem::path& link_path, std::string* error) {
    return RemoveExistingLink(link_path, error);
}

bool BroadcastEnvironmentChanged(std::string* error) {
    DWORD_PTR result = 0;
    const auto status = SendMessageTimeoutW(
        HWND_BROADCAST,
        WM_SETTINGCHANGE,
        0,
        reinterpret_cast<LPARAM>(L"Environment"),
        SMTO_ABORTIFHUNG,
        5000,
        &result);

    if (status == 0) {
        if (error != nullptr) {
            *error = "SendMessageTimeoutW returned failure";
        }
        return false;
    }

    return true;
}

}  // namespace jkm
