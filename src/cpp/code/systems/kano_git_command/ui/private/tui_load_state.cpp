#include "tui_load_state.hpp"

#include <algorithm>
#include <utility>

namespace kano::git::commands {
namespace {

auto Utf8SafePrefixLength(
    const std::string& InText,
    const std::size_t InMaxBytes) -> std::size_t {
    if (InText.size() <= InMaxBytes) {
        return InText.size();
    }

    std::size_t end = InMaxBytes;
    while (end > 0 &&
           (static_cast<unsigned char>(InText[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return end;
}

}  // namespace

auto TuiLoadPhaseName(const TuiLoadPhase InPhase) -> std::string_view {
    switch (InPhase) {
        case TuiLoadPhase::Idle:
            return "idle";
        case TuiLoadPhase::Loading:
            return "loading";
        case TuiLoadPhase::Ready:
            return "ready";
        case TuiLoadPhase::Empty:
            return "empty";
        case TuiLoadPhase::Cancelled:
            return "cancelled";
        case TuiLoadPhase::Failed:
            return "failed";
    }
    return "failed";
}

auto BeginTuiLoad(
    TuiLoadState& InOutState,
    const std::uint64_t InGeneration,
    std::string InOperation,
    std::string InHint) -> void {
    InOutState = TuiLoadState{
        .generation = InGeneration,
        .phase = TuiLoadPhase::Loading,
        .operation = std::move(InOperation),
        .diagnostic = {},
        .hint = std::move(InHint),
    };
}

auto CompleteTuiLoad(
    TuiLoadState& InOutState,
    const std::uint64_t InGeneration,
    const std::size_t InItemCount) -> bool {
    if (!IsCurrentTuiLoad(InOutState, InGeneration)) {
        return false;
    }
    InOutState.phase = InItemCount == 0
        ? TuiLoadPhase::Empty
        : TuiLoadPhase::Ready;
    InOutState.diagnostic.clear();
    return true;
}

auto FailTuiLoad(
    TuiLoadState& InOutState,
    const std::uint64_t InGeneration,
    const std::string_view InDiagnostic,
    const bool bInCancelled) -> bool {
    if (!IsCurrentTuiLoad(InOutState, InGeneration)) {
        return false;
    }
    InOutState.phase = bInCancelled
        ? TuiLoadPhase::Cancelled
        : TuiLoadPhase::Failed;
    InOutState.diagnostic = BoundTuiLoadDiagnostic(InDiagnostic);
    return true;
}

auto IsCurrentTuiLoad(
    const TuiLoadState& InState,
    const std::uint64_t InGeneration) -> bool {
    return InGeneration != 0 &&
        InState.generation == InGeneration &&
        InState.phase == TuiLoadPhase::Loading;
}

auto BoundTuiLoadDiagnostic(const std::string_view InDiagnostic)
    -> std::string {
    std::string sanitized;
    sanitized.reserve(std::min(
        InDiagnostic.size(),
        kTuiLoadDiagnosticMaxBytes));
    bool previousWasSpace = false;
    for (const unsigned char ch : InDiagnostic) {
        const bool isWhitespace = ch == '\n' || ch == '\r' || ch == '\t';
        const bool isControl = ch < 0x20U || ch == 0x7FU;
        const char rendered = isWhitespace
            ? ' '
            : (isControl ? '?' : static_cast<char>(ch));
        if (rendered == ' ' && previousWasSpace) {
            continue;
        }
        sanitized.push_back(rendered);
        previousWasSpace = rendered == ' ';
    }

    if (sanitized.size() <= kTuiLoadDiagnosticMaxBytes) {
        return sanitized;
    }
    constexpr std::string_view suffix = "... [bounded]";
    const auto prefixBudget =
        kTuiLoadDiagnosticMaxBytes - suffix.size();
    const auto prefixLength = Utf8SafePrefixLength(
        sanitized,
        prefixBudget);
    sanitized.resize(prefixLength);
    sanitized.append(suffix);
    return sanitized;
}

auto SetTuiInventoryProvenance(TuiLoadState& InOutState,
                               const TuiInventoryProvenance InProvenance) -> void {
    InOutState.inventoryProvenance = InProvenance;
    InOutState.retainedPriorRows = false;
}

auto RetainTuiLoadRowsOnFailure(TuiLoadState& InOutState,
                                const std::string_view InDiagnostic,
                                std::string InRetryHint,
                                const bool bInCancelled) -> void {
    InOutState.phase = bInCancelled
        ? TuiLoadPhase::Cancelled
        : TuiLoadPhase::Failed;
    InOutState.diagnostic = BoundTuiLoadDiagnostic(InDiagnostic);
    InOutState.hint = BoundTuiLoadDiagnostic(InRetryHint);
    InOutState.retainedPriorRows = true;
    // A failed refresh must never leave cached/root-fallback rows labelled live.
    if (InOutState.inventoryProvenance == TuiInventoryProvenance::Live) {
        InOutState.inventoryProvenance = TuiInventoryProvenance::Unknown;
    }
}

auto TuiInventoryProvenanceLabel(const TuiInventoryProvenance InProvenance)
    -> std::string_view {
    switch (InProvenance) {
    case TuiInventoryProvenance::Unknown: return "non-live";
    case TuiInventoryProvenance::Cache: return "cached";
    case TuiInventoryProvenance::RootFallback: return "root-fallback";
    case TuiInventoryProvenance::Live: return "live";
    }
    return "non-live";
}

}  // namespace kano::git::commands
