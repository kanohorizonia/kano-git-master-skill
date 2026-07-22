// Thin CLI registration boundary for the sync command.

#include <CLI/CLI.hpp>

#include "sync_cmd.hpp"

namespace kano::git::commands {

void RegisterSync(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand(
        "sync",
        "Pipeline sync stage (origin-latest by default); registered nested-repo mutations run child-first");
    const auto options = MakeSyncCommandOptions();

    auto* preCommit = cmd->add_subcommand("pre-commit", "Repair detached HEAD before commit workflow");
    preCommit->allow_extras();
    preCommit->add_option("--repo", options->preCommit.repo, "Target repository root path");
    preCommit->add_option("--remote", options->preCommit.remote, "Preferred remote name");
    preCommit->add_flag("--dry-run", options->preCommit.dryRun, "Preview detached-head repair actions");
    preCommit->add_option("--native-max-depth", options->preCommit.maxDepth, "Native discovery max depth (0 = unlimited)");
    preCommit->add_flag("--native-no-cache", options->preCommit.noCache, "Disable native discovery cache");
    preCommit->add_flag("--native-refresh-cache", options->preCommit.refreshCache, "Force native cache refresh");
    preCommit->add_flag("--no-recursive,-N", options->preCommit.noRecursive, "Repair only current repository");
    preCommit->add_option("--branch-mode", options->preCommit.branchMode, "Detached-branch inference mode: default|stable-dev");
    preCommit->add_flag("--profile", options->preCommit.profile, "Print native pre-commit timing/profile summary");
    preCommit->callback(MakeSyncPreCommitCommandCallback(*preCommit, options));

    auto* originLatest = cmd->add_subcommand("origin-latest", "Sync to origin default branch latest");
    originLatest->allow_extras();
    originLatest->add_flag("--shell", options->originLatest.shell, "Deprecated compatibility flag (shell path removed)");
    originLatest->add_option("--repo", options->originLatest.repo, "Target repository root path");
    originLatest->add_option("--remote", options->originLatest.remote, "Preferred remote name");
    originLatest->add_flag("--dry-run", options->originLatest.dryRun, "Preview sync actions without modifying repositories");
    originLatest->add_option("--native-max-depth", options->originLatest.maxDepth, "Native discovery max depth (0 = unlimited)");
    originLatest->add_flag("--native-no-cache", options->originLatest.noCache, "Disable native discovery cache");
    originLatest->add_flag("--native-refresh-cache", options->originLatest.refreshCache, "Force native cache refresh");
    originLatest->add_flag("--no-recursive,-N", options->originLatest.noRecursive, "Sync only current repository");
    originLatest->add_flag("--no-auto-stash", options->originLatest.noAutoStash, "Do not auto-stash local changes before sync");
    originLatest->add_flag("--no-auth-preflight", options->originLatest.noAuthPreflight, "Skip Git Credential Manager-focused non-interactive auth preflight before sync");
    originLatest->add_flag("--cleanup-stale-locks", options->originLatest.cleanupStaleLocks, "When auto-stash fails on index.lock and no git/kano-git process is active, remove the stale lock and retry once");
    originLatest->add_option("--jobs", options->originLatest.jobs, "Number of parallel repo workers for recursive sync (default: CPU cores)");
    originLatest->add_option("--execution-policy", options->originLatest.executionPolicy, "Recursive sync execution policy: serial|parallel (default: parallel)");
    originLatest->add_flag("--profile", options->originLatest.profile, "Print native sync timing/profile summary");
    originLatest->callback(MakeSyncOriginLatestCommandCallback(*originLatest, options));

    auto* upstreamForcePush = cmd->add_subcommand("upstream-force-push", "Sync from upstream, force-push to origin");
    upstreamForcePush->allow_extras();
    upstreamForcePush->add_option("--repo", options->upstreamForcePush.repo, "Target repository path");
    upstreamForcePush->add_flag("--dry-run", options->upstreamForcePush.dryRun, "Preview force-push sync actions");
    upstreamForcePush->add_flag("--profile", options->upstreamForcePush.profile, "Print native sync timing/profile summary");
    upstreamForcePush->callback(MakeSyncUpstreamForcePushCommandCallback(*upstreamForcePush, options));

    auto* stableDev = cmd->add_subcommand(
        "stable-dev",
        "Stable-dev sync: fetch upstream tags, rebase current stable branch onto latest stable tag, and retarget to branch_<latestTag> when needed");
    stableDev->allow_extras();
    stableDev->add_flag(
        "--workspace",
        options->stableDev.workspace,
        "Run stable-dev across src/* submodules with upstream remotes; may leave repos in rebase conflict state until resolved");
    stableDev->add_option("--format", options->stableDev.reportFormat, "Workspace report format: compact|table|tsv|json|markdown");
    stableDev->add_option("--repo", options->stableDev.repo, "Single-repo mode target path");
    stableDev->add_flag("--dry-run", options->stableDev.dryRun, "Preview fetch/tag/rebase/branch-retarget actions without modifying repos");
    stableDev->add_flag("--profile", options->stableDev.profile, "Print native sync timing/profile summary");
    stableDev->callback(MakeSyncStableDevCommandCallback(*stableDev, options));

    auto* dev = cmd->add_subcommand("dev", "Dev sync (upstream default branch tip)");
    dev->allow_extras();
    dev->add_option("--repo", options->dev.repo, "Target repository root path");
    dev->add_flag("--dry-run", options->dev.dryRun, "Preview sync actions without modifying repositories");
    dev->add_option("--native-max-depth", options->dev.maxDepth, "Native discovery max depth (0 = unlimited)");
    dev->add_flag("--native-no-cache", options->dev.noCache, "Disable native discovery cache");
    dev->add_flag("--native-refresh-cache", options->dev.refreshCache, "Force native cache refresh");
    dev->add_flag("--no-recursive,-N", options->dev.noRecursive, "Sync only current repository");
    dev->add_flag("--no-auth-preflight", options->dev.noAuthPreflight, "Skip Git Credential Manager-focused non-interactive auth preflight before sync");
    dev->add_flag("--cleanup-stale-locks", options->dev.cleanupStaleLocks, "When auto-stash fails on index.lock and no git/kano-git process is active, remove the stale lock and retry once");
    dev->add_option("--jobs", options->dev.jobs, "Number of parallel repo workers for recursive sync (default: CPU cores)");
    dev->add_option("--execution-policy", options->dev.executionPolicy, "Recursive sync execution policy: serial|parallel (default: parallel)");
    dev->add_flag("--profile", options->dev.profile, "Print native sync timing/profile summary");
    dev->callback(MakeSyncDevCommandCallback(*dev, options));

    auto* launcherUpdate = cmd->add_subcommand("launcher-update-check", "Launcher dev-mode remote update check");
    launcherUpdate->add_option("--repo", options->launcherUpdateCheck.repo, "Launcher repository root path");
    launcherUpdate->add_option("--remote", options->launcherUpdateCheck.remote, "Preferred remote name (fallback: origin/upstream)");
    launcherUpdate->add_flag("--auto-sync", options->launcherUpdateCheck.autoSync, "Auto-run sync without prompt when updates exist");
    launcherUpdate->add_flag("--non-interactive", options->launcherUpdateCheck.nonInteractive, "Disable prompt and skip auto-sync unless --auto-sync");
    launcherUpdate->callback(MakeSyncLauncherUpdateCheckCommandCallback(*launcherUpdate, options));

    cmd->add_flag("--no-recursive,-N", options->defaultSync.noRecursive, "Default sync: only current repository");
    cmd->add_flag("--no-auth-preflight", options->defaultSync.noAuthPreflight, "Default sync: skip Git Credential Manager-focused non-interactive auth preflight");
    cmd->add_flag("--cleanup-stale-locks", options->defaultSync.cleanupStaleLocks, "Default sync: remove stale index.lock automatically when no git/kano-git process is active");
    cmd->add_option("--jobs", options->defaultSync.jobs, "Default sync: number of parallel repo workers for recursive sync (default: CPU cores)");
    cmd->add_option("--execution-policy", options->defaultSync.executionPolicy, "Default sync execution policy: serial|parallel (default: parallel)");
    cmd->add_flag("--profile", options->defaultSync.profile, "Default sync: print native timing/profile summary");
    cmd->allow_extras();
    cmd->callback(MakeDefaultSyncCommandCallback(*cmd, options));
}

} // namespace kano::git::commands
