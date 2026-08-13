#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace kano::git::commands {

using TuiTerminalRestoreAction = std::function<void()>;
using TuiTerminalCaptureAction = std::function<TuiTerminalRestoreAction()>;

/// Platform facts collected without entering FTXUI or changing persistent
/// terminal state.  Tests supply this value directly, so admission policy is
/// independent from the terminal owned by the test process.
struct TuiTerminalCapabilities {
    bool bStdinInteractive = false;
    bool bStdoutInteractive = false;
    bool bInputStateHealthy = false;
    bool bOutputStateHealthy = false;
    int columns = 0;
    int rows = 0;
};

enum class TuiTerminalPreflightFailure {
    None,
    StandardStreamsNotInteractive,
    TerminalStateUnavailable,
    TerminalDimensionsUnavailable,
    VirtualTerminalUnavailable,
};

struct TuiTerminalPreflightResult {
    TuiTerminalPreflightFailure failure = TuiTerminalPreflightFailure::None;

    [[nodiscard]] auto Accepted() const -> bool {
        return failure == TuiTerminalPreflightFailure::None;
    }
};

using TuiTerminalCapabilityProbe = std::function<TuiTerminalCapabilities()>;
using TuiTerminalActivationAction = std::function<bool()>;

/// Probes the current process terminal without allowing TERM-name heuristics.
auto ProbePlatformTuiTerminalCapabilities() -> TuiTerminalCapabilities;
auto ValidateTuiTerminalCapabilities(const TuiTerminalCapabilities& InCapabilities)
    -> TuiTerminalPreflightResult;
auto DescribeTuiTerminalPreflightFailure(TuiTerminalPreflightFailure InFailure)
    -> std::string_view;

/// Captures terminal state at construction and restores it at destruction.
///
/// The injectable capture action keeps lifetime behavior deterministic in unit
/// tests without requiring the test process to own a live terminal.
class TuiTerminalModeGuard {
public:
    TuiTerminalModeGuard();
    explicit TuiTerminalModeGuard(TuiTerminalCaptureAction InCaptureAction);
    ~TuiTerminalModeGuard() noexcept;

    /// Restores captured state immediately and at most once.
    auto RestoreNow() noexcept -> void;

    TuiTerminalModeGuard(const TuiTerminalModeGuard&) = delete;
    auto operator=(const TuiTerminalModeGuard&)
        -> TuiTerminalModeGuard& = delete;
    TuiTerminalModeGuard(TuiTerminalModeGuard&&) = delete;
    auto operator=(TuiTerminalModeGuard&&)
        -> TuiTerminalModeGuard& = delete;

private:
    TuiTerminalRestoreAction mRestoreAction;
};

/// Admission result for the shared FTXUI runner seam.  A guard is created only
/// after the capability probe is accepted, keeping rejected runs completely
/// outside capture, code-page, fullscreen, and ANSI entry paths.
struct TuiTerminalSession {
    TuiTerminalPreflightResult preflight;
    std::unique_ptr<TuiTerminalModeGuard> modeGuard;
};

auto StartTuiTerminalSession(TuiTerminalCapabilityProbe InProbe,
                             TuiTerminalCaptureAction InCaptureAction,
                             TuiTerminalActivationAction InActivationAction)
    -> TuiTerminalSession;
auto StartTuiTerminalSession() -> TuiTerminalSession;

}  // namespace kano::git::commands
