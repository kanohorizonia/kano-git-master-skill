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
#include <vector>

using namespace kano::git::commands;

namespace {

auto UniqueTempWorkspace(const std::string& InSuffix) -> std::filesystem::path {
    const auto root = (std::filesystem::temp_directory_path() / "kog-commit-plan-normalization-tests" / InSuffix).lexically_normal();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (!ec) {
        return canonical;
    }
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

void InitGitRepo(const std::filesystem::path& InRepo) {
    std::error_code ec;
    std::filesystem::create_directories(InRepo, ec);
    REQUIRE(!ec);
    RequireGit(InRepo, {"init"});
    RequireGit(InRepo, {"config", "user.email", "tests@example.invalid"});
    RequireGit(InRepo, {"config", "user.name", "Kog Tests"});
}

auto NormalizePlan(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText, std::string* OutError = nullptr) -> std::optional<std::string> {
    return NormalizeCommitPlanRepoPaths(InWorkspaceRoot, InPlanText, OutError);
}

auto ParsePlan(const std::string& InPlanText) -> nlohmann::json {
    return nlohmann::json::parse(InPlanText);
}

auto BuildSingleCommitPlan(const std::string& InRepo,
                           const std::vector<std::string>& InInclude,
                           const nlohmann::json& InExclude = nlohmann::json::array()) -> std::string {
    nlohmann::json commit;
    commit["message"] = "test(commit-plan): pathspec fixture";
    commit["include"] = InInclude;
    commit["exclude"] = InExclude;

    nlohmann::json repoObject;
    repoObject["repo"] = InRepo;
    repoObject["commits"] = nlohmann::json::array({commit});

    nlohmann::json doc;
    doc["stages"]["commit"] = nlohmann::json::array({repoObject});
    return doc.dump();
}

} // namespace

TEST_CASE("NormalizeCommitPlanRepoPaths rejects phantom repo paths", "[Unit][CommitPlan][Normalize]") {
    const auto workspace = UniqueTempWorkspace("reject-phantom-repo");
    const auto planText = R"({"stages":{"commit":[{"repo":"ghost/repo","commits":[{"include":["README.md"],"exclude":[]}]}]}})";

    std::string error;
    const auto normalized = NormalizePlan(workspace, planText, &error);

    REQUIRE_FALSE(normalized.has_value());
    REQUIRE(error.find("INVALID_PLAN_REPO_PATH") != std::string::npos);
}

TEST_CASE("NormalizeCommitPlanRepoPaths rewrites mistaken subdir repos to the real worktree root",
          "[Unit][CommitPlan][Normalize]") {
    const auto workspace = UniqueTempWorkspace("rewrite-mistaken-subdir-repo");
    const auto repoRoot = (workspace / "parent-skill").lexically_normal();
    const auto mistakenRepo = (repoRoot / "public" / "src" / "cpp").lexically_normal();

    InitGitRepo(repoRoot);
    std::filesystem::create_directories(mistakenRepo);
    WriteTextFile(repoRoot / "README.md", "root file\n");

    const auto planText = std::string(R"({"stages":{"commit":[{"repo":")") + mistakenRepo.generic_string() + R"(","commits":[{"include":["README.md"],"exclude":[]}]}]}})";

    std::string error;
    const auto normalized = NormalizePlan(workspace, planText, &error);

    REQUIRE(normalized.has_value());
    REQUIRE(error.empty());

    const auto doc = ParsePlan(*normalized);
    REQUIRE(doc["stages"]["commit"].is_array());
    REQUIRE(doc["stages"]["commit"].size() == 1);
    REQUIRE(doc["stages"]["commit"][0]["repo"].get<std::string>() == "parent-skill");
    REQUIRE(doc["stages"]["commit"][0]["commits"][0]["include"].get<std::vector<std::string>>() == std::vector<std::string>{"README.md"});
}

TEST_CASE("NormalizeCommitPlanRepoPaths accepts deleted tracked files under the resolved repo root",
          "[Unit][CommitPlan][Normalize]") {
    const auto workspace = UniqueTempWorkspace("deleted-tracked-file-pathspec");
    const auto repoRoot = (workspace / "parent-skill").lexically_normal();
    const auto mistakenRepo = (repoRoot / "public" / "src" / "cpp").lexically_normal();

    InitGitRepo(repoRoot);
    std::filesystem::create_directories(mistakenRepo);
    WriteTextFile(repoRoot / "deleted.txt", "tracked then removed\n");
    RequireGit(repoRoot, {"add", "deleted.txt"});
    RequireGit(repoRoot, {"commit", "-m", "docs: add tracked file"});
    std::filesystem::remove(repoRoot / "deleted.txt");

    const auto planText = std::string(R"({"stages":{"commit":[{"repo":")") + mistakenRepo.generic_string() + R"(","commits":[{"include":["deleted.txt"],"exclude":[]}]}]}})";

    std::string error;
    const auto normalized = NormalizePlan(workspace, planText, &error);

    REQUIRE(normalized.has_value());
    REQUIRE(error.empty());

    const auto doc = ParsePlan(*normalized);
    REQUIRE(doc["stages"]["commit"][0]["repo"].get<std::string>() == "parent-skill");
    REQUIRE(doc["stages"]["commit"][0]["commits"][0]["include"].get<std::vector<std::string>>() == std::vector<std::string>{"deleted.txt"});
}

TEST_CASE("NormalizeCommitPlanRepoPaths accepts staged rename old pathspecs tracked in HEAD",
          "[Unit][CommitPlan][Normalize]") {
    const auto workspace = UniqueTempWorkspace("staged-rename-old-pathspec");
    const auto repoRoot = (workspace / "parent-skill").lexically_normal();

    InitGitRepo(repoRoot);
    WriteTextFile(repoRoot / "old-name.md", "tracked then renamed\n");
    RequireGit(repoRoot, {"add", "old-name.md"});
    RequireGit(repoRoot, {"commit", "-m", "docs: add rename fixture"});
    RequireGit(repoRoot, {"mv", "old-name.md", "new-name.md"});

    const auto planText = BuildSingleCommitPlan(repoRoot.generic_string(), {"old-name.md", "new-name.md"});

    std::string error;
    const auto normalized = NormalizePlan(workspace, planText, &error);

    REQUIRE(normalized.has_value());
    REQUIRE(error.empty());

    const auto doc = ParsePlan(*normalized);
    REQUIRE(doc["stages"]["commit"][0]["repo"].get<std::string>() == "parent-skill");
    REQUIRE(doc["stages"]["commit"][0]["commits"][0]["include"].get<std::vector<std::string>>() == std::vector<std::string>{"old-name.md", "new-name.md"});
}

TEST_CASE("NormalizeCommitPlanRepoPaths normalizes AI-corrupted include pathspec variants",
          "[Unit][CommitPlan][Normalize][Pathspec]") {
    const auto workspace = UniqueTempWorkspace("normalize-ai-pathspec-fixtures");
    const auto repoRoot = (workspace / "parent-skill").lexically_normal();
    const auto mistakenRepo = (repoRoot / "public" / "src" / "cpp").lexically_normal();

    InitGitRepo(repoRoot);

    WriteTextFile(repoRoot / "public" / "src" / "cpp" / "include" / "widget.hpp", "#pragma once\n");
    WriteTextFile(repoRoot / "docs" / "plan.md", "# Plan\n");

    struct Fixture {
        std::string name;
        std::string repo;
        std::vector<std::string> include;
        std::vector<std::string> expectedInclude;
    };

    const std::vector<Fixture> fixtures{
        {"windows separators relative to mistaken repo prefix",
         mistakenRepo.generic_string(),
         {"include\\widget.hpp"},
         {"public/src/cpp/include/widget.hpp"}},
        {"newline wrapped repo-prefixed path",
         repoRoot.generic_string(),
         {" public/\n src /cpp/include/widget.hpp "},
         {"public/src/cpp/include/widget.hpp"}},
        {"absolute path emitted by AI",
         repoRoot.generic_string(),
         {(repoRoot / "docs" / "plan.md").string()},
         {"docs/plan.md"}},
    };

    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.name);
        std::string error;
        const auto normalized = NormalizePlan(workspace, BuildSingleCommitPlan(fixture.repo, fixture.include), &error);

        INFO(error);
        REQUIRE(normalized.has_value());
        REQUIRE(error.empty());

        const auto doc = ParsePlan(*normalized);
        REQUIRE(doc["stages"]["commit"][0]["repo"].get<std::string>() == "parent-skill");
        REQUIRE(doc["stages"]["commit"][0]["commits"][0]["include"].get<std::vector<std::string>>() == fixture.expectedInclude);
    }
}

TEST_CASE("NormalizeCommitPlanRepoPaths rejects ambiguous pathspecs after repo-prefix repair",
          "[Unit][CommitPlan][Normalize][Pathspec]") {
    const auto workspace = UniqueTempWorkspace("reject-ambiguous-ai-pathspec");
    const auto repoRoot = (workspace / "parent-skill").lexically_normal();
    const auto mistakenRepo = (repoRoot / "public" / "src" / "cpp").lexically_normal();

    InitGitRepo(repoRoot);

    WriteTextFile(repoRoot / "README.md", "root\n");
    WriteTextFile(mistakenRepo / "README.md", "nested\n");

    std::string error;
    const auto normalized = NormalizePlan(workspace, BuildSingleCommitPlan(mistakenRepo.generic_string(), {"README.md"}), &error);

    REQUIRE_FALSE(normalized.has_value());
    REQUIRE(error.find("INVALID_PLAN_PATHSPEC: ambiguous pathspec") != std::string::npos);
}

TEST_CASE("NormalizeCommitPlanRepoPaths rejects schema-noise pathspec entries",
          "[Unit][CommitPlan][Normalize][Pathspec][Schema]") {
    const auto workspace = UniqueTempWorkspace("reject-schema-noise-pathspec");
    const auto repoRoot = (workspace / "parent-skill").lexically_normal();

    InitGitRepo(repoRoot);
    WriteTextFile(repoRoot / "README.md", "root\n");

    nlohmann::json commit;
    commit["message"] = "test(commit-plan): reject schema noise";
    commit["include"] = nlohmann::json::array();
    commit["include"].push_back("README.md");
    commit["include"].push_back(nlohmann::json{{"path", "README.md"}});
    commit["exclude"] = nlohmann::json::array();

    nlohmann::json repoObject;
    repoObject["repo"] = repoRoot.generic_string();
    repoObject["commits"] = nlohmann::json::array({commit});

    nlohmann::json doc;
    doc["stages"]["commit"] = nlohmann::json::array({repoObject});

    std::string error;
    const auto normalized = NormalizePlan(workspace, doc.dump(), &error);

    REQUIRE_FALSE(normalized.has_value());
    REQUIRE(error.find("INVALID_PLAN_PATHSPEC: include entries must be strings") != std::string::npos);
}
