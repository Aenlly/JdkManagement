#include "core/models.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>

namespace jkm {

std::string ToString(RuntimeType type) {
    switch (type) {
        case RuntimeType::Java:
            return "java";
        case RuntimeType::Python:
            return "python";
        case RuntimeType::Node:
            return "node";
        case RuntimeType::Go:
            return "go";
        case RuntimeType::Maven:
            return "maven";
        case RuntimeType::Gradle:
            return "gradle";
        default:
            return "unknown";
    }
}

std::optional<RuntimeType> ParseRuntimeType(const std::string& value) {
    const auto lowered = ToLowerAscii(value);
    if (lowered == "java") {
        return RuntimeType::Java;
    }
    if (lowered == "python") {
        return RuntimeType::Python;
    }
    if (lowered == "node" || lowered == "nodejs") {
        return RuntimeType::Node;
    }
    if (lowered == "go" || lowered == "golang") {
        return RuntimeType::Go;
    }
    if (lowered == "maven") {
        return RuntimeType::Maven;
    }
    if (lowered == "gradle") {
        return RuntimeType::Gradle;
    }
    return std::nullopt;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string NowUtcIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string GenerateOperationId() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<unsigned int> distribution(0, 0xffffu);

    std::ostringstream stream;
    stream << "op-" << NowUtcIso8601() << '-' << std::hex << std::setw(4) << std::setfill('0') << distribution(generator);
    return stream.str();
}

}  // namespace jkm
