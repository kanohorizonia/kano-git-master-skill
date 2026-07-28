#include <catch2/catch_test_macros.hpp>

#include "commit_plan_task_graph.hpp"
#include "plan_utils.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace kano::git::commands;

namespace {

auto MakeCommit(std::string InMessage,
                std::vector<std::string> InInclude = {},
                std::vector<std::string> InExclude = {})
    -> RepoCommitPlanEntry::CommitItem {
    RepoCommitPlanEntry::CommitItem item;
    item.message = std::move(InMessage);
    item.include = std::move(InInclude);
    item.exclude = std::move(InExclude);
    return item;
}

auto MakeEntry(
    std::string InRepoKey,
    std::vector<RepoCommitPlanEntry::CommitItem> InCommits)
    -> RepoCommitPlanEntry {
    RepoCommitPlanEntry entry;
    entry.repoKey = std::move(InRepoKey);
    entry.commits = std::move(InCommits);
    return entry;
}

auto MakeRecord(
    std::filesystem::path InPath,
    std::vector<std::filesystem::path> InDependencies = {})
    -> kano::git::workspace::RepoRecord {
    kano::git::workspace::RepoRecord record;
    record.path = std::move(InPath);
    record.dependencies = std::move(InDependencies);
    return record;
}

auto MakeRunbook(
    const std::size_t InRepoRecordIndex,
    std::filesystem::path InRepo,
    std::vector<RepoCommitPlanEntry::CommitItem> InCommits)
    -> RepoCommitRunbook {
    RepoCommitRunbook runbook;
    runbook.repoRecordIndex = InRepoRecordIndex;
    runbook.repo = std::move(InRepo);
    runbook.commits = std::move(InCommits);
    return runbook;
}

auto TaskMessagesForWave(
    const CommitTaskGraph& InGraph,
    const std::size_t InWaveIndex)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto taskIndex : InGraph.waves.at(InWaveIndex)) {
        out.push_back(InGraph.tasks.at(taskIndex).commit.message);
    }
    return out;
}

} // namespace

TEST_CASE("stage message selection preserves stage filters normalization and append order",
          "[Unit][CommitPlan][TaskGraph][KG-TSK-0121]") {
    CommitPlanPayload plan;
    plan.commitEntries.push_back(
        MakeEntry("repo\\nested/", {MakeCommit("commit-one")}));
    plan.commitEntries.push_back(
        MakeEntry("repo/nested", {MakeCommit("commit-two")}));
    plan.commitEntries.push_back(
        MakeEntry("commit-only", {MakeCommit("commit-only-message")}));
    plan.postSyncEntries.push_back(
        MakeEntry("repo/nested///", {MakeCommit("post-sync-one")}));
    plan.postSyncEntries.push_back(
        MakeEntry("post-only", {MakeCommit("post-only-message")}));

    const auto commit = BuildStageMessageMap(plan, CommitPlanStage::Commit);
    REQUIRE(commit.size() == 2);
    REQUIRE(commit.at("repo/nested").size() == 2);
    REQUIRE(commit.at("repo/nested")[0].message == "commit-one");
    REQUIRE(commit.at("repo/nested")[1].message == "commit-two");
    REQUIRE(commit.at("commit-only")[0].message == "commit-only-message");
    REQUIRE_FALSE(commit.contains("post-only"));

    const auto postSync =
        BuildStageMessageMap(plan, CommitPlanStage::PostSync);
    REQUIRE(postSync.size() == 2);
    REQUIRE(postSync.at("repo/nested")[0].message == "post-sync-one");
    REQUIRE(postSync.at("post-only")[0].message == "post-only-message");
    REQUIRE_FALSE(postSync.contains("commit-only"));

    const auto both = BuildStageMessageMap(plan, CommitPlanStage::Both);
    REQUIRE(both.size() == 3);
    REQUIRE(both.at("repo/nested").size() == 3);
    REQUIRE(both.at("repo/nested")[0].message == "commit-one");
    REQUIRE(both.at("repo/nested")[1].message == "commit-two");
    REQUIRE(both.at("repo/nested")[2].message == "post-sync-one");
}

TEST_CASE("repo runbooks preserve absolute relative basename and root message precedence",
          "[Unit][CommitPlan][TaskGraph][KG-TSK-0121]") {
    const auto workspaceRoot =
        (std::filesystem::temp_directory_path() /
         "kog-task-graph-message-precedence")
            .lexically_normal();
    const auto absoluteRepo = workspaceRoot / "absolute-repo";
    const auto relativeRepo = workspaceRoot / "nested" / "relative-repo";
    const auto basenameRepo = workspaceRoot / "nested" / "basename-repo";

    const std::vector<kano::git::workspace::RepoRecord> records = {
        MakeRecord(absoluteRepo),
        MakeRecord(relativeRepo),
        MakeRecord(basenameRepo),
        MakeRecord(workspaceRoot),
    };
    std::unordered_map<
        std::string,
        std::vector<RepoCommitPlanEntry::CommitItem>>
        messages;
    messages[NormalizePlanKey(ToGeneric(absoluteRepo))] = {
        MakeCommit("absolute-wins")};
    messages["absolute-repo"] = {MakeCommit("absolute-basename-loses")};
    messages["nested/relative-repo"] = {MakeCommit("relative-wins")};
    messages["relative-repo"] = {MakeCommit("relative-basename-loses")};
    messages["basename-repo"] = {MakeCommit("basename-wins")};
    messages[NormalizePlanKey(ToGeneric(workspaceRoot))] = {
        MakeCommit("workspace-absolute-wins")};
    messages["."] = {MakeCommit("root-dot-loses")};

    const auto runbooks =
        BuildRepoCommitRunbooks(records, messages, workspaceRoot, "", true);

    REQUIRE(runbooks.size() == records.size());
    REQUIRE(runbooks[0].repoRecordIndex == 0);
    REQUIRE(runbooks[0].repo == absoluteRepo);
    REQUIRE(runbooks[0].commits[0].message == "absolute-wins");
    REQUIRE(runbooks[1].repoRecordIndex == 1);
    REQUIRE(runbooks[1].repo == relativeRepo);
    REQUIRE(runbooks[1].commits[0].message == "relative-wins");
    REQUIRE(runbooks[2].repoRecordIndex == 2);
    REQUIRE(runbooks[2].repo == basenameRepo);
    REQUIRE(runbooks[2].commits[0].message == "basename-wins");
    REQUIRE(runbooks[3].repoRecordIndex == 3);
    REQUIRE(runbooks[3].repo == workspaceRoot);
    REQUIRE(runbooks[3].commits[0].message == "workspace-absolute-wins");

    const auto rootByDot = BuildRepoCommitRunbooks(
        {MakeRecord(workspaceRoot)},
        {{".", {MakeCommit("root-dot-wins")}}},
        workspaceRoot,
        "",
        true);
    REQUIRE(rootByDot[0].commits[0].message == "root-dot-wins");
}

TEST_CASE("repo runbooks preserve default empty fallback and exact unscoped failure",
          "[Unit][CommitPlan][TaskGraph][KG-TSK-0121]") {
    const auto workspaceRoot =
        (std::filesystem::temp_directory_path() / "kog-task-graph-runbook")
            .lexically_normal();
    const std::vector<kano::git::workspace::RepoRecord> records = {
        MakeRecord(workspaceRoot / "repo-a"),
        MakeRecord(workspaceRoot / "repo-b"),
    };

    const auto defaulted =
        BuildRepoCommitRunbooks(records, {}, workspaceRoot, "default-message", false);
    REQUIRE(defaulted.size() == 2);
    REQUIRE(defaulted[0].valid);
    REQUIRE(defaulted[0].commits.size() == 1);
    REQUIRE(defaulted[0].commits[0].message == "default-message");
    REQUIRE(defaulted[1].commits[0].message == "default-message");

    const auto emptyFallback =
        BuildRepoCommitRunbooks(records, {}, workspaceRoot, "", true);
    REQUIRE(emptyFallback[0].valid);
    REQUIRE(emptyFallback[0].commits.size() == 1);
    REQUIRE(emptyFallback[0].commits[0].message.empty());

    const std::unordered_map<
        std::string,
        std::vector<RepoCommitPlanEntry::CommitItem>>
        partlyUnscoped = {
            {"repo-a",
             {MakeCommit("scoped", {"src/a.cpp"}),
              MakeCommit("unscoped")}},
        };
    const auto rejected =
        BuildRepoCommitRunbooks(records, partlyUnscoped, workspaceRoot, "", true);
    REQUIRE_FALSE(rejected[0].valid);
    REQUIRE(
        rejected[0].validationError ==
        "plan has multiple commits for one repo but some commit entries miss include/exclude scope");
    REQUIRE(rejected[0].commits.empty());
    REQUIRE(rejected[1].valid);

    const auto nonPlan = BuildRepoCommitRunbooks(
        {records[0]}, partlyUnscoped, workspaceRoot, "", false);
    REQUIRE(nonPlan[0].valid);
    REQUIRE(nonPlan[0].commits.size() == 2);

    const std::unordered_map<
        std::string,
        std::vector<RepoCommitPlanEntry::CommitItem>>
        includeOrExcludeScoped = {
            {"repo-a",
             {MakeCommit("include-scoped", {"src/a.cpp"}),
              MakeCommit("exclude-scoped", {}, {"generated/"})}},
        };
    const auto accepted = BuildRepoCommitRunbooks(
        {records[0]}, includeOrExcludeScoped, workspaceRoot, "", true);
    REQUIRE(accepted[0].valid);
    REQUIRE(accepted[0].commits.size() == 2);
}

TEST_CASE("task graph preserves sequential commits and explicit child before parent dependencies",
          "[Unit][CommitPlan][TaskGraph][KG-TSK-0121]") {
    const auto root =
        (std::filesystem::temp_directory_path() / "kog-task-graph-explicit")
            .lexically_normal();
    const auto parent = root / "parent";
    const auto child = root / "child";
    const std::vector<kano::git::workspace::RepoRecord> records = {
        MakeRecord(parent),
        MakeRecord(child, {parent}),
    };
    const std::vector<RepoCommitRunbook> runbooks = {
        MakeRunbook(0, parent, {MakeCommit("parent")}),
        MakeRunbook(
            1,
            child,
            {MakeCommit("child-one"), MakeCommit("child-two")}),
    };

    const auto graph = BuildCommitTaskGraph(records, runbooks);

    REQUIRE_FALSE(graph.dependencyCycleDetected);
    REQUIRE(graph.tasks.size() == 3);
    REQUIRE(graph.tasks[1].repoRecordIndex == 1);
    REQUIRE(graph.tasks[1].commitIndexInRepo == 0);
    REQUIRE(graph.tasks[1].repoCommitCount == 2);
    REQUIRE(graph.tasks[2].commitIndexInRepo == 1);
    REQUIRE(graph.waves.size() == 3);
    REQUIRE(TaskMessagesForWave(graph, 0) == std::vector<std::string>{"child-one"});
    REQUIRE(TaskMessagesForWave(graph, 1) == std::vector<std::string>{"child-two"});
    REQUIRE(TaskMessagesForWave(graph, 2) == std::vector<std::string>{"parent"});
}

TEST_CASE("task graph infers containment edges and deterministically sorts independent waves",
          "[Unit][CommitPlan][TaskGraph][KG-TSK-0121]") {
    const auto root =
        (std::filesystem::temp_directory_path() / "kog-task-graph-inferred")
            .lexically_normal();
    const auto parent = root / "project";
    const auto child = parent / "nested";
    const std::vector<kano::git::workspace::RepoRecord> containedRecords = {
        MakeRecord(parent),
        MakeRecord(child),
    };
    const auto contained = BuildCommitTaskGraph(
        containedRecords,
        {MakeRunbook(0, parent, {MakeCommit("parent")}),
         MakeRunbook(1, child, {MakeCommit("child")})});
    REQUIRE(contained.waves.size() == 2);
    REQUIRE(TaskMessagesForWave(contained, 0) == std::vector<std::string>{"child"});
    REQUIRE(TaskMessagesForWave(contained, 1) == std::vector<std::string>{"parent"});

    const std::vector<kano::git::workspace::RepoRecord> independentRecords = {
        MakeRecord(root / "b"),
        MakeRecord(root / "z" / "deep"),
        MakeRecord(root / "a"),
    };
    const auto independent = BuildCommitTaskGraph(
        independentRecords,
        {MakeRunbook(0, independentRecords[0].path, {MakeCommit("b")}),
         MakeRunbook(1, independentRecords[1].path, {MakeCommit("deep")}),
         MakeRunbook(2, independentRecords[2].path, {MakeCommit("a")})});
    REQUIRE(independent.waves.size() == 1);
    REQUIRE(
        TaskMessagesForWave(independent, 0) ==
        std::vector<std::string>{"deep", "a", "b"});
}

TEST_CASE("task graph preserves deterministic single-task cycle fallback and empty behavior",
          "[Unit][CommitPlan][TaskGraph][KG-TSK-0121]") {
    const auto root =
        (std::filesystem::temp_directory_path() / "kog-task-graph-cycle")
            .lexically_normal();
    const auto repoA = root / "a";
    const auto repoB = root / "b";
    const std::vector<kano::git::workspace::RepoRecord> records = {
        MakeRecord(repoB, {repoA}),
        MakeRecord(repoA, {repoB}),
    };
    const auto cycle = BuildCommitTaskGraph(
        records,
        {MakeRunbook(0, repoB, {MakeCommit("b")}),
         MakeRunbook(1, repoA, {MakeCommit("a")})});

    REQUIRE(cycle.dependencyCycleDetected);
    REQUIRE(cycle.tasks.size() == 2);
    REQUIRE(cycle.waves.size() == 2);
    REQUIRE(TaskMessagesForWave(cycle, 0) == std::vector<std::string>{"a"});
    REQUIRE(TaskMessagesForWave(cycle, 1) == std::vector<std::string>{"b"});

    const auto noRecords = BuildCommitTaskGraph({}, {});
    REQUIRE(noRecords.tasks.empty());
    REQUIRE(noRecords.waves.empty());
    REQUIRE_FALSE(noRecords.dependencyCycleDetected);

    RepoCommitRunbook invalid =
        MakeRunbook(0, repoA, {MakeCommit("ignored")});
    invalid.valid = false;
    const auto invalidOnly =
        BuildCommitTaskGraph({MakeRecord(repoA)}, {invalid});
    REQUIRE(invalidOnly.tasks.empty());
    REQUIRE(invalidOnly.waves.empty());
    REQUIRE_FALSE(invalidOnly.dependencyCycleDetected);
}
