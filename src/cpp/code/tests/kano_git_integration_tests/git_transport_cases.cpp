#include "integration_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace kano::git::tests::integration {

TEST_CASE("local bare remote supports real push and fetch convergence",
          "[integration][git-transport][local-remote][push][fetch]") {
    const auto ctx = CreateRemoteWithClone("transport-roundtrip");

    WriteTextFile(ctx.cloneRepo / "clone.txt", "published from clone\n");
    RequireSuccess(functional::RunGit({"add", "clone.txt"}, ctx.cloneRepo), "stage clone change");
    RequireSuccess(functional::RunGit({"commit", "-m", "test(integration): publish clone change"}, ctx.cloneRepo),
                   "commit clone change");
    RequireSuccess(functional::RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo),
                   "push clone change");
    REQUIRE(RefSha(ctx.bareRemote, "refs/heads/" + ctx.branch) == CurrentHeadSha(ctx.cloneRepo));

    WriteTextFile(ctx.seedRepo / "seed.txt", "published from seed\n");
    RequireSuccess(functional::RunGit({"add", "seed.txt"}, ctx.seedRepo), "stage seed change");
    RequireSuccess(functional::RunGit({"commit", "-m", "test(integration): publish seed change"}, ctx.seedRepo),
                   "commit seed change");
    RequireSuccess(functional::RunGit({"pull", "--rebase", "origin", ctx.branch}, ctx.seedRepo),
                   "rebase seed after clone publication");
    RequireSuccess(functional::RunGit({"push", "origin", ctx.branch}, ctx.seedRepo),
                   "push seed change");
    const auto publishedHead = CurrentHeadSha(ctx.seedRepo);

    RequireSuccess(functional::RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRepo),
                   "fetch updated remote branch");
    REQUIRE(RefSha(ctx.cloneRepo, "refs/remotes/origin/" + ctx.branch) == publishedHead);

    RemoveRemoteContext(ctx);
}

TEST_CASE("non-interactive rebase conflict fails without editor or prompt hang",
          "[integration][git-transport][rebase][conflict][non-interactive]") {
    const auto ctx = CreateRemoteWithClone("rebase-conflict");

    WriteTextFile(ctx.cloneRepo / "README.md", "local conflicting line\n");
    RequireSuccess(functional::RunGit({"commit", "-am", "test(integration): local conflict"}, ctx.cloneRepo),
                   "commit local conflict");
    const auto localHead = CurrentHeadSha(ctx.cloneRepo);

    WriteTextFile(ctx.seedRepo / "README.md", "remote conflicting line\n");
    RequireSuccess(functional::RunGit({"commit", "-am", "test(integration): remote conflict"}, ctx.seedRepo),
                   "commit remote conflict");
    RequireSuccess(functional::RunGit({"push", "origin", ctx.branch}, ctx.seedRepo),
                   "push remote conflict");
    RequireSuccess(functional::RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRepo),
                   "fetch conflict source");

    const auto start = std::chrono::steady_clock::now();
    const auto result = functional::RunGit(
        {
            "-c", "core.editor=false",
            "-c", "sequence.editor=false",
            "rebase", "origin/" + ctx.branch
        },
        ctx.cloneRepo);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    RequireFailure(result, "rebase must report the conflict");
    REQUIRE(elapsed < std::chrono::seconds(15));
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) != localHead);
    REQUIRE((
        std::filesystem::exists(ctx.cloneRepo / ".git" / "rebase-merge") ||
        std::filesystem::exists(ctx.cloneRepo / ".git" / "rebase-apply")));

    RequireSuccess(functional::RunGit({"rebase", "--abort"}, ctx.cloneRepo),
                   "abort integration rebase conflict");
    REQUIRE(CurrentHeadSha(ctx.cloneRepo) == localHead);

    RemoveRemoteContext(ctx);
}

} // namespace kano::git::tests::integration
