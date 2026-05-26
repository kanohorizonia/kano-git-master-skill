#include <catch2/catch_test_macros.hpp>

#include "commit_ai_utils.hpp"
#include "plan_utils.hpp"
#include "shell_executor.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace kano::git::commands;

namespace {

auto UniqueTempWorkspace(const std::string& InSuffix) -> std::filesystem::path {
    const auto root = (std::filesystem::temp_directory_path() / "kog-commit-plan-normalization-tests" / InSuffix).lexically_normal();
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

auto NormalizePlan(const std::filesystem::path& InWorkspaceRoot, const std::string& InPlanText, std::string* OutError = nullptr) -> std::optional<std::string> {
    return NormalizeCommitPlanRepoPaths(InWorkspaceRoot, InPlanText, OutError);
}

auto ParsePlan(const std::string& InPlanText) -> nlohmann::json {
    return nlohmann::json::parse(InPlanText);
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

    RequireGit(repoRoot, {"init"});
    RequireGit(repoRoot, {"config", "user.email", "tests@example.invalid"});
    RequireGit(repoRoot, {"config", "user.name", "Kog Tests"});
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

    RequireGit(repoRoot, {"init"});
    RequireGit(repoRoot, {"config", "user.email", "tests@example.invalid"});
    RequireGit(repoRoot, {"config", "user.name", "Kog Tests"});
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
