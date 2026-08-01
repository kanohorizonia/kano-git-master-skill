#pragma once

#include <functional>

namespace kano::git::commands {

using TuiTerminalRestoreAction = std::function<void()>;
using TuiTerminalCaptureAction = std::function<TuiTerminalRestoreAction()>;

/// Captures terminal state at construction and restores it at destruction.
///
/// The injectable capture action keeps lifetime behavior deterministic in unit
/// tests without requiring the test process to own a live terminal.
class TuiTerminalModeGuard {
public:
    TuiTerminalModeGuard();
    explicit TuiTerminalModeGuard(TuiTerminalCaptureAction InCaptureAction);
    ~TuiTerminalModeGuard() noexcept;

    TuiTerminalModeGuard(const TuiTerminalModeGuard&) = delete;
    auto operator=(const TuiTerminalModeGuard&)
        -> TuiTerminalModeGuard& = delete;
    TuiTerminalModeGuard(TuiTerminalModeGuard&&) = delete;
    auto operator=(TuiTerminalModeGuard&&)
        -> TuiTerminalModeGuard& = delete;

private:
    TuiTerminalRestoreAction mRestoreAction;
};

}  // namespace kano::git::commands
