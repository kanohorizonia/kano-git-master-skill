#include <catch2/catch_test_macros.hpp>

#include "shell_executor.hpp"

#include <cstdlib>
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

    const std::vector<std::string> args{
        "/c",
        "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output DONE\""
    };
    const auto result = ExecuteCommand("cmd", args, ExecMode::Capture);

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.find("timeout") != std::string::npos);
    SetEnvVarForTest("KOG_SHELL_CAPTURE_TIMEOUT_MS", "0");
#else
    SUCCEED("Windows-specific timeout test skipped on non-Windows platform");
#endif
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
