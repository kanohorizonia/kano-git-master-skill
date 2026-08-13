// Real terminal lifecycle coverage for the production TUI binary.
//
// These tests deliberately do not use the in-process FTXUI harness: terminal
// mode, resize delivery and cancellation must be proved at the process / OS
// boundary that a user (or an agent terminal driver) actually reaches.

#include <catch2/catch_test_macros.hpp>

#include "functional_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <consoleapi3.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace {

using namespace std::chrono_literals;

constexpr auto kTerminalDeadline = 8s;
constexpr std::size_t kMaximumTranscriptBytes = 64U * 1024U;

auto TopBorderForColumns(const std::size_t InColumns) -> std::string {
    if (InColumns < 2U) return {};
    std::string border = "\xE2\x95\xAD"; // ╭
    for (std::size_t column = 2U; column < InColumns; ++column) {
        border += "\xE2\x94\x80"; // ─
    }
    border += "\xE2\x95\xAE"; // ╮
    return border;
}

auto StandaloneTuiBinary() -> std::filesystem::path {
    const auto kog = kano::git::tests::functional::ResolveKogBinaryPath();
    return kog.parent_path() /
#if defined(_WIN32)
        "kano-git-tui.exe";
#else
        "kano-git-tui";
#endif
}

constexpr std::string_view kProductionUtf8FrameGlyph = "\xE2\x95\xAD"; // ╭

#if !defined(_WIN32)

class PosixPtyProcess final {
  public:
    PosixPtyProcess(const std::filesystem::path& InProgram,
                    const bool bInAcknowledgeStartupCancellation = false) {
        struct winsize initialSize {};
        initialSize.ws_row = 30;
        initialSize.ws_col = 100;

        REQUIRE(openpty(&master_, &slave_, nullptr, nullptr, &initialSize) == 0);
        REQUIRE(tcgetattr(slave_, &initialMode_) == 0);
        child_ = fork();
        REQUIRE(child_ >= 0);
        if (child_ == 0) {
            if (setsid() < 0 || ioctl(slave_, TIOCSCTTY, 0) != 0 ||
                dup2(slave_, STDIN_FILENO) < 0 ||
                dup2(slave_, STDOUT_FILENO) < 0 ||
                dup2(slave_, STDERR_FILENO) < 0) {
                _exit(126);
            }
            if (slave_ > STDERR_FILENO) (void)close(slave_);
            (void)close(master_);
            if (bInAcknowledgeStartupCancellation) {
                (void)setenv("KOG_TEST_MODE", "1", 1);
                (void)setenv("KOG_TUI_TEST_STARTUP_CANCEL_ACK", "1", 1);
            }
            const auto program = InProgram.string();
            char* const argv[] = {
                const_cast<char*>(program.c_str()),
                nullptr,
            };
            execv(program.c_str(), argv);
            _exit(127);
        }
        REQUIRE(close(slave_) == 0);
        slave_ = -1;

        const int flags = fcntl(master_, F_GETFL, 0);
        REQUIRE(flags >= 0);
        REQUIRE(fcntl(master_, F_SETFL, flags | O_NONBLOCK) == 0);
    }

    ~PosixPtyProcess() {
        if (child_ > 0) {
            kill(child_, SIGKILL);
            (void)waitpid(child_, nullptr, 0);
        }
        if (master_ >= 0) {
            (void)close(master_);
        }
    }

    PosixPtyProcess(const PosixPtyProcess&) = delete;
    auto operator=(const PosixPtyProcess&) -> PosixPtyProcess& = delete;

    auto AwaitFirstFrame() -> bool {
        return AwaitText("q/Escape exits");
    }

    auto Resize(const unsigned short InColumns, const unsigned short InRows)
        -> bool {
        DrainAvailableOutput();
        resizeTranscriptOffset_ = transcript_.size();
        struct winsize size {};
        size.ws_col = InColumns;
        size.ws_row = InRows;
        return ioctl(master_, TIOCSWINSZ, &size) == 0;
    }

    auto AwaitResizedFrame(const std::size_t InColumns) -> bool {
        return AwaitTextFrom(
            TopBorderForColumns(InColumns),
            resizeTranscriptOffset_);
    }

    auto AwaitRawInput() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            struct termios mode {};
            if (tcgetattr(master_, &mode) == 0 &&
                (mode.c_lflag & ICANON) == 0) return true;
            (void)AwaitOutput(std::chrono::milliseconds(50));
        }
        return false;
    }

    auto Write(std::string_view InBytes) -> bool {
        const char* current = InBytes.data();
        std::size_t remaining = InBytes.size();
        while (remaining > 0U) {
            const auto written = write(master_, current, remaining);
            if (written > 0) {
                current += written;
                remaining -= static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    auto WaitForExit(int& OutExitCode) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            DrainAvailableOutput();
            int status = 0;
            const auto result = waitpid(child_, &status, WNOHANG);
            if (result == child_) {
                child_ = -1;
                OutExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                return true;
            }
            if (result < 0 && errno != EINTR) return false;
            (void)AwaitOutput(std::chrono::milliseconds(100));
        }
        return false;
    }

    auto TerminalWasRestored() const -> bool {
        struct termios current {};
        return tcgetattr(master_, &current) == 0 &&
            current.c_iflag == initialMode_.c_iflag &&
            current.c_oflag == initialMode_.c_oflag &&
            current.c_cflag == initialMode_.c_cflag &&
            current.c_lflag == initialMode_.c_lflag &&
            cfgetispeed(&current) == cfgetispeed(&initialMode_) &&
            cfgetospeed(&current) == cfgetospeed(&initialMode_) &&
            std::equal(std::begin(current.c_cc), std::end(current.c_cc),
                std::begin(initialMode_.c_cc));
    }

    [[nodiscard]] auto Transcript() const -> const std::string& { return transcript_; }
    [[nodiscard]] auto TranscriptWasTruncated() const -> bool {
        return transcriptTruncated_;
    }

  private:
    auto AwaitText(const std::string_view InNeedle) -> bool {
        return AwaitTextFrom(InNeedle, 0U);
    }

    auto AwaitTextFrom(const std::string_view InNeedle,
                       const std::size_t InOffset) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            if (transcript_.find(InNeedle, InOffset) != std::string::npos) {
                return true;
            }
            (void)AwaitOutput(std::chrono::milliseconds(100));
        }
        return transcript_.find(InNeedle, InOffset) != std::string::npos;
    }

    auto AwaitOutput(const std::chrono::milliseconds InSlice = kTerminalDeadline)
        -> bool {
        const auto deadline = std::chrono::steady_clock::now() + InSlice;
        while (std::chrono::steady_clock::now() < deadline) {
            struct pollfd descriptor {master_, POLLIN | POLLHUP, 0};
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const int timeout = static_cast<int>(
                std::max<std::chrono::milliseconds::rep>(1, elapsed.count()));
            const auto polled = poll(&descriptor, 1, timeout);
            if (polled > 0) {
                const auto received = DrainAvailableOutput();
                if (received) return true;
                if ((descriptor.revents & POLLHUP) != 0) return false;
            }
            if (polled < 0 && errno != EINTR) return false;
        }
        return false;
    }

    auto DrainAvailableOutput() -> bool {
        bool received = false;
        std::array<char, 4096> buffer {};
        while (true) {
            const auto count = read(master_, buffer.data(), buffer.size());
            if (count > 0) {
                const auto bytesRead = static_cast<std::size_t>(count);
                const auto retainedCapacity =
                    kMaximumTranscriptBytes - transcript_.size();
                const auto bytesRetained = std::min(
                    bytesRead,
                    retainedCapacity);
                transcript_.append(buffer.data(), bytesRetained);
                if (bytesRetained < bytesRead) {
                    transcriptTruncated_ = true;
                }
                received = true;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
        return received;
    }

    int master_ = -1;
    int slave_ = -1;
    pid_t child_ = -1;
    struct termios initialMode_ {};
    std::string transcript_;
    std::size_t resizeTranscriptOffset_ = 0U;
    bool transcriptTruncated_ = false;
};

auto RunPosixTerminalExitSmoke(const std::string_view InInput,
                               const bool bInAcknowledgeStartupCancellation = false)
    -> void {
    const auto binary = StandaloneTuiBinary();
    REQUIRE(std::filesystem::exists(binary));
    PosixPtyProcess process(binary, bInAcknowledgeStartupCancellation);
    REQUIRE(process.AwaitFirstFrame());
    REQUIRE(process.AwaitRawInput());
    REQUIRE(process.Resize(121, 37));
    REQUIRE(process.AwaitResizedFrame(121));

    REQUIRE(process.Write(InInput));
    int exitCode = -1;
    INFO("bounded PTY transcript: " << process.Transcript());
    REQUIRE(process.WaitForExit(exitCode));
    CHECK(exitCode == 0);
    CHECK(process.TerminalWasRestored());
    CHECK(process.Transcript().size() <= kMaximumTranscriptBytes);
    CHECK_FALSE(process.TranscriptWasTruncated());
    // FTXUI intentionally disables ECHO and normal mode does not render
    // arbitrary typed text.  Prove UTF-8 through its stable production frame
    // glyph rather than inventing an input-echo contract.
    CHECK(process.Transcript().find(kProductionUtf8FrameGlyph) !=
        std::string::npos);
    CHECK(process.Transcript().find("\x1b[?1049l") != std::string::npos);
    if (bInAcknowledgeStartupCancellation) {
        CHECK(process.Transcript().find("startup cancellation acknowledged") !=
            std::string::npos);
    }
}

TEST_CASE(
    "production standalone TUI owns a POSIX terminal lifecycle",
    "[integration][tui_terminal_session][production-path][tui_pr_focus][KG-TSK-0138]") {
    SECTION("q exits after live resize and production UTF-8 rendering") {
        RunPosixTerminalExitSmoke("q", true);
    }
    SECTION("Escape exits after live resize and production UTF-8 rendering") {
        RunPosixTerminalExitSmoke("\x1b", true);
    }
}

#else

class ScopedWindowsEnvironment final {
  public:
    ScopedWindowsEnvironment(const char* InName, const char* InValue)
        : name_(InName) {
        const DWORD needed = GetEnvironmentVariableA(name_.c_str(), nullptr, 0);
        if (needed > 0) {
            previous_.resize(needed);
            (void)GetEnvironmentVariableA(
                name_.c_str(), previous_.data(), needed);
            previous_.resize(previous_.size() - 1U);
        }
        REQUIRE(SetEnvironmentVariableA(name_.c_str(), InValue));
    }

    ~ScopedWindowsEnvironment() {
        (void)SetEnvironmentVariableA(
            name_.c_str(), previous_.empty() ? nullptr : previous_.c_str());
    }

  private:
    std::string name_;
    std::string previous_;
};

class WindowsConPtyProcess final {
  public:
    WindowsConPtyProcess(const std::filesystem::path& InProgram,
                         const std::filesystem::path& InProductionBinary) {
        REQUIRE(CreatePipe(&inputRead_, &inputWrite_, nullptr, 0));
        REQUIRE(CreatePipe(&outputRead_, &outputWrite_, nullptr, 0));
        REQUIRE(SetHandleInformation(inputWrite_, HANDLE_FLAG_INHERIT, 0));
        REQUIRE(SetHandleInformation(outputRead_, HANDLE_FLAG_INHERIT, 0));
        const auto conptyResult =
            CreatePseudoConsole(COORD{100, 30}, inputRead_, outputWrite_, 0, &pseudoConsole_);
        INFO("ConPTY creation HRESULT=" << static_cast<long>(conptyResult)
             << "; KG-TSK-0138 requires a Windows 10 1809+ runner");
        REQUIRE(SUCCEEDED(conptyResult));
        CloseHandle(inputRead_); inputRead_ = nullptr;
        CloseHandle(outputWrite_); outputWrite_ = nullptr;

        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        attributes_.resize(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data());
        REQUIRE(InitializeProcThreadAttributeList(list, 1, 0, &bytes));
        REQUIRE(UpdateProcThreadAttribute(
            list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudoConsole_,
            sizeof(pseudoConsole_), nullptr, nullptr));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + InProgram.wstring() + L"\" \"" +
            InProductionBinary.wstring() + L"\"";
        REQUIRE(CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, FALSE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &process));
        process_ = process.hProcess;
        CloseHandle(process.hThread);
    }

    ~WindowsConPtyProcess() {
        if (process_ != nullptr) {
            TerminateProcess(process_, 255);
            WaitForSingleObject(process_, 1000);
            CloseHandle(process_);
        }
        if (inputRead_ != nullptr) CloseHandle(inputRead_);
        if (inputWrite_ != nullptr) CloseHandle(inputWrite_);
        if (outputWrite_ != nullptr) CloseHandle(outputWrite_);
        // Windows versions before 11 24H2 can block ClosePseudoConsole while
        // an unread output pipe remains attached.  This harness has already
        // retained/drained its bounded evidence, so close that endpoint first.
        if (outputRead_ != nullptr) CloseHandle(outputRead_);
        if (pseudoConsole_ != nullptr) ClosePseudoConsole(pseudoConsole_);
        if (!attributes_.empty()) {
            DeleteProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data()));
        }
    }

    WindowsConPtyProcess(const WindowsConPtyProcess&) = delete;
    auto operator=(const WindowsConPtyProcess&) -> WindowsConPtyProcess& = delete;

    auto AwaitFirstFrame() -> bool { return AwaitText("q/Escape exits"); }

    auto Resize(const short InColumns, const short InRows) -> bool {
        DrainAvailableOutput();
        resizeTranscriptOffset_ = transcript_.size();
        return SUCCEEDED(ResizePseudoConsole(pseudoConsole_, COORD{InColumns, InRows}));
    }

    auto AwaitResizedFrame(const std::size_t InColumns) -> bool {
        return AwaitTextFrom(
            TopBorderForColumns(InColumns),
            resizeTranscriptOffset_);
    }

    auto Write(std::string_view InBytes) -> bool {
        DWORD written = 0;
        return WriteFile(inputWrite_, InBytes.data(), static_cast<DWORD>(InBytes.size()), &written, nullptr) &&
            written == InBytes.size();
    }

    auto WaitForExit(int& OutExitCode) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            DrainAvailableOutput();
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            const auto waitMilliseconds = std::clamp<std::chrono::milliseconds::rep>(
                remaining.count(), 1, 100);
            const auto result = WaitForSingleObject(
                process_, static_cast<DWORD>(waitMilliseconds));
            if (result == WAIT_OBJECT_0) {
                DWORD code = 0;
                if (!GetExitCodeProcess(process_, &code)) return false;
                OutExitCode = static_cast<int>(code);
                CloseHandle(process_);
                process_ = nullptr;
                DrainAvailableOutput();
                return true;
            }
            if (result != WAIT_TIMEOUT) return false;
        }
        return false;
    }

    [[nodiscard]] auto Transcript() const -> const std::string& { return transcript_; }
    [[nodiscard]] auto TranscriptWasTruncated() const -> bool {
        return transcriptTruncated_;
    }

  private:
    auto AwaitText(const std::string_view InNeedle) -> bool {
        return AwaitTextFrom(InNeedle, 0U);
    }

    auto AwaitTextFrom(const std::string_view InNeedle,
                       const std::size_t InOffset) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            if (transcript_.find(InNeedle, InOffset) != std::string::npos) {
                return true;
            }
            (void)AwaitOutput(std::chrono::milliseconds(100));
        }
        return transcript_.find(InNeedle, InOffset) != std::string::npos;
    }

    auto AwaitOutput(const std::chrono::milliseconds InTimeout) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + InTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (DrainAvailableOutput()) return true;
            if (WaitForSingleObject(process_, 25) == WAIT_OBJECT_0) return false;
        }
        return false;
    }

    auto DrainAvailableOutput() -> bool {
        bool received = false;
        std::array<char, 4096> buffer{};
        DWORD available = 0;
        while (PeekNamedPipe(outputRead_, nullptr, 0, nullptr, &available, nullptr) &&
               available > 0) {
            const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(
                buffer.size(), static_cast<std::size_t>(available)));
            DWORD count = 0;
            if (!ReadFile(outputRead_, buffer.data(), wanted, &count, nullptr) || count == 0) break;
            const auto bytesRead = static_cast<std::size_t>(count);
            const auto retainedCapacity =
                kMaximumTranscriptBytes - transcript_.size();
            const auto bytesRetained = std::min(
                bytesRead,
                retainedCapacity);
            transcript_.append(buffer.data(), bytesRetained);
            if (bytesRetained < bytesRead) {
                transcriptTruncated_ = true;
            }
            received = true;
        }
        return received;
    }

    HANDLE inputRead_ = nullptr;
    HANDLE inputWrite_ = nullptr;
    HANDLE outputRead_ = nullptr;
    HANDLE outputWrite_ = nullptr;
    HPCON pseudoConsole_ = nullptr;
    HANDLE process_ = nullptr;
    std::vector<std::byte> attributes_;
    std::string transcript_;
    std::size_t resizeTranscriptOffset_ = 0U;
    bool transcriptTruncated_ = false;
};

auto RunWindowsTerminalExitSmoke(const std::string_view InInput,
                                 const bool bInAcknowledgeStartupCancellation = false)
    -> void {
    const auto binary = StandaloneTuiBinary();
    REQUIRE(std::filesystem::exists(binary));
    const auto wrapper = kano::git::tests::functional::ResolveKogBinaryPath().parent_path() /
        "kano_git_tui_terminal_state_wrapper.exe";
    INFO("Windows terminal lifecycle wrapper: " << wrapper.string());
    REQUIRE(std::filesystem::exists(wrapper));
    const std::optional<ScopedWindowsEnvironment> testMode =
        bInAcknowledgeStartupCancellation
        ? std::optional<ScopedWindowsEnvironment>(
              std::in_place, "KOG_TEST_MODE", "1")
        : std::nullopt;
    const std::optional<ScopedWindowsEnvironment> cancellationHarness =
        bInAcknowledgeStartupCancellation
        ? std::optional<ScopedWindowsEnvironment>(
              std::in_place, "KOG_TUI_TEST_STARTUP_CANCEL_ACK", "1")
        : std::nullopt;
    WindowsConPtyProcess process(wrapper, binary);
    REQUIRE(process.AwaitFirstFrame());
    REQUIRE(process.Resize(121, 37));
    REQUIRE(process.AwaitResizedFrame(121));
    REQUIRE(process.Write(InInput));
    int exitCode = -1;
    INFO("bounded ConPTY transcript: " << process.Transcript());
    REQUIRE(process.WaitForExit(exitCode));
    CHECK(exitCode == 0);
    CHECK(process.Transcript().size() <= kMaximumTranscriptBytes);
    CHECK_FALSE(process.TranscriptWasTruncated());
    CHECK(process.Transcript().find(kProductionUtf8FrameGlyph) !=
        std::string::npos);
    CHECK(process.Transcript().find("\x1b[?1049l") != std::string::npos);
    CHECK(process.Transcript().find("KOG_TUI_TERMINAL_STATE_RESTORED") !=
        std::string::npos);
    if (bInAcknowledgeStartupCancellation) {
        CHECK(process.Transcript().find("startup cancellation acknowledged") !=
            std::string::npos);
    }
}

TEST_CASE(
    "production standalone TUI owns a Windows ConPTY terminal lifecycle",
    "[integration][tui_terminal_session][production-path][tui_pr_focus][KG-TSK-0138]") {
    SECTION("q exits after live resize and production UTF-8 rendering") {
        RunWindowsTerminalExitSmoke("q", true);
    }
    SECTION("Escape exits after live resize and production UTF-8 rendering") {
        RunWindowsTerminalExitSmoke("\x1b", true);
    }
}

#endif

} // namespace
