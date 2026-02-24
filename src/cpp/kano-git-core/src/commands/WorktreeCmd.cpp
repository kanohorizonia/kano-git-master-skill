// worktree command — Git worktree management
// Delegates to: scripts/worktree/*.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registerworktree(CLI::App& app) {
    auto* cmd = app.add_subcommand("worktree", "Git worktree management");

    auto* create = cmd->add_subcommand("create", "Create a new worktree");
    create->allow_extras();
    create->callback([=]() {
        auto& extras = create->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/create-worktree.sh", args);
        std::exit(result.exitCode);
    });

    auto* list = cmd->add_subcommand("list", "List all worktrees");
    list->allow_extras();
    list->callback([=]() {
        auto& extras = list->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/list-worktrees.sh", args);
        std::exit(result.exitCode);
    });

    auto* remove = cmd->add_subcommand("remove", "Remove a worktree");
    remove->allow_extras();
    remove->callback([=]() {
        auto& extras = remove->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/remove-worktree.sh", args);
        std::exit(result.exitCode);
    });

    auto* sync_wt = cmd->add_subcommand("sync", "Sync all worktrees");
    sync_wt->allow_extras();
    sync_wt->callback([=]() {
        auto& extras = sync_wt->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/sync-worktrees.sh", args);
        std::exit(result.exitCode);
    });

    auto* open = cmd->add_subcommand("open", "Open worktree in IDE");
    open->allow_extras();
    open->callback([=]() {
        auto& extras = open->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/open-worktree.sh", args);
        std::exit(result.exitCode);
    });

    auto* orphan = cmd->add_subcommand("create-orphan", "Create orphan branch worktree");
    orphan->allow_extras();
    orphan->callback([=]() {
        auto& extras = orphan->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("worktree/create-orphan-worktree.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
