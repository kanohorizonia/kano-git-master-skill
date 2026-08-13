#include <catch2/catch_test_macros.hpp>

#include "tui_terminal_guard.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

auto AcceptedTerminalCapabilities()
    -> kano::git::commands::TuiTerminalCapabilities {
    return {
        .bStdinInteractive = true,
        .bStdoutInteractive = true,
        .bInputStateHealthy = true,
        .bOutputStateHealthy = true,
        .columns = 120,
        .rows = 40,
    };
}

#if !defined(_WIN32)
class ScopedPosixPtyStandardStreams final {
public:
    ScopedPosixPtyStandardStreams()
        : mSavedInput(dup(STDIN_FILENO)),
          mSavedOutput(dup(STDOUT_FILENO)) {
        if (mSavedInput < 0 || mSavedOutput < 0) {
            return;
        }
        mMaster = posix_openpt(O_RDWR | O_NOCTTY);
        if (mMaster < 0 || grantpt(mMaster) != 0 || unlockpt(mMaster) != 0) {
            return;
        }
        const char* slaveName = ptsname(mMaster);
        if (slaveName == nullptr) {
            return;
        }
        mSlave = open(slaveName, O_RDWR | O_NOCTTY);
        if (mSlave < 0 || dup2(mSlave, STDIN_FILENO) < 0) {
            return;
        }
        mInputRedirected = true;
        if (dup2(mSlave, STDOUT_FILENO) < 0) {
            return;
        }
        mOutputRedirected = true;
    }

    ~ScopedPosixPtyStandardStreams() {
        if (mInputRedirected && mSavedInput >= 0) {
            dup2(mSavedInput, STDIN_FILENO);
        }
        if (mOutputRedirected && mSavedOutput >= 0) {
            dup2(mSavedOutput, STDOUT_FILENO);
        }
        if (mSavedInput >= 0) {
            close(mSavedInput);
        }
        if (mSavedOutput >= 0) {
            close(mSavedOutput);
        }
        if (mSlave >= 0) {
            close(mSlave);
        }
        if (mMaster >= 0) {
            close(mMaster);
        }
    }

    ScopedPosixPtyStandardStreams(const ScopedPosixPtyStandardStreams&) = delete;
    auto operator=(const ScopedPosixPtyStandardStreams&)
        -> ScopedPosixPtyStandardStreams& = delete;

    [[nodiscard]] auto IsReady() const -> bool {
        return mInputRedirected && mOutputRedirected;
    }

private:
    int mSavedInput = -1;
    int mSavedOutput = -1;
    int mMaster = -1;
    int mSlave = -1;
    bool mInputRedirected = false;
    bool mOutputRedirected = false;
};
#endif

}  // namespace

TEST_CASE(
    "TUI terminal guard restores captured state after the screen lifetime",
    "[unit][tui_terminal_guard][KG-BUG-0088]") {
    using kano::git::commands::TuiTerminalModeGuard;
    using kano::git::commands::TuiTerminalRestoreAction;

    int terminalMode = 7;
    std::vector<std::string> events;
    {
        TuiTerminalModeGuard guard([&]() -> TuiTerminalRestoreAction {
            const int capturedMode = terminalMode;
            events.push_back("captured");
            return [&, capturedMode]() {
                terminalMode = capturedMode;
                events.push_back("restored");
            };
        });

        terminalMode = 42;
        events.push_back("screen-loop-finished");
        REQUIRE(terminalMode == 42);
        REQUIRE(events == std::vector<std::string>{
                              "captured",
                              "screen-loop-finished",
                          });
    }

    REQUIRE(terminalMode == 7);
    REQUIRE(events == std::vector<std::string>{
                          "captured",
                          "screen-loop-finished",
                          "restored",
                      });
}

TEST_CASE(
    "TUI terminal preflight rejects unsupported capabilities before capture",
    "[unit][tui_terminal_guard][KG-BUG-0108]") {
    using kano::git::commands::StartTuiTerminalSession;
    using kano::git::commands::TuiTerminalCapabilities;
    using kano::git::commands::TuiTerminalPreflightFailure;
    using kano::git::commands::TuiTerminalRestoreAction;

    const std::vector<std::pair<TuiTerminalCapabilities, TuiTerminalPreflightFailure>> cases{
        {{}, TuiTerminalPreflightFailure::StandardStreamsNotInteractive},
        {{.bStdinInteractive = true, .bStdoutInteractive = true},
         TuiTerminalPreflightFailure::TerminalStateUnavailable},
        {{.bStdinInteractive = true, .bStdoutInteractive = true,
          .bInputStateHealthy = true, .bOutputStateHealthy = true},
         TuiTerminalPreflightFailure::TerminalDimensionsUnavailable},
    };

    for (const auto& [capabilities, expectedFailure] : cases) {
        int captureCount = 0;
        int activationCount = 0;
        const auto session = StartTuiTerminalSession(
            [capabilities] { return capabilities; },
            [&]() -> TuiTerminalRestoreAction {
                ++captureCount;
                return {};
            },
            [&]() {
                ++activationCount;
                return true;
            });
        REQUIRE(session.preflight.failure == expectedFailure);
        REQUIRE_FALSE(session.modeGuard);
        REQUIRE(captureCount == 0);
        REQUIRE(activationCount == 0);
    }
}

TEST_CASE(
    "TUI terminal session captures before activation and restores after success",
    "[unit][tui_terminal_guard][KG-BUG-0108]") {
    using kano::git::commands::StartTuiTerminalSession;
    using kano::git::commands::TuiTerminalRestoreAction;

    std::vector<std::string> events;
    {
        auto session = StartTuiTerminalSession(
            [&] {
                events.push_back("probe");
                return AcceptedTerminalCapabilities();
            },
            [&]() -> TuiTerminalRestoreAction {
                events.push_back("capture");
                return [&] { events.push_back("restore"); };
            },
            [&] {
                events.push_back("activate");
                return true;
            });
        REQUIRE(session.preflight.Accepted());
        REQUIRE(session.modeGuard);
        events.push_back("screen-entry");
        REQUIRE(events == std::vector<std::string>{
                              "probe",
                              "capture",
                              "activate",
                              "screen-entry",
                          });
    }
    REQUIRE(events == std::vector<std::string>{
                          "probe",
                          "capture",
                          "activate",
                          "screen-entry",
                          "restore",
                      });
}

TEST_CASE(
    "TUI terminal session restores before reporting guarded activation failure",
    "[unit][tui_terminal_guard][KG-BUG-0108]") {
    using kano::git::commands::StartTuiTerminalSession;
    using kano::git::commands::TuiTerminalPreflightFailure;
    using kano::git::commands::TuiTerminalRestoreAction;

    std::vector<std::string> events;
    auto session = StartTuiTerminalSession(
        [&] {
            events.push_back("probe");
            return AcceptedTerminalCapabilities();
        },
        [&]() -> TuiTerminalRestoreAction {
            events.push_back("capture");
            return [&] { events.push_back("restore"); };
        },
        [&] {
            events.push_back("activate");
            return false;
        });
    events.push_back("diagnostic");

    REQUIRE(session.preflight.failure ==
            TuiTerminalPreflightFailure::VirtualTerminalUnavailable);
    REQUIRE_FALSE(session.modeGuard);
    REQUIRE(events == std::vector<std::string>{
                          "probe",
                          "capture",
                          "activate",
                          "restore",
                          "diagnostic",
                      });
}

TEST_CASE(
    "TUI terminal session restores before reporting guarded activation exception",
    "[unit][tui_terminal_guard][KG-BUG-0108]") {
    using kano::git::commands::StartTuiTerminalSession;
    using kano::git::commands::TuiTerminalPreflightFailure;
    using kano::git::commands::TuiTerminalRestoreAction;

    std::vector<std::string> events;
    auto session = StartTuiTerminalSession(
        [&] {
            events.push_back("probe");
            return AcceptedTerminalCapabilities();
        },
        [&]() -> TuiTerminalRestoreAction {
            events.push_back("capture");
            return [&] { events.push_back("restore"); };
        },
        [&]() -> bool {
            events.push_back("activate-mutate");
            throw std::runtime_error("injected activation failure");
        });
    events.push_back("caller-visible-failure");

    REQUIRE(session.preflight.failure ==
            TuiTerminalPreflightFailure::VirtualTerminalUnavailable);
    REQUIRE_FALSE(session.modeGuard);
    REQUIRE(events == std::vector<std::string>{
                          "probe",
                          "capture",
                          "activate-mutate",
                          "restore",
                          "caller-visible-failure",
                      });
}

#if !defined(_WIN32)
TEST_CASE(
    "TUI terminal guard restores a real POSIX pseudo-terminal",
    "[unit][tui_terminal_guard][pty][KG-BUG-0088]") {
    ScopedPosixPtyStandardStreams pty;
    if (!pty.IsReady()) {
        SKIP("POSIX pseudo-terminal fixture unavailable");
    }

    winsize dimensions{};
    dimensions.ws_col = 120;
    dimensions.ws_row = 40;
    REQUIRE(ioctl(STDOUT_FILENO, TIOCSWINSZ, &dimensions) == 0);

    termios before{};
    REQUIRE(tcgetattr(STDIN_FILENO, &before) == 0);
    {
        auto session = kano::git::commands::StartTuiTerminalSession();
        REQUIRE(session.preflight.Accepted());
        REQUIRE(session.modeGuard);

        auto mutated = before;
        mutated.c_lflag ^= ECHO;
        REQUIRE(tcsetattr(STDIN_FILENO, TCSANOW, &mutated) == 0);
    }
    termios after{};
    REQUIRE(tcgetattr(STDIN_FILENO, &after) == 0);
    REQUIRE(after.c_iflag == before.c_iflag);
    REQUIRE(after.c_oflag == before.c_oflag);
    REQUIRE(after.c_cflag == before.c_cflag);
    REQUIRE(after.c_lflag == before.c_lflag);
}
#endif

TEST_CASE(
    "TUI terminal guard captures modes and code pages before mutation",
    "[unit][tui_terminal_guard][KG-BUG-0088]") {
    using kano::git::commands::TuiTerminalModeGuard;
    using kano::git::commands::TuiTerminalRestoreAction;

    int inputMode = 11;
    int outputMode = 12;
    unsigned int inputCodePage = 437;
    unsigned int outputCodePage = 850;
    std::vector<std::string> events;
    {
        TuiTerminalModeGuard guard([&]() -> TuiTerminalRestoreAction {
            const auto capturedInputMode = inputMode;
            const auto capturedOutputMode = outputMode;
            const auto capturedInputCodePage = inputCodePage;
            const auto capturedOutputCodePage = outputCodePage;
            events.push_back("capture-all-terminal-state");
            return [&, capturedInputMode, capturedOutputMode,
                    capturedInputCodePage, capturedOutputCodePage]() {
                inputMode = capturedInputMode;
                outputMode = capturedOutputMode;
                inputCodePage = capturedInputCodePage;
                outputCodePage = capturedOutputCodePage;
                events.push_back("restore-all-terminal-state");
            };
        });

        inputMode = 21;
        outputMode = 22;
        inputCodePage = 65001;
        outputCodePage = 65001;
        events.push_back("mutate-terminal-state");
    }

    REQUIRE(inputMode == 11);
    REQUIRE(outputMode == 12);
    REQUIRE(inputCodePage == 437);
    REQUIRE(outputCodePage == 850);
    REQUIRE(events == std::vector<std::string>{
                          "capture-all-terminal-state",
                          "mutate-terminal-state",
                          "restore-all-terminal-state",
                      });
}
