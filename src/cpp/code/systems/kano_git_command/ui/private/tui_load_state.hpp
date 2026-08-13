#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kano::git::commands {

enum class TuiLoadPhase {
    Idle,
    Loading,
    Ready,
    Empty,
    Cancelled,
    Failed,
};

enum class TuiInventoryProvenance {
    Unknown,
    Cache,
    RootFallback,
    Live,
};

struct TuiLoadState {
    std::uint64_t generation = 0;
    TuiLoadPhase phase = TuiLoadPhase::Idle;
    std::string operation;
    std::string diagnostic;
    std::string hint;
    TuiInventoryProvenance inventoryProvenance = TuiInventoryProvenance::Unknown;
    bool retainedPriorRows = false;
};

constexpr std::size_t kTuiLoadDiagnosticMaxBytes = 320;

[[nodiscard]] auto TuiLoadPhaseName(TuiLoadPhase InPhase)
    -> std::string_view;

auto BeginTuiLoad(
    TuiLoadState& InOutState,
    std::uint64_t InGeneration,
    std::string InOperation,
    std::string InHint) -> void;

[[nodiscard]] auto CompleteTuiLoad(
    TuiLoadState& InOutState,
    std::uint64_t InGeneration,
    std::size_t InItemCount) -> bool;

[[nodiscard]] auto FailTuiLoad(
    TuiLoadState& InOutState,
    std::uint64_t InGeneration,
    std::string_view InDiagnostic,
    bool bInCancelled) -> bool;

[[nodiscard]] auto IsCurrentTuiLoad(
    const TuiLoadState& InState,
    std::uint64_t InGeneration) -> bool;

[[nodiscard]] auto BoundTuiLoadDiagnostic(std::string_view InDiagnostic)
    -> std::string;

auto SetTuiInventoryProvenance(TuiLoadState& InOutState,
                               TuiInventoryProvenance InProvenance) -> void;
auto RetainTuiLoadRowsOnFailure(TuiLoadState& InOutState,
                                std::string_view InDiagnostic,
                                std::string InRetryHint,
                                bool bInCancelled) -> void;
[[nodiscard]] auto TuiInventoryProvenanceLabel(
    TuiInventoryProvenance InProvenance) -> std::string_view;

}  // namespace kano::git::commands
