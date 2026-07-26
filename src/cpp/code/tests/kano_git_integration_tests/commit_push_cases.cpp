#include "integration_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace kano::git::tests::integration {

TEST_CASE("prepared plan-file commit-push publishes through one native process route",
          "[integration][commit-push][plan-file][local-remote]") {
    const auto ctx = CreateRemoteWithClone("plan-file-commit-push");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nplan-file integration update\n");

    const auto planPath =
        (ctx.cloneRepo / ".kano" / "cache" / "git" / "plans" / "integration-plan.json").lexically_normal();
    RequireSuccess(
        functional::RunKog({"plan", "new", "--force", "--output", planPath.string()}, ctx.cloneRepo),
        "create integration commit plan");
    RequireSuccess(
        functional::RunKog(
            {
                "plan", "prepare", "add-commit-entry",
                "--plan-file", planPath.string(),
                "--repo", ".",
                "--commit-message", "test(integration): publish prepared plan",
                "--commit-include", "README.md",
                "--commit-review-verdict", "pass",
                "--commit-review-reason", "native integration lane"
            },
            ctx.cloneRepo),
        "add integration commit entry");
    RequireSuccess(
        functional::RunKog(
            {"plan", "verify", "pre-apply", "--stage", "commit", "--plan-file", planPath.string()},
            ctx.cloneRepo),
        "verify integration commit plan");

    const auto result = functional::RunKog(
        {"commit-push", "--plan-file", planPath.string()},
        ctx.cloneRepo);
    RequireSuccess(result, "apply and publish integration commit plan");
    REQUIRE(result.stdoutText.find("[commit-push][plan-pipeline] stage=push start") != std::string::npos);
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == CurrentHeadSha(ctx.cloneRepo));
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
    const auto [behind, ahead] = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(behind == 0);
    REQUIRE(ahead == 0);

    RemoveRemoteContext(ctx);
}

TEST_CASE("unrecoverable detached HEAD aborts before commit or push",
          "[integration][commit-push][detached-head][fail-fast]") {
    const auto ctx = CreateRemoteWithClone("detached-head-fail-fast", "weirdbranch");
    const auto originalHead = CurrentHeadSha(ctx.cloneRepo);
    RequireSuccess(functional::RunGit({"checkout", "--detach", originalHead}, ctx.cloneRepo),
                   "detach integration checkout");
    RequireSuccess(functional::RunGit({"update-ref", "-d", "refs/heads/" + ctx.branch}, ctx.bareRemote),
                   "delete remote recovery branch");
    RequireSuccess(functional::RunGit({"branch", "-D", ctx.branch}, ctx.cloneRepo),
                   "delete local recovery branch");
    RequireSuccess(functional::RunGit({"update-ref", "-d", "refs/remotes/origin/HEAD"}, ctx.cloneRepo),
                   "delete origin HEAD hint");
    RequireSuccess(
        functional::RunGit({"update-ref", "-d", "refs/remotes/origin/" + ctx.branch}, ctx.cloneRepo),
        "delete origin branch hint");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\ndetached dirty change\n");

    const auto result = functional::RunKog(
        {"commit-push", "-m", "test(integration): must not commit detached work"},
        ctx.cloneRepo);
    RequireFailure(result, "unrecoverable detached HEAD must fail");
    REQUIRE(result.elapsedMs < 15000);
    REQUIRE((result.stdoutText + "\n" + result.stderrText).find("detached HEAD") != std::string::npos);
    REQUIRE(CurrentBranch(ctx.cloneRepo).empty());
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == originalHead);
    REQUIRE(StatusPorcelain(ctx.cloneRepo).find("README.md") != std::string::npos);

    RemoveRemoteContext(ctx);
}

TEST_CASE("clean-ahead commit-push publishes instead of reporting a false no-op",
          "[integration][commit-push][clean-ahead][local-remote]") {
    const auto ctx = CreateRemoteWithClone("clean-ahead");
    WriteTextFile(ctx.cloneRepo / "ahead.txt", "local ahead commit\n");
    RequireSuccess(functional::RunGit({"add", "ahead.txt"}, ctx.cloneRepo),
                   "stage clean-ahead change");
    RequireSuccess(functional::RunGit({"commit", "-m", "test(integration): clean ahead"}, ctx.cloneRepo),
                   "create clean-ahead commit");
    REQUIRE(StatusPorcelain(ctx.cloneRepo).empty());
    const auto before = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(before.first == 0);
    REQUIRE(before.second == 1);

    const auto result = functional::RunKog({"commit-push"}, ctx.cloneRepo);
    RequireSuccess(result, "publish clean-ahead commit");
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == CurrentHeadSha(ctx.cloneRepo));
    const auto after = AheadBehindCounts(ctx.cloneRepo);
    REQUIRE(after.first == 0);
    REQUIRE(after.second == 0);

    RemoveRemoteContext(ctx);
}

} // namespace kano::git::tests::integration
