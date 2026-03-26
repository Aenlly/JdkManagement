#include "infrastructure/logger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "core/models.h"

namespace fs = std::filesystem;

namespace jkm {

namespace {

std::string LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warning:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        default:
            return "INFO";
    }
}

std::string EscapeJson(const std::string& value) {
    std::ostringstream stream;
    for (const char character : value) {
        switch (character) {
            case '\\':
                stream << "\\\\";
                break;
            case '"':
                stream << "\\\"";
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
                stream << character;
                break;
        }
    }
    return stream.str();
}

std::string CurrentDateStamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y%m%d");
    return stream.str();
}

}  // namespace

Logger::Logger(std::filesystem::path log_directory)
    : log_directory_(std::move(log_directory)) {}

void Logger::Trace(const std::string& operation_id, const std::string& message, const LogFields& fields) {
    Write(LogLevel::Trace, operation_id, message, fields);
}

void Logger::Debug(const std::string& operation_id, const std::string& message, const LogFields& fields) {
    Write(LogLevel::Debug, operation_id, message, fields);
}

void Logger::Info(const std::string& operation_id, const std::string& message, const LogFields& fields) {
    Write(LogLevel::Info, operation_id, message, fields);
}

void Logger::Warning(const std::string& operation_id, const std::string& message, const LogFields& fields) {
    Write(LogLevel::Warning, operation_id, message, fields);
}

void Logger::Error(const std::string& operation_id, const std::string& message, const LogFields& fields) {
    Write(LogLevel::Error, operation_id, message, fields);
}

std::filesystem::path Logger::ResolveTextLogPath() const {
    return log_directory_ / ("app-" + CurrentDateStamp() + ".log");
}

std::filesystem::path Logger::ResolveJsonLogPath() const {
    return log_directory_ / ("app-" + CurrentDateStamp() + ".ndjson");
}

void Logger::Write(LogLevel level, const std::string& operation_id, const std::string& message, const LogFields& fields) {
    std::lock_guard<std::mutex> guard(mutex_);
    fs::create_directories(log_directory_);

    const auto timestamp = NowUtcIso8601();
    const auto text_log_path = ResolveTextLogPath();
    const auto json_log_path = ResolveJsonLogPath();

    std::ofstream text_log(text_log_path, std::ios::app | std::ios::binary);
    text_log << '[' << timestamp << "] " << LevelToString(level) << " op=" << operation_id << ' ' << message;
    for (const auto& field : fields) {
        text_log << ' ' << field.first << '=' << '"' << field.second << '"';
    }
    text_log << '\n';

    std::ofstream json_log(json_log_path, std::ios::app | std::ios::binary);
    json_log << "{\"timestamp\":\"" << EscapeJson(timestamp)
             << "\",\"level\":\"" << EscapeJson(LevelToString(level))
             << "\",\"operationId\":\"" << EscapeJson(operation_id)
             << "\",\"message\":\"" << EscapeJson(message) << "\"";

    for (const auto& field : fields) {
        json_log << ",\"" << EscapeJson(field.first) << "\":\"" << EscapeJson(field.second) << '"';
    }

    json_log << "}\n";
}

}  // namespace jkm
