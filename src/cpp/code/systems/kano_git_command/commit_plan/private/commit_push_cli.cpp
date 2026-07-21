// Thin CLI registration boundary for the commit-push command.

#include <CLI/CLI.hpp>

#include "commit_push_cmd.hpp"

#include <memory>

namespace kano::git::commands {

void RegisterCommitPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand(
        "commit-push", "Run pre-commit, commit, sync, post-sync, then push in one command");
    cmd->allow_extras();

    const auto options = std::make_shared<CommitPushCommandOptions>();
    cmd->add_option("--repos", options->repos, "Target repos (comma-separated)");
    cmd->add_option("--repo-root", options->repoRoot, "Workspace root/start path used for repo-name lookup");
    cmd->add_option("target", options->target, "Optional repo target root (repo name or relative path)")->required(false);
    cmd->add_flag("--no-recursive,-N", options->noRecursive, "Only operate on current repository (or provided --repos)");
    cmd->add_option("--message,-m", options->message, "Commit message (skip AI generation)");
    cmd->add_option("--plan-file", options->commitPlanFile, "Plan JSON file (stage-aware)");
    cmd->add_flag("--write-plan-template", options->writeCommitPlanTemplate, "Write plan template JSON and exit");
    cmd->add_option("--plan-out", options->commitPlanOut, "Template output path (default: .kano/tmp/git/plans/plan-<utc>-<head>.json)");
    cmd->add_option("--ai-provider", options->aiProvider, "AI provider for commit (copilot, codex, opencode)");
    cmd->add_option("--ai-model", options->aiModel, "AI model for commit");
    cmd->add_option("--ai-commit-generation-mode,--ai-fill-mode", options->aiFillMode, "AI commit generation mode override (single|per-commit|adaptive)");
    cmd->add_flag("--ai-auto", options->aiAuto, "Enable commit AI auto mode");
    cmd->add_flag("--no-ai-review", options->noAiReview, "Skip AI review gate for commit");
    cmd->add_flag("--staged-only", options->stagedOnly, "Commit only staged changes");
    cmd->add_flag("--dry-run", options->dryRun, "Preview commit/sync/push actions without modifying repositories");
    cmd->add_flag("--profile", options->profile, "Print commit-push stage timing summary");
    cmd->add_option("--branch-mode", options->branchMode, "Detached branch inference mode for pre-commit: default|stable-dev");
    cmd->add_flag("--force-with-lease", options->forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", options->noVerify, "Pass --no-verify to push");
    cmd->add_option("--jobs", options->jobs, "Push parallel workers");
    cmd->add_flag("--verbose", options->verbose, "Verbose push output");
    cmd->add_option("--remote", options->remote, "Remote filter for push");
    cmd->add_flag("--yolo", options->yolo, "Enable all permissions for AI sub-agents");

    cmd->callback(MakeCommitPushCommandCallback(*cmd, options));
}

} // namespace kano::git::commands
