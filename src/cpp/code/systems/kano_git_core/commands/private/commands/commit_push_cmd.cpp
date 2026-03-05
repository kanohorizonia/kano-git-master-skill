// commit-push command — Orchestrate commit -> sync -> push workflow

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace kano::git::commands {
namespace {

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

auto ToLower(std::string InValue) -> std::string {
    std::transform(InValue.begin(), InValue.end(), InValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return InValue;
}

auto NormalizeInputPathForCurrentPlatform(std::string InPath) -> std::string {
    auto path = Trim(std::move(InPath));
    if (path.empty()) {
        return path;
    }
#if defined(_WIN32)
    auto toWindowsPath = [](char drive, std::string rest) -> std::string {
        for (auto& ch : rest) {
            if (ch == '/') {
                ch = '\\';
            }
        }
        if (!rest.empty() && (rest.front() == '\\' || rest.front() == '/')) {
            rest.erase(rest.begin());
        }
        std::string out;
        out.reserve(rest.size() + 3);
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(drive))));
        out.append(":\\");
        out.append(rest);
        return out;
    };

    if (path.rfind("/cygdrive/", 0) == 0 && path.size() > 11 && std::isalpha(static_cast<unsigned char>(path[10])) &&
        path[11] == '/') {
        return toWindowsPath(path[10], path.substr(12));
    }
    if (path.rfind("/mnt/", 0) == 0 && path.size() > 6 && std::isalpha(static_cast<unsigned char>(path[5])) &&
        path[6] == '/') {
        return toWindowsPath(path[5], path.substr(7));
    }
    if (path.size() > 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == '/') {
        return toWindowsPath(path[1], path.substr(3));
    }
#endif
    return path;
}

auto ParseReposCsv(const std::string& InCsv) -> std::vector<std::string> {
    std::vector<std::string> repos;
    std::istringstream iss(InCsv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        token = Trim(token);
        if (token.empty()) {
            continue;
        }
        repos.push_back(token);
    }
    return repos;
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
        repos.push_back(repo.path.lexically_normal());
    }
    if (repos.empty()) {
        repos.push_back(InRoot.lexically_normal());
    }
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
    return repos;
}

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill").lexically_normal();
}

auto LoadNormalizedLineSet(const std::filesystem::path& InFile) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    std::ifstream in(InFile);
    if (!in) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        std::replace(t.begin(), t.end(), '\\', '/');
        out.insert(ToLower(t));
    }
    return out;
}

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool {
    auto p = InPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    const auto lower = ToLower(p);
    auto contains = [&](const std::string& token) { return lower.find(token) != std::string::npos; };
    if (contains("/.cache/") || contains("/.pytest_cache/") || contains("/.mypy_cache/") || contains("/.idea/") || contains("/.vscode/")) {
        return true;
    }
    if (contains("/node_modules/") || contains("/dist/") || contains("/build/") || contains("/bin/") || contains("/obj/") || contains("/target/")) {
        return true;
    }
    return lower.ends_with(".log") || lower.ends_with(".tmp") || lower.ends_with(".temp") || lower.ends_with(".cache") ||
           lower.ends_with(".bak") || lower.ends_with(".swp") || lower.ends_with(".swo") || lower.ends_with(".class") ||
           lower.ends_with(".obj") || lower.ends_with(".o") || lower.ends_with(".pdb") || lower.ends_with(".ilk") ||
           lower.ends_with(".dmp") || lower.ends_with(".pyc");
}

struct SecretRule {
    std::string id;
    std::regex pattern;
};

struct SecretFinding {
    std::string repo;
    std::string file;
    int line = 0;
    std::string ruleId;
    std::string preview;
};

auto LoadSecretRules(const std::filesystem::path& InFile) -> std::vector<SecretRule> {
    std::vector<SecretRule> out;
    std::ifstream in(InFile);
    if (!in) {
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto t = Trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const auto delim = t.find('|');
        if (delim == std::string::npos) {
            continue;
        }
        const auto id = Trim(t.substr(0, delim));
        const auto expr = Trim(t.substr(delim + 1));
        if (id.empty() || expr.empty()) {
            continue;
        }
        try {
            out.push_back({id, std::regex(expr, std::regex::ECMAScript | std::regex::icase)});
        } catch (const std::regex_error&) {
            continue;
        }
    }
    return out;
}

auto CollectChangedCandidateFiles(const std::filesystem::path& InRepo) -> std::vector<std::string> {
    std::unordered_set<std::string> files;
    const std::vector<std::vector<std::string>> commands = {
        {"diff", "--cached", "--name-only"},
        {"diff", "--name-only"},
        {"ls-files", "--others", "--exclude-standard"},
    };
    for (const auto& args : commands) {
        const auto out = shell::ExecuteCommand("git", args, shell::ExecMode::Capture, InRepo);
        if (out.exitCode != 0) {
            continue;
        }
        std::istringstream iss(out.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            auto path = Trim(line);
            if (path.empty()) {
                continue;
            }
            const auto abs = (InRepo / std::filesystem::path(path)).lexically_normal();
            std::error_code ec;
            if (!std::filesystem::exists(abs, ec) || std::filesystem::is_directory(abs, ec)) {
                continue;
            }
            files.insert(path);
        }
    }
    return std::vector<std::string>(files.begin(), files.end());
}

auto ScanFileForSecretRules(const std::filesystem::path& InRepo,
                            const std::string& InFile,
                            const std::vector<SecretRule>& InRules,
                            const int InLimit,
                            std::vector<SecretFinding>* OutFindings) -> void {
    if (OutFindings == nullptr || static_cast<int>(OutFindings->size()) >= InLimit) {
        return;
    }
    std::ifstream in((InRepo / std::filesystem::path(InFile)).lexically_normal());
    if (!in) {
        return;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line) && static_cast<int>(OutFindings->size()) < InLimit) {
        lineNo += 1;
        for (const auto& rule : InRules) {
            if (std::regex_search(line, rule.pattern)) {
                SecretFinding f;
                f.file = InFile;
                f.line = lineNo;
                f.ruleId = rule.id;
                f.preview = Trim(line);
                if (f.preview.size() > 160) {
                    f.preview = f.preview.substr(0, 160) + "...";
                }
                OutFindings->push_back(std::move(f));
                break;
            }
        }
    }
}

auto IsAgentModeEnabled() -> bool {
    const char* raw = std::getenv("KANO_AGENT_MODE");
    if (raw == nullptr) {
        return false;
    }
    const auto value = Trim(raw);
    if (value.empty() || value == "0" || value == "false" || value == "FALSE") {
        return false;
    }
    return true;
}

auto CaptureHeadShortSha(const std::filesystem::path& InWorkingDir) -> std::string {
    const auto out = shell::ExecuteCommand("git", {"rev-parse", "--short", "HEAD"}, shell::ExecMode::Capture, InWorkingDir);
    if (out.exitCode != 0) {
        return "nohead";
    }
    auto value = Trim(out.stdoutStr);
    if (value.empty()) {
        return "nohead";
    }
    for (auto& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }
    return value;
}

auto CurrentUtcTimestampCompact() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(buffer);
}

auto CurrentUtcTimestampIso8601() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &tt);
#else
    gmtime_r(&tt, &utc);
#endif
    char buffer[32] = {0};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

auto DefaultCommitPlanOutputPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    const auto headShort = CaptureHeadShortSha(InWorkspaceRoot);
    const auto stamp = CurrentUtcTimestampCompact();
    return (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" /
            ("plan-" + stamp + "-" + headShort + ".json"))
        .lexically_normal();
}

auto BuildCommitPlanTemplateJson(const std::string& InGeneratedAtUtc) -> std::string {
    std::ostringstream oss;
    oss << R"json({
  "meta": {
    "schema_version": "2",
    "plan_id": "replace-with-unique-id",
    "generated_at_utc": ")json" << InGeneratedAtUtc << R"json(",
    "executed_at_utc": "",
    "base_head_sha": "replace-with-head-sha",
    "dirty_fingerprint": "replace-with-dirty-fingerprint",
    "planner": {
      "provider": "human",
      "ai-model": ""
    },
    "review": {
      "verdict": "pass",
      "reason": "replace-with-review-summary"
    }
  },
  "stages": {
    "commit": [
      {
        "repo": ".",
        "commits": [
          { "message": "feat(scope): replace-with-commit-message" }
        ]
      }
    ],
    "post_sync": []
  }
}
)json";
    return oss.str();
}

auto WriteTextFile(const std::filesystem::path& InPath,
                   const std::string& InText,
                   std::string* OutError) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot create parent directories";
        }
        return false;
    }

    std::ofstream out(InPath, std::ios::trunc | std::ios::binary);
    if (!out) {
        if (OutError != nullptr) {
            *OutError = "cannot open output file";
        }
        return false;
    }
    out << InText;
    if (!out.good()) {
        if (OutError != nullptr) {
            *OutError = "failed while writing output file";
        }
        return false;
    }
    return true;
}

auto StampCommitPlanExecutedAt(const std::filesystem::path& InPath,
                               std::string* OutError) -> bool {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) {
        if (OutError != nullptr) {
            *OutError = "cannot open plan file";
        }
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    if (text.empty()) {
        if (OutError != nullptr) {
            *OutError = "plan file is empty";
        }
        return false;
    }

    const auto executedAt = CurrentUtcTimestampIso8601();
    const std::regex executedPattern(R"("executed_at_utc"\s*:\s*"[^"]*")");
    if (std::regex_search(text, executedPattern)) {
        text = std::regex_replace(
            text,
            executedPattern,
            std::string("\"executed_at_utc\": \"") + executedAt + "\"",
            std::regex_constants::format_first_only);
    } else {
        const std::regex metaPattern(R"("meta"\s*:\s*\{)");
        if (std::regex_search(text, metaPattern)) {
            text = std::regex_replace(
                text,
                metaPattern,
                std::string("\"meta\": {\n    \"executed_at_utc\": \"") + executedAt + "\",",
                std::regex_constants::format_first_only);
        } else {
            if (OutError != nullptr) {
                *OutError = "cannot locate meta object in plan";
            }
            return false;
        }
    }

    return WriteTextFile(InPath, text, OutError);
}

auto RunPipelineSafetyGatesForNonAiCommitPush(const std::filesystem::path& InWorkspaceRoot) -> void {
    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();

    const auto allowIgnoreGate = ToLower(Trim(std::getenv("KOG_ALLOW_IGNORE_GATE") == nullptr ? "" : std::getenv("KOG_ALLOW_IGNORE_GATE")));
    const auto ignoreGateMode = ToLower(Trim(std::getenv("KOG_IGNORE_GATE") == nullptr ? "on" : std::getenv("KOG_IGNORE_GATE")));
    if (!(allowIgnoreGate == "1" || allowIgnoreGate == "true") && ignoreGateMode != "off") {
        const auto allowlistPath =
            (ResolveSkillRoot(workspaceRoot) / "assets" / "gitignore" / "ignore-gate-allowlist.txt").lexically_normal();
        const auto allowlist = LoadNormalizedLineSet(allowlistPath);

        auto repos = DiscoverWorkspaceRepos(workspaceRoot);
        std::vector<std::string> findings;
        findings.reserve(20);
        for (const auto& repo : repos) {
            const auto rel = repo.lexically_relative(workspaceRoot).generic_string();
            const auto repoLabel = rel.empty() ? "." : rel;
            const auto untracked = shell::ExecuteCommand("git", {"ls-files", "--others", "--exclude-standard"}, shell::ExecMode::Capture, repo);
            if (untracked.exitCode != 0) {
                continue;
            }
            std::istringstream iss(untracked.stdoutStr);
            std::string path;
            while (std::getline(iss, path)) {
                auto p = Trim(path);
                if (p.empty() || !IsProbableIgnoreArtifactPath(p)) {
                    continue;
                }
                std::replace(p.begin(), p.end(), '\\', '/');
                const auto key = ToLower(repoLabel == "." ? p : (repoLabel + "/" + p));
                if (allowlist.find(key) != allowlist.end()) {
                    continue;
                }
                findings.push_back(key);
                if (findings.size() >= 20) {
                    break;
                }
            }
            if (findings.size() >= 20) {
                break;
            }
        }
        if (!findings.empty()) {
            std::cerr << "Error: ignore gate failed (commit-push); unresolved untracked artifact-like files detected.\n";
            for (const auto& f : findings) {
                std::cerr << "  - " << f << "\n";
            }
            std::cerr << "Hint: update .gitignore first, then regenerate plan.\n";
            std::cerr << "Hint: override once with --allow-ignore-gate (or KOG_ALLOW_IGNORE_GATE=1).\n";
            std::exit(3);
        }
    }

    const auto disableSecretGate = ToLower(Trim(std::getenv("KOG_DISABLE_SECRET_GATE") == nullptr ? "" : std::getenv("KOG_DISABLE_SECRET_GATE")));
    if (disableSecretGate == "1" || disableSecretGate == "true") {
        return;
    }

    const auto rulesPath = (ResolveSkillRoot(workspaceRoot) / "assets" / "security" / "secret-blacklist.rules").lexically_normal();
    const auto rules = LoadSecretRules(rulesPath);
    if (rules.empty()) {
        return;
    }
    auto repos = DiscoverWorkspaceRepos(workspaceRoot);
    std::vector<SecretFinding> findings;
    findings.reserve(20);
    for (const auto& repo : repos) {
        const auto changedFiles = CollectChangedCandidateFiles(repo);
        if (changedFiles.empty()) {
            continue;
        }
        const auto rel = repo.lexically_relative(workspaceRoot).generic_string();
        const auto repoLabel = rel.empty() ? "." : rel;
        for (const auto& file : changedFiles) {
            if (static_cast<int>(findings.size()) >= 20) {
                break;
            }
            const auto before = findings.size();
            ScanFileForSecretRules(repo, file, rules, 20, &findings);
            for (std::size_t i = before; i < findings.size(); ++i) {
                findings[i].repo = repoLabel;
            }
        }
        if (static_cast<int>(findings.size()) >= 20) {
            break;
        }
    }
    if (!findings.empty()) {
        std::cerr << "Error: secret gate failed (commit-push); potential secrets detected.\n";
        for (const auto& f : findings) {
            std::cerr << std::format("  - [{}/{}:{}] rule={} preview={}\n",
                                     f.repo.empty() ? "." : f.repo,
                                     f.file,
                                     f.line,
                                     f.ruleId,
                                     f.preview);
        }
        std::cerr << "Hint: remove/redact secrets, rotate leaked credentials if needed, then rerun.\n";
        std::cerr << "Hint: disable once with KOG_DISABLE_SECRET_GATE=1 (not recommended).\n";
        std::exit(3);
    }
}

auto RunCommitPushPlanFilePipelineImpl(const std::filesystem::path& InWorkspaceRoot,
                                       const std::string& InNormalizedPlanFile,
                                       const std::vector<std::string>& InExtraArgs) -> int {
    const bool agentMode = IsAgentModeEnabled();
    if (InNormalizedPlanFile.empty()) {
        std::cerr << "Error: plan pipeline requires non-empty --plan-file\n";
        return 2;
    }

    if (!InExtraArgs.empty()) {
        std::cerr << "Error: unsupported extra arguments in plan pipeline mode:";
        for (const auto& extra : InExtraArgs) {
            std::cerr << ' ' << extra;
        }
        std::cerr << "\n";
        return 2;
    }

    {
        std::string stampError;
        const auto planPath = std::filesystem::path(InNormalizedPlanFile).lexically_normal();
        if (!StampCommitPlanExecutedAt(planPath, &stampError)) {
            std::cerr << "Warning: failed to stamp plan executed_at_utc: " << planPath.generic_string();
            if (!stampError.empty()) {
                std::cerr << " (" << stampError << ")";
            }
            std::cerr << "\n";
        }
    }

    if (agentMode) {
        std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
    }

    std::cout << "=== commit-push stage: safety-gates ===\n";
    RunPipelineSafetyGatesForNonAiCommitPush(InWorkspaceRoot);

    std::cout << "=== commit-push stage: pre-commit ===\n";
    {
        const auto preCommitCode = RunSyncPreCommitNative(InWorkspaceRoot, true, false, "default");
        if (preCommitCode != 0) {
            return preCommitCode;
        }
    }

    std::cout << "=== commit-push stage: commit ===\n";
    {
        const auto commitCode = RunCommitNativePlanStage(InWorkspaceRoot, InNormalizedPlanFile, "commit", false);
        if (commitCode != 0) {
            return commitCode;
        }
    }

    std::cout << "=== commit-push stage: sync ===\n";
    {
        const auto syncCode = RunSyncOriginLatestNative(InWorkspaceRoot, true, false);
        if (syncCode != 0) {
            return syncCode;
        }
    }

    std::cout << "=== commit-push stage: post-sync ===\n";
    {
        const auto postCommitCode = RunCommitNativePlanStage(InWorkspaceRoot, InNormalizedPlanFile, "post_sync", false);
        if (postCommitCode != 0) {
            return postCommitCode;
        }
    }

    std::cout << "=== commit-push stage: push ===\n";
    {
        return RunPushNativeSimple(InWorkspaceRoot, true, false, false, false, false, 0, false, "");
    }
}

} // namespace

auto RunCommitPushPlanFilePipeline(const std::filesystem::path& InWorkspaceRoot,
                                   const std::string& InNormalizedPlanFile,
                                   const std::vector<std::string>& InExtraArgs) -> int {
    return RunCommitPushPlanFilePipelineImpl(InWorkspaceRoot, InNormalizedPlanFile, InExtraArgs);
}

void RegisterCommitPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("commit-push", "Run pre-commit, commit, sync, post-sync, then push in one command");
    cmd->allow_extras();

    auto* repos = new std::string{};
    auto* noRecursive = new bool{false};
    auto* message = new std::string{};
    auto* commitPlanFile = new std::string{};
    auto* writeCommitPlanTemplate = new bool{false};
    auto* commitPlanOut = new std::string{};
    auto* aiProvider = new std::string{};
    auto* aiModel = new std::string{};
    auto* aiAuto = new bool{false};
    auto* noAiReview = new bool{false};
    auto* stagedOnly = new bool{false};
    auto* dryRun = new bool{false};
    auto* profile = new bool{false};
    auto* branchMode = new std::string{"default"};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* jobs = new int{0};
    auto* verbose = new bool{false};
    auto* remote = new std::string{};

    cmd->add_option("--repos", *repos, "Target repos (comma-separated)");
    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only operate on current repository (or provided --repos)");
    cmd->add_option("--message,-m", *message, "Commit message (skip AI generation)");
    cmd->add_option("--plan-file", *commitPlanFile, "Plan JSON file (stage-aware)");
    cmd->add_flag("--write-plan-template", *writeCommitPlanTemplate, "Write plan template JSON and exit");
    cmd->add_option("--plan-out", *commitPlanOut, "Template output path (default: .kano/cache/git/plans/plan-<utc>-<head>.json)");
    cmd->add_option("--ai-provider", *aiProvider, "AI provider for commit (copilot, codex, opencode)");
    cmd->add_option("--ai-model", *aiModel, "AI model for commit");
    cmd->add_flag("--ai-auto", *aiAuto, "Enable commit AI auto mode");
    cmd->add_flag("--no-ai-review", *noAiReview, "Skip AI review gate for commit");
    cmd->add_flag("--staged-only", *stagedOnly, "Commit only staged changes");
    cmd->add_flag("--dry-run", *dryRun, "Preview commit/sync/push actions without modifying repositories");
    cmd->add_flag("--profile", *profile, "Print commit-push stage timing summary");
    cmd->add_option("--branch-mode", *branchMode, "Detached branch inference mode for pre-commit: default|stable-dev");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Use force-with-lease for push");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to push");
    cmd->add_option("--jobs", *jobs, "Push parallel workers");
    cmd->add_flag("--verbose", *verbose, "Verbose push output");
    cmd->add_option("--remote", *remote, "Remote filter for push");

    cmd->callback([=]() {
        const auto totalStart = std::chrono::steady_clock::now();
        long long preCommitMillis = 0;
        long long commitMillis = 0;
        long long syncMillis = 0;
        long long postSyncMillis = 0;
        long long pushMillis = 0;

        const auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in commit-push mode:";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        if (*writeCommitPlanTemplate) {
            const auto outPath = commitPlanOut->empty()
                ? DefaultCommitPlanOutputPath(workspaceRoot)
                : std::filesystem::path(NormalizeInputPathForCurrentPlatform(*commitPlanOut)).lexically_normal();
            std::string error;
            if (!WriteTextFile(outPath, BuildCommitPlanTemplateJson(CurrentUtcTimestampIso8601()), &error)) {
                std::cerr << "Error: failed to write plan template to " << outPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
            std::exit(0);
        }
        if (!commitPlanOut->empty()) {
            std::cerr << "Error: --plan-out requires --write-plan-template\n";
            std::exit(2);
        }

        const auto normalizedCommitPlanFile = NormalizeInputPathForCurrentPlatform(*commitPlanFile);
        const bool hasCommitPlan = !normalizedCommitPlanFile.empty();
        const bool aiModeRequested = *aiAuto || !aiProvider->empty() || !aiModel->empty();
        if (hasCommitPlan && aiModeRequested) {
            std::cerr << "Error: --plan-file cannot be combined with --ai-* flags.\n";
            std::cerr << "Hint: fill plan first (kog plan runbook commit), then run commit-push with --plan-file only.\n";
            std::exit(2);
        }
        if (hasCommitPlan && !message->empty()) {
            std::cerr << "Error: --plan-file cannot be combined with --message/-m.\n";
            std::cerr << "Hint: set commit messages in plan entries.\n";
            std::exit(2);
        }
        if (hasCommitPlan && *stagedOnly) {
            std::cerr << "Error: --plan-file cannot be combined with --staged-only.\n";
            std::cerr << "Hint: plan apply handles staging per plan entries.\n";
            std::exit(2);
        }
        if (hasCommitPlan && *noAiReview) {
            std::cerr << "Error: --plan-file cannot be combined with --no-ai-review.\n";
            std::cerr << "Hint: AI review policy is captured when plan is prepared.\n";
            std::exit(2);
        }

        const bool canUsePlanPipelineFastPath = hasCommitPlan &&
                                                repos->empty() &&
                                                !*noRecursive &&
                                                message->empty() &&
                                                !*dryRun &&
                                                !*profile &&
                                                *branchMode == "default" &&
                                                !*forceWithLease &&
                                                !*noVerify &&
                                                *jobs <= 0 &&
                                                !*verbose &&
                                                remote->empty();

        if (canUsePlanPipelineFastPath) {
            const auto code = RunCommitPushPlanFilePipeline(workspaceRoot, normalizedCommitPlanFile, {});
            std::exit(code);
        }

        if (hasCommitPlan) {
            std::string stampError;
            const auto planPath = std::filesystem::path(normalizedCommitPlanFile).lexically_normal();
            if (!StampCommitPlanExecutedAt(planPath, &stampError)) {
                std::cerr << "Warning: failed to stamp plan executed_at_utc: "
                          << planPath.generic_string();
                if (!stampError.empty()) {
                    std::cerr << " (" << stampError << ")";
                }
                std::cerr << "\n";
            }
        }

        if (IsAgentModeEnabled() && hasCommitPlan) {
            std::cout << "[commit-push] agent mode + --plan-file detected; using plan-driven flow.\n";
        }

        if (!aiModeRequested) {
            std::cout << "=== commit-push stage: safety-gates ===\n";
            RunPipelineSafetyGatesForNonAiCommitPush(workspaceRoot);
        }

        std::cout << "=== commit-push stage: pre-commit ===\n";
        const auto preCommitStart = std::chrono::steady_clock::now();
        const auto repoList = ParseReposCsv(*repos);
        if (!repoList.empty()) {
            for (const auto& repo : repoList) {
                const auto repoRoot = (workspaceRoot / std::filesystem::path(repo)).lexically_normal();
                const auto preCommitCode = RunSyncPreCommitNative(repoRoot, false, *dryRun, *branchMode);
                if (preCommitCode != 0) {
                    std::exit(preCommitCode);
                }
            }
        } else {
            const auto preCommitCode = RunSyncPreCommitNative(workspaceRoot, !*noRecursive, *dryRun, *branchMode);
            if (preCommitCode != 0) {
                std::exit(preCommitCode);
            }
        }
        const auto preCommitEnd = std::chrono::steady_clock::now();
        preCommitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(preCommitEnd - preCommitStart).count();

        std::cout << "=== commit-push stage: commit ===\n";
        const auto commitStart = std::chrono::steady_clock::now();
        const auto commitResult = hasCommitPlan
            ? shell::ExecResult{
                RunCommitNativePlanStage(workspaceRoot, normalizedCommitPlanFile, "commit", false), "", ""}
            : shell::ExecResult{
                RunCommitNativeSimple(
                    workspaceRoot,
                    *repos,
                    *noRecursive,
                    *message,
                    *stagedOnly,
                    *dryRun,
                    *aiProvider,
                    *aiModel,
                    *aiAuto,
                    *noAiReview,
                    false),
                "", ""};
        const auto commitEnd = std::chrono::steady_clock::now();
        commitMillis = std::chrono::duration_cast<std::chrono::milliseconds>(commitEnd - commitStart).count();
        if (commitResult.exitCode != 0) {
            std::exit(commitResult.exitCode);
        }

        std::cout << "=== commit-push stage: sync ===\n";
        const auto syncStart = std::chrono::steady_clock::now();
        if (!repoList.empty()) {
            for (const auto& repo : repoList) {
                const auto repoRoot = (workspaceRoot / std::filesystem::path(repo)).lexically_normal();
                const auto syncCode = RunSyncOriginLatestNative(repoRoot, false, *dryRun);
                if (syncCode != 0) {
                    std::exit(syncCode);
                }
            }
        } else {
            const auto syncCode = RunSyncOriginLatestNative(workspaceRoot, !*noRecursive, *dryRun);
            if (syncCode != 0) {
                std::exit(syncCode);
            }
        }
        const auto syncEnd = std::chrono::steady_clock::now();
        syncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(syncEnd - syncStart).count();

        std::cout << "=== commit-push stage: post-sync ===\n";
        const auto postCommitStart = std::chrono::steady_clock::now();
        const auto postCommitResult = hasCommitPlan
            ? shell::ExecResult{
                RunCommitNativePlanStage(workspaceRoot, normalizedCommitPlanFile, "post_sync", false), "", ""}
            : shell::ExecResult{
                RunCommitNativeSimple(
                    workspaceRoot,
                    *repos,
                    *noRecursive,
                    *message,
                    *stagedOnly,
                    *dryRun,
                    *aiProvider,
                    *aiModel,
                    *aiAuto,
                    *noAiReview,
                    false),
                "", ""};
        const auto postCommitEnd = std::chrono::steady_clock::now();
        postSyncMillis = std::chrono::duration_cast<std::chrono::milliseconds>(postCommitEnd - postCommitStart).count();
        if (postCommitResult.exitCode != 0) {
            std::exit(postCommitResult.exitCode);
        }

        std::cout << "=== commit-push stage: push ===\n";
        const auto pushStart = std::chrono::steady_clock::now();
        const auto pushExitCode = RunPushNativeSimple(
            workspaceRoot,
            !*noRecursive,
            *dryRun,
            *profile,
            *forceWithLease,
            *noVerify,
            *jobs,
            *verbose,
            *remote);

        const auto pushEnd = std::chrono::steady_clock::now();
        pushMillis = std::chrono::duration_cast<std::chrono::milliseconds>(pushEnd - pushStart).count();

        if (*profile) {
            const auto totalEnd = std::chrono::steady_clock::now();
            const auto totalMillis = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count();
            std::cout << "\n=== Commit-Push Profile Summary ===\n";
            std::cout << "mode: native\n";
            std::cout << "pre_commit_ms: " << preCommitMillis << "\n";
            std::cout << "commit_ms: " << commitMillis << "\n";
            std::cout << "sync_ms: " << syncMillis << "\n";
            std::cout << "post_sync_ms: " << postSyncMillis << "\n";
            std::cout << "push_ms: " << pushMillis << "\n";
            std::cout << "total_ms: " << totalMillis << "\n";
        }

        std::exit(pushExitCode);
    });
}

} // namespace kano::git::commands
