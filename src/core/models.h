#pragma once

#include <optional>
#include <string>

namespace jkm {

enum class RuntimeType {
    Java,
    Python,
    Node,
    Go,
    Maven,
    Gradle
};

struct DoctorCheck {
    std::string status;
    std::string label;
    std::string detail;
};

std::string ToString(RuntimeType type);
std::optional<RuntimeType> ParseRuntimeType(const std::string& value);
std::string ToLowerAscii(std::string value);
std::string NowUtcIso8601();
std::string GenerateOperationId();

}  // namespace jkm
