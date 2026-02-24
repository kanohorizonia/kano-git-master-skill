// clone command — Smart clone with upstream support
// Delegates to: scripts/core/smart-clone.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registerclone(CLI::App& app) {
    auto* cmd = app.add_subcommand("clone", "Smart clone with upstream remote support");
    cmd->allow_extras();

    cmd->callback([=]() {
        auto& extras = cmd->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("core/smart-clone.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
