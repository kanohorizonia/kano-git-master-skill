// svn command — Git-Subversion bridge
// Delegates to: scripts/vcs-bridges/svn/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

void RegisterSvn(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("svn", "Git-Subversion bridge (git-svn)");

    auto* clone = cmd->add_subcommand("clone", "Clone a SVN repository");
    clone->allow_extras();
    clone->callback([=]() {
        auto extras = clone->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/svn/clone.sh", args);
        std::exit(result.exitCode);
    });

    auto* fetch = cmd->add_subcommand("fetch", "Fetch from SVN");
    fetch->allow_extras();
    fetch->callback([=]() {
        auto extras = fetch->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/svn/fetch.sh", args);
        std::exit(result.exitCode);
    });

    auto* dcommit = cmd->add_subcommand("dcommit", "Push commits to SVN");
    dcommit->allow_extras();
    dcommit->callback([=]() {
        auto extras = dcommit->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/svn/dcommit.sh", args);
        std::exit(result.exitCode);
    });

    auto* rebase = cmd->add_subcommand("rebase", "Rebase from SVN");
    rebase->allow_extras();
    rebase->callback([=]() {
        auto extras = rebase->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/svn/rebase.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
