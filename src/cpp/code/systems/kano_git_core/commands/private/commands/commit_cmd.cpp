// commit command — AI-powered commit message generation
// Delegates to: scripts/commit-tools/commit/smart-commit.sh (and variants)

#include "command_registry.hpp"
#include "shell_executor.hpp"
#include <format>

namespace kano::git::commands {

void RegisterCommit(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit", "AI-powered commit message generation");
    cmd->allow_extras();  // Pass unknown flags through to the script

    // Provider option
    auto* provider = new std::string{};
    cmd->add_option("--provider,-p", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    // Model option
    auto* model = new std::string{};
    cmd->add_option("--model", *model, "AI model to use");

    // Message option
    auto* message = new std::string{};
    cmd->add_option("--message,-m", *message, "Commit message (skips AI generation)");

    // Agent proxy mode
    auto* agent = new std::string{};
    cmd->add_option("--agent", *agent, "Agent proxy mode (codex, copilot, cursor, kiro, claude)");

    // Flags
    auto* bPush = new bool{false};
    cmd->add_flag("--push", *bPush, "Push after commit");

    auto* bNoAiReview = new bool{false};
    cmd->add_flag("--no-ai-review", *bNoAiReview, "Skip AI review gate");

    auto* bStagedOnly = new bool{false};
    cmd->add_flag("--staged-only", *bStagedOnly, "Commit only already-staged changes (skip auto git add)");

    cmd->callback([=]() {
        std::vector<std::string> args;

        // Determine which script variant to use
        std::string script = "commit-tools/commit/smart-commit.sh";

        if (!provider->empty() && *provider != "auto") {
            if (*provider == "copilot") script = "commit-tools/commit/smart-commit-copilot.sh";
            else if (*provider == "codex") script = "commit-tools/commit/smart-commit-codex.sh";
            else if (*provider == "opencode") script = "commit-tools/commit/smart-commit-opencode.sh";
        }

        if (!model->empty())    { args.push_back("--model");   args.push_back(*model); }
        if (!message->empty())  { args.push_back("-m");        args.push_back(*message); }
        if (!agent->empty())    { args.push_back("--agent");   args.push_back(*agent); }
        if (*bPush)             { args.push_back("--push"); }
        if (*bNoAiReview)       { args.push_back("--no-ai-review"); }
        if (*bStagedOnly)       { args.push_back("--staged-only"); }

        // Pass through any extra arguments
        auto extras = cmd->remaining();
        args.insert(args.end(), extras.begin(), extras.end());

        auto result = shell::ExecuteScript(script, args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
