#include "commit_plan_task_graph.hpp"

#include "commit_ai_utils.hpp"
#include "plan_utils.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kano::git::commands {
namespace {

auto IsParentRepoPath(const std::filesystem::path& InParent,
                      const std::filesystem::path& InChild) -> bool {
    const auto parent = ToGeneric(InParent);
    const auto child = ToGeneric(InChild);
    if (parent.empty() || child.empty() || parent == child) {
        return false;
    }
    const std::string prefix = parent + "/";
    return child.rfind(prefix, 0) == 0;
}

auto ResolveRepoMessages(
    const std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>>& InStageMessages,
    const std::filesystem::path& InWorkspaceRoot,
    const std::filesystem::path& InRepo,
    const std::string& InDefaultMessage)
    -> std::vector<RepoCommitPlanEntry::CommitItem> {
    std::vector<std::string> candidates;
    const auto rootNorm = NormalizePath(InWorkspaceRoot);
    const auto repoNorm = NormalizePath(InRepo);

    candidates.push_back(NormalizePlanKey(ToGeneric(repoNorm)));
    if (ToGeneric(rootNorm) == ToGeneric(repoNorm)) {
        candidates.push_back(".");
    } else {
        const auto rel = repoNorm.lexically_relative(rootNorm);
        if (!rel.empty() && rel != ".") {
            candidates.push_back(NormalizePlanKey(rel.generic_string()));
        }
    }
    candidates.push_back(NormalizePlanKey(repoNorm.filename().generic_string()));

    for (const auto& key : candidates) {
        if (const auto it = InStageMessages.find(key);
            it != InStageMessages.end() && !it->second.empty()) {
            return it->second;
        }
    }

    if (!InDefaultMessage.empty()) {
        RepoCommitPlanEntry::CommitItem one;
        one.message = InDefaultMessage;
        return {one};
    }
    RepoCommitPlanEntry::CommitItem one;
    one.message = "";
    return {one};
}

} // namespace

auto BuildStageMessageMap(
    const CommitPlanPayload& InPlan,
    const CommitPlanStage InStage)
    -> std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> {
    std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>> out;
    auto appendEntries = [&](const std::vector<RepoCommitPlanEntry>& entries) {
        for (const auto& entry : entries) {
            auto& bucket = out[NormalizePlanKey(entry.repoKey)];
            for (const auto& item : entry.commits) {
                bucket.push_back(item);
            }
        }
    };

    if (InStage == CommitPlanStage::Commit || InStage == CommitPlanStage::Both) {
        appendEntries(InPlan.commitEntries);
    }
    if (InStage == CommitPlanStage::PostSync || InStage == CommitPlanStage::Both) {
        appendEntries(InPlan.postSyncEntries);
    }
    return out;
}

auto BuildRepoCommitRunbooks(
    const std::vector<workspace::RepoRecord>& InRepoRecords,
    const std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>>& InStageMessages,
    const std::filesystem::path& InWorkspaceRoot,
    const std::string& InDefaultMessage,
    const bool InIsPlanMode)
    -> std::vector<RepoCommitRunbook> {
    std::vector<RepoCommitRunbook> out;
    out.reserve(InRepoRecords.size());
    for (std::size_t ridx = 0; ridx < InRepoRecords.size(); ++ridx) {
        RepoCommitRunbook runbook;
        runbook.repoRecordIndex = ridx;
        runbook.repo = InRepoRecords[ridx].path;
        runbook.commits = ResolveRepoMessages(
            InStageMessages, InWorkspaceRoot, runbook.repo, InDefaultMessage);
        if (InIsPlanMode && runbook.commits.size() > 1) {
            bool hasUnscoped = false;
            for (const auto& item : runbook.commits) {
                if (item.include.empty() && item.exclude.empty()) {
                    hasUnscoped = true;
                    break;
                }
            }
            if (hasUnscoped) {
                runbook.valid = false;
                runbook.validationError =
                    "plan has multiple commits for one repo but some commit entries miss include/exclude scope";
                runbook.commits.clear();
            }
        }
        out.push_back(std::move(runbook));
    }
    return out;
}

auto BuildCommitTaskGraph(
    const std::vector<workspace::RepoRecord>& InRepoRecords,
    const std::vector<RepoCommitRunbook>& InRunbooks)
    -> CommitTaskGraph {
    CommitTaskGraph graph;
    if (InRepoRecords.empty() || InRunbooks.empty()) {
        return graph;
    }

    std::vector<std::vector<std::size_t>> repoTaskIndices(InRepoRecords.size());
    for (const auto& runbook : InRunbooks) {
        if (!runbook.valid || runbook.commits.empty()) {
            continue;
        }
        for (std::size_t cidx = 0; cidx < runbook.commits.size(); ++cidx) {
            CommitTaskNode node;
            node.repoRecordIndex = runbook.repoRecordIndex;
            node.commitIndexInRepo = cidx;
            node.repoCommitCount = runbook.commits.size();
            node.repo = runbook.repo;
            node.commit = runbook.commits[cidx];
            const auto taskIndex = graph.tasks.size();
            graph.tasks.push_back(std::move(node));
            repoTaskIndices[runbook.repoRecordIndex].push_back(taskIndex);
        }
    }

    if (graph.tasks.empty()) {
        return graph;
    }

    std::vector<std::vector<std::size_t>> outgoing(graph.tasks.size());
    std::vector<std::unordered_set<std::size_t>> dedupOutgoing(graph.tasks.size());
    std::vector<std::size_t> indegree(graph.tasks.size(), 0);

    auto addEdge = [&](const std::size_t from, const std::size_t to) {
        if (from == to) {
            return;
        }
        if (dedupOutgoing[from].insert(to).second) {
            outgoing[from].push_back(to);
            indegree[to] += 1;
        }
    };

    for (const auto& taskList : repoTaskIndices) {
        for (std::size_t idx = 1; idx < taskList.size(); ++idx) {
            addEdge(taskList[idx - 1], taskList[idx]);
        }
    }

    std::unordered_map<std::string, std::size_t> repoByPath;
    repoByPath.reserve(InRepoRecords.size());
    for (std::size_t idx = 0; idx < InRepoRecords.size(); ++idx) {
        repoByPath.emplace(ToGeneric(InRepoRecords[idx].path), idx);
    }

    for (std::size_t ridx = 0; ridx < InRepoRecords.size(); ++ridx) {
        if (repoTaskIndices[ridx].empty()) {
            continue;
        }
        for (const auto& dep : InRepoRecords[ridx].dependencies) {
            const auto depIt = repoByPath.find(ToGeneric(dep));
            if (depIt == repoByPath.end()) {
                continue;
            }
            const auto depRepoIndex = depIt->second;
            if (depRepoIndex == ridx || repoTaskIndices[depRepoIndex].empty()) {
                continue;
            }
            // dependencies[] currently points to parent repos (superproject).
            // For commit apply, child commits must land first so parent pointer updates can commit afterward.
            const auto repoTail = repoTaskIndices[ridx].back();
            const auto depHead = repoTaskIndices[depRepoIndex].front();
            addEdge(repoTail, depHead);
        }
    }

    for (std::size_t parentIdx = 0; parentIdx < InRepoRecords.size(); ++parentIdx) {
        if (repoTaskIndices[parentIdx].empty()) {
            continue;
        }
        for (std::size_t childIdx = 0; childIdx < InRepoRecords.size(); ++childIdx) {
            if (parentIdx == childIdx || repoTaskIndices[childIdx].empty()) {
                continue;
            }
            if (!IsParentRepoPath(
                    InRepoRecords[parentIdx].path, InRepoRecords[childIdx].path)) {
                continue;
            }
            const auto childTail = repoTaskIndices[childIdx].back();
            const auto parentHead = repoTaskIndices[parentIdx].front();
            addEdge(childTail, parentHead);
        }
    }

    auto nodeLess = [&](const std::size_t A, const std::size_t B) {
        const auto& taskA = graph.tasks[A];
        const auto& taskB = graph.tasks[B];
        const auto& repoA = InRepoRecords[taskA.repoRecordIndex].path;
        const auto& repoB = InRepoRecords[taskB.repoRecordIndex].path;
        const auto depthA = PathDepth(repoA);
        const auto depthB = PathDepth(repoB);
        if (depthA != depthB) {
            return depthA > depthB;
        }
        const auto keyA = ToGeneric(repoA);
        const auto keyB = ToGeneric(repoB);
        if (keyA != keyB) {
            return keyA < keyB;
        }
        return taskA.commitIndexInRepo < taskB.commitIndexInRepo;
    };

    std::vector<std::size_t> ready;
    ready.reserve(graph.tasks.size());
    for (std::size_t idx = 0; idx < graph.tasks.size(); ++idx) {
        if (indegree[idx] == 0) {
            ready.push_back(idx);
        }
    }
    std::sort(ready.begin(), ready.end(), nodeLess);

    std::size_t processed = 0;
    while (!ready.empty()) {
        graph.waves.push_back(ready);
        processed += ready.size();
        std::vector<std::size_t> next;
        for (const auto node : ready) {
            for (const auto out : outgoing[node]) {
                if (indegree[out] == 0) {
                    continue;
                }
                indegree[out] -= 1;
                if (indegree[out] == 0) {
                    next.push_back(out);
                }
            }
        }
        std::sort(next.begin(), next.end(), nodeLess);
        next.erase(std::unique(next.begin(), next.end()), next.end());
        ready = std::move(next);
    }

    if (processed != graph.tasks.size()) {
        graph.dependencyCycleDetected = true;
        graph.waves.clear();
        std::vector<std::size_t> fallback;
        fallback.reserve(graph.tasks.size());
        for (std::size_t idx = 0; idx < graph.tasks.size(); ++idx) {
            fallback.push_back(idx);
        }
        std::sort(fallback.begin(), fallback.end(), nodeLess);
        for (const auto idx : fallback) {
            graph.waves.push_back({idx});
        }
    }

    return graph;
}

} // namespace kano::git::commands
