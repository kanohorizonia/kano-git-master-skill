#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <functional>

namespace kano::git::shell {

enum class ExecMode { Capture, PassThrough };

struct ExecResult {
    int exitCode = 0;
    std::string stdoutStr;  // Binary-safe; may contain embedded NUL bytes.
    std::string stderrStr;  // Binary-safe; may contain embedded NUL bytes.
    bool stdoutTruncated = false;
    bool stderrTruncated = false;
};

struct CaptureLimits {
    std::size_t stdoutMaxBytes = 0; // 0 retains all bytes.
    std::size_t stderrMaxBytes = 0;
};

using ProgressCallback = std::function<void(std::string_view chunk, bool isStderr)>;

struct CommandLogCallbacks {
    std::function<void(const std::string&)> onStdout;
    std::function<void(const std::string&)> onStderr;
};

class ScopedCommandLogCapture {
  public:
    explicit ScopedCommandLogCapture(CommandLogCallbacks InCallbacks);
    ~ScopedCommandLogCapture();

    ScopedCommandLogCapture(const ScopedCommandLogCapture&) = delete;
    auto operator=(const ScopedCommandLogCapture&) -> ScopedCommandLogCapture& = delete;

  private:
    bool active_ = false;
};

class ScopedConsoleWriteSuppression {
  public:
    ScopedConsoleWriteSuppression();
    ~ScopedConsoleWriteSuppression();

    ScopedConsoleWriteSuppression(const ScopedConsoleWriteSuppression&) = delete;
    auto operator=(const ScopedConsoleWriteSuppression&) -> ScopedConsoleWriteSuppression& = delete;

  private:
    bool active_ = false;
};

auto GetScriptsDir() -> std::filesystem::path;

auto ExecuteScript(
    std::string_view InRelativeScript,
    const std::vector<std::string>& InArgs = {},
    ExecMode InMode = ExecMode::PassThrough,
    std::optional<std::filesystem::path> InWorkingDir = std::nullopt
) -> ExecResult;

auto ExecuteCommand(
    const std::string& InCommand,
    const std::vector<std::string>& InArgs = {},
    ExecMode InMode = ExecMode::PassThrough,
    std::optional<std::filesystem::path> InWorkingDir = std::nullopt
) -> ExecResult;

auto ExecuteCommand(
    const std::string& InCommand,
    const std::vector<std::string>& InArgs,
    ExecMode InMode,
    std::optional<std::filesystem::path> InWorkingDir,
    ProgressCallback InProgressCallback,
    std::optional<unsigned int> InTimeoutOverrideMs = std::nullopt,
    CaptureLimits InCaptureLimits = {}
) -> ExecResult;

}
