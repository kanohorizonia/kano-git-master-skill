#pragma once

#include "commit_plan_payload.hpp"
#include "discovery.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace kano::git::commands {

struct RepoCommitRunbook {
    std::size_t repoRecordIndex = 0;
    std::filesystem::path repo;
    std::vector<RepoCommitPlanEntry::CommitItem> commits;
    bool valid = true;
    std::string validationError;
};

struct CommitTaskNode {
    std::size_t repoRecordIndex = 0;
    std::size_t commitIndexInRepo = 0;
    std::size_t repoCommitCount = 0;
    std::filesystem::path repo;
    RepoCommitPlanEntry::CommitItem commit;
};

struct CommitTaskGraph {
    std::vector<CommitTaskNode> tasks;
    std::vector<std::vector<std::size_t>> waves;
    bool dependencyCycleDetected = false;
};

auto BuildStageMessageMap(
    const CommitPlanPayload& InPlan,
    CommitPlanStage InStage)
    -> std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>>;

auto BuildRepoCommitRunbooks(
    const std::vector<workspace::RepoRecord>& InRepoRecords,
    const std::unordered_map<std::string, std::vector<RepoCommitPlanEntry::CommitItem>>& InStageMessages,
    const std::filesystem::path& InWorkspaceRoot,
    const std::string& InDefaultMessage,
    bool InIsPlanMode)
    -> std::vector<RepoCommitRunbook>;

auto BuildCommitTaskGraph(
    const std::vector<workspace::RepoRecord>& InRepoRecords,
    const std::vector<RepoCommitRunbook>& InRunbooks)
    -> CommitTaskGraph;

} // namespace kano::git::commands
