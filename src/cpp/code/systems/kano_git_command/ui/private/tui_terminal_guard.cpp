#include "tui_terminal_guard.hpp"

#ifdef KOG_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <memory>
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

auto ActivatePlatformTuiTerminal() -> bool {
#ifdef KOG_PLATFORM_WINDOWS
    const HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outputMode = 0;
    if (outputHandle == nullptr || outputHandle == INVALID_HANDLE_VALUE ||
        GetFileType(outputHandle) != FILE_TYPE_CHAR ||
        GetConsoleMode(outputHandle, &outputMode) == 0) {
        return false;
    }
    if ((outputMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        return true;
    }
    return SetConsoleMode(
               outputHandle,
               outputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

}  // namespace

auto ProbePlatformTuiTerminalCapabilities() -> TuiTerminalCapabilities {
    TuiTerminalCapabilities capabilities;
#ifdef KOG_PLATFORM_WINDOWS
    const HANDLE inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD inputMode = 0;
    DWORD outputMode = 0;
    capabilities.bStdinInteractive = inputHandle != nullptr &&
        inputHandle != INVALID_HANDLE_VALUE &&
        GetFileType(inputHandle) == FILE_TYPE_CHAR;
    capabilities.bStdoutInteractive = outputHandle != nullptr &&
        outputHandle != INVALID_HANDLE_VALUE &&
        GetFileType(outputHandle) == FILE_TYPE_CHAR;
    capabilities.bInputStateHealthy = capabilities.bStdinInteractive &&
        GetConsoleMode(inputHandle, &inputMode) != 0;
    capabilities.bOutputStateHealthy = capabilities.bStdoutInteractive &&
        GetConsoleMode(outputHandle, &outputMode) != 0;
    CONSOLE_SCREEN_BUFFER_INFO screenInfo{};
    if (capabilities.bOutputStateHealthy &&
        GetConsoleScreenBufferInfo(outputHandle, &screenInfo) != 0) {
        capabilities.columns =
            screenInfo.srWindow.Right - screenInfo.srWindow.Left + 1;
        capabilities.rows =
            screenInfo.srWindow.Bottom - screenInfo.srWindow.Top + 1;
    }
#else
    capabilities.bStdinInteractive = isatty(STDIN_FILENO) == 1;
    capabilities.bStdoutInteractive = isatty(STDOUT_FILENO) == 1;
    termios inputMode{};
    termios outputMode{};
    capabilities.bInputStateHealthy = capabilities.bStdinInteractive &&
        tcgetattr(STDIN_FILENO, &inputMode) == 0;
    capabilities.bOutputStateHealthy = capabilities.bStdoutInteractive &&
        tcgetattr(STDOUT_FILENO, &outputMode) == 0;
    winsize windowSize{};
    if (capabilities.bStdoutInteractive &&
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &windowSize) == 0) {
        capabilities.columns = static_cast<int>(windowSize.ws_col);
        capabilities.rows = static_cast<int>(windowSize.ws_row);
    }
#endif
    return capabilities;
}

auto ValidateTuiTerminalCapabilities(
    const TuiTerminalCapabilities& InCapabilities) -> TuiTerminalPreflightResult {
    if (!InCapabilities.bStdinInteractive || !InCapabilities.bStdoutInteractive) {
        return {TuiTerminalPreflightFailure::StandardStreamsNotInteractive};
    }
    if (!InCapabilities.bInputStateHealthy || !InCapabilities.bOutputStateHealthy) {
        return {TuiTerminalPreflightFailure::TerminalStateUnavailable};
    }
    if (InCapabilities.columns <= 0 || InCapabilities.rows <= 0) {
        return {TuiTerminalPreflightFailure::TerminalDimensionsUnavailable};
    }
    return {};
}

auto DescribeTuiTerminalPreflightFailure(
    const TuiTerminalPreflightFailure InFailure) -> std::string_view {
    switch (InFailure) {
    case TuiTerminalPreflightFailure::None:
        return "";
    case TuiTerminalPreflightFailure::StandardStreamsNotInteractive:
        return "interactive stdin and stdout are required";
    case TuiTerminalPreflightFailure::TerminalStateUnavailable:
        return "terminal state is unavailable";
    case TuiTerminalPreflightFailure::TerminalDimensionsUnavailable:
        return "terminal dimensions are unavailable";
    case TuiTerminalPreflightFailure::VirtualTerminalUnavailable:
        return "virtual terminal support is unavailable";
    }
    return "terminal capability probe failed";
}

TuiTerminalModeGuard::TuiTerminalModeGuard()
    : TuiTerminalModeGuard(CapturePlatformTerminalState) {}

TuiTerminalModeGuard::TuiTerminalModeGuard(
    TuiTerminalCaptureAction InCaptureAction) {
    if (InCaptureAction) {
        mRestoreAction = InCaptureAction();
    }
}

TuiTerminalModeGuard::~TuiTerminalModeGuard() noexcept {
    RestoreNow();
}

auto TuiTerminalModeGuard::RestoreNow() noexcept -> void {
    auto restoreAction = std::move(mRestoreAction);
    mRestoreAction = {};
    if (!restoreAction) {
        return;
    }
    try {
        restoreAction();
    } catch (...) {
        // Restoration must remain safe on every TUI exit path.
    }
}

auto StartTuiTerminalSession(TuiTerminalCapabilityProbe InProbe,
                             TuiTerminalCaptureAction InCaptureAction,
                             TuiTerminalActivationAction InActivationAction)
    -> TuiTerminalSession {
    TuiTerminalSession session;
    const auto capabilities = InProbe ? InProbe() : TuiTerminalCapabilities{};
    session.preflight = ValidateTuiTerminalCapabilities(capabilities);
    if (!session.preflight.Accepted()) {
        return session;
    }
    session.modeGuard = std::make_unique<TuiTerminalModeGuard>(
        std::move(InCaptureAction));
    bool bActivated = false;
    try {
        bActivated = InActivationAction && InActivationAction();
    } catch (...) {
        bActivated = false;
    }
    if (!bActivated) {
        session.modeGuard->RestoreNow();
        session.modeGuard.reset();
        session.preflight.failure =
            TuiTerminalPreflightFailure::VirtualTerminalUnavailable;
    }
    return session;
}

auto StartTuiTerminalSession() -> TuiTerminalSession {
    return StartTuiTerminalSession(
        ProbePlatformTuiTerminalCapabilities,
        CapturePlatformTerminalState,
        ActivatePlatformTuiTerminal);
}

}  // namespace kano::git::commands
