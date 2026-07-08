#include "infrastructure/platform_windows.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <vector>

#include <windows.h>
#include <winioctl.h>

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

std::string FormatWindowsError(DWORD error_code) {
    if (error_code == ERROR_SUCCESS) {
        return {};
    }

    wchar_t* message = nullptr;
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::string detail;
    if (length > 0 && message != nullptr) {
        detail = Utf8FromWide(std::wstring(message, length));
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ' || detail.back() == '\t' || detail.back() == '.')) {
            detail.pop_back();
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }

    if (detail.empty()) {
        std::ostringstream stream;
        stream << "Windows error " << error_code;
        detail = stream.str();
    }
    return detail;
}

constexpr DWORD kMountPointReparseTag = 0xA0000003L;
constexpr DWORD kMaxReparseDataBufferSize = 16 * 1024;

struct JkmMountPointReparseBuffer {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    WCHAR PathBuffer[1];
};

std::wstring ToAbsoluteNativePath(const fs::path& path) {
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    auto value = absolute.lexically_normal().wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\??\\", 0) == 0) {
        return value;
    }
    return L"\\??\\" + value;
}

bool CreateDirectoryJunctionNative(const fs::path& link_path, const fs::path& target_path, std::string* error) {
    std::error_code ec;
    fs::create_directory(link_path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "failed to create junction directory " + PathToUtf8(link_path) + ": " + ec.message();
        }
        return false;
    }

    const auto substitute_name = ToAbsoluteNativePath(target_path);
    auto print_name = fs::absolute(target_path, ec).lexically_normal().wstring();
    if (ec) {
        print_name = target_path.lexically_normal().wstring();
    }
    std::replace(print_name.begin(), print_name.end(), L'/', L'\\');

    const auto substitute_bytes = static_cast<WORD>(substitute_name.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<WORD>(print_name.size() * sizeof(wchar_t));
    const auto path_bytes = static_cast<DWORD>(substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t));
    const auto reparse_data_length = static_cast<WORD>(sizeof(WORD) * 4 + path_bytes);
    const auto total_size = FIELD_OFFSET(JkmMountPointReparseBuffer, PathBuffer) + path_bytes;
    if (total_size > kMaxReparseDataBufferSize) {
        if (error != nullptr) {
            *error = "junction target path is too long: " + PathToUtf8(target_path);
        }
        RemoveDirectoryW(link_path.c_str());
        return false;
    }

    std::vector<BYTE> buffer(total_size, 0);
    auto* reparse = reinterpret_cast<JkmMountPointReparseBuffer*>(buffer.data());
    reparse->ReparseTag = kMountPointReparseTag;
    reparse->ReparseDataLength = reparse_data_length;
    reparse->SubstituteNameOffset = 0;
    reparse->SubstituteNameLength = substitute_bytes;
    reparse->PrintNameOffset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    reparse->PrintNameLength = print_bytes;
    std::memcpy(reparse->PathBuffer, substitute_name.data(), substitute_bytes);
    std::memcpy(reinterpret_cast<BYTE*>(reparse->PathBuffer) + reparse->PrintNameOffset, print_name.data(), print_bytes);

    HANDLE handle = CreateFileW(
        link_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto open_error = GetLastError();
        if (error != nullptr) {
            *error = "failed to open junction directory " + PathToUtf8(link_path) + ": " + FormatWindowsError(open_error);
        }
        RemoveDirectoryW(link_path.c_str());
        return false;
    }

    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
        handle,
        FSCTL_SET_REPARSE_POINT,
        reparse,
        total_size,
        nullptr,
        0,
        &bytes_returned,
        nullptr);
    const auto ioctl_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        if (error != nullptr) {
            *error = "failed to set junction reparse point " + PathToUtf8(link_path) + ": " + FormatWindowsError(ioctl_error);
        }
        RemoveDirectoryW(link_path.c_str());
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

    if (!RemoveDirectoryW(link_path.c_str())) {
        const auto remove_error = GetLastError();
        if (error != nullptr) {
            *error = "failed to remove existing directory link " + PathToUtf8(link_path) + ": " + FormatWindowsError(remove_error);
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

    return CreateDirectoryJunctionNative(link_path, target_path, error);
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
    std::vector<std::wstring> reordered_entries;
    reordered_entries.push_back(entry.lexically_normal().wstring());

    const auto entries = SplitPath(current_path);
    for (const auto& existing : entries) {
        if (existing.empty() || NormalizeWindowsPath(existing) == normalized_entry) {
            continue;
        }
        reordered_entries.push_back(existing);
    }

    std::wstring updated_path;
    for (std::size_t index = 0; index < reordered_entries.size(); ++index) {
        if (index > 0) {
            updated_path += L';';
        }
        updated_path += reordered_entries[index];
    }

    if (updated_path == current_path) {
        return true;
    }

    return SetUserEnvironmentVariable("Path", Utf8FromWide(updated_path), error);
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
