#include <catch2/catch_test_macros.hpp>

#include "release_helpers.hpp"

#include <filesystem>
#include <fstream>

using namespace kano::git::commands;

namespace {

auto RepoRoot() -> std::filesystem::path {
    return std::filesystem::weakly_canonical(std::filesystem::path(KANO_GIT_TEST_REPO_ROOT));
}

auto LoadDogfoodMetadata() -> release::ReleaseMetadata {
    auto loaded = release::LoadReleaseMetadata(RepoRoot());
    REQUIRE(loaded.ok);
    return loaded.metadata;
}

auto MakeTempInstaller() -> std::filesystem::path {
    const auto root = std::filesystem::temp_directory_path() / "kano_git_release_tests";
    std::filesystem::create_directories(root);
    const auto path = root / "KanoGit-0.0.1-windows-x64.msi";
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    out << "fake msi for release metadata tests";
    return path;
}

} // namespace

TEST_CASE("dogfood release metadata uses public Kano Git package identity", "[release][metadata]") {
    const auto metadata = LoadDogfoodMetadata();

    REQUIRE(metadata.packageId == "KanoHorizonia.KanoGit");
    REQUIRE(metadata.packageName == "Kano Git");
    REQUIRE(metadata.publisher == "Kano Horizonia");
    REQUIRE(metadata.moniker == "kog");
    REQUIRE(metadata.skill.skillName == "kano-git-master-skill");
    REQUIRE(metadata.winget.packageIdentifier == "KanoHorizonia.KanoGit");
}

TEST_CASE("winget path follows package identifier and version", "[release][winget]") {
    REQUIRE(release::WingetManifestRelativeDirectory("KanoHorizonia.KanoGit", "0.0.1") ==
            "manifests/k/KanoHorizonia/KanoGit/0.0.1");
}

TEST_CASE("winget generation fails closed when installer is missing", "[release][winget]") {
    const auto metadata = LoadDogfoodMetadata();

    const auto plan = release::BuildWingetPlan(
        metadata,
        {},
        "",
        "https://github.com/kanohorizonia/kano-git-master-skill/releases/download/v0.0.1",
        {});

    REQUIRE(plan.packageIdentifier == "KanoHorizonia.KanoGit");
    REQUIRE(plan.blockedReason == "BLOCKED_INSTALLER_MISSING");
    REQUIRE(plan.manifestDirectory.generic_string().find("manifests/k/KanoHorizonia/KanoGit/0.0.1") != std::string::npos);
}

TEST_CASE("winget generation renders version installer and locale manifests", "[release][winget]") {
    const auto metadata = LoadDogfoodMetadata();
    const auto installer = MakeTempInstaller();

    const auto plan = release::BuildWingetPlan(
        metadata,
        installer,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "https://github.com/kanohorizonia/kano-git-master-skill/releases/download/v0.0.1",
        {});

    REQUIRE(plan.blockedReason.empty());
    REQUIRE(plan.installerType == "wix");
    REQUIRE(plan.manifestFiles.contains("KanoHorizonia.KanoGit.yaml"));
    REQUIRE(plan.manifestFiles.contains("KanoHorizonia.KanoGit.installer.yaml"));
    REQUIRE(plan.manifestFiles.contains("KanoHorizonia.KanoGit.locale.en-US.yaml"));
    REQUIRE(plan.installerUrl.find("KanoGit-0.0.1-windows-x64.msi") != std::string::npos);
}

TEST_CASE("winget PR plan uses Kano Git public branch title and commit naming", "[release][winget]") {
    const auto metadata = LoadDogfoodMetadata();

    const auto plan = release::BuildWingetPrPlan(metadata, "");

    REQUIRE(plan.branchName == "release/kanogit-winget-0.0.1");
    REQUIRE(plan.packagePath == "manifests/k/KanoHorizonia/KanoGit/0.0.1");
    REQUIRE(plan.commitMessage == "New version: KanoHorizonia.KanoGit version 0.0.1");
    REQUIRE(plan.prTitle == "New version: KanoHorizonia.KanoGit version 0.0.1");
}

TEST_CASE("skill install plan protects developer namespace by default", "[release][skill]") {
    const auto metadata = LoadDogfoodMetadata();
    const auto protectedTarget = release::ExpandUserPath("~/.agents/skills/kano/kano-git-master-skill");

    const auto blocked = release::BuildSkillInstallPlan(metadata, protectedTarget, false);
    REQUIRE_FALSE(blocked.allowed);
    REQUIRE(blocked.statusCode == "DEVELOPER_TARGET_PROTECTED");

    const auto allowed = release::BuildSkillInstallPlan(metadata, protectedTarget, true);
    REQUIRE(allowed.allowed);
    REQUIRE(allowed.statusCode == "OK");
}
