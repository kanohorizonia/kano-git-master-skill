#include <catch2/catch_test_macros.hpp>

#include "commit_ai_utils.hpp"
#include "plan_utils.hpp"
#include "shell_executor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace kano::git::commands;

namespace {

auto UniqueTempWorkspace(const std::string& InSuffix) -> std::filesystem::path {
    const auto root = (std::filesystem::temp_directory_path() / "kog-commit-scope-tests" / InSuffix).lexically_normal();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) {
    std::error_code ec;
    std::filesystem::create_directories(InPath.parent_path(), ec);
    std::ofstream out(InPath, std::ios::out | std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
    out.close();
    REQUIRE(out.good());
}

void RequireGit(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) {
    const auto result = kano::git::shell::ExecuteCommand("git", InArgs, kano::git::shell::ExecMode::Capture, InRepo);
    INFO("git command failed in " << InRepo.generic_string());
    INFO("stdout: " << result.stdoutStr);
    INFO("stderr: " << result.stderrStr);
    REQUIRE(result.exitCode == 0);
}

auto ContainsRepoPath(const std::vector<kano::git::workspace::RepoRecord>& InRecords,
                      const std::filesystem::path& InRepo) -> bool {
    const auto expected = InRepo.lexically_normal().generic_string();
    for (const auto& record : InRecords) {
        if (record.path.lexically_normal().generic_string() == expected) {
            return true;
        }
    }
    return false;
}

auto CommitStageRepos(const std::string& InPlanText) -> std::vector<std::string> {
    const auto doc = nlohmann::json::parse(InPlanText);
    std::vector<std::string> repos;
    if (!doc.contains("stages") || !doc["stages"].contains("commit")) {
        return repos;
    }
    for (const auto& repoObj : doc["stages"]["commit"]) {
        repos.push_back(repoObj.value("repo", ""));
    }
    return repos;
}

auto IndexOfRepo(const std::vector<std::string>& InRepos, const std::string& InRepo) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < InRepos.size(); ++index) {
        if (InRepos[index] == InRepo) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("BuildCommitScopeRecords includes recursively dirty nested git repos seen only through parent status",
          "[Unit][CommitScope][Subrepo]") {
    const auto root = UniqueTempWorkspace("dirty-recursive-nested-repo");
    const auto parent = (root / "parent-skill").lexically_normal();
    const auto child = (parent / "child-skill").lexically_normal();

    RequireGit(root, {"init"});
    RequireGit(root, {"config", "user.email", "tests@example.invalid"});
    RequireGit(root, {"config", "user.name", "Kog Tests"});

    std::filesystem::create_directories(parent);
    RequireGit(parent, {"init"});
    RequireGit(parent, {"config", "user.email", "tests@example.invalid"});
    RequireGit(parent, {"config", "user.name", "Kog Tests"});
    WriteTextFile(parent / "PARENT.md", "initial parent\n");
    RequireGit(parent, {"add", "PARENT.md"});
    RequireGit(parent, {"commit", "-m", "docs: initial parent"});

    std::filesystem::create_directories(child);
    RequireGit(child, {"init"});
    RequireGit(child, {"config", "user.email", "tests@example.invalid"});
    RequireGit(child, {"config", "user.name", "Kog Tests"});
    WriteTextFile(child / "README.md", "initial child\n");
    RequireGit(child, {"add", "README.md"});
    RequireGit(child, {"commit", "-m", "docs: initial child"});

    // Stage and commit the nested repository as a gitlink in the parent, then
    // stage and commit the parent gitlink in the workspace root.
    RequireGit(parent, {"add", "child-skill"});
    RequireGit(parent, {"commit", "-m", "chore: add child gitlink"});

    RequireGit(root, {"add", "parent-skill"});
    RequireGit(root, {"commit", "-m", "chore: add parent gitlink"});

    // Child-only edits propagate as parent/root dirty submodule paths, but the
    // actual file changes live only in the deepest nested repo.
    WriteTextFile(child / "README.md", "initial child\nupdated child\n");

    const auto records = BuildCommitScopeRecords(root, "", false, true);

    REQUIRE(ContainsRepoPath(records, child));
    REQUIRE(ContainsRepoPath(records, parent));
}

TEST_CASE("SeedCommitStage includes recursively dirty nested git repos in plan order",
          "[Unit][CommitPlan][CommitSeed][Subrepo]") {
    const auto root = UniqueTempWorkspace("commit-seed-recursive-nested-repo");
    const auto parent = (root / "parent-skill").lexically_normal();
    const auto child = (parent / "child-skill").lexically_normal();

    RequireGit(root, {"init"});
    RequireGit(root, {"config", "user.email", "tests@example.invalid"});
    RequireGit(root, {"config", "user.name", "Kog Tests"});

    std::filesystem::create_directories(parent);
    RequireGit(parent, {"init"});
    RequireGit(parent, {"config", "user.email", "tests@example.invalid"});
    RequireGit(parent, {"config", "user.name", "Kog Tests"});
    WriteTextFile(parent / "PARENT.md", "initial parent\n");
    RequireGit(parent, {"add", "PARENT.md"});
    RequireGit(parent, {"commit", "-m", "docs: initial parent"});

    std::filesystem::create_directories(child);
    RequireGit(child, {"init"});
    RequireGit(child, {"config", "user.email", "tests@example.invalid"});
    RequireGit(child, {"config", "user.name", "Kog Tests"});
    WriteTextFile(child / "README.md", "initial child\n");
    RequireGit(child, {"add", "README.md"});
    RequireGit(child, {"commit", "-m", "docs: initial child"});

    RequireGit(parent, {"add", "child-skill"});
    RequireGit(parent, {"commit", "-m", "chore: add child gitlink"});
    RequireGit(root, {"add", "parent-skill"});
    RequireGit(root, {"commit", "-m", "chore: add parent gitlink"});

    WriteTextFile(child / "README.md", "initial child\nupdated child\n");

    const auto seeded = SeedCommitStage(root, BuildDefaultPlanTemplate(root), true, true);
    REQUIRE(seeded.has_value());

    const auto repos = CommitStageRepos(*seeded);
    const auto childIndex = IndexOfRepo(repos, "parent-skill/child-skill");
    const auto parentIndex = IndexOfRepo(repos, "parent-skill");
    const auto rootIndex = IndexOfRepo(repos, ".");

    REQUIRE(childIndex.has_value());
    REQUIRE(parentIndex.has_value());
    REQUIRE(rootIndex.has_value());
    REQUIRE(*childIndex < *parentIndex);
    REQUIRE(*parentIndex < *rootIndex);
}

TEST_CASE("NormalizeCommitPlanRepoPaths strips unregistered gitlink include pathspec",
          "[Unit][CommitPlan][Normalize][Gitlink]") {
    const auto root = UniqueTempWorkspace("normalize-unregistered-gitlink-pathspec");
    const auto child = (root / "HorizonDialogueDemo").lexically_normal();

    RequireGit(root, {"init"});
    RequireGit(root, {"config", "user.email", "tests@example.invalid"});
    RequireGit(root, {"config", "user.name", "Kog Tests"});

    WriteTextFile(root / "README.md", "root\n");
    RequireGit(root, {"add", "README.md"});
    RequireGit(root, {"commit", "-m", "docs: seed root"});

    std::filesystem::create_directories(child);
    RequireGit(child, {"init"});
    RequireGit(child, {"config", "user.email", "tests@example.invalid"});
    RequireGit(child, {"config", "user.name", "Kog Tests"});
    WriteTextFile(child / "README.md", "child\n");
    RequireGit(child, {"add", "README.md"});
    RequireGit(child, {"commit", "-m", "docs: seed child"});

    RequireGit(root, {"add", "HorizonDialogueDemo"});
    RequireGit(root, {"commit", "-m", "chore: add unregistered gitlink"});

    WriteTextFile(root / "README.md", "root changed\n");
    WriteTextFile(child / "README.md", "child moved\n");
    RequireGit(child, {"commit", "-am", "child moved"});

    const auto planText = std::string(R"({"stages":{"commit":[{"repo":".","commits":[{"message":"docs: update root","include":["README.md","HorizonDialogueDemo"],"exclude":[]}]}]}})");
    std::string error;
    const auto normalized = NormalizeCommitPlanRepoPaths(root, planText, &error);

    INFO(error);
    REQUIRE(normalized.has_value());
    REQUIRE(error.empty());

    const auto doc = nlohmann::json::parse(*normalized);
    const auto include = doc["stages"]["commit"][0]["commits"][0]["include"].get<std::vector<std::string>>();
    REQUIRE(std::find(include.begin(), include.end(), "README.md") != include.end());
    REQUIRE(std::find(include.begin(), include.end(), "HorizonDialogueDemo") == include.end());
}
