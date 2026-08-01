#include "tui_terminal_guard.hpp"

#ifdef KOG_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <utility>

namespace kano::git::commands {
namespace {

auto CapturePlatformTerminalState() -> TuiTerminalRestoreAction {
#ifdef KOG_PLATFORM_WINDOWS
    const HANDLE inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD inputMode = 0;
    DWORD outputMode = 0;
    const UINT inputCodePage = GetConsoleCP();
    const UINT outputCodePage = GetConsoleOutputCP();
    const bool bInputValid =
        inputHandle != nullptr && inputHandle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(inputHandle, &inputMode) != 0;
    const bool bOutputValid =
        outputHandle != nullptr && outputHandle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(outputHandle, &outputMode) != 0;

    return [inputHandle,
            outputHandle,
            inputMode,
            outputMode,
            inputCodePage,
            outputCodePage,
            bInputValid,
            bOutputValid]() {
        if (bInputValid) {
            SetConsoleMode(inputHandle, inputMode);
        }
        if (bOutputValid) {
            SetConsoleMode(outputHandle, outputMode);
        }
        if (inputCodePage != 0) {
            SetConsoleCP(inputCodePage);
        }
        if (outputCodePage != 0) {
            SetConsoleOutputCP(outputCodePage);
        }
    };
#else
    termios inputMode{};
    termios outputMode{};
    const bool bInputValid =
        isatty(STDIN_FILENO) == 1 &&
        tcgetattr(STDIN_FILENO, &inputMode) == 0;
    const bool bOutputValid =
        isatty(STDOUT_FILENO) == 1 &&
        tcgetattr(STDOUT_FILENO, &outputMode) == 0;

    return [inputMode, outputMode, bInputValid, bOutputValid]() {
        if (bInputValid) {
            tcsetattr(STDIN_FILENO, TCSANOW, &inputMode);
        }
        if (bOutputValid) {
            tcsetattr(STDOUT_FILENO, TCSANOW, &outputMode);
        }
    };
#endif
}

}  // namespace

TuiTerminalModeGuard::TuiTerminalModeGuard()
    : TuiTerminalModeGuard(CapturePlatformTerminalState) {}

TuiTerminalModeGuard::TuiTerminalModeGuard(
    TuiTerminalCaptureAction InCaptureAction) {
    if (InCaptureAction) {
        mRestoreAction = InCaptureAction();
    }
}

TuiTerminalModeGuard::~TuiTerminalModeGuard() noexcept {
    if (!mRestoreAction) {
        return;
    }
    try {
        mRestoreAction();
    } catch (...) {
        // Destruction must remain safe on every TUI exit path.
    }
}

}  // namespace kano::git::commands
