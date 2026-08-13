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
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

class StreamingNeedle final {
  public:
    explicit StreamingNeedle(std::string InNeedle = {}) {
        Reset(std::move(InNeedle));
    }

    auto Reset(std::string InNeedle) -> void {
        needle_ = std::move(InNeedle);
        failure_.assign(needle_.size(), 0U);
        for (std::size_t index = 1U, prefix = 0U; index < needle_.size(); ++index) {
            while (prefix > 0U && needle_[index] != needle_[prefix]) {
                prefix = failure_[prefix - 1U];
            }
            if (needle_[index] == needle_[prefix]) ++prefix;
            failure_[index] = prefix;
        }
        matched_ = 0U;
        found_ = needle_.empty();
    }

    auto Consume(const std::string_view InBytes) -> void {
        if (found_) return;
        for (const char byte : InBytes) {
            while (matched_ > 0U && byte != needle_[matched_]) {
                matched_ = failure_[matched_ - 1U];
            }
            if (byte == needle_[matched_]) ++matched_;
            if (matched_ == needle_.size()) {
                found_ = true;
                return;
            }
        }
    }

    [[nodiscard]] auto Found() const -> bool { return found_; }

  private:
    std::string needle_;
    std::vector<std::size_t> failure_;
    std::size_t matched_ = 0U;
    bool found_ = false;
};

class BoundedDiagnosticTranscript final {
  public:
    auto Append(const std::string_view InBytes) -> void {
        totalBytes_ += InBytes.size();
        auto remaining = InBytes;
        const auto headAvailable = kHeadBytes - head_.size();
        const auto headBytes = std::min(headAvailable, remaining.size());
        head_.append(remaining.data(), headBytes);
        remaining.remove_prefix(headBytes);
        if (remaining.empty()) return;

        if (remaining.size() >= kTailBytes) {
            tail_.assign(
                remaining.data() + (remaining.size() - kTailBytes),
                kTailBytes);
            return;
        }
        const auto overflow = tail_.size() + remaining.size() > kTailBytes
            ? tail_.size() + remaining.size() - kTailBytes
            : 0U;
        if (overflow > 0U) tail_.erase(0U, overflow);
        tail_.append(remaining.data(), remaining.size());
    }

    [[nodiscard]] auto Snapshot() const -> std::string {
        std::string result;
        result.reserve(std::min(totalBytes_, kMaximumTranscriptBytes));
        result += head_;
        const auto omitted = OmittedBytes();
        if (omitted > 0U) {
            result += "\r\n... [KOG terminal diagnostic omitted ";
            result += std::to_string(omitted);
            result += " bytes] ...\r\n";
        }
        result += tail_;
        return result;
    }

    [[nodiscard]] auto TotalBytes() const -> std::size_t { return totalBytes_; }
    [[nodiscard]] auto OmittedBytes() const -> std::size_t {
        return totalBytes_ - head_.size() - tail_.size();
    }
    [[nodiscard]] auto WasTruncated() const -> bool { return OmittedBytes() > 0U; }

  private:
    static constexpr std::size_t kOmissionMarkerReserve = 256U;
    static constexpr std::size_t kHeadBytes = kMaximumTranscriptBytes / 2U;
    static constexpr std::size_t kTailBytes =
        kMaximumTranscriptBytes - kHeadBytes - kOmissionMarkerReserve;
    static_assert(kTailBytes > 0U);

    std::string head_;
    std::string tail_;
    std::size_t totalBytes_ = 0U;
};

class TerminalSemanticEvidence final {
  public:
    using OperationId = std::size_t;

    TerminalSemanticEvidence()
        : firstFrame_("q/Escape exits"),
          productionFrame_(std::string(kProductionUtf8FrameGlyph)) {}

    auto BeginResize(const std::size_t InColumns) -> OperationId {
        const auto operation = ++nextOperation_;
        resizeColumns_ = InColumns;
        resizedFrame_.Reset(TopBorderForColumns(InColumns));
        pendingResize_ = operation;
        committedResize_.reset();
        return operation;
    }

    auto CommitResize(const OperationId InOperation) -> void {
        if (pendingResize_ != InOperation) return;
        committedResize_ = InOperation;
        pendingResize_.reset();
    }

    auto CancelResize(const OperationId InOperation) -> void {
        if (pendingResize_ != InOperation) return;
        pendingResize_.reset();
        committedResize_.reset();
        resizedFrame_.Reset({});
    }

    auto BeginExit() -> OperationId {
        const auto operation = ++nextOperation_;
        altScreenExit_.Reset("\x1b[?1049l");
        cancellationAcknowledgement_.Reset("startup cancellation acknowledged");
        terminalStateRestored_.Reset("KOG_TUI_TERMINAL_STATE_RESTORED");
        pendingExit_ = operation;
        committedExit_.reset();
        return operation;
    }

    auto CommitExit(const OperationId InOperation) -> void {
        if (pendingExit_ != InOperation) return;
        committedExit_ = InOperation;
        pendingExit_.reset();
    }

    auto CancelExit(const OperationId InOperation) -> void {
        if (pendingExit_ != InOperation) return;
        pendingExit_.reset();
        committedExit_.reset();
        altScreenExit_.Reset({});
        cancellationAcknowledgement_.Reset({});
        terminalStateRestored_.Reset({});
    }

    auto Consume(const std::string_view InBytes) -> void {
        firstFrame_.Consume(InBytes);
        productionFrame_.Consume(InBytes);
        if (pendingResize_ || committedResize_) resizedFrame_.Consume(InBytes);
        if (pendingExit_ || committedExit_) {
            altScreenExit_.Consume(InBytes);
            cancellationAcknowledgement_.Consume(InBytes);
            terminalStateRestored_.Consume(InBytes);
        }
    }

    [[nodiscard]] auto SawFirstFrame() const -> bool {
        return firstFrame_.Found();
    }
    [[nodiscard]] auto SawProductionFrame() const -> bool {
        return productionFrame_.Found();
    }
    [[nodiscard]] auto SawResizedFrame(const std::size_t InColumns) const -> bool {
        return committedResize_.has_value() && resizeColumns_ == InColumns &&
            resizedFrame_.Found();
    }
    [[nodiscard]] auto SawAltScreenExit() const -> bool {
        return committedExit_.has_value() && altScreenExit_.Found();
    }
    [[nodiscard]] auto SawCancellationAcknowledgement() const -> bool {
        return committedExit_.has_value() && cancellationAcknowledgement_.Found();
    }
    [[nodiscard]] auto SawTerminalStateRestored() const -> bool {
        return committedExit_.has_value() && terminalStateRestored_.Found();
    }

  private:
    StreamingNeedle firstFrame_;
    StreamingNeedle productionFrame_;
    StreamingNeedle resizedFrame_;
    StreamingNeedle altScreenExit_;
    StreamingNeedle cancellationAcknowledgement_;
    StreamingNeedle terminalStateRestored_;
    std::size_t resizeColumns_ = 0U;
    OperationId nextOperation_ = 0U;
    std::optional<OperationId> pendingResize_;
    std::optional<OperationId> committedResize_;
    std::optional<OperationId> pendingExit_;
    std::optional<OperationId> committedExit_;
};

TEST_CASE(
    "terminal output evidence matches across arbitrary read boundaries",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    StreamingNeedle marker("KOG_TUI_TERMINAL_STATE_RESTORED");
    marker.Consume("noise-KOG_TUI_TERMINAL_");
    CHECK_FALSE(marker.Found());
    marker.Consume("STATE_REST");
    CHECK_FALSE(marker.Found());
    marker.Consume("ORED-tail");
    CHECK(marker.Found());
}

TEST_CASE(
    "terminal semantic evidence matches every lifecycle marker across chunks",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    TerminalSemanticEvidence evidence;
    evidence.Consume("q/Esca");
    evidence.Consume("pe exits");
    evidence.Consume("\xE2");
    evidence.Consume("\x95\xAD");
    CHECK(evidence.SawFirstFrame());
    CHECK(evidence.SawProductionFrame());

    const auto resize = evidence.BeginResize(121U);
    evidence.CommitResize(resize);
    const auto exit = evidence.BeginExit();
    evidence.CommitExit(exit);
    const auto border = TopBorderForColumns(121U);
    evidence.Consume(border.substr(0U, border.size() / 2U));
    evidence.Consume(border.substr(border.size() / 2U));
    evidence.Consume("\x1b[?104");
    evidence.Consume("9lstartup cancellation acknow");
    evidence.Consume("ledgedKOG_TUI_TERMINAL_STATE_RES");
    evidence.Consume("TORED");
    CHECK(evidence.SawResizedFrame(121U));
    CHECK(evidence.SawAltScreenExit());
    CHECK(evidence.SawCancellationAcknowledgement());
    CHECK(evidence.SawTerminalStateRestored());
}

TEST_CASE(
    "bounded terminal diagnostics preserve both ends with truthful omission metadata",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    BoundedDiagnosticTranscript transcript;
    transcript.Append("HEAD-MARKER");
    transcript.Append(std::string(kMaximumTranscriptBytes, 'x'));
    transcript.Append("TAIL-MARKER");

    const auto snapshot = transcript.Snapshot();
    REQUIRE(transcript.WasTruncated());
    CHECK(transcript.OmittedBytes() > 0U);
    CHECK(snapshot.size() <= kMaximumTranscriptBytes);
    CHECK(snapshot.starts_with("HEAD-MARKER"));
    CHECK(snapshot.ends_with("TAIL-MARKER"));
    CHECK(snapshot.find(
        "omitted " + std::to_string(transcript.OmittedBytes()) + " bytes") !=
        std::string::npos);
}

TEST_CASE(
    "semantic evidence survives when its marker is omitted from bounded diagnostics",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    constexpr std::string_view marker = "KOG_TUI_TERMINAL_STATE_RESTORED";
    BoundedDiagnosticTranscript transcript;
    TerminalSemanticEvidence evidence;
    const auto exit = evidence.BeginExit();
    evidence.CommitExit(exit);

    const std::string head(kMaximumTranscriptBytes / 2U, 'h');
    const std::string gap(kMaximumTranscriptBytes, 'g');
    const std::string tail(kMaximumTranscriptBytes, 't');
    for (const std::string_view chunk : {
             std::string_view(head), std::string_view(gap), marker,
             std::string_view(tail)}) {
        transcript.Append(chunk);
        evidence.Consume(chunk);
    }

    const auto snapshot = transcript.Snapshot();
    const auto expectedTotal = head.size() + gap.size() + marker.size() + tail.size();
    const auto expectedRetainedData = kMaximumTranscriptBytes - 256U;
    CHECK(transcript.TotalBytes() == expectedTotal);
    CHECK(transcript.OmittedBytes() == expectedTotal - expectedRetainedData);
    CHECK(snapshot.size() <= kMaximumTranscriptBytes);
    CHECK(snapshot.find(marker) == std::string::npos);
    CHECK(evidence.SawTerminalStateRestored());
}

TEST_CASE(
    "terminal semantic evidence does not synthesize matches across omitted gaps",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    StreamingNeedle marker("ABC");
    marker.Consume("prefix-A");
    marker.Consume(std::string(kMaximumTranscriptBytes + 1U, 'x'));
    marker.Consume("BC-suffix");
    CHECK_FALSE(marker.Found());
}

TEST_CASE(
    "terminal semantic evidence ignores resize and exit markers before arming",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    TerminalSemanticEvidence evidence;
    const auto resizedBorder = TopBorderForColumns(121U);
    evidence.Consume(resizedBorder);
    evidence.Consume("\x1b[?1049l");
    evidence.Consume("KOG_TUI_TERMINAL_STATE_RESTORED");
    CHECK_FALSE(evidence.SawResizedFrame(121U));
    CHECK_FALSE(evidence.SawAltScreenExit());
    CHECK_FALSE(evidence.SawTerminalStateRestored());

    const auto resize = evidence.BeginResize(121U);
    evidence.CommitResize(resize);
    const auto exit = evidence.BeginExit();
    evidence.CommitExit(exit);
    CHECK_FALSE(evidence.SawResizedFrame(121U));
    CHECK_FALSE(evidence.SawAltScreenExit());
    CHECK_FALSE(evidence.SawTerminalStateRestored());
    evidence.Consume(resizedBorder.substr(0U, 7U));
    evidence.Consume(resizedBorder.substr(7U));
    evidence.Consume("\x1b[?10");
    evidence.Consume("49lKOG_TUI_TERMINAL_STATE_");
    evidence.Consume("RESTORED");
    CHECK(evidence.SawResizedFrame(121U));
    CHECK(evidence.SawAltScreenExit());
    CHECK(evidence.SawTerminalStateRestored());
}

TEST_CASE(
    "terminal semantic evidence exposes pending bytes only after commit",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    TerminalSemanticEvidence evidence;
    const auto resize = evidence.BeginResize(121U);
    const auto exit = evidence.BeginExit();
    evidence.Consume(TopBorderForColumns(121U));
    evidence.Consume("\x1b[?1049lKOG_TUI_TERMINAL_STATE_RESTORED");
    CHECK_FALSE(evidence.SawResizedFrame(121U));
    CHECK_FALSE(evidence.SawAltScreenExit());
    CHECK_FALSE(evidence.SawTerminalStateRestored());

    evidence.CommitResize(resize);
    evidence.CommitExit(exit);
    CHECK(evidence.SawResizedFrame(121U));
    CHECK(evidence.SawAltScreenExit());
    CHECK(evidence.SawTerminalStateRestored());

    const auto cancelledResize = evidence.BeginResize(80U);
    const auto cancelledExit = evidence.BeginExit();
    evidence.Consume(TopBorderForColumns(80U));
    evidence.Consume("\x1b[?1049lKOG_TUI_TERMINAL_STATE_RESTORED");
    evidence.CancelResize(cancelledResize);
    evidence.CancelExit(cancelledExit);
    CHECK_FALSE(evidence.SawResizedFrame(80U));
    CHECK_FALSE(evidence.SawAltScreenExit());
    CHECK_FALSE(evidence.SawTerminalStateRestored());
}

#if !defined(_WIN32)

enum class PosixOutputCompletion { Running, EndObserved, CleanEof, ReadError };

constexpr auto ObservePosixOutputEnd(
    const PosixOutputCompletion InCompletion,
    const bool bInChildClosed) -> PosixOutputCompletion {
    if (InCompletion == PosixOutputCompletion::ReadError) return InCompletion;
    return bInChildClosed
        ? PosixOutputCompletion::CleanEof
        : PosixOutputCompletion::EndObserved;
}

constexpr auto ObservePosixChildClosed(
    const PosixOutputCompletion InCompletion) -> PosixOutputCompletion {
    return InCompletion == PosixOutputCompletion::EndObserved
        ? PosixOutputCompletion::CleanEof
        : InCompletion;
}

TEST_CASE(
    "POSIX terminal output end becomes clean only after child closure",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    auto completion = ObservePosixOutputEnd(
        PosixOutputCompletion::Running, false);
    CHECK(completion == PosixOutputCompletion::EndObserved);
    completion = ObservePosixChildClosed(completion);
    CHECK(completion == PosixOutputCompletion::CleanEof);
    CHECK(ObservePosixOutputEnd(PosixOutputCompletion::Running, true) ==
        PosixOutputCompletion::CleanEof);
    CHECK(ObservePosixOutputEnd(PosixOutputCompletion::ReadError, true) ==
        PosixOutputCompletion::ReadError);
}

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
        return AwaitEvidence([](const TerminalSemanticEvidence& InEvidence) {
            return InEvidence.SawFirstFrame();
        });
    }

    auto Resize(const unsigned short InColumns, const unsigned short InRows)
        -> bool {
        DrainAvailableOutput();
        struct winsize size {};
        size.ws_col = InColumns;
        size.ws_row = InRows;
        if (ioctl(master_, TIOCSWINSZ, &size) != 0) return false;
        const auto operation = evidence_.BeginResize(
            static_cast<std::size_t>(InColumns));
        evidence_.CommitResize(operation);
        return true;
    }

    auto AwaitResizedFrame(const std::size_t InColumns) -> bool {
        return AwaitEvidence([InColumns](const TerminalSemanticEvidence& InEvidence) {
            return InEvidence.SawResizedFrame(InColumns);
        });
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
        const auto operation = evidence_.BeginExit();
        evidence_.CommitExit(operation);
        return true;
    }

    auto WaitForExit(int& OutExitCode) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const auto result = waitpid(child_, &status, WNOHANG);
            if (result == child_) {
                child_ = -1;
                childClosed_ = true;
                OutExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                outputCompletion_ = ObservePosixChildClosed(outputCompletion_);
                (void)DrainAvailableOutput();
                while (std::chrono::steady_clock::now() < deadline &&
                       (outputCompletion_ == PosixOutputCompletion::Running ||
                        outputCompletion_ == PosixOutputCompletion::EndObserved)) {
                    (void)AwaitOutput(std::chrono::milliseconds(100));
                }
                return outputCompletion_ == PosixOutputCompletion::CleanEof;
            }
            if (result < 0 && errno != EINTR) return false;
            if (outputCompletion_ == PosixOutputCompletion::ReadError) return false;
            (void)DrainAvailableOutput();
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

    [[nodiscard]] auto Transcript() const -> std::string {
        return transcript_.Snapshot();
    }
    [[nodiscard]] auto TranscriptWasTruncated() const -> bool {
        return transcript_.WasTruncated();
    }
    [[nodiscard]] auto TranscriptTotalBytes() const -> std::size_t {
        return transcript_.TotalBytes();
    }
    [[nodiscard]] auto TranscriptOmittedBytes() const -> std::size_t {
        return transcript_.OmittedBytes();
    }
    [[nodiscard]] auto SawProductionFrame() const -> bool {
        return evidence_.SawProductionFrame();
    }
    [[nodiscard]] auto SawAltScreenExit() const -> bool {
        return evidence_.SawAltScreenExit();
    }
    [[nodiscard]] auto SawCancellationAcknowledgement() const -> bool {
        return evidence_.SawCancellationAcknowledgement();
    }
    [[nodiscard]] auto OutputReadError() const -> int { return outputReadError_; }

  private:
    template <typename Predicate>
    auto AwaitEvidence(Predicate InPredicate) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + kTerminalDeadline;
        while (std::chrono::steady_clock::now() < deadline) {
            if (InPredicate(evidence_)) return true;
            (void)AwaitOutput(std::chrono::milliseconds(100));
        }
        return InPredicate(evidence_);
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
                if (outputCompletion_ != PosixOutputCompletion::Running) return false;
            }
            if (polled < 0 && errno != EINTR) {
                outputReadError_ = errno;
                outputCompletion_ = PosixOutputCompletion::ReadError;
                return false;
            }
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
                const std::string_view chunk(buffer.data(), bytesRead);
                transcript_.Append(chunk);
                evidence_.Consume(chunk);
                received = true;
                continue;
            }
            if (count == 0) {
                outputReadError_ = 0;
                outputCompletion_ = ObservePosixOutputEnd(
                    outputCompletion_, childClosed_);
                break;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
#if defined(__linux__)
            if (count < 0 && errno == EIO) {
                outputReadError_ = errno;
                outputCompletion_ = ObservePosixOutputEnd(
                    outputCompletion_, childClosed_);
                break;
            }
#endif
            if (count < 0) {
                outputReadError_ = errno;
                outputCompletion_ = PosixOutputCompletion::ReadError;
            }
            break;
        }
        return received;
    }

    int master_ = -1;
    int slave_ = -1;
    pid_t child_ = -1;
    struct termios initialMode_ {};
    BoundedDiagnosticTranscript transcript_;
    TerminalSemanticEvidence evidence_;
    PosixOutputCompletion outputCompletion_ = PosixOutputCompletion::Running;
    int outputReadError_ = 0;
    bool childClosed_ = false;
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
    const bool exited = process.WaitForExit(exitCode);
    const auto transcript = process.Transcript();
    INFO("bounded PTY transcript: total=" << process.TranscriptTotalBytes()
         << "; omitted=" << process.TranscriptOmittedBytes()
         << "; truncated=" << process.TranscriptWasTruncated()
         << "; output-read-error=" << process.OutputReadError()
         << "\n" << transcript);
    REQUIRE(exited);
    CHECK(exitCode == 0);
    CHECK(process.TerminalWasRestored());
    CHECK(transcript.size() <= kMaximumTranscriptBytes);
    // FTXUI intentionally disables ECHO and normal mode does not render
    // arbitrary typed text.  Prove UTF-8 through its stable production frame
    // glyph rather than inventing an input-echo contract.
    CHECK(process.SawProductionFrame());
    CHECK(process.SawAltScreenExit());
    if (bInAcknowledgeStartupCancellation) {
        CHECK(process.SawCancellationAcknowledgement());
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

constexpr std::string_view kWindowsHostSuccess =
    "KOG_CONPTY_HOST/v1 result=ok code=0 win32=0 child_exit=0 output_eof=1";
constexpr std::string_view kWindowsHostBeforeClose =
    "KOG_CONPTY_HOST/v1 stage=before-close";

auto QuoteWindowsArgument(const std::wstring& InValue) -> std::wstring {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0U;
    for (const wchar_t character : InValue) {
        if (character == L'\\') ++slashes;
        else if (character == L'\"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(character);
            slashes = 0U;
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
            slashes = 0U;
        }
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

enum class WindowsHostRecordKind { Progress, Success, Error, Malformed };

auto ClassifyWindowsHostRecord(const std::string_view InLine)
    -> WindowsHostRecordKind {
    if (InLine == kWindowsHostBeforeClose) return WindowsHostRecordKind::Progress;
    if (InLine == kWindowsHostSuccess) return WindowsHostRecordKind::Success;
    if (InLine.starts_with("KOG_CONPTY_HOST/v1 result=error ")) {
        return WindowsHostRecordKind::Error;
    }
    return WindowsHostRecordKind::Malformed;
}

class ScopedWindowsEnvironment final {
  public:
    ScopedWindowsEnvironment(const char* InName, const char* InValue)
        : name_(InName) {
        const DWORD needed = GetEnvironmentVariableA(name_.c_str(), nullptr, 0);
        if (needed > 0U) {
            previous_.resize(needed);
            (void)GetEnvironmentVariableA(name_.c_str(), previous_.data(), needed);
            previous_.resize(previous_.size() - 1U);
            hadPrevious_ = true;
        }
        REQUIRE(SetEnvironmentVariableA(name_.c_str(), InValue));
    }

    ScopedWindowsEnvironment(const ScopedWindowsEnvironment&) = delete;
    auto operator=(const ScopedWindowsEnvironment&)
        -> ScopedWindowsEnvironment& = delete;
    ScopedWindowsEnvironment(ScopedWindowsEnvironment&&) = delete;
    auto operator=(ScopedWindowsEnvironment&&)
        -> ScopedWindowsEnvironment& = delete;

    ~ScopedWindowsEnvironment() {
        (void)SetEnvironmentVariableA(name_.c_str(),
            hadPrevious_ ? previous_.c_str() : nullptr);
    }

  private:
    std::string name_;
    std::string previous_;
    bool hadPrevious_ = false;
};

class WindowsHandle final {
  public:
    WindowsHandle() = default;
    explicit WindowsHandle(const HANDLE InValue) : value_(InValue) {}
    ~WindowsHandle() { Reset(); }
    WindowsHandle(const WindowsHandle&) = delete;
    auto operator=(const WindowsHandle&) -> WindowsHandle& = delete;
    WindowsHandle(WindowsHandle&& InOther) noexcept
        : value_(InOther.Release()) {}
    auto operator=(WindowsHandle&& InOther) noexcept -> WindowsHandle& {
        if (this != &InOther) Reset(InOther.Release());
        return *this;
    }
    [[nodiscard]] auto Get() const -> HANDLE { return value_; }
    [[nodiscard]] auto Release() -> HANDLE {
        const auto result = value_;
        value_ = nullptr;
        return result;
    }
    auto Reset(const HANDLE InValue = nullptr) -> void {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
        value_ = InValue;
    }

  private:
    HANDLE value_ = nullptr;
};

class WindowsHostStatus final {
  public:
    auto Append(const std::string_view InBytes) -> void {
        if (bytes_.size() + InBytes.size() > kMaximumBytes) {
            overflow_ = true;
            return;
        }
        bytes_.append(InBytes.data(), InBytes.size());
        while (true) {
            const auto newline = bytes_.find('\n', parsedBytes_);
            if (newline == std::string::npos) break;
            auto line = std::string_view(bytes_).substr(
                parsedBytes_, newline - parsedBytes_);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
            const auto kind = ClassifyWindowsHostRecord(line);
            if (kind == WindowsHostRecordKind::Progress) ++progressRecords_;
            else if (kind == WindowsHostRecordKind::Success) ++successRecords_;
            else if (kind == WindowsHostRecordKind::Error) ++errorRecords_;
            else ++malformedRecords_;
            parsedBytes_ = newline + 1U;
        }
    }

    auto Finish() -> void {
        if (parsedBytes_ != bytes_.size()) ++malformedRecords_;
    }
    [[nodiscard]] auto SawBeforeClose() const -> bool {
        return progressRecords_ == 1U;
    }
    [[nodiscard]] auto IsStrictBeforeCloseOnly() const -> bool {
        return !overflow_ && progressRecords_ == 1U && successRecords_ == 0U &&
            errorRecords_ == 0U && malformedRecords_ == 0U;
    }
    [[nodiscard]] auto IsStrictSuccess() const -> bool {
        return !overflow_ && progressRecords_ == 0U && successRecords_ == 1U &&
            errorRecords_ == 0U && malformedRecords_ == 0U;
    }
    [[nodiscard]] auto Snapshot() const -> std::string { return bytes_; }

  private:
    static constexpr std::size_t kMaximumBytes = 16U * 1024U;
    std::string bytes_;
    std::size_t parsedBytes_ = 0U;
    std::size_t progressRecords_ = 0U;
    std::size_t successRecords_ = 0U;
    std::size_t errorRecords_ = 0U;
    std::size_t malformedRecords_ = 0U;
    bool overflow_ = false;
};

TEST_CASE(
    "Windows ConPTY host success status is exact and unique",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    WindowsHostStatus success;
    success.Append(std::string(kWindowsHostSuccess) + "\n");
    success.Finish();
    CHECK(success.IsStrictSuccess());
}

TEST_CASE(
    "Windows ConPTY host status parser rejects incomplete or ambiguous records",
    "[unit][tui_terminal_session][tui_pr_focus][KG-TSK-0138]") {
    WindowsHostStatus missing;
    missing.Finish();
    CHECK_FALSE(missing.IsStrictSuccess());

    WindowsHostStatus duplicate;
    duplicate.Append(std::string(kWindowsHostSuccess) + "\n" +
        std::string(kWindowsHostSuccess) + "\n");
    duplicate.Finish();
    CHECK_FALSE(duplicate.IsStrictSuccess());

    WindowsHostStatus malformed;
    malformed.Append("KOG_CONPTY_HOST/v1 result=ok code=0\n");
    malformed.Finish();
    CHECK_FALSE(malformed.IsStrictSuccess());

    WindowsHostStatus error;
    error.Append(
        "KOG_CONPTY_HOST/v1 result=error code=11 win32=1 child_exit=259 output_eof=0\n");
    error.Finish();
    CHECK_FALSE(error.IsStrictSuccess());

    WindowsHostStatus strictStall;
    strictStall.Append(std::string(kWindowsHostBeforeClose) + "\n");
    strictStall.Finish();
    CHECK(strictStall.IsStrictBeforeCloseOnly());

    WindowsHostStatus extraStall;
    extraStall.Append(std::string(kWindowsHostBeforeClose) + "\n" +
        std::string(kWindowsHostBeforeClose) + "\n");
    extraStall.Finish();
    CHECK_FALSE(extraStall.IsStrictBeforeCloseOnly());
}

enum class WindowsHostOutcome { Success, Failed, KilledAtBeforeClose };

struct WindowsHostLaunchResources final {
    ~WindowsHostLaunchResources() {
        if (process.Get() != nullptr) {
            DWORD exitCode = STILL_ACTIVE;
            if (GetExitCodeProcess(process.Get(), &exitCode) &&
                exitCode == STILL_ACTIVE) {
                (void)TerminateProcess(process.Get(), 0x0138U);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                const auto remaining = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now);
                (void)WaitForSingleObject(process.Get(), static_cast<DWORD>(
                    std::max<std::chrono::milliseconds::rep>(0, remaining.count())));
            }
        }
        if (attributesInitialized) {
            DeleteProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data()));
        }
    }
    WindowsHandle stdoutRead;
    WindowsHandle stdoutWrite;
    WindowsHandle stderrRead;
    WindowsHandle stderrWrite;
    WindowsHandle stdinNull;
    WindowsHandle job;
    WindowsHandle process;
    WindowsHandle thread;
    std::vector<std::byte> attributes;
    bool attributesInitialized = false;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + kTerminalDeadline;
};

class WindowsConPtyHostController final {
  public:
    WindowsConPtyHostController(
        const std::filesystem::path& InHost,
        const std::filesystem::path& InWrapper,
        const std::filesystem::path& InProduction,
        const bool bInEscape,
        const bool bInStallBeforeClose)
        : deadline_(std::chrono::steady_clock::now() + kTerminalDeadline) {
        WindowsHostLaunchResources resources;
        resources.deadline = deadline_;
        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE stderrWrite = nullptr;
        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof(inheritable);
        inheritable.bInheritHandle = TRUE;
        REQUIRE(CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0));
        resources.stdoutRead.Reset(stdoutRead);
        resources.stdoutWrite.Reset(stdoutWrite);
        REQUIRE(CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0));
        resources.stderrRead.Reset(stderrRead);
        resources.stderrWrite.Reset(stderrWrite);
        REQUIRE(SetHandleInformation(resources.stdoutRead.Get(), HANDLE_FLAG_INHERIT, 0));
        REQUIRE(SetHandleInformation(resources.stderrRead.Get(), HANDLE_FLAG_INHERIT, 0));
        resources.stdinNull.Reset(CreateFileW(L"NUL", GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE(resources.stdinNull.Get() != INVALID_HANDLE_VALUE);
        REQUIRE(resources.stdinNull.Get() != nullptr);

        resources.job.Reset(CreateJobObjectW(nullptr, nullptr));
        REQUIRE(resources.job.Get() != nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        REQUIRE(SetInformationJobObject(resources.job.Get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)));

        SIZE_T bytes = 0U;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        resources.attributes.resize(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            resources.attributes.data());
        REQUIRE(bytes > 0U);
        REQUIRE(InitializeProcThreadAttributeList(list, 1, 0, &bytes));
        resources.attributesInitialized = true;
        HANDLE inherited[] = {resources.stdinNull.Get(), resources.stdoutWrite.Get(),
            resources.stderrWrite.Get()};
        REQUIRE(UpdateProcThreadAttribute(list, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
            nullptr, nullptr));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = resources.stdinNull.Get();
        startup.StartupInfo.hStdOutput = resources.stdoutWrite.Get();
        startup.StartupInfo.hStdError = resources.stderrWrite.Get();
        startup.lpAttributeList = list;
        std::wstring command = QuoteWindowsArgument(InHost.wstring()) + L" " +
            (bInEscape ? L"escape" : L"q") + L" " +
            QuoteWindowsArgument(InWrapper.wstring()) + L" " +
            QuoteWindowsArgument(InProduction.wstring());
        if (bInStallBeforeClose) command += L" --test-stall-before-close";
        PROCESS_INFORMATION process{};
        REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &process));
        resources.process.Reset(process.hProcess);
        resources.thread.Reset(process.hThread);
        REQUIRE(AssignProcessToJobObject(resources.job.Get(), resources.process.Get()));
        REQUIRE(ResumeThread(resources.thread.Get()) != static_cast<DWORD>(-1));
        resources.thread.Reset();

        stdoutRead_.Reset(resources.stdoutRead.Release());
        stdoutWrite_.Reset(resources.stdoutWrite.Release());
        stderrRead_.Reset(resources.stderrRead.Release());
        stderrWrite_.Reset(resources.stderrWrite.Release());
        stdinNull_.Reset(resources.stdinNull.Release());
        job_.Reset(resources.job.Release());
        process_.Reset(resources.process.Release());
        attributes_ = std::move(resources.attributes);
        attributesInitialized_ = resources.attributesInitialized;
        resources.attributesInitialized = false;
        stdoutWrite_.Reset();
        stderrWrite_.Reset();
        stdinNull_.Reset();
        const auto resize = evidence_.BeginResize(121U);
        evidence_.CommitResize(resize);
    }

    ~WindowsConPtyHostController() { CleanupWithinDeadline(); }

    WindowsConPtyHostController(const WindowsConPtyHostController&) = delete;
    auto operator=(const WindowsConPtyHostController&)
        -> WindowsConPtyHostController& = delete;

    auto Run(const bool bInKillAtBeforeClose) -> WindowsHostOutcome {
        constexpr auto kCleanupReserve = 500ms;
        const auto runDeadline = deadline_ - kCleanupReserve;
        bool killedAtStage = false;
        bool terminationIssued = false;
        bool ioOk = true;
        while (std::chrono::steady_clock::now() < runDeadline) {
            if (!DrainPipe(stdoutRead_, true, runDeadline) ||
                !DrainPipe(stderrRead_, false, runDeadline)) {
                ioOk = false;
                break;
            }
            if (bInKillAtBeforeClose && status_.SawBeforeClose()) {
                killedAtStage = true;
                if (job_.Get() == nullptr ||
                    !TerminateJobObject(job_.Get(), 0x0138U)) {
                    ioOk = false;
                    break;
                }
                terminationIssued = true;
            }
            const auto processWait = WaitForSingleObject(process_.Get(), 0U);
            if (processWait == WAIT_OBJECT_0) processExited_ = true;
            if (processExited_ && stdoutEof_ && stderrEof_ && JobIsEmpty()) break;
            if (processWait == WAIT_FAILED) {
                ioOk = false;
                break;
            }
            (void)WaitForSingleObject(process_.Get(), 10U);
        }
        if (!processExited_ || !stdoutEof_ || !stderrEof_ || !JobIsEmpty()) {
            if (job_.Get() != nullptr) {
                if (!terminationIssued &&
                    !TerminateJobObject(job_.Get(), 0x0138U)) ioOk = false;
                terminationIssued = true;
            }
        }
        while (std::chrono::steady_clock::now() < deadline_ &&
               (!processExited_ || !stdoutEof_ || !stderrEof_ || !JobIsEmpty())) {
            if (!DrainPipe(stdoutRead_, true, deadline_) ||
                !DrainPipe(stderrRead_, false, deadline_)) ioOk = false;
            processExited_ = WaitForSingleObject(process_.Get(), 0U) == WAIT_OBJECT_0;
            if (processExited_ && stdoutEof_ && stderrEof_ && JobIsEmpty()) break;
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline_) {
                const auto remaining = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline_ - now);
                (void)WaitForSingleObject(process_.Get(), static_cast<DWORD>(
                    std::clamp<std::chrono::milliseconds::rep>(remaining.count(), 1, 10)));
            }
        }
        processExited_ = WaitForSingleObject(process_.Get(), 0U) == WAIT_OBJECT_0;
        if (!stdoutEof_ || !stderrEof_) ioOk = false;
        status_.Finish();
        DWORD exitCode = STILL_ACTIVE;
        const bool gotExit = GetExitCodeProcess(process_.Get(), &exitCode) != FALSE;
        const bool emptyJob = JobIsEmpty();
        if (ioOk && bInKillAtBeforeClose && killedAtStage && processExited_ && emptyJob &&
            stdoutEof_ && stderrEof_ && status_.IsStrictBeforeCloseOnly()) {
            outcome_ = WindowsHostOutcome::KilledAtBeforeClose;
        } else if (gotExit && exitCode == 0U && processExited_ && emptyJob &&
                   stdoutEof_ && stderrEof_ && status_.IsStrictSuccess()) {
            outcome_ = WindowsHostOutcome::Success;
        } else {
            outcome_ = WindowsHostOutcome::Failed;
        }
        return outcome_;
    }

    [[nodiscard]] auto Transcript() const -> std::string {
        return transcript_.Snapshot();
    }
    [[nodiscard]] auto Status() const -> std::string { return status_.Snapshot(); }
    [[nodiscard]] auto SawProductionFrame() const -> bool {
        return evidence_.SawProductionFrame();
    }
    [[nodiscard]] auto SawResizedFrame() const -> bool {
        return evidence_.SawResizedFrame(121U);
    }
    [[nodiscard]] auto TranscriptTotalBytes() const -> std::size_t {
        return transcript_.TotalBytes();
    }
    [[nodiscard]] auto TranscriptOmittedBytes() const -> std::size_t {
        return transcript_.OmittedBytes();
    }
    [[nodiscard]] auto JobIsEmpty() const -> bool {
        if (job_.Get() == nullptr) {
            return process_.Get() == nullptr ||
                WaitForSingleObject(process_.Get(), 0U) == WAIT_OBJECT_0;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        return QueryInformationJobObject(job_.Get(), JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0U;
    }

  private:
    auto DrainPipe(WindowsHandle& InRead, const bool bInTerminalOutput,
                   const std::chrono::steady_clock::time_point InDeadline) -> bool {
        if (InRead.Get() == nullptr) return true;
        constexpr std::size_t kMaximumChunksPerPass = 8U;
        constexpr std::size_t kMaximumBytesPerPass = 32U * 1024U;
        std::array<char, 4096> buffer{};
        DWORD available = 0U;
        std::size_t chunks = 0U;
        std::size_t bytes = 0U;
        while (chunks < kMaximumChunksPerPass && bytes < kMaximumBytesPerPass &&
               std::chrono::steady_clock::now() < InDeadline &&
               PeekNamedPipe(InRead.Get(), nullptr, 0U, nullptr, &available, nullptr)) {
            if (available == 0U) return true;
            DWORD count = 0U;
            const DWORD wanted = std::min<DWORD>(available,
                static_cast<DWORD>(buffer.size()));
            if (!ReadFile(InRead.Get(), buffer.data(), wanted, &count, nullptr)) return false;
            ++chunks;
            bytes += static_cast<std::size_t>(count);
            const std::string_view chunk(buffer.data(), count);
            if (bInTerminalOutput) {
                transcript_.Append(chunk);
                evidence_.Consume(chunk);
            } else status_.Append(chunk);
        }
        if (chunks == kMaximumChunksPerPass || bytes >= kMaximumBytesPerPass ||
            std::chrono::steady_clock::now() >= InDeadline) return true;
        const DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF) return false;
        if (bInTerminalOutput) stdoutEof_ = true;
        else stderrEof_ = true;
        InRead.Reset();
        return true;
    }

    auto CleanupWithinDeadline() -> void {
        if (job_.Get() != nullptr && !JobIsEmpty()) {
            (void)TerminateJobObject(job_.Get(), 0x0138U);
        }
        while (job_.Get() != nullptr && !JobIsEmpty() &&
               std::chrono::steady_clock::now() < deadline_) {
            if (process_.Get() == nullptr) break;
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now());
            (void)WaitForSingleObject(process_.Get(), static_cast<DWORD>(
                std::clamp<std::chrono::milliseconds::rep>(remaining.count(), 1, 10)));
        }
        job_.Reset();
        if (attributesInitialized_) {
            DeleteProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data()));
            attributesInitialized_ = false;
        }
    }

    WindowsHandle stdoutRead_;
    WindowsHandle stdoutWrite_;
    WindowsHandle stderrRead_;
    WindowsHandle stderrWrite_;
    WindowsHandle stdinNull_;
    WindowsHandle job_;
    WindowsHandle process_;
    WindowsHandle thread_;
    std::vector<std::byte> attributes_;
    bool attributesInitialized_ = false;
    BoundedDiagnosticTranscript transcript_;
    TerminalSemanticEvidence evidence_;
    WindowsHostStatus status_;
    WindowsHostOutcome outcome_ = WindowsHostOutcome::Failed;
    bool processExited_ = false;
    bool stdoutEof_ = false;
    bool stderrEof_ = false;
    std::chrono::steady_clock::time_point deadline_;
};

auto RunWindowsTerminalExitSmoke(const bool bInEscape,
                                 const bool bInAcknowledgeStartupCancellation = false)
    -> void {
    const auto binary = StandaloneTuiBinary();
    REQUIRE(std::filesystem::exists(binary));
    const auto binaries = kano::git::tests::functional::ResolveKogBinaryPath().parent_path();
    const auto wrapper = binaries /
        "kano_git_tui_terminal_state_wrapper.exe";
    REQUIRE(std::filesystem::exists(wrapper));
    const auto host = binaries / "kano_git_tui_conpty_host.exe";
    REQUIRE(std::filesystem::exists(host));
    std::optional<ScopedWindowsEnvironment> testMode;
    std::optional<ScopedWindowsEnvironment> cancellationHarness;
    if (bInAcknowledgeStartupCancellation) {
        testMode.emplace("KOG_TEST_MODE", "1");
        cancellationHarness.emplace(
            "KOG_TUI_TEST_STARTUP_CANCEL_ACK", "1");
    }
    WindowsConPtyHostController controller(
        host, wrapper, binary, bInEscape, false);
    const auto outcome = controller.Run(false);
    const auto transcript = controller.Transcript();
    INFO("bounded ConPTY transcript: total=" << controller.TranscriptTotalBytes()
         << "; omitted=" << controller.TranscriptOmittedBytes()
         << "\n" << transcript
         << "\nhost status:\n" << controller.Status());
    REQUIRE(outcome == WindowsHostOutcome::Success);
    CHECK(transcript.size() <= kMaximumTranscriptBytes);
    CHECK(controller.SawProductionFrame());
    CHECK(controller.SawResizedFrame());
    // The isolated host validates ordered input, cancellation acknowledgement,
    // alternate-screen cleanup and RESTORED before emitting strict success.
    // The outer raw stream remains bounded corroborating diagnostics only.
}

TEST_CASE(
    "production standalone TUI owns a Windows ConPTY terminal lifecycle",
    "[integration][tui_terminal_session][production-path][tui_pr_focus][KG-TSK-0138]") {
    SECTION("q exits after live resize and production UTF-8 rendering") {
        RunWindowsTerminalExitSmoke(false, true);
    }
    SECTION("Escape exits after live resize and production UTF-8 rendering") {
        RunWindowsTerminalExitSmoke(true, true);
    }
}

TEST_CASE(
    "Windows ConPTY host stall is bounded by the controller job",
    "[integration][tui_terminal_session][production-path][tui_pr_focus][KG-TSK-0138]") {
    const ScopedWindowsEnvironment testMode("KOG_TEST_MODE", "1");
    const ScopedWindowsEnvironment cancellationHarness(
        "KOG_TUI_TEST_STARTUP_CANCEL_ACK", "1");
    const auto binaries =
        kano::git::tests::functional::ResolveKogBinaryPath().parent_path();
    WindowsConPtyHostController controller(
        binaries / "kano_git_tui_conpty_host.exe",
        binaries / "kano_git_tui_terminal_state_wrapper.exe",
        StandaloneTuiBinary(), false, true);
    const auto outcome = controller.Run(true);
    INFO("bounded ConPTY transcript: total=" << controller.TranscriptTotalBytes()
         << "; omitted=" << controller.TranscriptOmittedBytes()
         << "\n" << controller.Transcript()
         << "\nhost status:\n" << controller.Status());
    CHECK(outcome == WindowsHostOutcome::KilledAtBeforeClose);
    CHECK_FALSE(controller.Status().find(kWindowsHostSuccess) != std::string::npos);
    CHECK(controller.JobIsEmpty());
}

#endif

} // namespace
