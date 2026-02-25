// workspace command — Multi-repository workspace operations
// Delegates to: scripts/workspace/*.sh and scripts/core/*.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void RegisterWorkspace(CLI::App& app) {
    auto* cmd = app.add_subcommand("workspace", "Multi-repository workspace operations");

    auto* status = cmd->add_subcommand("status", "Status report of all repos");
    status->allow_extras();
    status->callback([=]() {
        auto extras = status->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("workspace/status-all-repos.sh", args);
        std::exit(result.exitCode);
    });

    auto* update = cmd->add_subcommand("update", "Update all workspace repos");
    update->allow_extras();
    update->callback([=]() {
        auto extras = update->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("workspace/update-workspace-repos.sh", args);
        std::exit(result.exitCode);
    });

    auto* foreach = cmd->add_subcommand("foreach", "Run command on each repo");
    foreach->allow_extras();
    foreach->callback([=]() {
        auto extras = foreach->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("workspace/foreach-repo.sh", args);
        std::exit(result.exitCode);
    });

    auto* discover = cmd->add_subcommand("discover", "Discover all repos in workspace");
    discover->allow_extras();
    discover->callback([=]() {
        auto extras = discover->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("core/discover-repos.sh", args);
        std::exit(result.exitCode);
    });

    auto* update_repo = cmd->add_subcommand("update-repo", "Update a single repo + registered subrepos");
    update_repo->allow_extras();
    update_repo->callback([=]() {
        auto extras = update_repo->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("core/update-repo.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
