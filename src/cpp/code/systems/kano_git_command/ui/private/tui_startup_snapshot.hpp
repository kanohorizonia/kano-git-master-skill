#pragma once

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kano::git::commands {

struct TuiStartupRepoSnapshot {
    std::filesystem::path path;
    std::filesystem::path parentPath;
    std::string relativePath;
    std::string type;
    std::string branch;
    std::string upstream;
    std::string tracking;
    bool statusKnown = true;
    bool repoDirty = false;
    bool worktreeDirty = false;
};

struct TuiStartupSnapshotLoadOptions {
    std::string binaryCommand;
    std::filesystem::path workspaceRoot;
    unsigned int timeoutMs = 5000;
    std::size_t maxCaptureBytes = 4U * 1024U * 1024U;
};

struct TuiStartupSnapshotMetadata {
    std::string source;
    std::string completeness;
    std::string probeMode;
    bool statusKnown = true;
    std::optional<std::chrono::system_clock::time_point> observedAtUtc;
    std::optional<std::string> observedAtUtcText;
    std::vector<std::filesystem::path> allowedExternalRoots;
};

enum class TuiStartupSnapshotFreshness {
    Unknown,
    Fresh,
    Stale,
    Future,
};

/// One immutable projection travels from the background operation to the
/// dashboard. It prevents cached metadata from being relabelled as live.
struct TuiStartupInventoryProvenance {
    TuiStartupSnapshotMetadata metadata;
    TuiStartupSnapshotFreshness freshness = TuiStartupSnapshotFreshness::Unknown;
    bool live = false;
};

/// Status certainty is accumulated for every discovered candidate, before a
/// display filter (for example dirty-only) may hide that candidate.  This
/// prevents a failed probe from being silently omitted from a "known" live
/// inventory claim.
struct TuiLiveInventoryStatusSummary {
    std::size_t candidateCount = 0;
    bool statusKnown = true;
};

struct TuiDirtyFilterProbeResult {
    bool dirty = false;
    bool statusKnown = false;
};

[[nodiscard]] auto MakeTuiDirtyFilterProbeResult(
    int InExitCode,
    std::string_view InOutput) -> TuiDirtyFilterProbeResult;
[[nodiscard]] auto ResolveTuiLiveCandidateStatusKnown(
    const TuiDirtyFilterProbeResult& InFilterProbe,
    bool bInLaterProbeStatusKnown) -> bool;

auto ObserveTuiLiveInventoryStatus(
    TuiLiveInventoryStatusSummary& InOutSummary,
    bool bInCandidateStatusKnown) -> void;

[[nodiscard]] auto MakeTuiLiveInventoryProvenance(
    bool bInDirtyFiltered,
    bool bInStatusKnown,
    std::chrono::system_clock::time_point InObservedAtUtc)
    -> TuiStartupInventoryProvenance;
[[nodiscard]] auto MakeTuiUnknownInventoryProvenance()
    -> TuiStartupInventoryProvenance;
[[nodiscard]] auto RetainTuiInventoryProvenanceAfterFailure(
    TuiStartupInventoryProvenance InPrevious) -> TuiStartupInventoryProvenance;
[[nodiscard]] auto FormatTuiInventoryProvenanceFull(
    const TuiStartupInventoryProvenance& InProvenance) -> std::string;
[[nodiscard]] auto FormatTuiInventoryProvenanceCompact(
    const TuiStartupInventoryProvenance& InProvenance) -> std::string;

constexpr std::size_t kTuiStartupMaxRepos = 4096;
constexpr std::size_t kTuiStartupMaxFieldBytes = 16U * 1024U;
constexpr std::size_t kTuiStartupMaxExternalRoots = 256;
constexpr auto kTuiStartupInventoryMaximumAge = std::chrono::minutes{15};

[[nodiscard]] auto TuiAuditBooleanLabel(
    bool bInKnown,
    bool bInValue) -> std::string_view;

/// Strictly parse the bounded UTC form emitted by the inventory manifest:
/// YYYY-MM-DDTHH:MM:SSZ. Generic document timestamps are never accepted as
/// inventory provenance by callers.
[[nodiscard]] auto ParseTuiObservedAtUtc(
    std::string_view InValue) -> std::optional<std::chrono::system_clock::time_point>;

[[nodiscard]] auto ClassifyTuiStartupSnapshotFreshness(
    const TuiStartupSnapshotMetadata& InMetadata,
    std::chrono::system_clock::time_point InNow,
    std::chrono::seconds InMaximumAge) -> TuiStartupSnapshotFreshness;

[[nodiscard]] auto TuiStartupSnapshotFreshnessLabel(
    TuiStartupSnapshotFreshness InFreshness) -> std::string_view;

[[nodiscard]] auto ParseTuiStartupSnapshotJson(
    std::string_view InJson,
    const std::filesystem::path& InWorkspaceRoot,
    std::string* OutError = nullptr,
    TuiStartupSnapshotMetadata* OutMetadata = nullptr)
    -> std::vector<TuiStartupRepoSnapshot>;

[[nodiscard]] auto LoadTuiStartupSnapshot(
    const TuiStartupSnapshotLoadOptions& InOptions,
    TuiStartupSnapshotMetadata* OutMetadata = nullptr)
    -> std::vector<TuiStartupRepoSnapshot>;

}  // namespace kano::git::commands
