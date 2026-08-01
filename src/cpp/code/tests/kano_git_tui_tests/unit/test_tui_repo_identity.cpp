#include <catch2/catch_test_macros.hpp>

#include "tui_history_lifecycle.hpp"
#include "tui_repo_identity.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;
using kano::git::commands::NormalizeRepoIdentityKey;
using kano::git::commands::ReconcileHistoryAfterRepoRefresh;
using kano::git::commands::ResolveRepoRelativePath;
using kano::git::commands::ResolveStableRepoIdentityKey;
using kano::git::commands::SelectPreferredRepoIdentityIndices;

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned long long> counter{0};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        mPath = fs::temp_directory_path() /
            ("kog-tui-repo-identity-" + std::to_string(stamp) + "-" +
             std::to_string(counter.fetch_add(1)));
        fs::create_directories(mPath);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(mPath, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    auto operator=(const TempDirectory&) -> TempDirectory& = delete;

    [[nodiscard]] auto Path() const -> const fs::path& {
        return mPath;
    }

private:
    fs::path mPath;
};

}  // namespace

TEST_CASE(
    "TUI repository identity normalization is lexical and separator stable",
    "[unit][tui_repo_identity][KG-TSK-0070]") {
    REQUIRE(
        NormalizeRepoIdentityKey("/workspace/repo/./nested/..") ==
        "/workspace/repo");
    REQUIRE(
        NormalizeRepoIdentityKey("/workspace/repo///") ==
        "/workspace/repo");
    REQUIRE(NormalizeRepoIdentityKey("/") == "/");
}

TEST_CASE(
    "TUI repository identity resolves symlink and host case aliases once",
    "[unit][tui_repo_identity][KG-TSK-0070]") {
    TempDirectory fixture;
    const auto realRepo = fixture.Path() / "Repository";
    const auto symlinkRepo = fixture.Path() / "repo-link";
    fs::create_directories(realRepo);

    std::error_code symlinkError;
    fs::create_directory_symlink(realRepo, symlinkRepo, symlinkError);
    if (!symlinkError) {
        REQUIRE(
            ResolveStableRepoIdentityKey(realRepo) ==
            ResolveStableRepoIdentityKey(symlinkRepo));
    }

    const auto caseAlias = fixture.Path() / "repository";
    std::error_code equivalentError;
    if (fs::equivalent(realRepo, caseAlias, equivalentError) &&
        !equivalentError) {
        const auto realIdentity =
            ResolveStableRepoIdentityKey(realRepo);
        const auto aliasIdentity =
            ResolveStableRepoIdentityKey(caseAlias);
        REQUIRE(realIdentity == aliasIdentity);

        const auto reconciliation =
            ReconcileHistoryAfterRepoRefresh(
                realIdentity,
                {aliasIdentity});
        REQUIRE(reconciliation.newRepoIndex == 0);
        REQUIRE_FALSE(reconciliation.bCloseHistory);
        REQUIRE(reconciliation.bCloseDetail);

        REQUIRE(
            ResolveRepoRelativePath(
                caseAlias,
                caseAlias / "nested" / "repo") ==
            fs::path("nested/repo"));
    }
}

TEST_CASE(
    "TUI repository identity keeps distinct case-sensitive directories separate",
    "[unit][tui_repo_identity][KG-TSK-0070]") {
    TempDirectory fixture;
    const auto upper = fixture.Path() / "CaseRepo";
    const auto lower = fixture.Path() / "caserepo";
    fs::create_directories(upper);
    std::error_code createError;
    fs::create_directory(lower, createError);

    std::error_code equivalentError;
    if (!createError &&
        !fs::equivalent(upper, lower, equivalentError) &&
        !equivalentError) {
        REQUIRE(
            ResolveStableRepoIdentityKey(upper) !=
            ResolveStableRepoIdentityKey(lower));
    }
}

TEST_CASE(
    "TUI repository identity has a stable non-throwing missing-path fallback",
    "[unit][tui_repo_identity][KG-TSK-0070]") {
    TempDirectory fixture;
    const auto missing = fixture.Path() / "missing" / "repo";
    REQUIRE_FALSE(fs::exists(missing));
    const auto first = ResolveStableRepoIdentityKey(missing);
    REQUIRE_FALSE(first.empty());
    REQUIRE(ResolveStableRepoIdentityKey(missing) == first);

    fs::create_directories(missing);
    const auto initialized =
        ResolveStableRepoIdentityKey(missing);
    REQUIRE(initialized != first);
    REQUIRE(ResolveStableRepoIdentityKey(missing) == initialized);
}

TEST_CASE(
    "TUI repository identity dedupe prefers authoritative metadata",
    "[unit][tui_repo_identity][KG-TSK-0070]") {
    TempDirectory fixture;
    const auto realRepo = fixture.Path() / "Repository";
    const auto aliasRepo = fixture.Path() / "repo-link";
    const auto otherRepo = fixture.Path() / "Other";
    fs::create_directories(realRepo);
    fs::create_directories(otherRepo);

    std::error_code symlinkError;
    fs::create_directory_symlink(
        realRepo,
        aliasRepo,
        symlinkError);
    if (symlinkError) {
        SKIP("directory symlinks are unavailable on this host");
    }

    const std::vector<std::string> identities{
        ResolveStableRepoIdentityKey(aliasRepo),
        ResolveStableRepoIdentityKey(otherRepo),
        ResolveStableRepoIdentityKey(realRepo),
    };
    const std::vector<int> priorities{
        4,
        3,
        0,
    };
    const auto selected =
        SelectPreferredRepoIdentityIndices(
            identities,
            priorities);
    REQUIRE(selected == std::vector<std::size_t>{2, 1});
}
