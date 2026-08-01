#include <catch2/catch_test_macros.hpp>

#include "tui_history_lifecycle.hpp"

#include <string>
#include <vector>

using namespace kano::git::commands;

TEST_CASE(
    "TUI history batch cancels between anchor and log without a second launch",
    "[unit][tui_history_lifecycle][KG-BUG-0088]") {
    std::vector<std::vector<std::string>> launches;
    int executorCalls = 0;
    const TuiGitProbeControl cancelAfterAnchor{
        .isCancelled = [&]() { return !launches.empty(); },
        .onLaunch = [&](const auto& InArguments) {
            launches.push_back(InArguments);
        },
    };
    const TuiHistoryProbeExecutor executor =
        [&](const std::filesystem::path&,
            const std::vector<std::string>& InArguments) {
            ++executorCalls;
            REQUIRE(InArguments.front() == "rev-parse");
            return TuiHistoryProbeResult{
                .stdoutText = "0123456789abcdef\n",
            };
        };

    const auto batch = FetchTuiHistoryBatch(
        "/workspace/repo",
        0,
        21,
        {},
        cancelAfterAnchor,
        executor);

    REQUIRE(batch.errorMessage == "history load cancelled");
    REQUIRE(batch.entries.empty());
    REQUIRE(launches.size() == 1);
    REQUIRE(executorCalls == 1);
}

TEST_CASE(
    "TUI history batch launches one anchor and one bounded log probe",
    "[unit][tui_history_lifecycle][KG-BUG-0088]") {
    std::vector<std::vector<std::string>> launches;
    const TuiGitProbeControl observe{
        .onLaunch = [&](const auto& InArguments) {
            launches.push_back(InArguments);
        },
    };
    const TuiHistoryProbeExecutor executor =
        [](const std::filesystem::path&,
           const std::vector<std::string>& InArguments) {
            if (InArguments.front() == "rev-parse") {
                return TuiHistoryProbeResult{
                    .stdoutText = "0123456789abcdef\n",
                };
            }
            std::string payload;
            payload += "0123456";
            payload.push_back('\0');
            payload += "subject\x1f" "trusted";
            payload.push_back('\0');
            payload += "agent@example.com";
            payload.push_back('\0');
            payload += "Agent";
            payload.push_back('\0');
            return TuiHistoryProbeResult{
                .stdoutText = payload,
            };
        };

    const auto batch = FetchTuiHistoryBatch(
        "/workspace/repo", 0, 21, {}, observe, executor);

    REQUIRE(batch.errorMessage.empty());
    REQUIRE(batch.anchorSha == "0123456789abcdef");
    REQUIRE(batch.entries.size() == 1);
    REQUIRE(batch.entries.front().subject == "subject\x1f" "trusted");
    REQUIRE(batch.entries.front().authorEmail == "agent@example.com");
    REQUIRE(batch.entries.front().authorName == "Agent");
    REQUIRE(launches.size() == 2);
    REQUIRE(launches[0].front() == "rev-parse");
    REQUIRE(launches[1].front() == "log");
}

TEST_CASE(
    "TUI history batch rejects a truncated page instead of parsing partial data",
    "[unit][tui_history_lifecycle][KG-BUG-0088]") {
    const TuiHistoryProbeExecutor executor =
        [](const std::filesystem::path&,
           const std::vector<std::string>&) {
            std::string payload;
            payload += "0123456";
            payload.push_back('\0');
            payload += "partial";
            payload.push_back('\0');
            return TuiHistoryProbeResult{
                .stdoutText = payload,
                .stdoutTruncated = true,
            };
        };

    const auto batch = FetchTuiHistoryBatch(
        "/workspace/repo",
        0,
        21,
        "0123456789abcdef",
        {},
        executor);

    REQUIRE(batch.entries.empty());
    REQUIRE_FALSE(batch.reachedEnd);
    REQUIRE(batch.errorMessage.find("no partial page was accepted") !=
            std::string::npos);
}

TEST_CASE(
    "TUI history batch rejects an incomplete NUL-delimited audit record",
    "[unit][tui_history_lifecycle][KG-BUG-0088]") {
    const TuiHistoryProbeExecutor executor =
        [](const std::filesystem::path&,
           const std::vector<std::string>&) {
            std::string payload;
            payload += "0123456";
            payload.push_back('\0');
            payload += "subject";
            payload.push_back('\0');
            payload += "agent@example.com";
            payload.push_back('\0');
            payload += "Agent";
            return TuiHistoryProbeResult{
                .stdoutText = payload,
            };
        };

    const auto batch = FetchTuiHistoryBatch(
        "/workspace/repo",
        0,
        21,
        "0123456789abcdef",
        {},
        executor);

    REQUIRE(batch.entries.empty());
    REQUIRE_FALSE(batch.reachedEnd);
    REQUIRE(batch.errorMessage.find("malformed NUL-delimited") !=
            std::string::npos);
}

TEST_CASE(
    "TUI refresh reconciliation keeps history on the same repository and closes stale detail",
    "[unit][tui_history_lifecycle][KG-BUG-0088]") {
    SECTION("repository order changes") {
        const std::vector<std::string> refreshedRepoCanonicalPaths{
            "/workspace/repo-c",
            "/workspace/repo-a",
            "/workspace/repo-b",
        };

        const auto reconciliation = ReconcileHistoryAfterRepoRefresh(
            "/workspace/repo-b",
            refreshedRepoCanonicalPaths);

        REQUIRE(reconciliation.newRepoIndex == 2);
        REQUIRE_FALSE(reconciliation.bCloseHistory);
        REQUIRE(reconciliation.bCloseDetail);
    }

    SECTION("active repository disappears") {
        const std::vector<std::string> refreshedRepoCanonicalPaths{
            "/workspace/repo-a",
            "/workspace/repo-c",
        };

        const auto reconciliation = ReconcileHistoryAfterRepoRefresh(
            "/workspace/repo-b",
            refreshedRepoCanonicalPaths);

        REQUIRE_FALSE(reconciliation.newRepoIndex.has_value());
        REQUIRE(reconciliation.bCloseHistory);
        REQUIRE(reconciliation.bCloseDetail);
    }
}

TEST_CASE(
    "TUI history fetch plan stays page bounded with one look-ahead row",
    "[unit][tui_history_lifecycle][KG-BUG-0088]") {
    const auto firstPage = PlanBoundedHistoryFetch(0, 20, 0);
    REQUIRE(firstPage.requiredEntries == 20);
    REQUIRE(firstPage.fetchCount == 21);

    const auto secondPage = PlanBoundedHistoryFetch(1, 20, 21);
    REQUIRE(secondPage.requiredEntries == 40);
    REQUIRE(secondPage.fetchCount == 20);

    const auto alreadyLoaded = PlanBoundedHistoryFetch(1, 20, 41);
    REQUIRE(alreadyLoaded.fetchCount == 0);

    const auto staleFarPage = PlanBoundedHistoryFetch(100, 20, 0);
    REQUIRE(staleFarPage.fetchCount == 21);
}

TEST_CASE(
    "TUI history keeps captured page boundaries across terminal resize",
    "[unit][tui_history_lifecycle][KG-TSK-0070]") {
    auto paging = BeginHistoryPaging(20);
    paging.pageIndex = 3;
    paging.selectedLine = 7;

    const auto resized =
        ReconcileHistoryPagingAfterResize(paging, 8);
    REQUIRE(resized.pageSize == 20);
    REQUIRE(resized.pageIndex == 3);
    REQUIRE(resized.selectedLine == 7);

    const auto samePageFetch = PlanBoundedHistoryFetch(
        resized.pageIndex,
        resized.pageSize,
        61);
    REQUIRE(samePageFetch.requiredEntries == 80);
    REQUIRE(samePageFetch.fetchCount == 20);

    const auto reopened = BeginHistoryPaging(8);
    REQUIRE(reopened.pageSize == 8);
    REQUIRE(reopened.pageIndex == 0);
    REQUIRE(reopened.selectedLine == 0);
}
