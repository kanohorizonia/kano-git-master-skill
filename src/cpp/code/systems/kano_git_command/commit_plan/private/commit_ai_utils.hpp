#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <format>
#include <iomanip>
#include <sstream>

#include "shell_executor.hpp"
#include "discovery.hpp"

namespace kano::git::commands {

struct CommitPreflightReport {
    bool inRepo = false;
    std::filesystem::path repoPath;
    int stagedCount = 0;
    int unstagedCount = 0;
    int untrackedCount = 0;
    std::vector<std::string> riskyFiles;
    std::vector<std::string> stagedFiles;
    std::vector<std::string> unstagedFiles;
    std::vector<std::string> untrackedFiles;
};

struct NativeAiConfig {
    bool enabled = false;
    bool reviewEnabled = true;
    bool yolo = false;
    std::string provider;
    std::string model;
};

auto BuildAiCommitPrompt(const std::filesystem::path& InWorkspaceRoot,
                         const std::filesystem::path& InRepo,
                         const CommitPreflightReport& InReport) -> std::string;

auto ExtractSingleLineMessage(const std::string& InText) -> std::string;

auto RunAiGenerate(const std::string& InProvider,
                   const std::string& InModel,
                   const std::string& InPrompt,
                   std::optional<std::filesystem::path> InWorkingDir = std::nullopt,
                   const std::string& InPurpose = "commit-ai") -> shell::ExecResult;

auto SummarizeAiFailure(const shell::ExecResult& InResult) -> std::string;

auto GenerateAiCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                             const std::filesystem::path& InRepo,
                             const CommitPreflightReport& InReport,
                             const NativeAiConfig& InAi,
                             std::string* OutFailureReason = nullptr) -> std::string;

auto ShouldBlockByAiReview(const std::filesystem::path& InRepo,
                           const std::string& InMessage,
                           const NativeAiConfig& InAi,
                           std::string& OutReason) -> bool;

// Synthetic plan helpers
auto CurrentUtcTimestampCompactForSyntheticPlan() -> std::string;
auto CurrentUtcTimestampIso8601ForSyntheticPlan() -> std::string;
auto DefaultMessagePlanOutputPath(const std::filesystem::path& InWorkspaceRoot,
                                  const std::string& InMessage) -> std::filesystem::path;
auto WriteSyntheticMessageCommitPlan(const std::filesystem::path& InWorkspaceRoot,
                                     const std::vector<workspace::RepoRecord>& InRepoRecords,
                                     const std::string& InMessage,
                                     const std::filesystem::path& InOutPath,
                                     std::string* OutError) -> bool;

// Config/Cache helpers
auto ResolveProvider(const std::string& InProviderRaw) -> std::string;
auto ResolveModelForAi(const std::string& InProvider,
                       const std::string& InModelRaw,
                       bool InAiAuto,
                       const std::filesystem::path& InWorkspaceRoot) -> std::string;
auto GitConfigPath(const std::string& InKey) -> std::string;
auto ResolveGlobalCacheRoot() -> std::filesystem::path;
auto AiCacheDir() -> std::filesystem::path;
auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::filesystem::path>;
auto JoinReposCsv(const std::vector<std::filesystem::path>& InRepos) -> std::string;

// Path/Discovery utilities (shared between commit and plan workflows)
auto DiscoverRegisteredPathsRecursive(const std::filesystem::path& InWorkspaceRoot) -> std::vector<std::filesystem::path>;
auto PathDepth(const std::filesystem::path& InPath) -> std::size_t;
auto DiscoverWorkspaceRepoRecords(const std::filesystem::path& InRoot,
                                  const std::string& InMetadataLevel,
                                  const bool InUseCache = true,
                                  const bool InRefreshCache = false) -> std::vector<workspace::RepoRecord>;
auto BuildCommitScopeRecords(const std::filesystem::path& InWorkspaceRoot,
                             const std::string& InReposCsv,
                             const bool InNoRecursive,
                             const bool InDirtyOnly) -> std::vector<workspace::RepoRecord>;

// Wave/topology helpers
auto BuildExecutionWaves(const std::vector<workspace::RepoRecord>& InRepos) -> std::vector<std::vector<std::size_t>>;

// Single repo result types
struct RepoCommitResult {
    std::filesystem::path repo;
    bool committed = false;
    bool pushed = false;
    bool failed = false;
    std::string note;
    std::string commitTitle;
};

struct RepoAmendResult {
    std::filesystem::path repo;
    bool amended = false;
    bool combined = false;
    bool failed = false;
    std::string note;
    std::string commitTitle;
};

} // namespace kano::git::commands
