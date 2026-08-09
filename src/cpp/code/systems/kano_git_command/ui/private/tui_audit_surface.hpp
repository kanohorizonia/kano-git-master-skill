#pragma once

#include "tui_audit_frame.hpp"

namespace kano::git::commands {

// Flattened, already-loaded surface state.  This is the runner's pure
// precedence seam; it owns no async lifecycle and performs no I/O.
struct TuiAuditSurfaceInput {
    bool discoverActive = false;
    bool helpActive = false;
    bool commandActive = false;
    bool commandPaletteActive = false;
    bool nonAuditOverlayActive = false;
    bool previewActive = false;
    bool previewIsReceipt = false;
    bool historyActive = false;
    bool historyDetailActive = false;
    bool repositoriesEmpty = false;

    TuiAuditLoad startupLoad = TuiAuditLoad::Idle;
    TuiAuditLoad discoverLoad = TuiAuditLoad::Idle;
    TuiAuditLoad previewLoad = TuiAuditLoad::Idle;
    TuiAuditLoad historyLoad = TuiAuditLoad::Idle;
    TuiAuditLoad historyDetailLoad = TuiAuditLoad::Idle;
};

struct TuiAuditSurfaceProjection {
    TuiAuditView view = TuiAuditView::Normal;
    TuiAuditLoad load = TuiAuditLoad::Ready;
    bool showFrame = true;
};

[[nodiscard]] auto ProjectTuiAuditSurface(
    const TuiAuditSurfaceInput& InInput) noexcept
    -> TuiAuditSurfaceProjection;

} // namespace kano::git::commands
