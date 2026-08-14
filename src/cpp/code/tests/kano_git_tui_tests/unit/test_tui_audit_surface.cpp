#include <catch2/catch_test_macros.hpp>

#include "tui_audit_surface.hpp"

using namespace kano::git::commands;

TEST_CASE("audit surface projection matches production panel precedence",
          "[unit][tui_audit_surface][tui_pr_focus][KG-TSK-0132]") {
    TuiAuditSurfaceInput input;
    input.repositoriesEmpty = true;
    input.startupLoad = TuiAuditLoad::Loading;
    auto projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Startup);
    CHECK(projected.load == TuiAuditLoad::Loading);

    input.historyActive = true;
    input.historyLoad = TuiAuditLoad::Empty;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::History);
    CHECK(projected.load == TuiAuditLoad::Empty);

    input.historyDetailActive = true;
    input.historyDetailLoad = TuiAuditLoad::Failed;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Detail);
    CHECK(projected.load == TuiAuditLoad::Failed);

    input.previewActive = true;
    input.previewLoad = TuiAuditLoad::Loading;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Preview);
    CHECK(projected.load == TuiAuditLoad::Loading);
    input.previewIsReceipt = true;
    CHECK(ProjectTuiAuditSurface(input).view == TuiAuditView::Receipt);

    input.helpActive = true;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Help);
    CHECK(projected.load == TuiAuditLoad::Ready);

    input.helpActive = false;
    input.commandActive = true;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Command);
    CHECK(projected.load == TuiAuditLoad::Idle);

    input.commandActive = false;
    input.commandPaletteActive = true;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Palette);
    CHECK(projected.showFrame);

    input.commandPaletteActive = false;
    input.nonAuditOverlayActive = true;
    projected = ProjectTuiAuditSurface(input);
    CHECK_FALSE(projected.showFrame);

    input.nonAuditOverlayActive = false;
    input.discoverActive = true;
    input.discoverLoad = TuiAuditLoad::Cancelled;
    projected = ProjectTuiAuditSurface(input);
    CHECK(projected.view == TuiAuditView::Discover);
    CHECK(projected.load == TuiAuditLoad::Cancelled);
}

TEST_CASE("audit surface projection keeps normal and generic preview truthful",
          "[unit][tui_audit_surface][KG-TSK-0132]") {
    const auto normal = ProjectTuiAuditSurface({});
    CHECK(normal.view == TuiAuditView::Normal);
    CHECK(normal.load == TuiAuditLoad::Ready);
    CHECK(normal.showFrame);

    const auto preview = ProjectTuiAuditSurface({
        .previewActive = true,
        .previewIsReceipt = false,
        .previewLoad = TuiAuditLoad::Failed,
    });
    CHECK(preview.view == TuiAuditView::Preview);
    CHECK(preview.load == TuiAuditLoad::Failed);
}
