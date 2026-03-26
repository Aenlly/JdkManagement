#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jkm {

struct OperationRecord {
    std::string operation_id;
    std::string command_name;
    std::string command_line;
    std::string developer;
    std::string runtime_type;
    std::string selector;
    int exit_code{0};
    std::string status;
    std::string started_at_utc;
    std::string ended_at_utc;
    std::string note;
};

class OperationAuditStore {
public:
    explicit OperationAuditStore(std::filesystem::path file_path);

    bool Append(const OperationRecord& record, std::string* error) const;
    std::optional<OperationRecord> FindByOperationId(const std::string& operation_id) const;
    std::vector<OperationRecord> LoadAll(std::string* error = nullptr) const;
    std::vector<OperationRecord> ListRecent(
        std::size_t limit,
        const std::optional<std::string>& command_name = std::nullopt,
        const std::optional<std::string>& runtime_type = std::nullopt,
        const std::optional<std::string>& status = std::nullopt,
        std::string* error = nullptr) const;
    std::filesystem::path FilePath() const;

private:
    std::filesystem::path file_path_;
};

}  // namespace jkm
