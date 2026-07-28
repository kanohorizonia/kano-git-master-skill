#include <catch2/catch_test_macros.hpp>

#include "shell_executor.hpp"
#include "tui_history_patch.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kano::git::shell::ExecMode;
using kano::git::shell::ExecResult;
using kano::git::shell::ExecuteCommand;

class TempRepo {
public:
    TempRepo() {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        mPath = fs::temp_directory_path()
            / ("kog-tui-history-patch-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(mPath);
    }

    ~TempRepo() {
        std::error_code ec;
        fs::remove_all(mPath, ec);
    }

    TempRepo(const TempRepo&) = delete;
    auto operator=(const TempRepo&) -> TempRepo& = delete;

    [[nodiscard]] auto Path() const -> const fs::path& {
        return mPath;
    }

private:
    fs::path mPath;
};

auto RunGit(const fs::path& InRepo, const std::vector<std::string>& InArgs) -> ExecResult {
    return ExecuteCommand("git", InArgs, ExecMode::Capture, InRepo);
}

void RequireGitSuccess(const fs::path& InRepo,
                       const std::vector<std::string>& InArgs,
                       const std::string& InOperation) {
    const auto result = RunGit(InRepo, InArgs);
    INFO(InOperation);
    INFO(result.stderrStr);
    REQUIRE(result.exitCode == 0);
}

void InitializeRepo(const fs::path& InRepo) {
    RequireGitSuccess(InRepo, {"init", "-q"}, "initialize disposable repository");
    RequireGitSuccess(InRepo, {"config", "user.name", "KOG Test"}, "set repository-local user name");
    RequireGitSuccess(InRepo, {"config", "user.email", "kog-test@example.invalid"}, "set repository-local user email");
}

void WriteFile(const fs::path& InPath, const std::string& InBody) {
    std::ofstream output(InPath, std::ios::binary);
    REQUIRE(output.good());
    output << InBody;
    REQUIRE(output.good());
}

void CommitAll(const fs::path& InRepo, const std::string& InMessage) {
    RequireGitSuccess(InRepo, {"add", "--all"}, "stage fixture files");
    RequireGitSuccess(InRepo, {"commit", "-q", "-m", InMessage}, "commit fixture files");
}

auto HeadSha(const fs::path& InRepo) -> std::string {
    const auto result = RunGit(InRepo, {"rev-parse", "HEAD"});
    INFO(result.stderrStr);
    REQUIRE(result.exitCode == 0);
    auto sha = result.stdoutStr;
    while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) {
        sha.pop_back();
    }
    return sha;
}

}

TEST_CASE("TUI history patch rejects an empty commit SHA",
          "[tdd][unit][feature:tui-history-patch][KG-TSK-0070]") {
    using namespace kano::git::commands;

    REQUIRE(FetchCommitDetail({}, "", 0) == "(invalid commit sha)");
    REQUIRE(FetchCommitFilePatch({}, "", "file.txt") == "(invalid commit sha)");
}

TEST_CASE("TUI history patch reports a clean working tree",
          "[tdd][unit][feature:tui-history-patch][KG-TSK-0070]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "tracked.txt", "baseline\n");
    CommitAll(repo.Path(), "baseline");

    REQUIRE(FetchWorkingTreeDetail(repo.Path(), 2) == "working tree is clean");
}

TEST_CASE("TUI history patch falls back to a no-index diff for an untracked file",
          "[tdd][unit][feature:tui-history-patch][KG-TSK-0070]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "tracked.txt", "baseline\n");
    CommitAll(repo.Path(), "baseline");
    WriteFile(repo.Path() / "untracked.txt", "untracked fixture\n");

    const auto patch = FetchWorkingTreeFilePatch(repo.Path(), "untracked.txt");
    REQUIRE(patch.rfind("# untracked (new file)\n", 0) == 0);
    REQUIRE(patch.find("+untracked fixture") != std::string::npos);
}

TEST_CASE("TUI history patch supports both paths of a committed rename",
          "[tdd][unit][feature:tui-history-patch][KG-TSK-0070]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "old_name.txt", "rename fixture\n");
    CommitAll(repo.Path(), "add old path");
    RequireGitSuccess(repo.Path(), {"mv", "old_name.txt", "new_name.txt"}, "rename fixture file");
    CommitAll(repo.Path(), "rename path");

    const auto patch = FetchCommitFilePatch(repo.Path(), HeadSha(repo.Path()), "new_name.txt", "old_name.txt");
    REQUIRE(patch.find("rename from old_name.txt") != std::string::npos);
    REQUIRE(patch.find("rename to new_name.txt") != std::string::npos);
}

TEST_CASE("TUI history patch truncates commit detail after 20000 characters",
          "[tdd][unit][feature:tui-history-patch][KG-TSK-0070]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "large.txt", std::string(25000, 'x') + "\n");
    CommitAll(repo.Path(), "large patch");

    constexpr std::string_view kSuffix = "\n... (truncated)";
    const auto detail = FetchCommitDetail(repo.Path(), HeadSha(repo.Path()), 2);
    REQUIRE(detail.size() == 20000 + kSuffix.size());
    REQUIRE(detail.ends_with(kSuffix));
}
