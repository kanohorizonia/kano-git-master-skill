// doctor command — Environment and repo health checks
// Delegates to: scripts/commit-tools/doctor.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

void RegisterDoctor(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("doctor", "Environment and repository health checks");
    cmd->allow_extras();

    cmd->callback([=]() {
        auto extras = cmd->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/doctor.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
