#include <catch2/catch_test_macros.hpp>

#include "commit_ai_utils.hpp"
#include "shell_executor.hpp"

#include <filesystem>
#include <fstream>
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

} // namespace

TEST_CASE("BuildCommitScopeRecords includes dirty nested git repos seen only through parent status",
          "[Unit][CommitScope][Subrepo]") {
    const auto root = UniqueTempWorkspace("dirty-nested-repo");
    const auto child = (root / "child-skill").lexically_normal();

    RequireGit(root, {"init"});
    RequireGit(root, {"config", "user.email", "tests@example.invalid"});
    RequireGit(root, {"config", "user.name", "Kog Tests"});

    std::filesystem::create_directories(child);
    RequireGit(child, {"init"});
    RequireGit(child, {"config", "user.email", "tests@example.invalid"});
    RequireGit(child, {"config", "user.name", "Kog Tests"});
    WriteTextFile(child / "README.md", "initial\n");
    RequireGit(child, {"add", "README.md"});
    RequireGit(child, {"commit", "-m", "docs: initial child"});

    // Stage and commit the nested repository as a gitlink in the parent. Later
    // child-only edits make parent status show a dirty git directory even though
    // `git add -A` in the parent cannot stage the child's actual file changes.
    RequireGit(root, {"add", "child-skill"});
    RequireGit(root, {"commit", "-m", "chore: add child gitlink"});

    WriteTextFile(child / "README.md", "initial\nupdated\n");

    const auto records = BuildCommitScopeRecords(root, "", false, true);

    REQUIRE(ContainsRepoPath(records, child));
}
