// converge command — deterministic sync+push with resumable state

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "command_runtime_ops.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace kano::git::commands {
namespace {

struct RepoStatus {
    std::string id;
    std::string absolutePath;
    std::string type;
    std::string managementPolicy;
    std::string dirtyKind;
    std::string branch;
    std::string head;
    std::string remote;
    std::string upstream;
    std::vector<std::string> parentRepos;
    std::vector<std::string> childRepos;
    std::vector<std::string> statusFlags;
    std::vector<std::string> submoduleFacts;
    bool blocksConverge = false;
    std::string blockReason;
    int ahead = 0;
    int behind = 0;
    std::map<std::string, std::string> commandPolicy;
};

struct Snapshot {
    std::filesystem::path workspaceRoot;
    std::vector<RepoStatus> repos;
};

struct PlanLine {
    std::string repo;
    std::string text;
};

struct Plan {
    std::map<std::string, std::size_t> dirtyCounts;
    std::vector<std::vector<std::string>> waves;
    std::vector<PlanLine> sync;
    std::vector<PlanLine> commit;
    std::vector<PlanLine> push;
    std::vector<PlanLine> skippedUnregisteredGitlinks;
    std::vector<PlanLine> skipped;
    std::vector<PlanLine> policy;
    std::vector<PlanLine> blocked;
};

struct BranchWorktree {
    std::string branch;
    std::string location;
    std::string head;
    std::filesystem::path absolutePath;
    bool detached = false;
    bool bare = false;
};

struct BranchAheadBehind {
    int ahead = 0;
    int behind = 0;
    bool hasUpstream = false;
};

struct BranchCandidate {
    std::string name;
    std::string ref;
    bool remoteOnly = false;
    std::string remote;
    std::string remoteBranch;
};

struct DetachedWorktreeEvaluation {
    bool primary = false;
    bool clean = false;
    bool integrated = false;
    std::string integrationProof;
    std::string cleanMessage;
    std::vector<std::string> blockers;
    std::vector<std::string> proposedActions;
};

struct PhaseSummary {
    std::vector<std::string> succeeded;
    std::vector<std::string> failed;
    std::vector<std::string> blocked;
    std::vector<std::string> skipped;
    std::vector<std::string> pending;
    std::map<std::string, std::string> failureCategory;
    std::map<std::string, std::string> failureMessage;
    std::map<std::string, std::string> blockedBy;
    std::map<std::string, std::string> policySkipReason;
    std::map<std::string, bool> retryEligible;
};

struct WorkflowState {
    std::string workflow = "converge";
    std::string schemaName = "kog.convergeWorkflowState";
    int schemaVersion = 1;
    std::filesystem::path workspaceRoot;
    std::string startedAt;
    std::string currentPhase;
    bool recursive = true;
    bool dryRunRequested = false;
    std::vector<std::string> completedPhases;
    std::string blockedReason;
    std::vector<std::string> blockedRepos;
    std::string resumeCommand;
    std::map<std::string, PhaseSummary> phaseResults;
    std::map<std::string, std::vector<std::string>> commandLinesUsed;
    std::vector<std::string> plannedSyncRepos;
    std::vector<std::string> plannedCommitRepos;
    std::vector<std::string> plannedPushRepos;
    std::vector<std::string> plannedBlockedRepos;
    std::vector<std::vector<std::string>> plannedWaves;
    std::string repoGraphFingerprint;
    std::vector<std::string> repoBaselines;
    std::map<std::string, std::string> repoManagementPolicy;
    std::map<std::string, std::string> repoType;
    std::map<std::string, std::map<std::string, std::string>> repoCommandPolicy;
    std::string detectedConflictInfo;
    std::vector<std::string> succeededRepos;
    std::vector<std::string> failedRepos;
    std::vector<std::string> blockedRepoSet;
    std::vector<std::string> skippedRepos;
    std::vector<std::string> pendingRepos;
};

struct IntentCommitGroup {
    std::string key;
    std::string message;
    std::string reviewReason;
    std::vector<std::string> includePaths;
};

struct DirtyPathEntry {
    std::string path;
    std::string originalPath;
    std::string statusKind;
    std::string rawStatus;
};

struct IntentCommitPlan {
    std::filesystem::path planPath;
    std::vector<IntentCommitGroup> groups;
    std::vector<std::string> ambiguousPaths;
    std::vector<std::string> localArtifactPaths;
    std::vector<std::string> addedIgnoreRules;
    std::string error;
};

constexpr const char* kPointerSingleMessage = "[Submodule][Chore] Update Build/Base pointer (NO-TICKET)";
constexpr const char* kPointerMultipleMessage = "[Submodule][Chore] Update shared dependency pointers (NO-TICKET)";
constexpr const char* kPushDisabledPointerBlockReason = "parent pointer references commit from push-disabled repo that is not available remotely";
const std::vector<std::string> kConvergePhases = {
    "status-preflight-plan",
    "commit-local-changes-if-needed",
    "sync-before-push",
    "push-nested-bottom-up",
    "status-delta-after-sync",
    "commit-pointer-updates-if-needed",
    "sync-converge-dependent-repos",
    "push-parents-bottom-up",
    "final-status-summary",
};
constexpr int kMaxConvergePasses = 8;
constexpr const char* kWorktreeHarvestDefaultMessage = "[Converge][Chore] Harvest dirty worktree changes (NO-TICKET)";

std::string Trim(std::string v) {
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
    std::size_t s = 0;
    while (s < v.size() && (v[s] == ' ' || v[s] == '\t')) ++s;
    return v.substr(s);
}

std::string ToLower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return v;
}

bool IsFalsePolicy(const std::map<std::string, std::string>& policy, const std::string& key) {
    const auto it = policy.find(key);
    if (it == policy.end()) return false;
    const auto value = ToLower(Trim(it->second));
    return value == "false" || value == "0" || value == "no" || value == "off" || value == "disabled";
}

bool Allows(const RepoStatus& repo, const std::string& key) { return !IsFalsePolicy(repo.commandPolicy, key); }

std::string PolicyValue(const RepoStatus& repo, const std::string& key) {
    const auto it = repo.commandPolicy.find(key);
    return it == repo.commandPolicy.end() || Trim(it->second).empty() ? "default-true" : it->second;
}

bool Contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

std::string Csv(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << ",";
        out << values[i];
    }
    return out.str();
}

std::filesystem::path ConvergeStatePath(const std::filesystem::path& root) {
    return (root / ".kano" / "tmp" / "workflows" / "converge" / "state.json").lexically_normal();
}

std::string UtcNowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buffer[64] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

nlohmann::json ToJson(const PhaseSummary& summary) {
    nlohmann::json out;
    out["succeeded"] = summary.succeeded;
    out["failed"] = summary.failed;
    out["blocked"] = summary.blocked;
    out["skipped"] = summary.skipped;
    out["pending"] = summary.pending;
    out["failureCategory"] = summary.failureCategory;
    out["failureMessage"] = summary.failureMessage;
    out["blockedBy"] = summary.blockedBy;
    out["policySkipReason"] = summary.policySkipReason;
    out["retryEligible"] = summary.retryEligible;
    return out;
}

PhaseSummary PhaseSummaryFromJson(const nlohmann::json& value) {
    PhaseSummary summary;
    if (const auto it = value.find("succeeded"); it != value.end() && it->is_array()) summary.succeeded = it->get<std::vector<std::string>>();
    if (const auto it = value.find("failed"); it != value.end() && it->is_array()) summary.failed = it->get<std::vector<std::string>>();
    if (const auto it = value.find("blocked"); it != value.end() && it->is_array()) summary.blocked = it->get<std::vector<std::string>>();
    if (const auto it = value.find("skipped"); it != value.end() && it->is_array()) summary.skipped = it->get<std::vector<std::string>>();
    if (const auto it = value.find("pending"); it != value.end() && it->is_array()) summary.pending = it->get<std::vector<std::string>>();
    if (const auto it = value.find("failureCategory"); it != value.end() && it->is_object()) summary.failureCategory = it->get<std::map<std::string, std::string>>();
    if (const auto it = value.find("failureMessage"); it != value.end() && it->is_object()) summary.failureMessage = it->get<std::map<std::string, std::string>>();
    if (const auto it = value.find("blockedBy"); it != value.end() && it->is_object()) summary.blockedBy = it->get<std::map<std::string, std::string>>();
    if (const auto it = value.find("policySkipReason"); it != value.end() && it->is_object()) summary.policySkipReason = it->get<std::map<std::string, std::string>>();
    if (const auto it = value.find("retryEligible"); it != value.end() && it->is_object()) summary.retryEligible = it->get<std::map<std::string, bool>>();
    return summary;
}

nlohmann::json ToJson(const WorkflowState& state) {
    nlohmann::json phaseResults = nlohmann::json::object();
    for (const auto& [phase, summary] : state.phaseResults) phaseResults[phase] = ToJson(summary);

    nlohmann::json out;
    out["workflow"] = state.workflow;
    out["schemaName"] = state.schemaName;
    out["schemaVersion"] = state.schemaVersion;
    out["workspaceRoot"] = state.workspaceRoot.generic_string();
    out["startedAt"] = state.startedAt;
    out["currentPhase"] = state.currentPhase;
    out["recursive"] = state.recursive;
    out["dryRunRequested"] = state.dryRunRequested;
    out["completedPhases"] = state.completedPhases;
    out["blockedReason"] = state.blockedReason;
    out["blockedRepos"] = state.blockedRepos;
    out["resumeCommand"] = state.resumeCommand;
    out["phaseResults"] = phaseResults;
    out["commandLinesUsed"] = state.commandLinesUsed;
    out["convergePlan"] = {
        {"sync", state.plannedSyncRepos},
        {"commit", state.plannedCommitRepos},
        {"push", state.plannedPushRepos},
        {"blocked", state.plannedBlockedRepos},
        {"waves", state.plannedWaves},
    };
    out["repoGraphFingerprint"] = state.repoGraphFingerprint;
    out["repoBaselines"] = state.repoBaselines;
    out["repoTaxonomy"] = {
        {"type", state.repoType},
        {"managementPolicy", state.repoManagementPolicy},
    };
    out["commandPolicy"] = state.repoCommandPolicy;
    out["detectedConflictInfo"] = state.detectedConflictInfo;
    out["results"] = {
        {"succeeded", state.succeededRepos},
        {"failed", state.failedRepos},
        {"blocked", state.blockedRepoSet},
        {"skipped", state.skippedRepos},
        {"pending", state.pendingRepos},
    };
    return out;
}

WorkflowState WorkflowStateFromJson(const nlohmann::json& value) {
    WorkflowState state;
    state.workflow = value.value("workflow", std::string{"converge"});
    state.schemaName = value.value("schemaName", std::string{"kog.convergeWorkflowState"});
    state.schemaVersion = value.value("schemaVersion", 1);
    state.workspaceRoot = std::filesystem::path(value.value("workspaceRoot", std::string{})).lexically_normal();
    state.startedAt = value.value("startedAt", std::string{});
    state.currentPhase = value.value("currentPhase", std::string{});
    state.recursive = value.value("recursive", true);
    state.dryRunRequested = value.value("dryRunRequested", false);
    if (const auto it = value.find("completedPhases"); it != value.end() && it->is_array()) state.completedPhases = it->get<std::vector<std::string>>();
    state.blockedReason = value.value("blockedReason", std::string{});
    if (const auto it = value.find("blockedRepos"); it != value.end() && it->is_array()) state.blockedRepos = it->get<std::vector<std::string>>();
    state.resumeCommand = value.value("resumeCommand", std::string{});
    if (const auto it = value.find("phaseResults"); it != value.end() && it->is_object()) {
        for (const auto& [phase, summary] : it->items()) state.phaseResults[phase] = PhaseSummaryFromJson(summary);
    }
    if (const auto it = value.find("commandLinesUsed"); it != value.end() && it->is_object()) {
        state.commandLinesUsed = it->get<std::map<std::string, std::vector<std::string>>>();
    }
    if (const auto it = value.find("convergePlan"); it != value.end() && it->is_object()) {
        if (const auto p = it->find("sync"); p != it->end() && p->is_array()) state.plannedSyncRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("commit"); p != it->end() && p->is_array()) state.plannedCommitRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("push"); p != it->end() && p->is_array()) state.plannedPushRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("blocked"); p != it->end() && p->is_array()) state.plannedBlockedRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("waves"); p != it->end() && p->is_array()) state.plannedWaves = p->get<std::vector<std::vector<std::string>>>();
    }
    state.repoGraphFingerprint = value.value("repoGraphFingerprint", std::string{});
    if (const auto it = value.find("repoBaselines"); it != value.end() && it->is_array()) state.repoBaselines = it->get<std::vector<std::string>>();
    if (const auto it = value.find("repoTaxonomy"); it != value.end() && it->is_object()) {
        if (const auto p = it->find("type"); p != it->end() && p->is_object()) state.repoType = p->get<std::map<std::string, std::string>>();
        if (const auto p = it->find("managementPolicy"); p != it->end() && p->is_object()) state.repoManagementPolicy = p->get<std::map<std::string, std::string>>();
    }
    if (const auto it = value.find("commandPolicy"); it != value.end() && it->is_object()) {
        state.repoCommandPolicy = it->get<std::map<std::string, std::map<std::string, std::string>>>();
    }
    state.detectedConflictInfo = value.value("detectedConflictInfo", std::string{});
    if (const auto it = value.find("results"); it != value.end() && it->is_object()) {
        if (const auto p = it->find("succeeded"); p != it->end() && p->is_array()) state.succeededRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("failed"); p != it->end() && p->is_array()) state.failedRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("blocked"); p != it->end() && p->is_array()) state.blockedRepoSet = p->get<std::vector<std::string>>();
        if (const auto p = it->find("skipped"); p != it->end() && p->is_array()) state.skippedRepos = p->get<std::vector<std::string>>();
        if (const auto p = it->find("pending"); p != it->end() && p->is_array()) state.pendingRepos = p->get<std::vector<std::string>>();
    }
    return state;
}

bool WriteState(const std::filesystem::path& statePath, const WorkflowState& state) {
    std::error_code ec;
    std::filesystem::create_directories(statePath.parent_path(), ec);
    const auto tmpPath = (statePath.parent_path() / (statePath.filename().string() + ".tmp")).lexically_normal();
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    out << ToJson(state).dump(2) << "\n";
    out.close();
    if (!out.good()) return false;
    std::filesystem::rename(tmpPath, statePath, ec);
    if (ec) {
        std::filesystem::remove(statePath, ec);
        ec.clear();
        std::filesystem::rename(tmpPath, statePath, ec);
        if (ec) return false;
    }
    return true;
}

bool ReadState(const std::filesystem::path& statePath, WorkflowState& state) {
    std::ifstream in(statePath, std::ios::binary);
    if (!in.good()) return false;
    nlohmann::json parsed;
    in >> parsed;
    if (!in.good() && !in.eof()) return false;
    state = WorkflowStateFromJson(parsed);
    return true;
}

void DeleteState(const std::filesystem::path& statePath) {
    std::error_code ec;
    std::filesystem::remove(statePath, ec);
}

std::string SelfBinaryPath() {
    if (const char* path = std::getenv("KANO_GIT_BINARY_PATH"); path != nullptr && *path != '\0') return std::string(path);
#if defined(_WIN32)
    std::string buffer(MAX_PATH, '\0');
    const auto written = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written > 0) {
        buffer.resize(written);
        return std::filesystem::path(buffer).lexically_normal().string();
    }
#else
    std::string buffer(4096, '\0');
    const auto written = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written > 0) {
        buffer.resize(static_cast<std::size_t>(written));
        return std::filesystem::path(buffer).lexically_normal().string();
    }
#endif
    return "kano-git";
}

bool IsTruthyEnvValue(const char* value) {
    if (value == nullptr) return false;
    const auto normalized = ToLower(Trim(std::string(value)));
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

bool IsConvergeAgentModeEnabled() {
    return IsTruthyEnvValue(std::getenv("KANO_AGENT_MODE")) || IsTruthyEnvValue(std::getenv("AGENT_MODE"));
}

std::vector<std::string> JsonStringArray(const nlohmann::json& item, const char* key) {
    std::vector<std::string> out;
    const auto it = item.find(key);
    if (it == item.end() || !it->is_array()) return out;
    for (const auto& v : *it) if (v.is_string()) out.push_back(v.get<std::string>());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::map<std::string, std::string> JsonPolicy(const nlohmann::json& item) {
    std::map<std::string, std::string> out;
    const auto it = item.find("commandPolicy");
    if (it == item.end() || !it->is_object()) return out;
    for (const auto& [key, value] : it->items()) {
        if (value.is_string()) out[key] = value.get<std::string>();
        else if (value.is_boolean()) out[key] = value.get<bool>() ? "true" : "false";
    }
    return out;
}

Snapshot ParseSnapshot(const std::string& jsonText) {
    const auto root = nlohmann::json::parse(jsonText);
    if (root.value("schemaName", std::string{}) != "kog.recursiveStatusSnapshot") {
        throw std::runtime_error("status JSON is not kog.recursiveStatusSnapshot");
    }
    Snapshot snapshot;
    snapshot.workspaceRoot = std::filesystem::path(root.value("workspaceRoot", std::string{})).lexically_normal();
    for (const auto& item : root.value("repos", nlohmann::json::array())) {
        RepoStatus repo;
        repo.id = item.value("id", std::string{});
        repo.absolutePath = item.value("absolutePath", std::string{});
        repo.type = item.value("type", std::string{});
        repo.managementPolicy = item.value("managementPolicy", std::string{});
        repo.dirtyKind = item.value("dirtyKind", std::string{"CLEAN"});
        repo.branch = item.value("branch", std::string{});
        repo.head = item.value("head", std::string{});
        repo.remote = item.value("remote", std::string{});
        repo.upstream = item.value("upstream", std::string{});
        repo.parentRepos = JsonStringArray(item, "parentRepos");
        repo.childRepos = JsonStringArray(item, "childRepos");
        repo.statusFlags = JsonStringArray(item, "statusFlags");
        repo.submoduleFacts = JsonStringArray(item, "submoduleFacts");
        repo.blocksConverge = item.value("blocksConverge", false);
        repo.blockReason = item.value("blockReason", std::string{});
        repo.ahead = item.value("ahead", 0);
        repo.behind = item.value("behind", 0);
        repo.commandPolicy = JsonPolicy(item);
        if (!repo.id.empty()) snapshot.repos.push_back(std::move(repo));
    }
    std::sort(snapshot.repos.begin(), snapshot.repos.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return snapshot;
}

Snapshot LoadSnapshot(const std::filesystem::path& root,
                      int jobs,
                      bool unregisteredScan,
                      std::optional<unsigned int> timeoutOverrideMs = std::nullopt) {
    std::vector<std::string> args{"status", "--recursive", "--format", "json", "--repo-root", root.generic_string(), "--jobs", std::to_string(std::max(1, jobs))};
    args.push_back("--no-fetch-health");
    if (!unregisteredScan) {
        args.push_back("--no-unregistered-scan");
    } else {
        args.push_back("--refresh-cache");
        args.push_back("--unregistered-depth");
        args.push_back("3");
    }
    const auto result = shell::ExecuteCommand(
        SelfBinaryPath(), args, shell::ExecMode::Capture, root, shell::ProgressCallback{}, timeoutOverrideMs);
    if (result.exitCode != 0) throw std::runtime_error("failed to read recursive status snapshot via kog status: " + Trim(result.stderrStr));
    const auto start = result.stdoutStr.find("{\"schemaName\":\"kog.recursiveStatusSnapshot\"");
    const auto end = result.stdoutStr.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        throw std::runtime_error("failed to locate kog.recursiveStatusSnapshot JSON in status output");
    }
    return ParseSnapshot(result.stdoutStr.substr(start, end - start + 1));
}

std::pair<int, int> ParseAheadBehindCounts(const std::string& text) {
    std::istringstream stream(text);
    int ahead = 0;
    int behind = 0;
    stream >> ahead >> behind;
    if (!stream) {
        return {0, 0};
    }
    return {ahead, behind};
}

std::string FirstNonEmptyLine(const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return {};
}

bool IsUnmergedPorcelainStatus(const std::string& status) {
    if (status.size() < 2) {
        return false;
    }
    return status[0] == 'U' || status[1] == 'U' || status == "AA" || status == "DD";
}

std::string DirtyKindFromPorcelain(const std::string& porcelain, int ahead, int behind) {
    bool any = false;
    bool allUntracked = true;
    bool anyStaged = false;
    bool anyUnstaged = false;
    bool anyConflict = false;

    std::istringstream stream(porcelain);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 2) {
            continue;
        }
        any = true;
        const auto status = line.substr(0, 2);
        anyConflict = anyConflict || IsUnmergedPorcelainStatus(status);
        const bool untracked = status == "??";
        allUntracked = allUntracked && untracked;
        anyStaged = anyStaged || (!untracked && status[0] != ' ');
        anyUnstaged = anyUnstaged || (!untracked && status[1] != ' ');
    }

    if (anyConflict) {
        return "CONFLICTED";
    }
    if (any) {
        if (allUntracked) {
            return "UNTRACKED_ONLY";
        }
        if (anyStaged && !anyUnstaged) {
            return "INDEX_DIRTY";
        }
        return "CONTENT_DIRTY";
    }
    if (ahead > 0 && behind > 0) {
        return "DIVERGED";
    }
    if (ahead > 0) {
        return "AHEAD_ONLY";
    }
    if (behind > 0) {
        return "BEHIND_ONLY";
    }
    return "CLEAN";
}

Snapshot LoadCurrentRepoSnapshot(const std::filesystem::path& root) {
    const auto repoRootResult = shell::ExecuteCommand("git", {"rev-parse", "--show-toplevel"}, shell::ExecMode::Capture, root);
    if (repoRootResult.exitCode != 0) {
        throw std::runtime_error("failed to resolve current repository root: " + Trim(repoRootResult.stderrStr));
    }
    const auto repoRoot = std::filesystem::path(FirstNonEmptyLine(repoRootResult.stdoutStr)).lexically_normal();

    RepoStatus repo;
    repo.id = ".";
    repo.absolutePath = repoRoot.generic_string();
    repo.type = "root";
    repo.managementPolicy = "managed";
    repo.head = FirstNonEmptyLine(shell::ExecuteCommand("git", {"rev-parse", "HEAD"}, shell::ExecMode::Capture, repoRoot).stdoutStr);
    repo.branch = FirstNonEmptyLine(shell::ExecuteCommand("git", {"branch", "--show-current"}, shell::ExecMode::Capture, repoRoot).stdoutStr);

    if (!repo.branch.empty()) {
        repo.remote = FirstNonEmptyLine(shell::ExecuteCommand("git", {"config", "--get", "branch." + repo.branch + ".remote"}, shell::ExecMode::Capture, repoRoot).stdoutStr);
        repo.upstream = FirstNonEmptyLine(shell::ExecuteCommand("git", {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}"}, shell::ExecMode::Capture, repoRoot).stdoutStr);
    }
    if (repo.remote.empty()) {
        repo.remote = FirstNonEmptyLine(shell::ExecuteCommand("git", {"remote"}, shell::ExecMode::Capture, repoRoot).stdoutStr);
    }

    int ahead = 0;
    int behind = 0;
    if (!repo.upstream.empty()) {
        const auto counts = shell::ExecuteCommand("git", {"rev-list", "--left-right", "--count", "HEAD..." + repo.upstream}, shell::ExecMode::Capture, repoRoot);
        if (counts.exitCode == 0) {
            const auto parsed = ParseAheadBehindCounts(counts.stdoutStr);
            ahead = parsed.first;
            behind = parsed.second;
        }
    }
    repo.ahead = ahead;
    repo.behind = behind;

    const auto status = shell::ExecuteCommand("git", {"status", "--porcelain=v1", "--untracked-files=all"}, shell::ExecMode::Capture, repoRoot);
    if (status.exitCode != 0) {
        repo.dirtyKind = "STATUS_FAILED";
        repo.blocksConverge = true;
        repo.blockReason = "STATUS_FAILED: git status failed during current repo branch inventory preflight";
        repo.statusFlags.push_back("STATUS_FAILED");
    } else {
        repo.dirtyKind = DirtyKindFromPorcelain(status.stdoutStr, ahead, behind);
    }

    Snapshot snapshot;
    snapshot.workspaceRoot = repoRoot;
    snapshot.repos.push_back(std::move(repo));
    return snapshot;
}

bool EnsureRegisteredLinkedWorktreeExcludes(const Snapshot& snapshot);

Snapshot LoadConvergeSnapshot(const std::filesystem::path& root,
                              int jobs,
                              bool unregisteredScan,
                              bool recursive,
                              bool ensureRegisteredWorktreeExcludes = false,
                              std::optional<unsigned int> timeoutOverrideMs = std::nullopt) {
    if (!recursive) {
        return LoadCurrentRepoSnapshot(root);
    }
    auto snapshot = LoadSnapshot(root, jobs, unregisteredScan, timeoutOverrideMs);
    if (ensureRegisteredWorktreeExcludes && EnsureRegisteredLinkedWorktreeExcludes(snapshot)) {
        snapshot = LoadSnapshot(root, jobs, unregisteredScan, timeoutOverrideMs);
    }
    return std::move(snapshot);
}

std::vector<std::vector<std::string>> Waves(const Snapshot& snapshot) {
    std::unordered_map<std::string, RepoStatus> byId;
    for (const auto& repo : snapshot.repos) byId.emplace(repo.id, repo);
    std::unordered_map<std::string, int> memo;
    std::function<int(const std::string&, std::unordered_set<std::string>&)> waveFor = [&](const std::string& id, std::unordered_set<std::string>& visiting) {
        if (memo.contains(id)) return memo[id];
        if (visiting.contains(id)) return memo[id] = 0;
        visiting.insert(id);
        int wave = 0;
        if (const auto it = byId.find(id); it != byId.end()) {
            for (const auto& child : it->second.childRepos) if (byId.contains(child)) wave = std::max(wave, waveFor(child, visiting) + 1);
        }
        visiting.erase(id);
        return memo[id] = wave;
    };
    int maxWave = 0;
    for (const auto& repo : snapshot.repos) {
        std::unordered_set<std::string> visiting;
        maxWave = std::max(maxWave, waveFor(repo.id, visiting));
    }
    std::vector<std::vector<std::string>> waves(static_cast<std::size_t>(maxWave + 1));
    for (const auto& repo : snapshot.repos) waves[static_cast<std::size_t>(memo[repo.id])].push_back(repo.id);
    for (auto& wave : waves) std::sort(wave.begin(), wave.end());
    return waves;
}

std::string PointerMessage(const RepoStatus& repo) {
    return std::max<std::size_t>(repo.childRepos.size(), 1) == 1 ? kPointerSingleMessage : kPointerMultipleMessage;
}

std::vector<std::string> ExtractUnregisteredGitlinkPaths(const RepoStatus& repo) {
    constexpr std::string_view kPrefix = "UnregisteredGitlinkSkipped:";
    std::vector<std::string> out;
    for (const auto& fact : repo.submoduleFacts) {
        if (fact.rfind(kPrefix, 0) == 0) {
            const auto path = Trim(fact.substr(kPrefix.size()));
            if (!path.empty()) {
                out.push_back(path);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Add(std::vector<PlanLine>& lines, const std::string& repo, const std::string& text) {
    if (std::none_of(lines.begin(), lines.end(), [&](const auto& line) { return line.repo == repo && line.text == text; })) lines.push_back({repo, text});
}

void AddPushAfterCommit(Plan& plan, const RepoStatus& repo, const std::string& reason) {
    if (!Allows(repo, "push")) {
        Add(plan.skipped, repo.id, "push skipped by commandPolicy.push=false");
        return;
    }
    if (repo.behind > 0) {
        if (!Allows(repo, "sync")) {
            Add(plan.blocked, repo.id, "BEHIND_UPSTREAM: sync required after " + reason + " before push but commandPolicy.sync=false");
            return;
        }
        Add(plan.sync, repo.id, "kog sync origin-latest after " + reason + " before push");
    }
    Add(plan.push, repo.id, "kog push --repos " + repo.id + " after " + reason);
}

bool HasUnpushedSubmoduleCommit(const RepoStatus& repo) {
    return Contains(repo.submoduleFacts, "SubmoduleCommitUnpushed") || Contains(repo.statusFlags, "UNPUSHED_SUBMODULE_COMMIT");
}

bool IsCleanNestedPreflightOnlyBlocker(const RepoStatus& repo) {
    if (repo.type == "root") return false;
    if (repo.managementPolicy == "discovered-untrusted") return false;
    if (repo.ahead > 0) return false;
    return repo.dirtyKind == "DETACHED_HEAD" || repo.dirtyKind == "MISSING_REMOTE";
}

bool CanPublishLocalMutation(const RepoStatus& repo) {
    return Allows(repo, "commit") && Allows(repo, "push") && (repo.behind == 0 || Allows(repo, "sync")) && !repo.remote.empty();
}

bool CanRepoConvergeBeforeParent(const RepoStatus& repo,
                                 const std::unordered_map<std::string, RepoStatus>& byId,
                                 std::unordered_set<std::string>& visiting);

bool ChildrenCanConvergeBeforeParent(const RepoStatus& repo,
                                     const std::unordered_map<std::string, RepoStatus>& byId,
                                     std::unordered_set<std::string>& visiting) {
    for (const auto& child : repo.childRepos) {
        const auto it = byId.find(child);
        if (it == byId.end()) {
            return false;
        }
        const auto& childRepo = it->second;
        const bool childNeedsAction =
            childRepo.dirtyKind != "CLEAN" ||
            HasUnpushedSubmoduleCommit(childRepo) ||
            Contains(childRepo.statusFlags, "CHILD_WORKTREE_DIRTY") ||
            Contains(childRepo.statusFlags, "CHILD_COMMIT_UNPUSHED");
        if (childNeedsAction && !CanRepoConvergeBeforeParent(childRepo, byId, visiting)) {
            return false;
        }
    }
    return true;
}

bool CanRepoConvergeBeforeParent(const RepoStatus& repo,
                                 const std::unordered_map<std::string, RepoStatus>& byId,
                                 std::unordered_set<std::string>& visiting) {
    if (!visiting.insert(repo.id).second) {
        return false;
    }

    const auto finish = [&](bool result) {
        visiting.erase(repo.id);
        return result;
    };

    if (repo.dirtyKind == "CONFLICTED" || repo.dirtyKind == "DIVERGED") {
        return finish(false);
    }
    if (repo.blocksConverge && !IsCleanNestedPreflightOnlyBlocker(repo)) {
        return finish(false);
    }
    if (repo.dirtyKind == "CLEAN") {
        return finish(true);
    }
    if (repo.dirtyKind == "DETACHED_HEAD" || repo.dirtyKind == "MISSING_REMOTE") {
        return finish(IsCleanNestedPreflightOnlyBlocker(repo));
    }
    if (repo.dirtyKind == "BEHIND_ONLY") {
        return finish(Allows(repo, "sync"));
    }
    if (repo.dirtyKind == "AHEAD_ONLY") {
        return finish(Allows(repo, "push") && !repo.remote.empty());
    }
    if (repo.dirtyKind == "GITLINK_DIRTY_ONLY") {
        return finish(CanPublishLocalMutation(repo));
    }
    if (repo.dirtyKind == "GITLINK_DIRTY_UNSAFE") {
        return finish(CanPublishLocalMutation(repo) && ChildrenCanConvergeBeforeParent(repo, byId, visiting));
    }
    if (repo.dirtyKind == "CONTENT_AND_GITLINK_DIRTY") {
        return finish(CanPublishLocalMutation(repo) && ChildrenCanConvergeBeforeParent(repo, byId, visiting));
    }
    if (repo.dirtyKind == "INDEX_DIRTY" || repo.dirtyKind == "CONTENT_DIRTY" || repo.dirtyKind == "UNTRACKED_ONLY") {
        return finish(CanPublishLocalMutation(repo));
    }
    return finish(false);
}

bool UnsafeParentCanWaitForChildConverge(const RepoStatus& repo,
                                         const std::unordered_map<std::string, RepoStatus>& byId) {
    bool foundRemediableChild = false;
    std::unordered_set<std::string> visiting{repo.id};
    for (const auto& child : repo.childRepos) {
        const auto it = byId.find(child);
        if (it == byId.end()) {
            return false;
        }
        const auto& childRepo = it->second;
        const bool childNeedsAction =
            childRepo.dirtyKind != "CLEAN" ||
            HasUnpushedSubmoduleCommit(childRepo) ||
            Contains(childRepo.statusFlags, "CHILD_WORKTREE_DIRTY") ||
            Contains(childRepo.statusFlags, "CHILD_COMMIT_UNPUSHED");
        if (!childNeedsAction) {
            continue;
        }
        if (!CanRepoConvergeBeforeParent(childRepo, byId, visiting)) {
            return false;
        }
        foundRemediableChild = true;
    }
    return foundRemediableChild;
}

Plan BuildPlan(const Snapshot& snapshot) {
    Plan plan;
    plan.waves = Waves(snapshot);
    std::unordered_map<std::string, RepoStatus> byId;
    for (const auto& repo : snapshot.repos) {
        byId.emplace(repo.id, repo);
        plan.dirtyCounts[repo.dirtyKind] += 1;
    }
    for (const auto& repo : snapshot.repos) {
        if (IsFalsePolicy(repo.commandPolicy, "sync") || IsFalsePolicy(repo.commandPolicy, "commit") || IsFalsePolicy(repo.commandPolicy, "push") || IsFalsePolicy(repo.commandPolicy, "hygiene")) {
            Add(plan.policy, repo.id, std::format("sync={} commit={} push={} hygiene={}", PolicyValue(repo, "sync"), PolicyValue(repo, "commit"), PolicyValue(repo, "push"), PolicyValue(repo, "hygiene")));
        }
        for (const auto& path : ExtractUnregisteredGitlinkPaths(repo)) {
            Add(plan.skippedUnregisteredGitlinks,
                path,
                "no .gitmodules mapping; not registered as managed submodule; skipped parent pointer update");
        }
        if (repo.dirtyKind == "CONFLICTED") { Add(plan.blocked, repo.id, "CONFLICTED: resolve conflicts before converge mutation"); continue; }
        if (repo.dirtyKind == "DIVERGED") { Add(plan.blocked, repo.id, "DIVERGED: no conflict-safe converge sync policy is configured"); continue; }
        if (IsCleanNestedPreflightOnlyBlocker(repo)) {
            Add(plan.skipped, repo.id, "preflight-only clean nested repo skipped: " + repo.dirtyKind);
            continue;
        }

        std::vector<std::string> unpushedChildren;
        for (const auto& child : repo.childRepos) if (const auto it = byId.find(child); it != byId.end() && HasUnpushedSubmoduleCommit(it->second)) unpushedChildren.push_back(child);
        if (!unpushedChildren.empty()) {
            std::sort(unpushedChildren.begin(), unpushedChildren.end());
            bool blockedByPolicy = false;
            for (const auto& child : unpushedChildren) {
                const auto& childRepo = byId.at(child);
                if (Allows(childRepo, "push")) {
                    Add(plan.push, child, "push child first before parent pointer commit/push");
                } else {
                    Add(plan.blocked, repo.id, kPushDisabledPointerBlockReason);
                    blockedByPolicy = true;
                }
            }
            if (!blockedByPolicy) {
                Add(plan.skipped, repo.id, "parent pointer commit waits for child push");
            }
            continue;
        }

        if (repo.blocksConverge) {
            Add(plan.blocked, repo.id, repo.blockReason.empty() ? "status snapshot blocks converge" : repo.blockReason);
            Add(plan.skipped, repo.id, "blocked by recursive status preflight");
            continue;
        }

        if (repo.dirtyKind == "DETACHED_HEAD" || repo.dirtyKind == "MISSING_REMOTE") {
            Add(plan.blocked, repo.id, repo.dirtyKind + ": repair branch/remote state before converge mutation");
            continue;
        }

        if (repo.dirtyKind == "BEHIND_ONLY") { Allows(repo, "sync") ? Add(plan.sync, repo.id, "kog sync origin-latest before commit/push decisions") : Add(plan.skipped, repo.id, "sync skipped by commandPolicy.sync=false"); Add(plan.skipped, repo.id, "commit skipped: BEHIND_ONLY"); continue; }
        if (repo.dirtyKind == "CLEAN") { Add(plan.skipped, repo.id, "commit skipped: CLEAN"); continue; }
        if (repo.dirtyKind == "AHEAD_ONLY") { Add(plan.skipped, repo.id, "commit skipped: AHEAD_ONLY"); Allows(repo, "push") ? Add(plan.push, repo.id, "kog push --repos " + repo.id) : Add(plan.skipped, repo.id, "push skipped by commandPolicy.push=false"); continue; }
        if (repo.dirtyKind == "GITLINK_DIRTY_ONLY") {
            Add(plan.skipped, repo.id, "kog commit -ai skipped: GITLINK_DIRTY_ONLY");
            if (Allows(repo, "commit")) {
                Add(plan.commit, repo.id, "deterministic pointer commit: " + PointerMessage(repo));
                AddPushAfterCommit(plan, repo, "pointer commit");
            } else {
                Add(plan.skipped, repo.id, "pointer commit skipped by commandPolicy.commit=false");
            }
            continue;
        }
        if (repo.dirtyKind == "UNREGISTERED_GITLINK_DIRTY_ONLY_SKIPPED") { Add(plan.skipped, repo.id, "kog commit -ai skipped: UNREGISTERED_GITLINK_DIRTY_ONLY_SKIPPED"); continue; }
        if (repo.dirtyKind == "GITLINK_DIRTY_UNSAFE") {
            if (UnsafeParentCanWaitForChildConverge(repo, byId)) {
                Add(plan.skipped, repo.id, "parent pointer commit waits for child worktree converge");
            } else {
                Add(plan.blocked, repo.id, "PARENT_POINTER_UNSAFE: child worktree/commit state is not safe for deterministic pointer-only commit");
            }
            continue;
        }
        if (repo.dirtyKind == "INDEX_DIRTY") {
            if (Allows(repo, "commit")) {
                Add(plan.commit, repo.id, "kog commit -ai --repos " + repo.id + " (staged/index changes included by commit policy)");
                AddPushAfterCommit(plan, repo, "commit");
            } else {
                Add(plan.skipped, repo.id, "commit skipped by commandPolicy.commit=false");
            }
            continue;
        }
        if (repo.dirtyKind == "CONTENT_DIRTY") {
            if (Allows(repo, "commit")) {
                Add(plan.commit, repo.id, "kog commit -ai --repos " + repo.id);
                AddPushAfterCommit(plan, repo, "commit");
            } else {
                Add(plan.skipped, repo.id, "commit skipped by commandPolicy.commit=false");
            }
            continue;
        }
        if (repo.dirtyKind == "CONTENT_AND_GITLINK_DIRTY") {
            if (Allows(repo, "commit")) {
                Add(plan.commit, repo.id, "kog commit -ai --repos " + repo.id + " (combined content/pointer context)");
                AddPushAfterCommit(plan, repo, "commit");
            } else {
                Add(plan.skipped, repo.id, "commit skipped by commandPolicy.commit=false");
            }
            continue;
        }
        if (repo.dirtyKind == "UNTRACKED_ONLY") {
            if (Allows(repo, "commit")) {
                Add(plan.commit, repo.id, "kog commit -ai --repos " + repo.id + " (untracked files included by git add -A policy)");
                AddPushAfterCommit(plan, repo, "commit");
            } else {
                Add(plan.blocked, repo.id, "DIRTY_WORKTREE: UNTRACKED_ONLY but commandPolicy.commit=false");
            }
            continue;
        }
        if (repo.ahead > 0) Allows(repo, "push") ? Add(plan.push, repo.id, "kog push --repos " + repo.id) : Add(plan.skipped, repo.id, "push skipped by commandPolicy.push=false");
        else Add(plan.skipped, repo.id, "no converge action for dirtyKind=" + repo.dirtyKind);
    }
    auto sortLines = [](std::vector<PlanLine>& lines) { std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.repo == b.repo ? a.text < b.text : a.repo < b.repo; }); };
    sortLines(plan.sync); sortLines(plan.commit); sortLines(plan.push); sortLines(plan.skippedUnregisteredGitlinks); sortLines(plan.skipped); sortLines(plan.policy); sortLines(plan.blocked);
    return plan;
}

std::vector<std::string> UniqueRepos(const std::vector<PlanLine>& lines) {
    std::vector<std::string> repos;
    repos.reserve(lines.size());
    for (const auto& line : lines) repos.push_back(line.repo);
    std::sort(repos.begin(), repos.end());
    repos.erase(std::unique(repos.begin(), repos.end()), repos.end());
    return repos;
}

bool RepoHasDescendantInSet(const Snapshot& snapshot,
                            const std::string& repoId,
                            const std::unordered_set<std::string>& candidates) {
    std::unordered_map<std::string, const RepoStatus*> byId;
    for (const auto& repo : snapshot.repos) {
        byId[repo.id] = &repo;
    }
    const auto root = byId.find(repoId);
    if (root == byId.end()) {
        return false;
    }

    std::vector<std::string> pending = root->second->childRepos;
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        auto child = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(child).second) {
            continue;
        }
        if (candidates.contains(child)) {
            return true;
        }
        if (const auto it = byId.find(child); it != byId.end()) {
            pending.insert(pending.end(), it->second->childRepos.begin(), it->second->childRepos.end());
        }
    }
    return false;
}

bool RepoHasPlannedDescendantPush(const Snapshot& snapshot, const Plan& plan, const std::string& repoId) {
    const auto plannedPushRepos = UniqueRepos(plan.push);
    return RepoHasDescendantInSet(
        snapshot,
        repoId,
        std::unordered_set<std::string>(plannedPushRepos.begin(), plannedPushRepos.end()));
}

std::size_t RepoWaveIndex(const Plan& plan, const std::string& repoId) {
    for (std::size_t index = 0; index < plan.waves.size(); ++index) {
        if (std::find(plan.waves[index].begin(), plan.waves[index].end(), repoId) != plan.waves[index].end()) {
            return index;
        }
    }
    return plan.waves.size();
}

bool IsNestedRepo(const Snapshot& snapshot, const std::string& repoId) {
    const auto it = std::find_if(snapshot.repos.begin(), snapshot.repos.end(), [&](const auto& repo) {
        return repo.id == repoId;
    });
    return it != snapshot.repos.end() && !it->parentRepos.empty();
}

bool PlanHasRunnableActions(const Plan& plan) {
    return !plan.sync.empty() || !plan.commit.empty() || !plan.push.empty();
}

std::optional<unsigned int> ParsePositiveEnvMs(const char* raw) {
    if (raw == nullptr || raw[0] == '\0') {
        return std::nullopt;
    }
    char* end = nullptr;
    const auto parsed = std::strtoul(raw, &end, 10);
    if (end == raw || parsed == 0 || parsed > static_cast<unsigned long>(std::numeric_limits<unsigned int>::max())) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(parsed);
}

std::optional<unsigned int> ResolveConvergeSyncTimeoutMs() {
    if (const auto explicitMs = ParsePositiveEnvMs(std::getenv("KOG_CONVERGE_SYNC_TIMEOUT_MS")); explicitMs.has_value()) {
        return explicitMs;
    }
    return static_cast<unsigned int>(120 * 1000);
}

unsigned int ResolveBranchStatusTimeoutMs() {
    if (const auto explicitMs = ParsePositiveEnvMs(std::getenv("KOG_BRANCH_STATUS_TIMEOUT_MS")); explicitMs.has_value()) {
        return *explicitMs;
    }
    return 90 * 1000;
}

unsigned int ResolveBranchPlanDeadlineMs() {
    if (const auto explicitMs = ParsePositiveEnvMs(std::getenv("KOG_BRANCH_PLAN_DEADLINE_MS")); explicitMs.has_value()) {
        return *explicitMs;
    }
    return 90 * 1000;
}

unsigned int ResolveBranchProbeTimeoutMs() {
    if (const auto explicitMs = ParsePositiveEnvMs(std::getenv("KOG_BRANCH_PROBE_TIMEOUT_MS")); explicitMs.has_value()) {
        return *explicitMs;
    }
    return 5 * 1000;
}

std::string SnapshotFingerprint(const Snapshot& snapshot) {
    std::vector<std::string> parts;
    for (const auto& repo : snapshot.repos) {
        std::vector<std::string> policyParts;
        for (const auto& [key, value] : repo.commandPolicy) policyParts.push_back(key + "=" + value);
        std::sort(policyParts.begin(), policyParts.end());
        parts.push_back(repo.id + "|" + repo.type + "|" + repo.managementPolicy + "|" + Csv(repo.childRepos) + "|" + Csv(policyParts));
    }
    std::sort(parts.begin(), parts.end());
    return std::to_string(std::hash<std::string>{}(Csv(parts)));
}

std::filesystem::path ResolveRepoPath(const std::filesystem::path& workspaceRoot, const std::string& repo) {
    return repo.empty() || repo == "."
        ? workspaceRoot.lexically_normal()
        : (workspaceRoot / std::filesystem::path(repo)).lexically_normal();
}

std::string NormalizeGitPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) {
        path = path.substr(2);
    }
    return path;
}

std::vector<std::string> SplitPathParts(const std::string& path) {
    std::vector<std::string> out;
    std::istringstream iss(path);
    std::string part;
    while (std::getline(iss, part, '/')) {
        if (!part.empty()) {
            out.push_back(part);
        }
    }
    return out;
}

std::string SanitizePlanComponent(std::string value) {
    value = NormalizeGitPath(std::move(value));
    if (value.empty() || value == ".") {
        value = "root";
    }
    for (auto& ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '-' && ch != '_') {
            ch = '-';
        }
    }
    return value;
}

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::optional<std::string> ExtractWorkItemId(const std::string& text) {
    static const std::regex kWorkItemPattern(
        R"(([A-Z][A-Z0-9]*-(BUG|TSK|FTR|USR|EPIC|ISS|ADR)-[0-9]+))",
        std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(text, match, kWorkItemPattern)) {
        return std::nullopt;
    }
    return UpperAscii(match.str(1));
}

std::string WorkItemKind(const std::string& itemId) {
    const auto first = itemId.find('-');
    if (first == std::string::npos) return "item";
    const auto second = itemId.find('-', first + 1);
    if (second == std::string::npos) return "item";
    const auto raw = ToLower(itemId.substr(first + 1, second - first - 1));
    if (raw == "bug") return "bug";
    if (raw == "tsk") return "task";
    if (raw == "ftr") return "feature";
    if (raw == "usr") return "story";
    if (raw == "epic") return "epic";
    if (raw == "iss") return "issue";
    if (raw == "adr") return "adr";
    return raw.empty() ? "item" : raw;
}

std::string BuildBacklogScope(const std::string& product) {
    return product.empty() ? "backlog" : ("backlog-" + product);
}

std::string KccSubject(const std::string& subsystem,
                       const std::string& type,
                       const std::string& summary,
                       const std::string& ticket = "NO-TICKET") {
    const auto normalizedTicket = Trim(ticket).empty() ? std::string("NO-TICKET") : ticket;
    return "[" + subsystem + "][" + type + "] " + summary + " (" + normalizedTicket + ")";
}

IntentCommitGroup MakeGroup(const std::string& key,
                            const std::string& message,
                            const std::string& reviewReason,
                            const std::string& path) {
    IntentCommitGroup group;
    group.key = key;
    group.message = message;
    group.reviewReason = reviewReason;
    group.includePaths.push_back(path);
    return group;
}

std::optional<IntentCommitGroup> ClassifyBacklogIntentPath(const std::string& path) {
    const auto parts = SplitPathParts(path);
    if (parts.empty()) {
        return std::nullopt;
    }

    if (parts[0] == "products" && parts.size() >= 3) {
        const auto& product = parts[1];
        const auto& section = parts[2];
        const auto scope = BuildBacklogScope(product);
        if (section == "items") {
            if (const auto itemId = ExtractWorkItemId(path); itemId.has_value()) {
                const auto kind = WorkItemKind(*itemId);
                return MakeGroup(
                    "backlog-item:" + product + ":" + *itemId,
                    KccSubject("Backlog", "Docs", "Update " + *itemId + " " + kind + " item", *itemId),
                    "native classifier matched backlog work item path " + *itemId,
                    path);
            }
        }
        if (section == "artifacts") {
            if (parts.size() >= 4 && parts[3] == "_receipts") {
                return MakeGroup(
                    "backlog-receipts:" + product,
                    KccSubject("Backlog", "Docs", "Add " + product + " mutation receipts"),
                    "native classifier matched backlog mutation receipt evidence for " + product,
                    path);
            }
            const auto itemId = parts.size() >= 4 ? ExtractWorkItemId(parts[3]) : ExtractWorkItemId(path);
            if (itemId.has_value()) {
                return MakeGroup(
                    "backlog-evidence:" + product + ":" + *itemId,
                    KccSubject("Backlog", "Docs", "Add " + *itemId + " evidence", *itemId),
                    "native classifier matched backlog evidence path " + *itemId,
                    path);
            }
            return MakeGroup(
                "backlog-artifacts:" + product,
                KccSubject("Backlog", "Docs", "Update " + product + " backlog artifacts"),
                "native classifier matched generic backlog artifact path for " + product,
                path);
        }
        if (section == "topics") {
            return MakeGroup(
                "backlog-topics:" + product,
                KccSubject("Backlog", "Docs", "Update " + product + " backlog topics"),
                "native classifier matched backlog topic graph path for " + product,
                path);
        }
        if (section == "views") {
            return MakeGroup(
                "backlog-views:" + product,
                KccSubject("Backlog", "Docs", "Refresh " + product + " backlog views"),
                "native classifier matched generated backlog views for " + product,
                path);
        }
        if (section == "config" || section == "_config" || section == "settings") {
            return MakeGroup(
                "backlog-config:" + product,
                KccSubject("Backlog", "Chore", "Update " + product + " backlog configuration"),
                "native classifier matched backlog configuration for " + product,
                path);
        }
    }

    if (parts[0] == "_shared" && parts.size() >= 3 && parts[1] == "artifacts") {
        if (const auto itemId = ExtractWorkItemId(parts[2]); itemId.has_value()) {
            return MakeGroup(
                "backlog-evidence:shared:" + *itemId,
                KccSubject("Backlog", "Docs", "Add " + *itemId + " shared evidence", *itemId),
                "native classifier matched shared backlog evidence path " + *itemId,
                path);
        }
    }

    if (path == ".kano/backlog_config.toml" || path == "backlog_config.toml") {
        return MakeGroup(
            "backlog-config:root",
            KccSubject("Backlog", "Chore", "Update backlog configuration"),
            "native classifier matched backlog root configuration",
            path);
    }

    return std::nullopt;
}

std::optional<IntentCommitGroup> ClassifyKogSourceIntentPath(const std::string& path) {
    const auto lowered = ToLower(path);
    if (lowered.find("converge") != std::string::npos ||
        lowered.find("repo_sync/private/converge_cmd.cpp") != std::string::npos) {
        return MakeGroup(
            "kog-converge",
            KccSubject("KOG-Converge", "BugFix", "Update intent-scoped agent commits"),
            "native classifier matched converge implementation or regression tests",
            path);
    }

    if (lowered.find("commit_plan") != std::string::npos) {
        return MakeGroup(
            "kog-commit-plan",
            KccSubject("KOG-CommitPlan", "BugFix", "Update commit planning workflow"),
            "native classifier matched KOG commit-plan source path",
            path);
    }

    if (lowered.rfind("src/cpp/code/systems/", 0) == 0) {
        const auto parts = SplitPathParts(path);
        const std::string system = parts.size() >= 5 ? parts[4] : "source";
        return MakeGroup(
            "kog-system:" + system,
            KccSubject(system, "Chore", "Update source intent"),
            "native classifier matched KOG source system " + system,
            path);
    }

    if (lowered.rfind("src/", 0) == 0) {
        return MakeGroup(
            "kog-source",
            KccSubject("KOG", "Chore", "Update source intent"),
            "native classifier matched KOG source path",
            path);
    }

    if (lowered.rfind("scripts/", 0) == 0) {
        return MakeGroup(
            "kog-scripts",
            KccSubject("KOG-Scripts", "Chore", "Update automation scripts"),
            "native classifier matched script path",
            path);
    }

    if (lowered.rfind("bin/", 0) == 0 ||
        lowered.rfind("vars/", 0) == 0 ||
        lowered.ends_with(".groovy") ||
        lowered.ends_with(".ps1") ||
        lowered.ends_with(".py") ||
        lowered.ends_with(".sh")) {
        return MakeGroup(
            "automation-scripts",
            KccSubject("Automation", "Chore", "Update automation scripts"),
            "native classifier matched generic automation or pipeline script path",
            path);
    }

    if (lowered.rfind("config/", 0) == 0 ||
        lowered.rfind(".github/", 0) == 0 ||
        lowered.starts_with("docker-compose") ||
        lowered.ends_with(".toml") ||
        lowered.ends_with(".toml.example") ||
        lowered.ends_with(".json") ||
        lowered.ends_with(".yml") ||
        lowered.ends_with(".yaml")) {
        return MakeGroup(
            "kog-config",
            KccSubject("KOG", "Chore", "Update configuration"),
            "native classifier matched repository configuration path",
            path);
    }

    if (lowered.rfind("templates/", 0) == 0) {
        return MakeGroup(
            "kog-templates",
            KccSubject("KOG", "Docs", "Update workflow templates"),
            "native classifier matched workflow template path",
            path);
    }

    if (lowered.rfind("docs/", 0) == 0 || lowered.ends_with(".md")) {
        return MakeGroup(
            "kog-docs",
            KccSubject("KOG", "Docs", "Update documentation"),
            "native classifier matched documentation path",
            path);
    }

    if (lowered == "cmakelists.txt" || lowered.ends_with("/cmakelists.txt") ||
        lowered.ends_with(".cmake") || lowered.rfind("cmake/", 0) == 0) {
        return MakeGroup(
            "kog-build",
            KccSubject("KOG", "Build", "Update build configuration"),
            "native classifier matched build configuration path",
            path);
    }

    if (lowered == ".gitignore" || lowered.ends_with("/.gitignore") ||
        lowered == ".gitmodules" || lowered.ends_with("/.gitmodules") ||
        lowered.ends_with(".gitattributes")) {
        return MakeGroup(
            "kog-repo-policy",
            KccSubject("KOG", "Chore", "Update repository policy"),
            "native classifier matched repository policy path",
            path);
    }

    return std::nullopt;
}

std::optional<IntentCommitGroup> ClassifyIntentPath(std::string path) {
    path = NormalizeGitPath(std::move(path));
    if (path.empty()) {
        return std::nullopt;
    }
    if (const auto backlog = ClassifyBacklogIntentPath(path); backlog.has_value()) {
        return backlog;
    }
    return ClassifyKogSourceIntentPath(path);
}

std::string TicketCommitType(const std::string& itemId) {
    const auto kind = WorkItemKind(itemId);
    if (kind == "bug" || kind == "issue") return "BugFix";
    if (kind == "feature" || kind == "story" || kind == "epic") return "Feature";
    if (kind == "adr") return "Docs";
    return "Chore";
}

std::string DirtyStatusKind(std::string_view rawStatus) {
    if (rawStatus == "??") return "untracked";
    if (rawStatus == "!!") return "ignored";
    if (rawStatus.find('R') != std::string_view::npos) return "renamed";
    if (rawStatus.find('C') != std::string_view::npos) return "copied";
    if (rawStatus.find('D') != std::string_view::npos) return "deleted";
    if (rawStatus.find('A') != std::string_view::npos) return "added";
    if (rawStatus.find('M') != std::string_view::npos) return "modified";
    if (rawStatus.find('U') != std::string_view::npos) return "unmerged";
    return "changed";
}

bool HasStagedIndexChange(std::string_view rawStatus) {
    return !rawStatus.empty() && rawStatus.front() != ' ' && rawStatus.front() != '?' && rawStatus.front() != '!';
}

std::optional<std::string> ExtractDirtyEntryWorkItemId(const DirtyPathEntry& entry) {
    if (const auto itemId = ExtractWorkItemId(entry.path); itemId.has_value()) {
        return itemId;
    }
    if (!entry.originalPath.empty()) {
        return ExtractWorkItemId(entry.originalPath);
    }
    return std::nullopt;
}

std::optional<std::string> UniqueWorkItemIdForDirtyEntries(const std::vector<DirtyPathEntry>& entries) {
    std::set<std::string> itemIds;
    for (const auto& entry : entries) {
        if (const auto itemId = ExtractDirtyEntryWorkItemId(entry); itemId.has_value()) {
            itemIds.insert(*itemId);
        }
    }
    if (itemIds.size() != 1) {
        return std::nullopt;
    }
    return *itemIds.begin();
}

bool IsKogSourceIntentPath(const std::string& path) {
    const auto normalized = NormalizeGitPath(path);
    if (normalized.empty()) {
        return false;
    }
    const auto lowered = ToLower(normalized);
    if (lowered.rfind("products/", 0) == 0 || lowered.rfind("_shared/", 0) == 0) {
        return false;
    }
    return ClassifyKogSourceIntentPath(normalized).has_value();
}

bool IsTicketContextPromotableEntry(const DirtyPathEntry& entry, const std::string& itemId) {
    if (const auto pathItemId = ExtractWorkItemId(entry.path); pathItemId.has_value() && *pathItemId != itemId) {
        return false;
    }
    if (!entry.originalPath.empty()) {
        if (const auto originalItemId = ExtractWorkItemId(entry.originalPath); originalItemId.has_value() && *originalItemId != itemId) {
            return false;
        }
    }
    return IsKogSourceIntentPath(entry.path) || (!entry.originalPath.empty() && IsKogSourceIntentPath(entry.originalPath));
}

std::string TicketContextSubsystemForDirtyEntries(const std::vector<DirtyPathEntry>& entries, const std::string& itemId) {
    for (const auto& entry : entries) {
        if (!IsTicketContextPromotableEntry(entry, itemId)) {
            continue;
        }
        const auto path = ToLower(entry.path + "\n" + entry.originalPath);
        if (path.find("converge") != std::string::npos ||
            path.find("repo_sync/private/converge_cmd.cpp") != std::string::npos) {
            return "KOG-Converge";
        }
    }
    for (const auto& entry : entries) {
        if (!IsTicketContextPromotableEntry(entry, itemId)) {
            continue;
        }
        const auto path = ToLower(entry.path + "\n" + entry.originalPath);
        if (path.find("commit_plan") != std::string::npos) {
            return "KOG-CommitPlan";
        }
    }
    return "KOG";
}

IntentCommitGroup MakeTicketContextGroup(const std::string& itemId, const std::string& subsystem) {
    IntentCommitGroup group;
    group.key = "kog-ticket:" + itemId;
    group.message = KccSubject(subsystem, TicketCommitType(itemId), "Update " + itemId + " implementation intent", itemId);
    group.reviewReason = "native classifier matched status-aware dirty entries for unique work item " + itemId;
    return group;
}

std::optional<IntentCommitGroup> ClassifyDirtyEntryIntent(const DirtyPathEntry& entry) {
    if (auto classified = ClassifyIntentPath(entry.path); classified.has_value()) {
        return classified;
    }
    if (!entry.originalPath.empty()) {
        return ClassifyIntentPath(entry.originalPath);
    }
    return std::nullopt;
}

void AppendDirtyEntryIncludePaths(std::vector<std::string>& paths, const DirtyPathEntry& entry) {
    if (!entry.originalPath.empty()) {
        paths.push_back(entry.originalPath);
    }
    if (!entry.path.empty()) {
        paths.push_back(entry.path);
    }
}

std::optional<std::string> LocalToolArtifactIgnoreRuleForPath(const std::string& path) {
    auto normalized = NormalizeGitPath(path);
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }

    const auto lowered = ToLower(normalized);
    if (lowered == "microsoft/windows/powershell/moduleanalysiscache") {
        return "/Microsoft/Windows/PowerShell/ModuleAnalysisCache";
    }
    if (normalized == "$log") {
        return "/$log";
    }
    if (lowered == "nul_diff.txt") {
        return "/NUL_diff.txt";
    }

    constexpr std::string_view topicPrefix = "topics/";
    constexpr std::string_view activeMarkerSuffix = "/.active";
    if (lowered.rfind(topicPrefix, 0) == 0 && lowered.ends_with(activeMarkerSuffix)) {
        const auto topicName = lowered.substr(
            topicPrefix.size(),
            lowered.size() - topicPrefix.size() - activeMarkerSuffix.size());
        if (!topicName.empty() && topicName.find('/') == std::string::npos) {
            return "/topics/*/.active";
        }
    }
    return std::nullopt;
}

bool IsIgnoredByGit(const std::filesystem::path& repoPath, const std::string& path) {
    const auto result = shell::ExecuteCommand(
        "git",
        {"check-ignore", "--quiet", "--", path},
        shell::ExecMode::Capture,
        repoPath);
    return result.exitCode == 0;
}

std::vector<std::string> EnsureLocalToolArtifactIgnoreRules(const std::filesystem::path& repoPath,
                                                           const std::vector<std::string>& dirtyPaths,
                                                           std::vector<std::string>* localArtifactPaths,
                                                           std::string* outError) {
    std::vector<std::string> rules;
    for (const auto& path : dirtyPaths) {
        const auto rule = LocalToolArtifactIgnoreRuleForPath(path);
        if (!rule.has_value()) {
            continue;
        }
        if (localArtifactPaths != nullptr) {
            localArtifactPaths->push_back(path);
        }
        if (!IsIgnoredByGit(repoPath, path)) {
            rules.push_back(*rule);
        }
    }

    if (localArtifactPaths != nullptr) {
        std::sort(localArtifactPaths->begin(), localArtifactPaths->end());
        localArtifactPaths->erase(std::unique(localArtifactPaths->begin(), localArtifactPaths->end()), localArtifactPaths->end());
    }

    std::sort(rules.begin(), rules.end());
    rules.erase(std::unique(rules.begin(), rules.end()), rules.end());
    if (rules.empty()) {
        return {};
    }

    const auto ignorePath = repoPath / ".gitignore";
    std::string content;
    if (std::filesystem::exists(ignorePath)) {
        std::ifstream in(ignorePath, std::ios::binary);
        if (!in.good()) {
            if (outError != nullptr) {
                *outError = "failed to read .gitignore for local artifact rules: " + ignorePath.generic_string();
            }
            return {};
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        content = buffer.str();
    }

    std::set<std::string> existingLines;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        existingLines.insert(Trim(line));
    }

    std::vector<std::string> missingRules;
    for (const auto& rule : rules) {
        if (!existingLines.contains(rule)) {
            missingRules.push_back(rule);
        }
    }
    if (missingRules.empty()) {
        return {};
    }

    std::ofstream out(ignorePath, std::ios::binary | std::ios::app);
    if (!out.good()) {
        if (outError != nullptr) {
            *outError = "failed to append local artifact ignore rules: " + ignorePath.generic_string();
        }
        return {};
    }
    if (!content.empty() && content.back() != '\n' && content.back() != '\r') {
        out << "\n";
    }
    if (!content.empty()) {
        out << "\n";
    }
    out << "# Local tool artifacts\n";
    for (const auto& rule : missingRules) {
        out << rule << "\n";
    }
    out.close();
    if (!out.good()) {
        if (outError != nullptr) {
            *outError = "failed to flush local artifact ignore rules: " + ignorePath.generic_string();
        }
        return {};
    }
    return missingRules;
}

std::string RepoRelativeChildPath(const std::string& parentRepo, const std::string& childRepo) {
    const auto parent = NormalizeGitPath(parentRepo);
    auto child = NormalizeGitPath(childRepo);
    if (parent.empty() || parent == ".") {
        return child;
    }
    const auto prefix = parent + "/";
    if (child.rfind(prefix, 0) == 0) {
        child = child.substr(prefix.size());
    }
    return child;
}

bool IsRegisteredChildRepoStatusPath(const Snapshot& snapshot, const std::string& repo, const std::string& path) {
    const auto normalizedPath = NormalizeGitPath(path);
    const auto normalizedRepo = NormalizeGitPath(repo.empty() ? "." : repo);
    for (const auto& candidate : snapshot.repos) {
        if (NormalizeGitPath(candidate.id) != normalizedRepo) {
            continue;
        }
        for (const auto& child : candidate.childRepos) {
            if (RepoRelativeChildPath(candidate.id, child) == normalizedPath) {
                return true;
            }
        }
        return false;
    }
    return false;
}

std::vector<DirtyPathEntry> CollectDirtyEntries(const std::filesystem::path& repoPath, std::string* outError) {
    const auto result = shell::ExecuteCommand(
        "git",
        {"status", "--porcelain=v1", "--untracked-files=all"},
        shell::ExecMode::Capture,
        repoPath);
    if (result.exitCode != 0) {
        if (outError != nullptr) {
            *outError = "git status --porcelain failed: " + Trim(result.stderrStr);
        }
        return {};
    }

    std::vector<DirtyPathEntry> entries;
    std::istringstream stream(result.stdoutStr);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 4) {
            continue;
        }
        DirtyPathEntry entry;
        entry.rawStatus = line.substr(0, 2);
        auto path = NormalizeGitPath(Trim(line.substr(3)));
        const auto arrow = path.find(" -> ");
        if (arrow != std::string::npos) {
            entry.originalPath = NormalizeGitPath(Trim(path.substr(0, arrow)));
            path = NormalizeGitPath(Trim(path.substr(arrow + 4)));
        }
        if (!path.empty()) {
            entry.path = std::move(path);
            entry.statusKind = DirtyStatusKind(entry.rawStatus);
            entries.push_back(std::move(entry));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return std::tie(a.path, a.originalPath, a.rawStatus) < std::tie(b.path, b.originalPath, b.rawStatus);
    });
    entries.erase(std::unique(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.path == b.path && a.originalPath == b.originalPath && a.rawStatus == b.rawStatus;
    }), entries.end());
    return entries;
}

std::string ComparableAbsolutePath(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = std::filesystem::absolute(path, ec);
        if (ec) {
            normalized = path;
        }
    }
    auto value = normalized.lexically_normal().generic_string();
#if defined(_WIN32)
    value = ToLower(std::move(value));
#endif
    return value;
}

std::string TrimTrailingDirectorySeparators(std::string path) {
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::optional<std::filesystem::path> CommonGitDirFromRegisteredRepo(const std::filesystem::path& repoPath) {
    const auto dotGit = repoPath / ".git";
    std::error_code ec;
    if (std::filesystem::is_directory(dotGit, ec) && !ec) {
        return dotGit.lexically_normal();
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(dotGit, ec) || ec) {
        return std::nullopt;
    }

    std::ifstream in(dotGit, std::ios::binary);
    std::string line;
    if (!in.good() || !std::getline(in, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    constexpr std::string_view prefix = "gitdir:";
    if (line.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    auto gitDir = std::filesystem::path(Trim(line.substr(prefix.size())));
    if (gitDir.is_relative()) {
        gitDir = repoPath / gitDir;
    }
    gitDir = gitDir.lexically_normal();
    if (ToLower(gitDir.parent_path().filename().generic_string()) == "worktrees") {
        return gitDir.parent_path().parent_path().lexically_normal();
    }
    return gitDir;
}

bool IsRegisteredLinkedWorktree(const std::filesystem::path& candidate,
                                const std::set<std::string>& registeredCommonGitDirs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec) || ec ||
        !std::filesystem::is_regular_file(candidate / ".git", ec) || ec) {
        return false;
    }

    const auto commonDirResult = shell::ExecuteCommand(
        "git",
        {"rev-parse", "--path-format=absolute", "--git-common-dir"},
        shell::ExecMode::Capture,
        candidate);
    if (commonDirResult.exitCode != 0 ||
        !registeredCommonGitDirs.contains(ComparableAbsolutePath(FirstNonEmptyLine(commonDirResult.stdoutStr)))) {
        return false;
    }

    const auto listed = shell::ExecuteCommand(
        "git",
        {"worktree", "list", "--porcelain"},
        shell::ExecMode::Capture,
        candidate);
    if (listed.exitCode != 0) {
        return false;
    }

    const auto candidateKey = ComparableAbsolutePath(candidate);
    bool candidateListed = false;
    std::istringstream lines(listed.stdoutStr);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        constexpr std::string_view prefix = "worktree ";
        if (line.rfind(prefix, 0) != 0) {
            continue;
        }
        const auto listedKey = ComparableAbsolutePath(Trim(line.substr(prefix.size())));
        candidateListed = candidateListed || listedKey == candidateKey;
    }
    return candidateListed;
}

bool AppendRegisteredWorktreeExcludeRules(const RepoStatus& repo,
                                          const std::vector<std::string>& rules) {
    if (rules.empty()) {
        return false;
    }

    const auto repoPath = std::filesystem::path(repo.absolutePath).lexically_normal();
    const auto gitPathResult = shell::ExecuteCommand(
        "git",
        {"rev-parse", "--git-path", "info/exclude"},
        shell::ExecMode::Capture,
        repoPath);
    if (gitPathResult.exitCode != 0) {
        throw std::runtime_error(
            "failed to resolve local exclude path for " + repo.id + ": " + Trim(gitPathResult.stderrStr));
    }

    auto excludePath = std::filesystem::path(FirstNonEmptyLine(gitPathResult.stdoutStr));
    if (excludePath.is_relative()) {
        excludePath = repoPath / excludePath;
    }
    excludePath = excludePath.lexically_normal();

    std::string content;
    if (std::filesystem::exists(excludePath)) {
        std::ifstream in(excludePath, std::ios::binary);
        if (!in.good()) {
            throw std::runtime_error("failed to read local exclude file: " + excludePath.generic_string());
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        content = buffer.str();
    }

    std::set<std::string> existingLines;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        existingLines.insert(Trim(line));
    }

    std::vector<std::string> missingRules;
    for (const auto& rule : rules) {
        if (!existingLines.contains(rule)) {
            missingRules.push_back(rule);
        }
    }
    if (missingRules.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(excludePath.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("failed to create local exclude directory: " + ec.message());
    }
    std::ofstream out(excludePath, std::ios::binary | std::ios::app);
    if (!out.good()) {
        throw std::runtime_error("failed to append local exclude file: " + excludePath.generic_string());
    }
    if (!content.empty() && content.back() != '\n' && content.back() != '\r') {
        out << "\n";
    }
    constexpr std::string_view marker = "# KOG registered linked worktrees (KG-BUG-0037)";
    if (!existingLines.contains(std::string(marker))) {
        out << marker << "\n";
    }
    for (const auto& rule : missingRules) {
        out << rule << "\n";
        std::cout << "[converge] local_exclude_repo=" << repo.id
                  << " registered_worktree=" << rule << "\n";
    }
    out.close();
    if (!out.good()) {
        throw std::runtime_error("failed to flush local exclude file: " + excludePath.generic_string());
    }
    return true;
}

bool EnsureRegisteredLinkedWorktreeExcludes(const Snapshot& snapshot) {
    std::set<std::string> registeredRepoPaths;
    std::set<std::string> registeredCommonGitDirs;
    for (const auto& repo : snapshot.repos) {
        if (repo.absolutePath.empty()) {
            continue;
        }
        const auto repoPath = std::filesystem::path(repo.absolutePath).lexically_normal();
        registeredRepoPaths.insert(ComparableAbsolutePath(repoPath));
        if (const auto commonDir = CommonGitDirFromRegisteredRepo(repoPath); commonDir.has_value()) {
            registeredCommonGitDirs.insert(ComparableAbsolutePath(*commonDir));
        }
    }

    bool changed = false;
    for (const auto& repo : snapshot.repos) {
        if (repo.absolutePath.empty() || !Contains(repo.statusFlags, "??")) {
            continue;
        }

        const auto repoPath = std::filesystem::path(repo.absolutePath).lexically_normal();
        std::string statusError;
        const auto dirtyEntries = CollectDirtyEntries(repoPath, &statusError);
        if (!statusError.empty()) {
            throw std::runtime_error("failed to inspect linked worktree candidates for " + repo.id + ": " + statusError);
        }

        std::vector<std::string> rules;
        for (const auto& entry : dirtyEntries) {
            if (entry.rawStatus != "??") {
                continue;
            }
            const auto relative = TrimTrailingDirectorySeparators(NormalizeGitPath(entry.path));
            if (relative.empty()) {
                continue;
            }
            const auto candidate = (repoPath / std::filesystem::path(relative)).lexically_normal();
            if (registeredRepoPaths.contains(ComparableAbsolutePath(candidate))) {
                continue;
            }
            if (IsRegisteredLinkedWorktree(candidate, registeredCommonGitDirs)) {
                rules.push_back("/" + relative + "/");
            }
        }

        std::sort(rules.begin(), rules.end());
        rules.erase(std::unique(rules.begin(), rules.end()), rules.end());
        changed = AppendRegisteredWorktreeExcludeRules(repo, rules) || changed;
    }
    return changed;
}

std::vector<std::string> DirtyEntryPaths(const std::vector<DirtyPathEntry>& entries) {
    std::vector<std::string> paths;
    for (const auto& entry : entries) {
        if (!entry.path.empty()) {
            paths.push_back(entry.path);
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

std::string Fnv1a64HexLocal(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char ch : value) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::nouppercase << hash;
    return out.str();
}

bool IsInternalConvergeArtifactPath(const std::string& path) {
    const auto normalized = NormalizeGitPath(path);
    return normalized.rfind(".kano/tmp/", 0) == 0 ||
           normalized.rfind("src/cpp/.kano/tmp/", 0) == 0 ||
           normalized.find("/.kano/tmp/") != std::string::npos;
}

std::string ComputeScopedBaseHeadShaForPlan(const Snapshot& snapshot, const std::string& repo) {
    std::vector<std::string> lines;
    const auto normalizedRepo = NormalizeGitPath(repo.empty() ? "." : repo);
    for (const auto& candidate : snapshot.repos) {
        if (NormalizeGitPath(candidate.id) != normalizedRepo) {
            continue;
        }
        const auto sha = candidate.head.empty()
            ? std::string("0000000000000000000000000000000000000000")
            : candidate.head;
        lines.push_back(normalizedRepo + "\t" + sha);
        break;
    }
    if (lines.empty()) {
        lines.push_back(normalizedRepo + "\t0000000000000000000000000000000000000000");
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "scope-head-v1-" + Fnv1a64HexLocal(canonical.str());
}

std::string ComputeScopedDirtyFingerprintForPlan(const Snapshot& snapshot,
                                                 const std::string& repo,
                                                 const std::vector<DirtyPathEntry>& dirtyEntries) {
    std::vector<std::string> lines;
    const auto normalizedRepo = NormalizeGitPath(repo.empty() ? "." : repo);
    const RepoStatus* repoStatus = nullptr;
    for (const auto& candidate : snapshot.repos) {
        if (NormalizeGitPath(candidate.id) == normalizedRepo) {
            repoStatus = &candidate;
            break;
        }
    }

    lines.push_back("repo=" + normalizedRepo);
    if (repoStatus != nullptr) {
        lines.push_back("head=" + repoStatus->head);
    }

    for (const auto& entry : dirtyEntries) {
        if (entry.path.empty() || IsInternalConvergeArtifactPath(entry.path)) {
            continue;
        }
        lines.push_back(entry.rawStatus + "|" + entry.originalPath + "|" + entry.path + "|" + entry.statusKind);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "scope-dirty-v1-" + Fnv1a64HexLocal(canonical.str());
}

IntentCommitPlan BuildIntentCommitPlan(const std::filesystem::path& workspaceRoot,
                                       const Snapshot& snapshot,
                                       const std::string& repo) {
    IntentCommitPlan plan;
    const auto repoPath = ResolveRepoPath(workspaceRoot, repo);
    std::string statusError;
    auto dirtyEntries = CollectDirtyEntries(repoPath, &statusError);
    if (!statusError.empty()) {
        plan.error = statusError;
        return plan;
    }
    auto dirtyPaths = DirtyEntryPaths(dirtyEntries);
    if (dirtyPaths.empty()) {
        plan.error = "no dirty paths found for intent commit planning";
        return plan;
    }

    plan.addedIgnoreRules = EnsureLocalToolArtifactIgnoreRules(repoPath, dirtyPaths, &plan.localArtifactPaths, &statusError);
    if (!statusError.empty()) {
        plan.error = statusError;
        return plan;
    }
    if (!plan.addedIgnoreRules.empty()) {
        dirtyEntries = CollectDirtyEntries(repoPath, &statusError);
        if (!statusError.empty()) {
            plan.error = statusError;
            return plan;
        }
        dirtyPaths = DirtyEntryPaths(dirtyEntries);
        if (dirtyPaths.empty()) {
            plan.error = "no dirty paths found after local artifact ignore update";
            return plan;
        }
    }

    std::map<std::string, IntentCommitGroup> grouped;
    std::vector<DirtyPathEntry> stagedEntries;
    std::vector<std::string> registeredChildPaths;
    const auto uniqueWorkItemId = UniqueWorkItemIdForDirtyEntries(dirtyEntries);
    const auto ticketContextSubsystem = uniqueWorkItemId.has_value()
        ? std::optional<std::string>(TicketContextSubsystemForDirtyEntries(dirtyEntries, *uniqueWorkItemId))
        : std::nullopt;
    for (const auto& entry : dirtyEntries) {
        const auto& path = entry.path;
        if (IsRegisteredChildRepoStatusPath(snapshot, repo, path)) {
            AppendDirtyEntryIncludePaths(registeredChildPaths, entry);
            if (HasStagedIndexChange(entry.rawStatus)) {
                stagedEntries.push_back(entry);
            }
            continue;
        }
        std::optional<IntentCommitGroup> classified;
        if (uniqueWorkItemId.has_value() &&
            ticketContextSubsystem.has_value() &&
            IsTicketContextPromotableEntry(entry, *uniqueWorkItemId)) {
            classified = MakeTicketContextGroup(*uniqueWorkItemId, *ticketContextSubsystem);
        } else {
            classified = ClassifyDirtyEntryIntent(entry);
        }
        if (!classified.has_value()) {
            AppendDirtyEntryIncludePaths(plan.ambiguousPaths, entry);
            continue;
        }
        if (HasStagedIndexChange(entry.rawStatus)) {
            stagedEntries.push_back(entry);
            continue;
        }
        auto& group = grouped[classified->key];
        if (group.key.empty()) {
            group = std::move(*classified);
            group.includePaths.clear();
            AppendDirtyEntryIncludePaths(group.includePaths, entry);
        } else {
            AppendDirtyEntryIncludePaths(group.includePaths, entry);
        }
    }

    if (!stagedEntries.empty()) {
        IntentCommitGroup stagedGroup;
        if (const auto stagedWorkItemId = UniqueWorkItemIdForDirtyEntries(stagedEntries); stagedWorkItemId.has_value()) {
            stagedGroup = MakeTicketContextGroup(
                *stagedWorkItemId,
                TicketContextSubsystemForDirtyEntries(stagedEntries, *stagedWorkItemId));
        } else {
            stagedGroup.message = KccSubject(
                "Converge", "Chore", "Commit pre-staged intent changes", "NO-TICKET");
            stagedGroup.reviewReason = "converge coalesced the complete classified pre-staged set before remaining intent groups";
        }
        stagedGroup.key = "0000-pre-staged";
        for (const auto& entry : stagedEntries) {
            AppendDirtyEntryIncludePaths(stagedGroup.includePaths, entry);
        }
        grouped[stagedGroup.key] = std::move(stagedGroup);
    }

    if (grouped.empty() && plan.ambiguousPaths.empty() && !registeredChildPaths.empty()) {
        std::sort(registeredChildPaths.begin(), registeredChildPaths.end());
        registeredChildPaths.erase(std::unique(registeredChildPaths.begin(), registeredChildPaths.end()), registeredChildPaths.end());
        IntentCommitGroup group;
        group.key = "submodule-pointers";
        group.message = kPointerMultipleMessage;
        group.reviewReason = "native classifier matched registered child repo pointer paths";
        group.includePaths = registeredChildPaths;
        grouped[group.key] = std::move(group);
    }

    for (auto& [key, group] : grouped) {
        std::sort(group.includePaths.begin(), group.includePaths.end());
        group.includePaths.erase(std::unique(group.includePaths.begin(), group.includePaths.end()), group.includePaths.end());
        plan.groups.push_back(std::move(group));
    }
    std::sort(plan.groups.begin(), plan.groups.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    std::sort(plan.ambiguousPaths.begin(), plan.ambiguousPaths.end());
    plan.ambiguousPaths.erase(std::unique(plan.ambiguousPaths.begin(), plan.ambiguousPaths.end()), plan.ambiguousPaths.end());

    if (plan.groups.empty()) {
        plan.error = "no intent-scoped groups could be inferred";
        return plan;
    }

    const auto safeRepo = SanitizePlanComponent(repo);
    const auto baseHeadSha = ComputeScopedBaseHeadShaForPlan(snapshot, repo);
    const auto fingerprint = ComputeScopedDirtyFingerprintForPlan(snapshot, repo, dirtyEntries);
    plan.planPath = (workspaceRoot / ".kano" / "tmp" / "workflows" / "converge" /
                     "intent-commit-plans" / (safeRepo + "-" + fingerprint + ".json")).lexically_normal();

    nlohmann::json json;
    json["meta"] = {
        {"schema_version", "1"},
        {"plan_id", "converge-intent-" + safeRepo + "-" + fingerprint},
        {"generated_at_utc", UtcNowIso8601()},
        {"base_head_sha", baseHeadSha},
        {"dirty_fingerprint_pre_ignore", fingerprint},
        {"dirty_fingerprint", fingerprint},
        {"freshness_scope", "repo"},
        {"scope_repo", repo.empty() ? "." : repo},
        {"planner", {
            {"provider", "native"},
            {"ai-model", "converge-intent-classifier-v1"},
            {"request_id", "native-" + fingerprint}
        }},
        {"review", {
            {"verdict", "pass"},
            {"reason", "native converge classifier produced deterministic intent-scoped commit groups"}
        }}
    };
    nlohmann::json repoEntry;
    repoEntry["repo"] = repo.empty() ? "." : repo;
    repoEntry["commits"] = nlohmann::json::array();
    for (const auto& group : plan.groups) {
        repoEntry["commits"].push_back({
            {"message", group.message},
            {"include", group.includePaths},
            {"exclude", nlohmann::json::array()},
            {"review", {
                {"verdict", "pass"},
                {"reason", group.reviewReason}
            }}
        });
    }
    json["stages"] = {
        {"commit", nlohmann::json::array({repoEntry})},
        {"post_sync", nlohmann::json::array()}
    };

    std::error_code ec;
    std::filesystem::create_directories(plan.planPath.parent_path(), ec);
    if (ec) {
        plan.error = "failed to create plan directory: " + ec.message();
        return plan;
    }
    std::ofstream out(plan.planPath, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        plan.error = "failed to write intent commit plan: " + plan.planPath.generic_string();
        return plan;
    }
    out << json.dump(2) << "\n";
    out.close();
    if (!out.good()) {
        plan.error = "failed to flush intent commit plan: " + plan.planPath.generic_string();
    }
    return plan;
}

void PrintIntentCommitPlan(const std::string& repo, const IntentCommitPlan& plan) {
    std::cout << "Converge agent intent commit plan\n";
    std::cout << "  repo=" << (repo.empty() ? "." : repo) << "\n";
    if (!plan.planPath.empty()) {
        std::cout << "  plan_file=" << plan.planPath.generic_string() << "\n";
    }
    std::cout << "  review=pass\n";
    for (const auto& group : plan.groups) {
        std::cout << "  - " << group.message << "\n";
        for (const auto& path : group.includePaths) {
            std::cout << "      include " << path << "\n";
        }
    }
    if (!plan.ambiguousPaths.empty()) {
        std::cout << "  skipped_ambiguous=" << plan.ambiguousPaths.size() << "\n";
        for (const auto& path : plan.ambiguousPaths) {
            std::cout << "      ambiguous " << path << "\n";
        }
    }
    if (!plan.localArtifactPaths.empty()) {
        std::cout << "  local_artifacts=" << plan.localArtifactPaths.size() << "\n";
        for (const auto& path : plan.localArtifactPaths) {
            std::cout << "      local " << path << "\n";
        }
    }
    if (!plan.addedIgnoreRules.empty()) {
        std::cout << "  added_ignore_rules=" << plan.addedIgnoreRules.size() << "\n";
        for (const auto& rule : plan.addedIgnoreRules) {
            std::cout << "      ignore " << rule << "\n";
        }
    }
}

int RunIntentCommitPlan(const std::filesystem::path& workspaceRoot,
                        const Snapshot& snapshot,
                        const std::string& repo,
                        bool profile,
                        std::string* outCommandLine,
                        std::string* outFailureCategory,
                        std::string* outFailureMessage) {
    auto plan = BuildIntentCommitPlan(workspaceRoot, snapshot, repo);
    PrintIntentCommitPlan(repo, plan);
    if (!plan.error.empty()) {
        if (outFailureCategory != nullptr) *outFailureCategory = "INTENT_PLAN_FAILED";
        if (outFailureMessage != nullptr) *outFailureMessage = plan.error;
        std::cerr << "Error: failed to build converge agent intent commit plan: " << plan.error << "\n";
        return 4;
    }
    if (!plan.ambiguousPaths.empty()) {
        if (outFailureCategory != nullptr) *outFailureCategory = "AMBIGUOUS_INTENT_SCOPE";
        if (outFailureMessage != nullptr) *outFailureMessage = "agent intent commit plan has unclassified paths";
        std::cerr << "Error: converge agent intent commit plan has ambiguous paths; leaving repo untouched.\n";
        return 4;
    }
    if (outCommandLine != nullptr) {
        *outCommandLine = "kog commit --plan-file " + plan.planPath.generic_string() + " --plan-stage commit";
    }
    return RunCommitNativePlanStage(workspaceRoot, plan.planPath.generic_string(), "commit", profile, true);
}

std::vector<std::string> RepoBaselines(const Snapshot& snapshot) {
    std::vector<std::string> out;
    out.reserve(snapshot.repos.size());
    for (const auto& repo : snapshot.repos) {
        out.push_back(
            repo.id +
            " branch=" + repo.branch +
            " head=" + repo.head +
            " remote=" + repo.remote +
            " upstream=" + repo.upstream +
            " dirtyKind=" + repo.dirtyKind +
            " ahead=" + std::to_string(repo.ahead));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::map<std::string, std::string> BaselineByRepo(const std::vector<std::string>& baselines) {
    std::map<std::string, std::string> out;
    for (const auto& line : baselines) {
        const auto split = line.find(' ');
        const auto id = split == std::string::npos ? line : line.substr(0, split);
        if (!id.empty()) {
            out[id] = line;
        }
    }
    return out;
}

bool PlanReferencesRepo(const Plan& plan, const std::string& repo) {
    const auto contains = [&](const std::vector<PlanLine>& lines) {
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
            return line.repo == repo;
        });
    };
    const auto containsDeferredParentPointer = std::any_of(
        plan.skipped.begin(),
        plan.skipped.end(),
        [&](const auto& line) {
            return line.repo == repo && line.text.find("parent pointer commit waits") != std::string::npos;
        });
    return contains(plan.sync) || contains(plan.commit) || contains(plan.push) || containsDeferredParentPointer;
}

void ResetStateForPlan(WorkflowState& state, const Snapshot& snapshot, const Plan& plan) {
    state.currentPhase = kConvergePhases.front();
    state.completedPhases.clear();
    state.blockedReason.clear();
    state.blockedRepos.clear();
    state.phaseResults.clear();
    state.commandLinesUsed.clear();
    state.plannedSyncRepos = UniqueRepos(plan.sync);
    state.plannedCommitRepos = UniqueRepos(plan.commit);
    state.plannedPushRepos = UniqueRepos(plan.push);
    state.plannedBlockedRepos = UniqueRepos(plan.blocked);
    state.plannedWaves = plan.waves;
    state.repoGraphFingerprint = SnapshotFingerprint(snapshot);
    state.repoBaselines = RepoBaselines(snapshot);
    state.repoManagementPolicy.clear();
    state.repoType.clear();
    state.repoCommandPolicy.clear();
    for (const auto& repo : snapshot.repos) {
        state.repoManagementPolicy[repo.id] = repo.managementPolicy;
        state.repoType[repo.id] = repo.type;
        state.repoCommandPolicy[repo.id] = repo.commandPolicy;
    }
    state.detectedConflictInfo.clear();
    state.succeededRepos.clear();
    state.failedRepos.clear();
    state.blockedRepoSet.clear();
    state.skippedRepos.clear();
    state.pendingRepos.clear();
    for (const auto& repo : snapshot.repos) state.pendingRepos.push_back(repo.id);
    std::sort(state.pendingRepos.begin(), state.pendingRepos.end());
}

void MergeUnique(std::vector<std::string>& target, const std::vector<std::string>& values) {
    target.insert(target.end(), values.begin(), values.end());
    std::sort(target.begin(), target.end());
    target.erase(std::unique(target.begin(), target.end()), target.end());
}

void RemoveRepos(std::vector<std::string>& target, const std::vector<std::string>& repos) {
    if (repos.empty() || target.empty()) {
        return;
    }
    for (const auto& repo : repos) {
        target.erase(std::remove(target.begin(), target.end(), repo), target.end());
    }
}

void RecordPhaseState(WorkflowState& state, const std::string& phase, const PhaseSummary& summary) {
    state.phaseResults[phase] = summary;
    if (std::none_of(state.completedPhases.begin(), state.completedPhases.end(), [&](const auto& p) { return p == phase; })) {
        state.completedPhases.push_back(phase);
    }
    MergeUnique(state.succeededRepos, summary.succeeded);
    MergeUnique(state.failedRepos, summary.failed);
    MergeUnique(state.blockedRepoSet, summary.blocked);
    MergeUnique(state.skippedRepos, summary.skipped);
    MergeUnique(state.pendingRepos, summary.pending);
}

bool PhaseAlreadyCompleted(const WorkflowState& state, const std::string& phase) {
    return std::find(state.completedPhases.begin(), state.completedPhases.end(), phase) != state.completedPhases.end();
}

bool PhaseSummaryContainsRepo(const WorkflowState& state,
                              const std::string& phase,
                              const std::string& repo,
                              const std::vector<std::string> PhaseSummary::* field) {
    const auto it = state.phaseResults.find(phase);
    if (it == state.phaseResults.end()) {
        return false;
    }
    const auto& values = it->second.*field;
    return std::find(values.begin(), values.end(), repo) != values.end();
}

const RepoStatus* FindSnapshotRepo(const Snapshot& snapshot, const std::string& repoId) {
    const auto it = std::find_if(snapshot.repos.begin(), snapshot.repos.end(), [&](const auto& repo) {
        return repo.id == repoId;
    });
    return it == snapshot.repos.end() ? nullptr : &*it;
}

bool RestoreSavedResumeTransportPlan(const WorkflowState& state,
                                     const Snapshot& snapshot,
                                     Plan& plan) {
    bool restoredSync = false;
    for (const auto& repoId : state.plannedSyncRepos) {
        const auto* repo = FindSnapshotRepo(snapshot, repoId);
        if (repo == nullptr || repo->behind <= 0 || !Allows(*repo, "sync")) {
            continue;
        }
        if (repo->dirtyKind != "BEHIND_ONLY" && repo->dirtyKind != "DIVERGED") {
            continue;
        }
        Add(plan.sync, repoId, "resume saved post-commit sync intent before push");
        restoredSync = true;
    }
    for (const auto& repoId : state.plannedPushRepos) {
        const auto* repo = FindSnapshotRepo(snapshot, repoId);
        if (repo == nullptr || repo->ahead <= 0 || !Allows(*repo, "push")) {
            continue;
        }
        Add(plan.push, repoId, "resume saved push intent after post-commit sync");
    }
    return restoredSync;
}

std::set<std::string> ToSet(const std::vector<std::string>& values) {
    return std::set<std::string>(values.begin(), values.end());
}

std::string MapSyncFailureCategoryToReason(const std::string& category) {
    if (category == "SYNC_CONFLICT") return "CONFLICT_DETECTED";
    if (category == "FAILED_MISSING_REMOTE") return "REMOTE_MISSING";
    if (category == "FAILED_AUTH") return "AUTH_REQUIRED";
    if (category == "FAILED_CONNECTION") return "NETWORK_ERROR";
    if (category.rfind("SKIPPED_", 0) == 0) return "SKIPPED_BY_POLICY";
    return "SYNC_ERROR";
}

std::string MapPushFailureCategoryToReason(const std::string& category) {
    if (category == "FAILED_PUSH_NONFASTFORWARD") return "NON_FAST_FORWARD";
    if (category == "FAILED_AUTH") return "AUTH_REQUIRED";
    if (category == "FAILED_CONNECTION") return "NETWORK_ERROR";
    if (category == "FAILED_MISSING_REMOTE") return "REMOTE_MISSING";
    if (category.rfind("SKIPPED_", 0) == 0) return "SKIPPED_BY_POLICY";
    return "PUSH_ERROR";
}

void PopulatePhaseSummaryFromAggregate(const workspace::RepoOperationAggregate& aggregate,
                                       bool isPushPhase,
                                       PhaseSummary& summary) {
    for (const auto& result : aggregate.results) {
        const auto repo = result.repoId.empty() ? result.repoPath.generic_string() : result.repoId;
        switch (result.status) {
            case workspace::RepoOperationStatus::Succeeded:
                summary.succeeded.push_back(repo);
                summary.retryEligible[repo] = false;
                break;
            case workspace::RepoOperationStatus::Skipped:
                summary.skipped.push_back(repo);
                summary.failureCategory[repo] = "SKIPPED_BY_POLICY";
                summary.policySkipReason[repo] = result.skipReason;
                summary.failureMessage[repo] = result.message.empty() ? result.skipReason : result.message;
                summary.retryEligible[repo] = result.resumeRetryEligible;
                break;
            case workspace::RepoOperationStatus::Blocked:
                summary.blocked.push_back(repo);
                summary.failureCategory[repo] = "BLOCKED_BY_CHILD_FAILURE";
                summary.failureMessage[repo] = result.blockReason;
                summary.blockedBy[repo] = result.blockedBy;
                summary.retryEligible[repo] = result.resumeRetryEligible;
                break;
            case workspace::RepoOperationStatus::Failed:
                summary.failed.push_back(repo);
                summary.failureCategory[repo] = isPushPhase
                    ? MapPushFailureCategoryToReason(result.failureCategory)
                    : MapSyncFailureCategoryToReason(result.failureCategory);
                summary.failureMessage[repo] = result.message;
                summary.retryEligible[repo] = result.resumeRetryEligible;
                break;
            case workspace::RepoOperationStatus::Pending:
                summary.pending.push_back(repo);
                summary.failureCategory[repo] = "PENDING";
                summary.failureMessage[repo] = "scheduler did not execute repository";
                summary.retryEligible[repo] = true;
                break;
        }
    }
}

void PopulatePhaseSummaryFromSingleRepoAggregate(const std::string& repo,
                                                const workspace::RepoOperationAggregate& aggregate,
                                                bool isPushPhase,
                                                int exitCode,
                                                PhaseSummary& summary) {
    if (aggregate.results.empty()) {
        if (exitCode == 0) {
            summary.succeeded.push_back(repo);
            summary.retryEligible[repo] = false;
        } else {
            summary.failed.push_back(repo);
            summary.failureCategory[repo] = isPushPhase ? "PUSH_ERROR" : "SYNC_ERROR";
            summary.failureMessage[repo] = isPushPhase ? "push failed" : "sync failed";
            summary.retryEligible[repo] = true;
        }
        return;
    }

    for (const auto& result : aggregate.results) {
        switch (result.status) {
            case workspace::RepoOperationStatus::Succeeded:
                summary.succeeded.push_back(repo);
                summary.retryEligible[repo] = false;
                break;
            case workspace::RepoOperationStatus::Skipped:
                summary.skipped.push_back(repo);
                summary.failureCategory[repo] = "SKIPPED_BY_POLICY";
                summary.policySkipReason[repo] = result.skipReason;
                summary.failureMessage[repo] = result.message.empty() ? result.skipReason : result.message;
                summary.retryEligible[repo] = result.resumeRetryEligible;
                break;
            case workspace::RepoOperationStatus::Blocked:
                summary.blocked.push_back(repo);
                summary.failureCategory[repo] = "BLOCKED_BY_CHILD_FAILURE";
                summary.failureMessage[repo] = result.blockReason;
                summary.blockedBy[repo] = result.blockedBy;
                summary.retryEligible[repo] = result.resumeRetryEligible;
                break;
            case workspace::RepoOperationStatus::Failed:
                summary.failed.push_back(repo);
                summary.failureCategory[repo] = isPushPhase
                    ? MapPushFailureCategoryToReason(result.failureCategory)
                    : MapSyncFailureCategoryToReason(result.failureCategory);
                summary.failureMessage[repo] = result.message;
                summary.retryEligible[repo] = result.resumeRetryEligible;
                break;
            case workspace::RepoOperationStatus::Pending:
                summary.pending.push_back(repo);
                summary.failureCategory[repo] = "PENDING";
                summary.failureMessage[repo] = "scheduler did not execute repository";
                summary.retryEligible[repo] = true;
                break;
        }
    }
}

void PrintStatusPhaseSummary(const WorkflowState& state) {
    for (const auto& [phase, summary] : state.phaseResults) {
        std::cout << "phase=" << phase
                  << " succeeded=" << summary.succeeded.size()
                  << " failed=" << summary.failed.size()
                  << " blocked=" << summary.blocked.size()
                  << " skipped=" << summary.skipped.size()
                  << " pending=" << summary.pending.size() << "\n";
        for (const auto& [repo, category] : summary.failureCategory) {
            const auto messageIt = summary.failureMessage.find(repo);
            const auto blockedByIt = summary.blockedBy.find(repo);
            std::cout << "  repo=" << repo
                      << " failureCategory=" << category
                      << " failureMessage=" << (messageIt == summary.failureMessage.end() ? std::string{} : messageIt->second)
                      << " blockedBy=" << (blockedByIt == summary.blockedBy.end() ? std::string{} : blockedByIt->second)
                      << "\n";
        }
    }
}

void PrintLines(const std::string& title, const std::vector<PlanLine>& lines) {
    std::cout << title << "\n";
    if (lines.empty()) { std::cout << "  - (none)\n"; return; }
    for (const auto& line : lines) std::cout << "  - " << line.repo << ": " << line.text << "\n";
}

void PrintPlan(const Snapshot& snapshot, const Plan& plan, bool unregisteredScan) {
    std::size_t dirty = 0;
    for (const auto& repo : snapshot.repos) dirty += repo.dirtyKind == "CLEAN" ? 0 : 1;
    std::cout << "Converge Plan\n";
    std::cout << "Status preflight counts\n";
    std::cout << "  repos=" << snapshot.repos.size() << " dirty=" << dirty << " blocked=" << plan.blocked.size() << " unregisteredScan=" << (unregisteredScan ? "bounded" : "disabled") << "\n";
    std::cout << "  dirtyKindCounts";
    for (const auto& [kind, count] : plan.dirtyCounts) std::cout << " " << kind << "=" << count;
    std::cout << "\nRuntime phases\n";
    for (std::size_t i = 0; i < kConvergePhases.size(); ++i) std::cout << "  " << (i + 1) << ". " << kConvergePhases[i] << "\n";
    std::cout << "  behavior=dependency-aware best-effort (non-fail-fast)\n";
    std::cout << "\nDependency waves\n";
    for (std::size_t i = 0; i < plan.waves.size(); ++i) std::cout << "  - wave " << i << ": " << (plan.waves[i].empty() ? "(none)" : Csv(plan.waves[i])) << "\n";
    PrintLines("Command-policy decisions", plan.policy);
    PrintLines("Phase sync actions", plan.sync);
    PrintLines("Phase commit actions", plan.commit);
    PrintLines("Phase push actions", plan.push);
    PrintLines("Skipped unregistered gitlinks", plan.skippedUnregisteredGitlinks);
    PrintLines("Skipped repos", plan.skipped);
    PrintLines("Blocked repos", plan.blocked);
}

int RunDryRunPlanner(const std::filesystem::path& root, int jobs, bool unregisteredScan, bool recursive) {
    try {
        const auto snapshot = LoadConvergeSnapshot(root, jobs, unregisteredScan, recursive);
        const auto plan = BuildPlan(snapshot);
        PrintPlan(snapshot, plan, unregisteredScan);
        return plan.blocked.empty() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}

struct BranchPlanTimeoutDiagnostic {
    std::string code;
    std::string operation;
    std::string message;
    unsigned int timeoutMs = 0;
};

struct BranchPlanExecutionContext {
    std::chrono::steady_clock::time_point deadline;
    unsigned int deadlineMs = 0;
    unsigned int probeTimeoutMs = 0;
    bool allowPatchEquivalentProof = true;
    bool repoTimedOut = false;
    std::vector<BranchPlanTimeoutDiagnostic> repoDiagnostics;
};

thread_local BranchPlanExecutionContext* g_branchPlanExecutionContext = nullptr;

class ScopedBranchPlanExecutionContext {
  public:
    explicit ScopedBranchPlanExecutionContext(BranchPlanExecutionContext& context)
        : previous_(g_branchPlanExecutionContext) {
        g_branchPlanExecutionContext = &context;
    }

    ~ScopedBranchPlanExecutionContext() {
        g_branchPlanExecutionContext = previous_;
    }

    ScopedBranchPlanExecutionContext(const ScopedBranchPlanExecutionContext&) = delete;
    ScopedBranchPlanExecutionContext& operator=(const ScopedBranchPlanExecutionContext&) = delete;

  private:
    BranchPlanExecutionContext* previous_ = nullptr;
};

bool GitCaptureTimedOut(const shell::ExecResult& result) {
    return result.exitCode == 124 || result.stderrStr.find("[kog-timeout]") != std::string::npos;
}

shell::ExecResult BranchPlanDeadlineResult() {
    return {
        .exitCode = 124,
        .stderrStr = "[kog-timeout] source=branch_plan_deadline command_family=git safe_next_action=inspect planningDiagnostics",
    };
}

unsigned int RemainingBranchPlanMs(const BranchPlanExecutionContext& context) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        context.deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
        return 0;
    }
    return static_cast<unsigned int>(std::min<long long>(
        remaining, static_cast<long long>(std::numeric_limits<unsigned int>::max())));
}

shell::ExecResult GitCapture(const std::filesystem::path& repo, const std::vector<std::string>& args) {
    auto* context = g_branchPlanExecutionContext;
    if (context == nullptr) {
        return shell::ExecuteCommand("git", args, shell::ExecMode::Capture, repo);
    }
    if (context->repoTimedOut) {
        return BranchPlanDeadlineResult();
    }

    const auto remainingMs = RemainingBranchPlanMs(*context);
    if (remainingMs == 0) {
        context->repoTimedOut = true;
        context->repoDiagnostics.push_back({
            .code = "BRANCH_PLAN_DEADLINE",
            .operation = args.empty() ? std::string{"git"} : "git " + args.front(),
            .message = "branch planning deadline exhausted before the probe started",
            .timeoutMs = context->deadlineMs,
        });
        return BranchPlanDeadlineResult();
    }

    const auto timeoutMs = std::min(context->probeTimeoutMs, remainingMs);
    auto result = shell::ExecuteCommand(
        "git", args, shell::ExecMode::Capture, repo, shell::ProgressCallback{}, timeoutMs);
    if (GitCaptureTimedOut(result)) {
        context->repoTimedOut = true;
        const bool deadlineReached = RemainingBranchPlanMs(*context) == 0;
        auto message = Trim(result.stderrStr);
        if (message.empty()) {
            message = Trim(result.stdoutStr);
        }
        context->repoDiagnostics.push_back({
            .code = deadlineReached ? "BRANCH_PLAN_DEADLINE" : "BRANCH_PROBE_TIMEOUT",
            .operation = args.empty() ? std::string{"git"} : "git " + args.front(),
            .message = std::move(message),
            .timeoutMs = timeoutMs,
        });
    }
    return result;
}

shell::ExecResult GitCaptureForCleanup(const std::filesystem::path& repo, const std::vector<std::string>& args) {
    return shell::ExecuteCommand(
        "git", args, shell::ExecMode::Capture, repo, shell::ProgressCallback{}, ResolveBranchProbeTimeoutMs());
}

std::filesystem::path RepoPathForBranchPlan(const std::filesystem::path& workspaceRoot, const RepoStatus& repo) {
    if (!repo.absolutePath.empty()) {
        return std::filesystem::path(repo.absolutePath).lexically_normal();
    }
    return ResolveRepoPath(workspaceRoot, repo.id);
}

bool PathIsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto normalizedChild = child.lexically_normal();
    const auto normalizedParent = parent.lexically_normal();
    auto childIt = normalizedChild.begin();
    auto parentIt = normalizedParent.begin();
    for (; parentIt != normalizedParent.end(); ++parentIt, ++childIt) {
        if (childIt == normalizedChild.end() || *childIt != *parentIt) {
            return false;
        }
    }
    return true;
}

std::string DisplayPathForBranchPlan(const std::filesystem::path& workspaceRoot, const std::filesystem::path& path) {
    const auto normalized = path.lexically_normal();
    if (PathIsWithin(normalized, workspaceRoot)) {
        const auto relative = normalized.lexically_relative(workspaceRoot.lexically_normal()).generic_string();
        return relative.empty() || relative == "." ? std::string{"."} : relative;
    }
    return std::string{"<external-worktree>/"} + normalized.filename().generic_string();
}

std::vector<std::string> BranchPlanTraversalOrder(const Snapshot& snapshot) {
    std::vector<std::string> order;
    for (const auto& wave : Waves(snapshot)) {
        order.insert(order.end(), wave.begin(), wave.end());
    }
    return order;
}

const RepoStatus* FindRepo(const Snapshot& snapshot, const std::string& id) {
    const auto it = std::find_if(snapshot.repos.begin(), snapshot.repos.end(), [&](const auto& repo) { return repo.id == id; });
    return it == snapshot.repos.end() ? nullptr : &*it;
}

std::vector<BranchCandidate> BranchCandidates(const std::filesystem::path& repoPath, const std::string& preferredRemote) {
    const auto remote = preferredRemote.empty() ? std::string{"origin"} : preferredRemote;
    const auto result = GitCapture(
        repoPath,
        {"for-each-ref", "--format=%(refname)", "refs/heads", "refs/remotes/" + remote});
    if (result.exitCode != 0) {
        return {};
    }

    std::vector<std::string> locals;
    std::vector<std::string> remoteRefs;
    constexpr std::string_view localPrefix = "refs/heads/";
    const auto remoteRefPrefix = "refs/remotes/" + remote + "/";
    const auto remoteShortPrefix = remote + "/";
    std::istringstream lines(result.stdoutStr);
    std::string line;
    while (std::getline(lines, line)) {
        auto ref = Trim(line);
        if (ref.empty()) {
            continue;
        }
        if (ref.rfind(remoteRefPrefix, 0) == 0) {
            const auto remoteBranch = ref.substr(remoteRefPrefix.size());
            if (!remoteBranch.empty() && remoteBranch != "HEAD") {
                remoteRefs.push_back(remoteShortPrefix + remoteBranch);
            }
        } else if (ref.rfind(localPrefix, 0) == 0) {
            locals.push_back(ref.substr(localPrefix.size()));
        }
    }

    std::unordered_set<std::string> localSet(locals.begin(), locals.end());
    std::vector<BranchCandidate> candidates;
    candidates.reserve(locals.size());
    for (const auto& branch : locals) {
        BranchCandidate candidate;
        candidate.name = branch;
        candidate.ref = branch;
        candidates.push_back(candidate);
    }

    for (const auto& ref : remoteRefs) {
        const auto remoteBranch = ref.substr(remoteShortPrefix.size());
        if (remoteBranch.empty() || localSet.find(remoteBranch) != localSet.end()) {
            continue;
        }
        BranchCandidate candidate;
        candidate.name = ref;
        candidate.ref = ref;
        candidate.remoteOnly = true;
        candidate.remote = remote;
        candidate.remoteBranch = remoteBranch;
        candidates.push_back(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.name == b.name;
    }), candidates.end());
    return candidates;
}

std::vector<BranchWorktree> Worktrees(const std::filesystem::path& repoPath, const std::filesystem::path& workspaceRoot) {
    const auto result = GitCapture(repoPath, {"worktree", "list", "--porcelain"});
    std::vector<BranchWorktree> out;
    if (result.exitCode != 0) {
        return out;
    }

    BranchWorktree current;
    bool hasRecord = false;
    auto flush = [&]() {
        if (hasRecord) {
            out.push_back(current);
        }
        current = BranchWorktree{};
        hasRecord = false;
    };

    std::istringstream lines(result.stdoutStr);
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty()) {
            flush();
            continue;
        }
        if (line.rfind("worktree ", 0) == 0) {
            flush();
            hasRecord = true;
            current.absolutePath = std::filesystem::path(line.substr(std::string("worktree ").size())).lexically_normal();
            current.location = DisplayPathForBranchPlan(workspaceRoot, current.absolutePath);
        } else if (line.rfind("HEAD ", 0) == 0) {
            current.head = line.substr(std::string("HEAD ").size());
        } else if (line.rfind("branch ", 0) == 0) {
            constexpr std::string_view prefix = "refs/heads/";
            auto branch = line.substr(std::string("branch ").size());
            if (branch.rfind(prefix, 0) == 0) {
                branch = branch.substr(prefix.size());
            }
            current.branch = branch;
        } else if (line == "detached") {
            current.detached = true;
        } else if (line == "bare") {
            current.bare = true;
        }
    }
    flush();

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.branch == b.branch ? a.location < b.location : a.branch < b.branch;
    });
    return out;
}

std::vector<std::string> WorktreeLocationsForBranch(const std::vector<BranchWorktree>& worktrees, const std::string& branch) {
    std::vector<std::string> out;
    for (const auto& worktree : worktrees) {
        if (worktree.branch == branch) {
            out.push_back(worktree.location);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<BranchWorktree> WorktreesForBranch(const std::vector<BranchWorktree>& worktrees, const std::string& branch) {
    std::vector<BranchWorktree> out;
    for (const auto& worktree : worktrees) {
        if (worktree.branch == branch) {
            out.push_back(worktree);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.location < b.location;
    });
    return out;
}

std::string PathKey(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = path.lexically_normal();
    }
    auto value = normalized.generic_string();
#if defined(_WIN32)
    value = ToLower(value);
#endif
    return value;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return PathKey(left) == PathKey(right);
}

std::string CombinedGitError(const shell::ExecResult& result) {
    auto message = Trim(result.stderrStr);
    if (message.empty()) {
        message = Trim(result.stdoutStr);
    }
    return message;
}

std::string UpstreamForBranch(const std::filesystem::path& repoPath, const std::string& branch) {
    const auto upstream = GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", branch + "@{upstream}"});
    if (upstream.exitCode != 0) {
        return {};
    }
    return Trim(upstream.stdoutStr);
}

bool SplitRemoteTrackingRef(const std::string& upstream, std::string& remote, std::string& remoteBranch) {
    const auto split = upstream.find('/');
    if (split == std::string::npos || split == 0 || split + 1 >= upstream.size()) {
        return false;
    }
    remote = upstream.substr(0, split);
    remoteBranch = upstream.substr(split + 1);
    return true;
}

std::vector<std::string> BranchBlockers(const nlohmann::json& branch) {
    std::vector<std::string> blockers;
    if (const auto it = branch.find("blockers"); it != branch.end() && it->is_array()) {
        for (const auto& blocker : *it) {
            if (blocker.is_string()) {
                blockers.push_back(blocker.get<std::string>());
            }
        }
    }
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    return blockers;
}

bool IsBranchIntegrationStrategy(const std::string& strategy) {
    return strategy == "rebase" || strategy == "merge" || strategy == "cherry-pick";
}

bool DirtyKindBlocksBranchPlan(const std::string& dirtyKind) {
    return dirtyKind != "CLEAN" && dirtyKind != "AHEAD_ONLY" && dirtyKind != "BEHIND_ONLY";
}

bool IsPublishBranchCandidate(const BranchCandidate& candidate) {
    const auto branchName = candidate.remoteOnly && !candidate.remoteBranch.empty()
        ? candidate.remoteBranch
        : candidate.name;
    return branchName == "gh-pages";
}

std::vector<std::string> SplitNonEmptyLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

void AppendBranchBlocked(nlohmann::json& result,
                         const std::string& repoId,
                         const std::string& branch,
                         std::vector<std::string> blockers,
                         const std::string& message) {
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    result["blocked"].push_back({
        {"repo", repoId},
        {"branch", branch},
        {"blockers", blockers},
        {"message", message},
    });
}

void AppendBranchAction(nlohmann::json& result,
                        const std::string& field,
                        const std::string& repoId,
                        const std::string& branch,
                        const std::string& action,
                        const std::string& message) {
    result[field].push_back({
        {"repo", repoId},
        {"branch", branch},
        {"action", action},
        {"message", message},
    });
}

std::string DetachedWorktreeBranchLabel(const BranchWorktree& worktree) {
    if (worktree.head.empty()) {
        return "<detached>";
    }
    return "<detached:" + worktree.head.substr(0, std::min<std::size_t>(12, worktree.head.size())) + ">";
}

std::string WorktreeBranchLabel(const BranchWorktree& worktree) {
    return worktree.detached ? DetachedWorktreeBranchLabel(worktree) : worktree.branch;
}

std::string BranchWorktreeHarvestCommand(const std::string& targetBranch, const std::string& branch) {
    return "kog converge branches retire --target " + targetBranch +
           " --branch " + branch +
           " --remove-worktrees --harvest-branch-worktrees --confirm";
}

void AppendDetachedWorktreeBlocked(nlohmann::json& result,
                                   const std::string& repoId,
                                   const BranchWorktree& worktree,
                                   std::vector<std::string> blockers,
                                   const std::string& message,
                                   const std::string& integrationProof = {}) {
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    result["blocked"].push_back({
        {"repo", repoId},
        {"branch", DetachedWorktreeBranchLabel(worktree)},
        {"action", "remove-detached-worktree"},
        {"worktree", worktree.location},
        {"head", worktree.head},
        {"integrationProof", integrationProof},
        {"blockers", blockers},
        {"message", message},
    });
}

void AppendDetachedWorktreePreserved(nlohmann::json& result,
                                     const std::string& repoId,
                                     const BranchWorktree& worktree,
                                     std::vector<std::string> blockers,
                                     const std::string& message,
                                     const std::string& integrationProof = {}) {
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    result["preserved"].push_back({
        {"repo", repoId},
        {"branch", DetachedWorktreeBranchLabel(worktree)},
        {"action", "preserve-detached-worktree"},
        {"worktree", worktree.location},
        {"head", worktree.head},
        {"integrationProof", integrationProof},
        {"blockers", blockers},
        {"message", message},
    });
}

void AppendDetachedWorktreeAction(nlohmann::json& result,
                                  const std::string& field,
                                  const std::string& repoId,
                                  const BranchWorktree& worktree,
                                  const std::string& action,
                                  const std::string& message,
                                  const std::string& integrationProof = {}) {
    result[field].push_back({
        {"repo", repoId},
        {"branch", DetachedWorktreeBranchLabel(worktree)},
        {"action", action},
        {"worktree", worktree.location},
        {"head", worktree.head},
        {"integrationProof", integrationProof},
        {"message", message},
    });
}

bool BranchMatchesFilter(const nlohmann::json& branchJson, const std::string& branchFilter) {
    if (branchFilter.empty()) {
        return true;
    }
    return branchFilter == branchJson.value("name", std::string{}) ||
           branchFilter == branchJson.value("ref", std::string{}) ||
           branchFilter == branchJson.value("remoteBranch", std::string{});
}

bool StatusIsUntrackedOnly(const std::string& porcelainStatus) {
    const auto lines = SplitNonEmptyLines(porcelainStatus);
    return !lines.empty() && std::all_of(lines.begin(), lines.end(), [](const std::string& line) {
        return line.rfind("?? ", 0) == 0;
    });
}

bool WorktreeIsClean(const std::filesystem::path& worktreePath, std::string& message, bool allowUntrackedOnly = false) {
    const auto status = GitCapture(worktreePath, {"status", "--porcelain"});
    if (status.exitCode != 0) {
        message = "git status failed: " + CombinedGitError(status);
        return false;
    }
    if (!Trim(status.stdoutStr).empty() && !(allowUntrackedOnly && StatusIsUntrackedOnly(status.stdoutStr))) {
        message = "worktree has uncommitted or untracked changes";
        return false;
    }
    return true;
}

std::vector<std::string> UntrackedDirtyPaths(const std::vector<DirtyPathEntry>& entries) {
    std::vector<std::string> paths;
    for (const auto& entry : entries) {
        if (entry.rawStatus == "??" && !entry.path.empty()) {
            paths.push_back(entry.path);
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

bool MarkIntentToAddUntracked(const std::filesystem::path& worktreePath,
                              const std::vector<std::string>& paths,
                              std::string& errorMessage) {
    if (paths.empty()) {
        return true;
    }
    std::vector<std::string> args{"add", "-N", "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    const auto add = GitCapture(worktreePath, args);
    if (add.exitCode != 0) {
        errorMessage = CombinedGitError(add);
        return false;
    }
    return true;
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::string& content, std::string& errorMessage) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        errorMessage = "failed to open patch file: " + path.generic_string();
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (!out.good()) {
        errorMessage = "failed to write patch file: " + path.generic_string();
        return false;
    }
    return true;
}

std::filesystem::path MakeWorktreeHarvestPatchPath(const std::filesystem::path& repoPath,
                                                   const std::string& repoId,
                                                   const BranchWorktree& worktree) {
    auto component = SanitizePlanComponent(repoId + "-" + WorktreeBranchLabel(worktree));
    if (!worktree.head.empty()) {
        component += "-" + worktree.head.substr(0, std::min<std::size_t>(12, worktree.head.size()));
    }
    return (repoPath / ".kano" / "tmp" / "converge" / "worktree-harvest" / (component + ".patch")).lexically_normal();
}

void AppendWorktreeHarvestBlocked(nlohmann::json& result,
                                  const std::string& repoId,
                                  const BranchWorktree& worktree,
                                  const std::string& action,
                                  std::vector<std::string> blockers,
                                  const std::string& message,
                                  const std::string& integrationProof,
                                  const std::vector<std::string>& changedPaths = {},
                                  const std::string& recoveryCommand = {}) {
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    result["blocked"].push_back({
        {"repo", repoId},
        {"branch", WorktreeBranchLabel(worktree)},
        {"action", action},
        {"worktree", worktree.location},
        {"head", worktree.head},
        {"integrationProof", integrationProof},
        {"changedPaths", changedPaths},
        {"blockers", blockers},
        {"message", message},
        {"recoveryCommand", recoveryCommand},
    });
}

void AppendDetachedWorktreeHarvestBlocked(nlohmann::json& result,
                                          const std::string& repoId,
                                          const BranchWorktree& worktree,
                                          std::vector<std::string> blockers,
                                          const std::string& message,
                                          const std::string& integrationProof,
                                          const std::vector<std::string>& changedPaths = {}) {
    AppendWorktreeHarvestBlocked(result,
                                 repoId,
                                 worktree,
                                 "harvest-detached-worktree",
                                 std::move(blockers),
                                 message,
                                 integrationProof,
                                 changedPaths);
}

bool PushTargetBranch(const std::filesystem::path& repoPath,
                      const RepoStatus& repo,
                      const std::string& targetBranch,
                      std::string& errorMessage) {
    auto push = GitCapture(repoPath, {"push"});
    if (push.exitCode == 0) {
        return true;
    }

    const auto remote = repo.remote.empty() ? std::string{"origin"} : repo.remote;
    push = GitCapture(repoPath, {"push", remote, targetBranch});
    if (push.exitCode == 0) {
        return true;
    }

    errorMessage = CombinedGitError(push);
    return false;
}

bool CheckoutTargetBranch(const std::filesystem::path& repoPath,
                          const std::string& repoId,
                          const std::string& targetBranch,
                          nlohmann::json& result);

bool HarvestDirtyWorktree(const std::filesystem::path& repoPath,
                          const RepoStatus& repo,
                          const std::string& targetBranch,
                          const BranchWorktree& worktree,
                          const std::string& integrationProof,
                          const std::string& action,
                          const std::string& successMessage,
                          const std::string& commitMessage,
                          const std::string& recoveryCommand,
                          nlohmann::json& result) {
    std::string dirtyError;
    const auto dirtyEntries = CollectDirtyEntries(worktree.absolutePath, &dirtyError);
    const auto changedPaths = DirtyEntryPaths(dirtyEntries);
    if (!dirtyError.empty()) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"DIRTY_STATUS_FAILED"}, dirtyError, integrationProof, changedPaths, recoveryCommand);
        return false;
    }
    if (changedPaths.empty()) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"EMPTY_HARVEST_PATCH"}, "dirty worktree had no changed paths to harvest", integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    std::string targetCleanMessage;
    if (!WorktreeIsClean(repoPath, targetCleanMessage)) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"DIRTY_TARGET_WORKTREE"}, targetCleanMessage, integrationProof, changedPaths, recoveryCommand);
        return false;
    }
    if (!CheckoutTargetBranch(repoPath, repo.id, targetBranch, result)) {
        return false;
    }

    const auto untrackedPaths = UntrackedDirtyPaths(dirtyEntries);
    std::string intentError;
    if (!MarkIntentToAddUntracked(worktree.absolutePath, untrackedPaths, intentError)) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"UNTRACKED_INTENT_TO_ADD_FAILED"}, intentError, integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    const auto diff = GitCapture(worktree.absolutePath, {"diff", "--binary", "HEAD", "--"});
    if (!untrackedPaths.empty()) {
        std::vector<std::string> resetArgs{"reset", "-q", "--"};
        resetArgs.insert(resetArgs.end(), untrackedPaths.begin(), untrackedPaths.end());
        (void)GitCapture(worktree.absolutePath, resetArgs);
    }
    if (diff.exitCode != 0) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"HARVEST_DIFF_FAILED"}, CombinedGitError(diff), integrationProof, changedPaths, recoveryCommand);
        return false;
    }
    if (Trim(diff.stdoutStr).empty()) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"EMPTY_HARVEST_PATCH"}, "dirty worktree produced an empty patch", integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    const auto patchPath = MakeWorktreeHarvestPatchPath(repoPath, repo.id, worktree);
    std::string writeError;
    if (!WriteBinaryFile(patchPath, diff.stdoutStr, writeError)) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"HARVEST_PATCH_WRITE_FAILED"}, writeError, integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    auto cleanupPatch = [&]() {
        std::error_code ec;
        std::filesystem::remove(patchPath, ec);
    };

    const auto apply = GitCapture(repoPath, {"apply", "--index", "--whitespace=nowarn", patchPath.string()});
    if (apply.exitCode != 0) {
        cleanupPatch();
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"HARVEST_PATCH_APPLY_FAILED"}, CombinedGitError(apply), integrationProof, changedPaths, recoveryCommand);
        return false;
    }
    cleanupPatch();

    const auto commit = GitCapture(repoPath, {"commit", "-m", commitMessage.empty() ? std::string{kWorktreeHarvestDefaultMessage} : commitMessage});
    if (commit.exitCode != 0) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"HARVEST_COMMIT_FAILED"}, CombinedGitError(commit), integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    const auto commitHead = GitCapture(repoPath, {"rev-parse", "HEAD"});
    const auto harvestedCommit = commitHead.exitCode == 0 ? Trim(commitHead.stdoutStr) : std::string{};
    result["mutationPerformed"] = true;

    std::string pushError;
    if (!PushTargetBranch(repoPath, repo, targetBranch, pushError)) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"HARVEST_PUSH_FAILED"}, pushError, integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    const auto remove = GitCapture(repoPath, {"worktree", "remove", "--force", worktree.absolutePath.string()});
    if (remove.exitCode != 0) {
        AppendWorktreeHarvestBlocked(result, repo.id, worktree, action, {"WORKTREE_REMOVE_FAILED"}, CombinedGitError(remove), integrationProof, changedPaths, recoveryCommand);
        return false;
    }

    result["mutationPerformed"] = true;
    result["harvested"].push_back({
        {"repo", repo.id},
        {"branch", WorktreeBranchLabel(worktree)},
        {"action", action},
        {"message", successMessage},
        {"worktree", worktree.location},
        {"head", worktree.head},
        {"targetBranch", targetBranch},
        {"integrationProof", integrationProof},
        {"changedPaths", changedPaths},
        {"commit", harvestedCommit},
    });
    return true;
}

bool CheckoutTargetBranch(const std::filesystem::path& repoPath,
                          const std::string& repoId,
                          const std::string& targetBranch,
                          nlohmann::json& result) {
    const auto checkout = GitCapture(repoPath, {"checkout", targetBranch});
    if (checkout.exitCode != 0) {
        AppendBranchBlocked(result, repoId, targetBranch, {"TARGET_CHECKOUT_FAILED"}, CombinedGitError(checkout));
        return false;
    }
    return true;
}

bool SyncTargetBranch(const std::filesystem::path& repoPath,
                      const std::string& repoId,
                      const std::string& targetBranch,
                      nlohmann::json& result,
                      bool allowUntrackedOnly = false) {
    std::string cleanMessage;
    if (!WorktreeIsClean(repoPath, cleanMessage, allowUntrackedOnly)) {
        AppendBranchBlocked(result, repoId, targetBranch, {"DIRTY_TARGET_WORKTREE"}, cleanMessage);
        return false;
    }

    const auto fetch = GitCapture(repoPath, {"fetch", "--prune"});
    if (fetch.exitCode != 0) {
        AppendBranchBlocked(result, repoId, targetBranch, {"FETCH_FAILED"}, CombinedGitError(fetch));
        return false;
    }

    if (!CheckoutTargetBranch(repoPath, repoId, targetBranch, result)) {
        return false;
    }

    auto upstream = UpstreamForBranch(repoPath, targetBranch);
    if (upstream.empty() && GitCapture(repoPath, {"rev-parse", "--verify", "--quiet", ("origin/" + targetBranch) + "^{commit}"}).exitCode == 0) {
        upstream = "origin/" + targetBranch;
    }
    if (upstream.empty()) {
        result["targetSync"].push_back({
            {"repo", repoId},
            {"branch", targetBranch},
            {"status", "no-upstream"},
        });
        return true;
    }

    const auto merge = GitCapture(repoPath, {"merge", "--ff-only", upstream});
    if (merge.exitCode != 0) {
        AppendBranchBlocked(result, repoId, targetBranch, {"TARGET_FAST_FORWARD_FAILED"}, CombinedGitError(merge));
        return false;
    }
    result["targetSync"].push_back({
        {"repo", repoId},
        {"branch", targetBranch},
        {"status", "fast-forwarded"},
        {"upstream", upstream},
    });
    return true;
}

std::string ResolveTargetRef(const std::filesystem::path& repoPath, const std::string& targetBranch, const std::string& preferredRemote) {
    if (GitCapture(repoPath, {"rev-parse", "--verify", "--quiet", targetBranch + "^{commit}"}).exitCode == 0) {
        return targetBranch;
    }
    if (!preferredRemote.empty()) {
        const auto remoteRef = preferredRemote + "/" + targetBranch;
        if (GitCapture(repoPath, {"rev-parse", "--verify", "--quiet", remoteRef + "^{commit}"}).exitCode == 0) {
            return remoteRef;
        }
    }
    const auto originRef = "origin/" + targetBranch;
    if (preferredRemote != "origin" && GitCapture(repoPath, {"rev-parse", "--verify", "--quiet", originRef + "^{commit}"}).exitCode == 0) {
        return originRef;
    }
    return {};
}

BranchAheadBehind AheadBehindForBranch(const std::filesystem::path& repoPath, const std::string& branch) {
    BranchAheadBehind out;
    const auto upstream = GitCapture(repoPath, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", branch + "@{upstream}"});
    const auto upstreamName = Trim(upstream.stdoutStr);
    if (upstream.exitCode != 0 || upstreamName.empty()) {
        return out;
    }
    out.hasUpstream = true;
    const auto counts = GitCapture(repoPath, {"rev-list", "--left-right", "--count", branch + "..." + upstreamName});
    if (counts.exitCode == 0) {
        std::istringstream iss(Trim(counts.stdoutStr));
        iss >> out.ahead >> out.behind;
    }
    return out;
}

bool BranchMergedIntoTarget(const std::filesystem::path& repoPath, const std::string& branch, const std::string& targetRef) {
    if (targetRef.empty() || branch == targetRef) {
        return branch == targetRef;
    }
    return GitCapture(repoPath, {"merge-base", "--is-ancestor", branch, targetRef}).exitCode == 0;
}

bool CommitMergedIntoTarget(const std::filesystem::path& repoPath, const std::string& commit, const std::string& targetRef) {
    if (targetRef.empty() || commit.empty()) {
        return false;
    }
    return GitCapture(repoPath, {"merge-base", "--is-ancestor", commit, targetRef}).exitCode == 0;
}

bool BranchPatchEquivalentToTarget(const std::filesystem::path& repoPath, const std::string& targetBranch, const std::string& branch) {
    if (g_branchPlanExecutionContext != nullptr && !g_branchPlanExecutionContext->allowPatchEquivalentProof) {
        return false;
    }
    const auto cherry = GitCapture(repoPath, {"cherry", targetBranch, branch});
    if (cherry.exitCode != 0) {
        return false;
    }
    const auto lines = SplitNonEmptyLines(cherry.stdoutStr);
    if (lines.empty()) {
        return false;
    }
    return std::none_of(lines.begin(), lines.end(), [](const std::string& line) {
        return !line.empty() && line[0] == '+';
    });
}

bool CommitPatchEquivalentToTarget(const std::filesystem::path& repoPath, const std::string& targetBranch, const std::string& commit) {
    if (targetBranch.empty() || commit.empty()) {
        return false;
    }
    if (g_branchPlanExecutionContext != nullptr && !g_branchPlanExecutionContext->allowPatchEquivalentProof) {
        return false;
    }
    const auto cherry = GitCapture(repoPath, {"cherry", targetBranch, commit});
    if (cherry.exitCode != 0) {
        return false;
    }
    const auto lines = SplitNonEmptyLines(cherry.stdoutStr);
    if (lines.empty()) {
        return false;
    }
    return std::none_of(lines.begin(), lines.end(), [](const std::string& line) {
        return !line.empty() && line[0] == '+';
    });
}

std::string DetachedWorktreeIntegrationProof(const std::filesystem::path& repoPath,
                                             const std::string& targetBranch,
                                             const std::string& targetRef,
                                             const BranchWorktree& worktree) {
    if (CommitMergedIntoTarget(repoPath, worktree.head, targetRef)) {
        return "merged";
    }
    if (CommitPatchEquivalentToTarget(repoPath, targetBranch, worktree.head)) {
        return "patch-equivalent";
    }
    return {};
}

DetachedWorktreeEvaluation EvaluateDetachedWorktree(const std::filesystem::path& repoPath,
                                                    const std::string& targetBranch,
                                                    const std::string& targetRef,
                                                    const BranchWorktree& worktree) {
    DetachedWorktreeEvaluation out;
    out.primary = SamePath(worktree.absolutePath, repoPath);
    out.clean = WorktreeIsClean(worktree.absolutePath, out.cleanMessage);
    if (targetRef.empty()) {
        out.blockers.push_back("TARGET_REF_MISSING");
    }
    if (out.primary) {
        out.blockers.push_back("PRIMARY_WORKTREE_REMOVE_REFUSED");
    }
    if (worktree.head.empty()) {
        out.blockers.push_back("DETACHED_WORKTREE_HEAD_MISSING");
    } else {
        out.integrationProof = DetachedWorktreeIntegrationProof(repoPath, targetBranch, targetRef, worktree);
        out.integrated = !out.integrationProof.empty();
    }
    if (!out.integrated) {
        out.blockers.push_back("DETACHED_WORKTREE_NOT_INTEGRATED");
    }
    if (!out.clean) {
        out.blockers.push_back("DIRTY_DETACHED_WORKTREE");
    }
    std::sort(out.blockers.begin(), out.blockers.end());
    out.blockers.erase(std::unique(out.blockers.begin(), out.blockers.end()), out.blockers.end());

    if (out.blockers.empty()) {
        out.proposedActions.push_back("candidate for human-reviewed detached worktree retirement with --remove-worktrees");
    } else {
        out.proposedActions.push_back("resolve blockers before detached worktree retirement");
    }
    return out;
}

std::vector<std::string> RetireBlockersForIntegratedBranch(std::vector<std::string> blockers,
                                                           bool removeWorktrees,
                                                           bool deleteRemote,
                                                           bool harvestBranchWorktrees) {
    blockers.erase(std::remove(blockers.begin(), blockers.end(), "UNPUSHED_COMMITS"), blockers.end());
    blockers.erase(std::remove_if(blockers.begin(),
                                  blockers.end(),
                                  [](const std::string& blocker) {
                                      return blocker.rfind("DIRTY_WORKTREE:", 0) == 0;
                                  }),
                   blockers.end());
    if (!deleteRemote) {
        blockers.erase(std::remove(blockers.begin(), blockers.end(), "STALE_LOCAL_BRANCH"), blockers.end());
    }
    if (removeWorktrees) {
        blockers.erase(std::remove(blockers.begin(), blockers.end(), "ACTIVE_WORKTREE_LEASE"), blockers.end());
    }
    if (harvestBranchWorktrees) {
        blockers.erase(std::remove(blockers.begin(), blockers.end(), "DIRTY_WORKTREE_LEASE"), blockers.end());
    }
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    return blockers;
}

std::vector<std::string> BlockersForNoopProof(std::vector<std::string> blockers) {
    blockers.erase(std::remove_if(blockers.begin(),
                                  blockers.end(),
                                  [](const std::string& blocker) {
                                      return blocker == "ACTIVE_WORKTREE_LEASE" ||
                                             blocker == "UNPUSHED_COMMITS" ||
                                             blocker.rfind("DIRTY_WORKTREE:", 0) == 0;
                                  }),
                   blockers.end());
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    return blockers;
}

std::vector<std::string> ApplyBlockersForCherryPickBranch(std::vector<std::string> blockers) {
    blockers.erase(std::remove(blockers.begin(), blockers.end(), "UNPUSHED_COMMITS"), blockers.end());
    blockers.erase(std::remove(blockers.begin(), blockers.end(), "ACTIVE_WORKTREE_LEASE"), blockers.end());
    std::sort(blockers.begin(), blockers.end());
    blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
    return blockers;
}

bool BranchWorktreesAreClean(const std::vector<BranchWorktree>& branchWorktrees,
                             const std::string& repoId,
                             const std::string& branch,
                             nlohmann::json& result) {
    bool clean = true;
    for (const auto& worktree : branchWorktrees) {
        std::string cleanMessage;
        if (!WorktreeIsClean(worktree.absolutePath, cleanMessage)) {
            AppendBranchBlocked(result, repoId, branch, {"DIRTY_WORKTREE_LEASE"}, cleanMessage);
            clean = false;
        }
    }
    return clean;
}

std::vector<std::string> CherryPickCommitsForBranch(const std::filesystem::path& repoPath,
                                                    const std::string& targetBranch,
                                                    const std::string& branch,
                                                    std::string& errorMessage) {
    const auto mergeBase = GitCapture(repoPath, {"merge-base", targetBranch, branch});
    if (mergeBase.exitCode != 0) {
        errorMessage = CombinedGitError(mergeBase);
        return {};
    }
    const auto base = Trim(mergeBase.stdoutStr);
    const auto revList = GitCapture(repoPath, {"rev-list", "--reverse", base + ".." + branch});
    if (revList.exitCode != 0) {
        errorMessage = CombinedGitError(revList);
        return {};
    }
    const auto cherry = GitCapture(repoPath, {"cherry", targetBranch, branch});
    if (cherry.exitCode != 0) {
        errorMessage = CombinedGitError(cherry);
        return {};
    }

    std::unordered_set<std::string> missing;
    for (const auto& line : SplitNonEmptyLines(cherry.stdoutStr)) {
        if (line.size() > 2 && line[0] == '+') {
            missing.insert(Trim(line.substr(1)));
        }
    }

    std::vector<std::string> commits;
    for (const auto& commit : SplitNonEmptyLines(revList.stdoutStr)) {
        if (missing.find(commit) != missing.end()) {
            commits.push_back(commit);
        }
    }
    return commits;
}

bool IsEmptyCherryPickError(const shell::ExecResult& result) {
    const auto message = ToLower(CombinedGitError(result));
    return message.find("previous cherry-pick is now empty") != std::string::npos ||
           message.find("nothing to commit") != std::string::npos;
}

bool SkipEmptyCherryPick(const std::filesystem::path& repoPath, std::string& errorMessage) {
    const auto skip = GitCapture(repoPath, {"cherry-pick", "--skip"});
    if (skip.exitCode == 0) {
        return true;
    }

    const auto status = GitCapture(repoPath, {"status", "--porcelain"});
    if (status.exitCode == 0 && Trim(status.stdoutStr).empty()) {
        return true;
    }

    errorMessage = CombinedGitError(skip);
    return false;
}

std::filesystem::path MakeCherryPickProbePath() {
    if (const char* testPath = std::getenv("KOG_TEST_CHERRY_PICK_PROBE_PATH"); testPath != nullptr && testPath[0] != '\0') {
        return std::filesystem::path(testPath).lexically_normal();
    }
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long long>(getpid());
#endif
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / ("kog-cherry-pick-probe-" + std::to_string(pid) + "-" + std::to_string(ticks));
}

bool BranchCherryPickNoopIntoTarget(const std::filesystem::path& repoPath,
                                    const std::string& targetBranch,
                                    const std::string& branch,
                                    std::string* outError = nullptr) {
    std::string commitPlanError;
    const auto commits = CherryPickCommitsForBranch(repoPath, targetBranch, branch, commitPlanError);
    if (!commitPlanError.empty()) {
        if (outError != nullptr) {
            *outError = commitPlanError;
        }
        return false;
    }
    if (commits.empty()) {
        return true;
    }

    const auto targetHead = GitCapture(repoPath, {"rev-parse", "--verify", targetBranch + "^{commit}"});
    if (targetHead.exitCode != 0) {
        if (outError != nullptr) {
            *outError = CombinedGitError(targetHead);
        }
        return false;
    }

    const auto probePath = MakeCherryPickProbePath();
    auto cleanupProbe = [&]() {
        (void)GitCaptureForCleanup(repoPath, {"worktree", "unlock", probePath.string()});
        (void)GitCaptureForCleanup(repoPath, {"worktree", "remove", "--force", probePath.string()});
        std::error_code ec;
        std::filesystem::remove_all(probePath, ec);
        (void)GitCaptureForCleanup(repoPath, {"worktree", "prune", "--expire", "now"});
    };

    const auto add = GitCapture(repoPath, {"worktree", "add", "--detach", probePath.string(), Trim(targetHead.stdoutStr)});
    if (add.exitCode != 0) {
        cleanupProbe();
        if (outError != nullptr) {
            *outError = CombinedGitError(add);
        }
        return false;
    }
    bool sawNoop = false;
    for (const auto& commit : commits) {
        const auto cherryPick = GitCapture(probePath, {"cherry-pick", commit});
        if (cherryPick.exitCode == 0) {
            cleanupProbe();
            return false;
        }
        if (!IsEmptyCherryPickError(cherryPick)) {
            if (outError != nullptr) {
                *outError = CombinedGitError(cherryPick);
            }
            cleanupProbe();
            return false;
        }
        std::string skipError;
        if (!SkipEmptyCherryPick(probePath, skipError)) {
            if (outError != nullptr) {
                *outError = skipError;
            }
            cleanupProbe();
            return false;
        }
        sawNoop = true;
    }

    cleanupProbe();
    return sawNoop;
}

nlohmann::json BranchPlanJsonForRepo(const Snapshot& snapshot,
                                     const RepoStatus& repo,
                                     const std::filesystem::path& repoPath,
                                     const std::string& targetBranch,
                                     const std::string& strategy,
                                     bool allowNoopProof) {
    const bool snapshotIsTarget = repo.branch == targetBranch && !repo.head.empty();
    const auto targetRef = snapshotIsTarget
        ? targetBranch
        : ResolveTargetRef(repoPath, targetBranch, repo.remote);
    const auto targetCounts = snapshotIsTarget
        ? BranchAheadBehind{.ahead = repo.ahead, .behind = repo.behind, .hasUpstream = !repo.upstream.empty()}
        : AheadBehindForBranch(repoPath, targetBranch);
    const bool targetBranchBehind = targetCounts.hasUpstream && targetCounts.behind > 0;
    const auto branches = BranchCandidates(repoPath, repo.remote);
    const auto worktrees = Worktrees(repoPath, snapshot.workspaceRoot);

    nlohmann::json repoJson;
    repoJson["id"] = repo.id;
    repoJson["type"] = repo.type;
    repoJson["managementPolicy"] = repo.managementPolicy;
    repoJson["branch"] = repo.branch;
    repoJson["head"] = repo.head;
    repoJson["remote"] = repo.remote;
    repoJson["upstream"] = repo.upstream;
    repoJson["ahead"] = repo.ahead;
    repoJson["behind"] = repo.behind;
    repoJson["dirtyKind"] = repo.dirtyKind;
    repoJson["parentRepos"] = repo.parentRepos;
    repoJson["childRepos"] = repo.childRepos;

    nlohmann::json worktreeJson = nlohmann::json::array();
    for (const auto& worktree : worktrees) {
        nlohmann::json worktreeObject = {
            {"branch", worktree.branch},
            {"location", worktree.location},
            {"head", worktree.head},
            {"detached", worktree.detached},
            {"bare", worktree.bare},
        };
        if (worktree.detached && !worktree.bare) {
            const auto evaluation = EvaluateDetachedWorktree(repoPath, targetBranch, targetRef, worktree);
            worktreeObject["primary"] = evaluation.primary;
            worktreeObject["clean"] = evaluation.clean;
            worktreeObject["cleanMessage"] = evaluation.clean ? std::string{} : evaluation.cleanMessage;
            worktreeObject["integratedIntoTarget"] = evaluation.integrated;
            worktreeObject["integrationProof"] = evaluation.integrationProof;
            worktreeObject["blockers"] = evaluation.blockers;
            worktreeObject["proposedActions"] = evaluation.proposedActions;
        } else if (!worktree.bare) {
            std::string cleanMessage;
            const auto clean = WorktreeIsClean(worktree.absolutePath, cleanMessage);
            std::string dirtyError;
            const auto changedPaths = clean
                ? std::vector<std::string>{}
                : DirtyEntryPaths(CollectDirtyEntries(worktree.absolutePath, &dirtyError));
            worktreeObject["primary"] = SamePath(worktree.absolutePath, repoPath);
            worktreeObject["clean"] = clean;
            worktreeObject["cleanMessage"] = clean ? std::string{} : cleanMessage;
            worktreeObject["changedPaths"] = changedPaths;
            worktreeObject["dirtyStatusError"] = dirtyError;
            worktreeObject["recoveryCommand"] = clean || worktree.branch.empty()
                ? std::string{}
                : BranchWorktreeHarvestCommand(targetBranch, worktree.branch);
        }
        worktreeJson.push_back(std::move(worktreeObject));
    }
    repoJson["worktrees"] = std::move(worktreeJson);

    nlohmann::json branchesJson = nlohmann::json::array();
    for (const auto& candidate : branches) {
        const auto& branch = candidate.name;
        const auto& branchRef = candidate.ref;
        const auto isTarget = !candidate.remoteOnly && branch == targetBranch;
        if (!isTarget && IsPublishBranchCandidate(candidate)) {
            branchesJson.push_back({
                {"name", branch},
                {"ref", branchRef},
                {"remoteOnly", candidate.remoteOnly},
                {"remote", candidate.remote},
                {"remoteBranch", candidate.remoteBranch},
                {"isTarget", false},
                {"targetRef", targetRef},
                {"strategy", strategy},
                {"checkedOutWorktrees", nlohmann::json::array()},
                {"activeLeaseBlocker", false},
                {"hasUpstream", false},
                {"ahead", 0},
                {"behind", 0},
                {"mergedIntoTarget", false},
                {"patchEquivalentToTarget", false},
                {"cherryPickNoopIntoTarget", false},
                {"integratedIntoTarget", false},
                {"integrationProof", ""},
                {"nonConvergeTarget", true},
                {"skipReason", "PUBLISH_BRANCH"},
                {"blockers", nlohmann::json::array()},
                {"proposedActions", nlohmann::json::array({"skipped publish branch; not a coding convergence target"})},
            });
            continue;
        }
        const auto branchWorktrees = candidate.remoteOnly ? std::vector<BranchWorktree>{} : WorktreesForBranch(worktrees, branch);
        const auto checkedOut = candidate.remoteOnly ? std::vector<std::string>{} : WorktreeLocationsForBranch(worktrees, branch);
        nlohmann::json branchWorktreeInventory = nlohmann::json::array();
        std::size_t dirtyWorktreeCount = 0;
        for (const auto& worktree : branchWorktrees) {
            std::string cleanMessage;
            const auto clean = WorktreeIsClean(worktree.absolutePath, cleanMessage);
            std::string dirtyError;
            const auto changedPaths = clean
                ? std::vector<std::string>{}
                : DirtyEntryPaths(CollectDirtyEntries(worktree.absolutePath, &dirtyError));
            if (!clean) {
                ++dirtyWorktreeCount;
            }
            branchWorktreeInventory.push_back({
                {"location", worktree.location},
                {"head", worktree.head},
                {"primary", SamePath(worktree.absolutePath, repoPath)},
                {"clean", clean},
                {"cleanMessage", clean ? std::string{} : cleanMessage},
                {"changedPaths", changedPaths},
                {"dirtyStatusError", dirtyError},
                {"recoveryCommand", clean ? std::string{} : BranchWorktreeHarvestCommand(targetBranch, branch)},
            });
        }
        const auto counts = candidate.remoteOnly
            ? BranchAheadBehind{}
            : (isTarget && snapshotIsTarget ? targetCounts : AheadBehindForBranch(repoPath, branch));
        const auto merged = isTarget || BranchMergedIntoTarget(repoPath, branchRef, targetRef);
        const auto patchEquivalent = !isTarget && !merged && !targetRef.empty() && BranchPatchEquivalentToTarget(repoPath, targetBranch, branchRef);

        std::vector<std::string> blockers;
        if (targetRef.empty()) {
            blockers.push_back("TARGET_REF_MISSING");
        }
        if (!isTarget && !checkedOut.empty()) {
            blockers.push_back("ACTIVE_WORKTREE_LEASE");
        }
        if (!isTarget && dirtyWorktreeCount > 0) {
            blockers.push_back("DIRTY_WORKTREE_LEASE");
        }
        if (DirtyKindBlocksBranchPlan(repo.dirtyKind) && !IsCleanNestedPreflightOnlyBlocker(repo)) {
            blockers.push_back("DIRTY_WORKTREE:" + repo.dirtyKind);
        }
        if (repo.blocksConverge && !IsCleanNestedPreflightOnlyBlocker(repo)) {
            blockers.push_back(repo.blockReason.empty() ? std::string{"REPO_PREFLIGHT_BLOCKED"} : ("REPO_PREFLIGHT_BLOCKED:" + repo.blockReason));
        }
        if (targetBranchBehind) {
            blockers.push_back("STALE_TARGET_BRANCH");
        }
        if (!candidate.remoteOnly && (counts.ahead > 0 || (!isTarget && !counts.hasUpstream && !merged))) {
            blockers.push_back("UNPUSHED_COMMITS");
        }
        if (!candidate.remoteOnly && !isTarget && counts.behind > 0) {
            blockers.push_back("STALE_LOCAL_BRANCH");
        }
        std::sort(blockers.begin(), blockers.end());
        blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());

        bool cherryPickNoop = false;
        bool cherryPickNoopProbePerformed = false;
        if (allowNoopProof && !isTarget && !merged && !patchEquivalent && strategy == "cherry-pick" && BlockersForNoopProof(blockers).empty()) {
            cherryPickNoopProbePerformed = true;
            cherryPickNoop = BranchCherryPickNoopIntoTarget(repoPath, targetBranch, branchRef);
        }
        const auto integrated = merged || patchEquivalent || cherryPickNoop;
        if (integrated && !isTarget) {
            blockers = RetireBlockersForIntegratedBranch(std::move(blockers), true, false, false);
        }

        std::vector<std::string> actions;
        if (isTarget) {
            actions.push_back("keep target branch");
        } else if (integrated && dirtyWorktreeCount > 0) {
            actions.push_back("review dirty worktree paths, then run " + BranchWorktreeHarvestCommand(targetBranch, branch));
        } else if (!blockers.empty()) {
            actions.push_back("resolve blockers before branch integration or retirement");
        } else if (integrated) {
            actions.push_back("candidate for human-reviewed branch retirement; planner does not delete branches");
        } else if (strategy == "merge") {
            actions.push_back("would plan merge integration into " + targetBranch);
        } else if (strategy == "cherry-pick") {
            actions.push_back("would plan cherry-pick integration onto " + targetBranch);
        } else {
            actions.push_back("would plan rebase integration onto " + targetBranch);
        }

        branchesJson.push_back({
            {"name", branch},
            {"ref", branchRef},
            {"remoteOnly", candidate.remoteOnly},
            {"remote", candidate.remote},
            {"remoteBranch", candidate.remoteBranch},
            {"isTarget", isTarget},
            {"targetRef", targetRef},
            {"strategy", strategy},
            {"checkedOutWorktrees", checkedOut},
            {"activeLeaseBlocker", !isTarget && !checkedOut.empty()},
            {"worktreeInventory", branchWorktreeInventory},
            {"dirtyWorktreeCount", dirtyWorktreeCount},
            {"dirtyWorktreeRecoveryCommand", dirtyWorktreeCount == 0 ? std::string{} : BranchWorktreeHarvestCommand(targetBranch, branch)},
            {"hasUpstream", counts.hasUpstream},
            {"ahead", counts.ahead},
            {"behind", counts.behind},
            {"mergedIntoTarget", merged},
            {"patchEquivalentToTarget", patchEquivalent},
            {"cherryPickNoopIntoTarget", cherryPickNoop},
            {"cherryPickNoopProbePerformed", cherryPickNoopProbePerformed},
            {"integratedIntoTarget", integrated},
            {"integrationProof", merged ? "merged" : (patchEquivalent ? "patch-equivalent" : (cherryPickNoop ? "cherry-pick-noop" : ""))},
            {"blockers", blockers},
            {"proposedActions", actions},
        });
    }

    std::unordered_map<std::string, std::set<std::size_t>> dirtyPathOwners;
    for (std::size_t branchIndex = 0; branchIndex < branchesJson.size(); ++branchIndex) {
        for (const auto& worktree : branchesJson[branchIndex].value("worktreeInventory", nlohmann::json::array())) {
            for (const auto& path : worktree.value("changedPaths", nlohmann::json::array())) {
                if (path.is_string()) {
                    dirtyPathOwners[path.get<std::string>()].insert(branchIndex);
                }
            }
        }
    }
    std::vector<std::vector<std::string>> overlapPaths(branchesJson.size());
    for (const auto& [path, owners] : dirtyPathOwners) {
        if (owners.size() < 2) {
            continue;
        }
        for (const auto owner : owners) {
            overlapPaths[owner].push_back(path);
        }
    }
    for (std::size_t branchIndex = 0; branchIndex < branchesJson.size(); ++branchIndex) {
        if (overlapPaths[branchIndex].empty()) {
            branchesJson[branchIndex]["dirtyWorktreeOverlapPaths"] = nlohmann::json::array();
            continue;
        }
        auto& paths = overlapPaths[branchIndex];
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        auto blockers = BranchBlockers(branchesJson[branchIndex]);
        blockers.push_back("DIRTY_WORKTREE_OVERLAP");
        std::sort(blockers.begin(), blockers.end());
        blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
        branchesJson[branchIndex]["blockers"] = blockers;
        branchesJson[branchIndex]["dirtyWorktreeOverlapPaths"] = paths;
        branchesJson[branchIndex]["proposedActions"] = nlohmann::json::array({
            "resolve overlapping dirty worktree paths before branch harvest",
        });
    }
    repoJson["branches"] = std::move(branchesJson);
    return repoJson;
}

nlohmann::json BuildBranchPlanJson(const Snapshot& snapshot,
                                   const std::filesystem::path& workspaceRoot,
                                   const std::string& targetBranch,
                                   const std::string& strategy,
                                   bool recursive,
                                   bool allowNoopProof,
                                   bool allowPatchEquivalentProof) {
    BranchPlanExecutionContext executionContext;
    executionContext.deadlineMs = ResolveBranchPlanDeadlineMs();
    executionContext.probeTimeoutMs = ResolveBranchProbeTimeoutMs();
    executionContext.allowPatchEquivalentProof = allowPatchEquivalentProof;
    executionContext.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(executionContext.deadlineMs);
    ScopedBranchPlanExecutionContext scopedExecutionContext(executionContext);

    const auto traversal = BranchPlanTraversalOrder(snapshot);
    nlohmann::json repos = nlohmann::json::array();
    nlohmann::json planDiagnostics = nlohmann::json::array();
    for (const auto& repoId : traversal) {
        const auto* repo = FindRepo(snapshot, repoId);
        if (repo == nullptr) {
            continue;
        }
        executionContext.repoTimedOut = false;
        executionContext.repoDiagnostics.clear();
        auto repoJson = BranchPlanJsonForRepo(
            snapshot, *repo, RepoPathForBranchPlan(workspaceRoot, *repo), targetBranch, strategy, allowNoopProof);
        nlohmann::json repoDiagnostics = nlohmann::json::array();
        std::vector<std::string> planningBlockers;
        for (const auto& diagnostic : executionContext.repoDiagnostics) {
            const auto item = nlohmann::json{
                {"code", diagnostic.code},
                {"operation", diagnostic.operation},
                {"message", diagnostic.message},
                {"timeoutMs", diagnostic.timeoutMs},
            };
            repoDiagnostics.push_back(item);
            planDiagnostics.push_back({
                {"repo", repoId},
                {"code", diagnostic.code},
                {"operation", diagnostic.operation},
                {"message", diagnostic.message},
                {"timeoutMs", diagnostic.timeoutMs},
            });
            planningBlockers.push_back(diagnostic.code);
        }
        std::sort(planningBlockers.begin(), planningBlockers.end());
        planningBlockers.erase(std::unique(planningBlockers.begin(), planningBlockers.end()), planningBlockers.end());
        repoJson["planningTimedOut"] = !planningBlockers.empty();
        repoJson["planningBlockers"] = planningBlockers;
        repoJson["planningDiagnostics"] = std::move(repoDiagnostics);
        repos.push_back(std::move(repoJson));
    }

    return {
        {"schemaName", "kog.convergeBranchesPlan"},
        {"schemaVersion", 1},
        {"mutationPerformed", false},
        {"workspaceRoot", workspaceRoot.generic_string()},
        {"targetBranch", targetBranch},
        {"strategy", strategy},
        {"recursive", recursive},
        {"planningDeadlineMs", executionContext.deadlineMs},
        {"probeTimeoutMs", executionContext.probeTimeoutMs},
        {"cherryPickNoopProofEnabled", allowNoopProof},
        {"patchEquivalentProofEnabled", allowPatchEquivalentProof},
        {"planningTimedOut", !planDiagnostics.empty()},
        {"planningBlockers", planDiagnostics.empty() ? nlohmann::json::array() : nlohmann::json::array({"BRANCH_PLANNING_TIMEOUT"})},
        {"planningDiagnostics", planDiagnostics},
        {"traversalOrder", traversal},
        {"repos", repos},
    };
}

bool BranchPlanHasBlockers(const nlohmann::json& plan) {
    if (const auto it = plan.find("planningBlockers"); it != plan.end() && it->is_array() && !it->empty()) {
        return true;
    }
    for (const auto& repo : plan.value("repos", nlohmann::json::array())) {
        if (const auto it = repo.find("planningBlockers"); it != repo.end() && it->is_array() && !it->empty()) {
            return true;
        }
        for (const auto& branch : repo.value("branches", nlohmann::json::array())) {
            if (const auto it = branch.find("blockers"); it != branch.end() && it->is_array() && !it->empty()) {
                return true;
            }
        }
    }
    return false;
}

void PrintBranchPlanText(const nlohmann::json& plan) {
    std::cout << "Converge Branches Plan\n";
    std::cout << "  target=" << plan.value("targetBranch", std::string{})
              << " strategy=" << plan.value("strategy", std::string{})
              << " recursive=" << (plan.value("recursive", true) ? "true" : "false")
              << " mutation_performed=false\n";
    std::cout << "Traversal order\n";
    for (const auto& repoId : plan.value("traversalOrder", nlohmann::json::array())) {
        if (repoId.is_string()) {
            std::cout << "  - " << repoId.get<std::string>() << "\n";
        }
    }
    std::cout << "Branch candidates\n";
    for (const auto& repo : plan.value("repos", nlohmann::json::array())) {
        const auto repoId = repo.value("id", std::string{});
        std::cout << "  repo=" << repoId << " dirtyKind=" << repo.value("dirtyKind", std::string{}) << "\n";
        for (const auto& branch : repo.value("branches", nlohmann::json::array())) {
            std::vector<std::string> blockers;
            if (const auto it = branch.find("blockers"); it != branch.end() && it->is_array()) {
                for (const auto& blocker : *it) {
                    if (blocker.is_string()) {
                        blockers.push_back(blocker.get<std::string>());
                    }
                }
            }
            std::vector<std::string> actions;
            if (const auto it = branch.find("proposedActions"); it != branch.end() && it->is_array()) {
                for (const auto& action : *it) {
                    if (action.is_string()) {
                        actions.push_back(action.get<std::string>());
                    }
                }
            }
            std::cout << "    - branch=" << branch.value("name", std::string{})
                      << " target=" << (branch.value("isTarget", false) ? "true" : "false")
                      << " merged=" << (branch.value("mergedIntoTarget", false) ? "true" : "false")
                      << " ahead=" << branch.value("ahead", 0)
                      << " behind=" << branch.value("behind", 0)
                      << " blockers=" << (blockers.empty() ? std::string{"none"} : Csv(blockers))
                      << " action=" << (actions.empty() ? std::string{"none"} : Csv(actions))
                      << "\n";
        }
    }
}

bool BranchActionResultHasBlocked(const nlohmann::json& result) {
    const auto it = result.find("blocked");
    return it != result.end() && it->is_array() && !it->empty();
}

nlohmann::json MakeBranchActionResult(const std::string& schemaName,
                                      const std::string& targetBranch,
                                      const std::string& strategy,
                                      bool recursive,
                                      bool confirm) {
    return {
        {"schemaName", schemaName},
        {"schemaVersion", 1},
        {"mutationPerformed", false},
        {"operationPending", false},
        {"pendingOperation", nullptr},
        {"confirm", confirm},
        {"targetBranch", targetBranch},
        {"strategy", strategy},
        {"recursive", recursive},
        {"targetSync", nlohmann::json::array()},
        {"applied", nlohmann::json::array()},
        {"retired", nlohmann::json::array()},
        {"harvested", nlohmann::json::array()},
        {"pruned", nlohmann::json::array()},
        {"planned", nlohmann::json::array()},
        {"skipped", nlohmann::json::array()},
        {"preserved", nlohmann::json::array()},
        {"blocked", nlohmann::json::array()},
    };
}

void PrintBranchActionResultText(const std::string& title, const nlohmann::json& result) {
    std::cout << title << "\n";
    std::cout << "  target=" << result.value("targetBranch", std::string{})
              << " strategy=" << result.value("strategy", std::string{})
              << " recursive=" << (result.value("recursive", true) ? "true" : "false")
              << " confirm=" << (result.value("confirm", false) ? "true" : "false")
              << " mutation_performed=" << (result.value("mutationPerformed", false) ? "true" : "false")
              << "\n";
    for (const auto& field : {"targetSync", "planned", "applied", "retired", "harvested", "pruned", "skipped", "preserved", "blocked"}) {
        std::cout << field << "\n";
        const auto it = result.find(field);
        if (it == result.end() || !it->is_array() || it->empty()) {
            std::cout << "  - (none)\n";
            continue;
        }
        for (const auto& item : *it) {
            std::vector<std::string> blockers;
            if (const auto blockersIt = item.find("blockers"); blockersIt != item.end() && blockersIt->is_array()) {
                for (const auto& blocker : *blockersIt) {
                    if (blocker.is_string()) {
                        blockers.push_back(blocker.get<std::string>());
                    }
                }
            }
            std::cout << "  - repo=" << item.value("repo", std::string{})
                      << " branch=" << item.value("branch", std::string{})
                      << " action=" << item.value("action", item.value("status", std::string{}))
                      << " blockers=" << (blockers.empty() ? std::string{"none"} : Csv(blockers))
                      << " message=" << item.value("message", std::string{})
                      << "\n";
        }
    }
}

int RunBranchInventory(const std::filesystem::path& root,
                       int jobs,
                       bool recursive,
                       const std::string& targetBranch,
                       const std::string& strategy,
                       bool emitJson) {
    try {
        if (targetBranch.empty()) {
            std::cerr << "Error: --target must not be empty\n";
            return 2;
        }
        if (!IsBranchIntegrationStrategy(strategy)) {
            std::cerr << "Error: --strategy must be rebase, merge, or cherry-pick\n";
            return 2;
        }
        const bool jsonOutput = emitJson || IsConvergeAgentModeEnabled();
        std::optional<shell::ScopedConsoleWriteSuppression> suppressCommandLogs;
        if (jsonOutput) {
            suppressCommandLogs.emplace();
        }
        const auto snapshot = LoadConvergeSnapshot(root, jobs, false, recursive);
        auto plan = BuildBranchPlanJson(snapshot, root, targetBranch, strategy, recursive, true, true);
        plan["schemaName"] = "kog.convergeBranchesInventory";
        plan["inventoryOnly"] = true;
        suppressCommandLogs.reset();
        if (jsonOutput) {
            std::cout << plan.dump(2) << "\n";
        } else {
            PrintBranchPlanText(plan);
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}

int RunBranchApply(const std::filesystem::path& root,
                   int jobs,
                   bool recursive,
                   const std::string& targetBranch,
                   const std::string& strategy,
                   const std::string& branchFilter,
                   bool confirm,
                   bool syncTarget,
                   bool emitJson) {
    try {
        if (targetBranch.empty()) {
            std::cerr << "Error: --target must not be empty\n";
            return 2;
        }
        if (!IsBranchIntegrationStrategy(strategy)) {
            std::cerr << "Error: --strategy must be rebase, merge, or cherry-pick\n";
            return 2;
        }
        if (strategy == "cherry-pick" && branchFilter.empty()) {
            std::cerr << "Error: --branch is required when --strategy cherry-pick is used\n";
            return 2;
        }
        const bool jsonOutput = emitJson || IsConvergeAgentModeEnabled();
        auto result = MakeBranchActionResult("kog.convergeBranchesApplyResult", targetBranch, strategy, recursive, confirm);

        std::optional<shell::ScopedConsoleWriteSuppression> suppressCommandLogs;
        if (jsonOutput) {
            suppressCommandLogs.emplace();
        }

        auto snapshot = LoadConvergeSnapshot(root, jobs, false, recursive);
        if (confirm && syncTarget) {
            for (const auto& repoId : BranchPlanTraversalOrder(snapshot)) {
                const auto* repo = FindRepo(snapshot, repoId);
                if (repo == nullptr) {
                    continue;
                }
                (void)SyncTargetBranch(RepoPathForBranchPlan(root, *repo), repo->id, targetBranch, result);
            }
            if (!BranchActionResultHasBlocked(result)) {
                snapshot = LoadConvergeSnapshot(root, jobs, false, recursive);
            }
        }

        const auto plan = BuildBranchPlanJson(snapshot, root, targetBranch, strategy, recursive, false, false);
        for (const auto& repoJson : plan.value("repos", nlohmann::json::array())) {
            const auto repoId = repoJson.value("id", std::string{});
            const auto* repo = FindRepo(snapshot, repoId);
            if (repo == nullptr) {
                continue;
            }
            const auto repoPath = RepoPathForBranchPlan(root, *repo);
            const auto liveWorktrees = Worktrees(repoPath, root);
            for (const auto& branchJson : repoJson.value("branches", nlohmann::json::array())) {
                const auto branch = branchJson.value("name", std::string{});
                const auto branchRef = branchJson.value("ref", branch);
                if (branch.empty() || branchJson.value("isTarget", false)) {
                    continue;
                }
                if (!BranchMatchesFilter(branchJson, branchFilter)) {
                    continue;
                }
                if (branchJson.value("nonConvergeTarget", false)) {
                    AppendBranchAction(result, "skipped", repoId, branch, "non-converge-target", branchJson.value("skipReason", std::string{"skipped branch"}));
                    continue;
                }
                auto blockers = BranchBlockers(branchJson);
                if (branchJson.value("mergedIntoTarget", false)) {
                    AppendBranchAction(result, "skipped", repoId, branch, "already-merged", "branch is a retire candidate, not an apply candidate");
                    continue;
                }
                if (strategy == "cherry-pick") {
                    blockers = ApplyBlockersForCherryPickBranch(std::move(blockers));
                    if (!BranchWorktreesAreClean(WorktreesForBranch(liveWorktrees, branch), repoId, branch, result)) {
                        continue;
                    }
                }
                if (!blockers.empty()) {
                    AppendBranchBlocked(result, repoId, branch, blockers, "planner blockers must be resolved before apply");
                    continue;
                }
                if (!confirm) {
                    AppendBranchBlocked(result, repoId, branch, {"CONFIRM_REQUIRED"}, "rerun with --confirm to mutate target branch");
                    continue;
                }
                if (!CheckoutTargetBranch(repoPath, repoId, targetBranch, result)) {
                    continue;
                }

                shell::ExecResult integrate;
                if (strategy == "merge") {
                    integrate = GitCapture(repoPath, {"merge", "--no-ff", "--no-edit", branch});
                } else if (strategy == "cherry-pick") {
                    std::string commitPlanError;
                    const auto commits = CherryPickCommitsForBranch(repoPath, targetBranch, branchRef, commitPlanError);
                    if (!commitPlanError.empty()) {
                        AppendBranchBlocked(result, repoId, branch, {"CHERRY_PICK_PLAN_FAILED"}, commitPlanError);
                        continue;
                    }
                    if (commits.empty()) {
                        AppendBranchAction(result, "skipped", repoId, branch, "already-equivalent", "branch commits are already patch-equivalent to target");
                        continue;
                    }
                    const auto targetHeadBefore = GitCapture(repoPath, {"rev-parse", "--verify", targetBranch + "^{commit}"});
                    std::vector<std::string> cherryPickArgs{"cherry-pick"};
                    cherryPickArgs.insert(cherryPickArgs.end(), commits.begin(), commits.end());
                    const auto cherryPick = GitCapture(repoPath, cherryPickArgs);
                    bool cherryPickSucceeded = cherryPick.exitCode == 0;
                    if (!cherryPickSucceeded && IsEmptyCherryPickError(cherryPick)) {
                        std::string skipError;
                        if (SkipEmptyCherryPick(repoPath, skipError)) {
                            cherryPickSucceeded = true;
                        } else {
                            AppendBranchBlocked(result, repoId, branch, {"CHERRY_PICK_SKIP_FAILED"}, skipError);
                        }
                    } else if (!cherryPickSucceeded) {
                        AppendBranchBlocked(result, repoId, branch, {"CHERRY_PICK_CONFLICT"}, CombinedGitError(cherryPick));
                        result["mutationPerformed"] = true;
                        result["operationPending"] = true;
                        result["pendingOperation"] = {
                            {"type", "cherry-pick"},
                            {"repo", repoId},
                            {"branch", branch},
                            {"targetBranch", targetBranch},
                            {"workingDirectory", repoPath.generic_string()},
                            {"continueCommand", "kog cherry-pick --continue --repo ."},
                            {"abortCommand", "kog cherry-pick --abort --repo ."},
                        };
                    }
                    const auto targetHeadAfter = GitCapture(repoPath, {"rev-parse", "--verify", targetBranch + "^{commit}"});
                    const bool targetAdvanced = targetHeadBefore.exitCode == 0 && targetHeadAfter.exitCode == 0 &&
                                                Trim(targetHeadBefore.stdoutStr) != Trim(targetHeadAfter.stdoutStr);
                    const int appliedCherryPickCommits = targetAdvanced ? static_cast<int>(commits.size()) : 0;
                    if (targetAdvanced) {
                        result["mutationPerformed"] = true;
                    }
                    if (!cherryPickSucceeded) {
                        continue;
                    }
                    if (appliedCherryPickCommits == 0) {
                        AppendBranchAction(result, "skipped", repoId, branch, "already-equivalent", "branch commits produced no target changes");
                        continue;
                    }
                } else {
                    const auto targetIsAncestor = GitCapture(repoPath, {"merge-base", "--is-ancestor", targetBranch, branch});
                    if (targetIsAncestor.exitCode != 0) {
                        AppendBranchBlocked(result, repoId, branch, {"REBASE_APPLY_REQUIRES_FAST_FORWARD_TARGET"}, "default rebase apply only advances target when target is an ancestor of the branch");
                        continue;
                    }
                    integrate = GitCapture(repoPath, {"merge", "--ff-only", branch});
                }
                if (strategy != "cherry-pick" && integrate.exitCode != 0) {
                    AppendBranchBlocked(result, repoId, branch, {"BRANCH_INTEGRATION_FAILED"}, CombinedGitError(integrate));
                    continue;
                }

                auto remote = repo->remote.empty() ? std::string{"origin"} : repo->remote;
                std::string upstreamRemote;
                std::string upstreamBranch;
                if (SplitRemoteTrackingRef(UpstreamForBranch(repoPath, targetBranch), upstreamRemote, upstreamBranch)) {
                    remote = upstreamRemote;
                }
                const auto push = GitCapture(repoPath, {"push", remote, targetBranch});
                if (push.exitCode != 0) {
                    AppendBranchBlocked(result, repoId, branch, {"TARGET_PUSH_FAILED"}, CombinedGitError(push));
                    continue;
                }
                result["mutationPerformed"] = true;
                AppendBranchAction(result, "applied", repoId, branch, strategy == "merge" ? "merge" : (strategy == "cherry-pick" ? "cherry-pick" : "fast-forward"), "target branch integrated and pushed");
            }
        }

        suppressCommandLogs.reset();
        if (jsonOutput) {
            std::cout << result.dump(2) << "\n";
        } else {
            PrintBranchActionResultText("Converge Branches Apply", result);
        }
        return BranchActionResultHasBlocked(result) ? 1 : 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}

int RunBranchRetire(const std::filesystem::path& root,
                    int jobs,
                    bool recursive,
                    const std::string& targetBranch,
                    bool confirm,
                    bool removeWorktrees,
                    bool deleteRemote,
                    bool pruneWorktrees,
                    bool harvestDetachedWorktrees,
                    bool harvestBranchWorktrees,
                    const std::string& harvestCommitMessage,
                    const std::string& branchFilter,
                    bool syncTarget,
                    bool emitJson) {
    try {
        if (targetBranch.empty()) {
            std::cerr << "Error: --target must not be empty\n";
            return 2;
        }
        const bool jsonOutput = emitJson || IsConvergeAgentModeEnabled();
        auto result = MakeBranchActionResult("kog.convergeBranchesRetireResult", targetBranch, "retire", recursive, confirm);
        result["removeWorktrees"] = removeWorktrees;
        result["deleteRemote"] = deleteRemote;
        result["pruneWorktrees"] = pruneWorktrees;
        result["harvestDetachedWorktrees"] = harvestDetachedWorktrees;
        result["harvestBranchWorktrees"] = harvestBranchWorktrees;
        result["branchFilter"] = branchFilter;

        std::optional<shell::ScopedConsoleWriteSuppression> suppressCommandLogs;
        if (jsonOutput) {
            suppressCommandLogs.emplace();
        }

        auto snapshot = LoadConvergeSnapshot(root, jobs, false, recursive);
        if (confirm && syncTarget) {
            for (const auto& repoId : BranchPlanTraversalOrder(snapshot)) {
                const auto* repo = FindRepo(snapshot, repoId);
                if (repo == nullptr) {
                    continue;
                }
                (void)SyncTargetBranch(RepoPathForBranchPlan(root, *repo), repo->id, targetBranch, result, true);
            }
            if (!BranchActionResultHasBlocked(result)) {
                snapshot = LoadConvergeSnapshot(root, jobs, false, recursive);
            }
        }

        const auto plan = BuildBranchPlanJson(snapshot, root, targetBranch, "cherry-pick", recursive, true, true);
        for (const auto& repoJson : plan.value("repos", nlohmann::json::array())) {
            const auto repoId = repoJson.value("id", std::string{});
            const auto* repo = FindRepo(snapshot, repoId);
            if (repo == nullptr) {
                continue;
            }
            const auto repoPath = RepoPathForBranchPlan(root, *repo);
            const auto targetRef = ResolveTargetRef(repoPath, targetBranch, repo->remote);
            if (pruneWorktrees) {
                if (!confirm) {
                    result["planned"].push_back({
                        {"repo", repoId},
                        {"branch", targetBranch},
                        {"action", "worktree-prune"},
                        {"message", "rerun with --confirm to prune stale worktree metadata"},
                    });
                } else {
                    const auto prune = GitCapture(repoPath, {"worktree", "prune", "--verbose"});
                    if (prune.exitCode != 0) {
                        AppendBranchBlocked(result, repoId, targetBranch, {"WORKTREE_PRUNE_FAILED"}, CombinedGitError(prune));
                    } else {
                        const auto output = Trim(prune.stdoutStr.empty() ? prune.stderrStr : prune.stdoutStr);
                        result["pruned"].push_back({
                            {"repo", repoId},
                            {"branch", targetBranch},
                            {"action", "worktree-prune"},
                            {"message", output.empty() ? "stale worktree metadata checked" : output},
                        });
                        if (!output.empty()) {
                            result["mutationPerformed"] = true;
                        }
                    }
                }
            }
            const auto liveWorktrees = Worktrees(repoPath, root);
            for (const auto& branchJson : repoJson.value("branches", nlohmann::json::array())) {
                const auto branch = branchJson.value("name", std::string{});
                const auto remoteOnly = branchJson.value("remoteOnly", false);
                if (branch.empty() || branchJson.value("isTarget", false)) {
                    continue;
                }
                if (!BranchMatchesFilter(branchJson, branchFilter)) {
                    continue;
                }
                if (branchJson.value("nonConvergeTarget", false)) {
                    AppendBranchAction(result, "skipped", repoId, branch, "non-converge-target", branchJson.value("skipReason", std::string{"skipped branch"}));
                    continue;
                }
                const auto branchWorktrees = remoteOnly ? std::vector<BranchWorktree>{} : WorktreesForBranch(liveWorktrees, branch);
                if (!branchJson.value("integratedIntoTarget", branchJson.value("mergedIntoTarget", false))) {
                    bool dirtyUnprovenWorktree = false;
                    for (const auto& worktree : branchWorktrees) {
                        std::string cleanMessage;
                        if (WorktreeIsClean(worktree.absolutePath, cleanMessage)) {
                            continue;
                        }
                        dirtyUnprovenWorktree = true;
                        std::string dirtyError;
                        const auto changedPaths = DirtyEntryPaths(CollectDirtyEntries(worktree.absolutePath, &dirtyError));
                        const auto integrationCommand =
                            "kog converge branches apply --target " + targetBranch +
                            " --strategy cherry-pick --branch " + branch + " --confirm";
                        AppendWorktreeHarvestBlocked(result,
                                                     repoId,
                                                     worktree,
                                                     "integrate-dirty-branch-worktree",
                                                     {"DIRTY_BRANCH_WORKTREE_NOT_INTEGRATED"},
                                                     dirtyError.empty()
                                                         ? "commit the reported changed paths in the branch worktree before integrating it into the target"
                                                         : dirtyError,
                                                     std::string{},
                                                     changedPaths,
                                                     integrationCommand);
                    }
                    if (!dirtyUnprovenWorktree) {
                        AppendBranchAction(result, "skipped", repoId, branch, "not-integrated", "branch is not proven integrated into target");
                    }
                    continue;
                }
                auto blockers = RetireBlockersForIntegratedBranch(
                    BranchBlockers(branchJson), removeWorktrees, deleteRemote, harvestBranchWorktrees);
                if (!blockers.empty()) {
                    AppendBranchBlocked(result, repoId, branch, blockers, "planner blockers must be resolved before retirement");
                    continue;
                }
                if (remoteOnly && !deleteRemote) {
                    AppendBranchBlocked(result, repoId, branch, {"REMOTE_DELETE_REQUIRED"}, "remote-only branch retirement requires explicit --delete-remote");
                    continue;
                }

                if (!branchWorktrees.empty() && !removeWorktrees) {
                    AppendBranchBlocked(result, repoId, branch, {"ACTIVE_WORKTREE_LEASE"}, "rerun with --remove-worktrees after reviewing clean Git-managed worktrees");
                    continue;
                }

                std::vector<std::string> worktreeLocations;
                std::unordered_set<std::string> harvestedWorktrees;
                bool hasDirtyWorktrees = false;
                bool worktreesSafe = true;
                for (const auto& worktree : branchWorktrees) {
                    worktreeLocations.push_back(worktree.location);
                    if (SamePath(worktree.absolutePath, repoPath)) {
                        AppendBranchBlocked(result, repoId, branch, {"PRIMARY_WORKTREE_REMOVE_REFUSED"}, "refusing to remove the primary repository worktree");
                        worktreesSafe = false;
                        continue;
                    }
                    std::string cleanMessage;
                    if (!WorktreeIsClean(worktree.absolutePath, cleanMessage)) {
                        hasDirtyWorktrees = true;
                        std::string dirtyError;
                        const auto changedPaths = DirtyEntryPaths(CollectDirtyEntries(worktree.absolutePath, &dirtyError));
                        const auto recoveryCommand = BranchWorktreeHarvestCommand(targetBranch, branch);
                        if (!harvestBranchWorktrees) {
                            AppendWorktreeHarvestBlocked(result,
                                                         repoId,
                                                         worktree,
                                                         "harvest-branch-worktree",
                                                         {"DIRTY_WORKTREE_LEASE"},
                                                         dirtyError.empty() ? cleanMessage : dirtyError,
                                                         branchJson.value("integrationProof", std::string{}),
                                                         changedPaths,
                                                         recoveryCommand);
                            worktreesSafe = false;
                            continue;
                        }
                        if (!confirm) {
                            continue;
                        }
                        if (!HarvestDirtyWorktree(repoPath,
                                                  *repo,
                                                  targetBranch,
                                                  worktree,
                                                  branchJson.value("integrationProof", std::string{}),
                                                  "harvest-branch-worktree",
                                                  "dirty branch worktree harvested into target and removed",
                                                  harvestCommitMessage,
                                                  recoveryCommand,
                                                  result)) {
                            worktreesSafe = false;
                            continue;
                        }
                        harvestedWorktrees.insert(PathKey(worktree.absolutePath));
                    }
                }
                if (!worktreesSafe) {
                    continue;
                }

                if (!confirm) {
                    result["planned"].push_back({
                        {"repo", repoId},
                        {"branch", branch},
                        {"action", remoteOnly ? "delete-remote" : (hasDirtyWorktrees ? "harvest-and-retire" : "retire")},
                        {"message", remoteOnly
                            ? "rerun with --confirm to delete the remote branch"
                            : (hasDirtyWorktrees
                                ? "rerun with --confirm to patch/apply, commit, push, and retire the dirty branch worktree"
                                : "rerun with --confirm to delete the local branch")},
                        {"remoteOnly", remoteOnly},
                        {"remote", branchJson.value("remote", std::string{})},
                        {"remoteBranch", branchJson.value("remoteBranch", std::string{})},
                        {"worktrees", worktreeLocations},
                        {"recoveryCommand", hasDirtyWorktrees ? BranchWorktreeHarvestCommand(targetBranch, branch) : std::string{}},
                    });
                    continue;
                }

                if (!remoteOnly && !CheckoutTargetBranch(repoPath, repoId, targetBranch, result)) {
                    continue;
                }

                for (const auto& worktree : branchWorktrees) {
                    if (harvestedWorktrees.contains(PathKey(worktree.absolutePath))) {
                        continue;
                    }
                    const auto remove = GitCapture(repoPath, {"worktree", "remove", worktree.absolutePath.string()});
                    if (remove.exitCode != 0) {
                        AppendBranchBlocked(result, repoId, branch, {"WORKTREE_REMOVE_FAILED"}, CombinedGitError(remove));
                        worktreesSafe = false;
                        break;
                    }
                }
                if (!worktreesSafe) {
                    continue;
                }

                std::string upstream;
                std::string trackedRemote;
                std::string trackedRemoteBranch;
                bool deleteTrackedRemote = false;
                std::string remoteDeleteStatus = deleteRemote ? "not-configured" : "not-requested";
                if (!remoteOnly) {
                    upstream = UpstreamForBranch(repoPath, branch);
                    if (deleteRemote && !upstream.empty()) {
                        if (SplitRemoteTrackingRef(upstream, trackedRemote, trackedRemoteBranch)) {
                            if (trackedRemoteBranch == targetBranch) {
                                remoteDeleteStatus = "skipped-target-branch";
                            } else if (trackedRemoteBranch != branch) {
                                remoteDeleteStatus = "skipped-mismatched-upstream";
                            } else {
                                deleteTrackedRemote = true;
                                remoteDeleteStatus = "pending";
                            }
                        } else {
                            remoteDeleteStatus = "skipped-invalid-upstream";
                        }
                    } else if (deleteRemote) {
                        const auto sameNameRemote = repo->remote.empty() ? std::string{"origin"} : repo->remote;
                        const auto sameNameRef = "refs/remotes/" + sameNameRemote + "/" + branch;
                        if (GitCapture(repoPath, {"show-ref", "--verify", "--quiet", sameNameRef}).exitCode == 0) {
                            trackedRemote = sameNameRemote;
                            trackedRemoteBranch = branch;
                            deleteTrackedRemote = true;
                            remoteDeleteStatus = "pending-same-name";
                        }
                    }
                    auto deleteLocal = GitCapture(repoPath, {"branch", "-d", branch});
                    if (deleteLocal.exitCode != 0) {
                        deleteLocal = GitCapture(repoPath, {"branch", "-D", branch});
                    }
                    if (deleteLocal.exitCode != 0) {
                        AppendBranchBlocked(result, repoId, branch, {"LOCAL_BRANCH_DELETE_FAILED"}, CombinedGitError(deleteLocal));
                        continue;
                    }
                }

                if (deleteRemote && remoteOnly) {
                    const auto remote = branchJson.value("remote", std::string{});
                    const auto remoteBranch = branchJson.value("remoteBranch", std::string{});
                    if (remote.empty() || remoteBranch.empty()) {
                        AppendBranchBlocked(result, repoId, branch, {"REMOTE_BRANCH_DELETE_FAILED"}, "remote-only branch is missing remote metadata");
                        continue;
                    }
                    if (remoteBranch == targetBranch) {
                        AppendBranchBlocked(result, repoId, branch, {"REMOTE_TARGET_DELETE_FORBIDDEN"}, "remote target branch retirement is forbidden");
                        continue;
                    }
                    const auto deleteUpstream = GitCapture(repoPath, {"push", remote, "--delete", remoteBranch});
                    if (deleteUpstream.exitCode != 0) {
                        AppendBranchBlocked(result, repoId, branch, {"REMOTE_BRANCH_DELETE_FAILED"}, CombinedGitError(deleteUpstream));
                        continue;
                    }
                    remoteDeleteStatus = "deleted";
                } else if (deleteTrackedRemote) {
                    const auto deleteUpstream = GitCapture(repoPath, {"push", trackedRemote, "--delete", trackedRemoteBranch});
                    if (deleteUpstream.exitCode != 0) {
                        AppendBranchBlocked(result, repoId, branch, {"REMOTE_BRANCH_DELETE_FAILED"}, CombinedGitError(deleteUpstream));
                        continue;
                    }
                    remoteDeleteStatus = "deleted";
                }

                const auto retiredAction = remoteOnly
                    ? "delete-remote"
                    : remoteDeleteStatus == "deleted"
                        ? "delete-local-and-remote"
                        : deleteRemote && remoteDeleteStatus.starts_with("skipped-")
                            ? "delete-local-remote-skipped"
                            : "delete-local";
                const auto retiredMessage = remoteDeleteStatus.starts_with("skipped-")
                    ? "integrated branch retired locally; remote deletion skipped by identity guard"
                    : "integrated branch retired";
                result["mutationPerformed"] = true;
                result["retired"].push_back({
                    {"repo", repoId},
                    {"branch", branch},
                    {"action", retiredAction},
                    {"message", retiredMessage},
                    {"remoteOnly", remoteOnly},
                    {"remote", remoteOnly ? branchJson.value("remote", std::string{}) : trackedRemote},
                    {"remoteBranch", remoteOnly ? branchJson.value("remoteBranch", std::string{}) : trackedRemoteBranch},
                    {"remoteDeleteStatus", remoteDeleteStatus},
                    {"integrationProof", branchJson.value("integrationProof", std::string{})},
                    {"worktrees", worktreeLocations},
                });
            }

            for (const auto& worktree : liveWorktrees) {
                if (!branchFilter.empty()) {
                    continue;
                }
                if (!worktree.detached || worktree.bare) {
                    continue;
                }
                const auto evaluation = EvaluateDetachedWorktree(repoPath, targetBranch, targetRef, worktree);
                if (!evaluation.clean && !evaluation.integrated) {
                    if (harvestDetachedWorktrees) {
                        AppendDetachedWorktreeHarvestBlocked(result,
                                                             repoId,
                                                             worktree,
                                                             {"DIRTY_DETACHED_WORKTREE_NON_ANCESTOR"},
                                                             "dirty detached worktree HEAD is not proven integrated into target; harvest is refused",
                                                             evaluation.integrationProof);
                    } else {
                        AppendDetachedWorktreePreserved(result,
                                                        repoId,
                                                        worktree,
                                                        {"DIRTY_DETACHED_WORKTREE_NON_ANCESTOR"},
                                                        "unrelated dirty detached worktree is preserved; it does not prevent eligible branch retirement",
                                                        evaluation.integrationProof);
                    }
                    continue;
                }
                if (!evaluation.integrated) {
                    AppendDetachedWorktreeAction(result,
                                                 "skipped",
                                                 repoId,
                                                 worktree,
                                                 "not-integrated",
                                                 "detached worktree HEAD is not proven integrated into target",
                                                 evaluation.integrationProof);
                    continue;
                }
                if (!evaluation.clean && harvestDetachedWorktrees) {
                    if (!removeWorktrees) {
                        AppendDetachedWorktreeHarvestBlocked(result,
                                                             repoId,
                                                             worktree,
                                                             {"DETACHED_WORKTREE_REMOVE_REQUIRED"},
                                                             "dirty detached harvest requires --remove-worktrees so the harvested worktree can be retired after push",
                                                             evaluation.integrationProof);
                        continue;
                    }
                    if (!confirm) {
                        std::string dirtyError;
                        const auto changedPaths = DirtyEntryPaths(CollectDirtyEntries(worktree.absolutePath, &dirtyError));
                        result["planned"].push_back({
                            {"repo", repoId},
                            {"branch", DetachedWorktreeBranchLabel(worktree)},
                            {"action", "harvest-detached-worktree"},
                            {"message", dirtyError.empty() ? "rerun with --confirm to patch/apply, commit, push, and remove the detached worktree" : dirtyError},
                            {"worktree", worktree.location},
                            {"head", worktree.head},
                            {"integrationProof", evaluation.integrationProof},
                            {"changedPaths", changedPaths},
                        });
                        continue;
                    }
                    (void)HarvestDirtyWorktree(repoPath,
                                               *repo,
                                               targetBranch,
                                               worktree,
                                               evaluation.integrationProof,
                                               "harvest-detached-worktree",
                                               "dirty detached worktree harvested into target and removed",
                                               harvestCommitMessage,
                                               std::string{},
                                               result);
                    continue;
                }
                if (!evaluation.clean || evaluation.primary) {
                    AppendDetachedWorktreeBlocked(result,
                                                  repoId,
                                                  worktree,
                                                  evaluation.blockers,
                                                  evaluation.clean ? "detached worktree cannot be safely removed" : evaluation.cleanMessage,
                                                  evaluation.integrationProof);
                    continue;
                }
                if (!removeWorktrees) {
                    AppendDetachedWorktreeBlocked(result,
                                                  repoId,
                                                  worktree,
                                                  {"DETACHED_WORKTREE_REMOVE_REQUIRED"},
                                                  "rerun with --remove-worktrees after reviewing clean Git-managed detached worktrees",
                                                  evaluation.integrationProof);
                    continue;
                }
                if (!confirm) {
                    result["planned"].push_back({
                        {"repo", repoId},
                        {"branch", DetachedWorktreeBranchLabel(worktree)},
                        {"action", "remove-detached-worktree"},
                        {"message", "rerun with --confirm to remove the detached worktree"},
                        {"worktree", worktree.location},
                        {"head", worktree.head},
                        {"integrationProof", evaluation.integrationProof},
                    });
                    continue;
                }

                const auto remove = GitCapture(repoPath, {"worktree", "remove", worktree.absolutePath.string()});
                if (remove.exitCode != 0) {
                    AppendDetachedWorktreeBlocked(result,
                                                  repoId,
                                                  worktree,
                                                  {"WORKTREE_REMOVE_FAILED"},
                                                  CombinedGitError(remove),
                                                  evaluation.integrationProof);
                    continue;
                }

                result["mutationPerformed"] = true;
                result["retired"].push_back({
                    {"repo", repoId},
                    {"branch", DetachedWorktreeBranchLabel(worktree)},
                    {"action", "remove-detached-worktree"},
                    {"message", "integrated detached worktree removed"},
                    {"worktree", worktree.location},
                    {"head", worktree.head},
                    {"integrationProof", evaluation.integrationProof},
                });
            }
        }

        const bool blocked = BranchActionResultHasBlocked(result);
        const bool preserved = !result["preserved"].empty();
        result["requestedClosureComplete"] = !blocked;
        result["status"] = blocked
            ? "blocked"
            : preserved
                ? (confirm ? "success_with_preserved_blockers" : "preview_with_preserved_blockers")
                : (confirm ? "success" : "preview");

        suppressCommandLogs.reset();
        if (jsonOutput) {
            std::cout << result.dump(2) << "\n";
        } else {
            PrintBranchActionResultText("Converge Branches Retire", result);
        }
        return blocked ? 1 : 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}

int RunBranchPlanner(const std::filesystem::path& root,
                     int jobs,
                     bool recursive,
                     const std::string& targetBranch,
                     const std::string& strategy,
                     bool emitJson) {
    const bool jsonOutput = emitJson || IsConvergeAgentModeEnabled();
    try {
        if (targetBranch.empty()) {
            std::cerr << "Error: --target must not be empty\n";
            return 2;
        }
        if (!IsBranchIntegrationStrategy(strategy)) {
            std::cerr << "Error: --strategy must be rebase, merge, or cherry-pick\n";
            return 2;
        }
        std::optional<shell::ScopedConsoleWriteSuppression> suppressCommandLogs;
        if (jsonOutput) {
            suppressCommandLogs.emplace();
        }
        const auto snapshot = LoadConvergeSnapshot(
            root, jobs, false, recursive, false, ResolveBranchStatusTimeoutMs());
        const auto plan = BuildBranchPlanJson(snapshot, root, targetBranch, strategy, recursive, false, false);
        suppressCommandLogs.reset();
        if (jsonOutput) {
            std::cout << plan.dump(2) << "\n";
        } else {
            PrintBranchPlanText(plan);
        }
        return BranchPlanHasBlockers(plan) ? 1 : 0;
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (jsonOutput) {
            const bool timedOut = message.find("[kog-timeout]") != std::string::npos ||
                                  message.find("timeout") != std::string::npos;
            const auto blocker = timedOut ? "BRANCH_STATUS_SNAPSHOT_TIMEOUT" : "BRANCH_STATUS_SNAPSHOT_FAILED";
            const auto failure = nlohmann::json{
                {"schemaName", "kog.convergeBranchesPlan"},
                {"schemaVersion", 1},
                {"mutationPerformed", false},
                {"workspaceRoot", root.generic_string()},
                {"targetBranch", targetBranch},
                {"strategy", strategy},
                {"recursive", recursive},
                {"planningTimedOut", timedOut},
                {"planningFailed", true},
                {"planningBlockers", nlohmann::json::array({blocker})},
                {"planningDiagnostics", nlohmann::json::array({{
                    {"repo", "."},
                    {"code", blocker},
                    {"operation", "recursive status snapshot"},
                    {"message", message},
                    {"timeoutMs", ResolveBranchStatusTimeoutMs()},
                }})},
                {"traversalOrder", nlohmann::json::array()},
                {"repos", nlohmann::json::array()},
            };
            std::cout << failure.dump(2) << "\n";
        } else {
            std::cerr << "Error: " << message << "\n";
        }
        return 1;
    }
}

} // namespace

void RegisterConverge(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("converge", "Converge repo state or branch state with explicit planners");

    auto* noRecursive = new bool{false};
    auto* dryRun = new bool{false};
    auto* statusOnly = new bool{false};
    auto* resume = new bool{false};
    auto* abort = new bool{false};
    auto* profile = new bool{false};
    auto* verbose = new bool{false};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* aiCompat = new bool{false};
    auto* unregisteredScan = new bool{false};
    auto* settleWorktrees = new bool{false};
    auto* settleRemoveWorktrees = new bool{false};
    auto* settlePruneWorktrees = new bool{false};
    auto* settleHarvestDetachedWorktrees = new bool{false};
    auto* settleHarvestBranchWorktrees = new bool{false};
    auto* jobs = new int{1};
    auto* remote = new std::string{};
    auto* settleTarget = new std::string{"main"};
    auto* settleHarvestMessage = new std::string{kWorktreeHarvestDefaultMessage};

    auto* repos = cmd->add_subcommand("repos", "Alias for the existing repo-state converge workflow");
    auto* branches = cmd->add_subcommand("branches", "Plan branch integration and retirement safely");
    branches->fallthrough(false);
    branches->require_subcommand(1);
    auto* branchesPlan = branches->add_subcommand("plan", "Read-only branch convergence planner");
    branchesPlan->fallthrough(false);
    auto* branchesInventory = branches->add_subcommand("inventory", "Read-only branch/worktree divergence inventory");
    branchesInventory->fallthrough(false);
    auto* branchesStatus = branches->add_subcommand("status", "Alias for branch/worktree divergence inventory");
    branchesStatus->fallthrough(false);
    auto* branchesApply = branches->add_subcommand("apply", "Guarded branch integration into the target branch");
    branchesApply->fallthrough(false);
    auto* branchesRetire = branches->add_subcommand("retire", "Guarded harvest and retirement for integrated branches and Git worktrees");
    branchesRetire->fallthrough(false);
    auto* branchesNoRecursive = new bool{false};
    auto* branchesJson = new bool{false};
    auto* branchesJobs = new int{1};
    auto* branchesTarget = new std::string{"main"};
    auto* branchesStrategy = new std::string{"rebase"};
    auto* branchesApplyBranch = new std::string{};
    auto* branchesRetireBranch = new std::string{};
    auto* branchesConfirm = new bool{false};
    auto* branchesNoSyncTarget = new bool{false};
    auto* branchesRemoveWorktrees = new bool{false};
    auto* branchesDeleteRemote = new bool{false};
    auto* branchesPruneWorktrees = new bool{false};
    auto* branchesHarvestDetachedWorktrees = new bool{false};
    auto* branchesHarvestBranchWorktrees = new bool{false};
    auto* branchesHarvestMessage = new std::string{kWorktreeHarvestDefaultMessage};

    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only run on current repository");
    cmd->add_flag("--dry-run", *dryRun, "Preview converge actions without changing repositories");
    cmd->add_flag("--status", *statusOnly, "Show current converge state");
    cmd->add_flag("--resume", *resume, "Resume from saved converge phase");
    cmd->add_flag("--abort", *abort, "Abort converge and remove saved state");
    cmd->add_flag("--profile", *profile, "Print push profile summary");
    cmd->add_flag("--verbose", *verbose, "Verbose push output");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Pass --force-with-lease to converge push stage");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to converge push stage");
    cmd->add_flag("--ai", *aiCompat, "Enable converge agent commit compatibility path for intent-scoped commit planning");
    cmd->add_flag("--unregistered-scan", *unregisteredScan, "Opt in to bounded unregistered discovery during dry-run status preflight");
    cmd->add_flag("--settle-worktrees", *settleWorktrees, "Run guarded branch/worktree settle before repo converge passes");
    cmd->add_flag("--remove-worktrees", *settleRemoveWorktrees, "Allow settle to remove clean integrated Git worktrees");
    cmd->add_flag("--prune-worktrees", *settlePruneWorktrees, "Allow settle to prune stale Git worktree metadata");
    cmd->add_flag("--harvest-detached-worktrees", *settleHarvestDetachedWorktrees, "Allow settle to patch/apply dirty detached ancestor worktrees into the target branch");
    cmd->add_flag("--harvest-branch-worktrees", *settleHarvestBranchWorktrees, "Allow settle to patch/apply dirty integrated branch worktrees into the target branch");
    cmd->add_option("--jobs", *jobs, "Parallel workers for converge status/push stages");
    cmd->add_option("--remote", *remote, "Optional remote filter for converge push stage");
    cmd->add_option("--target", *settleTarget, "Target branch for worktree settle")->default_str("main");
    cmd->add_option("--harvest-message", *settleHarvestMessage, "Commit message for dirty worktree harvest")->default_str(kWorktreeHarvestDefaultMessage);

    repos->add_flag("--no-recursive,-N", *noRecursive, "Only run on current repository");
    repos->add_flag("--dry-run", *dryRun, "Preview converge actions without changing repositories");
    repos->add_flag("--status", *statusOnly, "Show current converge state");
    repos->add_flag("--resume", *resume, "Resume from saved converge phase");
    repos->add_flag("--abort", *abort, "Abort converge and remove saved state");
    repos->add_flag("--profile", *profile, "Print push profile summary");
    repos->add_flag("--verbose", *verbose, "Verbose push output");
    repos->add_flag("--force-with-lease", *forceWithLease, "Pass --force-with-lease to converge push stage");
    repos->add_flag("--no-verify", *noVerify, "Pass --no-verify to converge push stage");
    repos->add_flag("--ai", *aiCompat, "Enable converge agent commit compatibility path for intent-scoped commit planning");
    repos->add_flag("--unregistered-scan", *unregisteredScan, "Opt in to bounded unregistered discovery during dry-run status preflight");
    repos->add_flag("--settle-worktrees", *settleWorktrees, "Run guarded branch/worktree settle before repo converge passes");
    repos->add_flag("--remove-worktrees", *settleRemoveWorktrees, "Allow settle to remove clean integrated Git worktrees");
    repos->add_flag("--prune-worktrees", *settlePruneWorktrees, "Allow settle to prune stale Git worktree metadata");
    repos->add_flag("--harvest-detached-worktrees", *settleHarvestDetachedWorktrees, "Allow settle to patch/apply dirty detached ancestor worktrees into the target branch");
    repos->add_flag("--harvest-branch-worktrees", *settleHarvestBranchWorktrees, "Allow settle to patch/apply dirty integrated branch worktrees into the target branch");
    repos->add_option("--jobs", *jobs, "Parallel workers for converge status/push stages");
    repos->add_option("--remote", *remote, "Optional remote filter for converge push stage");
    repos->add_option("--target", *settleTarget, "Target branch for worktree settle")->default_str("main");
    repos->add_option("--harvest-message", *settleHarvestMessage, "Commit message for dirty worktree harvest")->default_str(kWorktreeHarvestDefaultMessage);

    branchesPlan->add_flag("--no-recursive,-N", *branchesNoRecursive, "Only plan branches for the current repository");
    branchesPlan->add_flag("--json", *branchesJson, "Emit stable machine-readable branch plan JSON");
    branchesPlan->add_option("--jobs", *branchesJobs, "Parallel workers for recursive status snapshot loading");
    branchesPlan->add_option("--target", *branchesTarget, "Target integration branch")->default_str("main");
    branchesPlan->add_option("--strategy", *branchesStrategy, "Integration strategy recorded in the plan: rebase|merge|cherry-pick")->default_str("rebase");

    for (auto* branchReadOnly : {branchesInventory, branchesStatus}) {
        branchReadOnly->add_flag("--no-recursive,-N", *branchesNoRecursive, "Only inspect branches for the current repository");
        branchReadOnly->add_flag("--json", *branchesJson, "Emit stable machine-readable branch inventory JSON");
        branchReadOnly->add_option("--jobs", *branchesJobs, "Parallel workers for recursive status snapshot loading");
        branchReadOnly->add_option("--target", *branchesTarget, "Target integration branch")->default_str("main");
        branchReadOnly->add_option("--strategy", *branchesStrategy, "Integration strategy recorded in the inventory: rebase|merge|cherry-pick")->default_str("rebase");
    }

    branchesApply->add_flag("--no-recursive,-N", *branchesNoRecursive, "Only apply branches for the current repository");
    branchesApply->add_flag("--json", *branchesJson, "Emit stable machine-readable apply result JSON");
    branchesApply->add_flag("--confirm", *branchesConfirm, "Confirm branch integration mutation");
    branchesApply->add_flag("--no-sync-target", *branchesNoSyncTarget, "Do not fetch and fast-forward the target branch before apply");
    branchesApply->add_option("--jobs", *branchesJobs, "Parallel workers for recursive status snapshot loading");
    branchesApply->add_option("--target", *branchesTarget, "Target integration branch")->default_str("main");
    branchesApply->add_option("--strategy", *branchesStrategy, "Integration strategy: rebase|merge|cherry-pick")->default_str("rebase");
    branchesApply->add_option("--branch", *branchesApplyBranch, "Limit apply to one source branch; required for cherry-pick strategy");

    branchesRetire->add_flag("--no-recursive,-N", *branchesNoRecursive, "Only retire branches for the current repository");
    branchesRetire->add_flag("--json", *branchesJson, "Emit stable machine-readable retire result JSON");
    branchesRetire->add_flag("--confirm", *branchesConfirm, "Confirm branch/worktree retirement mutation");
    branchesRetire->add_flag("--no-sync-target", *branchesNoSyncTarget, "Do not fetch and fast-forward the target branch before retire");
    branchesRetire->add_flag("--remove-worktrees", *branchesRemoveWorktrees, "Remove clean Git-managed worktrees for retired branches");
    branchesRetire->add_flag("--delete-remote", *branchesDeleteRemote, "Delete tracked remote branches after local retirement");
    branchesRetire->add_flag("--prune-worktrees", *branchesPruneWorktrees, "Prune stale Git worktree metadata");
    branchesRetire->add_flag("--harvest-detached-worktrees", *branchesHarvestDetachedWorktrees, "Patch/apply dirty detached ancestor worktrees into the target branch before removal");
    branchesRetire->add_flag("--harvest-branch-worktrees", *branchesHarvestBranchWorktrees, "Patch/apply dirty integrated branch worktrees into the target branch before removal");
    branchesRetire->add_option("--jobs", *branchesJobs, "Parallel workers for recursive status snapshot loading");
    branchesRetire->add_option("--target", *branchesTarget, "Target integration branch")->default_str("main");
    branchesRetire->add_option("--branch", *branchesRetireBranch, "Limit retirement or dirty worktree harvest to one branch");
    branchesRetire->add_option("--harvest-message", *branchesHarvestMessage, "Commit message for dirty worktree harvest")->default_str(kWorktreeHarvestDefaultMessage);

    repos->callback([=]() {
        std::vector<std::string> args{"converge"};
        if (*noRecursive) args.push_back("--no-recursive");
        if (*dryRun) args.push_back("--dry-run");
        if (*statusOnly) args.push_back("--status");
        if (*resume) args.push_back("--resume");
        if (*abort) args.push_back("--abort");
        if (*profile) args.push_back("--profile");
        if (*verbose) args.push_back("--verbose");
        if (*forceWithLease) args.push_back("--force-with-lease");
        if (*noVerify) args.push_back("--no-verify");
        if (*aiCompat) args.push_back("--ai");
        if (*unregisteredScan) args.push_back("--unregistered-scan");
        if (*settleWorktrees) args.push_back("--settle-worktrees");
        if (*settleRemoveWorktrees) args.push_back("--remove-worktrees");
        if (*settlePruneWorktrees) args.push_back("--prune-worktrees");
        if (*settleHarvestDetachedWorktrees) args.push_back("--harvest-detached-worktrees");
        if (*settleHarvestBranchWorktrees) args.push_back("--harvest-branch-worktrees");
        args.push_back("--jobs");
        args.push_back(std::to_string(*jobs));
        if (!remote->empty()) {
            args.push_back("--remote");
            args.push_back(*remote);
        }
        if (!Trim(*settleTarget).empty()) {
            args.push_back("--target");
            args.push_back(Trim(*settleTarget));
        }
        if (!Trim(*settleHarvestMessage).empty() && Trim(*settleHarvestMessage) != kWorktreeHarvestDefaultMessage) {
            args.push_back("--harvest-message");
            args.push_back(Trim(*settleHarvestMessage));
        }
        const auto result = shell::ExecuteCommand(SelfBinaryPath(), args, shell::ExecMode::PassThrough, std::filesystem::current_path());
        std::exit(result.exitCode);
    });

    branchesPlan->callback([=]() {
        if (*branchesJobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(2);
        }
        const auto code = RunBranchPlanner(
            std::filesystem::current_path().lexically_normal(),
            *branchesJobs,
            !*branchesNoRecursive,
            Trim(*branchesTarget),
            ToLower(Trim(*branchesStrategy)),
            *branchesJson);
        std::exit(code);
    });

    auto branchInventoryCallback = [=]() {
        if (*branchesJobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(2);
        }
        const auto code = RunBranchInventory(
            std::filesystem::current_path().lexically_normal(),
            *branchesJobs,
            !*branchesNoRecursive,
            Trim(*branchesTarget),
            ToLower(Trim(*branchesStrategy)),
            *branchesJson);
        std::exit(code);
    };
    branchesInventory->callback(branchInventoryCallback);
    branchesStatus->callback(branchInventoryCallback);

    branchesApply->callback([=]() {
        if (*branchesJobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(2);
        }
        const auto code = RunBranchApply(
            std::filesystem::current_path().lexically_normal(),
            *branchesJobs,
            !*branchesNoRecursive,
            Trim(*branchesTarget),
            ToLower(Trim(*branchesStrategy)),
            Trim(*branchesApplyBranch),
            *branchesConfirm,
            !*branchesNoSyncTarget,
            *branchesJson);
        std::exit(code);
    });

    branchesRetire->callback([=]() {
        if (*branchesJobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(2);
        }
        const auto code = RunBranchRetire(
            std::filesystem::current_path().lexically_normal(),
            *branchesJobs,
            !*branchesNoRecursive,
            Trim(*branchesTarget),
            *branchesConfirm,
            *branchesRemoveWorktrees,
            *branchesDeleteRemote,
            *branchesPruneWorktrees,
            *branchesHarvestDetachedWorktrees,
            *branchesHarvestBranchWorktrees,
            Trim(*branchesHarvestMessage),
            Trim(*branchesRetireBranch),
            !*branchesNoSyncTarget,
            *branchesJson);
        std::exit(code);
    });

    cmd->callback([=]() {
        if (!cmd->get_subcommands().empty()) {
            return;
        }
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto recursive = !*noRecursive;
        const auto agentIntentCommitMode = *aiCompat || IsConvergeAgentModeEnabled();
        const auto worktreeSettleRequested =
            *settleWorktrees ||
            *settleRemoveWorktrees ||
            *settlePruneWorktrees ||
            *settleHarvestDetachedWorktrees ||
            *settleHarvestBranchWorktrees;
        auto phases = kConvergePhases;
        if (worktreeSettleRequested) {
            const auto finalIt = std::find(phases.begin(), phases.end(), "final-status-summary");
            phases.insert(finalIt == phases.end() ? phases.end() : finalIt, "settle-worktrees");
        }
        const auto statePath = ConvergeStatePath(workspaceRoot);

        const int controlFlags = (*statusOnly ? 1 : 0) + (*resume ? 1 : 0) + (*abort ? 1 : 0);
        if (controlFlags > 1) { std::cerr << "Error: --status, --resume, and --abort are mutually exclusive\n"; std::exit(2); }
        if (*jobs < 1) { std::cerr << "Error: --jobs must be a positive integer\n"; std::exit(2); }
        if (worktreeSettleRequested && Trim(*settleTarget).empty()) { std::cerr << "Error: --target must not be empty when worktree settle is requested\n"; std::exit(2); }

        if (*statusOnly) {
            WorkflowState state;
            if (!ReadState(statePath, state)) { std::cout << "converge state: none\n"; std::exit(0); }
            std::cout << "converge state file: " << statePath.generic_string() << "\n";
            std::cout << "workflow=" << state.workflow << "\n";
            std::cout << "currentPhase=" << state.currentPhase << "\n";
            std::cout << "completedPhases=" << Csv(state.completedPhases) << "\n";
            std::cout << "succeeded=" << state.succeededRepos.size() << " failed=" << state.failedRepos.size() << " blocked=" << state.blockedRepoSet.size() << " skipped=" << state.skippedRepos.size() << " pending=" << state.pendingRepos.size() << "\n";
            std::cout << "repoGraphFingerprint=" << state.repoGraphFingerprint << "\n";
            std::cout << "resumePossible=" << (!state.currentPhase.empty() ? "yes" : "no") << "\n";
            std::cout << "suggestedNextAction=" << (state.resumeCommand.empty() ? "kog converge" : state.resumeCommand) << "\n";
            if (!state.blockedReason.empty()) std::cout << "blockedReason=" << state.blockedReason << "\n";
            if (!state.blockedRepos.empty()) std::cout << "blockedRepos=" << Csv(state.blockedRepos) << "\n";
            if (!state.resumeCommand.empty()) std::cout << "resumeCommand=" << state.resumeCommand << "\n";
            PrintStatusPhaseSummary(state);
            std::exit(0);
        }
        if (*abort) {
            DeleteState(statePath);
            std::cout << "converge state removed: " << statePath.generic_string() << "\n";
            std::exit(0);
        }
        if (*dryRun) {
            if (worktreeSettleRequested) {
                const auto settleCode = RunBranchRetire(
                    workspaceRoot,
                    *jobs,
                    recursive,
                    Trim(*settleTarget),
                    false,
                    *settleRemoveWorktrees,
                    false,
                    *settlePruneWorktrees,
                    *settleHarvestDetachedWorktrees,
                    *settleHarvestBranchWorktrees,
                    Trim(*settleHarvestMessage),
                    std::string{},
                    true,
                    false);
                if (settleCode != 0) {
                    std::exit(settleCode);
                }
            }
            std::exit(RunDryRunPlanner(workspaceRoot, *jobs, *unregisteredScan, recursive));
        }

        WorkflowState state;
        Snapshot snapshot;
        Plan plan;
        std::size_t startPhaseIndex = 0;

        if (*resume) {
            if (!ReadState(statePath, state)) { std::cerr << "Error: no converge state to resume\n"; std::exit(1); }
            if (state.workspaceRoot.lexically_normal() != workspaceRoot) {
                std::cerr << "Error: converge state workspace mismatch; expected " << state.workspaceRoot.generic_string() << " got " << workspaceRoot.generic_string() << "\n";
                std::exit(1);
            }
            if (state.recursive != recursive) {
                std::cerr << "Error: converge resume refused due to recursive-mode mismatch\n";
                std::exit(1);
            }
            if (state.dryRunRequested) {
                std::cerr << "Error: converge resume refused because saved state was created from dry-run mode\n";
                std::exit(1);
            }
            try {
                snapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive, true);
            } catch (const std::exception& ex) {
                std::cerr << "Error: failed to load status snapshot during resume: " << ex.what() << "\n";
                std::exit(1);
            }
            const auto resumeFingerprint = SnapshotFingerprint(snapshot);
            if (!state.repoGraphFingerprint.empty() && state.repoGraphFingerprint != resumeFingerprint) {
                std::cerr << "Error: converge resume refused due to changed repo graph fingerprint\n";
                std::cerr << "saved=" << state.repoGraphFingerprint << " current=" << resumeFingerprint << "\n";
                std::exit(1);
            }
            plan = BuildPlan(snapshot);
            const bool restoredResumeSync = RestoreSavedResumeTransportPlan(state, snapshot, plan);
            const auto savedBaselines = BaselineByRepo(state.repoBaselines);
            const auto currentBaselines = BaselineByRepo(RepoBaselines(snapshot));
            for (const auto& repo : state.succeededRepos) {
                const auto savedIt = savedBaselines.find(repo);
                const auto currentIt = currentBaselines.find(repo);
                const bool baselineChanged = savedIt == savedBaselines.end() ||
                    currentIt == currentBaselines.end() ||
                    savedIt->second != currentIt->second;
                if (baselineChanged && !PlanReferencesRepo(plan, repo)) {
                    std::cerr << "Error: converge resume refused because successful repo baseline changed: " << repo << "\n";
                    std::exit(1);
                }
            }
            state.repoGraphFingerprint = resumeFingerprint;
            if (state.currentPhase == "settle-worktrees" && !worktreeSettleRequested) {
                std::cerr << "Error: converge resume for settle-worktrees requires the original worktree settle flags\n";
                std::exit(2);
            }
            if (state.currentPhase == "settle-worktrees" && std::find(phases.begin(), phases.end(), "settle-worktrees") == phases.end()) {
                const auto finalIt = std::find(phases.begin(), phases.end(), "final-status-summary");
                phases.insert(finalIt == phases.end() ? phases.end() : finalIt, "settle-worktrees");
            }
            const auto phaseIt = std::find(phases.begin(), phases.end(), state.currentPhase);
            startPhaseIndex = phaseIt == phases.end() ? 0 : static_cast<std::size_t>(std::distance(phases.begin(), phaseIt));
            const auto syncPhaseIt = std::find(phases.begin(), phases.end(), "sync-before-push");
            const auto pushPhaseIt = std::find(phases.begin(), phases.end(), "push-nested-bottom-up");
            if (restoredResumeSync &&
                syncPhaseIt != phases.end() &&
                pushPhaseIt != phases.end() &&
                startPhaseIndex >= static_cast<std::size_t>(std::distance(phases.begin(), pushPhaseIt))) {
                state.completedPhases.erase(
                    std::remove(state.completedPhases.begin(), state.completedPhases.end(), "sync-before-push"),
                    state.completedPhases.end());
                state.currentPhase = "sync-before-push";
                startPhaseIndex = static_cast<std::size_t>(std::distance(phases.begin(), syncPhaseIt));
                std::cout << "[converge] resume_rewind=sync-before-push reason=saved_sync_repo_still_behind\n";
            }
            std::cout << "Resuming converge from phase: " << state.currentPhase << "\n";
        } else {
            try {
                snapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive, true);
            } catch (const std::exception& ex) {
                std::cerr << "Error: failed to load status snapshot: " << ex.what() << "\n";
                std::exit(1);
            }
            plan = BuildPlan(snapshot);
            state.workflow = "converge";
            state.schemaName = "kog.convergeWorkflowState";
            state.schemaVersion = 1;
            state.workspaceRoot = workspaceRoot;
            state.startedAt = UtcNowIso8601();
            state.recursive = recursive;
            state.dryRunRequested = *dryRun;
            state.resumeCommand = recursive ? "kog converge --resume" : "kog converge --resume --no-recursive";
            ResetStateForPlan(state, snapshot, plan);
        }

        if (!WriteState(statePath, state)) {
            std::cerr << "Error: failed to write converge state file: " << statePath.generic_string() << "\n";
            std::exit(1);
        }

        auto persist = [&]() {
            if (!WriteState(statePath, state)) {
                std::cerr << "Error: failed to persist converge state file: " << statePath.generic_string() << "\n";
                std::exit(1);
            }
        };

        const auto convergeSyncTimeoutMs = ResolveConvergeSyncTimeoutMs();

        int passIndex = 0;
        while (true) {
            bool runAnotherPass = false;
            Snapshot nextSnapshot;
            Plan nextPlan;
            if (passIndex > 0) {
                std::cout << "[converge] pass=" << (passIndex + 1) << "\n";
            }

            for (std::size_t idx = startPhaseIndex; idx < phases.size(); ++idx) {
                const auto& phase = phases[idx];
                if (PhaseAlreadyCompleted(state, phase)) continue;

                state.currentPhase = phase;
                persist();
                std::cout << "[converge] phase=" << phase << "\n";
                const auto phaseStartedAt = std::chrono::steady_clock::now();

                PhaseSummary summary;
                summary.pending = state.pendingRepos;

                if (phase == "status-preflight-plan") {
                PrintPlan(snapshot, plan, false);
                if (worktreeSettleRequested) {
                    std::string previewCommand = "kog converge branches retire --target " + Trim(*settleTarget);
                    if (*settleRemoveWorktrees) {
                        previewCommand += " --remove-worktrees";
                    }
                    if (*settleHarvestBranchWorktrees) {
                        previewCommand += " --harvest-branch-worktrees";
                    }
                    if (*settleHarvestDetachedWorktrees) {
                        previewCommand += " --harvest-detached-worktrees";
                    }
                    state.commandLinesUsed[phase].push_back(std::move(previewCommand));
                    const auto previewCode = RunBranchRetire(
                        workspaceRoot,
                        *jobs,
                        recursive,
                        Trim(*settleTarget),
                        false,
                        *settleRemoveWorktrees,
                        false,
                        *settlePruneWorktrees,
                        *settleHarvestDetachedWorktrees,
                        *settleHarvestBranchWorktrees,
                        Trim(*settleHarvestMessage),
                        std::string{},
                        false,
                        false);
                    if (previewCode != 0) {
                        summary.blocked.push_back(".");
                        summary.failureCategory["."] = "WORKTREE_SETTLE_PREFLIGHT_BLOCKED";
                        summary.failureMessage["."] = "worktree settle preflight found blocked branch/worktree state";
                        summary.retryEligible["."] = true;
                    }
                }
                summary.skipped = UniqueRepos(plan.skipped);
                MergeUnique(summary.blocked, UniqueRepos(plan.blocked));
                if (!summary.blocked.empty()) {
                    state.blockedReason = "status preflight detected blocked repositories";
                    state.blockedRepos = summary.blocked;
                    for (const auto& repo : summary.blocked) {
                        summary.failureCategory[repo] = "BLOCKED_BY_POLICY";
                        summary.failureMessage[repo] = state.blockedReason;
                        summary.retryEligible[repo] = true;
                    }
                }
            } else if (phase == "commit-local-changes-if-needed") {
                for (const auto& line : plan.commit) {
                    if (PhaseSummaryContainsRepo(state, phase, line.repo, &PhaseSummary::succeeded)) {
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    if (PhaseSummaryContainsRepo(state, phase, line.repo, &PhaseSummary::skipped)) {
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    const bool pointerCommit = line.text.find("deterministic pointer commit") != std::string::npos;
                    std::string commandLine = "kog commit -ai --repos " + line.repo;
                    std::string failureCategory;
                    std::string failureMessage;
                    int code = 0;
                    if (agentIntentCommitMode && !pointerCommit) {
                        code = RunIntentCommitPlan(workspaceRoot, snapshot, line.repo, *profile, &commandLine, &failureCategory, &failureMessage);
                    } else {
                        const std::string message = pointerCommit ? (line.text.find(kPointerMultipleMessage) != std::string::npos ? kPointerMultipleMessage : kPointerSingleMessage) : std::string{};
                        code = RunCommitNativeSimple(workspaceRoot, line.repo, true, message, false, false, "", "", false, true, false);
                    }
                    state.commandLinesUsed[phase].push_back(commandLine);
                    if (code == 0) {
                        summary.succeeded.push_back(line.repo);
                    } else {
                        summary.failed.push_back(line.repo);
                        summary.failureCategory[line.repo] = failureCategory.empty() ? "FAILED_COMMIT" : failureCategory;
                        summary.failureMessage[line.repo] = failureMessage.empty() ? "commit-local-changes-if-needed failed" : failureMessage;
                        summary.retryEligible[line.repo] = true;
                    }
                }
                for (const auto& line : plan.skipped) {
                    if (line.text.find("commit skipped") != std::string::npos || line.text.find("kog commit -ai skipped") != std::string::npos || line.text.find("pointer commit skipped") != std::string::npos) {
                        summary.skipped.push_back(line.repo);
                    }
                }
            } else if (phase == "sync-before-push" || phase == "sync-converge-dependent-repos") {
                for (const auto& line : plan.sync) {
                    if (phase == "sync-before-push" && RepoHasPlannedDescendantPush(snapshot, plan, line.repo)) {
                        state.commandLinesUsed[phase].push_back("kog sync deferred until planned descendant push: " + line.repo);
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    const auto repoPath = line.repo == "."
                        ? workspaceRoot
                        : (workspaceRoot / std::filesystem::path(line.repo)).lexically_normal();
                    const auto timeoutText = convergeSyncTimeoutMs.has_value() ? std::to_string(*convergeSyncTimeoutMs) : std::string{"none"};
                    std::cout << "[converge] sync_repo=" << line.repo << " timeout_ms=" << timeoutText << "\n";
                    state.commandLinesUsed[phase].push_back("KOG_CONVERGE_SYNC_TIMEOUT_MS=" + timeoutText + " kog sync origin-latest --repo " + repoPath.generic_string() + " --no-recursive");
                    const auto detailed = RunSyncOriginLatestNativeDetailed(repoPath, false, false, false, true, convergeSyncTimeoutMs, true);
                    PopulatePhaseSummaryFromSingleRepoAggregate(line.repo, detailed.second, false, detailed.first, summary);
                    if (detailed.first != 0 && summary.failed.empty() && summary.blocked.empty()) {
                        summary.failed.push_back(line.repo);
                        summary.failureCategory[line.repo] = "SYNC_ERROR";
                        summary.failureMessage[line.repo] = "sync phase failed";
                        summary.retryEligible[line.repo] = true;
                    }
                }
                } else if (phase == "push-nested-bottom-up" || phase == "push-parents-bottom-up") {
                auto pushRepos = UniqueRepos(plan.push);
                if (phase == "push-nested-bottom-up") {
                    std::erase_if(pushRepos, [&](const auto& repo) { return !IsNestedRepo(snapshot, repo); });
                }
                std::sort(pushRepos.begin(), pushRepos.end(), [&](const auto& lhs, const auto& rhs) {
                    const auto lhsWave = RepoWaveIndex(plan, lhs);
                    const auto rhsWave = RepoWaveIndex(plan, rhs);
                    return lhsWave == rhsWave ? lhs < rhs : lhsWave < rhsWave;
                });
                if (pushRepos.empty()) {
                    state.commandLinesUsed[phase].push_back("kog push skipped: no planned repositories");
                } else {
                    std::unordered_set<std::string> failedOrBlocked;
                    for (const auto& repo : pushRepos) {
                        if (RepoHasDescendantInSet(snapshot, repo, failedOrBlocked)) {
                            summary.blocked.push_back(repo);
                            summary.failureCategory[repo] = "BLOCKED_BY_CHILD_FAILURE";
                            summary.failureMessage[repo] = "planned descendant push failed in the same phase";
                            summary.retryEligible[repo] = true;
                            failedOrBlocked.insert(repo);
                            continue;
                        }
                        std::cout << "[converge] push_repo=" << repo << " mode=no-recursive\n";
                        state.commandLinesUsed[phase].push_back("kog push --no-recursive --repos " + repo);
                        const auto detailed = RunPushNativeSimpleDetailed(
                            workspaceRoot, false, false, *profile, *forceWithLease, *noVerify, 1, *verbose, *remote, {repo});
                        PopulatePhaseSummaryFromSingleRepoAggregate(repo, detailed.second, true, detailed.first, summary);
                        const auto failed = std::find(summary.failed.begin(), summary.failed.end(), repo) != summary.failed.end();
                        const auto blocked = std::find(summary.blocked.begin(), summary.blocked.end(), repo) != summary.blocked.end();
                        if (failed || blocked) {
                            failedOrBlocked.insert(repo);
                        }
                    }
                }
            } else if (phase == "status-delta-after-sync") {
                try {
                    snapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive, true);
                    plan = BuildPlan(snapshot);
                    state.plannedSyncRepos = UniqueRepos(plan.sync);
                    state.plannedCommitRepos = UniqueRepos(plan.commit);
                    state.plannedPushRepos = UniqueRepos(plan.push);
                    state.plannedBlockedRepos = UniqueRepos(plan.blocked);
                    state.plannedWaves = plan.waves;
                    state.repoGraphFingerprint = SnapshotFingerprint(snapshot);
                    state.repoBaselines = RepoBaselines(snapshot);
                    summary.succeeded.push_back(".");
                } catch (const std::exception& ex) {
                    summary.failed.push_back(".");
                    summary.failureCategory["."] = "FAILED_STATUS";
                    summary.failureMessage["."] = ex.what();
                    summary.retryEligible["."] = true;
                }
            } else if (phase == "commit-pointer-updates-if-needed") {
                for (const auto& line : plan.commit) {
                    if (PhaseSummaryContainsRepo(state, phase, line.repo, &PhaseSummary::succeeded)) {
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    if (PhaseSummaryContainsRepo(state, phase, line.repo, &PhaseSummary::skipped)) {
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    const bool pointerCommit = line.text.find("deterministic pointer commit") != std::string::npos;
                    std::string commandLine = "kog commit -ai --repos " + line.repo;
                    std::string failureCategory;
                    std::string failureMessage;
                    int code = 0;
                    if (agentIntentCommitMode && !pointerCommit) {
                        code = RunIntentCommitPlan(workspaceRoot, snapshot, line.repo, *profile, &commandLine, &failureCategory, &failureMessage);
                    } else {
                        const std::string message = pointerCommit ? (line.text.find(kPointerMultipleMessage) != std::string::npos ? kPointerMultipleMessage : kPointerSingleMessage) : std::string{};
                        code = RunCommitNativeSimple(workspaceRoot, line.repo, true, message, false, false, "", "", false, true, false);
                        if (pointerCommit) commandLine += " --message \"" + message + "\"";
                    }
                    state.commandLinesUsed[phase].push_back(commandLine);
                    if (code == 0) summary.succeeded.push_back(line.repo);
                    else {
                        summary.failed.push_back(line.repo);
                        summary.failureCategory[line.repo] = failureCategory.empty() ? "FAILED_COMMIT" : failureCategory;
                        summary.failureMessage[line.repo] = failureMessage.empty() ? "commit-pointer-updates-if-needed failed" : failureMessage;
                        summary.retryEligible[line.repo] = true;
                    }
                }
                for (const auto& line : plan.skipped) {
                    if (line.text.find("commit skipped") != std::string::npos || line.text.find("kog commit -ai skipped") != std::string::npos || line.text.find("pointer commit skipped") != std::string::npos) {
                        summary.skipped.push_back(line.repo);
                    }
                }
                } else if (phase == "settle-worktrees") {
                    if (!worktreeSettleRequested) {
                        state.commandLinesUsed[phase].push_back("kog converge worktree settle skipped: no worktree settle flags requested");
                        summary.skipped.push_back(".");
                    } else {
                        std::ostringstream cmdLine;
                        cmdLine << "kog converge branches retire --target " << Trim(*settleTarget)
                                << " --confirm --jobs " << *jobs;
                        if (!recursive) {
                            cmdLine << " --no-recursive";
                        }
                        if (*settleRemoveWorktrees) {
                            cmdLine << " --remove-worktrees";
                        }
                        if (*settlePruneWorktrees) {
                            cmdLine << " --prune-worktrees";
                        }
                        if (*settleHarvestDetachedWorktrees) {
                            cmdLine << " --harvest-detached-worktrees";
                        }
                        if (*settleHarvestBranchWorktrees) {
                            cmdLine << " --harvest-branch-worktrees";
                        }
                        state.commandLinesUsed[phase].push_back(cmdLine.str());
                        const auto settleCode = RunBranchRetire(
                            workspaceRoot,
                            *jobs,
                            recursive,
                            Trim(*settleTarget),
                            true,
                            *settleRemoveWorktrees,
                            false,
                            *settlePruneWorktrees,
                            *settleHarvestDetachedWorktrees,
                            *settleHarvestBranchWorktrees,
                            Trim(*settleHarvestMessage),
                            std::string{},
                            true,
                            false);
                        if (settleCode == 0) {
                            summary.succeeded.push_back(".");
                        } else {
                            summary.failed.push_back(".");
                            summary.failureCategory["."] = "WORKTREE_SETTLE_BLOCKED";
                            summary.failureMessage["."] = "worktree settle phase found blocked branch/worktree state";
                            summary.retryEligible["."] = true;
                        }
                    }
                } else if (phase == "final-status-summary") {
                    std::cout << "Converge final summary\n";
                    std::cout << "  succeeded=" << state.succeededRepos.size() << " failed=" << state.failedRepos.size() << " blocked=" << state.blockedRepoSet.size() << " skipped=" << state.skippedRepos.size() << " pending=" << state.pendingRepos.size() << "\n";
                    try {
                        nextSnapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive, true);
                        nextPlan = BuildPlan(nextSnapshot);
                        if (!nextPlan.blocked.empty()) {
                            summary.blocked = UniqueRepos(nextPlan.blocked);
                            state.blockedReason = "final status detected blocked repositories";
                            state.blockedRepos = summary.blocked;
                            for (const auto& repo : summary.blocked) {
                                summary.failureCategory[repo] = "BLOCKED_BY_POLICY";
                                summary.failureMessage[repo] = state.blockedReason;
                                summary.retryEligible[repo] = true;
                            }
                        } else if (PlanHasRunnableActions(nextPlan)) {
                            runAnotherPass = true;
                            std::cout << "[converge] remaining actions detected; next pass required"
                                      << " sync=" << nextPlan.sync.size()
                                      << " commit=" << nextPlan.commit.size()
                                      << " push=" << nextPlan.push.size()
                                      << "\n";
                        } else {
                            state.failedRepos.clear();
                            state.blockedRepoSet.clear();
                            state.blockedRepos.clear();
                            state.blockedReason.clear();
                        }
                        if (summary.failed.empty() && summary.blocked.empty()) {
                            summary.succeeded.push_back(".");
                        }
                    } catch (const std::exception& ex) {
                        summary.failed.push_back(".");
                        summary.failureCategory["."] = "FAILED_STATUS";
                        summary.failureMessage["."] = ex.what();
                        summary.retryEligible["."] = true;
                    }
                }

                std::sort(summary.succeeded.begin(), summary.succeeded.end());
            summary.succeeded.erase(std::unique(summary.succeeded.begin(), summary.succeeded.end()), summary.succeeded.end());
            std::sort(summary.failed.begin(), summary.failed.end());
            summary.failed.erase(std::unique(summary.failed.begin(), summary.failed.end()), summary.failed.end());
            std::sort(summary.blocked.begin(), summary.blocked.end());
            summary.blocked.erase(std::unique(summary.blocked.begin(), summary.blocked.end()), summary.blocked.end());
            std::sort(summary.skipped.begin(), summary.skipped.end());
            summary.skipped.erase(std::unique(summary.skipped.begin(), summary.skipped.end()), summary.skipped.end());

            for (const auto& repo : summary.succeeded) {
                state.pendingRepos.erase(std::remove(state.pendingRepos.begin(), state.pendingRepos.end(), repo), state.pendingRepos.end());
            }
            for (const auto& repo : summary.failed) {
                state.pendingRepos.erase(std::remove(state.pendingRepos.begin(), state.pendingRepos.end(), repo), state.pendingRepos.end());
            }
            // Resumed phases can turn previously failed/blocked repos into success;
            // keep aggregate result sets convergent instead of append-only.
            RemoveRepos(state.failedRepos, summary.succeeded);
            RemoveRepos(state.blockedRepoSet, summary.succeeded);
            RemoveRepos(state.blockedRepos, summary.succeeded);
            summary.pending = state.pendingRepos;
            RecordPhaseState(state, phase, summary);
            persist();
            const auto phaseElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - phaseStartedAt).count();
            std::cout << "[converge] phase_result=" << phase
                      << " elapsed_ms=" << phaseElapsedMs
                      << " succeeded=" << summary.succeeded.size()
                      << " failed=" << summary.failed.size()
                      << " blocked=" << summary.blocked.size()
                      << " skipped=" << summary.skipped.size()
                      << " pending=" << summary.pending.size()
                      << "\n";

                if (!summary.failed.empty() || !summary.blocked.empty()) {
                    const auto phaseReason = !summary.failed.empty()
                        ? (phase + " encountered failures")
                        : (state.blockedReason.empty() ? (phase + " encountered blocked repositories") : state.blockedReason);
                    state.blockedReason = phaseReason + "; post-failure recursive status baseline refresh skipped; resume reloads status before mutation";
                    if (!summary.failed.empty()) state.blockedRepos = summary.failed;
                    else state.blockedRepos = summary.blocked;
                    state.completedPhases.erase(std::remove(state.completedPhases.begin(), state.completedPhases.end(), phase), state.completedPhases.end());
                    state.currentPhase = phase;
                    state.commandLinesUsed[phase].push_back("kog status --recursive skipped after phase failure");
                    persist();
                    std::cerr << "Error: " << state.blockedReason << "\n";
                    std::exit(1);
                }
            }

            if (runAnotherPass) {
                ++passIndex;
                if (passIndex >= kMaxConvergePasses) {
                    std::vector<std::string> remaining = UniqueRepos(nextPlan.sync);
                    MergeUnique(remaining, UniqueRepos(nextPlan.commit));
                    MergeUnique(remaining, UniqueRepos(nextPlan.push));
                    state.blockedReason = "converge did not reach a fixpoint after " + std::to_string(kMaxConvergePasses) + " passes";
                    state.blockedRepos = remaining;
                    state.blockedRepoSet = remaining;
                    state.currentPhase = "final-status-summary";
                    persist();
                    std::cerr << "Error: " << state.blockedReason << "\n";
                    if (!remaining.empty()) {
                        std::cerr << "remainingRepos=" << Csv(remaining) << "\n";
                    }
                    std::exit(1);
                }
                snapshot = std::move(nextSnapshot);
                plan = std::move(nextPlan);
                ResetStateForPlan(state, snapshot, plan);
                startPhaseIndex = 0;
                persist();
                continue;
            }
            break;
        }

        DeleteState(statePath);
        const bool hasFailures = !state.failedRepos.empty() || !state.blockedRepoSet.empty();
        std::cout << "[converge] completed\n";
        std::exit(hasFailures ? 1 : 0);
    });
}

} // namespace kano::git::commands
