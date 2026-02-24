// version command — prints version information

#include "KanoGit.CommandRegistry.hpp"
#include "../version.hpp"
#include <fmt/core.h>
#include <iostream>

namespace kano::git::commands {

void Registerversion(CLI::App& app) {
    auto* cmd = app.add_subcommand("version", "Show version information");

    cmd->callback([]() {
        fmt::print("{}\n", kano::version::full());
    });
}

} // namespace kano::git::commands
