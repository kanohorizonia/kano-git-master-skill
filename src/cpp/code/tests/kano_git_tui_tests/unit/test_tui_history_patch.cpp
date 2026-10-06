#include <catch2/catch_test_macros.hpp>

#include "shell_executor.hpp"
#include "tui_history_patch.hpp"

#include <algorithm>
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

auto FindStatusEntry(
    const kano::git::commands::TuiPorcelainParseResult& InStatus,
    const std::string& InPath)
    -> const kano::git::commands::TuiPorcelainPath* {
    const auto entry = std::find_if(
        InStatus.entries.begin(),
        InStatus.entries.end(),
        [&](const auto& InEntry) { return InEntry.path == InPath; });
    return entry == InStatus.entries.end() ? nullptr : &*entry;
}

}

TEST_CASE("TUI history patch rejects an empty commit SHA",
          "[tdd][unit][feature:tui-history-patch][KG-TSK-0070]") {
    using namespace kano::git::commands;

    REQUIRE(FetchCommitDetail({}, "", 0) == "(invalid commit sha)");
    REQUIRE(FetchCommitFilePatch({}, "", "file.txt") == "(invalid commit sha)");
}

TEST_CASE("TUI history parses NUL name-status paths including rename and newline bytes",
          "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    std::string raw;
    raw.append("M", 1);
    raw.push_back('\0');
    raw.append("path\nwith-newline.txt");
    raw.push_back('\0');
    raw.append("R100");
    raw.push_back('\0');
    raw.append("old name.txt");
    raw.push_back('\0');
    raw.append("new name.txt");
    raw.push_back('\0');

    const auto paths = ParseHistoryNameStatusZ(raw);
    REQUIRE_FALSE(paths.truncated);
    REQUIRE_FALSE(paths.malformed);
    REQUIRE(paths.entries.size() == 2);
    REQUIRE(paths.entries[0].status == "M");
    REQUIRE(paths.entries[0].path == "path\nwith-newline.txt");
    REQUIRE(paths.entries[0].previousPath.empty());
    REQUIRE(paths.entries[1].status == "R100");
    REQUIRE(paths.entries[1].previousPath == "old name.txt");
    REQUIRE(paths.entries[1].path == "new name.txt");
}

TEST_CASE(
    "TUI history name-status parser enforces byte and entry bounds",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    std::string raw;
    for (const auto path : {"one.txt", "two.txt", "three.txt"}) {
        raw.append("M", 1);
        raw.push_back('\0');
        raw.append(path);
        raw.push_back('\0');
    }

    const auto entryBound = ParseHistoryNameStatusZ(
        raw,
        {.maxBytes = raw.size(), .maxEntries = 2});
    REQUIRE(entryBound.entries.size() == 2);
    REQUIRE(entryBound.truncated);
    REQUIRE_FALSE(entryBound.malformed);

    const auto byteBound = ParseHistoryNameStatusZ(
        raw,
        {.maxBytes = 4, .maxEntries = 10});
    REQUIRE(byteBound.entries.empty());
    REQUIRE(byteBound.truncated);
    REQUIRE_FALSE(byteBound.malformed);
}

TEST_CASE(
    "TUI porcelain parser preserves rename and copy path field order",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    std::string raw;
    raw += "R  new\nname -> literal.txt";
    raw.push_back('\0');
    raw.append("old\tname \"quoted\".txt");
    raw.push_back('\0');
    raw.append("C  copied name.txt");
    raw.push_back('\0');
    raw.append("source name.txt");
    raw.push_back('\0');

    const auto status = ParseTuiPorcelainV1Z(raw);
    REQUIRE_FALSE(status.truncated);
    REQUIRE_FALSE(status.malformed);
    REQUIRE(status.entries.size() == 2);
    CHECK(status.entries[0].indexStatus == 'R');
    CHECK(status.entries[0].path == "new\nname -> literal.txt");
    CHECK(status.entries[0].previousPath == "old\tname \"quoted\".txt");
    CHECK(status.entries[1].indexStatus == 'C');
    CHECK(status.entries[1].path == "copied name.txt");
    CHECK(status.entries[1].previousPath == "source name.txt");
    CHECK(EscapeTuiPathForDisplay(status.entries[0].path) ==
          "new\\nname -> literal.txt");
}

TEST_CASE(
    "TUI porcelain parser enforces byte and entry bounds",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    const std::string raw =
        std::string("?? first.txt\0", 13) +
        std::string("?? second.txt\0", 14) +
        std::string("?? third.txt\0", 13);

    const auto entryBound = ParseTuiPorcelainV1Z(
        raw,
        {.maxBytes = raw.size(), .maxEntries = 2});
    REQUIRE(entryBound.entries.size() == 2);
    REQUIRE(entryBound.truncated);
    REQUIRE_FALSE(entryBound.malformed);

    const auto byteBound = ParseTuiPorcelainV1Z(
        raw,
        {.maxBytes = 10, .maxEntries = 10});
    REQUIRE(byteBound.entries.empty());
    REQUIRE(byteBound.truncated);
    REQUIRE_FALSE(byteBound.malformed);
}

TEST_CASE(
    "TUI display paths preserve valid UTF-8 and expose C1 or invalid bytes",
    "[unit][tui_history_patch][tui_pr_focus][KG-BUG-0088]") {
    using namespace kano::git::commands;

    const std::string valid = "資料夾/檔案.txt";
    REQUIRE(EscapeTuiPathForDisplay(valid) == valid);

    const std::string encodedC1{"before\xC2\x9B" "after"};
    REQUIRE(EscapeTuiPathForDisplay(encodedC1) ==
            "before\\u009Bafter");

    const std::string invalidByte{"bad\x9B" "name"};
    REQUIRE(EscapeTuiPathForDisplay(invalidByte) ==
            "bad\\x9Bname");
}

TEST_CASE(
    "TUI working-tree status preserves exact NUL-delimited path identities",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    const std::string modifiedPath = "literal -> arrow\t\"quoted\".txt";
    const std::string oldPath = "old\tname -> \"quoted\".txt";
    const std::string newPath = "new\nname -> \"quoted\".txt";
    const std::string untrackedPath = "untracked\nname\t\"quoted\" -> literal.txt";

    WriteFile(repo.Path() / modifiedPath, "baseline\n");
    WriteFile(repo.Path() / oldPath, "rename fixture\n");
    CommitAll(repo.Path(), "baseline weird paths");
    WriteFile(repo.Path() / modifiedPath, "baseline\nexact modified fixture\n");
    RequireGitSuccess(
        repo.Path(),
        {"mv", oldPath, newPath},
        "rename exact path fixture");
    WriteFile(repo.Path() / untrackedPath, "exact untracked fixture\n");

    const auto rawStatus = RunGit(
        repo.Path(),
        {"status", "--porcelain=v1", "-z"});
    INFO(rawStatus.stderrStr);
    REQUIRE(rawStatus.exitCode == 0);
    const auto status = ParseTuiPorcelainV1Z(rawStatus.stdoutStr);
    REQUIRE_FALSE(status.truncated);
    REQUIRE_FALSE(status.malformed);

    const auto* modified = FindStatusEntry(status, modifiedPath);
    REQUIRE(modified != nullptr);
    CHECK(modified->previousPath.empty());
    const auto modifiedPatch = FetchWorkingTreeFilePatch(
        repo.Path(),
        modified->path,
        modified->previousPath);
    CHECK(modifiedPatch.find("+exact modified fixture") != std::string::npos);

    const auto* renamed = FindStatusEntry(status, newPath);
    REQUIRE(renamed != nullptr);
    CHECK(renamed->indexStatus == 'R');
    CHECK(renamed->previousPath == oldPath);
    const auto renamePatch = FetchWorkingTreeFilePatch(
        repo.Path(),
        renamed->path,
        renamed->previousPath);
    CHECK(renamePatch.find("rename from") != std::string::npos);
    CHECK(renamePatch.find("rename to") != std::string::npos);

    const auto* untracked = FindStatusEntry(status, untrackedPath);
    REQUIRE(untracked != nullptr);
    CHECK(untracked->indexStatus == '?');
    const auto untrackedPatch = FetchWorkingTreeFilePatch(
        repo.Path(),
        untracked->path,
        untracked->previousPath);
    CHECK(untrackedPatch.find("+exact untracked fixture") != std::string::npos);
}

TEST_CASE(
    "TUI patch detail budget caps Git fetch launches and bytes",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TuiDetailPatchBudget budget;
    std::size_t launchedFetches = 0;
    for (std::size_t changedFile = 0; changedFile < 1000; ++changedFile) {
        if (!TryStartTuiDetailPatchFetch(budget)) {
            continue;
        }
        launchedFetches += 1;
    }

    REQUIRE(launchedFetches == kTuiDetailPatchFetchLimit);
    REQUIRE(budget.startedFetches == kTuiDetailPatchFetchLimit);
    REQUIRE_FALSE(TryStartTuiDetailPatchFetch(budget));
    REQUIRE(kTuiDetailPatchMaxBytes ==
            kTuiDetailPatchFetchLimit * kTuiFilePatchMaxBytes);
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
    const std::string oldPath = "old\nname.txt";
    const std::string newPath = "new\tname.txt";
    WriteFile(repo.Path() / oldPath, "rename fixture\n");
    CommitAll(repo.Path(), "add old path");
    RequireGitSuccess(repo.Path(), {"mv", oldPath, newPath}, "rename fixture file");
    CommitAll(repo.Path(), "rename path");

    const auto patch = FetchCommitFilePatch(
        repo.Path(), HeadSha(repo.Path()), newPath, oldPath);
    REQUIRE(patch.find("similarity index") != std::string::npos);
    REQUIRE(patch.find("rename from") != std::string::npos);
    REQUIRE(patch.find("rename to") != std::string::npos);
    REQUIRE(EscapeTuiPathForDisplay(oldPath) == "old\\nname.txt");
    REQUIRE(EscapeTuiPathForDisplay(newPath) == "new\\tname.txt");
}

TEST_CASE(
    "TUI working-tree patch launches HEAD fallback lazily and cancels between probes",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "tracked.txt", "baseline\n");
    CommitAll(repo.Path(), "baseline");
    WriteFile(repo.Path() / "tracked.txt", "baseline\nmodified\n");

    std::vector<std::vector<std::string>> launches;
    const TuiGitProbeControl observe{
        .onLaunch = [&](const auto& InArgs) { launches.push_back(InArgs); },
    };
    const auto patch = FetchWorkingTreeFilePatch(
        repo.Path(), "tracked.txt", {}, observe);
    REQUIRE(patch.find("+modified") != std::string::npos);
    REQUIRE(launches.size() == 2);
    REQUIRE(std::find(launches[0].begin(), launches[0].end(), "HEAD") ==
            launches[0].end());
    REQUIRE(std::find(launches[1].begin(), launches[1].end(), "HEAD") ==
            launches[1].end());

    launches.clear();
    const TuiGitProbeControl cancelAfterFirst{
        .isCancelled = [&]() { return !launches.empty(); },
        .onLaunch = [&](const auto& InArgs) { launches.push_back(InArgs); },
    };
    const auto cancelled = FetchWorkingTreeFilePatch(
        repo.Path(), "tracked.txt", {}, cancelAfterFirst);
    REQUIRE(cancelled == "(detail load cancelled)");
    REQUIRE(launches.size() == 1);
}

TEST_CASE(
    "TUI commit detail cancellation prevents the first and later Git probes",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "tracked.txt", "baseline\n");
    CommitAll(repo.Path(), "baseline");
    const auto sha = HeadSha(repo.Path());

    std::vector<std::vector<std::string>> launches;
    const TuiGitProbeControl cancelBeforeFirst{
        .isCancelled = []() { return true; },
        .onLaunch = [&](const auto& InArgs) { launches.push_back(InArgs); },
    };
    REQUIRE(FetchCommitDetail(repo.Path(), sha, 2, cancelBeforeFirst) ==
            "(detail load cancelled)");
    REQUIRE(launches.empty());

    const TuiGitProbeControl cancelBeforeFallback{
        .isCancelled = [&]() { return !launches.empty(); },
        .onLaunch = [&](const auto& InArgs) { launches.push_back(InArgs); },
    };
    REQUIRE(FetchCommitFilePatch(
                repo.Path(),
                sha,
                "path-that-is-not-in-the-commit.txt",
                {},
                cancelBeforeFallback) == "(detail load cancelled)");
    REQUIRE(launches.size() == 1);

    launches.clear();
    REQUIRE(FetchCommitFilePatch(
                repo.Path(), sha, {}, {}, cancelBeforeFirst) ==
            "(detail load cancelled)");
    REQUIRE(launches.empty());
}

TEST_CASE(
    "TUI shared probe gate prevents a history follow-up launch after cancellation",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    std::vector<std::vector<std::string>> launches;
    const TuiGitProbeControl cancelAfterFirst{
        .isCancelled = [&]() { return !launches.empty(); },
        .onLaunch = [&](const auto& InArgs) { launches.push_back(InArgs); },
    };

    REQUIRE(TryBeginTuiGitProbe(
        cancelAfterFirst,
        {"rev-parse", "--verify", "--quiet", "HEAD"}));
    REQUIRE_FALSE(TryBeginTuiGitProbe(
        cancelAfterFirst,
        {"log", "--max-count=21", "HEAD"}));
    REQUIRE(launches.size() == 1);
}

TEST_CASE(
    "TUI working-tree patch retains at most the documented file budget",
    "[unit][tui_history_patch][KG-BUG-0088]") {
    using namespace kano::git::commands;

    TempRepo repo;
    InitializeRepo(repo.Path());
    WriteFile(repo.Path() / "large.txt", "baseline\n");
    CommitAll(repo.Path(), "baseline");
    WriteFile(repo.Path() / "large.txt", std::string(100000, 'x') + "\n");

    const auto patch = FetchWorkingTreeFilePatch(repo.Path(), "large.txt");
    REQUIRE(patch.size() <= kTuiFilePatchMaxBytes);
    REQUIRE(patch.find("truncated") != std::string::npos);
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
    REQUIRE(detail.size() == 20000);
    REQUIRE(detail.ends_with(kSuffix));
}
