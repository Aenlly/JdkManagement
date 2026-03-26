#pragma once

#include <functional>
#include <string>

namespace jkm {

struct ProcessResult {
    int exit_code{0};
    std::string output;
};

using ProcessOutputLineHandler = std::function<void(const std::string&)>;

bool RunProcess(
    const std::wstring& command_line,
    ProcessResult* result,
    std::string* error,
    const ProcessOutputLineHandler& output_line_handler = {});
bool RunPowerShellScript(
    const std::string& script,
    ProcessResult* result,
    std::string* error,
    const ProcessOutputLineHandler& output_line_handler = {});
bool RunTempBatchScript(
    const std::string& script,
    ProcessResult* result,
    std::string* error,
    const ProcessOutputLineHandler& output_line_handler = {});

}  // namespace jkm
