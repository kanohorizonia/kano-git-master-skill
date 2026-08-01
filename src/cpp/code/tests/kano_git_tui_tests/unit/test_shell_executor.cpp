#include <catch2/catch_test_macros.hpp>

#include "shell_executor.hpp"
#include <kano_process.h>

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace kano::git::shell;

static_assert(offsetof(KanoProcessResult, exit_code) == 0);
static_assert(offsetof(KanoProcessResult, stderr_data) ==
              offsetof(KanoProcessResult, stdout_data) + sizeof(char*));
static_assert(offsetof(KanoProcessResult, timed_out) ==
              offsetof(KanoProcessResult, stderr_data) + sizeof(char*));

namespace {

class ScopedTempDirectory final {
public:
    ScopedTempDirectory() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
            ("kog-process-v2-" + std::to_string(nonce) + "-" +
             std::to_string(
                 reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    auto operator=(const ScopedTempDirectory&)
        -> ScopedTempDirectory& = delete;

    auto Path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void SetEnvVarForTest(const char* InName, const char* InValue) {
#if defined(_WIN32)
    _putenv_s(InName, InValue);
#else
    if (InValue == nullptr || std::string(InValue).empty()) {
        unsetenv(InName);
    } else {
        setenv(InName, InValue, 1);
    }
#endif
}

std::string ReadTextFile(const std::filesystem::path& InPath) {
    std::ifstream input(InPath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE(
    "ShellExecutor capture preserves embedded NUL bytes",
    "[Unit][shell-executor][binary-capture][KG-BUG-0088]") {
#if defined(_WIN32)
    const auto result = ExecuteCommand(
        "powershell",
        {
            "-NoProfile",
            "-Command",
            "$out=[Console]::OpenStandardOutput();"
            "$stdoutBytes=[byte[]](65,0,66);"
            "$out.Write($stdoutBytes,0,$stdoutBytes.Length);"
            "$err=[Console]::OpenStandardError();"
            "$stderrBytes=[byte[]](69,0,70);"
            "$err.Write($stderrBytes,0,$stderrBytes.Length)",
        },
        ExecMode::Capture);
#else
    const auto result = ExecuteCommand(
        "sh",
        {
            "-c",
            "printf 'A\\000B'; printf 'E\\000F' >&2",
        },
        ExecMode::Capture);
#endif

    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr == std::string("A\0B", 3));
    REQUIRE(result.stderrStr == std::string("E\0F", 3));
}

TEST_CASE(
    "Kano process legacy result ABI remains bounded to the V1 object",
    "[Unit][shell-executor][binary-capture][abi][KG-BUG-0088]") {
    struct LegacyCallerStorage {
        KanoProcessResult result{};
        std::uint64_t canary = 0xA55AA55ADEADBEEFULL;
    } storage;

#if defined(_WIN32)
    const char* args[] = {"cmd", "/c", "echo legacy", nullptr};
    KanoProcessOptions options{};
    options.executable = "cmd";
    options.argv_count = 3;
#else
    const char* args[] = {"-c", "printf legacy", nullptr};
    KanoProcessOptions options{};
    options.executable = "sh";
    options.argv_count = 2;
#endif
    options.argv = args;
    options.mode = KANO_PROCESS_MODE_CAPTURE;

    REQUIRE(kano_process_run_ex(&options, &storage.result));
    REQUIRE(storage.canary == 0xA55AA55ADEADBEEFULL);
    REQUIRE(storage.result.exit_code == 0);
    REQUIRE(storage.result.stdout_data != nullptr);
    REQUIRE(std::string(storage.result.stdout_data).find("legacy") !=
            std::string::npos);
    kano_process_free_result(&storage.result);
    REQUIRE(storage.canary == 0xA55AA55ADEADBEEFULL);
}

TEST_CASE(
    "Kano process V2 rejects null output before spawn and zeros failures",
    "[Unit][shell-executor][binary-capture][abi][KG-BUG-0088]") {
    KanoProcessResultV2 failedResult;
    std::memset(&failedResult, 0xA5, sizeof(failedResult));
    REQUIRE_FALSE(kano_process_run_ex_v2(nullptr, nullptr, &failedResult));
    REQUIRE(failedResult.exit_code == 0);
    REQUIRE(failedResult.stdout_data == nullptr);
    REQUIRE(failedResult.stdout_size == 0);
    REQUIRE_FALSE(failedResult.stdout_truncated);
    REQUIRE(failedResult.stderr_data == nullptr);
    REQUIRE(failedResult.stderr_size == 0);
    REQUIRE_FALSE(failedResult.stderr_truncated);
    REQUIRE_FALSE(failedResult.timed_out);

    KanoProcessResultV2 failedWait;
    std::memset(&failedWait, 0x5A, sizeof(failedWait));
    REQUIRE_FALSE(kano_process_wait_v2(nullptr, 1, nullptr, &failedWait));
    REQUIRE(failedWait.exit_code == 0);
    REQUIRE(failedWait.stdout_data == nullptr);
    REQUIRE(failedWait.stdout_size == 0);
    REQUIRE_FALSE(failedWait.stdout_truncated);
    REQUIRE(failedWait.stderr_data == nullptr);
    REQUIRE(failedWait.stderr_size == 0);
    REQUIRE_FALSE(failedWait.stderr_truncated);
    REQUIRE_FALSE(failedWait.timed_out);

#if !defined(_WIN32)
    const ScopedTempDirectory temp;
    const auto marker = temp.Path() / "must-not-spawn";
    const auto command = "touch '" + marker.string() + "'";
    const char* args[] = {"-c", command.c_str(), nullptr};
    KanoProcessOptions options{};
    options.executable = "sh";
    options.argv = args;
    options.argv_count = 2;
    options.mode = KANO_PROCESS_MODE_CAPTURE;
    REQUIRE_FALSE(kano_process_run_ex_v2(&options, nullptr, nullptr));
    REQUIRE_FALSE(std::filesystem::exists(marker));
#endif
}

TEST_CASE(
    "ShellExecutor bounded capture truncates retention while draining both pipes",
    "[Unit][shell-executor][binary-capture][KG-BUG-0088]") {
#if defined(_WIN32)
    const auto result = ExecuteCommand(
        "powershell",
        {
            "-NoProfile",
            "-Command",
            "$out=[Console]::OpenStandardOutput();"
            "$err=[Console]::OpenStandardError();"
            "$bytes=New-Object byte[] 65536;"
            "$out.Write($bytes,0,$bytes.Length);"
            "$err.Write($bytes,0,$bytes.Length)",
        },
        ExecMode::Capture,
        std::nullopt,
        ProgressCallback{},
        std::nullopt,
        CaptureLimits{128, 64});
#else
    const auto result = ExecuteCommand(
        "sh",
        {"-c", "head -c 65536 /dev/zero; head -c 65536 /dev/zero >&2"},
        ExecMode::Capture,
        std::nullopt,
        ProgressCallback{},
        std::nullopt,
        CaptureLimits{128, 64});
#endif

    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.size() == 128);
    REQUIRE(result.stderrStr.size() == 64);
    REQUIRE(result.stdoutTruncated);
    REQUIRE(result.stderrTruncated);
}

TEST_CASE(
    "ShellExecutor V2 capture handles empty streams and spawn failure",
    "[Unit][shell-executor][binary-capture][KG-BUG-0088]") {
#if defined(_WIN32)
    const auto empty = ExecuteCommand("cmd", {"/c", "exit /b 0"}, ExecMode::Capture);
#else
    const auto empty = ExecuteCommand("sh", {"-c", "exit 0"}, ExecMode::Capture);
#endif
    REQUIRE(empty.exitCode == 0);
    REQUIRE(empty.stdoutStr.empty());
    REQUIRE(empty.stderrStr.empty());
    REQUIRE_FALSE(empty.stdoutTruncated);
    REQUIRE_FALSE(empty.stderrTruncated);

    const auto missing = ExecuteCommand(
        "kog-command-that-does-not-exist-kg-bug-0088",
        {},
        ExecMode::Capture);
    REQUIRE(missing.exitCode != 0);
}

TEST_CASE(
    "ShellExecutor V2 capture timeout releases the child and returns",
    "[Unit][shell-executor][binary-capture][timeout][KG-BUG-0088]") {
#if defined(_WIN32)
    const auto result = ExecuteCommand(
        "powershell",
        {"-NoProfile", "-Command", "Start-Sleep -Seconds 2"},
        ExecMode::Capture,
        std::nullopt,
        ProgressCallback{},
        50);
#else
    const auto result = ExecuteCommand(
        "sh",
        {"-c", "sleep 2"},
        ExecMode::Capture,
        std::nullopt,
        ProgressCallback{},
        50);
#endif
    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("timeout") != std::string::npos);
}

TEST_CASE("ShellExecutor capture drains stdout/stderr without truncation", "[Unit][shell-executor][windows]") {
#if defined(_WIN32)
    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");

    const std::vector<std::string> args{
        "/c",
        "(for /L %i in (1,1,3000) do @echo OUT-%i) & (for /L %i in (1,1,3000) do @echo ERR-%i 1>&2)"
    };
    const auto result = ExecuteCommand("cmd", args, ExecMode::Capture);

    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.find("OUT-3000") != std::string::npos);
    REQUIRE(result.stderrStr.find("ERR-3000") != std::string::npos);
#else
    SUCCEED("Windows-specific shell executor stress test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShellExecutor capture timeout terminates process with code 124", "[Unit][shell-executor][windows]") {
#if defined(_WIN32)
    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "50");
    SetEnvVarForTest("KOG_SHELL_PASSTHROUGH_TIMEOUT_MS", "");

    const std::vector<std::string> args{
        "/c",
        "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output DONE\""
    };
    const auto result = ExecuteCommand("cmd", args, ExecMode::Capture);

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("timeout") != std::string::npos);
    REQUIRE(result.stderrStr.find("source=external_command_timeout") != std::string::npos);
    REQUIRE(result.stderrStr.find("configured_timeout_ms=50") != std::string::npos);
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");
#else
    SUCCEED("Windows-specific timeout test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShellExecutor explicit timeout overrides the ambient capture timeout", "[Unit][shell-executor][timeout][KG-BUG-0006][windows]") {
#if defined(_WIN32)
    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "5000");

    const auto result = ExecuteCommand(
        "cmd",
        {"/c", "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output DONE\""},
        ExecMode::Capture,
        std::nullopt,
        ProgressCallback{},
        50);

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("source=command_timeout_override") != std::string::npos);
    REQUIRE(result.stderrStr.find("configured_timeout_ms=50") != std::string::npos);
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");
#else
    SUCCEED("Windows-specific explicit timeout test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShellExecutor gives git commit a dedicated bounded timeout", "[Unit][shell-executor][timeout][KG-BUG-0048][windows]") {
#if defined(_WIN32)
    namespace fs = std::filesystem;
    const auto tempRoot = fs::temp_directory_path() / "kog git commit timeout";
    std::error_code ec;
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot / ".git" / "hooks");

    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");
    REQUIRE(ExecuteCommand("git", {"init", "-q"}, ExecMode::Capture, tempRoot).exitCode == 0);
    REQUIRE(ExecuteCommand("git", {"config", "user.name", "KOG Test"}, ExecMode::Capture, tempRoot).exitCode == 0);
    REQUIRE(ExecuteCommand("git", {"config", "user.email", "kog-test@example.invalid"}, ExecMode::Capture, tempRoot).exitCode == 0);
    {
        std::ofstream hook(tempRoot / ".git" / "hooks" / "pre-commit", std::ios::binary);
        REQUIRE(hook.good());
        hook << "#!/bin/sh\n";
        hook << "sleep 2\n";
    }

    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_GIT_COMMIT_TIMEOUT_MS", "50");
    const auto result = ExecuteCommand(
        "git", {"commit", "--allow-empty", "-m", "timeout fixture"}, ExecMode::Capture, tempRoot);

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("configured_timeout_ms=50") != std::string::npos);
    REQUIRE(result.stderrStr.find("command_family=git:commit") != std::string::npos);
    REQUIRE(result.stderrStr.find("safe_next_action=check for git prompts, locks, or active sibling processes before retry") != std::string::npos);

    SetEnvVarForTest("KOG_GIT_COMMIT_TIMEOUT_MS", "");
    fs::remove_all(tempRoot, ec);
#else
    SUCCEED("Windows-specific git commit timeout test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShellExecutor KOG capture timeout reports provenance and safe action", "[Unit][shell-executor][timeout][KG-BUG-0014][windows]") {
#if defined(_WIN32)
    namespace fs = std::filesystem;
    const auto tempRoot = fs::temp_directory_path() / "kog shell executor timeout provenance";
    fs::create_directories(tempRoot);
    const auto scriptPath = tempRoot / "kog.cmd";
    const auto diagPath = tempRoot / "kog-timeout-process-diag.log";
    fs::remove(diagPath);

    {
        std::ofstream script(scriptPath, std::ios::binary);
        REQUIRE(script.good());
        script << "@echo off\r\n";
        script << "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output DONE\"\r\n";
    }

    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "50");
    SetEnvVarForTest("KOG_SHELL_PASSTHROUGH_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_PROCESS_DIAGNOSTICS", "1");
    SetEnvVarForTest("KOG_PROCESS_DIAGNOSTICS_LOG", diagPath.string().c_str());

    const auto result = ExecuteCommand(scriptPath.string(), {"converge"}, ExecMode::Capture, tempRoot);

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("source=kog_capture_timeout") != std::string::npos);
    REQUIRE(result.stderrStr.find("configured_timeout_ms=50") != std::string::npos);
    REQUIRE(result.stderrStr.find("command_family=kog:converge") != std::string::npos);
    REQUIRE(result.stderrStr.find("safe_next_action=inspect `kog converge --status`; resume or abort after checking active agents") != std::string::npos);

    REQUIRE(fs::exists(diagPath));
    const auto diagText = ReadTextFile(diagPath);
    REQUIRE(diagText.find("timeout_kill_marker=1") != std::string::npos);
    REQUIRE(diagText.find("timeout_source=kog_capture_timeout") != std::string::npos);
    REQUIRE(diagText.find("configured_timeout_ms=50") != std::string::npos);
    REQUIRE(diagText.find("command_family=kog:converge") != std::string::npos);
    REQUIRE(diagText.find("safe_next_action=inspect `kog converge --status`; resume or abort after checking active agents") != std::string::npos);

    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");
    SetEnvVarForTest("KOG_PROCESS_DIAGNOSTICS", "");
    SetEnvVarForTest("KOG_PROCESS_DIAGNOSTICS_LOG", "");
    std::error_code ec;
    fs::remove(scriptPath, ec);
    fs::remove(diagPath, ec);
#else
    SUCCEED("Windows-specific timeout provenance test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShellExecutor pass-through timeout terminates process with code 124", "[Unit][shell-executor]") {
    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_PASSTHROUGH_TIMEOUT_MS", "50");

#if defined(_WIN32)
    const std::vector<std::string> args{
        "/c",
        "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output DONE\""
    };
    const auto result = ExecuteCommand("cmd", args, ExecMode::PassThrough);
#else
    const std::vector<std::string> args{
        "-c",
        "sleep 2; echo DONE"
    };
    const auto result = ExecuteCommand("sh", args, ExecMode::PassThrough);
#endif

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("timeout") != std::string::npos);
    REQUIRE(result.stderrStr.find("source=external_command_timeout") != std::string::npos);
    REQUIRE(result.stderrStr.find("configured_timeout_ms=50") != std::string::npos);
    SetEnvVarForTest("KOG_SHELL_PASSTHROUGH_TIMEOUT_MS", "");
}

TEST_CASE("ShellExecutor capture preserves early-exit non-zero code", "[Unit][shell-executor][windows]") {
#if defined(_WIN32)
    SetEnvVarForTest("KOG_SHELL_TIMEOUT_MS", "");
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");

    const std::vector<std::string> args{
        "/c",
        "echo EARLY-OUT & echo EARLY-ERR 1>&2 & exit /b 37"
    };
    const auto result = ExecuteCommand("cmd", args, ExecMode::Capture);

    REQUIRE(result.exitCode == 37);
    REQUIRE(result.stdoutStr.find("EARLY-OUT") != std::string::npos);
    REQUIRE(result.stderrStr.find("EARLY-ERR") != std::string::npos);
#else
    SUCCEED("Windows-specific early-exit test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShellExecutor executes cmd scripts with spaced paths and preserved args", "[Unit][shell-executor][windows]") {
#if defined(_WIN32)
    namespace fs = std::filesystem;

    const auto tempRoot = fs::temp_directory_path() / "kog shell executor tests";
    fs::create_directories(tempRoot);
    const auto scriptPath = tempRoot / "echo-args.cmd";

    {
        std::ofstream script(scriptPath, std::ios::binary);
        REQUIRE(script.good());
        script << "@echo off\r\n";
        script << "echo ARG1=%1\r\n";
        script << "echo ARG2=%2\r\n";
    }

    const auto result = ExecuteCommand(scriptPath.string(), {"--model", "gpt-5.4"}, ExecMode::Capture);

    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.find("ARG1=--model") != std::string::npos);
    REQUIRE(result.stdoutStr.find("ARG2=gpt-5.4") != std::string::npos);

    std::error_code ec;
    fs::remove(scriptPath, ec);
#else
    SUCCEED("Windows-specific cmd script quoting test skipped on non-Windows platform");
#endif
}
