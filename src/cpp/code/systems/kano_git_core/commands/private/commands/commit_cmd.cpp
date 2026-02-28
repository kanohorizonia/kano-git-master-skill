// commit command — AI-powered commit message generation
// Delegates to: scripts/commit-tools/commit/smart-commit.sh (and variants)

#include "command_registry.hpp"
#include "shell_executor.hpp"
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

struct CommitPreflightReport {
    bool inRepo = false;
    int stagedCount = 0;
    int unstagedCount = 0;
    int untrackedCount = 0;
    std::vector<std::string> riskyFiles;
};

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto LooksRiskyPath(const std::string& InPath) -> bool {
    const std::string lower = [&]() {
        std::string out = InPath;
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }();

    return lower.find(".env") != std::string::npos ||
           lower.find("credentials") != std::string::npos ||
           lower.find("secret") != std::string::npos ||
           lower.find("id_rsa") != std::string::npos ||
           lower.ends_with(".pem") ||
           lower.ends_with(".key");
}

auto RunCommitPreflight() -> CommitPreflightReport {
    CommitPreflightReport report;
    const auto inRepo = shell::ExecuteCommand("git", {"rev-parse", "--is-inside-work-tree"}, shell::ExecMode::Capture);
    report.inRepo = (inRepo.exitCode == 0 && Trim(inRepo.stdoutStr) == "true");
    if (!report.inRepo) {
        return report;
    }

    const auto status = shell::ExecuteCommand("git", {"status", "--porcelain"}, shell::ExecMode::Capture);
    if (status.exitCode != 0) {
        return report;
    }

    std::string line;
    std::string content = status.stdoutStr;
    std::size_t start = 0;
    while (start <= content.size()) {
        const auto end = content.find('\n', start);
        line = (end == std::string::npos) ? content.substr(start) : content.substr(start, end - start);
        start = (end == std::string::npos) ? (content.size() + 1) : (end + 1);

        line = Trim(line);
        if (line.size() < 3) {
            continue;
        }

        const char x = line[0];
        const char y = line[1];
        const auto filePath = Trim(line.substr(3));

        if (x == '?' && y == '?') {
            report.untrackedCount += 1;
        }
        if (x != ' ' && x != '?') {
            report.stagedCount += 1;
        }
        if (y != ' ' || (x == '?' && y == '?')) {
            report.unstagedCount += 1;
        }

        if (!filePath.empty() && LooksRiskyPath(filePath)) {
            report.riskyFiles.push_back(filePath);
        }
    }

    return report;
}

auto PrintCommitPreflight(const CommitPreflightReport& InReport, bool InStagedOnly) -> void {
    std::cout << "=== Commit Preflight (native) ===\n";
    if (!InReport.inRepo) {
        std::cout << "repo: not a git repository\n";
        return;
    }

    std::cout << "staged: " << InReport.stagedCount << "\n";
    std::cout << "unstaged: " << InReport.unstagedCount << "\n";
    std::cout << "untracked: " << InReport.untrackedCount << "\n";
    std::cout << "mode: " << (InStagedOnly ? "staged-only" : "auto-stage shell path") << "\n";

    if (InReport.riskyFiles.empty()) {
        std::cout << "risk: no obvious secret-like file names\n";
    } else {
        std::cout << "risk: potential secret-like files detected\n";
        for (const auto& file : InReport.riskyFiles) {
            std::cout << "  - " << file << "\n";
        }
    }
}

} // namespace

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

    auto* bShell = new bool{false};
    cmd->add_flag("--shell", *bShell, "Use shell fallback implementation directly");

    auto* bPreflightOnly = new bool{false};
    cmd->add_flag("--preflight-only", *bPreflightOnly, "Run native preflight checks and exit without commit");

    auto* bNoNativePreflight = new bool{false};
    cmd->add_flag("--no-native-preflight", *bNoNativePreflight, "Skip native preflight checks before shell commit");

    cmd->callback([=]() {
        if (!*bNoNativePreflight || *bPreflightOnly) {
            const auto report = RunCommitPreflight();
            PrintCommitPreflight(report, *bStagedOnly);
            if (!report.inRepo) {
                std::exit(1);
            }
            if (*bStagedOnly && report.stagedCount == 0) {
                std::cerr << "Preflight blocked: --staged-only but nothing staged\n";
                std::exit(2);
            }
            if (*bPreflightOnly) {
                std::exit(0);
            }
        }

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
        for (const auto& extra : extras) {
            if (extra == "--shell" || extra == "--preflight-only" || extra == "--no-native-preflight") {
                continue;
            }
            args.push_back(extra);
        }

        if (*bShell) {
            auto result = shell::ExecuteScript(script, args);
            std::exit(result.exitCode);
        }

        auto result = shell::ExecuteScript(script, args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
