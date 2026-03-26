#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace jkm {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error
};

using LogFields = std::vector<std::pair<std::string, std::string>>;

class Logger {
public:
    explicit Logger(std::filesystem::path log_directory);

    void Trace(const std::string& operation_id, const std::string& message, const LogFields& fields = {});
    void Debug(const std::string& operation_id, const std::string& message, const LogFields& fields = {});
    void Info(const std::string& operation_id, const std::string& message, const LogFields& fields = {});
    void Warning(const std::string& operation_id, const std::string& message, const LogFields& fields = {});
    void Error(const std::string& operation_id, const std::string& message, const LogFields& fields = {});

private:
    void Write(LogLevel level, const std::string& operation_id, const std::string& message, const LogFields& fields);
    std::filesystem::path ResolveTextLogPath() const;
    std::filesystem::path ResolveJsonLogPath() const;

    std::filesystem::path log_directory_;
    mutable std::mutex mutex_;
};

}  // namespace jkm
