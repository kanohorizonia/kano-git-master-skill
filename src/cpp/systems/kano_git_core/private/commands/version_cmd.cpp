// version command — prints version information

#include "command_registry.hpp"
#include "version.hpp"
#include <iostream>

namespace kano::git::commands {

void RegisterVersion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("version", "Show version information");

    cmd->callback([]() {
        std::cout << kano::git::GetVersion() << "\n";
    });
}

} // namespace kano::git::commands
