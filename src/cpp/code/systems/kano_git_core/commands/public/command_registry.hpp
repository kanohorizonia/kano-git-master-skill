#pragma once
#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands {

void RegisterAll(CLI::App& InApp);

void RegisterVersion(CLI::App& InApp);
void RegisterCache(CLI::App& InApp);
void RegisterMeta(CLI::App& InApp);
void RegisterComplete(CLI::App& InApp);
void RegisterCompletion(CLI::App& InApp);
void RegisterDirty(CLI::App& InApp);
void RegisterDiscover(CLI::App& InApp);
void RegisterForeach(CLI::App& InApp);
void RegisterGuide(CLI::App& InApp);
void RegisterLog(CLI::App& InApp);
void RegisterRemote(CLI::App& InApp);
void RegisterStatus(CLI::App& InApp);
void RegisterTui(CLI::App& InApp);
void RegisterUpdate(CLI::App& InApp);
void RegisterCommit(CLI::App& InApp);
void RegisterCommitPush(CLI::App& InApp);
void RegisterAmend(CLI::App& InApp);
void RegisterResolve(CLI::App& InApp);
void RegisterSelf(CLI::App& InApp);
void RegisterSync(CLI::App& InApp);
void RegisterPush(CLI::App& InApp);
void RegisterRepo(CLI::App& InApp);

void RegisterWorktree(CLI::App& InApp);
void RegisterSubtree(CLI::App& InApp);
void RegisterSubmodule(CLI::App& InApp);
void RegisterScalar(CLI::App& InApp);

void RegisterP4(CLI::App& InApp);
void RegisterPlan(CLI::App& InApp);
void RegisterSlog(CLI::App& InApp);
void RegisterSvn(CLI::App& InApp);
void RegisterUplog(CLI::App& InApp);

void RegisterBranch(CLI::App& InApp);

void RegisterClone(CLI::App& InApp);
void RegisterDoctor(CLI::App& InApp);
void RegisterIgnore(CLI::App& InApp);

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
                               bool InCleanupStaleLocks = false) -> int;

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

}
