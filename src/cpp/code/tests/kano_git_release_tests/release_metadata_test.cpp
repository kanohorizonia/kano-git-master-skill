#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "release_helpers.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

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

struct TempDirectory {
    std::filesystem::path path;

    explicit TempDirectory(const std::string& label) {
        static std::uint64_t sequence = 0;
        path = std::filesystem::temp_directory_path() /
               ("kano_git_release_tests_" + label + "_" + std::to_string(++sequence));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path);
    }

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void WriteFixtureFile(const std::filesystem::path& path, const std::string& content = "fixture\n") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
}

void WriteRequiredWindowsPackageSource(const std::filesystem::path& repoRoot) {
    WriteFixtureFile(repoRoot / "SKILL.md");
    WriteFixtureFile(repoRoot / "README.md");
    WriteFixtureFile(repoRoot / "VERSION", "0.0.1\n");
    WriteFixtureFile(repoRoot / "scripts" / "kog");
    WriteFixtureFile(repoRoot / "docs" / "README.md");
    WriteFixtureFile(repoRoot / ".kano" / "release.toml");
    WriteFixtureFile(repoRoot / "assets" / "ignore" / "README.md");
    WriteFixtureFile(repoRoot / "assets" / "ignore" / "datasource" / "manifest.json", "{}\n");
    WriteFixtureFile(repoRoot / "assets" / "ignore" / "local-rules" / "kano.gitignore");
    WriteFixtureFile(repoRoot / "assets" / "ignore" / "policy" / "ignore-gate-allowlist.txt");
    WriteFixtureFile(
        repoRoot / "assets" / "ignore" / "datasource" / "upstream" /
            "github-gitignore" / "Global" / "macOS.gitignore");
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

TEST_CASE("Windows package stages canonical ignore assets without submodule git metadata",
          "[release][windows][package]") {
    TempDirectory fixture("windows_package_assets");
    const auto repoRoot = fixture.path / "repo";
    const auto outputRoot = fixture.path / "out";
    const auto kogBinary = fixture.path / "bin" / "kog.exe";
    const auto kanoGitBinary = fixture.path / "bin" / "kano-git.exe";

    WriteRequiredWindowsPackageSource(repoRoot);
    const auto upstreamRoot =
        repoRoot / "assets" / "ignore" / "datasource" / "upstream" / "github-gitignore";
    WriteFixtureFile(upstreamRoot / ".git", "gitdir: /developer/machine/private/modules/path\n");
    WriteFixtureFile(upstreamRoot / "nested" / ".git" / "config", "private metadata\n");
    WriteFixtureFile(kogBinary, "binary\n");
    WriteFixtureFile(kanoGitBinary, "binary\n");

    release::ReleaseMetadata metadata;
    metadata.repoRoot = repoRoot;
    metadata.packageId = "KanoHorizonia.KanoGit";
    metadata.packageName = "Kano Git";
    metadata.version = "0.0.1";
    metadata.skill.skillName = "kano-git-master-skill";

    release::WindowsPackagePlan plan;
    plan.packageDirectoryName = "Kano Git-0.0.1-windows-x64";
    plan.packageRoot = outputRoot / plan.packageDirectoryName;
    plan.foundBinaries = {kogBinary, kanoGitBinary};
    plan.missingKogBinary = false;
    plan.missingKanoGitBinary = false;

    const auto stagedSkillRoot = plan.packageRoot / "skills" / metadata.skill.skillName;
    WriteFixtureFile(
        stagedSkillRoot / "assets" / "ignore-sources" / "private-machine-path.txt",
        "/developer/private/worktree\n");
    WriteFixtureFile(plan.packageRoot / "private-release-note.txt", "stale private payload\n");

    REQUIRE_NOTHROW(release::StageWindowsPackage(metadata, plan));

    const auto skillRoot = plan.packageRoot / "skills" / metadata.skill.skillName;
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "ignore" / "README.md"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "ignore" / "datasource" / "manifest.json"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "ignore" / "local-rules" / "kano.gitignore"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "ignore" / "policy" / "ignore-gate-allowlist.txt"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "ignore" / "datasource" / "upstream" /
        "github-gitignore" / "Global" / "macOS.gitignore"));
    REQUIRE_FALSE(std::filesystem::exists(
        skillRoot / "assets" / "ignore-sources" / "private-machine-path.txt"));
    REQUIRE_FALSE(std::filesystem::exists(
        plan.packageRoot / "private-release-note.txt"));

    for (const auto& entry : std::filesystem::recursive_directory_iterator(skillRoot)) {
        INFO(entry.path());
        REQUIRE(entry.path().filename() != ".git");
    }
}

TEST_CASE("Windows package rejects a symlinked output parent that resolves to the repo",
          "[release][windows][package]") {
    TempDirectory fixture("windows_package_symlink_guard");
    const std::string packageDirectoryName = "Kano Git-0.0.1-windows-x64";
    const auto repoParent = fixture.path / "real";
    const auto repoRoot = repoParent / packageDirectoryName;
    const auto outputAlias = fixture.path / "alias-to-repo-parent";
    const auto kogBinary = fixture.path / "bin" / "kog.exe";
    const auto kanoGitBinary = fixture.path / "bin" / "kano-git.exe";
    const auto repoMarker = repoRoot / "must-survive.txt";

    WriteRequiredWindowsPackageSource(repoRoot);
    WriteFixtureFile(repoMarker, "do not delete\n");
    WriteFixtureFile(kogBinary, "binary\n");
    WriteFixtureFile(kanoGitBinary, "binary\n");

    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(repoParent, outputAlias, symlinkError);
    if (symlinkError) {
        SKIP("filesystem directory symlink creation is unavailable: " << symlinkError.message());
    }

    release::ReleaseMetadata metadata;
    metadata.repoRoot = repoRoot;
    metadata.packageId = "KanoHorizonia.KanoGit";
    metadata.packageName = "Kano Git";
    metadata.version = "0.0.1";
    metadata.skill.skillName = "kano-git-master-skill";

    release::WindowsPackagePlan plan;
    plan.packageDirectoryName = packageDirectoryName;
    plan.packageRoot = outputAlias / packageDirectoryName;
    plan.foundBinaries = {kogBinary, kanoGitBinary};
    plan.missingKogBinary = false;
    plan.missingKanoGitBinary = false;

    REQUIRE_THROWS_WITH(
        release::StageWindowsPackage(metadata, plan),
        "WINDOWS_PACKAGE_UNSAFE_OUTPUT_ROOT");
    REQUIRE(std::filesystem::is_regular_file(repoMarker));
}
