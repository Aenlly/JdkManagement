#include "infrastructure/process.h"

#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

#include "infrastructure/platform_windows.h"

namespace fs = std::filesystem;

namespace jkm {

namespace {

std::wstring QuoteForCommandLine(const std::wstring& value) {
    std::wstring quoted = L"\"";
    for (const auto ch : value) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::wstring RandomScriptName() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<unsigned int> distribution(0, 0xffffffffu);

    wchar_t buffer[32];
    swprintf_s(buffer, L"jkm-%08x.ps1", distribution(generator));
    return buffer;
}

std::wstring RandomBatchName() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<unsigned int> distribution(0, 0xffffffffu);

    wchar_t buffer[32];
    swprintf_s(buffer, L"jkm-%08x.cmd", distribution(generator));
    return buffer;
}

bool WriteUtf8BomFile(const fs::path& path, const std::string& content, std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        if (error != nullptr) {
            *error = "unable to create temporary PowerShell script";
        }
        return false;
    }

    constexpr std::array<unsigned char, 3> bom{0xEF, 0xBB, 0xBF};
    stream.write(reinterpret_cast<const char*>(bom.data()), static_cast<std::streamsize>(bom.size()));
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

void EmitBufferedLines(
    std::string* pending,
    const ProcessOutputLineHandler& output_line_handler,
    bool flush_tail) {
    if (pending == nullptr || !output_line_handler) {
        return;
    }

    std::size_t line_end = pending->find('\n');
    while (line_end != std::string::npos) {
        auto line = pending->substr(0, line_end);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        output_line_handler(line);
        pending->erase(0, line_end + 1);
        line_end = pending->find('\n');
    }

    if (flush_tail && !pending->empty()) {
        auto line = *pending;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        output_line_handler(line);
        pending->clear();
    }
}

}  // namespace

bool RunProcess(
    const std::wstring& command_line,
    ProcessResult* result,
    std::string* error,
    const ProcessOutputLineHandler& output_line_handler) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &attributes, 0)) {
        if (error != nullptr) {
            *error = "CreatePipe failed";
        }
        return false;
    }

    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdOutput = write_handle;
    startup_info.hStdError = write_handle;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    auto mutable_command = command_line;
    const auto created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup_info,
        &process_info);

    CloseHandle(write_handle);
    write_handle = nullptr;

    if (!created) {
        CloseHandle(read_handle);
        if (error != nullptr) {
            *error = "CreateProcessW failed";
        }
        return false;
    }

    std::string output;
    std::string pending_output_line;
    std::array<char, 4096> buffer{};
    bool process_finished = false;

    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }

        while (available > 0) {
            DWORD bytes_read = 0;
            if (!ReadFile(read_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) || bytes_read == 0) {
                available = 0;
                break;
            }

            output.append(buffer.data(), buffer.data() + bytes_read);
            if (output_line_handler) {
                pending_output_line.append(buffer.data(), buffer.data() + bytes_read);
                EmitBufferedLines(&pending_output_line, output_line_handler, false);
            }
            if (!PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr)) {
                available = 0;
            }
        }

        if (!process_finished) {
            const auto wait_result = WaitForSingleObject(process_info.hProcess, 50);
            if (wait_result == WAIT_OBJECT_0) {
                process_finished = true;
            }
        } else if (available == 0) {
            break;
        }
    }

    if (output_line_handler) {
        EmitBufferedLines(&pending_output_line, output_line_handler, true);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);

    CloseHandle(read_handle);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    if (result != nullptr) {
        result->exit_code = static_cast<int>(exit_code);
        result->output = std::move(output);
    }
    return true;
}

bool RunPowerShellScript(
    const std::string& script,
    ProcessResult* result,
    std::string* error,
    const ProcessOutputLineHandler& output_line_handler) {
    std::error_code ec;
    const auto script_path = fs::temp_directory_path(ec) / RandomScriptName();
    if (ec) {
        if (error != nullptr) {
            *error = "unable to resolve temp directory for PowerShell script";
        }
        return false;
    }

    if (!WriteUtf8BomFile(script_path, script, error)) {
        return false;
    }

    const std::wstring command_line =
        L"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File " + QuoteForCommandLine(script_path.wstring());

    ProcessResult process_result;
    const auto ok = RunProcess(command_line, &process_result, error, output_line_handler);

    std::error_code remove_ec;
    fs::remove(script_path, remove_ec);

    if (!ok) {
        return false;
    }

    if (result != nullptr) {
        *result = std::move(process_result);
    }
    return true;
}

bool RunTempBatchScript(
    const std::string& script,
    ProcessResult* result,
    std::string* error,
    const ProcessOutputLineHandler& output_line_handler) {
    std::error_code ec;
    const auto script_path = fs::temp_directory_path(ec) / RandomBatchName();
    if (ec) {
        if (error != nullptr) {
            *error = "unable to resolve temp directory for batch script";
        }
        return false;
    }

    if (!WriteUtf8BomFile(script_path, script, error)) {
        return false;
    }

    const std::wstring command_line =
        L"cmd.exe /d /c " + QuoteForCommandLine(script_path.wstring());

    ProcessResult process_result;
    const auto ok = RunProcess(command_line, &process_result, error, output_line_handler);

    std::error_code remove_ec;
    fs::remove(script_path, remove_ec);

    if (!ok) {
        return false;
    }

    if (result != nullptr) {
        *result = std::move(process_result);
    }
    return true;
}

}  // namespace jkm
