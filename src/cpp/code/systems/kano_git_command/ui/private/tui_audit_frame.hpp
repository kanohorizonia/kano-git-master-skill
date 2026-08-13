#pragma once

#include "audit_run_reader.hpp"
#include "tui_theme.hpp"

#include <ftxui/dom/elements.hpp>

#include <optional>
#include <string>

namespace kano::git::commands {

enum class TuiAuditView {
    Startup,
    Normal,
    Help,
    Discover,
    History,
    Detail,
    Receipt,
    Preview,
    Command,
    Palette,
};

enum class TuiAuditLoad {
    Idle,
    Loading,
    Empty,
    Failed,
    Cancelled,
    Ready,
};

enum class TuiAuditReceiptState {
    Linked,
    Reading,
    Missing,
    Pending,
    Incomplete,
    Corrupt,
    Incompatible,
    Invalid,
};

// Evidence presentation is a projection of the verified reader result. It
// deliberately distinguishes unavailable evidence from a receipt that has no
// evidence references, and fails closed when evidence invariants disagree.
enum class TuiAuditEvidenceAvailability {
    None,
    Retained,
    Unavailable,
    Inconsistent,
};

struct TuiAuditFrameGeometry {
    int width = 120;
    int height = 36;
};

struct TuiAuditDashboardGeometry {
    TuiAuditFrameGeometry frame;
    bool compactRoot = false;
    int mainHeight = 0;
    int rightPanelContentHeight = 0;
};

// A UI-safe receipt projection. The runner supplies a result it has already
// read on a worker; this seam never opens files or starts work.
struct TuiAuditReceiptTruth {
    TuiAuditReceiptState state = TuiAuditReceiptState::Missing;
    TuiAuditLoad load = TuiAuditLoad::Empty;
    std::string runId;
    std::string receiptId;
    std::string correlation;
    std::string repository = "not available";
    std::string receiptAbsenceReason;
    TuiAuditEvidenceAvailability evidenceAvailability =
        TuiAuditEvidenceAvailability::Unavailable;
    bool evidenceRedacted = false;
    bool evidenceWithheld = false;
    bool evidenceTruncated = false;
};

[[nodiscard]] auto ProjectTuiAuditReceiptTruth(
    const std::optional<OperationAuditRunReadResult>& InReadResult)
    -> TuiAuditReceiptTruth;

[[nodiscard]] auto ComputeTuiAuditFrameGeometry(
    int InTerminalWidth,
    int InTerminalHeight,
    bool bInMono = false) -> TuiAuditFrameGeometry;

[[nodiscard]] auto ComputeTuiAuditDashboardGeometry(
    int InTerminalWidth,
    int InTerminalHeight,
    bool bInCommandMode,
    bool bInMono = false) -> TuiAuditDashboardGeometry;

// The audit frame consumes the dashboard's resolved semantic palette. Mono
// selects emphasis-only decorators, so readability never depends on ANSI
// color snapshots or a separately detected terminal theme.
struct TuiAuditSemanticTheme {
    bool mono = false;
    TuiThemePalette palette;
};

struct TuiAuditFrameModel {
    TuiAuditView view = TuiAuditView::Startup;
    TuiAuditLoad load = TuiAuditLoad::Idle;
    TuiAuditReceiptState receiptState = TuiAuditReceiptState::Missing;

    std::string scope = "unknown";
    std::string repository = "none";
    std::string provenanceSource = "unknown";
    std::string provenanceFreshness = "unknown";
    std::string observedAtUtc = "unknown";
    std::string runId;
    std::string receiptId;
    std::string correlation;
    std::string receiptAbsenceReason = "no receipt selected in this view";
    std::string diagnostic;
    std::string hint;
    std::string nextAction = "inspect only; use :refresh to retry bounded reads";
    std::string footer;

    TuiAuditEvidenceAvailability evidenceAvailability =
        TuiAuditEvidenceAvailability::Unavailable;
    bool evidenceRedacted = false;
    bool evidenceWithheld = false;
    bool evidenceTruncated = false;
};

[[nodiscard]] auto ApplyTuiAuditRunReadResult(
    TuiAuditFrameModel InModel,
    const std::optional<OperationAuditRunReadResult>& InReadResult)
    -> TuiAuditFrameModel;

[[nodiscard]] auto RenderTuiAuditFrame(
    const TuiAuditFrameModel& InModel,
    const TuiAuditFrameGeometry& InGeometry,
    const TuiAuditSemanticTheme& InTheme) -> ftxui::Element;

// Deterministic offscreen adapter used by contract tests. It performs no
// terminal, filesystem, Git, subprocess, or worker I/O.
[[nodiscard]] auto RenderTuiAuditFrameText(
    const TuiAuditFrameModel& InModel,
    const TuiAuditFrameGeometry& InGeometry,
    const TuiAuditSemanticTheme& InTheme) -> std::string;

[[nodiscard]] auto ComposeTuiAuditCompactRoot(
    ftxui::Element InMainPanel,
    ftxui::Elements InCommandRows) -> ftxui::Element;

[[nodiscard]] auto RenderTuiAuditCompactRootText(
    ftxui::Element InMainPanel,
    ftxui::Elements InCommandRows,
    int InWidth,
    int InHeight) -> std::string;

} // namespace kano::git::commands
