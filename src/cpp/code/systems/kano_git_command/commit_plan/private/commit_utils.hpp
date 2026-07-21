#pragma once

namespace CLI {
class App;
}

namespace kano::git::commands {

void ConfigureCommitCommand(CLI::App& InApp);
void ConfigureAmendCommand(CLI::App& InApp);

} // namespace kano::git::commands
