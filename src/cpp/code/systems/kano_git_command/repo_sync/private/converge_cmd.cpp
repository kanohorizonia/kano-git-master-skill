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
    bool detached = false;
    bool bare = false;
};

struct BranchAheadBehind {
    int ahead = 0;
    int behind = 0;
    bool hasUpstream = false;
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
    "sync-converge-dependent-repos",
    "status-delta-after-sync",
    "commit-pointer-updates-if-needed",
    "push-parents-bottom-up",
    "final-status-summary",
};
constexpr int kMaxConvergePasses = 8;

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

Snapshot LoadSnapshot(const std::filesystem::path& root, int jobs, bool unregisteredScan) {
    std::vector<std::string> args{"status", "--recursive", "--format", "json", "--repo-root", root.generic_string(), "--jobs", std::to_string(std::max(1, jobs))};
    args.push_back("--no-fetch-health");
    if (!unregisteredScan) {
        args.push_back("--no-unregistered-scan");
    } else {
        args.push_back("--refresh-cache");
        args.push_back("--unregistered-depth");
        args.push_back("3");
    }
    const auto result = shell::ExecuteCommand(SelfBinaryPath(), args, shell::ExecMode::Capture, root);
    if (result.exitCode != 0) throw std::runtime_error("failed to read recursive status snapshot via kog status: " + Trim(result.stderrStr));
    const auto start = result.stdoutStr.find("{\"schemaName\":\"kog.recursiveStatusSnapshot\"");
    const auto end = result.stdoutStr.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) {
        throw std::runtime_error("failed to locate kog.recursiveStatusSnapshot JSON in status output");
    }
    return ParseSnapshot(result.stdoutStr.substr(start, end - start + 1));
}

Snapshot CurrentRepoOnlySnapshot(Snapshot snapshot) {
    const auto it = std::find_if(snapshot.repos.begin(), snapshot.repos.end(), [](const auto& repo) {
        return repo.id == ".";
    });
    if (it == snapshot.repos.end()) {
        throw std::runtime_error("status snapshot did not include current repository");
    }
    RepoStatus current = *it;
    snapshot.repos.clear();
    snapshot.repos.push_back(std::move(current));
    return snapshot;
}

Snapshot LoadConvergeSnapshot(const std::filesystem::path& root, int jobs, bool unregisteredScan, bool recursive) {
    auto snapshot = LoadSnapshot(root, jobs, unregisteredScan);
    return recursive ? std::move(snapshot) : CurrentRepoOnlySnapshot(std::move(snapshot));
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
        if (repo.blocksConverge && IsCleanNestedPreflightOnlyBlocker(repo)) {
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

bool PlanHasRunnableActions(const Plan& plan) {
    return !plan.sync.empty() || !plan.commit.empty() || !plan.push.empty();
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

    if (lowered == ".gitignore" || lowered.ends_with("/.gitignore") || lowered.ends_with(".gitattributes")) {
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

std::string WorkspaceHeadSha(const std::filesystem::path& workspaceRoot) {
    const auto result = shell::ExecuteCommand("git", {"rev-parse", "HEAD"}, shell::ExecMode::Capture, workspaceRoot);
    if (result.exitCode == 0) {
        const auto sha = Trim(result.stdoutStr);
        if (!sha.empty()) {
            return sha;
        }
    }
    return "unknown-head-" + std::to_string(std::hash<std::string>{}(workspaceRoot.generic_string()));
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

std::string RepoKeyForWorkspaceFingerprint(const std::filesystem::path& workspaceRoot,
                                           const std::filesystem::path& repo) {
    const auto root = workspaceRoot.lexically_normal();
    const auto normalizedRepo = repo.lexically_normal();
    if (normalizedRepo == root) {
        return ".";
    }
    const auto relative = normalizedRepo.lexically_relative(root);
    if (!relative.empty() && relative != ".") {
        return NormalizeGitPath(relative.generic_string());
    }
    return NormalizeGitPath(normalizedRepo.generic_string());
}

bool IsFalseString(const std::string& value) {
    const auto normalized = ToLower(Trim(value));
    return normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off" || normalized == "disabled";
}

std::vector<std::filesystem::path> DiscoverFingerprintReposRecursive(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> repos{root.lexically_normal()};
    std::vector<std::filesystem::path> queue{root.lexically_normal()};
    std::set<std::string> seen{NormalizeGitPath(root.lexically_normal().generic_string())};

    while (!queue.empty()) {
        const auto current = queue.back();
        queue.pop_back();
        const auto modulesPath = current / ".gitmodules";
        if (!std::filesystem::exists(modulesPath)) {
            continue;
        }

        const auto modules = shell::ExecuteCommand(
            "git",
            {"config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$"},
            shell::ExecMode::Capture,
            current);
        if (modules.exitCode != 0) {
            continue;
        }

        std::istringstream lines(modules.stdoutStr);
        std::string line;
        while (std::getline(lines, line)) {
            line = Trim(std::move(line));
            if (line.empty()) {
                continue;
            }
            const auto split = line.find(' ');
            if (split == std::string::npos || split + 1 >= line.size()) {
                continue;
            }
            const auto key = line.substr(0, split);
            const auto pathValue = Trim(line.substr(split + 1));
            const auto namePrefix = std::string("submodule.");
            const auto nameSuffix = std::string(".path");
            if (key.rfind(namePrefix, 0) != 0 || key.size() <= namePrefix.size() + nameSuffix.size()) {
                continue;
            }
            const auto name = key.substr(namePrefix.size(), key.size() - namePrefix.size() - nameSuffix.size());
            const auto commitPolicy = shell::ExecuteCommand(
                "git",
                {"config", "-f", ".gitmodules", "--get", "submodule." + name + ".kog-commit"},
                shell::ExecMode::Capture,
                current);
            if (commitPolicy.exitCode == 0 && IsFalseString(commitPolicy.stdoutStr)) {
                continue;
            }

            const auto repo = (current / std::filesystem::path(pathValue)).lexically_normal();
            const auto gitDir = shell::ExecuteCommand("git", {"rev-parse", "--git-dir"}, shell::ExecMode::Capture, repo);
            if (gitDir.exitCode != 0) {
                continue;
            }
            const auto repoKey = NormalizeGitPath(repo.generic_string());
            if (seen.insert(repoKey).second) {
                repos.push_back(repo);
                queue.push_back(repo);
            }
        }
    }

    std::sort(repos.begin(), repos.end(), [&](const auto& a, const auto& b) {
        return RepoKeyForWorkspaceFingerprint(root, a) < RepoKeyForWorkspaceFingerprint(root, b);
    });
    return repos;
}

std::optional<std::string> ParseStatusChangedPathForFingerprint(const std::string& line) {
    if (line.size() < 4) {
        return std::nullopt;
    }
    auto path = Trim(line.substr(3));
    const auto arrow = path.find(" -> ");
    if (arrow != std::string::npos) {
        path = Trim(path.substr(arrow + 4));
    }
    if (path.empty()) {
        return std::nullopt;
    }
    return NormalizeGitPath(path);
}

bool IsInternalConvergeArtifactPath(const std::string& path) {
    const auto normalized = NormalizeGitPath(path);
    return normalized.rfind(".kano/tmp/", 0) == 0 ||
           normalized.rfind("src/cpp/.kano/tmp/", 0) == 0 ||
           normalized.find("/.kano/tmp/") != std::string::npos;
}

std::string ComputeWorkspaceBaseHeadShaForPlan(const std::filesystem::path& workspaceRoot) {
    std::vector<std::string> lines;
    const auto repos = DiscoverFingerprintReposRecursive(workspaceRoot);
    lines.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto head = shell::ExecuteCommand("git", {"rev-parse", "HEAD"}, shell::ExecMode::Capture, repo);
        const auto sha = head.exitCode == 0 ? Trim(head.stdoutStr) : std::string("0000000000000000000000000000000000000000");
        lines.push_back(RepoKeyForWorkspaceFingerprint(workspaceRoot, repo) + "\t" + sha);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "ws-head-v2-" + Fnv1a64HexLocal(canonical.str());
}

std::string ComputeWorkspaceDirtyFingerprintForPlan(const std::filesystem::path& workspaceRoot) {
    std::vector<std::string> lines;
    const auto repos = DiscoverFingerprintReposRecursive(workspaceRoot);
    lines.reserve(repos.size());
    for (const auto& repo : repos) {
        const auto status = shell::ExecuteCommand(
            "git",
            {"status", "--porcelain", "--branch", "--untracked-files=normal", "--ignore-submodules=none"},
            shell::ExecMode::Capture,
            repo);
        if (status.exitCode != 0) {
            continue;
        }

        std::istringstream stream(status.stdoutStr);
        std::ostringstream filtered;
        std::string line;
        while (std::getline(stream, line)) {
            const auto trimmed = Trim(line);
            if (trimmed.empty() || trimmed.rfind("## ", 0) == 0 || trimmed.rfind("# ", 0) == 0) {
                continue;
            }
            const auto path = ParseStatusChangedPathForFingerprint(trimmed);
            if (path.has_value() && IsInternalConvergeArtifactPath(*path)) {
                continue;
            }
            if (trimmed.find(" ../") != std::string::npos || trimmed.rfind("../", 0) == 0) {
                continue;
            }
            filtered << trimmed << "\n";
        }

        const auto normalized = Trim(filtered.str());
        const auto head = shell::ExecuteCommand("git", {"rev-parse", "HEAD"}, shell::ExecMode::Capture, repo);
        const auto sha = head.exitCode == 0 ? Trim(head.stdoutStr) : std::string("0000000000000000000000000000000000000000");
        const auto statusFingerprint = normalized.empty() ? std::string("clean") : Fnv1a64HexLocal(normalized);
        lines.push_back(RepoKeyForWorkspaceFingerprint(workspaceRoot, repo) + "|" + sha + "|" + statusFingerprint);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream canonical;
    for (const auto& line : lines) {
        canonical << line << "\n";
    }
    return "ws-dirty-v2-" + Fnv1a64HexLocal(canonical.str());
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
    std::vector<std::string> registeredChildPaths;
    const auto uniqueWorkItemId = UniqueWorkItemIdForDirtyEntries(dirtyEntries);
    const auto ticketContextSubsystem = uniqueWorkItemId.has_value()
        ? std::optional<std::string>(TicketContextSubsystemForDirtyEntries(dirtyEntries, *uniqueWorkItemId))
        : std::nullopt;
    for (const auto& entry : dirtyEntries) {
        const auto& path = entry.path;
        if (IsRegisteredChildRepoStatusPath(snapshot, repo, path)) {
            AppendDirtyEntryIncludePaths(registeredChildPaths, entry);
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
        auto& group = grouped[classified->key];
        if (group.key.empty()) {
            group = std::move(*classified);
            group.includePaths.clear();
            AppendDirtyEntryIncludePaths(group.includePaths, entry);
        } else {
            AppendDirtyEntryIncludePaths(group.includePaths, entry);
        }
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
    const auto baseHeadSha = ComputeWorkspaceBaseHeadShaForPlan(workspaceRoot);
    const auto fingerprint = ComputeWorkspaceDirtyFingerprintForPlan(workspaceRoot);
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
    return RunCommitNativePlanStage(workspaceRoot, plan.planPath.generic_string(), "commit", profile);
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

bool RefreshStateBaselinesForResume(WorkflowState& state,
                                    const std::filesystem::path& workspaceRoot,
                                    int jobs,
                                    bool recursive) {
    try {
        const auto snapshot = LoadConvergeSnapshot(workspaceRoot, jobs, false, recursive);
        state.repoBaselines = RepoBaselines(snapshot);
        return true;
    } catch (const std::exception&) {
        return false;
    }
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

shell::ExecResult GitCapture(const std::filesystem::path& repo, const std::vector<std::string>& args) {
    return shell::ExecuteCommand("git", args, shell::ExecMode::Capture, repo);
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

std::vector<std::string> LocalBranches(const std::filesystem::path& repoPath) {
    const auto result = GitCapture(repoPath, {"for-each-ref", "--format=%(refname:short)", "refs/heads"});
    std::vector<std::string> branches;
    if (result.exitCode != 0) {
        return branches;
    }
    std::istringstream lines(result.stdoutStr);
    std::string line;
    while (std::getline(lines, line)) {
        auto branch = Trim(line);
        if (!branch.empty()) {
            branches.push_back(branch);
        }
    }
    std::sort(branches.begin(), branches.end());
    branches.erase(std::unique(branches.begin(), branches.end()), branches.end());
    return branches;
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
            current.location = DisplayPathForBranchPlan(workspaceRoot, std::filesystem::path(line.substr(std::string("worktree ").size())));
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

nlohmann::json BranchPlanJsonForRepo(const Snapshot& snapshot,
                                     const RepoStatus& repo,
                                     const std::filesystem::path& repoPath,
                                     const std::string& targetBranch,
                                     const std::string& strategy) {
    const auto targetRef = ResolveTargetRef(repoPath, targetBranch, repo.remote);
    const auto targetCounts = AheadBehindForBranch(repoPath, targetBranch);
    const bool targetBranchBehind = targetCounts.hasUpstream && targetCounts.behind > 0;
    const auto branches = LocalBranches(repoPath);
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
        worktreeJson.push_back({
            {"branch", worktree.branch},
            {"location", worktree.location},
            {"detached", worktree.detached},
            {"bare", worktree.bare},
        });
    }
    repoJson["worktrees"] = std::move(worktreeJson);

    nlohmann::json branchesJson = nlohmann::json::array();
    for (const auto& branch : branches) {
        const auto checkedOut = WorktreeLocationsForBranch(worktrees, branch);
        const auto counts = AheadBehindForBranch(repoPath, branch);
        const auto isTarget = branch == targetBranch;
        const auto merged = isTarget || BranchMergedIntoTarget(repoPath, branch, targetRef);

        std::vector<std::string> blockers;
        if (targetRef.empty()) {
            blockers.push_back("TARGET_REF_MISSING");
        }
        if (!isTarget && !checkedOut.empty()) {
            blockers.push_back("ACTIVE_WORKTREE_LEASE");
        }
        if (repo.dirtyKind != "CLEAN") {
            blockers.push_back("DIRTY_WORKTREE:" + repo.dirtyKind);
        }
        if (repo.blocksConverge) {
            blockers.push_back(repo.blockReason.empty() ? std::string{"REPO_PREFLIGHT_BLOCKED"} : ("REPO_PREFLIGHT_BLOCKED:" + repo.blockReason));
        }
        if (targetBranchBehind) {
            blockers.push_back("STALE_TARGET_BRANCH");
        }
        if (counts.ahead > 0 || (!isTarget && !counts.hasUpstream && !merged)) {
            blockers.push_back("UNPUSHED_COMMITS");
        }
        if (!isTarget && counts.behind > 0) {
            blockers.push_back("STALE_LOCAL_BRANCH");
        }
        std::sort(blockers.begin(), blockers.end());
        blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());

        std::vector<std::string> actions;
        if (isTarget) {
            actions.push_back("keep target branch");
        } else if (!blockers.empty()) {
            actions.push_back("resolve blockers before branch integration or retirement");
        } else if (merged) {
            actions.push_back("candidate for human-reviewed branch retirement; planner does not delete branches");
        } else if (strategy == "merge") {
            actions.push_back("would plan merge integration into " + targetBranch);
        } else {
            actions.push_back("would plan rebase integration onto " + targetBranch);
        }

        branchesJson.push_back({
            {"name", branch},
            {"isTarget", isTarget},
            {"targetRef", targetRef},
            {"strategy", strategy},
            {"checkedOutWorktrees", checkedOut},
            {"activeLeaseBlocker", !isTarget && !checkedOut.empty()},
            {"hasUpstream", counts.hasUpstream},
            {"ahead", counts.ahead},
            {"behind", counts.behind},
            {"mergedIntoTarget", merged},
            {"blockers", blockers},
            {"proposedActions", actions},
        });
    }
    repoJson["branches"] = std::move(branchesJson);
    return repoJson;
}

nlohmann::json BuildBranchPlanJson(const Snapshot& snapshot,
                                   const std::filesystem::path& workspaceRoot,
                                   const std::string& targetBranch,
                                   const std::string& strategy,
                                   bool recursive) {
    const auto traversal = BranchPlanTraversalOrder(snapshot);
    nlohmann::json repos = nlohmann::json::array();
    for (const auto& repoId : traversal) {
        const auto* repo = FindRepo(snapshot, repoId);
        if (repo == nullptr) {
            continue;
        }
        repos.push_back(BranchPlanJsonForRepo(snapshot, *repo, RepoPathForBranchPlan(workspaceRoot, *repo), targetBranch, strategy));
    }

    return {
        {"schemaName", "kog.convergeBranchesPlan"},
        {"schemaVersion", 1},
        {"mutationPerformed", false},
        {"workspaceRoot", workspaceRoot.generic_string()},
        {"targetBranch", targetBranch},
        {"strategy", strategy},
        {"recursive", recursive},
        {"traversalOrder", traversal},
        {"repos", repos},
    };
}

bool BranchPlanHasBlockers(const nlohmann::json& plan) {
    for (const auto& repo : plan.value("repos", nlohmann::json::array())) {
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

int RunBranchPlanner(const std::filesystem::path& root,
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
        if (strategy != "rebase" && strategy != "merge") {
            std::cerr << "Error: --strategy must be rebase or merge\n";
            return 2;
        }
        const bool jsonOutput = emitJson || IsConvergeAgentModeEnabled();
        std::optional<shell::ScopedConsoleWriteSuppression> suppressCommandLogs;
        if (jsonOutput) {
            suppressCommandLogs.emplace();
        }
        const auto snapshot = LoadConvergeSnapshot(root, jobs, false, recursive);
        const auto plan = BuildBranchPlanJson(snapshot, root, targetBranch, strategy, recursive);
        suppressCommandLogs.reset();
        if (jsonOutput) {
            std::cout << plan.dump(2) << "\n";
        } else {
            PrintBranchPlanText(plan);
        }
        return BranchPlanHasBlockers(plan) ? 1 : 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
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
    auto* jobs = new int{1};
    auto* remote = new std::string{};

    auto* repos = cmd->add_subcommand("repos", "Alias for the existing repo-state converge workflow");
    auto* branches = cmd->add_subcommand("branches", "Plan branch integration and retirement safely");
    branches->fallthrough(false);
    branches->require_subcommand(1);
    auto* branchesPlan = branches->add_subcommand("plan", "Read-only branch convergence planner");
    branchesPlan->fallthrough(false);
    auto* branchesNoRecursive = new bool{false};
    auto* branchesJson = new bool{false};
    auto* branchesJobs = new int{1};
    auto* branchesTarget = new std::string{"main"};
    auto* branchesStrategy = new std::string{"rebase"};

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
    cmd->add_option("--jobs", *jobs, "Parallel workers for converge status/push stages");
    cmd->add_option("--remote", *remote, "Optional remote filter for converge push stage");

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
    repos->add_option("--jobs", *jobs, "Parallel workers for converge status/push stages");
    repos->add_option("--remote", *remote, "Optional remote filter for converge push stage");

    branchesPlan->add_flag("--no-recursive,-N", *branchesNoRecursive, "Only plan branches for the current repository");
    branchesPlan->add_flag("--json", *branchesJson, "Emit stable machine-readable branch plan JSON");
    branchesPlan->add_option("--jobs", *branchesJobs, "Parallel workers for recursive status snapshot loading");
    branchesPlan->add_option("--target", *branchesTarget, "Target integration branch")->default_str("main");
    branchesPlan->add_option("--strategy", *branchesStrategy, "Integration strategy recorded in the plan: rebase|merge")->default_str("rebase");

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
        args.push_back("--jobs");
        args.push_back(std::to_string(*jobs));
        if (!remote->empty()) {
            args.push_back("--remote");
            args.push_back(*remote);
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

    cmd->callback([=]() {
        if (!cmd->get_subcommands().empty()) {
            return;
        }
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto recursive = !*noRecursive;
        const auto agentIntentCommitMode = *aiCompat || IsConvergeAgentModeEnabled();
        const auto statePath = ConvergeStatePath(workspaceRoot);

        const int controlFlags = (*statusOnly ? 1 : 0) + (*resume ? 1 : 0) + (*abort ? 1 : 0);
        if (controlFlags > 1) { std::cerr << "Error: --status, --resume, and --abort are mutually exclusive\n"; std::exit(2); }
        if (*jobs < 1) { std::cerr << "Error: --jobs must be a positive integer\n"; std::exit(2); }

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
                snapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive);
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
            const auto phaseIt = std::find(kConvergePhases.begin(), kConvergePhases.end(), state.currentPhase);
            startPhaseIndex = phaseIt == kConvergePhases.end() ? 0 : static_cast<std::size_t>(std::distance(kConvergePhases.begin(), phaseIt));
            std::cout << "Resuming converge from phase: " << state.currentPhase << "\n";
        } else {
            try {
                snapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive);
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

        int passIndex = 0;
        while (true) {
            bool runAnotherPass = false;
            Snapshot nextSnapshot;
            Plan nextPlan;
            if (passIndex > 0) {
                std::cout << "[converge] pass=" << (passIndex + 1) << "\n";
            }

            for (std::size_t idx = startPhaseIndex; idx < kConvergePhases.size(); ++idx) {
                const auto& phase = kConvergePhases[idx];
                if (PhaseAlreadyCompleted(state, phase)) continue;

                state.currentPhase = phase;
                persist();
                std::cout << "[converge] phase=" << phase << "\n";

                PhaseSummary summary;
                summary.pending = state.pendingRepos;

                if (phase == "status-preflight-plan") {
                PrintPlan(snapshot, plan, false);
                summary.skipped = UniqueRepos(plan.skipped);
                summary.blocked = UniqueRepos(plan.blocked);
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
                    const auto repoPath = line.repo == "."
                        ? workspaceRoot
                        : (workspaceRoot / std::filesystem::path(line.repo)).lexically_normal();
                    state.commandLinesUsed[phase].push_back("kog sync origin-latest --repo " + repoPath.generic_string() + " --no-recursive");
                    const auto detailed = RunSyncOriginLatestNativeDetailed(repoPath, false, false, false);
                    PopulatePhaseSummaryFromSingleRepoAggregate(line.repo, detailed.second, false, detailed.first, summary);
                    if (detailed.first != 0 && summary.failed.empty() && summary.blocked.empty()) {
                        summary.failed.push_back(line.repo);
                        summary.failureCategory[line.repo] = "SYNC_ERROR";
                        summary.failureMessage[line.repo] = "sync phase failed";
                        summary.retryEligible[line.repo] = true;
                    }
                }
                } else if (phase == "push-nested-bottom-up" || phase == "push-parents-bottom-up") {
                const auto pushRepos = UniqueRepos(plan.push);
                if (pushRepos.empty()) {
                    state.commandLinesUsed[phase].push_back("kog push skipped: no planned repositories");
                } else {
                    state.commandLinesUsed[phase].push_back(
                        recursive
                            ? ("kog push --recursive --repos " + Csv(pushRepos) + " --jobs " + std::to_string(*jobs))
                            : ("kog push --no-recursive --jobs " + std::to_string(*jobs)));
                    const auto pushFilters = recursive ? pushRepos : std::vector<std::string>{};
                    const auto detailed = RunPushNativeSimpleDetailed(workspaceRoot, recursive, false, *profile, *forceWithLease, *noVerify, *jobs, *verbose, *remote, pushFilters);
                    PopulatePhaseSummaryFromAggregate(detailed.second, true, summary);
                    if (detailed.first != 0 && summary.failed.empty() && summary.blocked.empty()) {
                        summary.failed.push_back(".");
                        summary.failureCategory["."] = "PUSH_ERROR";
                        summary.failureMessage["."] = "push phase failed";
                        summary.retryEligible["."] = true;
                    }
                }
            } else if (phase == "status-delta-after-sync") {
                try {
                    snapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive);
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
                    if (line.text.find("deterministic pointer commit") == std::string::npos) continue;
                    if (PhaseSummaryContainsRepo(state, phase, line.repo, &PhaseSummary::succeeded)) {
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    if (PhaseSummaryContainsRepo(state, phase, line.repo, &PhaseSummary::skipped)) {
                        summary.skipped.push_back(line.repo);
                        continue;
                    }
                    const std::string message = line.text.find(kPointerMultipleMessage) != std::string::npos ? kPointerMultipleMessage : kPointerSingleMessage;
                    const auto code = RunCommitNativeSimple(workspaceRoot, line.repo, true, message, false, false, "", "", false, true, false);
                    state.commandLinesUsed[phase].push_back("kog commit -ai --repos " + line.repo + " --message \"" + message + "\"");
                    if (code == 0) summary.succeeded.push_back(line.repo);
                    else {
                        summary.failed.push_back(line.repo);
                        summary.failureCategory[line.repo] = "FAILED_COMMIT";
                        summary.failureMessage[line.repo] = "pointer commit failed";
                        summary.retryEligible[line.repo] = true;
                    }
                }
                for (const auto& line : plan.skipped) if (line.text.find("pointer commit skipped") != std::string::npos) summary.skipped.push_back(line.repo);
                } else if (phase == "final-status-summary") {
                    std::cout << "Converge final summary\n";
                    std::cout << "  succeeded=" << state.succeededRepos.size() << " failed=" << state.failedRepos.size() << " blocked=" << state.blockedRepoSet.size() << " skipped=" << state.skippedRepos.size() << " pending=" << state.pendingRepos.size() << "\n";
                    try {
                        nextSnapshot = LoadConvergeSnapshot(workspaceRoot, *jobs, false, recursive);
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

                if (!summary.failed.empty() || !summary.blocked.empty()) {
                    state.blockedReason = !summary.failed.empty() ? (phase + " encountered failures") : "blocked repositories present";
                    if (!summary.failed.empty()) state.blockedRepos = summary.failed;
                    else state.blockedRepos = summary.blocked;
                    state.completedPhases.erase(std::remove(state.completedPhases.begin(), state.completedPhases.end(), phase), state.completedPhases.end());
                    state.currentPhase = phase;
                    RefreshStateBaselinesForResume(state, workspaceRoot, *jobs, recursive);
                    persist();
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
