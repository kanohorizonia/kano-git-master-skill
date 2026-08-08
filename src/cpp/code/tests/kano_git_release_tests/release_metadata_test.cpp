#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <CLI/CLI.hpp>

#include "regression_coverage.hpp"
#include "release_helpers.hpp"
#include "runtime_path_layout.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace kano::git::commands;

namespace kano::git::commands {
void RegisterRegression(CLI::App& InApp);
}

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

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, std::optional<std::string> value)
        : name_(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) {
            previous_ = std::string(previous);
        }
        Set(std::move(value));
    }

    ~ScopedEnvironment() {
        Set(previous_);
    }

private:
    void Set(const std::optional<std::string>& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.has_value() ? value->c_str() : "");
#else
        if (value.has_value()) {
            setenv(name_.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
    }

private:
    std::filesystem::path previous_;
};

void WriteFixtureFile(const std::filesystem::path& path, const std::string& content = "fixture\n") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << content;
}

auto ReadFixtureFile(const std::filesystem::path& path) -> std::string {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

auto ShellQuote(const std::filesystem::path& path) -> std::string {
    const auto text = path.string();
    std::string quoted{"'"};
    for (const char character : text) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
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
    WriteFixtureFile(
        repoRoot / "assets" / "regression" / "incidents.json",
        R"({
  "schema": "kog.regression.incident-map.v1",
  "incidents": [
    {
      "incident_id": "KG-BUG-0001",
      "incident_date": "2026-07-28",
      "root_cause": "A verified staged package regression fixture.",
      "backlog_ref": "KG-BUG-0001",
      "change_ref": "commit:staged-package-fixture",
      "workflow_contracts": ["package-assets-resolve-from-binary-root"],
      "regression_cases": [
        {
          "case_id": "KG-BUG-0001/staged-package",
          "test_name": "staged package",
          "test_file": "src/tests/staged-package.cpp",
          "test_kind": "contract",
          "mapping_state": "source-linked"
        }
      ]
    }
  ]
}
)");
    WriteFixtureFile(
        repoRoot / "assets" / "regression" / "case-template.json",
        "{}\n");
    WriteFixtureFile(
        repoRoot / "assets" / "audit" / "schemas" /
            "kog.auditEvent.v1.schema.json",
        "{}\n");
    WriteFixtureFile(
        repoRoot / "assets" / "audit" / "schemas" /
            "kog.runReceipt.v1.schema.json",
        "{}\n");
    WriteFixtureFile(
        repoRoot / "assets" / "audit" / "schemas" /
            "kog.auditCapability.v1.schema.json",
        "{}\n");
    WriteFixtureFile(
        repoRoot / "assets" / "audit" / "schemas" /
            "kog.auditVerification.v1.schema.json",
        "{}\n");
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
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "regression" / "incidents.json"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "regression" / "case-template.json"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "audit" / "schemas" /
        "kog.auditEvent.v1.schema.json"));
    REQUIRE(std::filesystem::is_regular_file(
        skillRoot / "assets" / "audit" / "schemas" /
        "kog.runReceipt.v1.schema.json"));
    REQUIRE_FALSE(std::filesystem::exists(
        skillRoot / "assets" / "ignore-sources" / "private-machine-path.txt"));
    REQUIRE_FALSE(std::filesystem::exists(
        plan.packageRoot / "private-release-note.txt"));

    for (const auto& entry : std::filesystem::recursive_directory_iterator(skillRoot)) {
        INFO(entry.path());
        REQUIRE(entry.path().filename() != ".git");
    }

    const auto stagedBinary = plan.packageRoot / "bin" / "kano-git.exe";
    const auto externalWorkspace = fixture.path / "external-workspace";
    std::filesystem::create_directories(externalWorkspace);
    ScopedEnvironment noExplicitRoot("KANO_GIT_SKILL_ROOT", std::nullopt);
    ScopedEnvironment noLauncherRoot("KANO_GIT_MASTER_ROOT", std::nullopt);
    ScopedEnvironment binaryPath("KANO_GIT_BINARY_PATH", stagedBinary.generic_string());
    ScopedCurrentPath externalCurrentPath(externalWorkspace);

    const auto layout = runtime_path::Layout::Resolve(externalWorkspace);
    REQUIRE(layout.SkillRoot() == skillRoot.lexically_normal());
    REQUIRE(layout.RegressionIncidentManifest() ==
            skillRoot / "assets" / "regression" / "incidents.json");
    const auto loaded = regression::LoadCoverageManifest(
        layout.RegressionIncidentManifest());
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.report.gaps.empty());

    CLI::App app{"test", "kano-git"};
    RegisterRegression(app);
    std::vector<std::string> arguments = {
        "kano-git",
        "regression",
        "coverage",
        "--fail-on-gap",
    };
    std::vector<char*> argv;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    std::ostringstream stdoutCapture;
    std::ostringstream stderrCapture;
    auto* previousStdout = std::cout.rdbuf(stdoutCapture.rdbuf());
    auto* previousStderr = std::cerr.rdbuf(stderrCapture.rdbuf());
    int exitCode = 0;
    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& error) {
        exitCode = error.get_exit_code();
    }
    std::cout.rdbuf(previousStdout);
    std::cerr.rdbuf(previousStderr);

    INFO(stderrCapture.str());
    REQUIRE(exitCode == 0);
    REQUIRE(stdoutCapture.str().find("execution_evidence=not-evaluated") !=
            std::string::npos);
    REQUIRE(stdoutCapture.str().find("gaps=0") != std::string::npos);

#if !defined(_WIN32)
    const auto stagedKogLauncher = skillRoot / "scripts" / "kog";
    const auto stagedKanoGitLauncher = skillRoot / "scripts" / "kano-git";
    const auto stagedHostBinary = plan.packageRoot / "bin" / "kano-git";
    REQUIRE(std::filesystem::copy_file(
        RepoRoot() / "scripts" / "kog",
        stagedKogLauncher,
        std::filesystem::copy_options::overwrite_existing));
    REQUIRE(std::filesystem::copy_file(
        RepoRoot() / "scripts" / "kano-git",
        stagedKanoGitLauncher,
        std::filesystem::copy_options::overwrite_existing));
    REQUIRE(std::filesystem::copy_file(
        std::filesystem::path(KANO_GIT_TEST_CLI_BINARY),
        stagedHostBinary,
        std::filesystem::copy_options::overwrite_existing));
    const auto executablePermissions =
        std::filesystem::perms::owner_exec |
        std::filesystem::perms::group_exec |
        std::filesystem::perms::others_exec;
    std::filesystem::permissions(
        stagedKogLauncher,
        executablePermissions,
        std::filesystem::perm_options::add);
    std::filesystem::permissions(
        stagedKanoGitLauncher,
        executablePermissions,
        std::filesystem::perm_options::add);
    std::filesystem::permissions(
        stagedHostBinary,
        executablePermissions,
        std::filesystem::perm_options::add);

    const auto launcherOutput = fixture.path / "staged-launcher-output.txt";
    const auto launcherCommand =
        "cd " + ShellQuote(externalWorkspace) +
        " && /usr/bin/env -u KANO_GIT_SKILL_ROOT -u KANO_GIT_MASTER_ROOT"
        " -u KANO_GIT_BINARY_PATH /bin/bash " +
        ShellQuote(stagedKogLauncher) +
        " regression coverage --fail-on-gap > " +
        ShellQuote(launcherOutput) + " 2>&1";
    REQUIRE(std::system(launcherCommand.c_str()) == 0);
    const auto launcherText = ReadFixtureFile(launcherOutput);
    REQUIRE(launcherText.find("execution_evidence=not-evaluated") != std::string::npos);
    REQUIRE(launcherText.find("gaps=0") != std::string::npos);
    REQUIRE(launcherText.find("pixi") == std::string::npos);

    const auto pathInvocationOutput = fixture.path / "path-invocation-output.txt";
    const auto pathEnvironment = std::filesystem::path(
        "PATH=" + (plan.packageRoot / "bin").string() + ":/usr/bin:/bin");
    const auto pathInvocationCommand =
        "cd " + ShellQuote(externalWorkspace) +
        " && /usr/bin/env -u KANO_GIT_SKILL_ROOT -u KANO_GIT_MASTER_ROOT"
        " -u KANO_GIT_BINARY_PATH " +
        ShellQuote(pathEnvironment) +
        " kano-git regression coverage --fail-on-gap > " +
        ShellQuote(pathInvocationOutput) + " 2>&1";
    REQUIRE(std::system(pathInvocationCommand.c_str()) == 0);
    const auto pathInvocationText = ReadFixtureFile(pathInvocationOutput);
    REQUIRE(pathInvocationText.find("execution_evidence=not-evaluated") !=
            std::string::npos);
    REQUIRE(pathInvocationText.find("gaps=0") != std::string::npos);

    const auto installState = skillRoot / ".kog-install.json";
    const auto disabledInstallState = skillRoot / ".kog-install.json.disabled";
    std::filesystem::rename(installState, disabledInstallState);
    REQUIRE(std::system(launcherCommand.c_str()) != 0);
    const auto rejectedLauncherText = ReadFixtureFile(launcherOutput);
    REQUIRE(rejectedLauncherText.find("shared infra bootstrap is missing") !=
            std::string::npos);
#endif
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
