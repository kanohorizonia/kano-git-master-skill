#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kano::git::commands {

enum class CommitPlanStage {
    Commit,
    PostSync,
    Both,
};

struct RepoCommitPlanEntry {
    struct CommitReviewMeta {
        std::string verdict;
        std::string reason;
    };

    struct CommitItem {
        std::string message;
        CommitReviewMeta review;
        std::vector<std::string> include;
        std::vector<std::string> exclude;
    };

    std::string repoKey;
    std::vector<CommitItem> commits;
};

struct CommitPlanPayload {
    struct PlannerMeta {
        std::string provider;
        std::string model;
        std::string requestId;
    };

    struct ReviewMeta {
        std::string verdict;
        std::string reason;
    };

    struct Meta {
        std::string schemaVersion;
        std::string planId;
        std::string generatedAtUtc;
        std::string executedAtUtc;
        std::string baseHeadSha;
        std::string dirtyFingerprintPreIgnore;
        std::string dirtyFingerprint;
        std::string freshnessScope;
        std::string scopeRepo;
        PlannerMeta planner;
        ReviewMeta review;
    };

    Meta meta;
    std::vector<RepoCommitPlanEntry> commitEntries;
    std::vector<RepoCommitPlanEntry> postSyncEntries;
};

auto NormalizePlanKey(std::string InValue) -> std::string;
auto ParseCommitPlanStage(const std::string& InValue) -> std::optional<CommitPlanStage>;
auto PlanStageNeedsPreCommit(CommitPlanStage InStage) -> bool;
auto ParseCommitPlan(const std::filesystem::path& InFile,
                     std::string* OutError) -> std::optional<CommitPlanPayload>;
auto LoadNormalizedCommitPlan(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InPlanFile,
                              std::string* OutError) -> std::optional<CommitPlanPayload>;
auto ValidateCommitPlanForAiMode(const CommitPlanPayload& InPlan,
                                 std::string* OutError) -> bool;
auto UsesRepoScopedFreshness(const CommitPlanPayload& InPlan) -> bool;
auto HumanAutoPlanLooksDeterministic(const std::filesystem::path& InPlanPath,
                                     std::string* OutReason) -> bool;

} // namespace kano::git::commands
