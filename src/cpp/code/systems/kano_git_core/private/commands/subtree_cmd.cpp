// subtree command — Git subtree management
// Delegates to: scripts/subtree/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

void RegisterSubtree(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("subtree", "Git subtree management");

    auto* add = cmd->add_subcommand("add", "Add a subtree");
    add->allow_extras();
    add->callback([=]() {
        auto extras = add->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/add-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* pull = cmd->add_subcommand("pull", "Pull subtree updates");
    pull->allow_extras();
    pull->callback([=]() {
        auto extras = pull->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/pull-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* push = cmd->add_subcommand("push", "Push subtree changes");
    push->allow_extras();
    push->callback([=]() {
        auto extras = push->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/push-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* split = cmd->add_subcommand("split", "Split subtree");
    split->allow_extras();
    split->callback([=]() {
        auto extras = split->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/split-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* list = cmd->add_subcommand("list", "List subtrees");
    list->allow_extras();
    list->callback([=]() {
        auto extras = list->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/list-subtrees.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
