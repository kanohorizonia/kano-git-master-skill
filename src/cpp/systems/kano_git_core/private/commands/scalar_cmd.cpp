// scalar command — Git Scalar integration for mono-repo performance
// Delegates to: scripts/mono-repo/scalar/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

void RegisterScalar(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("scalar", "Git Scalar mono-repo performance tools");

    auto* reg = cmd->add_subcommand("register", "Register repository with Scalar");
    reg->allow_extras();
    reg->callback([=]() {
        auto extras = reg->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("mono-repo/scalar/register.sh", args);
        std::exit(result.exitCode);
    });

    auto* status = cmd->add_subcommand("status", "Show Scalar status");
    status->allow_extras();
    status->callback([=]() {
        auto extras = status->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("mono-repo/scalar/status.sh", args);
        std::exit(result.exitCode);
    });

    auto* optimize = cmd->add_subcommand("optimize", "Optimize repository");
    optimize->allow_extras();
    optimize->callback([=]() {
        auto extras = optimize->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("mono-repo/scalar/optimize.sh", args);
        std::exit(result.exitCode);
    });

    auto* unreg = cmd->add_subcommand("unregister", "Unregister repository from Scalar");
    unreg->allow_extras();
    unreg->callback([=]() {
        auto extras = unreg->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("mono-repo/scalar/unregister.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
