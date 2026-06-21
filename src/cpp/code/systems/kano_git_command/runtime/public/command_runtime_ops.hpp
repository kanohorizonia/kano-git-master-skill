#pragma once

#include "repo_operation_scheduler.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace kano::git::commands {

auto RunCommitPushPlanFilePipeline(const std::filesystem::path& InWorkspaceRoot,
                                   const std::string& InNormalizedPlanFile,
                                   const std::vector<std::string>& InExtraArgs) -> int;

auto RunSyncPreCommitNative(const std::filesystem::path& InRepoRoot,
                            bool InRecursive,
                            bool InDryRun,
                            const std::string& InBranchMode) -> int;

auto RunSyncOriginLatestNative(const std::filesystem::path& InRepoRoot,
                               bool InRecursive,
                               bool InDryRun,
                               bool InCleanupStaleLocks = false,
                               bool InCheckGitlinkReachability = true) -> int;

auto RunSyncOriginLatestNativeDetailed(const std::filesystem::path& InRepoRoot,
                                       bool InRecursive,
                                       bool InDryRun,
                                       bool InCleanupStaleLocks = false,
                                       bool InCheckGitlinkReachability = true) -> std::pair<int, workspace::RepoOperationAggregate>;

void FixRepoHygieneRecursive(const std::filesystem::path& InWorkspaceRoot);

auto RunCommitNativePlanStage(const std::filesystem::path& InWorkspaceRoot,
                              const std::string& InPlanFile,
                              const std::string& InPlanStage,
                              bool InProfile) -> int;

auto RunCommitNativeSimple(const std::filesystem::path& InWorkspaceRoot,
                           const std::string& InReposCsv,
                           bool InNoRecursive,
                           const std::string& InMessage,
                           bool InStagedOnly,
                           bool InDryRun,
                           const std::string& InAiProvider,
                           const std::string& InAiModel,
                           bool InAiAuto,
                           bool InNoAiReview,
                           bool InProfile) -> int;

auto RunPushNativeSimple(const std::filesystem::path& InWorkspaceRoot,
                         bool InRecursive,
                         bool InDryRun,
                         bool InProfile,
                         bool InForceWithLease,
                         bool InNoVerify,
                         int InJobs,
                         bool InVerbose,
                         const std::string& InRemote) -> int;

auto RunPushNativeSimpleDetailed(const std::filesystem::path& InWorkspaceRoot,
                                 bool InRecursive,
                                 bool InDryRun,
                                 bool InProfile,
                                 bool InForceWithLease,
                                 bool InNoVerify,
                                 int InJobs,
                                 bool InVerbose,
                                 const std::string& InRemote) -> std::pair<int, workspace::RepoOperationAggregate>;

auto RunPushNativeSimpleDetailed(const std::filesystem::path& InWorkspaceRoot,
                                 bool InRecursive,
                                 bool InDryRun,
                                 bool InProfile,
                                 bool InForceWithLease,
                                 bool InNoVerify,
                                 int InJobs,
                                 bool InVerbose,
                                 const std::string& InRemote,
                                 const std::vector<std::string>& InRepoFilters) -> std::pair<int, workspace::RepoOperationAggregate>;

} // namespace kano::git::commands
