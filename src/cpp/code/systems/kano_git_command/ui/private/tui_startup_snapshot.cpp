#include "tui_startup_snapshot.hpp"

#include "shell_executor.hpp"
#include "tui_load_state.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace kano::git::commands {
namespace {

auto IsLeapYear(const int InYear) -> bool {
    return InYear % 4 == 0 && (InYear % 100 != 0 || InYear % 400 == 0);
}

auto DaysInMonth(const int InYear, const int InMonth) -> int {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
    if (InMonth == 2 && IsLeapYear(InYear)) {
        return 29;
    }
    return kDays[InMonth - 1];
}

auto ParseFixedDigits(const std::string_view InValue, const std::size_t InStart,
                      const std::size_t InCount, int* OutValue) -> bool {
    int value = 0;
    for (std::size_t index = 0; index < InCount; ++index) {
        const char ch = InValue[InStart + index];
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + (ch - '0');
    }
    *OutValue = value;
    return true;
}

// Howard Hinnant's civil-date conversion, kept local to make parsing timezone
// independent and deterministic in tests.
auto DaysSinceUnixEpoch(int InYear, const unsigned InMonth, const unsigned InDay) -> long long {
    InYear -= InMonth <= 2;
    const int era = (InYear >= 0 ? InYear : InYear - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(InYear - era * 400);
    const unsigned doy = (153 * (InMonth + (InMonth > 2 ? -3 : 9)) + 2) / 5 + InDay - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
}

auto SetError(std::string* OutError, std::string InMessage) -> void {
    if (OutError != nullptr) {
        *OutError = std::move(InMessage);
    }
}

auto ReadRequiredString(
    const nlohmann::json& InObject,
    const char* InKey,
    std::string* OutValue,
    std::string* OutError) -> bool {
    const auto it = InObject.find(InKey);
    if (it == InObject.end() || !it->is_string()) {
        SetError(OutError, std::string("startup inventory field '") +
            InKey + "' must be a string");
        return false;
    }
    *OutValue = it->get<std::string>();
    if (OutValue->size() > kTuiStartupMaxFieldBytes) {
        SetError(OutError, std::string("startup inventory field '") +
            InKey + "' exceeds the field-size limit");
        return false;
    }
    if (OutValue->find('\0') != std::string::npos) {
        SetError(OutError, std::string("startup inventory field '") +
            InKey + "' contains NUL");
        return false;
    }
    return true;
}

auto ReadRequiredBoolean(
    const nlohmann::json& InObject,
    const char* InKey,
    bool* OutValue,
    std::string* OutError) -> bool {
    const auto it = InObject.find(InKey);
    if (it == InObject.end() || !it->is_boolean()) {
        SetError(OutError, std::string("startup inventory field '") +
            InKey + "' must be a boolean");
        return false;
    }
    *OutValue = it->get<bool>();
    return true;
}

auto NormalizeRoot(const std::filesystem::path& InWorkspaceRoot)
    -> std::filesystem::path {
    const auto absolute = InWorkspaceRoot.is_absolute()
        ? InWorkspaceRoot
        : std::filesystem::absolute(InWorkspaceRoot);
    return absolute.lexically_normal();
}

auto IsLexicalDescendant(
    const std::filesystem::path& InParent,
    const std::filesystem::path& InChild) -> bool {
    const auto relative = InChild.lexically_relative(InParent);
    if (relative.empty() || relative == ".") {
        return false;
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

auto IsWithinWorkspace(
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InPath) -> bool {
    return InPath == InWorkspaceRoot ||
        IsLexicalDescendant(InWorkspaceRoot, InPath);
}

auto InferLexicalParents(
    std::vector<TuiStartupRepoSnapshot>& InOutRows) -> void {
    for (std::size_t childIndex = 0;
         childIndex < InOutRows.size();
         ++childIndex) {
        std::size_t bestParentLength = 0;
        for (std::size_t candidateIndex = 0;
             candidateIndex < InOutRows.size();
             ++candidateIndex) {
            if (candidateIndex == childIndex ||
                !IsLexicalDescendant(
                    InOutRows[candidateIndex].path,
                    InOutRows[childIndex].path)) {
                continue;
            }
            const auto candidateLength = InOutRows[candidateIndex]
                .path.generic_string().size();
            if (candidateLength > bestParentLength) {
                bestParentLength = candidateLength;
                InOutRows[childIndex].parentPath =
                    InOutRows[candidateIndex].path;
            }
        }
    }
}

}  // namespace

auto TuiAuditBooleanLabel(
    const bool bInKnown,
    const bool bInValue) -> std::string_view {
    if (!bInKnown) {
        return "unknown";
    }
    return bInValue ? "yes" : "no";
}

auto ParseTuiObservedAtUtc(const std::string_view InValue)
    -> std::optional<std::chrono::system_clock::time_point> {
    if (InValue.size() != 20 || InValue[4] != '-' || InValue[7] != '-' ||
        InValue[10] != 'T' || InValue[13] != ':' || InValue[16] != ':' ||
        InValue[19] != 'Z') return std::nullopt;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!ParseFixedDigits(InValue, 0, 4, &year) || !ParseFixedDigits(InValue, 5, 2, &month) ||
        !ParseFixedDigits(InValue, 8, 2, &day) || !ParseFixedDigits(InValue, 11, 2, &hour) ||
        !ParseFixedDigits(InValue, 14, 2, &minute) || !ParseFixedDigits(InValue, 17, 2, &second) ||
        // system_clock nanosecond implementations commonly end in 2262;
        // reject values outside the portable, representable contract.
        year < 1970 || year > 2261 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour > 23 || minute > 59 || second > 59) return std::nullopt;
    const auto seconds = DaysSinceUnixEpoch(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL +
        hour * 3600LL + minute * 60LL + second;
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

auto ClassifyTuiStartupSnapshotFreshness(const TuiStartupSnapshotMetadata& InMetadata,
                                         const std::chrono::system_clock::time_point InNow,
                                         const std::chrono::seconds InMaximumAge) -> TuiStartupSnapshotFreshness {
    if (!InMetadata.observedAtUtc.has_value() || InMaximumAge.count() < 0) return TuiStartupSnapshotFreshness::Unknown;
    if (*InMetadata.observedAtUtc > InNow) return TuiStartupSnapshotFreshness::Future;
    return InNow - *InMetadata.observedAtUtc <= InMaximumAge
        ? TuiStartupSnapshotFreshness::Fresh : TuiStartupSnapshotFreshness::Stale;
}

auto TuiStartupSnapshotFreshnessLabel(const TuiStartupSnapshotFreshness InFreshness) -> std::string_view {
    switch (InFreshness) {
    case TuiStartupSnapshotFreshness::Unknown: return "unknown";
    case TuiStartupSnapshotFreshness::Fresh: return "fresh";
    case TuiStartupSnapshotFreshness::Stale: return "stale";
    case TuiStartupSnapshotFreshness::Future: return "clock-skew";
    }
    return "unknown";
}

auto MakeTuiLiveInventoryProvenance(
    const bool bInDirtyFiltered,
    const bool bInStatusKnown,
    const std::chrono::system_clock::time_point InObservedAtUtc)
    -> TuiStartupInventoryProvenance {
    const auto raw = std::chrono::system_clock::to_time_t(InObservedAtUtc);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    TuiStartupInventoryProvenance out;
    out.live = true;
    out.freshness = TuiStartupSnapshotFreshness::Fresh;
    out.metadata.source = "live";
    out.metadata.completeness = bInDirtyFiltered
        ? "dirty-filtered-workspace" : "workspace-inventory";
    out.metadata.probeMode = "git-status";
    out.metadata.statusKnown = bInStatusKnown;
    out.metadata.observedAtUtc = InObservedAtUtc;
    out.metadata.observedAtUtcText = text.str();
    return out;
}

auto ObserveTuiLiveInventoryStatus(
    TuiLiveInventoryStatusSummary& InOutSummary,
    const bool bInCandidateStatusKnown) -> void {
    ++InOutSummary.candidateCount;
    InOutSummary.statusKnown = InOutSummary.statusKnown && bInCandidateStatusKnown;
}

auto MakeTuiDirtyFilterProbeResult(
    const int InExitCode,
    const std::string_view InOutput) -> TuiDirtyFilterProbeResult {
    return {
        .dirty = InExitCode == 0 && !InOutput.empty(),
        .statusKnown = InExitCode == 0,
    };
}

auto ResolveTuiLiveCandidateStatusKnown(
    const TuiDirtyFilterProbeResult& InFilterProbe,
    const bool bInLaterProbeStatusKnown) -> bool {
    return InFilterProbe.statusKnown && bInLaterProbeStatusKnown;
}

auto MakeTuiUnknownInventoryProvenance() -> TuiStartupInventoryProvenance {
    TuiStartupInventoryProvenance out;
    out.metadata.source = "unknown";
    out.metadata.completeness = "unknown";
    out.metadata.probeMode = "unknown";
    out.metadata.statusKnown = false;
    return out;
}

auto RetainTuiInventoryProvenanceAfterFailure(
    TuiStartupInventoryProvenance InPrevious) -> TuiStartupInventoryProvenance {
    InPrevious.live = false;
    if (InPrevious.metadata.source == "live") {
        InPrevious.metadata.source = "retained-prior-live";
        InPrevious.metadata.probeMode = "none";
    }
    InPrevious.freshness = InPrevious.metadata.observedAtUtc.has_value()
        ? TuiStartupSnapshotFreshness::Stale : TuiStartupSnapshotFreshness::Unknown;
    return InPrevious;
}

auto FormatTuiInventoryProvenanceFull(
    const TuiStartupInventoryProvenance& InProvenance) -> std::string {
    return "source=" + InProvenance.metadata.source +
        " | completeness=" + InProvenance.metadata.completeness +
        " | probe=" + InProvenance.metadata.probeMode +
        " | status=" + std::string(InProvenance.metadata.statusKnown ? "known" : "unknown") +
        " | observed=" + InProvenance.metadata.observedAtUtcText.value_or("unknown") +
        " | freshness=" + std::string(TuiStartupSnapshotFreshnessLabel(InProvenance.freshness));
}

auto FormatTuiInventoryProvenanceCompact(
    const TuiStartupInventoryProvenance& InProvenance) -> std::string {
    return InProvenance.metadata.source + " | " +
        std::string(TuiStartupSnapshotFreshnessLabel(InProvenance.freshness));
}

auto ParseTuiStartupSnapshotJson(
    const std::string_view InJson,
    const std::filesystem::path& InWorkspaceRoot,
    std::string* OutError,
    TuiStartupSnapshotMetadata* OutMetadata)
    -> std::vector<TuiStartupRepoSnapshot> {
    if (OutError != nullptr) {
        OutError->clear();
    }
    if (OutMetadata != nullptr) {
        *OutMetadata = {};
    }
    const auto root = NormalizeRoot(InWorkspaceRoot);

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(InJson.begin(), InJson.end());
    } catch (const nlohmann::json::exception& exception) {
        SetError(OutError, std::string("invalid startup inventory JSON: ") +
            exception.what());
        return {};
    }
    if (!payload.is_object()) {
        SetError(OutError, "startup inventory payload must be an object");
        return {};
    }
    const auto schemaName = payload.find("schemaName");
    const auto schemaVersion = payload.find("schemaVersion");
    if (schemaName == payload.end() || !schemaName->is_string() ||
        schemaName->get_ref<const std::string&>() != "kog.repoOverview" ||
        schemaVersion == payload.end() ||
        !schemaVersion->is_number_integer() ||
        schemaVersion->get<std::int64_t>() != 1) {
        SetError(OutError,
            "startup inventory schema must be kog.repoOverview v1");
        return {};
    }
    TuiStartupSnapshotMetadata metadata;
    if (!ReadRequiredString(
            payload,
            "source",
            &metadata.source,
            OutError) ||
        !ReadRequiredString(
            payload,
            "completeness",
            &metadata.completeness,
            OutError) ||
        !ReadRequiredString(
            payload,
            "probeMode",
            &metadata.probeMode,
            OutError) ||
        !ReadRequiredBoolean(
            payload,
            "statusKnown",
            &metadata.statusKnown,
            OutError)) {
        return {};
    }
    const auto externalRootsIt = payload.find("allowedExternalRoots");
    if (externalRootsIt == payload.end() ||
        !externalRootsIt->is_array()) {
        SetError(
            OutError,
            "startup inventory allowedExternalRoots must be an array");
        return {};
    }
    if (externalRootsIt->size() > kTuiStartupMaxExternalRoots) {
        SetError(
            OutError,
            "startup inventory exceeds the external-root limit");
        return {};
    }
    std::unordered_set<std::string> seenExternalRoots;
    for (const auto& value : *externalRootsIt) {
        if (!value.is_string()) {
            SetError(
                OutError,
                "startup inventory external roots must be strings");
            return {};
        }
        const auto text = value.get<std::string>();
        if (text.empty() || text.size() > kTuiStartupMaxFieldBytes ||
            text.find('\0') != std::string::npos) {
            SetError(
                OutError,
                "startup inventory external root is empty, oversized, or contains NUL");
            return {};
        }
        const auto path = std::filesystem::path(text);
        if (!path.is_absolute()) {
            SetError(
                OutError,
                "startup inventory external roots must be absolute");
            return {};
        }
        const auto normalized = path.lexically_normal();
        if (!seenExternalRoots.insert(
                normalized.generic_string()).second) {
            SetError(
                OutError,
                "startup inventory contains a duplicate external root");
            return {};
        }
        metadata.allowedExternalRoots.push_back(normalized);
    }
    const bool trustedInventory =
        metadata.source == "trusted-workspace-manifest" &&
        metadata.completeness == "workspace-inventory" &&
        metadata.statusKnown;
    const bool rootFallback =
        metadata.source == "root-fallback" &&
        metadata.completeness == "root-only" &&
        !metadata.statusKnown &&
        metadata.allowedExternalRoots.empty();
    if ((!trustedInventory && !rootFallback) ||
        metadata.probeMode != "none") {
        SetError(
            OutError,
            "startup inventory requires a no-probe trusted manifest or root fallback");
        return {};
    }
    const auto observedAtIt = payload.find("observedAtUtc");
    if (observedAtIt != payload.end()) {
        if (!observedAtIt->is_string() || observedAtIt->get_ref<const std::string&>().size() > kTuiStartupMaxFieldBytes) {
            SetError(OutError, "startup inventory observedAtUtc must be a bounded UTC string");
            return {};
        }
        metadata.observedAtUtc = ParseTuiObservedAtUtc(observedAtIt->get_ref<const std::string&>());
        if (!metadata.observedAtUtc.has_value()) {
            SetError(OutError, "startup inventory observedAtUtc must be strict UTC");
            return {};
        }
        metadata.observedAtUtcText = observedAtIt->get_ref<const std::string&>();
    }
    const auto reposIt = payload.find("repos");
    if (reposIt == payload.end() || !reposIt->is_array()) {
        SetError(OutError, "startup inventory repos must be an array");
        return {};
    }
    if (reposIt->size() > kTuiStartupMaxRepos) {
        SetError(OutError, std::format(
            "startup inventory exceeds the {} repository limit",
            kTuiStartupMaxRepos));
        return {};
    }

    std::unordered_set<std::string> seenPaths;
    std::vector<TuiStartupRepoSnapshot> rows;
    rows.reserve(reposIt->size());
    for (const auto& repo : *reposIt) {
        if (!repo.is_object()) {
            SetError(OutError,
                "startup inventory repo entry must be an object");
            return {};
        }
        std::string pathText;
        std::string type;
        std::string branch;
        std::string tracking;
        bool dirty = false;
        bool worktreeDirty = false;
        if (!ReadRequiredString(repo, "path", &pathText, OutError) ||
            !ReadRequiredString(repo, "type", &type, OutError) ||
            !ReadRequiredString(repo, "branch", &branch, OutError) ||
            !ReadRequiredString(repo, "tracking", &tracking, OutError) ||
            !ReadRequiredBoolean(repo, "dirty", &dirty, OutError) ||
            !ReadRequiredBoolean(
                repo,
                "worktree_dirty",
                &worktreeDirty,
                OutError)) {
            return {};
        }
        if (pathText.empty() || type.empty()) {
            SetError(OutError,
                "startup inventory path and type must not be empty");
            return {};
        }

        const auto path = std::filesystem::path(pathText);
        if (!path.is_absolute()) {
            SetError(OutError,
                "startup inventory paths must be absolute");
            return {};
        }
        const auto normalizedPath = path.lexically_normal();
        const bool withinAllowedExternalRoot = std::any_of(
            metadata.allowedExternalRoots.begin(),
            metadata.allowedExternalRoots.end(),
            [&](const std::filesystem::path& InAllowedRoot) {
                return IsWithinWorkspace(
                    InAllowedRoot,
                    normalizedPath);
            });
        if (!IsWithinWorkspace(root, normalizedPath) &&
            !withinAllowedExternalRoot) {
            SetError(OutError,
                "startup inventory path escapes the workspace and configured external roots");
            return {};
        }
        if (!seenPaths.insert(normalizedPath.generic_string()).second) {
            SetError(OutError,
                "startup inventory contains a duplicate repository path");
            return {};
        }

        TuiStartupRepoSnapshot row;
        row.path = normalizedPath;
        const auto relative = normalizedPath.lexically_relative(root);
        row.relativePath = relative.empty() || relative == "."
            ? "."
            : relative.generic_string();
        row.type = std::move(type);
        row.branch = branch.empty() ? "(detached)" : std::move(branch);
        row.tracking = std::move(tracking);
        row.statusKnown = metadata.statusKnown;
        row.repoDirty = dirty;
        row.worktreeDirty = worktreeDirty;
        rows.push_back(std::move(row));
    }

    if (rootFallback &&
        (rows.size() != 1 ||
         rows.front().path != root ||
         rows.front().type != "root")) {
        SetError(
            OutError,
            "root fallback must contain exactly the requested root repository");
        return {};
    }

    InferLexicalParents(rows);
    if (OutMetadata != nullptr) {
        *OutMetadata = std::move(metadata);
    }
    return rows;
}

auto LoadTuiStartupSnapshot(
    const TuiStartupSnapshotLoadOptions& InOptions,
    TuiStartupSnapshotMetadata* OutMetadata)
    -> std::vector<TuiStartupRepoSnapshot> {
    if (InOptions.binaryCommand.empty() ||
        InOptions.workspaceRoot.empty() ||
        InOptions.timeoutMs == 0 ||
        InOptions.maxCaptureBytes == 0) {
        throw std::invalid_argument(
            "startup inventory requires a binary, workspace, timeout, and capture limit");
    }
    const auto root = NormalizeRoot(InOptions.workspaceRoot);
    const auto capture = shell::ExecuteCommand(
        InOptions.binaryCommand,
        {
            "overview",
            "--format",
            "json",
            "--inventory-only",
            "--root-fallback",
            "--repo-root",
            root.generic_string(),
        },
        shell::ExecMode::Capture,
        root,
        shell::ProgressCallback{},
        InOptions.timeoutMs,
        shell::CaptureLimits{
            InOptions.maxCaptureBytes,
            InOptions.maxCaptureBytes});
    if (capture.stdoutTruncated || capture.stderrTruncated) {
        throw std::runtime_error(
            "startup inventory exceeded the bounded capture budget");
    }
    if (capture.exitCode != 0) {
        const auto diagnostic = BoundTuiLoadDiagnostic(
            capture.stderrStr.empty()
                ? std::string("overview process exit ") +
                    std::to_string(capture.exitCode)
                : capture.stderrStr);
        throw std::runtime_error(
            "bounded startup inventory failed: " + diagnostic);
    }

    std::string parseError;
    auto rows = ParseTuiStartupSnapshotJson(
        capture.stdoutStr,
        root,
        &parseError,
        OutMetadata);
    if (!parseError.empty()) {
        throw std::runtime_error(parseError);
    }
    return rows;
}

}  // namespace kano::git::commands
