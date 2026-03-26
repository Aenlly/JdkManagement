#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace jkm {

std::string Utf8FromWide(const std::wstring& value);
std::wstring WideFromUtf8(const std::string& value);
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(const std::string& path);

bool RepointDirectoryJunction(
    const std::filesystem::path& link_path,
    const std::filesystem::path& target_path,
    std::string* error);

bool SetUserEnvironmentVariable(
    const std::string& name,
    const std::string& value,
    std::string* error);

bool DeleteUserEnvironmentVariable(
    const std::string& name,
    std::string* error);

std::optional<std::string> ReadUserEnvironmentVariable(
    const std::string& name,
    std::string* error);

bool EnsureUserPathEntry(const std::filesystem::path& entry, std::string* error);
bool RemoveDirectoryJunction(const std::filesystem::path& link_path, std::string* error);
bool BroadcastEnvironmentChanged(std::string* error);

}  // namespace jkm
