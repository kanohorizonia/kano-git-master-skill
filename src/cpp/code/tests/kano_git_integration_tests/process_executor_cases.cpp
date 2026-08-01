#include "shell_executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#endif

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

TEST_CASE("capture timeout includes inherited writers after the parent exits",
          "[integration][process][capture][timeout][KG-BUG-0090]") {
#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        R"ps($child = Start-Process -FilePath powershell -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 10; Write-Output DONE"' -NoNewWindow -PassThru; Write-Output EARLY; exit 0)ps"
    };
    const auto [result, elapsed] = RunTimedProcess(program, args, 2000);
    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stdoutStr.find("EARLY") != std::string::npos);
    REQUIRE(result.stdoutStr.find("DONE") == std::string::npos);
    INFO("elapsed_ms=" << elapsed.count());
    REQUIRE(elapsed < std::chrono::seconds(4));
#else
    const std::vector<std::string> commands{
        "printf 'EARLY\\n'; (sleep 5; printf 'DONE\\n') & exit 0",
        "printf 'EARLY\\n'; (sleep 5; printf 'DONE\\n') 2>/dev/null & exit 0",
        "printf 'EARLY\\n' >&2; (sleep 5; printf 'DONE\\n' >&2) >/dev/null & exit 0",
    };

    for (const auto& command : commands) {
        CAPTURE(command);
        const auto [result, elapsed] = RunTimedProcess("sh", {"-c", command}, 75);
        INFO(result.stdoutStr);
        INFO(result.stderrStr);
        REQUIRE(result.exitCode == 124);
        REQUIRE((result.stdoutStr + result.stderrStr).find("EARLY") != std::string::npos);
        REQUIRE(result.stdoutStr.find("DONE") == std::string::npos);
        REQUIRE(result.stderrStr.find("DONE") == std::string::npos);
        INFO("elapsed_ms=" << elapsed.count());
        REQUIRE(elapsed < std::chrono::milliseconds(1500));
    }
#endif
}

TEST_CASE("capture preserves a descendant that closes inherited writers before deadline",
          "[integration][process][capture][early-exit][KG-BUG-0090]") {
#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        R"ps($child = Start-Process -FilePath powershell -ArgumentList '-NoProfile -Command "Start-Sleep -Milliseconds 100; Write-Output LATE"' -NoNewWindow -PassThru; Write-Output EARLY; exit 0)ps"
    };
    constexpr unsigned int timeout_ms = 5000;
    constexpr auto elapsed_bound = std::chrono::seconds(5);
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c",
        "printf 'EARLY\\n'; (sleep 0.05; printf 'LATE\\n') & exit 0"
    };
    constexpr unsigned int timeout_ms = 2000;
    constexpr auto elapsed_bound = std::chrono::seconds(2);
#endif

    const auto [result, elapsed] = RunTimedProcess(program, args, timeout_ms);
    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.find("EARLY") != std::string::npos);
    REQUIRE(result.stdoutStr.find("LATE") != std::string::npos);
    REQUIRE(elapsed < elapsed_bound);
}

TEST_CASE("capture timeout closes writers held by an escaped POSIX session",
          "[integration][process][capture][timeout][KG-BUG-0090]") {
#if defined(_WIN32)
    SUCCEED("POSIX escaped-session capture test skipped on Windows");
#else
    namespace fs = std::filesystem;
    const fs::path perl = "/usr/bin/perl";
    if (!fs::exists(perl)) {
        SUCCEED("escaped-session fixture requires /usr/bin/perl");
        return;
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto pid_path = fs::temp_directory_path() /
        ("kog-kg-bug-0090-" + std::to_string(nonce) + ".pid");
    std::error_code ec;
    fs::remove(pid_path, ec);

    const std::string command =
        "/usr/bin/perl -MPOSIX -e '"
        "POSIX::setsid(); open(my $fh, q(>), $ARGV[0]) or die; "
        "print {$fh} \"$$\\n\"; close($fh); sleep 5' \"$1\" & "
        "while [ ! -s \"$1\" ]; do sleep 0.01; done; exit 0";
    const auto [result, elapsed] = RunTimedProcess(
        "sh", {"-c", command, "kg-bug-0090", pid_path.string()}, 500);

    long descendant_pid = 0;
    {
        std::ifstream input(pid_path);
        input >> descendant_pid;
    }
    if (descendant_pid > 0) {
        ::kill(static_cast<pid_t>(descendant_pid), SIGKILL);
    }
    fs::remove(pid_path, ec);

    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    INFO("elapsed_ms=" << elapsed.count());
    REQUIRE(descendant_pid > 0);
    REQUIRE(result.exitCode == 124);
    REQUIRE(elapsed < std::chrono::milliseconds(2500));
#endif
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
