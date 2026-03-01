// commit command — Native multi-repo commit workflow (pure C++)

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <format>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace kano::git::commands {
namespace {

struct CommitPreflightReport {
    bool inRepo = false;
    std::filesystem::path repoPath;
    int stagedCount = 0;
    int unstagedCount = 0;
    int untrackedCount = 0;
    std::vector<std::string> riskyFiles;
    std::vector<std::string> stagedFiles;
    std::vector<std::string> unstagedFiles;
    std::vector<std::string> untrackedFiles;
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

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        const auto trimmed = Trim(token);
        if (trimmed.empty()) {
            continue;
        }
        out.emplace_back(trimmed);
    }
    return out;
}

auto JoinReposCsv(const std::vector<std::filesystem::path>& InRepos) -> std::string {
    std::string out;
    for (std::size_t idx = 0; idx < InRepos.size(); ++idx) {
        auto value = InRepos[idx].generic_string();
        if (value.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += value;
    }
    return out;
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path>;

auto NormalizePath(const std::filesystem::path& InPath) -> std::filesystem::path {
    return InPath.lexically_normal();
}

auto ToGeneric(const std::filesystem::path& InPath) -> std::string {
    return NormalizePath(InPath).generic_string();
}

auto ResolveRepoPath(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    if (InPath.empty() || InPath == ".") {
        return NormalizePath(InWorkspaceRoot);
    }
    if (InPath.is_absolute()) {
        return NormalizePath(InPath);
    }
    return NormalizePath(InWorkspaceRoot / InPath);
}

auto PathDepth(const std::filesystem::path& InPath) -> std::size_t {
    std::size_t depth = 0;
    for (const auto& part : InPath) {
        if (!part.empty()) {
            depth += 1;
        }
    }
    return depth;
}

auto DisplayRepoLabel(const std::filesystem::path& InWorkspaceRoot, const std::filesystem::path& InRepo) -> std::string {
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        return ".";
    }
    const auto rel = repoNorm.lexically_relative(rootNorm);
    if (!rel.empty() && rel != ".") {
        return rel.generic_string();
    }
    return repoNorm.generic_string();
}

auto BuildOrderedRepoList(const std::filesystem::path& InWorkspaceRoot, const std::string& InReposCsv) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> repos;
    if (Trim(InReposCsv).empty()) {
        repos = DiscoverWorkspaceRepos(InWorkspaceRoot);
        if (repos.empty()) {
            repos.push_back(InWorkspaceRoot);
        }
    } else {
        const auto parsed = ParseReposCsv(InReposCsv);
        repos.reserve(parsed.size());
        for (const auto& item : parsed) {
            repos.push_back(ResolveRepoPath(InWorkspaceRoot, item));
        }
    }

    std::unordered_set<std::string> seen;
    std::vector<std::filesystem::path> deduped;
    deduped.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto key = ToGeneric(repo);
        if (key.empty()) {
            continue;
        }
        if (seen.insert(key).second) {
            deduped.push_back(repo);
        }
    }

    const auto rootKey = ToGeneric(InWorkspaceRoot);
    std::sort(deduped.begin(), deduped.end(), [&](const auto& A, const auto& B) {
        const auto aKey = ToGeneric(A);
        const auto bKey = ToGeneric(B);
        const bool aIsRoot = aKey == rootKey;
        const bool bIsRoot = bKey == rootKey;
        if (aIsRoot != bIsRoot) {
            return !aIsRoot && bIsRoot;
        }
        const auto aDepth = PathDepth(A);
        const auto bDepth = PathDepth(B);
        if (aDepth != bDepth) {
            return aDepth > bDepth;
        }
        return aKey < bKey;
    });

    return deduped;
}

auto DiscoverWorkspaceRepos(const std::filesystem::path& InRoot) -> std::vector<std::filesystem::path> {
    workspace::DiscoverOptions options;
    options.rootDir = InRoot;
    options.maxDepth = 12;
    options.useCache = true;
    options.metadataLevel = "minimal";

    const auto discovery = workspace::DiscoverRepos(options);
    std::vector<std::filesystem::path> repos;
    repos.reserve(discovery.repos.size());

    for (const auto& repo : discovery.repos) {
        repos.push_back(repo.path);
    }

    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.lexically_normal().generic_string() < B.lexically_normal().generic_string();
    });

    repos.erase(std::unique(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.lexically_normal().generic_string() == B.lexically_normal().generic_string();
    }), repos.end());

    return repos;
}

auto RunCommitPreflight(const std::filesystem::path& InRepo) -> CommitPreflightReport {
    CommitPreflightReport report;
    report.repoPath = InRepo;
    const auto inRepo = GitCapture(InRepo, {"rev-parse", "--is-inside-work-tree"});
    report.inRepo = (inRepo.exitCode == 0 && Trim(inRepo.stdoutStr) == "true");
    if (!report.inRepo) {
        return report;
    }

    const auto status = GitCapture(InRepo, {"-c", "color.status=false", "status", "--porcelain"});
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

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.size() < 3) {
            continue;
        }

        const char x = line[0];
        const char y = line[1];
        const auto filePath = Trim(line.substr(3));

        if (x == '?' && y == '?') {
            report.untrackedCount += 1;
            if (!filePath.empty()) {
                report.untrackedFiles.push_back(filePath);
            }
        }
        if (x != ' ' && x != '?') {
            report.stagedCount += 1;
            if (!filePath.empty()) {
                report.stagedFiles.push_back(filePath);
            }
        }
        if (y != ' ' || (x == '?' && y == '?')) {
            report.unstagedCount += 1;
            if (!filePath.empty() && !(x == '?' && y == '?')) {
                report.unstagedFiles.push_back(filePath);
            }
        }

        if (!filePath.empty() && LooksRiskyPath(filePath)) {
            report.riskyFiles.push_back(filePath);
        }
    }

    return report;
}

auto HasAnyChanges(const CommitPreflightReport& InReport) -> bool {
    return InReport.stagedCount > 0 || InReport.unstagedCount > 0 || InReport.untrackedCount > 0;
}

auto BuildAutoCommitMessage(const std::filesystem::path& InWorkspaceRoot,
                           const std::filesystem::path& InRepo,
                           const CommitPreflightReport& InReport) -> std::string {
    std::string type = "chore";
    bool hasFiles = false;
    bool docsOnly = true;

    auto inspectFile = [&](const std::string& path) {
        if (path.empty()) {
            return;
        }
        hasFiles = true;
        std::string lower = path;
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        const bool isDoc = lower.ends_with(".md") || lower.rfind("docs/", 0) == 0 || lower.find("/docs/") != std::string::npos;
        if (!isDoc) {
            docsOnly = false;
        }
        if (lower.find("test") != std::string::npos) {
            type = "test";
        }
    };

    for (const auto& file : InReport.stagedFiles) {
        inspectFile(file);
    }
    for (const auto& file : InReport.unstagedFiles) {
        inspectFile(file);
    }
    for (const auto& file : InReport.untrackedFiles) {
        inspectFile(file);
    }

    if (type != "test" && hasFiles && docsOnly) {
        type = "docs";
    }

    auto scope = DisplayRepoLabel(InWorkspaceRoot, InRepo);
    if (scope == ".") {
        scope = "root";
    }
    for (auto& c : scope) {
        if (c == '/' || c == '\\' || c == ' ') {
            c = '-';
        }
    }

    const int changedFiles = static_cast<int>(InReport.stagedFiles.size() + InReport.unstagedFiles.size() + InReport.untrackedFiles.size());
    const int safeCount = changedFiles > 0 ? changedFiles : 1;
    return std::format("{}({}): update {} file{}", type, scope, safeCount, safeCount == 1 ? "" : "s");
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto out = GitCapture(InRepo, {"rev-parse", "--abbrev-ref", "HEAD"});
    if (out.exitCode != 0) {
        return {};
    }
    const auto value = Trim(out.stdoutStr);
    if (value == "HEAD") {
        return {};
    }
    return value;
}

auto HasRemote(const std::filesystem::path& InRepo, const std::string& InRemote) -> bool {
    const auto out = GitCapture(InRepo, {"remote", "get-url", InRemote});
    return out.exitCode == 0;
}

auto PushRepo(const std::filesystem::path& InRepo, const std::string& InBranch) -> bool {
    const std::vector<std::string> remotes = {"origin-ssh", "origin-http", "origin"};
    bool triedRemote = false;
    for (const auto& remote : remotes) {
        if (!HasRemote(InRepo, remote)) {
            continue;
        }
        triedRemote = true;
        const auto push = GitPassThrough(InRepo, {"push", remote, InBranch});
        if (push.exitCode == 0) {
            return true;
        }
    }
    return !triedRemote ? false : false;
}

struct RepoCommitResult {
    std::filesystem::path repo;
    bool committed = false;
    bool pushed = false;
    bool failed = false;
    std::string note;
};

auto CommitSingleRepo(const std::filesystem::path& InWorkspaceRoot,
                     const std::filesystem::path& InRepo,
                     const std::string& InMessage,
                     const bool InStagedOnly,
                     const bool InPush) -> RepoCommitResult {
    RepoCommitResult result;
    result.repo = InRepo;

    auto report = RunCommitPreflight(InRepo);
    if (!report.inRepo) {
        result.failed = true;
        result.note = "not a git repository";
        return result;
    }

    if (!HasAnyChanges(report)) {
        result.note = "no changes";
        return result;
    }

    if (InStagedOnly && report.stagedCount == 0) {
        result.note = "staged-only with nothing staged";
        return result;
    }

    if (!InStagedOnly && (report.unstagedCount > 0 || report.untrackedCount > 0)) {
        const auto add = GitPassThrough(InRepo, {"add", "-A"});
        if (add.exitCode != 0) {
            result.failed = true;
            result.note = "git add -A failed";
            return result;
        }
        report = RunCommitPreflight(InRepo);
    }

    if (report.stagedCount == 0) {
        result.note = "nothing staged after preparation";
        return result;
    }

    const auto commitMessage = InMessage.empty() ? BuildAutoCommitMessage(InWorkspaceRoot, InRepo, report) : InMessage;
    const auto commit = GitPassThrough(InRepo, {"commit", "-m", commitMessage});
    if (commit.exitCode != 0) {
        const auto status = RunCommitPreflight(InRepo);
        if (status.stagedCount == 0) {
            result.note = "nothing to commit";
            return result;
        }
        result.failed = true;
        result.note = "git commit failed";
        return result;
    }

    result.committed = true;
    result.note = "committed";

    if (InPush) {
        const auto branch = CurrentBranch(InRepo);
        if (branch.empty()) {
            result.failed = true;
            result.note = "cannot push: detached HEAD or unknown branch";
            return result;
        }

        if (!PushRepo(InRepo, branch)) {
            result.failed = true;
            result.note = "push failed on all origin remotes";
            return result;
        }
        result.pushed = true;
        result.note = "committed + pushed";
    }

    return result;
}

auto PrintCommitSummary(const std::filesystem::path& InWorkspaceRoot,
                        const std::vector<RepoCommitResult>& InResults) -> int {
    int failed = 0;
    int committed = 0;
    int pushed = 0;
    int skipped = 0;

    std::cout << "\n=== Native Commit Summary ===\n";
    std::cout << std::left << std::setw(36) << "Repo"
              << std::setw(12) << "Result"
              << "Detail\n";
    std::cout << std::left << std::setw(36) << "----"
              << std::setw(12) << "------"
              << "------\n";

    for (const auto& item : InResults) {
        const auto repoLabel = DisplayRepoLabel(InWorkspaceRoot, item.repo);
        std::string status;
        if (item.failed) {
            status = "failed";
            failed += 1;
        } else if (item.committed) {
            status = item.pushed ? "pushed" : "committed";
            committed += 1;
            if (item.pushed) {
                pushed += 1;
            }
        } else {
            status = "skipped";
            skipped += 1;
        }

        std::cout << std::left << std::setw(36) << repoLabel
                  << std::setw(12) << status
                  << item.note << "\n";
    }

    std::cout << "\nTotals: committed=" << committed
              << " pushed=" << pushed
              << " skipped=" << skipped
              << " failed=" << failed << "\n";

    return failed == 0 ? 0 : 1;
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

    auto printFileTable = [](const std::string& title, const std::vector<std::string>& files) {
        if (files.empty()) {
            return;
        }
        std::cout << "\n" << title << "\n";
        std::cout << std::left << std::setw(6) << "No." << "Path\n";
        std::cout << std::left << std::setw(6) << "---" << "----\n";
        const std::size_t limit = 25;
        const std::size_t count = std::min(files.size(), limit);
        for (std::size_t i = 0; i < count; ++i) {
            std::cout << std::left << std::setw(6) << (i + 1) << files[i] << "\n";
        }
        if (files.size() > limit) {
            std::cout << "... and " << (files.size() - limit) << " more\n";
        }
    };

    printFileTable("Staged set preview", InReport.stagedFiles);
    printFileTable("Unstaged changes preview", InReport.unstagedFiles);
    printFileTable("Untracked files preview", InReport.untrackedFiles);

    if (InReport.riskyFiles.empty()) {
        std::cout << "risk: no obvious secret-like file names\n";
    } else {
        std::cout << "risk: potential secret-like files detected\n";
        for (const auto& file : InReport.riskyFiles) {
            std::cout << "  - " << file << "\n";
        }
    }

    std::cout << "policy hints:\n";
    if (InReport.stagedCount == 0) {
        std::cout << "  - Stage intended files before commit\n";
    }
    if (InReport.unstagedCount > 0) {
        std::cout << "  - Unstaged changes exist; commit scope may be incomplete\n";
    }
    if (InReport.untrackedCount > 0) {
        std::cout << "  - Untracked files exist; verify if they should be included\n";
    }
}

} // namespace

void RegisterCommit(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit", "Native multi-repo commit workflow (pure C++)");

    auto* repos = new std::string{};
    cmd->add_option("--repos", *repos, "Commit target repos (comma-separated). Default: auto-discover workspace repos");

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
    cmd->add_flag("--shell", *bShell, "Deprecated compatibility flag (shell path removed)");

    auto* bPreflightOnly = new bool{false};
    cmd->add_flag("--preflight-only", *bPreflightOnly, "Run native preflight checks and exit without commit");

    auto* bNoNativePreflight = new bool{false};
    cmd->add_flag("--no-native-preflight", *bNoNativePreflight, "Skip native preflight checks before shell commit");

    cmd->callback([=]() {
        if (*bShell) {
            std::cerr << "Error: --shell is no longer supported; commit workflow is fully native now\n";
            std::exit(2);
        }

        const auto workspaceRoot = std::filesystem::current_path();

        if (!*bNoNativePreflight || *bPreflightOnly) {
            const auto report = RunCommitPreflight(workspaceRoot);
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

        if ((!provider->empty() && *provider != "auto") || !model->empty() || !agent->empty() || !*bNoAiReview) {
            std::cout << "[native-commit] AI provider/model/review flags are compatibility no-ops in pure native mode.\n";
        }

        auto reposCsv = Trim(*repos);
        auto repoList = BuildOrderedRepoList(workspaceRoot, reposCsv);
        if (repoList.empty()) {
            repoList.push_back(workspaceRoot);
        }

        std::vector<RepoCommitResult> results;
        results.reserve(repoList.size());

        for (const auto& repo : repoList) {
            const auto label = DisplayRepoLabel(workspaceRoot, repo);
            std::cout << "\n[commit] " << label << "\n";
            const auto one = CommitSingleRepo(workspaceRoot, repo, *message, *bStagedOnly, *bPush);
            results.push_back(one);
        }

        const auto exitCode = PrintCommitSummary(workspaceRoot, results);
        std::exit(exitCode);
    });
}

} // namespace kano::git::commands
