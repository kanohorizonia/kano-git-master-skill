// version command — prints version information

#include "command_registry.hpp"
#include <iostream>

#if defined(KOG_USE_MODULES)
import kano.git.version;
#else
#include "version.hpp"
#endif

namespace kano::git::commands {

void RegisterVersion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("version", "Show version information");

    cmd->callback([]() {
        std::cout << kano::git::GetBuildInfo() << "\n";
    });
}

} // namespace kano::git::commands
