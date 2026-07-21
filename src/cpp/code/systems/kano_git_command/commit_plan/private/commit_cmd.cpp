// Thin public registration facade for commit and amend commands.

#include <CLI/CLI.hpp>

#include "commit_utils.hpp"

namespace kano::git::commands {

void RegisterCommit(CLI::App& InApp) {
    ConfigureCommitCommand(InApp);
}

void RegisterAmend(CLI::App& InApp) {
    ConfigureAmendCommand(InApp);
}

} // namespace kano::git::commands
