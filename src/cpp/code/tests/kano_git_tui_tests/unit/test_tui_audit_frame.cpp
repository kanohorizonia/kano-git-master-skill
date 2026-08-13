#include <catch2/catch_test_macros.hpp>

#include "tui_audit_frame.hpp"
#include "tui_keymap.hpp"

#include <ftxui/screen/string.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kano::git::commands;

auto SplitRenderedLines(const std::string& InText) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::istringstream stream(InText);
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

auto RequireBoundedFrame(const std::string& InText,
                         const TuiAuditFrameGeometry& InGeometry) -> void {
    const auto lines = SplitRenderedLines(InText);
    REQUIRE(lines.size() <= static_cast<std::size_t>(InGeometry.height));
    for (const auto& line : lines) {
        REQUIRE(ftxui::string_width(line) <= InGeometry.width);
    }
    REQUIRE(InText.find('\x1b') == std::string::npos);
}

auto RequireCanonicalControls(const std::string& InRendered,
                              const std::string_view InControls) -> void {
    const auto compactAscii = [](const std::string_view value) {
        std::string compact;
        compact.reserve(value.size());
        for (const auto byte : value) {
            const auto character = static_cast<unsigned char>(byte);
            if (character < 0x80U && std::isspace(character) == 0) {
                compact.push_back(byte);
            }
        }
        return compact;
    };
    const auto compactRendered = compactAscii(InRendered);
    constexpr std::string_view separator = " | ";
    std::size_t start = 0;
    while (start < InControls.size()) {
        const auto end = InControls.find(separator, start);
        const auto token = InControls.substr(
            start,
            end == std::string_view::npos
                ? InControls.size() - start
                : end - start);
        CAPTURE(token);
        REQUIRE(compactRendered.find(compactAscii(token)) != std::string::npos);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + separator.size();
    }
}

auto BaseModel() -> TuiAuditFrameModel {
    TuiAuditFrameModel model;
    model.scope = "scope-token";
    model.repository = "repo-token";
    model.provenanceSource = "source-token";
    model.provenanceFreshness = "freshness-token";
    model.observedAtUtc = "2030-01-02T03:04:05Z";
    model.receiptAbsenceReason = "absence-token";
    model.nextAction = "next-action-token";
    model.footer = "footer-token";
    return model;
}

auto VerifiedReadResult(const OperationAuditRunReadState InState)
    -> OperationAuditRunReadResult {
    OperationAuditRunProjection projection;
    projection.runId = "run-token";
    projection.receiptId = "receipt-token";
    projection.correlation.mode = kano::git::audit::CorrelationMode::Koa;
    projection.correlation.itemId = "item-token";
    projection.totalRepositories = 1;
    projection.retainedRepositories = 1;
    projection.repositories.push_back({
        .repositoryId = "repo-token",
        .before = {
            .headSha = "1111111111111111111111111111111111111111",
        },
        .after = {
            .headSha = "2222222222222222222222222222222222222222",
        },
    });
    projection.hasRedactedEvidence = true;
    projection.hasWithheldEvidence = true;
    projection.eventsTruncated = true;
    projection.evidenceTruncated = true;
    projection.totalEvidenceReferences = 3;
    projection.retainedEvidenceReferences = 1;
    return {
        .state = InState,
        .code = OperationAuditRunReadCode::None,
        .run = std::move(projection),
    };
}

auto VerifiedReadResultWithEvidence(const std::uint64_t InTotal,
                                    const std::uint64_t InRetained,
                                    const bool bInRedacted,
                                    const bool bInWithheld,
                                    const bool bInTruncated)
    -> OperationAuditRunReadResult {
    auto result = VerifiedReadResult(OperationAuditRunReadState::Ready);
    auto& projection = *result.run;
    projection.totalEvidenceReferences = InTotal;
    projection.retainedEvidenceReferences = InRetained;
    projection.hasRedactedEvidence = bInRedacted;
    projection.hasWithheldEvidence = bInWithheld;
    projection.eventsTruncated = false;
    projection.evidenceTruncated = bInTruncated;
    return result;
}

} // namespace

TEST_CASE("audit frame renders bounded lifecycle contracts",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    struct Scenario {
        TuiAuditView view;
        TuiAuditLoad load;
        std::string_view expectedState;
        std::string_view expectedExit;
    };
    constexpr std::array scenarios{
        Scenario{TuiAuditView::Startup, TuiAuditLoad::Loading, "loading", "q quit"},
        Scenario{TuiAuditView::Startup, TuiAuditLoad::Empty, "empty", "q quit"},
        Scenario{TuiAuditView::Startup, TuiAuditLoad::Failed, "failed", "q quit"},
        Scenario{TuiAuditView::Startup, TuiAuditLoad::Cancelled, "cancelled", "q quit"},
        Scenario{TuiAuditView::Normal, TuiAuditLoad::Ready, "ready", "q quit"},
        Scenario{TuiAuditView::Help, TuiAuditLoad::Ready, "ready", "Esc/q close"},
        Scenario{TuiAuditView::Discover, TuiAuditLoad::Loading, "loading", "Esc/q close"},
        Scenario{TuiAuditView::History, TuiAuditLoad::Empty, "empty", "Esc/q back"},
        Scenario{TuiAuditView::Detail, TuiAuditLoad::Failed, "failed", "Esc/q back"},
        Scenario{TuiAuditView::Receipt, TuiAuditLoad::Ready, "ready", "Esc/q close"},
        Scenario{TuiAuditView::Preview, TuiAuditLoad::Ready, "ready", "Esc/q close"},
        Scenario{TuiAuditView::Command, TuiAuditLoad::Idle, "idle", "Esc cancel"},
        Scenario{TuiAuditView::Palette, TuiAuditLoad::Idle, "idle", "Esc close"},
    };
    const TuiAuditFrameGeometry geometry{120, 36};

    for (const auto& scenario : scenarios) {
        auto model = BaseModel();
        model.view = scenario.view;
        model.load = scenario.load;
        model.diagnostic = "diagnostic-token";
        model.hint = "hint-token";
        const auto rendered = RenderTuiAuditFrameText(model, geometry, {});
        CAPTURE(static_cast<int>(scenario.view), static_cast<int>(scenario.load));
        RequireBoundedFrame(rendered, geometry);
        REQUIRE(rendered.find(scenario.expectedState) != std::string::npos);
        REQUIRE(rendered.find(model.scope) != std::string::npos);
        REQUIRE(rendered.find(model.repository) != std::string::npos);
        REQUIRE(rendered.find(model.provenanceSource) != std::string::npos);
        REQUIRE(rendered.find(model.provenanceFreshness) != std::string::npos);
        REQUIRE(rendered.find(model.receiptAbsenceReason) != std::string::npos);
        REQUIRE(rendered.find(model.nextAction) != std::string::npos);
        REQUIRE(rendered.find(model.diagnostic) != std::string::npos);
        REQUIRE(rendered.find(model.hint) != std::string::npos);
        REQUIRE(rendered.find(scenario.expectedExit) != std::string::npos);
    }
}

TEST_CASE("audit frame renders provenance and receipt truth",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    const TuiAuditFrameGeometry geometry{120, 36};

    SECTION("cached known") {
        auto model = BaseModel();
        model.provenanceSource = "trusted-cache-token";
        model.provenanceFreshness = "fresh-token";
        const auto rendered = RenderTuiAuditFrameText(model, geometry, {});
        REQUIRE(rendered.find(model.provenanceSource) != std::string::npos);
        REQUIRE(rendered.find(model.provenanceFreshness) != std::string::npos);
        RequireBoundedFrame(rendered, geometry);
    }

    SECTION("fresh live") {
        auto model = BaseModel();
        model.provenanceSource = "live-token";
        model.provenanceFreshness = "fresh-token";
        const auto rendered = RenderTuiAuditFrameText(model, geometry, {});
        REQUIRE(rendered.find(model.provenanceSource) != std::string::npos);
        REQUIRE(rendered.find(model.provenanceFreshness) != std::string::npos);
        RequireBoundedFrame(rendered, geometry);
    }

    SECTION("unknown or stale") {
        auto model = BaseModel();
        model.provenanceSource = "unknown-token";
        model.provenanceFreshness = "stale-token";
        const auto rendered = RenderTuiAuditFrameText(model, geometry, {});
        REQUIRE(rendered.find(model.provenanceSource) != std::string::npos);
        REQUIRE(rendered.find(model.provenanceFreshness) != std::string::npos);
        RequireBoundedFrame(rendered, geometry);
    }

    SECTION("linked receipt and bounded evidence") {
        auto model = BaseModel();
        model.view = TuiAuditView::Receipt;
        model.receiptState = TuiAuditReceiptState::Linked;
        model.runId = "run-token";
        model.receiptId = "receipt-token";
        model.correlation = "correlation-token";
        model.evidenceAvailability = TuiAuditEvidenceAvailability::Retained;
        model.evidenceRedacted = true;
        model.evidenceWithheld = true;
        model.evidenceTruncated = true;
        const auto rendered = RenderTuiAuditFrameText(model, geometry, {});
        REQUIRE(rendered.find(model.runId) != std::string::npos);
        REQUIRE(rendered.find(model.receiptId) != std::string::npos);
        REQUIRE(rendered.find(model.correlation) != std::string::npos);
        REQUIRE(rendered.find("redacted") != std::string::npos);
        REQUIRE(rendered.find("withheld") != std::string::npos);
        REQUIRE(rendered.find("truncated") != std::string::npos);
        RequireBoundedFrame(rendered, geometry);
    }

    SECTION("corrupt receipt gives an explicit reason") {
        auto model = BaseModel();
        model.receiptState = TuiAuditReceiptState::Corrupt;
        model.receiptAbsenceReason = "corrupt-reason-token";
        const auto rendered = RenderTuiAuditFrameText(model, geometry, {});
        REQUIRE(rendered.find("corrupt") != std::string::npos);
        REQUIRE(rendered.find(model.receiptAbsenceReason) != std::string::npos);
        RequireBoundedFrame(rendered, geometry);
    }
}

TEST_CASE("audit frame uses canonical help and exit guidance",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    auto model = BaseModel();
    model.view = TuiAuditView::Help;
    model.load = TuiAuditLoad::Ready;
    const TuiAuditFrameGeometry geometry{120, 36};
    const auto rendered = RenderTuiAuditFrameText(model, geometry, {});

    for (const auto& guidance : GetAllTuiKeyGuidance()) {
        REQUIRE(rendered.find(guidance.label) != std::string::npos);
        RequireCanonicalControls(rendered, guidance.controls);
    }
    REQUIRE(rendered.find("Esc/q close") != std::string::npos);
    RequireBoundedFrame(rendered, geometry);
}

TEST_CASE("audit frame consumes resolved semantic terminal palettes",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    auto model = BaseModel();
    model.load = TuiAuditLoad::Failed;
    const TuiAuditFrameGeometry geometry{120, 36};
    for (const auto mode : {TuiThemeMode::Dark, TuiThemeMode::Light,
                            TuiThemeMode::Mono}) {
        const auto resolved = ResolveTuiTheme(mode, {});
        const auto rendered = RenderTuiAuditFrameText(
            model,
            geometry,
            TuiAuditSemanticTheme{
                .mono = resolved.effectiveMode == TuiThemeMode::Mono,
                .palette = resolved.palette,
            });
        CAPTURE(std::string(TuiThemeModeName(mode)));
        REQUIRE(rendered.find("failed") != std::string::npos);
        RequireBoundedFrame(rendered, geometry);
    }
}

TEST_CASE("narrow audit frame retains linked receipt and correlation identity",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    auto model = BaseModel();
    model.view = TuiAuditView::Receipt;
    model.receiptState = TuiAuditReceiptState::Linked;
    model.scope = "scope";
    model.repository = "repo-測試";
    model.provenanceSource = "live";
    model.provenanceFreshness = "fresh";
    model.runId = "run-0123456789abcdef";
    model.receiptId =
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    model.correlation = "koa item=item-token";
    model.nextAction = "inspect";
    model.evidenceRedacted = true;
    model.evidenceWithheld = true;
    model.evidenceTruncated = true;
    const auto geometry = ComputeTuiAuditFrameGeometry(72, 22, true);
    const auto rendered = RenderTuiAuditFrameText(
        model,
        geometry,
        TuiAuditSemanticTheme{true});

    REQUIRE(rendered.find("run=run-0..def") != std::string::npos);
    REQUIRE(rendered.find("receipt=01234..def") != std::string::npos);
    REQUIRE(rendered.find(model.correlation) != std::string::npos);
    REQUIRE(rendered.find("redacted") != std::string::npos);
    REQUIRE(rendered.find("withheld") != std::string::npos);
    REQUIRE(rendered.find("trunc") != std::string::npos);
    REQUIRE(rendered.find("ev=unavail redacted withheld trunc") !=
            std::string::npos);
    REQUIRE(rendered.find(GetTuiKeyGuidance(TuiKeyContext::Preview).compactControls) !=
            std::string::npos);
    RequireBoundedFrame(rendered, geometry);
}

TEST_CASE("audit frame remains bounded in narrow mono mode",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    auto model = BaseModel();
    model.view = TuiAuditView::Detail;
    model.load = TuiAuditLoad::Failed;
    model.receiptState = TuiAuditReceiptState::Corrupt;
    model.scope = "scope";
    model.repository = "repo";
    model.provenanceSource = "live";
    model.provenanceFreshness = "fresh";
    model.receiptAbsenceReason = "bad";
    model.nextAction = "retry";
    model.diagnostic = "bounded-diagnostic-token";
    model.hint = "bounded-hint-token";
    model.evidenceRedacted = true;
    model.evidenceTruncated = true;
    const auto geometry = ComputeTuiAuditFrameGeometry(72, 22, true);
    const auto rendered = RenderTuiAuditFrameText(
        model,
        geometry,
        TuiAuditSemanticTheme{true});

    REQUIRE(geometry.width == 36);
    REQUIRE(geometry.height == 11);
    REQUIRE(rendered.find("AUDIT") != std::string::npos);
    REQUIRE(rendered.find("failed") != std::string::npos);
    REQUIRE(rendered.find("redacted") != std::string::npos);
    REQUIRE(rendered.find("trunc") != std::string::npos);
    REQUIRE(rendered.find(model.scope) != std::string::npos);
    REQUIRE(rendered.find(model.repository) != std::string::npos);
    REQUIRE(rendered.find(model.provenanceSource) != std::string::npos);
    REQUIRE(rendered.find(model.provenanceFreshness) != std::string::npos);
    REQUIRE(rendered.find(model.receiptAbsenceReason) != std::string::npos);
    REQUIRE(rendered.find(model.nextAction) != std::string::npos);
    REQUIRE(rendered.find("Esc/q back") != std::string::npos);
    REQUIRE(rendered.find(
                GetTuiKeyGuidance(TuiKeyContext::Detail).compactControls) !=
            std::string::npos);
    RequireBoundedFrame(rendered, geometry);
}

TEST_CASE("audit dashboard reserves achievable compact production geometry",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    const auto normal = ComputeTuiAuditDashboardGeometry(72, 22, false, true);
    REQUIRE(normal.compactRoot);
    REQUIRE(normal.frame.width == 36);
    REQUIRE(normal.frame.height == 11);
    REQUIRE(normal.mainHeight == 20);
    REQUIRE(normal.rightPanelContentHeight == 9);

    const auto command = ComputeTuiAuditDashboardGeometry(72, 22, true, true);
    REQUIRE(command.compactRoot);
    REQUIRE(command.frame.width == normal.frame.width);
    REQUIRE(command.frame.height == normal.frame.height);
    REQUIRE(command.mainHeight == 17);
    REQUIRE(command.rightPanelContentHeight == 6);
}

TEST_CASE("compact production composition keeps frame controls and content visible",
          "[unit][tui_audit_frame][KG-TSK-0132]") {
    const auto makeMainPanel = [](const TuiAuditFrameGeometry geometry,
                                  const TuiAuditView view) {
        auto model = BaseModel();
        model.view = view;
        model.scope = "workspace";
        model.repository = "repo";
        model.nextAction = "inspect";
        auto right = ftxui::vbox({
            RenderTuiAuditFrame(model, geometry, {.mono = true}),
            ftxui::text("content-token") | ftxui::flex,
        });
        return ftxui::hbox({
            ftxui::text("repositories") |
                ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 22),
            ftxui::separator(),
            std::move(right) | ftxui::flex,
        });
    };

    const auto normal = ComputeTuiAuditDashboardGeometry(72, 22, false, true);
    const auto rendered = RenderTuiAuditCompactRootText(
        makeMainPanel(normal.frame, TuiAuditView::Normal),
        {},
        72,
        22);
    REQUIRE(rendered.find("content-token") != std::string::npos);
    REQUIRE(rendered.find("? help | q quit") != std::string::npos);
    RequireBoundedFrame(rendered, {72, 22});

    const auto command = ComputeTuiAuditDashboardGeometry(72, 22, true, true);
    ftxui::Elements commandRows{
        ftxui::text(":status") | ftxui::border,
    };
    const auto commandRendered = RenderTuiAuditCompactRootText(
        makeMainPanel(command.frame, TuiAuditView::Command),
        std::move(commandRows),
        72,
        22);
    REQUIRE(commandRendered.find("content-token") != std::string::npos);
    REQUIRE(commandRendered.find("Enter inspect | Esc cancel") !=
            std::string::npos);
    REQUIRE(commandRendered.find(":status") != std::string::npos);
    RequireBoundedFrame(commandRendered, {72, 22});
}

TEST_CASE("audit receipt truth projects verified and typed reader outcomes",
          "[unit][tui_audit_frame][tui_pr_focus][KG-TSK-0132]") {
    SECTION("unread receipt is explicitly missing") {
        const auto truth = ProjectTuiAuditReceiptTruth(std::nullopt);
        REQUIRE(truth.state == TuiAuditReceiptState::Missing);
        REQUIRE(truth.evidenceAvailability ==
                TuiAuditEvidenceAvailability::Unavailable);
        REQUIRE_FALSE(truth.receiptAbsenceReason.empty());

        auto model = BaseModel();
        model.receiptAbsenceReason = "view-specific-absence";
        model.load = TuiAuditLoad::Cancelled;
        model.evidenceAvailability = TuiAuditEvidenceAvailability::Retained;
        model.evidenceRedacted = true;
        model.evidenceWithheld = true;
        model.evidenceTruncated = true;
        const auto applied = ApplyTuiAuditRunReadResult(model, std::nullopt);
        REQUIRE(applied.receiptAbsenceReason == model.receiptAbsenceReason);
        REQUIRE(applied.load == model.load);
        REQUIRE(applied.evidenceAvailability ==
                TuiAuditEvidenceAvailability::Unavailable);
        REQUIRE_FALSE(applied.evidenceRedacted);
        REQUIRE_FALSE(applied.evidenceWithheld);
        REQUIRE_FALSE(applied.evidenceTruncated);
        const auto rendered = RenderTuiAuditFrameText(
            applied,
            TuiAuditFrameGeometry{120, 36},
            {});
        REQUIRE(rendered.find("evidence: unavailable") != std::string::npos);
    }

    for (const auto state : {OperationAuditRunReadState::Ready,
                             OperationAuditRunReadState::Truncated}) {
        const auto truth = ProjectTuiAuditReceiptTruth(VerifiedReadResult(state));
        CAPTURE(static_cast<int>(state));
        REQUIRE(truth.state == TuiAuditReceiptState::Linked);
        REQUIRE(truth.load == TuiAuditLoad::Ready);
        REQUIRE(truth.runId == "run-token");
        REQUIRE(truth.receiptId == "receipt-token");
        REQUIRE(truth.correlation.find("item-token") != std::string::npos);
        REQUIRE(truth.repository.find("repo-token") != std::string::npos);
        REQUIRE(truth.repository.find("111111111111") != std::string::npos);
        REQUIRE(truth.repository.find("222222222222") != std::string::npos);
        REQUIRE(truth.evidenceAvailability ==
                TuiAuditEvidenceAvailability::Retained);
        REQUIRE(truth.evidenceRedacted);
        REQUIRE(truth.evidenceWithheld);
        REQUIRE(truth.evidenceTruncated);

        const auto model = ApplyTuiAuditRunReadResult(
            BaseModel(),
            VerifiedReadResult(state));
        REQUIRE(model.scope == "workspace");
        REQUIRE(model.repository == truth.repository);
    }

    SECTION("missing result remains missing") {
        OperationAuditRunReadResult result;
        result.state = OperationAuditRunReadState::Missing;
        result.diagnostic = "missing-token";
        const auto truth = ProjectTuiAuditReceiptTruth(result);
        REQUIRE(truth.state == TuiAuditReceiptState::Missing);
        REQUIRE(truth.load == TuiAuditLoad::Empty);
        REQUIRE(truth.receiptAbsenceReason == result.diagnostic);
        REQUIRE(truth.evidenceAvailability ==
                TuiAuditEvidenceAvailability::Unavailable);
        REQUIRE_FALSE(truth.evidenceTruncated);

        auto model = ApplyTuiAuditRunReadResult(BaseModel(), result);
        REQUIRE(model.receiptState == TuiAuditReceiptState::Missing);
        REQUIRE(model.load == TuiAuditLoad::Empty);
        REQUIRE(model.scope == "workspace");
        REQUIRE(model.repository == "not available");
        REQUIRE(model.receiptAbsenceReason == result.diagnostic);
        REQUIRE(model.evidenceAvailability ==
                TuiAuditEvidenceAvailability::Unavailable);
        const auto rendered = RenderTuiAuditFrameText(
            model,
            TuiAuditFrameGeometry{120, 36},
            {});
        REQUIRE(rendered.find("evidence: unavailable") != std::string::npos);
    }

    for (const auto state : {OperationAuditRunReadState::Ready,
                             OperationAuditRunReadState::Truncated,
                             OperationAuditRunReadState::Pending,
                             OperationAuditRunReadState::Incomplete,
                             OperationAuditRunReadState::Corrupt,
                             OperationAuditRunReadState::Incompatible,
                             OperationAuditRunReadState::Invalid}) {
        OperationAuditRunReadResult result;
        result.state = state;
        result.diagnostic = std::string(240, 'x');
        result.diagnosticTruncated = true;
        const auto truth = ProjectTuiAuditReceiptTruth(result);
        CAPTURE(static_cast<int>(state));
        const auto expectedState = [&]() {
            switch (state) {
                case OperationAuditRunReadState::Pending:
                    return TuiAuditReceiptState::Pending;
                case OperationAuditRunReadState::Incomplete:
                    return TuiAuditReceiptState::Incomplete;
                case OperationAuditRunReadState::Corrupt:
                    return TuiAuditReceiptState::Corrupt;
                case OperationAuditRunReadState::Incompatible:
                    return TuiAuditReceiptState::Incompatible;
                case OperationAuditRunReadState::Invalid:
                    return TuiAuditReceiptState::Invalid;
                default:
                    return TuiAuditReceiptState::Invalid;
            }
        }();
        REQUIRE(truth.state == expectedState);
        REQUIRE(truth.load == (state == OperationAuditRunReadState::Pending
                                   ? TuiAuditLoad::Loading
                                   : TuiAuditLoad::Failed));
        REQUIRE(truth.receiptAbsenceReason.size() <= 192U);
        REQUIRE(truth.evidenceAvailability ==
                TuiAuditEvidenceAvailability::Unavailable);
        REQUIRE_FALSE(truth.evidenceTruncated);

        auto model = ApplyTuiAuditRunReadResult(BaseModel(), result);
        model.view = TuiAuditView::Receipt;
        const auto rendered = RenderTuiAuditFrameText(
            model,
            TuiAuditFrameGeometry{120, 36},
            {});
        const auto expectedLabel = [&]() -> std::string_view {
            switch (state) {
                case OperationAuditRunReadState::Pending: return "pending";
                case OperationAuditRunReadState::Incomplete: return "incomplete";
                case OperationAuditRunReadState::Corrupt: return "corrupt";
                case OperationAuditRunReadState::Incompatible: return "incompatible";
                case OperationAuditRunReadState::Invalid: return "invalid";
                default: return "invalid";
            }
        }();
        REQUIRE(rendered.find("receipt=" + std::string(expectedLabel)) !=
                std::string::npos);
        REQUIRE(rendered.find("evidence: unavailable") != std::string::npos);
    }

    SECTION("in-flight receipt read is explicit") {
        auto model = BaseModel();
        model.view = TuiAuditView::Receipt;
        model.load = TuiAuditLoad::Loading;
        model.receiptState = TuiAuditReceiptState::Reading;
        model.receiptAbsenceReason = "bounded read in progress";
        const auto rendered = RenderTuiAuditFrameText(
            model,
            TuiAuditFrameGeometry{120, 36},
            {});
        REQUIRE(rendered.find("receipt=reading") != std::string::npos);
        REQUIRE(rendered.find(model.receiptAbsenceReason) != std::string::npos);
        REQUIRE(rendered.find("evidence: unavailable") != std::string::npos);
    }
}

TEST_CASE("audit receipt evidence availability is count-derived and bounded",
          "[unit][tui_audit_frame][tui_pr_focus][KG-BUG-0105]") {
    struct Scenario {
        std::uint64_t total;
        std::uint64_t retained;
        bool redacted;
        bool withheld;
        bool truncated;
        TuiAuditEvidenceAvailability expected;
        std::string_view compact;
        std::string_view full;
    };
    const std::array scenarios{
        Scenario{0, 0, false, false, false,
                 TuiAuditEvidenceAvailability::None,
                 "ev=none",
                 "evidence: none"},
        Scenario{3, 1, false, false, false,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{3, 3, false, false, false,
                 TuiAuditEvidenceAvailability::Retained,
                 "ev=kept",
                 "evidence: retained"},
        Scenario{3, 1, true, true, true,
                 TuiAuditEvidenceAvailability::Retained,
                 "ev=kept redacted withheld trunc",
                 "redacted | withheld | truncated"},
        Scenario{3, 1, false, false, true,
                 TuiAuditEvidenceAvailability::Retained,
                 "ev=kept",
                 "evidence: retained"},
        Scenario{3, 0, false, false, false,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{3, 0, true, false, true,
                 TuiAuditEvidenceAvailability::Unavailable,
                 "redacted trunc",
                 "redacted | truncated"},
        Scenario{3, 0, false, true, true,
                 TuiAuditEvidenceAvailability::Unavailable,
                 "withheld trunc",
                 "withheld | truncated"},
        Scenario{3, 0, false, false, true,
                 TuiAuditEvidenceAvailability::Unavailable, "trunc", "truncated"},
        Scenario{3, 0, true, true, true,
                 TuiAuditEvidenceAvailability::Unavailable,
                 "ev=unavail redacted withheld trunc",
                 "redacted | withheld | truncated"},
        Scenario{0, 0, true, false, false,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{0, 0, false, true, false,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{0, 0, false, false, true,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{2, 2, false, false, true,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{1, 2, false, false, false,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons",
                 "evidence: inconsistent"},
        Scenario{1, 2, true, true, true,
                 TuiAuditEvidenceAvailability::Inconsistent,
                 "ev=incons redacted withheld trunc",
                 "evidence: inconsistent"},
    };

    for (const auto& scenario : scenarios) {
        const auto result = VerifiedReadResultWithEvidence(
            scenario.total,
            scenario.retained,
            scenario.redacted,
            scenario.withheld,
            scenario.truncated);
        const auto truth = ProjectTuiAuditReceiptTruth(result);
        CAPTURE(scenario.total, scenario.retained, scenario.redacted,
                scenario.withheld, scenario.truncated);
        REQUIRE(truth.evidenceAvailability == scenario.expected);
        REQUIRE(truth.evidenceRedacted == scenario.redacted);
        REQUIRE(truth.evidenceWithheld == scenario.withheld);
        REQUIRE(truth.evidenceTruncated == scenario.truncated);

        auto model = ApplyTuiAuditRunReadResult(BaseModel(), result);
        model.view = TuiAuditView::Receipt;
        const auto full = RenderTuiAuditFrameText(
            model,
            TuiAuditFrameGeometry{120, 36},
            {});
        const auto compactGeometry = ComputeTuiAuditFrameGeometry(72, 22);
        const auto compact = RenderTuiAuditFrameText(model, compactGeometry, {});
        REQUIRE(full.find(scenario.full) != std::string::npos);
        REQUIRE(compact.find(scenario.compact) != std::string::npos);
        RequireBoundedFrame(full, {120, 36});
        RequireBoundedFrame(compact, compactGeometry);
    }

    struct UnrelatedTruncationScenario {
        OperationAuditRunReadState state;
        bool diagnostic;
        bool preview;
        bool events;
        bool repositories;
    };
    const std::array unrelatedTruncationScenarios{
        UnrelatedTruncationScenario{
            OperationAuditRunReadState::Truncated, false, false, false, false},
        UnrelatedTruncationScenario{
            OperationAuditRunReadState::Ready, true, false, false, false},
        UnrelatedTruncationScenario{
            OperationAuditRunReadState::Ready, false, true, false, false},
        UnrelatedTruncationScenario{
            OperationAuditRunReadState::Ready, false, false, true, false},
        UnrelatedTruncationScenario{
            OperationAuditRunReadState::Ready, false, false, false, true},
    };
    for (const auto& scenario : unrelatedTruncationScenarios) {
        auto result = VerifiedReadResultWithEvidence(0, 0, false, false, false);
        result.state = scenario.state;
        result.diagnosticTruncated = scenario.diagnostic;
        result.run->previewTruncated = scenario.preview;
        result.run->eventsTruncated = scenario.events;
        result.run->repositoriesTruncated = scenario.repositories;
        const auto truth = ProjectTuiAuditReceiptTruth(result);
        CAPTURE(static_cast<int>(scenario.state), scenario.diagnostic,
                scenario.preview, scenario.events, scenario.repositories);
        REQUIRE(truth.evidenceAvailability ==
                TuiAuditEvidenceAvailability::None);
        REQUIRE_FALSE(truth.evidenceTruncated);

        const auto model = ApplyTuiAuditRunReadResult(BaseModel(), result);
        const auto rendered = RenderTuiAuditFrameText(
            model,
            TuiAuditFrameGeometry{120, 36},
            {});
        REQUIRE(rendered.find("evidence: none") != std::string::npos);
        REQUIRE(rendered.find("evidence: none | truncated") ==
                std::string::npos);
    }
}
