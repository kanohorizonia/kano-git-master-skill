// branch command — Branch operations (rebase, compare, cherry-pick)
// Delegates to: scripts/branches/*.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registerbranch(CLI::App& app) {
    auto* cmd = app.add_subcommand("branch", "Branch operations");

    auto* rebase = cmd->add_subcommand("rebase-upstream", "Rebase to upstream latest");
    rebase->allow_extras();
    rebase->callback([=]() {
        auto& extras = rebase->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("branches/rebase-to-upstream-latest.sh", args);
        std::exit(result.exitCode);
    });

    auto* compare = cmd->add_subcommand("compare", "Compare two branches");
    compare->allow_extras();
    compare->callback([=]() {
        auto& extras = compare->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("branches/compare-branches.sh", args);
        std::exit(result.exitCode);
    });

    auto* cherry = cmd->add_subcommand("cherry-pick-batch", "Batch cherry-pick from file");
    cherry->allow_extras();
    cherry->callback([=]() {
        auto& extras = cherry->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("branches/cherry-pick-batch.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
