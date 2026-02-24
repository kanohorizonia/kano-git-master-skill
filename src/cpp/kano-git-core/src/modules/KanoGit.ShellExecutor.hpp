#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>

namespace kano::git::shell {

enum class ExecMode { Capture, PassThrough };

struct ExecResult {
    int exitCode = 0;
    std::string stdoutStr;
    std::string stderrStr;
};

auto GetScriptsDir() -> std::filesystem::path;

auto ExecuteScript(
    std::string_view relativeScript,
    const std::vector<std::string>& args = {},
    ExecMode mode = ExecMode::PassThrough,
    std::optional<std::filesystem::path> workingDir = std::nullopt
) -> ExecResult;

auto ExecuteCommand(
    const std::string& command,
    const std::vector<std::string>& args = {},
    ExecMode mode = ExecMode::PassThrough,
    std::optional<std::filesystem::path> workingDir = std::nullopt
) -> ExecResult;

}
