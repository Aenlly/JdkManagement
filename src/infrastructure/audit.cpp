#include "infrastructure/audit.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace jkm {

namespace {

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

std::string ExtractJsonString(const std::string& line, const std::string& key) {
    const auto pattern = "\"" + key + "\":\"";
    const auto start = line.find(pattern);
    if (start == std::string::npos) {
        return {};
    }

    std::string value;
    bool escaped = false;
    for (std::size_t index = start + pattern.size(); index < line.size(); ++index) {
        const auto ch = line[index];
        if (escaped) {
            switch (ch) {
                case '\\':
                    value.push_back('\\');
                    break;
                case '"':
                    value.push_back('"');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(ch);
                    break;
            }
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        value.push_back(ch);
    }
    return value;
}

int ExtractJsonInt(const std::string& line, const std::string& key) {
    const auto pattern = "\"" + key + "\":";
    const auto start = line.find(pattern);
    if (start == std::string::npos) {
        return 0;
    }

    std::size_t end = start + pattern.size();
    while (end < line.size() && (line[end] == '-' || (line[end] >= '0' && line[end] <= '9'))) {
        ++end;
    }

    return std::stoi(line.substr(start + pattern.size(), end - (start + pattern.size())));
}

OperationRecord ParseOperationRecord(const std::string& line) {
    return OperationRecord{
        ExtractJsonString(line, "operationId"),
        ExtractJsonString(line, "commandName"),
        ExtractJsonString(line, "commandLine"),
        ExtractJsonString(line, "developer"),
        ExtractJsonString(line, "runtimeType"),
        ExtractJsonString(line, "selector"),
        ExtractJsonInt(line, "exitCode"),
        ExtractJsonString(line, "status"),
        ExtractJsonString(line, "startedAt"),
        ExtractJsonString(line, "endedAt"),
        ExtractJsonString(line, "note")
    };
}

}  // namespace

OperationAuditStore::OperationAuditStore(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

bool OperationAuditStore::Append(const OperationRecord& record, std::string* error) const {
    std::error_code ec;
    fs::create_directories(file_path_.parent_path(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = "unable to create audit directory";
        }
        return false;
    }

    std::ofstream output(file_path_, std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "unable to open audit store for append";
        }
        return false;
    }

    output << "{\"operationId\":\"" << EscapeJson(record.operation_id)
           << "\",\"commandName\":\"" << EscapeJson(record.command_name)
           << "\",\"commandLine\":\"" << EscapeJson(record.command_line)
           << "\",\"developer\":\"" << EscapeJson(record.developer)
           << "\",\"runtimeType\":\"" << EscapeJson(record.runtime_type)
           << "\",\"selector\":\"" << EscapeJson(record.selector)
           << "\",\"exitCode\":" << record.exit_code
           << ",\"status\":\"" << EscapeJson(record.status)
           << "\",\"startedAt\":\"" << EscapeJson(record.started_at_utc)
           << "\",\"endedAt\":\"" << EscapeJson(record.ended_at_utc)
           << "\",\"note\":\"" << EscapeJson(record.note)
           << "\"}\n";
    return true;
}

std::optional<OperationRecord> OperationAuditStore::FindByOperationId(const std::string& operation_id) const {
    std::ifstream input(file_path_, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (ExtractJsonString(line, "operationId") != operation_id) {
            continue;
        }

        return ParseOperationRecord(line);
    }

    return std::nullopt;
}

std::vector<OperationRecord> OperationAuditStore::LoadAll(std::string* error) const {
    std::ifstream input(file_path_, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }

    std::vector<OperationRecord> records;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        records.push_back(ParseOperationRecord(line));
    }

    if (input.bad() && error != nullptr) {
        *error = "unable to read audit store";
    }
    return records;
}

std::vector<OperationRecord> OperationAuditStore::ListRecent(
    std::size_t limit,
    const std::optional<std::string>& command_name,
    const std::optional<std::string>& runtime_type,
    const std::optional<std::string>& status,
    std::string* error) const {
    auto records = LoadAll(error);
    std::vector<OperationRecord> filtered;
    filtered.reserve(records.size());

    for (const auto& record : records) {
        if (command_name.has_value() && record.command_name != *command_name) {
            continue;
        }
        if (runtime_type.has_value() && record.runtime_type != *runtime_type) {
            continue;
        }
        if (status.has_value() && record.status != *status) {
            continue;
        }
        filtered.push_back(record);
    }

    if (filtered.size() > limit) {
        filtered.erase(filtered.begin(), filtered.end() - static_cast<std::ptrdiff_t>(limit));
    }
    std::reverse(filtered.begin(), filtered.end());
    return filtered;
}

std::filesystem::path OperationAuditStore::FilePath() const {
    return file_path_;
}

}  // namespace jkm
