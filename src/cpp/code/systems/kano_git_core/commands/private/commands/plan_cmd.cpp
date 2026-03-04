// plan/ignore commands - native plan pipeline and ignore doctor

#include "command_registry.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace kano::git::commands {
namespace {

struct IgnoreFinding {
    std::filesystem::path repo;
    std::string repoRel;
    std::string repoPath;
    std::string display;
};

struct IgnoreStageEntry {
    std::string repo = ".";
    std::string applyTarget = ".gitignore";
    std::string mergedOutputPath;
    std::vector<std::string> rules;
};

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
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

auto CurrentUtcIso8601() -> std::string {
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

auto ReadFileText(const std::filesystem::path& InPath) -> std::optional<std::string> {
    std::ifstream in(InPath, std::ios::in | std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto WriteFileText(const std::filesystem::path& InPath, const std::string& InText, std::string* OutError = nullptr) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    if (ec) {
        if (OutError != nullptr) {
            *OutError = "cannot create parent directory";
        }
        return false;
    }
    std::ofstream out(InPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        if (OutError != nullptr) {
            *OutError = "cannot open output file";
        }
        return false;
    }
    out << InText;
    return out.good();
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::string {
    const auto rel = InPath.lexically_relative(InRoot);
    if (!rel.empty()) {
        return rel.generic_string();
    }
    return InPath.lexically_normal().generic_string();
}

auto ResolvePath(const std::filesystem::path& InBase, const std::string& InPath) -> std::filesystem::path {
    const std::filesystem::path p(InPath);
    if (p.is_absolute()) {
        return p.lexically_normal();
    }
    return (InBase / p).lexically_normal();
}

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto GitPassThrough(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::PassThrough, InRepo);
}

auto IsProbableIgnoreArtifactPath(const std::string& InPath) -> bool {
    const auto lower = ToLower(InPath);
    const auto containsAny = [&](const std::vector<std::string>& keys) {
        for (const auto& key : keys) {
            if (lower.find(key) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    if (containsAny({"/.cache/", ".cache/", "/.pytest_cache/", ".pytest_cache/", "/.mypy_cache/", ".mypy_cache/", "/.idea/",
                     ".idea/", "/.vscode/", ".vscode/", "/node_modules/", "node_modules/", "/dist/", "dist/", "/obj/",
                     "obj/", "/target/", "target/", "/out/", "out/"})) {
        return true;
    }
    for (const auto& suffix : {".log", ".tmp", ".temp", ".cache", ".bak", ".swp", ".swo", ".class", ".obj", ".o", ".pdb",
                               ".ilk", ".dmp", ".pyc"}) {
        if (lower.size() >= std::strlen(suffix) && lower.ends_with(suffix)) {
            return true;
        }
    }
    return false;
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
    std::sort(repos.begin(), repos.end(), [](const auto& A, const auto& B) {
        return A.generic_string() < B.generic_string();
    });
    repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
    if (repos.empty()) {
        repos.push_back(InRoot.lexically_normal());
    }
    return repos;
}

auto BuildDefaultPlanTemplate(const std::filesystem::path& InWorkspaceRoot) -> std::string {
    const auto dsRoot = (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill" / "assets" / "ignore-sources")
                            .lexically_normal()
                            .generic_string();
    const auto dsManifest =
        (InWorkspaceRoot / ".agents" / "skills" / "kano" / "kano-git-master-skill" / "assets" / "ignore-sources" / "local" /
         "datasource.manifest.json")
            .lexically_normal()
            .generic_string();
    std::ostringstream oss;
    oss << R"json({
  "meta": {
    "schema_version": "3",
    "plan_id": "replace-with-unique-id",
    "generated_at_utc": ")json"
        << CurrentUtcIso8601() << R"json(",
    "executed_at_utc": "",
    "base_head_sha": "replace-with-head-sha",
    "dirty_fingerprint": "replace-with-dirty-fingerprint",
    "planner": {
      "provider": "replace-with-provider",
      "ai-model": "replace-with-ai-model"
    },
    "review": {
      "verdict": "pass",
      "reason": "replace-with-review-summary"
    },
    "ignore_datasource": {
      "root": ")json"
        << dsRoot << R"json(",
      "manifest": ")json"
        << dsManifest << R"json(",
      "prefer_sources": ["kano-local-rules", "github-gitignore"]
    }
  },
  "stages": {
    "ignore": [],
    "commit": [],
    "post_sync": []
  }
})json";
    return oss.str();
}

auto UnescapeJsonString(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size());
    for (std::size_t i = 0; i < InValue.size(); ++i) {
        const char ch = InValue[i];
        if (ch != '\\' || i + 1 >= InValue.size()) {
            out.push_back(ch);
            continue;
        }
        const char next = InValue[i + 1];
        switch (next) {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(next); break;
        }
        i += 1;
    }
    return out;
}

auto SkipJsonWhitespace(const std::string& InText, std::size_t InPos) -> std::size_t {
    std::size_t pos = InPos;
    while (pos < InText.size()) {
        const char ch = InText[pos];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            break;
        }
        pos += 1;
    }
    return pos;
}

auto ParseJsonStringAt(const std::string& InText, std::size_t InPos) -> std::optional<std::pair<std::string, std::size_t>> {
    if (InPos >= InText.size() || InText[InPos] != '"') {
        return std::nullopt;
    }
    std::string raw;
    std::size_t pos = InPos + 1;
    while (pos < InText.size()) {
        const char ch = InText[pos];
        if (ch == '\\') {
            if (pos + 1 >= InText.size()) {
                return std::nullopt;
            }
            raw.push_back(ch);
            raw.push_back(InText[pos + 1]);
            pos += 2;
            continue;
        }
        if (ch == '"') {
            return std::make_pair(UnescapeJsonString(raw), pos + 1);
        }
        raw.push_back(ch);
        pos += 1;
    }
    return std::nullopt;
}

auto FindJsonKeyValueStart(const std::string& InText, const std::string& InKey, std::size_t InFrom = 0) -> std::optional<std::size_t> {
    std::size_t pos = InFrom;
    while (pos < InText.size()) {
        pos = InText.find('"', pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const auto parsed = ParseJsonStringAt(InText, pos);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        const auto& [key, nextPos] = *parsed;
        pos = SkipJsonWhitespace(InText, nextPos);
        if (key == InKey && pos < InText.size() && InText[pos] == ':') {
            return SkipJsonWhitespace(InText, pos + 1);
        }
    }
    return std::nullopt;
}

auto ExtractBracketBody(const std::string& InText, std::size_t InStart, char InOpen, char InClose) -> std::optional<std::string> {
    if (InStart >= InText.size() || InText[InStart] != InOpen) {
        return std::nullopt;
    }
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t pos = InStart; pos < InText.size(); ++pos) {
        const char ch = InText[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == InOpen) {
            depth += 1;
            continue;
        }
        if (ch == InClose) {
            depth -= 1;
            if (depth == 0) {
                return InText.substr(InStart + 1, pos - InStart - 1);
            }
        }
    }
    return std::nullopt;
}

auto ExtractObjectBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    return ExtractBracketBody(InText, *valuePos, '{', '}');
}

auto ExtractArrayBodyForKey(const std::string& InText, const std::string& InKey) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    return ExtractBracketBody(InText, *valuePos, '[', ']');
}

auto SplitTopLevelObjects(const std::string& InArrayBody) -> std::vector<std::string> {
    std::vector<std::string> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    std::size_t objectStart = std::string::npos;
    for (std::size_t pos = 0; pos < InArrayBody.size(); ++pos) {
        const char ch = InArrayBody[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            if (depth == 0) {
                objectStart = pos;
            }
            depth += 1;
            continue;
        }
        if (ch == '}') {
            depth -= 1;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(InArrayBody.substr(objectStart, pos - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return objects;
}

auto ExtractStringField(const std::string& InObjectText, const std::string& InField) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InObjectText, InField);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    const auto parsed = ParseJsonStringAt(InObjectText, *valuePos);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->first;
}

auto ParseIgnoreEntries(const std::string& InText) -> std::vector<IgnoreStageEntry> {
    std::vector<IgnoreStageEntry> out;
    const auto stages = ExtractObjectBodyForKey(InText, "stages");
    if (!stages.has_value()) {
        return out;
    }
    const auto ignoreArray = ExtractArrayBodyForKey(*stages, "ignore");
    if (!ignoreArray.has_value()) {
        return out;
    }
    for (const auto& item : SplitTopLevelObjects(*ignoreArray)) {
        IgnoreStageEntry entry;
        if (const auto repo = ExtractStringField(item, "repo"); repo.has_value()) {
            entry.repo = Trim(*repo);
        }
        if (const auto target = ExtractStringField(item, "apply_target"); target.has_value()) {
            entry.applyTarget = Trim(*target);
        }
        if (const auto merged = ExtractStringField(item, "merged_output_path"); merged.has_value()) {
            entry.mergedOutputPath = Trim(*merged);
        }
        if (const auto candidates = ExtractArrayBodyForKey(item, "candidates"); candidates.has_value()) {
            for (const auto& c : SplitTopLevelObjects(*candidates)) {
                if (const auto rule = ExtractStringField(c, "rule"); rule.has_value()) {
                    const auto v = Trim(*rule);
                    if (!v.empty()) {
                        entry.rules.push_back(v);
                    }
                }
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

auto NormalizeRule(std::string InRule) -> std::string {
    auto r = Trim(std::move(InRule));
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n')) {
        r.pop_back();
    }
    return r;
}

auto MergeGitignore(const std::filesystem::path& InTarget,
                    const std::vector<std::string>& InRules) -> std::string {
    std::vector<std::string> existing;
    if (const auto text = ReadFileText(InTarget); text.has_value()) {
        std::istringstream iss(*text);
        std::string line;
        while (std::getline(iss, line)) {
            existing.push_back(line);
        }
    }

    std::unordered_set<std::string> seen;
    for (const auto& line : existing) {
        const auto norm = NormalizeRule(line);
        if (norm.empty() || norm[0] == '#') {
            continue;
        }
        seen.insert(norm);
    }

    std::ostringstream out;
    for (const auto& line : existing) {
        out << line << "\n";
    }
    for (const auto& rule : InRules) {
        const auto norm = NormalizeRule(rule);
        if (norm.empty() || norm[0] == '#') {
            continue;
        }
        if (seen.insert(norm).second) {
            out << norm << "\n";
        }
    }
    return out.str();
}

auto StampIgnoreAppliedAtAll(std::string InText, const std::string& InTimestamp) -> std::string {
    const std::regex pattern(R"("applied_at_utc"\s*:\s*"")");
    return std::regex_replace(InText, pattern, std::string("\"applied_at_utc\": \"") + InTimestamp + "\"");
}

auto ResolveKogBinaryCommand() -> std::optional<std::string> {
    if (const char* bin = std::getenv("KANO_GIT_BINARY_PATH"); bin != nullptr && std::string(bin).size() > 0) {
        return std::string(bin);
    }
    if (shell::ExecuteCommand("kano-git", {"--version"}, shell::ExecMode::Capture).exitCode == 0) {
        return std::string("kano-git");
    }
    if (shell::ExecuteCommand("kog", {"--version"}, shell::ExecMode::Capture).exitCode == 0) {
        return std::string("kog");
    }
    return std::nullopt;
}

auto DefaultPlanPath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (InWorkspaceRoot / ".kano" / "cache" / "git" / "plans" / "default-plan.json").lexically_normal();
}

} // namespace

void RegisterPlan(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("plan", "Plan pipeline commands");

    auto* init = cmd->add_subcommand("init", "Write plan template");
    auto* initOut = new std::string{};
    auto* initForce = new bool{false};
    init->add_option("--output,-o", *initOut, "Plan output path (default: .kano/cache/git/plans/default-plan.json)");
    init->add_flag("--force,-f", *initForce, "Overwrite existing output");
    init->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto outPath = initOut->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*initOut).lexically_normal();
        if (std::filesystem::exists(outPath) && !*initForce) {
            std::cerr << "Error: output already exists: " << outPath.generic_string() << "\n";
            std::cerr << "Hint: pass --force to overwrite.\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(outPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
            std::cerr << "Error: failed to write plan template: " << outPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
    });

    auto* verify = cmd->add_subcommand("verify", "Verify plan schema");
    auto* verifyFile = new std::string{};
    auto* verifyStage = new std::string{"all"};
    verify->add_option("--plan-file", *verifyFile, "Plan file path");
    verify->add_option("--stage", *verifyStage, "Stage: ignore|commit|all")->default_str("all");
    verify->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto text = *payload;
        const auto stages = ExtractObjectBodyForKey(text, "stages");
        if (stages == std::nullopt || ExtractObjectBodyForKey(text, "meta") == std::nullopt) {
            std::cerr << "Error: plan schema invalid: missing meta/stages\n";
            std::exit(2);
        }
        const auto stage = ToLower(Trim(*verifyStage));
        if (stage == "ignore" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "ignore").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.ignore missing\n";
                std::exit(2);
            }
        }
        if (stage == "commit" || stage == "all") {
            if (!ExtractArrayBodyForKey(*stages, "commit").has_value() || !ExtractArrayBodyForKey(*stages, "post_sync").has_value()) {
                std::cerr << "Error: plan schema invalid: stages.commit/post_sync missing\n";
                std::exit(2);
            }
        }
        std::cout << "Plan verify passed: " << planPath.generic_string() << "\n";
    });

    auto* apply = cmd->add_subcommand("apply", "Apply plan stages");
    apply->allow_extras();
    auto* applyFile = new std::string{};
    auto* applyStage = new std::string{"all"};
    apply->add_option("--plan-file", *applyFile, "Plan file path");
    apply->add_option("--stage", *applyStage, "Stage: ignore|commit|all")->default_str("all");
    apply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = applyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*applyFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto stage = ToLower(Trim(*applyStage));
        if (stage != "ignore" && stage != "commit" && stage != "all") {
            std::cerr << "Error: invalid --stage value: " << *applyStage << " (expected ignore|commit|all)\n";
            std::exit(2);
        }

        if (stage == "ignore" || stage == "all") {
            const auto entries = ParseIgnoreEntries(*payload);
            for (std::size_t idx = 0; idx < entries.size(); ++idx) {
                const auto& e = entries[idx];
                const auto repoAbs = ResolvePath(workspaceRoot, e.repo);
                const auto targetAbs = ResolvePath(repoAbs, e.applyTarget);
                auto mergedAbs = e.mergedOutputPath.empty()
                    ? (workspaceRoot / ".kano" / "cache" / "git" / "plans" / std::format("ignore-merged-{}.gitignore", idx)).lexically_normal()
                    : ResolvePath(workspaceRoot, e.mergedOutputPath);
                const auto mergedText = MergeGitignore(targetAbs, e.rules);
                std::string error;
                if (!WriteFileText(mergedAbs, mergedText, &error)) {
                    std::cerr << "Error: failed to write merged ignore: " << mergedAbs.generic_string() << " (" << error << ")\n";
                    std::exit(2);
                }
                if (!WriteFileText(targetAbs, mergedText, &error)) {
                    std::cerr << "Error: failed to apply ignore target: " << targetAbs.generic_string() << " (" << error << ")\n";
                    std::exit(2);
                }
                std::cout << "[plan][ignore] applied: repo=" << e.repo << " target=" << e.applyTarget
                          << " merged=" << mergedAbs.generic_string() << "\n";
            }
            *payload = StampIgnoreAppliedAtAll(*payload, CurrentUtcIso8601());
            std::string error;
            if (!WriteFileText(planPath, *payload, &error)) {
                std::cerr << "Error: failed to stamp plan applied_at_utc: " << planPath.generic_string() << " (" << error << ")\n";
                std::exit(2);
            }
            std::cout << "[plan][ignore] apply complete\n";
            if (stage == "ignore") {
                std::exit(0);
            }
        }

        // Forward commit stage to existing native commit-push pipeline.
        std::vector<std::string> args{"commit-push", "--plan-file", planPath.generic_string()};
        const auto extras = apply->remaining();
        args.insert(args.end(), extras.begin(), extras.end());
        const auto bin = ResolveKogBinaryCommand();
        if (!bin.has_value()) {
            std::cerr << "Error: cannot resolve kog binary for commit stage forwarding.\n";
            std::exit(2);
        }
        const auto run = shell::ExecuteCommand(*bin, args, shell::ExecMode::PassThrough, workspaceRoot);
        std::exit(run.exitCode);
    });
}

void RegisterIgnore(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("ignore", "Ignore management commands");
    auto* doctor = cmd->add_subcommand("doctor", "Scan tracked files for likely ignore candidates");
    auto* repo = new std::string{};
    auto* limit = new int{200};
    auto* asJson = new bool{false};
    auto* apply = new bool{false};
    auto* dryRun = new bool{false};
    auto* yes = new bool{false};
    doctor->add_option("--repo", *repo, "Workspace/repo root path");
    doctor->add_option("--limit", *limit, "Max findings")->default_val(200);
    doctor->add_flag("--json", *asJson, "Output JSON");
    doctor->add_flag("--apply", *apply, "Apply untrack (git rm --cached) to findings");
    doctor->add_flag("--dry-run", *dryRun, "Print apply actions only");
    doctor->add_flag("--yes,-y", *yes, "Skip interactive confirmation gate");
    doctor->callback([=]() {
        const auto root = repo->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*repo).lexically_normal();
        if (GitCapture(root, {"rev-parse", "--git-dir"}).exitCode != 0) {
            std::cerr << "Error: not a git repository/workspace root: " << root.generic_string() << "\n";
            std::exit(2);
        }
        if (*limit <= 0) {
            std::cerr << "Error: --limit must be a positive integer\n";
            std::exit(2);
        }
        auto repos = DiscoverWorkspaceRepos(root);
        std::vector<IgnoreFinding> findings;
        findings.reserve(static_cast<std::size_t>(*limit));
        for (const auto& repoPath : repos) {
            const auto rel = RelativeDisplayPath(root, repoPath);
            const auto tracked = GitCapture(repoPath, {"ls-files"});
            if (tracked.exitCode != 0) {
                continue;
            }
            std::istringstream iss(tracked.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                auto path = Trim(line);
                if (path.empty()) {
                    continue;
                }
                if (!IsProbableIgnoreArtifactPath(path)) {
                    continue;
                }
                IgnoreFinding f;
                f.repo = repoPath;
                f.repoRel = rel;
                f.repoPath = path;
                f.display = (rel == "." ? path : std::format("{}/{}", rel, path));
                findings.push_back(std::move(f));
                if (static_cast<int>(findings.size()) >= *limit) {
                    break;
                }
            }
            if (static_cast<int>(findings.size()) >= *limit) {
                break;
            }
        }

        if (*asJson) {
            std::cout << "{\n";
            std::cout << "  \"repo_root\": \"" << root.generic_string() << "\",\n";
            std::cout << "  \"count\": " << findings.size() << ",\n";
            std::cout << "  \"findings\": [\n";
            for (std::size_t i = 0; i < findings.size(); ++i) {
                std::cout << "    \"" << findings[i].display << "\"";
                if (i + 1 < findings.size()) {
                    std::cout << ",";
                }
                std::cout << "\n";
            }
            std::cout << "  ]\n";
            std::cout << "}\n";
        } else {
            if (findings.empty()) {
                std::cout << "[ignore-doctor] no tracked artifact-like paths found.\n";
            } else {
                std::cout << "[ignore-doctor] tracked artifact-like paths (review candidates):\n";
                for (const auto& f : findings) {
                    std::cout << "  - " << f.display << "\n";
                }
            }
        }

        if (!*apply) {
            std::exit(0);
        }
        if (!*yes && !*dryRun) {
            std::cerr << "Error: --apply requires --yes (or use --dry-run).\n";
            std::exit(2);
        }

        int removed = 0;
        int failed = 0;
        for (const auto& f : findings) {
            if (*dryRun) {
                std::cout << std::format("[ignore-doctor][dry-run] git -C {} rm --cached -- \"{}\"\n",
                                         f.repo.generic_string(),
                                         f.repoPath);
                continue;
            }
            const auto rm = GitPassThrough(f.repo, {"rm", "--cached", "--", f.repoPath});
            if (rm.exitCode == 0) {
                removed += 1;
                std::cout << "[ignore-doctor][applied] " << f.display << "\n";
            } else {
                failed += 1;
                std::cerr << "[ignore-doctor][failed] " << f.display << "\n";
            }
        }
        if (!*dryRun) {
            std::cout << std::format("[ignore-doctor] apply summary: removed={} failed={}\n", removed, failed);
        }
        std::exit(failed == 0 ? 0 : 2);
    });
}

} // namespace kano::git::commands
