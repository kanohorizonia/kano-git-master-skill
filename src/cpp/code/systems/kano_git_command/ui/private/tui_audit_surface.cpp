#include "tui_audit_surface.hpp"

namespace kano::git::commands {

auto ProjectTuiAuditSurface(const TuiAuditSurfaceInput& InInput) noexcept
    -> TuiAuditSurfaceProjection {
    if (InInput.discoverActive) {
        return {TuiAuditView::Discover, InInput.discoverLoad};
    }
    if (InInput.helpActive) {
        return {TuiAuditView::Help, TuiAuditLoad::Ready};
    }
    if (InInput.commandActive) {
        return {TuiAuditView::Command, TuiAuditLoad::Idle};
    }
    if (InInput.commandPaletteActive) {
        return {TuiAuditView::Palette, TuiAuditLoad::Idle, true};
    }
    if (InInput.nonAuditOverlayActive) {
        return {TuiAuditView::Normal, TuiAuditLoad::Idle, false};
    }
    if (InInput.previewActive) {
        return {
            InInput.previewIsReceipt
                ? TuiAuditView::Receipt
                : TuiAuditView::Preview,
            InInput.previewLoad,
        };
    }
    if (InInput.historyActive && InInput.historyDetailActive) {
        return {TuiAuditView::Detail, InInput.historyDetailLoad};
    }
    if (InInput.historyActive) {
        return {TuiAuditView::History, InInput.historyLoad};
    }
    if (InInput.repositoriesEmpty) {
        return {TuiAuditView::Startup, InInput.startupLoad};
    }
    return {TuiAuditView::Normal, TuiAuditLoad::Ready};
}

} // namespace kano::git::commands
