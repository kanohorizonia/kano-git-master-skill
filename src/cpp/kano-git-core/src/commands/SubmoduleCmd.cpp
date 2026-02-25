// submodule command — Enhanced Git submodule management
// Delegates to: scripts/submodules/smart-submodule.sh (canonical entrypoint)

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void RegisterSubmodule(CLI::App& app) {
    auto* cmd = app.add_subcommand("submodule", "Enhanced submodule management");

    auto* add = cmd->add_subcommand("add", "Add a submodule");
    add->allow_extras();
    add->callback([=]() {
        auto extras = add->remaining();
        std::vector<std::string> args = {"add"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/smart-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* sync = cmd->add_subcommand("sync", "Sync submodule URLs");
    sync->allow_extras();
    sync->callback([=]() {
        auto extras = sync->remaining();
        std::vector<std::string> args = {"sync"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/smart-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* update = cmd->add_subcommand("update", "Update submodules");
    update->allow_extras();
    update->callback([=]() {
        auto extras = update->remaining();
        std::vector<std::string> args = {"update"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/smart-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* remove = cmd->add_subcommand("remove", "Remove a submodule");
    remove->allow_extras();
    remove->callback([=]() {
        auto extras = remove->remaining();
        std::vector<std::string> args = {"remove"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/smart-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* foreach = cmd->add_subcommand("foreach", "Run command on each submodule");
    foreach->allow_extras();
    foreach->callback([=]() {
        auto extras = foreach->remaining();
        std::vector<std::string> args = {"foreach"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/smart-submodule.sh", args);
        std::exit(result.exitCode);
    });

    auto* sync_urls = cmd->add_subcommand("sync-urls", "Sync submodule URLs from .gitmodules");
    sync_urls->allow_extras();
    sync_urls->callback([=]() {
        auto extras = sync_urls->remaining();
        std::vector<std::string> args = {"sync-urls"};
        args.insert(args.end(), extras.begin(), extras.end());
        auto result = shell::ExecuteScript("submodules/smart-submodule.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
