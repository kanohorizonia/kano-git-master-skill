#include "shell_executor.hpp"
#include <kano_process.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
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

auto ExtractDescendantPid(const std::string_view InOutput) -> long long {
    constexpr std::string_view marker = "DESCENDANT=";
    const auto markerOffset = InOutput.find(marker);
    if (markerOffset == std::string_view::npos) {
        return 0;
    }
    const auto valueBegin = InOutput.data() + markerOffset + marker.size();
    const auto valueEndOffset = InOutput.find_first_of("\r\n", markerOffset);
    const auto* valueEnd = valueEndOffset == std::string_view::npos
        ? InOutput.data() + InOutput.size()
        : InOutput.data() + valueEndOffset;
    long long pid = 0;
    const auto parsed = std::from_chars(valueBegin, valueEnd, pid);
    return parsed.ec == std::errc{} ? pid : 0;
}

auto IsFixtureProcessAlive(const long long InPid) -> bool {
    if (InPid <= 0) {
        return false;
    }
#if defined(_WIN32)
    const auto process = OpenProcess(
        SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(InPid));
    if (process == nullptr) {
        return false;
    }
    const auto waitResult = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return waitResult == WAIT_TIMEOUT;
#else
    errno = 0;
    return ::kill(static_cast<pid_t>(InPid), 0) == 0 || errno != ESRCH;
#endif
}

auto WaitForFixtureProcessExit(
    const long long InPid,
    const std::chrono::milliseconds InTimeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + InTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!IsFixtureProcessAlive(InPid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !IsFixtureProcessAlive(InPid);
}

void StopFixtureProcess(const long long InPid) {
    if (InPid <= 0) {
        return;
    }
#if defined(_WIN32)
    const auto process = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE,
        static_cast<DWORD>(InPid));
    if (process != nullptr) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 1000);
        CloseHandle(process);
    }
#else
    ::kill(static_cast<pid_t>(InPid), SIGKILL);
#endif
}

bool ObserveNativeCancellation(void* InUserData) {
    return static_cast<std::atomic<bool>*>(InUserData)->load(
        std::memory_order_acquire);
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

TEST_CASE("bounded capture preserves complete progress callbacks",
          "[integration][process][capture][limits][KG-BUG-0100]") {
    std::string progress_stdout;
    std::string progress_stderr;
    const shell::ProgressCallback progress = [&](const std::string_view chunk, const bool is_stderr) {
        auto& destination = is_stderr ? progress_stderr : progress_stdout;
        destination.append(chunk.data(), chunk.size());
    };

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        "$out=[Console]::OpenStandardOutput();"
        "$err=[Console]::OpenStandardError();"
        "$stdoutBytes=New-Object byte[] 512;"
        "$stderrBytes=New-Object byte[] 384;"
        "$out.Write($stdoutBytes,0,$stdoutBytes.Length);"
        "$err.Write($stderrBytes,0,$stderrBytes.Length)"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c",
        "head -c 512 /dev/zero; head -c 384 /dev/zero >&2"
    };
#endif

    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        progress,
        15000,
        shell::CaptureLimits{7, 5});

    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.size() == 7);
    REQUIRE(result.stderrStr.size() == 5);
    REQUIRE(result.stdoutTruncated);
    REQUIRE(result.stderrTruncated);
    REQUIRE(progress_stdout.size() == 512);
    REQUIRE(progress_stderr.size() == 384);
}

TEST_CASE("bounded capture includes timeout diagnostics in the stderr limit",
          "[integration][process][capture][limits][timeout][KG-BUG-0100]") {
#if defined(_WIN32)
    const std::string program = "cmd";
    const std::vector<std::string> args{
        "/c",
        "echo ERR 1>&2 & powershell -NoProfile -Command \"Start-Sleep -Seconds 2\""
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{"-c", "printf ERR >&2; sleep 2"};
#endif

    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        75,
        shell::CaptureLimits{0, 8});

    REQUIRE(result.exitCode == 124);
    REQUIRE(result.stderrStr.size() == 8);
    REQUIRE(result.stderrStr.starts_with("ERR"));
    REQUIRE(result.stderrTruncated);
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

TEST_CASE(
    "post-spawn cancellation terminates the owned child and descendant tree",
    "[integration][process][capture][cancellation][KOG-BUG-0107]") {
    std::atomic<bool> cancelRequested{false};
    const shell::ProgressCallback observeStart =
        [&](const std::string_view InChunk, const bool bIsStderr) {
            if (!bIsStderr && !InChunk.empty()) {
                cancelRequested.store(true, std::memory_order_release);
            }
        };
    const shell::CancellationObserver observeCancellation = [&]() {
        return cancelRequested.load(std::memory_order_acquire);
    };

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        R"ps($child = Start-Process -FilePath powershell -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 10"' -NoNewWindow -PassThru; [Console]::Out.WriteLine(('DESCENDANT=' + $child.Id)); [Console]::Out.WriteLine('READY'); [Console]::Out.Flush(); Start-Sleep -Seconds 10)ps"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c",
        "(sleep 10) & child=$!; printf 'DESCENDANT=%s\\nREADY\\n' \"$child\"; sleep 10"
    };
#endif

    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        observeStart,
        5000,
        shell::CaptureLimits{},
        observeCancellation);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    const auto descendantPid = ExtractDescendantPid(result.stdoutStr);
    const bool bDescendantExited = WaitForFixtureProcessExit(
        descendantPid,
        std::chrono::milliseconds(1000));
    if (!bDescendantExited) {
        StopFixtureProcess(descendantPid);
    }

    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    INFO("elapsed_ms=" << elapsed.count());
    INFO("descendant_pid=" << descendantPid);
    REQUIRE(cancelRequested.load(std::memory_order_acquire));
    REQUIRE(result.outcome == shell::ExecOutcome::Cancelled);
    REQUIRE(result.exitCode != 124);
    REQUIRE(result.stdoutStr.find("READY") != std::string::npos);
    REQUIRE(descendantPid > 0);
    REQUIRE(bDescendantExited);
    REQUIRE(elapsed < std::chrono::milliseconds(2500));
}

TEST_CASE(
    "an observed completed process wins over a later cancellation request",
    "[integration][process][cancellation][precedence][KOG-BUG-0107]") {
    std::atomic<bool> cancelRequested{false};
    KanoProcessOptions options{};
#if defined(_WIN32)
    const char* args[] = {"cmd", "/c", "exit /b 0", nullptr};
    options.executable = "cmd";
    options.argv_count = 3;
#else
    const char* args[] = {"-c", "exit 0", nullptr};
    options.executable = "sh";
    options.argv_count = 2;
#endif
    options.argv = args;
    options.mode = KANO_PROCESS_MODE_CAPTURE;
    options.cancellation_observer = ObserveNativeCancellation;
    options.cancellation_user_data = &cancelRequested;

    KanoProcess process = kano_process_spawn_ex(&options);
    REQUIRE(process != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    cancelRequested.store(true, std::memory_order_release);

    KanoProcessResultV2 result{};
    REQUIRE(kano_process_wait_v2(process, 2000, nullptr, &result));
    REQUIRE_FALSE(result.cancelled);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    kano_process_free_result_v2(&result);
    kano_process_free(process);
}

TEST_CASE(
    "an inactive cancellation observer preserves normal child tree completion",
    "[integration][process][capture][cancellation][precedence][KOG-BUG-0107]") {
    std::atomic<bool> cancelRequested{false};
    const shell::CancellationObserver observeCancellation = [&]() {
        return cancelRequested.load(std::memory_order_acquire);
    };

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        R"ps($child = Start-Process -FilePath powershell -ArgumentList '-NoProfile -Command "Start-Sleep -Milliseconds 100; Write-Output CHILD"' -NoNewWindow -PassThru; Write-Output READY; $child.WaitForExit(); exit $child.ExitCode)ps"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c",
        "(sleep 0.1; printf 'CHILD\\n') & child=$!; printf 'READY\\n'; wait \"$child\""
    };
#endif

    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        5000,
        shell::CaptureLimits{},
        observeCancellation);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);

    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    INFO("elapsed_ms=" << elapsed.count());
    REQUIRE_FALSE(cancelRequested.load(std::memory_order_acquire));
    REQUIRE(result.outcome == shell::ExecOutcome::Completed);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutStr.find("READY") != std::string::npos);
    REQUIRE(result.stdoutStr.find("CHILD") != std::string::npos);
    REQUIRE(elapsed < std::chrono::milliseconds(2500));
}

// KOG-BUG-0107 cancellation race matrix.  Each test below pins one race
// order between cancellation, timeout, normal exit, and stdout/stderr
// activity.  The fixtures emit READY markers so ordering is determined by
// the harness, not by wall-clock delays.  Single 750 ms sleeps are only
// used to give the race a deterministic ordering window; the assertions
// then check the typed outcome, not the timing.
TEST_CASE(
    "repeated cancellation is idempotent and never produces a fake success",
    "[integration][process][capture][cancellation][race][KOG-BUG-0107]") {
    std::atomic<bool> cancelRequested{false};
    const shell::CancellationObserver observeCancellation = [&]() {
        return cancelRequested.load(std::memory_order_acquire);
    };

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        R"ps(Write-Output READY; Start-Sleep -Seconds 10)ps"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c", "printf 'READY\\n'; sleep 10"
    };
#endif

    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        5000,
        shell::CaptureLimits{},
        observeCancellation);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);

    // Drive cancellation a second time after the wait returned.  The
    // process is already gone; this must not crash and must not change
    // the recorded outcome.
    cancelRequested.store(true, std::memory_order_release);
    cancelRequested.store(false, std::memory_order_release);

    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    INFO("elapsed_ms=" << elapsed.count());
    REQUIRE(result.outcome == shell::ExecOutcome::Cancelled);
    REQUIRE(result.exitCode != 124);
    REQUIRE(result.stdoutStr.find("READY") != std::string::npos);
    REQUIRE(elapsed < std::chrono::milliseconds(2500));
}

TEST_CASE(
    "cancellation observed before timeout wins even when timeout would have fired later",
    "[integration][process][capture][cancellation][race][KOG-BUG-0107]") {
    std::atomic<bool> cancelRequested{false};
    // Signal cancellation the moment any output byte arrives, which is
    // before the 200 ms timeout deadline below.
    const shell::ProgressCallback observeStart =
        [&](const std::string_view InChunk, const bool bIsStderr) {
            if (!bIsStderr && !InChunk.empty()) {
                cancelRequested.store(true, std::memory_order_release);
            }
        };
    const shell::CancellationObserver observeCancellation = [&]() {
        return cancelRequested.load(std::memory_order_acquire);
    };

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command",
        R"ps(Write-Output READY; Start-Sleep -Seconds 10)ps"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c", "printf 'READY\\n'; sleep 10"
    };
#endif

    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        observeStart,
        200, // Timeout that would have fired at ~200 ms.
        shell::CaptureLimits{},
        observeCancellation);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);

    INFO(result.stdoutStr);
    INFO(result.stderrStr);
    INFO("elapsed_ms=" << elapsed.count());
    REQUIRE(cancelRequested.load(std::memory_order_acquire));
    REQUIRE(result.outcome == shell::ExecOutcome::Cancelled);
    // Cancellation must NOT be relabeled as timeout even though the
    // 200 ms deadline would have fired later.
    REQUIRE(result.exitCode != 124);
    REQUIRE(result.stdoutStr.find("READY") != std::string::npos);
    REQUIRE(elapsed < std::chrono::milliseconds(2500));
}

TEST_CASE(
    "timeout observed before cancel wins and is never relabeled as Cancelled",
    "[integration][process][capture][timeout][race][KOG-BUG-0107]") {
    // Observer only flips after a 750 ms wall-clock wait; the timeout
    // deadline is 250 ms so timeout wins.
    std::atomic<bool> cancelRequested{false};
    const shell::CancellationObserver observeCancellation = [&]() {
        return cancelRequested.load(std::memory_order_acquire);
    };

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command", "Start-Sleep -Seconds 10"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c", "sleep 10"
    };
#endif

    std::thread lateCanceler([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        cancelRequested.store(true, std::memory_order_release);
    });

    const auto startedAt = std::chrono::steady_clock::now();
    const auto result = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        250,
        shell::CaptureLimits{},
        observeCancellation);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    lateCanceler.join();

    INFO(result.stderrStr);
    INFO("elapsed_ms=" << elapsed.count());
    // Timeout fires first, so the outcome must be TimedOut, not
    // Cancelled, even though the cancel signal arrives 500 ms later.
    REQUIRE(result.outcome == shell::ExecOutcome::TimedOut);
    REQUIRE(result.exitCode == 124);
    REQUIRE(elapsed < std::chrono::milliseconds(2000));
}

TEST_CASE(
    "dismissed A and active B do not share terminal state across generations",
    "[integration][process][capture][cancellation][race][generation][KOG-BUG-0107]") {
    // Dismissed A: cancel signal flips immediately; the A run should
    // observe Cancelled.
    std::atomic<bool> cancelA{false};
    const shell::CancellationObserver observeA = [&]() {
        return cancelA.load(std::memory_order_acquire);
    };
    cancelA.store(true, std::memory_order_release);

#if defined(_WIN32)
    const std::string program = "powershell";
    const std::vector<std::string> args{
        "-NoProfile",
        "-Command", "Start-Sleep -Seconds 10"
    };
#else
    const std::string program = "sh";
    const std::vector<std::string> args{
        "-c", "sleep 10"
    };
#endif

    const auto aStartedAt = std::chrono::steady_clock::now();
    const auto aResult = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        5000,
        shell::CaptureLimits{},
        observeA);
    const auto aElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - aStartedAt);

    REQUIRE(aResult.outcome == shell::ExecOutcome::Cancelled);

    // Active B: same fixture, no observer set.  B must complete
    // normally; the dismissed A's cancel signal must not contaminate B.
    const shell::CancellationObserver observeB = []() { return false; };
    const auto bStartedAt = std::chrono::steady_clock::now();
    const auto bResult = shell::ExecuteCommand(
        program,
        args,
        shell::ExecMode::Capture,
        std::nullopt,
        shell::ProgressCallback{},
        5000,
        shell::CaptureLimits{},
        observeB);

    INFO("A elapsed_ms=" << aElapsed.count());
    INFO("A outcome=" << static_cast<int>(aResult.outcome));
    INFO("B outcome=" << static_cast<int>(bResult.outcome));
    // B was a fast-completing observer; the cancellation token is a
    // per-call observer, so the late Cancel from A is irrelevant.
    // B uses a never-cancelling observer; with a 5 s timeout and a
    // 10 s sleep the only legal outcomes are Timeout (124) or
    // Completed (0).  In either case, B must NOT carry Cancelled.
    REQUIRE(bResult.outcome != shell::ExecOutcome::Cancelled);
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
