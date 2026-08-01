#include <catch2/catch_test_macros.hpp>

#include "tui_terminal_guard.hpp"

#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cstdlib>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

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

#if !defined(_WIN32)
TEST_CASE(
    "TUI terminal guard restores a real POSIX pseudo-terminal",
    "[unit][tui_terminal_guard][pty][KG-BUG-0088]") {
    using kano::git::commands::TuiTerminalModeGuard;

    bool bPtyAvailable = false;
    bool bRestored = false;
    const int savedInput = dup(STDIN_FILENO);
    const int savedOutput = dup(STDOUT_FILENO);
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    int slave = -1;
    if (savedInput >= 0 && savedOutput >= 0 && master >= 0 &&
        grantpt(master) == 0 && unlockpt(master) == 0) {
        const char* slaveName = ptsname(master);
        if (slaveName != nullptr) {
            slave = open(slaveName, O_RDWR | O_NOCTTY);
        }
    }

    if (slave >= 0 && dup2(slave, STDIN_FILENO) >= 0 &&
        dup2(slave, STDOUT_FILENO) >= 0) {
        termios before{};
        if (tcgetattr(STDIN_FILENO, &before) == 0) {
            bPtyAvailable = true;
            {
                TuiTerminalModeGuard guard;
                auto mutated = before;
                mutated.c_lflag ^= ECHO;
                if (tcsetattr(STDIN_FILENO, TCSANOW, &mutated) != 0) {
                    bPtyAvailable = false;
                }
            }
            termios after{};
            if (bPtyAvailable && tcgetattr(STDIN_FILENO, &after) == 0) {
                bRestored = after.c_iflag == before.c_iflag &&
                    after.c_oflag == before.c_oflag &&
                    after.c_cflag == before.c_cflag &&
                    after.c_lflag == before.c_lflag;
            }
        }
    }

    if (savedInput >= 0) {
        dup2(savedInput, STDIN_FILENO);
        close(savedInput);
    }
    if (savedOutput >= 0) {
        dup2(savedOutput, STDOUT_FILENO);
        close(savedOutput);
    }
    if (slave >= 0) {
        close(slave);
    }
    if (master >= 0) {
        close(master);
    }

    if (!bPtyAvailable) {
        SUCCEED("POSIX pseudo-terminal unavailable in this test environment");
        return;
    }
    REQUIRE(bRestored);
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
