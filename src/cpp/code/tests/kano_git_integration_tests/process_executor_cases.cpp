#include "shell_executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace kano::git::tests::integration {
namespace {

auto RunTimedProcess(const std::string& InProgram,
                     const std::vector<std::string>& InArgs,
                     const unsigned int InTimeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = shell::ExecuteCommand(
        InProgram,
        InArgs,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        InTimeoutMs);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return std::pair{result, elapsed};
}

} // namespace

TEST_CASE("capture drains high-volume stdout and stderr without deadlock",
          "[integration][process][capture][deadlock]") {
#if defined(_WIN32)
    const std::string program = "cmd";
    const std::vector<std::string> args{
        "/c",
        "(for /L %i in (1,1,3000) do @echo OUT-%i) & "
        "(for /L %i in (1,1,3000) do @echo ERR-%i 1>&2)"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c",
        "i=1; while [ \"$i\" -le 3000 ]; do "
        "printf 'OUT-%s\\n' \"$i\"; printf 'ERR-%s\\n' \"$i\" >&2; "
        "i=$((i+1)); done"
    };
#endif

    const auto [result, elapsed] = RunTimedProcess(program, args, 15000);
    INFO(result.stderrStr);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.find("OUT-3000") != std::string::npos);
    REQUIRE(result.stderrStr.find("ERR-3000") != std::string::npos);
    REQUIRE(elapsed < std::chrono::seconds(15));
}

TEST_CASE("capture timeout terminates a long-running child deterministically",
          "[integration][process][timeout]") {
#if defined(_WIN32)
    const std::string program = "cmd";
    const std::vector<std::string> args{
        "/c",
        "powershell -NoProfile -Command \"Start-Sleep -Seconds 2; Write-Output DONE\""
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{"-c", "sleep 2; printf 'DONE\\n'"};
#endif

    const auto [result, elapsed] = RunTimedProcess(program, args, 75);
    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stdoutStr.find("DONE") == std::string::npos);
    REQUIRE(result.stderrStr.find("source=command_timeout_override") != std::string::npos);
    REQUIRE(result.stderrStr.find("configured_timeout_ms=75") != std::string::npos);
    INFO("elapsed_ms=" << elapsed.count());
    REQUIRE(elapsed < std::chrono::seconds(1));
}

TEST_CASE("capture preserves early non-zero exit and both output streams",
          "[integration][process][early-exit]") {
#if defined(_WIN32)
    const std::string program = "cmd";
    const std::vector<std::string> args{
        "/c",
        "echo EARLY-OUT & echo EARLY-ERR 1>&2 & exit /b 37"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c",
        "printf 'EARLY-OUT\\n'; printf 'EARLY-ERR\\n' >&2; exit 37"
    };
#endif

    const auto [result, elapsed] = RunTimedProcess(program, args, 5000);
    REQUIRE(result.exitCode == 37);
    REQUIRE(result.stdoutStr.find("EARLY-OUT") != std::string::npos);
    REQUIRE(result.stderrStr.find("EARLY-ERR") != std::string::npos);
    REQUIRE(elapsed < std::chrono::seconds(5));
}

} // namespace kano::git::tests::integration
