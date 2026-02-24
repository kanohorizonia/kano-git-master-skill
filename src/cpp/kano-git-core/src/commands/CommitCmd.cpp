// commit command — AI-powered commit message generation
// Delegates to: scripts/commit-tools/commit/smart-commit.sh (and variants)

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"
#include <fmt/core.h>

namespace kano::git::commands {

void Registercommit(CLI::App& app) {
    auto* cmd = app.add_subcommand("commit", "AI-powered commit message generation");
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
    auto* push_flag = new bool{false};
    cmd->add_flag("--push", *push_flag, "Push after commit");

    auto* no_ai_review = new bool{false};
    cmd->add_flag("--no-ai-review", *no_ai_review, "Skip AI review gate");

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
        if (*push_flag)         { args.push_back("--push"); }
        if (*no_ai_review)      { args.push_back("--no-ai-review"); }

        // Pass through any extra arguments
        auto& extras = cmd->remaining();
        args.insert(args.end(), extras.begin(), extras.end());

        auto result = shell::ExecuteScript(script, args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
