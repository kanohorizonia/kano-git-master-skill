#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <regex>
#include <utility>
#include <unordered_set>
#include "shell_executor.hpp"

namespace kano::git::commands {

struct IgnoreFinding {
    std::filesystem::path repo;
    std::string repoRel;
    std::string repoPath;
    std::string display;
};

struct IgnoreStageEntry {
    std::string repo = ".";
    std::string applyTarget = ".gitignore";
    std::string mergedOutputPath;
    std::vector<std::string> rules;
};

struct IgnoreDatasourceSource {
    std::string id;
    std::string kind;
    std::string pathRaw;
    bool enabled = true;
    std::filesystem::path resolvedPath;
};

struct SecretRule {
    std::string id;
    std::regex pattern;
};

struct SecretFinding {
    std::string repo;
    std::string file;
    std::string ruleId;
    int line = 0;
    std::string preview;
};

struct CommitPlanEntry {
    int index = -1;
    std::string repo;
    std::string message;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::string reviewVerdict;
    std::string reviewReason;
};

struct CommitFillOp {
    int index = -1;
    std::string message;
    std::string reviewVerdict;
    std::string reviewReason;
    std::string plannerProvider;
    std::string plannerModel;
};

struct CommitFillOpsBatch {
    std::vector<CommitFillOp> ops;
    std::optional<std::string> commitStageJson;
};

// Common utilities
auto Trim(std::string InValue) -> std::string;
auto ToLower(std::string InValue) -> std::string;
auto SplitNonEmptyLines(const std::string& InText) -> std::vector<std::string>;
auto ReplaceAll(std::string InText, const std::string& InSearch, const std::string& InReplace) -> std::string;
auto NormalizeAiModelKeyword(const std::string& InValue) -> std::string;
auto IsTruthyEnv(const char* InValue) -> bool;
auto IsKogDebugEnabled() -> bool;
auto IsAgentModeEnabled() -> bool;
auto SplitEnvList(const char* InValue) -> std::vector<std::string>;
auto StartsWith(const std::string& InValue, const std::string& InPrefix) -> bool;
auto CurrentUtcIso8601() -> std::string;
auto CurrentUtcCompact() -> std::string;
auto HomeDirectory() -> std::filesystem::path;
auto Fnv1a64Hex(const std::string& InText) -> std::string;
auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool;
auto BuildIgnoreRuleForArtifactPath(const std::string& InPath) -> std::string;
auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool;
auto JsonEscape(std::string InValue) -> std::string;

// Path utilities
auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::string;
auto NormalizePath(const std::filesystem::path& InPath) -> std::filesystem::path;
auto RepoKey(const std::filesystem::path& InPath) -> std::string;
auto ToGeneric(const std::filesystem::path& InPath) -> std::string;
auto NormalizeInputPathForCurrentPlatform(std::string InPath) -> std::string;
auto ResolvePath(const std::filesystem::path& InBase, const std::string& InPath) -> std::filesystem::path;
auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;

// File/Git utilities
auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string>;
auto WriteFileText(const std::filesystem::path& InPath, const std::string& InText, std::string* OutError) -> bool;
auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult;
auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult;

// JSON lightweight utilities
auto ExtractJsonBetweenMarkers(const std::string& InText) -> std::string;
auto ExtractJsonBetweenMarkers(const std::string& InText,
                               const std::string& InBeginMarker,
                               const std::string& InEndMarker) -> std::string;

// AI Model resolution utilities
auto ResolveSystemRecommendedModel(const std::string& InProvider) -> std::string;
auto ResolveAiModelDirective(const std::string& InProvider,
                             const std::string& InRequested,
                             const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto ResolveAiModelForChangeCount(const std::string& InProvider,
                                  const std::string& InModelDirective,
                                  const std::filesystem::path& InWorkspaceRoot,
                                  const int InChangeCount) -> std::string;

// Prompt/Asset utilities
auto LoadPromptAssetText(const std::filesystem::path& InWorkspaceRoot,
                        const char* InEnvOverride,
                        const std::filesystem::path& InPathRelativeToSkill) -> std::optional<std::string>;
auto AppendCommitConventionSkillSection(const std::filesystem::path& InWorkspaceRoot, std::string InPrompt) -> std::string;

// Workspace/Repo utilities
auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path>;
auto CountWorkspaceDirtyEntries(const std::filesystem::path& InRoot) -> int;
auto WorkspaceRepoKey(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepo) -> std::string;
auto ComputeWorkspaceBaseHeadSha(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto ComputeWorkspaceDirtyFingerprint(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto BuildFallbackCommitScope(const std::string& InRepoDisplay) -> std::string;
auto ResolveUpstreamRef(const std::filesystem::path& InRepo) -> std::string;
auto CountUnpushedCommits(const std::filesystem::path& InRepo, const std::string& InUpstreamRef) -> int;

// Plan utilities
auto PlanNeedsRefresh(const std::string& InPlanText) -> bool;
auto PlanWorkspaceStateDrifted(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText) -> bool;
auto BuildDefaultPlanTemplate(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto BuildDefaultPlanTemplate(const std::filesystem::path& InWorkspaceRoot,
                              const std::optional<std::filesystem::path>& InDatasourceRoot,
                              const std::optional<std::filesystem::path>& InDatasourceManifest) -> std::string;
auto BuildSetAiModelHelpFooter() -> std::string;
auto CountTopLevelObjects(const std::string& InArrayBody) -> std::size_t;
auto ParseIgnoreEntries(const std::string& InText) -> std::vector<IgnoreStageEntry>;
auto ExtractPlanWorkspaceHashes(const std::string& InPlanText, std::string* OutBaseHeadSha, std::string* OutDirtyFingerprint) -> bool;
auto BuildCommitSeedEntriesJson(const std::filesystem::path& InWorkspaceRoot, const bool InUsePlaceholders) -> std::string;
auto HasValidCommitItems(const std::string& InPlanText) -> bool;
auto SeedCommitStage(const std::filesystem::path& InWorkspaceRoot,
                     const std::string& InPlanText,
                     const bool InForce,
                     const bool InUsePlaceholders) -> std::optional<std::string>;
auto BuildFallbackCommitEntriesJson(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto BuildDeterministicCommitFillOps(const std::vector<CommitPlanEntry>& InEntries) -> std::vector<CommitFillOp>;
auto BuildDeterministicCommitFillOp(const CommitPlanEntry& InEntry) -> CommitFillOp;
auto ResolvePlanCommitGenerationMode(const std::filesystem::path& InWorkspaceRoot,
                                     const std::string& InRequestedMode) -> std::string;
auto AllowDeterministicCommitFallbackForMode(const std::string& InFillMode) -> bool;
auto BuildSingleCommitFillPrompt(const std::filesystem::path& InWorkspaceRoot,
                                 const std::string& InProvider,
                                 const std::string& InModel,
                                 const std::filesystem::path& InPlanPath,
                                 const std::filesystem::path& InWorkingPlanPath,
                                 const CommitPlanEntry& InEntry,
                                 const std::string& InPlanText,
                                 const std::string& InDirtyContext,
                                 const std::filesystem::path& InWorkingGitignorePath) -> std::string;
auto TryInjectFallbackCommits(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText) -> std::optional<std::string>;
auto BuildJsonStringArray(const std::vector<std::string>& InValues) -> std::string;
auto BuildCommitObjectJson(const std::string& InMessage,
                           const std::string& InIncludeArrayBody,
                           const std::string& InExcludeArrayBody,
                           const std::string& InReviewVerdict,
                           const std::string& InReviewReason,
                           const std::string& InPlannerProvider,
                           const std::string& InPlannerModel) -> std::string;
auto CollectCommitPlanEntries(const std::string& InPlanText) -> std::vector<CommitPlanEntry>;
auto CommitEntryNeedsReview(const CommitPlanEntry& InEntry) -> bool;
auto CollectCommitIndexesNeedingReview(const std::vector<CommitPlanEntry>& InEntries) -> std::vector<int>;
auto FindCommitEntryByFlatIndex(const std::string& InPlanText,
                                 int InCommitIndex,
                                 std::string* OutError = nullptr) -> std::optional<CommitPlanEntry>;
auto ParseCommitFillOps(const std::string& InJson, std::string* OutError = nullptr) -> std::vector<CommitFillOp>;
auto ParseCommitFillOpsBatch(const std::string& InJson, std::string* OutError = nullptr) -> CommitFillOpsBatch;
auto ApplyCommitFillOps(std::string InPlanText, const std::vector<CommitFillOp>& InOps) -> std::string;
auto ApplyCommitStageReplacement(std::string InPlanText,
                                 const std::string& InCommitStageJson,
                                 std::string* OutError = nullptr) -> std::optional<std::string>;
auto StampPlanAiPlannerMetadata(std::string InPlanText,
                                const std::string& InProvider,
                                const std::string& InModel) -> std::optional<std::string>;
auto NormalizeAiReadyPlanReviewVerdicts(std::string InPlanText) -> std::optional<std::string>;
auto UpsertCommitEntry(const std::string& InPlanText,
                       const std::string& InRepo,
                       const std::string& InMessage,
                       const std::vector<std::string>& InInclude,
                       const std::vector<std::string>& InExclude,
                       const std::string& InReviewVerdict,
                       const std::string& InReviewReason) -> std::optional<std::string>;
auto FillCommitEntryByFlatIndex(const std::string& InPlanText,
                                int InCommitIndex,
                                const std::optional<std::string>& InCommitMessage,
                                const std::optional<std::string>& InReviewVerdict,
                                const std::optional<std::string>& InReviewReason,
                                const std::optional<std::string>& InPlannerProvider,
                                const std::optional<std::string>& InPlannerModel,
                                std::string* OutError = nullptr) -> std::optional<std::string>;
auto ResolveAiProvider(const std::string& InRequested) -> std::string;
auto RunAiGenerate(const std::string& InProvider,
                    const std::string& InModel,
                    const std::string& InPrompt,
                    const std::filesystem::path& InWorkspaceRoot,
                    bool InQuiet = false,
                    bool InYolo = false) -> shell::ExecResult;
auto CollectDirtyRepoContextText(const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto BuildPlanPrompt(const std::filesystem::path& InWorkspaceRoot,
                      const std::string& InProvider,
                      const std::string& InModel,
                      const std::string& InDirtyContext) -> std::string;
auto BuildPlanFillOpsPrompt(const std::filesystem::path& InWorkspaceRoot,
                              const std::string& InProvider,
                              const std::string& InModel,
                              const std::filesystem::path& InPlanPath,
                              const std::filesystem::path& InWorkingPlanPath,
                              const std::string& InPlanText,
                              const std::string& InDirtyContext,
                              const std::filesystem::path& InWorkingGitignorePath) -> std::string;
auto ExtractPlanFillOpsJson(const std::string& InAiCombined) -> std::string;
auto BuildFillOpsRetryPrompt(const std::string& InBasePrompt,
                              std::size_t InExpectedCount,
                              const std::optional<std::size_t>& InActualCount,
                              const std::string& InFailureCategory,
                              const std::string& InFailureDetail,
                              const std::string& InAiCombined) -> std::string;
auto FillPlanByAi(const std::filesystem::path& InWorkspaceRoot,
                  const std::filesystem::path& InPlanPath,
                  const std::string& InRequestedProvider,
                  const std::string& InRequestedModel,
                  const std::string& InRequestedFillMode,
                  bool InDebugAi,
                  std::string* OutError = nullptr,
                  bool InAllowEmptyDirty = false,
                  bool InYolo = false) -> bool;

auto DefaultPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto ReplacePlanDirtyFingerprint(std::string InPlanText, const std::string& InNewDirtyFingerprint) -> std::optional<std::string>;
auto RefreshPlanWorkspaceHashes(const std::filesystem::path& InPlanPath,
                                const std::filesystem::path& InWorkspaceRoot) -> bool;
auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto ResolveIgnoreDatasourceRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto ResolveRepoPathFromDisplay(const std::filesystem::path& InWorkspaceRoot, const std::string& InRepoDisplay) -> std::filesystem::path;
auto RepoHasGitlinkOnlyChanges(const std::filesystem::path& InRepo) -> bool;
auto CollectChangedSubmoduleNames(const std::filesystem::path& InRepo) -> std::vector<std::string>;
auto BuildSubmoduleUpdateMessage(const std::vector<std::string>& InNames) -> std::string;
auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool;
auto BuildIgnoreRuleForArtifactPath(const std::string& InPath) -> std::string;
auto IsInternalPipelineArtifactPath(const std::string& InPath) -> bool;
auto DefaultSecretRulesPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto LoadSecretRules(const std::filesystem::path& InRulesPath, std::string* OutError) -> std::vector<SecretRule>;
auto ParseStatusChangedPath(const std::string& InLine) -> std::optional<std::string>;
auto CollectChangedCandidateFiles(const std::filesystem::path& InRepo) -> std::vector<std::string>;
auto ScanFileForSecretRules(const std::filesystem::path& InRepo,
                            const std::string& InFile,
                            const std::vector<SecretRule>& InRules,
                            int InMaxFindings,
                            std::vector<SecretFinding>* OutFindings) -> void;
auto BuildSetAiModelHelpFooter() -> std::string;
auto ValidateAiReadyPlan(const std::string& InPlanText, std::string* OutReason = nullptr) -> bool;
auto NormalizeCommitPlanRepoPaths(const std::filesystem::path& InWorkspaceRoot,
                                  const std::string& InPlanText,
                                  std::string* OutError = nullptr) -> std::optional<std::string>;
auto CompactSingleLine(const std::string& InText, int InMax) -> std::string;

auto FindBracketRange(const std::string& InText, std::size_t InStart, char InOpen, char InClose) -> std::optional<std::pair<std::size_t, std::size_t>>;
auto InjectIgnoreEntries(std::string InPlanText, const std::vector<IgnoreStageEntry>& InEntries) -> std::optional<std::string>;
auto ApplyIgnoreDatasourceOverrides(std::string InPlanText,
                                    const std::optional<std::filesystem::path>& InDatasourceRoot,
                                    const std::optional<std::filesystem::path>& InDatasourceManifest) -> std::optional<std::string>;
auto ReplacePlanDirtyFingerprint(std::string InPlanText, const std::string& InNewDirtyFingerprint) -> std::optional<std::string>;
auto BuildIgnoreEntriesFromWorkingTree(const std::filesystem::path& InWorkspaceRoot, int InMaxPerRepo) -> std::vector<IgnoreStageEntry>;
auto ReadIgnoreGateAllowlist(const std::filesystem::path& InAllowlistPath) -> std::unordered_set<std::string>;
auto MergeGitignore(const std::filesystem::path& InTarget, const std::vector<std::string>& InRules) -> std::string;
auto StampIgnoreAppliedAtAll(std::string InText, const std::string& InTimestamp) -> std::string;
auto GitHeadSha(const std::filesystem::path& InRepo) -> std::optional<std::string>;
auto GitSubmoduleGitlinkShaAtHead(const std::filesystem::path& InRepo, const std::string& InSubmodulePath) -> std::optional<std::string>;
auto IsGitlinkPathInHead(const std::filesystem::path& InRepoRoot, const std::string& InPath) -> bool;
auto IsRegisteredSubmodulePath(const std::filesystem::path& InRepoRoot, const std::string& InPath) -> bool;

auto RunCommitRunbook(const std::filesystem::path& InWorkspaceRoot,
                      const std::filesystem::path& InPlanPath,
                      const std::string& InProvider,
                      const std::string& InModel,
                      const std::string& InFillMode,
                      bool InDebugAi,
                      int InMaxCommits,
                      bool InAllowEmptyDirty = false,
                      bool InYolo = false) -> int;

auto RunPreApplyVerify(const std::filesystem::path& InWorkspaceRoot,
                        const std::filesystem::path& InPlanPath,
                        const std::string& InStage) -> int;

auto RunPostApplyVerify(const std::filesystem::path& InPlanPath,
                        const std::string& InStage) -> int;

auto CountDirtyScope(const std::filesystem::path& InWorkspaceRoot) -> std::pair<std::size_t, std::size_t>;

auto RunIgnoreRunbook(const std::filesystem::path& InWorkspaceRoot,
                       const std::filesystem::path& InPlanPath,
                       bool InForce,
                       int InMaxPerRepo,
                       const std::string& InDatasourceRoot,
                       const std::string& InDatasourceManifest) -> int;

auto RunIgnoreInit(const std::filesystem::path& InWorkspaceRoot,
                    const std::filesystem::path& InPlanPath,
                    bool InForce,
                    int InMaxPerRepo,
                    const std::string& InDatasourceRoot,
                    const std::string& InDatasourceManifest) -> int;

auto RunDatasourceSync(const std::filesystem::path& InWorkspaceRoot,
                        const std::string& InSource,
                        bool InDryRun) -> int;

auto RunIgnoreGate(const std::filesystem::path& InWorkspaceRoot,
                    const std::string& InContext,
                    const std::string& InAllowlistPath,
                    int InLimit) -> int;

auto RunSecretGate(const std::filesystem::path& InWorkspaceRoot,
                    const std::string& InContext,
                    const std::string& InRulesFile,
                    int InLimit) -> int;

auto RunIgnoreDoctor(const std::filesystem::path& InRoot,
                     int InLimit,
                     bool InAsJson,
                     bool InApply,
                     bool InDryRun,
                     bool InYes) -> int;

auto RunPlanApply(const std::filesystem::path& InWorkspaceRoot,
                  const std::filesystem::path& InPlanPath,
                  const std::string& InStage,
                  const std::vector<std::string>& InExtraArgs) -> int;

} // namespace kano::git::commands
