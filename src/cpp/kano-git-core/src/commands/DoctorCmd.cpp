// doctor command — Environment and repo health checks
// Delegates to: scripts/commit-tools/doctor.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registerdoctor(CLI::App& app) {
    auto* cmd = app.add_subcommand("doctor", "Environment and repository health checks");
    cmd->allow_extras();

    cmd->callback([=]() {
        auto& extras = cmd->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/doctor.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
