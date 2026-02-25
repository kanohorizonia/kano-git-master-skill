// version command — prints version information

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.Version.hpp"
#include <iostream>

namespace kano::git::commands {

void RegisterVersion(CLI::App& app) {
    auto* cmd = app.add_subcommand("version", "Show version information");

    cmd->callback([]() {
        std::cout << kano::git::GetVersion() << "\n";
    });
}

} // namespace kano::git::commands
