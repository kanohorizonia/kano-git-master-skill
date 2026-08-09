#include "tui_audit_frame.hpp"

#include "tui_keymap.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace kano::git::commands {
namespace {

auto ViewLabel(const TuiAuditView InView) -> std::string_view {
    switch (InView) {
        case TuiAuditView::Startup:
            return "Startup Audit";
        case TuiAuditView::Normal:
            return "Repository Audit";
        case TuiAuditView::Help:
            return "Audit Help";
        case TuiAuditView::Discover:
            return "Discovery Audit";
        case TuiAuditView::History:
            return "History Audit";
        case TuiAuditView::Detail:
            return "Detail Audit";
        case TuiAuditView::Receipt:
            return "Audit Receipt";
        case TuiAuditView::Preview:
            return "Command Audit";
        case TuiAuditView::Command:
            return "Audit Command";
        case TuiAuditView::Palette:
            return "Audit Palette";
    }
    return "Audit";
}

auto LoadLabel(const TuiAuditLoad InLoad) -> std::string_view {
    switch (InLoad) {
        case TuiAuditLoad::Idle:
            return "idle";
        case TuiAuditLoad::Loading:
            return "loading";
        case TuiAuditLoad::Empty:
            return "empty";
        case TuiAuditLoad::Failed:
            return "failed";
        case TuiAuditLoad::Cancelled:
            return "cancelled";
        case TuiAuditLoad::Ready:
            return "ready";
    }
    return "unknown";
}

auto KeyContextForView(const TuiAuditView InView) -> TuiKeyContext {
    if (InView == TuiAuditView::History) {
        return TuiKeyContext::History;
    }
    if (InView == TuiAuditView::Discover) {
        return TuiKeyContext::Discover;
    }
    if (InView == TuiAuditView::Detail) {
        return TuiKeyContext::Detail;
    }
    if (InView == TuiAuditView::Receipt || InView == TuiAuditView::Preview) {
        return TuiKeyContext::Preview;
    }
    if (InView == TuiAuditView::Command) {
        return TuiKeyContext::Command;
    }
    if (InView == TuiAuditView::Palette) {
        return TuiKeyContext::Palette;
    }
    return TuiKeyContext::Normal;
}

auto GuidanceForView(const TuiAuditView InView) -> std::string {
    return std::string(GetTuiKeyGuidance(KeyContextForView(InView)).controls);
}

auto ExitAffordance(const TuiAuditView InView) -> std::string_view {
    switch (InView) {
        case TuiAuditView::Startup:
        case TuiAuditView::Normal:
            return "q quit";
        case TuiAuditView::History:
        case TuiAuditView::Detail:
            return "Esc/q back";
        case TuiAuditView::Help:
        case TuiAuditView::Discover:
        case TuiAuditView::Receipt:
        case TuiAuditView::Preview:
            return "Esc/q close";
        case TuiAuditView::Command:
            return "Esc cancel";
        case TuiAuditView::Palette:
            return "Esc close";
    }
    return "Esc/q close";
}

auto BoundedText(const std::string_view InValue,
                 const std::size_t InMaximum = 192U) -> std::string {
    if (InValue.size() <= InMaximum) {
        return std::string(InValue);
    }
    return std::string(InValue.substr(0U, InMaximum - 3U)) + "...";
}

auto CorrelationText(const audit::CorrelationRefs& InCorrelation) -> std::string {
    if (InCorrelation.mode == audit::CorrelationMode::Standalone) {
        return "standalone";
    }

    std::string text = "koa";
    const std::array<std::pair<std::string_view, const std::optional<std::string>*>, 8> fields{{
        std::pair<std::string_view, const std::optional<std::string>*>{
            "product", &InCorrelation.productId},
        {"topic", &InCorrelation.topicId},
        {"item", &InCorrelation.itemId},
        {"work-order", &InCorrelation.workOrderId},
        {"request", &InCorrelation.requestId},
        {"producer", &InCorrelation.producerId},
        {"route", &InCorrelation.routeId},
        {"agent", &InCorrelation.agentId},
    }};
    for (const auto& [label, value] : fields) {
        if (!value->has_value()) {
            continue;
        }
        text += " | ";
        text += label;
        text += "=";
        text += **value;
    }
    return BoundedText(text);
}

auto ShortHead(const std::optional<std::string>& InHead) -> std::string {
    if (!InHead.has_value()) {
        return "unknown";
    }
    return InHead->substr(0U, std::min<std::size_t>(12U, InHead->size()));
}

auto RepositoryText(const OperationAuditRunProjection& InRun) -> std::string {
    if (InRun.repositories.empty()) {
        return InRun.totalRepositories == 0U
            ? std::string("none recorded")
            : "withheld " + std::to_string(InRun.totalRepositories) +
                " repositories";
    }

    const auto& repository = InRun.repositories.front();
    auto text = repository.repositoryId + " " +
        ShortHead(repository.before.headSha) + "->" +
        ShortHead(repository.after.headSha);
    if (InRun.totalRepositories > 1U) {
        text += " +" + std::to_string(InRun.totalRepositories - 1U);
    }
    return BoundedText(text);
}

auto ReadStateReason(const OperationAuditRunReadState InState) -> std::string_view {
    switch (InState) {
        case OperationAuditRunReadState::Pending:
            return "audit receipt pending";
        case OperationAuditRunReadState::Incomplete:
            return "audit receipt incomplete";
        case OperationAuditRunReadState::Corrupt:
            return "audit receipt corrupt";
        case OperationAuditRunReadState::Incompatible:
            return "audit receipt incompatible";
        case OperationAuditRunReadState::Invalid:
            return "audit receipt invalid";
        case OperationAuditRunReadState::Missing:
            return "audit receipt missing";
        case OperationAuditRunReadState::Ready:
        case OperationAuditRunReadState::Truncated:
            return "audit receipt unavailable";
    }
    return "audit receipt unavailable";
}

auto StripTerminalSequences(const std::string_view InText) -> std::string {
    std::string plain;
    plain.reserve(InText.size());
    for (std::size_t index = 0; index < InText.size();) {
        const auto value = static_cast<unsigned char>(InText[index]);
        if (value == '\r') {
            ++index;
            continue;
        }
        if (value != 0x1bU) {
            plain.push_back(InText[index++]);
            continue;
        }

        ++index;
        if (index >= InText.size()) {
            break;
        }
        if (InText[index] == '[') {
            ++index;
            while (index < InText.size()) {
                const auto finalByte =
                    static_cast<unsigned char>(InText[index++]);
                if (finalByte >= 0x40U && finalByte <= 0x7eU) {
                    break;
                }
            }
            continue;
        }
        if (InText[index] == ']') {
            ++index;
            while (index < InText.size()) {
                if (InText[index] == '\a') {
                    ++index;
                    break;
                }
                if (InText[index] == '\x1b' &&
                    index + 1U < InText.size() &&
                    InText[index + 1U] == '\\') {
                    index += 2U;
                    break;
                }
                ++index;
            }
            continue;
        }
        ++index;
    }
    return plain;
}

auto AuditFrameColor(const TuiColor InColor) -> ftxui::Color {
    switch (InColor) {
        case TuiColor::TerminalDefault: return ftxui::Color::Default;
        case TuiColor::Black: return ftxui::Color::Black;
        case TuiColor::Red: return ftxui::Color::Red;
        case TuiColor::Green: return ftxui::Color::Green;
        case TuiColor::Yellow: return ftxui::Color::Yellow;
        case TuiColor::Blue: return ftxui::Color::Blue;
        case TuiColor::Magenta: return ftxui::Color::Magenta;
        case TuiColor::Cyan: return ftxui::Color::Cyan;
        case TuiColor::White: return ftxui::Color::GrayLight;
        case TuiColor::BrightBlack: return ftxui::Color::GrayDark;
        case TuiColor::BrightRed: return ftxui::Color::RedLight;
        case TuiColor::BrightGreen: return ftxui::Color::GreenLight;
        case TuiColor::BrightYellow: return ftxui::Color::YellowLight;
        case TuiColor::BrightBlue: return ftxui::Color::BlueLight;
        case TuiColor::BrightMagenta: return ftxui::Color::MagentaLight;
        case TuiColor::BrightCyan: return ftxui::Color::CyanLight;
        case TuiColor::BrightWhite: return ftxui::Color::White;
    }
    return ftxui::Color::Default;
}

auto AuditFrameDecorator(const TuiTextStyle& InStyle) -> ftxui::Decorator {
    return [InStyle](ftxui::Element InElement) {
        auto element = std::move(InElement);
        if (InStyle.foreground != TuiColor::TerminalDefault) {
            element = element |
                ftxui::color(AuditFrameColor(InStyle.foreground));
        }
        if (InStyle.background != TuiColor::TerminalDefault) {
            element = element |
                ftxui::bgcolor(AuditFrameColor(InStyle.background));
        }
        if (InStyle.bold) element = element | ftxui::bold;
        if (InStyle.dim) element = element | ftxui::dim;
        if (InStyle.inverted) element = element | ftxui::inverted;
        return element;
    };
}

auto LoadDecorator(const TuiAuditLoad InLoad,
                   const TuiAuditSemanticTheme& InTheme)
    -> ftxui::Decorator {
    if (InTheme.mono) {
        if (InLoad == TuiAuditLoad::Failed ||
            InLoad == TuiAuditLoad::Cancelled) {
            return ftxui::inverted;
        }
        if (InLoad == TuiAuditLoad::Loading) {
            return ftxui::bold;
        }
        return ftxui::nothing;
    }
    const auto* style = &InTheme.palette.primary;
    switch (InLoad) {
        case TuiAuditLoad::Failed: style = &InTheme.palette.error; break;
        case TuiAuditLoad::Cancelled:
        case TuiAuditLoad::Empty: style = &InTheme.palette.warning; break;
        case TuiAuditLoad::Loading: style = &InTheme.palette.running; break;
        case TuiAuditLoad::Ready: style = &InTheme.palette.success; break;
        case TuiAuditLoad::Idle: break;
    }
    return AuditFrameDecorator(*style);
}

auto ReceiptStateLabel(TuiAuditReceiptState InState) -> std::string_view;

auto AuditEvidenceLine(const TuiAuditFrameModel& InModel) -> std::string {
    if (InModel.receiptState == TuiAuditReceiptState::Linked) {
        return "audit: run=" +
            (InModel.runId.empty() ? std::string("unknown") : InModel.runId) +
            " | receipt=" +
            (InModel.receiptId.empty() ? std::string("unknown") : InModel.receiptId) +
            " | correlation=" +
            (InModel.correlation.empty()
                 ? std::string("unknown")
                 : InModel.correlation);
    }
    return "audit: receipt=" +
        std::string(ReceiptStateLabel(InModel.receiptState)) +
        " | reason=" +
        (InModel.receiptAbsenceReason.empty()
             ? std::string("not provided")
             : InModel.receiptAbsenceReason);
}

auto ReceiptStateLabel(const TuiAuditReceiptState InState) -> std::string_view {
    switch (InState) {
        case TuiAuditReceiptState::Linked:
            return "linked";
        case TuiAuditReceiptState::Reading:
            return "reading";
        case TuiAuditReceiptState::Missing:
            return "missing";
        case TuiAuditReceiptState::Pending:
            return "pending";
        case TuiAuditReceiptState::Incomplete:
            return "incomplete";
        case TuiAuditReceiptState::Corrupt:
            return "corrupt";
        case TuiAuditReceiptState::Incompatible:
            return "incompatible";
        case TuiAuditReceiptState::Invalid:
            return "invalid";
    }
    return "unknown";
}

auto FitLine(const std::string_view InText, const int InWidth) -> std::string {
    const auto maximum = std::max(1, InWidth);
    if (ftxui::string_width(std::string(InText)) <= maximum) {
        return std::string(InText);
    }
    if (maximum <= 3) {
        return std::string(static_cast<std::size_t>(maximum), '.');
    }
    std::string clipped;
    for (const auto& glyph : ftxui::Utf8ToGlyphs(std::string(InText))) {
        if (glyph.empty() ||
            ftxui::string_width(clipped) + ftxui::string_width(glyph) > maximum - 3) {
            break;
        }
        clipped += glyph;
    }
    return clipped + "...";
}

auto CompactGuidanceForView(const TuiAuditView InView) -> std::string_view {
    return GetTuiKeyGuidance(KeyContextForView(InView)).compactControls;
}

auto CompactEvidenceLine(const TuiAuditFrameModel& InModel) -> std::string {
    std::string line = InModel.evidenceAvailable ? "ev=retained" : "ev=none";
    if (InModel.evidenceRedacted) {
        line += " redacted";
    }
    if (InModel.evidenceWithheld) {
        line += " withheld";
    }
    if (InModel.evidenceTruncated) {
        line += " trunc";
    }
    return line;
}

auto CompactAuditIdentity(const std::string_view InIdentity) -> std::string {
    constexpr std::size_t kMaximum = 10U;
    if (InIdentity.empty()) {
        return "unknown";
    }
    if (InIdentity.size() <= kMaximum) {
        return std::string(InIdentity);
    }
    // Run IDs and receipt hashes are protocol-constrained ASCII identities.
    // Retain both ends so a narrow frame stays useful for audit correlation
    // instead of clipping the receipt label entirely.
    return std::string(InIdentity.substr(0U, 5U)) + ".." +
        std::string(InIdentity.substr(InIdentity.size() - 3U));
}

} // namespace

auto ProjectTuiAuditReceiptTruth(
    const std::optional<OperationAuditRunReadResult>& InReadResult)
    -> TuiAuditReceiptTruth {
    if (!InReadResult.has_value()) {
        return {.state = TuiAuditReceiptState::Missing,
                .load = TuiAuditLoad::Empty,
                .receiptAbsenceReason = "no audit receipt read"};
    }

    const auto& result = *InReadResult;
    if (result.verified()) {
        const auto& run = *result.run;
        return {
            .state = TuiAuditReceiptState::Linked,
            .load = TuiAuditLoad::Ready,
            .runId = run.runId,
            .receiptId = run.receiptId,
            .correlation = CorrelationText(run.correlation),
            .repository = RepositoryText(run),
            .evidenceAvailable = true,
            .evidenceRedacted = run.hasRedactedEvidence,
            .evidenceWithheld = run.hasWithheldEvidence,
            .evidenceTruncated =
                result.state == OperationAuditRunReadState::Truncated ||
                result.diagnosticTruncated || run.previewTruncated ||
                run.eventsTruncated || run.repositoriesTruncated ||
                run.evidenceTruncated,
        };
    }

    const auto reason = BoundedText(
        result.diagnostic.empty() ? ReadStateReason(result.state) : result.diagnostic);
    const auto state = [&]() {
        switch (result.state) {
            case OperationAuditRunReadState::Missing:
                return TuiAuditReceiptState::Missing;
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
            case OperationAuditRunReadState::Ready:
            case OperationAuditRunReadState::Truncated:
                return TuiAuditReceiptState::Invalid;
        }
        return TuiAuditReceiptState::Invalid;
    }();
    return {
        .state = state,
        .load = result.state == OperationAuditRunReadState::Missing
            ? TuiAuditLoad::Empty
            : (result.state == OperationAuditRunReadState::Pending
                   ? TuiAuditLoad::Loading
                   : TuiAuditLoad::Failed),
        .receiptAbsenceReason = reason,
        .evidenceTruncated = result.diagnosticTruncated,
    };
}

auto ApplyTuiAuditRunReadResult(
    TuiAuditFrameModel InModel,
    const std::optional<OperationAuditRunReadResult>& InReadResult)
    -> TuiAuditFrameModel {
    if (!InReadResult.has_value()) {
        return InModel;
    }
    const auto truth = ProjectTuiAuditReceiptTruth(InReadResult);
    InModel.load = truth.load;
    InModel.receiptState = truth.state;
    InModel.runId = truth.runId;
    InModel.receiptId = truth.receiptId;
    InModel.correlation = truth.correlation;
    InModel.scope = "workspace";
    InModel.repository = truth.repository;
    InModel.receiptAbsenceReason = truth.receiptAbsenceReason;
    InModel.evidenceAvailable = truth.evidenceAvailable;
    InModel.evidenceRedacted = truth.evidenceRedacted;
    InModel.evidenceWithheld = truth.evidenceWithheld;
    InModel.evidenceTruncated = truth.evidenceTruncated;
    return InModel;
}

auto ComputeTuiAuditFrameGeometry(const int InTerminalWidth,
                                  const int InTerminalHeight,
                                  const bool bInMono) -> TuiAuditFrameGeometry {
    (void)bInMono;
    return {
        .width = std::max(20, InTerminalWidth / 2),
        .height = InTerminalHeight <= 24
            ? 11
            : std::max(11, std::min(14, InTerminalHeight / 2)),
    };
}

auto ComputeTuiAuditDashboardGeometry(const int InTerminalWidth,
                                      const int InTerminalHeight,
                                      const bool bInCommandMode,
                                      const bool bInMono)
    -> TuiAuditDashboardGeometry {
    const bool compactRoot = InTerminalHeight <= 24;
    const auto frame = ComputeTuiAuditFrameGeometry(
        InTerminalWidth,
        InTerminalHeight,
        bInMono);
    // Compact production composition keeps only the outer border (two rows)
    // and, in command mode, the bordered input (three rows).  This arithmetic
    // is shared with the runner and contract tests so the frame cannot claim
    // rows that production will clip.
    const int rootBorderRows = compactRoot ? 2 : 0;
    const int commandRows = compactRoot && bInCommandMode ? 3 : 0;
    const int mainHeight = compactRoot
        ? std::max(0, InTerminalHeight - rootBorderRows - commandRows)
        : 0;
    return {
        .frame = frame,
        .compactRoot = compactRoot,
        .mainHeight = mainHeight,
        .rightPanelContentHeight = compactRoot
            ? std::max(0, mainHeight - frame.height)
            : 0,
    };
}

auto RenderTuiAuditFrame(const TuiAuditFrameModel& InModel,
                         const TuiAuditFrameGeometry& InGeometry,
                         const TuiAuditSemanticTheme& InTheme)
    -> ftxui::Element {
    using namespace ftxui;

    const auto width = std::max(20, InGeometry.width);
    const auto height = std::max(8, InGeometry.height);
    const bool narrow = width < 80;

    Elements rows;
    if (narrow) {
        const auto contentWidth = width - 2;
        rows.push_back(text(FitLine(
            "AUDIT " + std::string(LoadLabel(InModel.load)) +
                " receipt=" +
                std::string(ReceiptStateLabel(InModel.receiptState)),
            contentWidth)) | bold);
        rows.push_back(text(FitLine(
            "scope=" + InModel.scope + " repo=" + InModel.repository,
            contentWidth)));
        rows.push_back(text(FitLine(
            "source=" + InModel.provenanceSource +
            " fresh=" + InModel.provenanceFreshness,
            contentWidth)));
        if (InModel.receiptState == TuiAuditReceiptState::Linked) {
            rows.push_back(text(FitLine(
                "run=" + CompactAuditIdentity(InModel.runId) +
                    " receipt=" + CompactAuditIdentity(InModel.receiptId),
                contentWidth)));
            rows.push_back(text(FitLine("corr=" + InModel.correlation, contentWidth)));
        } else {
            rows.push_back(text(FitLine(
                "receipt=" + std::string(ReceiptStateLabel(InModel.receiptState)) +
                " reason=" + InModel.receiptAbsenceReason,
                contentWidth)));
        }
        rows.push_back(text(FitLine(CompactEvidenceLine(InModel), contentWidth)));
        rows.push_back(text(FitLine("next=" + InModel.nextAction, contentWidth)));
        rows.push_back(text(FitLine(
            "keys=" + std::string(CompactGuidanceForView(InModel.view)),
            contentWidth)));
        return vbox(std::move(rows)) |
            border |
            size(WIDTH, EQUAL, width) |
            size(HEIGHT, EQUAL, height);
    }

    rows.push_back(hbox({
        text(std::string(ViewLabel(InModel.view))) | bold,
        filler(),
        text(std::string(LoadLabel(InModel.load))) |
            LoadDecorator(InModel.load, InTheme),
    }));
    rows.push_back(separator());
    rows.push_back(paragraph(
        "scope: " + InModel.scope + " | repo: " + InModel.repository));
    rows.push_back(paragraph(
        "inventory: source=" + InModel.provenanceSource +
        " | freshness=" + InModel.provenanceFreshness +
        " | observed=" + InModel.observedAtUtc));
    rows.push_back(paragraph(AuditEvidenceLine(InModel)));

    std::string evidence = InModel.evidenceAvailable
        ? "evidence: retained preview"
        : "evidence: not projected in this view";
    if (InModel.evidenceRedacted) {
        evidence += " | redacted";
    }
    if (InModel.evidenceWithheld) {
        evidence += " | withheld";
    }
    if (InModel.evidenceTruncated) {
        evidence += " | truncated";
    }
    rows.push_back(paragraph(evidence));

    // Exit affordances remain above diagnostics so a short/narrow frame never
    // hides the safe way back from an error or loading state.
    rows.push_back(paragraph("next: " + InModel.nextAction));
    rows.push_back(paragraph("exit: " + std::string(ExitAffordance(InModel.view))));
    if (InModel.view == TuiAuditView::Help) {
        for (const auto& guidance : GetAllTuiKeyGuidance()) {
            rows.push_back(paragraph(
                "keys " + std::string(guidance.label) + ": " +
                std::string(guidance.controls)));
        }
    } else {
        rows.push_back(paragraph("keys: " + GuidanceForView(InModel.view)));
    }

    if (!InModel.diagnostic.empty()) {
        rows.push_back(paragraph("diagnostic: " + InModel.diagnostic));
    }
    if (!InModel.hint.empty()) {
        rows.push_back(paragraph("hint: " + InModel.hint));
    }
    if (!InModel.footer.empty()) {
        rows.push_back(paragraph("status: " + InModel.footer));
    }

    return vbox(std::move(rows)) |
        border |
        size(WIDTH, EQUAL, width) |
        size(HEIGHT, EQUAL, height);
}

auto RenderTuiAuditFrameText(const TuiAuditFrameModel& InModel,
                             const TuiAuditFrameGeometry& InGeometry,
                             const TuiAuditSemanticTheme& InTheme)
    -> std::string {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(std::max(1, InGeometry.width)),
        ftxui::Dimension::Fixed(std::max(1, InGeometry.height)));
    ftxui::Render(
        screen,
        RenderTuiAuditFrame(InModel, InGeometry, InTheme));
    // Screen::ToString() intentionally contains terminal styling.  The
    // deterministic contract surface keeps layout/text while removing those
    // transport sequences, so tests never depend on a terminal or RGB bytes.
    return StripTerminalSequences(screen.ToString());
}

auto ComposeTuiAuditCompactRoot(ftxui::Element InMainPanel,
                                ftxui::Elements InCommandRows)
    -> ftxui::Element {
    ftxui::Elements rows;
    rows.push_back(std::move(InMainPanel) | ftxui::flex);
    if (!InCommandRows.empty()) {
        rows.push_back(ftxui::vbox(std::move(InCommandRows)));
    }
    return ftxui::vbox(std::move(rows)) | ftxui::border;
}

auto RenderTuiAuditCompactRootText(ftxui::Element InMainPanel,
                                   ftxui::Elements InCommandRows,
                                   const int InWidth,
                                   const int InHeight) -> std::string {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(std::max(1, InWidth)),
        ftxui::Dimension::Fixed(std::max(1, InHeight)));
    ftxui::Render(
        screen,
        ComposeTuiAuditCompactRoot(
            std::move(InMainPanel),
            std::move(InCommandRows)));
    return StripTerminalSequences(screen.ToString());
}

} // namespace kano::git::commands
