// p4 command — Git-Perforce bridge
// Delegates to: scripts/vcs-bridges/p4/*.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registerp4(CLI::App& app) {
    auto* cmd = app.add_subcommand("p4", "Git-Perforce bridge (git-p4)");

    auto* clone = cmd->add_subcommand("clone", "Clone a Perforce depot");
    clone->allow_extras();
    clone->callback([=]() {
        auto& extras = clone->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/p4/clone.sh", args);
        std::exit(result.exitCode);
    });

    auto* sync = cmd->add_subcommand("sync", "Sync from Perforce");
    sync->allow_extras();
    sync->callback([=]() {
        auto& extras = sync->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/p4/sync.sh", args);
        std::exit(result.exitCode);
    });

    auto* submit = cmd->add_subcommand("submit", "Submit to Perforce");
    submit->allow_extras();
    submit->callback([=]() {
        auto& extras = submit->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/p4/submit.sh", args);
        std::exit(result.exitCode);
    });

    auto* rebase = cmd->add_subcommand("rebase", "Rebase from Perforce");
    rebase->allow_extras();
    rebase->callback([=]() {
        auto& extras = rebase->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("vcs-bridges/p4/rebase.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
