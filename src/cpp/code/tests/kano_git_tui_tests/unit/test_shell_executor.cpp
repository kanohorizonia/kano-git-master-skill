#include <catch2/catch_test_macros.hpp>

#include "shell_executor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace kano::git::shell;

namespace {

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
