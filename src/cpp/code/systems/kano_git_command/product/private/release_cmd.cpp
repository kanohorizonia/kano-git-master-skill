#include <CLI/CLI.hpp>

#include "release_helpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace kano::git::commands {
namespace {

auto LoadMetadataOrExit(const std::string& repoRoot,
                        const std::string& metadataPath,
                        bool dryRun) -> release::ReleaseMetadata {
    const auto loaded = release::LoadReleaseMetadata(
        std::filesystem::path(repoRoot),
        metadataPath.empty() ? std::filesystem::path{} : std::filesystem::path(metadataPath));
    if (loaded.ok) {
        return loaded.metadata;
    }
    std::cout << "status=" << loaded.code << "\n";
    std::cout << "message=" << loaded.message << "\n";
    std::exit(dryRun ? 0 : 1);
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write " + path.generic_string());
    }
    out << content;
}

void CopyFileIfExists(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || ec) {
        return;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("failed to copy " + source.generic_string() + " to " + target.generic_string());
    }
}

void CopyDirectoryIfExists(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || ec) {
        return;
    }
    std::filesystem::create_directories(target, ec);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        if (ec) {
            throw std::runtime_error("failed to enumerate " + source.generic_string());
        }
        const auto relative = std::filesystem::relative(entry.path(), source, ec);
        if (ec) {
            continue;
        }
        const auto destination = target / relative;
        if (entry.is_directory()) {
            std::filesystem::create_directories(destination, ec);
            continue;
        }
        if (entry.is_regular_file()) {
            CopyFileIfExists(entry.path(), destination);
        }
    }
}

void PrintWindowsPackagePlan(const release::WindowsPackagePlan& plan) {
    std::cout << "packageRoot=" << plan.packageRoot.generic_string() << "\n";
    std::cout << "packageDirectoryName=" << plan.packageDirectoryName << "\n";
    std::cout << "missingKogBinary=" << (plan.missingKogBinary ? "true" : "false") << "\n";
    std::cout << "missingKanoGitBinary=" << (plan.missingKanoGitBinary ? "true" : "false") << "\n";
    for (const auto& path : plan.foundBinaries) {
        std::cout << "foundBinary=" << path.generic_string() << "\n";
    }
}

void StageWindowsPackage(const release::ReleaseMetadata& metadata,
                         const release::WindowsPackagePlan& plan) {
    if (plan.missingKogBinary || plan.missingKanoGitBinary) {
        throw std::runtime_error("WINDOWS_PACKAGE_BINARY_MISSING");
    }
    const auto binRoot = plan.packageRoot / "bin";
    const auto skillRoot = plan.packageRoot / "skills" / metadata.skill.skillName;
    std::error_code ec;
    std::filesystem::create_directories(binRoot, ec);
    std::filesystem::create_directories(skillRoot, ec);
    for (const auto& binary : plan.foundBinaries) {
        CopyFileIfExists(binary, binRoot / binary.filename());
    }
    CopyFileIfExists(metadata.repoRoot / "SKILL.md", skillRoot / "SKILL.md");
    CopyFileIfExists(metadata.repoRoot / "README.md", skillRoot / "README.md");
    CopyFileIfExists(metadata.repoRoot / "VERSION", skillRoot / "VERSION");
    CopyDirectoryIfExists(metadata.repoRoot / "scripts", skillRoot / "scripts");
    CopyDirectoryIfExists(metadata.repoRoot / "docs", skillRoot / "docs");
    CopyDirectoryIfExists(metadata.repoRoot / ".kano", skillRoot / ".kano");
    WriteTextFile(plan.packageRoot / "release-manifest.json", release::RenderReleaseManifestJson(metadata));
    WriteTextFile(skillRoot / ".kog-install.json", release::RenderSkillInstallJson(metadata, binRoot));
}

void PrintWingetPlan(const release::WingetPlan& plan) {
    std::cout << "packageIdentifier=" << plan.packageIdentifier << "\n";
    std::cout << "version=" << plan.version << "\n";
    std::cout << "manifestDirectory=" << plan.manifestDirectory.generic_string() << "\n";
    if (!plan.installerPath.empty()) {
        std::cout << "installerPath=" << plan.installerPath.generic_string() << "\n";
    }
    if (!plan.installerUrl.empty()) {
        std::cout << "installerUrl=" << plan.installerUrl << "\n";
    }
    if (!plan.blockedReason.empty()) {
        std::cout << "status=" << plan.blockedReason << "\n";
    } else {
        std::cout << "status=OK\n";
        for (const auto& [name, _] : plan.manifestFiles) {
            std::cout << "manifestFile=" << (plan.manifestDirectory / name).generic_string() << "\n";
        }
        std::cout << "installCommand=winget install --id " << plan.packageIdentifier << " -e\n";
        std::cout << "postInstallCommand=kog skill install\n";
    }
}

void WriteWingetManifests(const release::WingetPlan& plan) {
    if (!plan.blockedReason.empty()) {
        throw std::runtime_error(plan.blockedReason);
    }
    for (const auto& [name, content] : plan.manifestFiles) {
        WriteTextFile(plan.manifestDirectory / name, content);
    }
}

void PrintWingetPrPlan(const release::WingetPrPlan& plan) {
    std::cout << "forkRepo=" << plan.forkRepo << "\n";
    std::cout << "upstreamRepo=" << plan.upstreamRepo << "\n";
    std::cout << "branchName=" << plan.branchName << "\n";
    std::cout << "packagePath=" << plan.packagePath << "\n";
    std::cout << "commitMessage=" << plan.commitMessage << "\n";
    std::cout << "prTitle=" << plan.prTitle << "\n";
    std::cout << "prBody=" << plan.prBody << "\n";
    for (const auto& command : plan.commands) {
        std::cout << "planCommand=" << command << "\n";
    }
}

} // namespace

void RegisterRelease(CLI::App& InApp) {
    auto* releaseCmd = InApp.add_subcommand("release", "Prepare Kano skill release packages and package-manager metadata");
    releaseCmd->require_subcommand(1);

    auto* packageCmd = releaseCmd->add_subcommand("package", "Build release package payloads");
    packageCmd->require_subcommand(1);

    auto* packageWindows = packageCmd->add_subcommand("windows", "Prepare the Windows release package layout");
    auto* packageRepo = new std::string{"."};
    auto* packageMetadata = new std::string{};
    auto* packageOutput = new std::string{"artifacts/release/windows"};
    auto* packageDryRun = new bool{false};
    packageWindows->add_option("--repo", *packageRepo, "Repository root containing .kano/release.toml");
    packageWindows->add_option("--metadata", *packageMetadata, "Override release metadata path");
    packageWindows->add_option("--output", *packageOutput, "Package staging output root");
    packageWindows->add_flag("--dry-run", *packageDryRun, "Render package plan without writing files");
    packageWindows->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*packageRepo, *packageMetadata, *packageDryRun);
        const auto plan = release::BuildWindowsPackagePlan(metadata, std::filesystem::path(*packageOutput));
        PrintWindowsPackagePlan(plan);
        if (*packageDryRun) {
            return;
        }
        try {
            StageWindowsPackage(metadata, plan);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }
    });

    auto* manifestCmd = releaseCmd->add_subcommand("manifest", "Render the release manifest from .kano/release.toml");
    auto* manifestRepo = new std::string{"."};
    auto* manifestMetadata = new std::string{};
    auto* manifestOutput = new std::string{"Release/release-manifest.json"};
    auto* manifestDryRun = new bool{false};
    manifestCmd->add_option("--repo", *manifestRepo, "Repository root containing .kano/release.toml");
    manifestCmd->add_option("--metadata", *manifestMetadata, "Override release metadata path");
    manifestCmd->add_option("--output", *manifestOutput, "Manifest output path");
    manifestCmd->add_flag("--dry-run", *manifestDryRun, "Print manifest without writing files");
    manifestCmd->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*manifestRepo, *manifestMetadata, *manifestDryRun);
        const auto json = release::RenderReleaseManifestJson(metadata);
        if (*manifestDryRun) {
            std::cout << json;
            return;
        }
        try {
            WriteTextFile(std::filesystem::path(*manifestOutput), json);
            std::cout << "manifest=" << std::filesystem::path(*manifestOutput).generic_string() << "\n";
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }
    });

    auto* githubCmd = releaseCmd->add_subcommand("github", "Prepare GitHub release commands");
    githubCmd->require_subcommand(1);
    auto* githubPrepare = githubCmd->add_subcommand("prepare", "Render gh release create command and asset intent");
    auto* githubRepo = new std::string{"."};
    auto* githubMetadata = new std::string{};
    auto* githubDryRun = new bool{true};
    githubPrepare->add_option("--repo", *githubRepo, "Repository root containing .kano/release.toml");
    githubPrepare->add_option("--metadata", *githubMetadata, "Override release metadata path");
    githubPrepare->add_flag("--dry-run,!--no-dry-run", *githubDryRun, "Render without creating a GitHub Release");
    githubPrepare->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*githubRepo, *githubMetadata, *githubDryRun);
        std::cout << "packageIdentifier=" << metadata.winget.packageIdentifier << "\n";
        std::cout << "packageName=" << metadata.packageName << "\n";
        std::cout << "releaseTag=" << release::EnsureReleaseTag(metadata.version) << "\n";
        std::cout << "dryRun=" << (*githubDryRun ? "true" : "false") << "\n";
        std::cout << "planCommand=gh release create " << release::EnsureReleaseTag(metadata.version)
                  << " --repo <owner/repo> --title \"" << metadata.packageName << " "
                  << release::EnsureVersionWithoutPrefix(metadata.version)
                  << "\" --notes-file Release/reports/release-notes.md <assets...>\n";
        if (!*githubDryRun) {
            std::cerr << "Error: GITHUB_RELEASE_CREATE_NOT_RUN_BY_PREPARE\n";
            std::exit(1);
        }
    });

    auto* wingetCmd = releaseCmd->add_subcommand("winget", "Generate, validate, and stage winget metadata");
    wingetCmd->require_subcommand(1);

    auto* wingetGenerate = wingetCmd->add_subcommand("generate", "Generate winget manifests from a Windows installer");
    auto* wingetRepo = new std::string{"."};
    auto* wingetMetadata = new std::string{};
    auto* wingetInstaller = new std::string{};
    auto* wingetSha = new std::string{};
    auto* wingetBaseUrl = new std::string{};
    auto* wingetOutput = new std::string{"Release/package-managers/winget"};
    auto* wingetDryRun = new bool{true};
    wingetGenerate->add_option("--repo", *wingetRepo, "Repository root containing .kano/release.toml");
    wingetGenerate->add_option("--metadata", *wingetMetadata, "Override release metadata path");
    wingetGenerate->add_option("--installer", *wingetInstaller, "MSI or EXE installer asset path");
    wingetGenerate->add_option("--installer-sha256", *wingetSha, "Installer SHA256");
    wingetGenerate->add_option("--release-asset-base-url", *wingetBaseUrl, "Immutable GitHub Release asset base URL");
    wingetGenerate->add_option("--output", *wingetOutput, "Winget manifest output root");
    wingetGenerate->add_flag("--dry-run,!--no-dry-run", *wingetDryRun, "Render without writing winget manifest files");
    wingetGenerate->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*wingetRepo, *wingetMetadata, *wingetDryRun);
        const auto plan = release::BuildWingetPlan(
            metadata,
            wingetInstaller->empty() ? std::filesystem::path{} : std::filesystem::path(*wingetInstaller),
            *wingetSha,
            *wingetBaseUrl,
            std::filesystem::path(*wingetOutput));
        PrintWingetPlan(plan);
        if (*wingetDryRun) {
            return;
        }
        try {
            WriteWingetManifests(plan);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }
    });

    auto* wingetValidate = wingetCmd->add_subcommand("validate", "Render winget validation intent");
    auto* validateRepo = new std::string{"."};
    auto* validateMetadata = new std::string{};
    auto* validateManifestRoot = new std::string{"Release/package-managers/winget"};
    auto* validateDryRun = new bool{true};
    wingetValidate->add_option("--repo", *validateRepo, "Repository root containing .kano/release.toml");
    wingetValidate->add_option("--metadata", *validateMetadata, "Override release metadata path");
    wingetValidate->add_option("--manifest-root", *validateManifestRoot, "Generated manifest root");
    wingetValidate->add_flag("--dry-run,!--no-dry-run", *validateDryRun, "Render validation command without executing it");
    wingetValidate->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*validateRepo, *validateMetadata, *validateDryRun);
        std::cout << "packageIdentifier=" << metadata.winget.packageIdentifier << "\n";
        std::cout << "manifestRoot=" << *validateManifestRoot << "\n";
        std::cout << "planCommand=winget validate " << *validateManifestRoot << "\n";
        if (!*validateDryRun) {
            std::cerr << "Error: WINGET_VALIDATE_EXECUTION_NOT_ENABLED\n";
            std::exit(1);
        }
    });

    auto* wingetPreparePr = wingetCmd->add_subcommand("prepare-pr", "Render the winget fork branch and PR plan");
    auto* prRepo = new std::string{"."};
    auto* prMetadata = new std::string{};
    auto* prBranch = new std::string{};
    auto* prDryRun = new bool{true};
    wingetPreparePr->add_option("--repo", *prRepo, "Repository root containing .kano/release.toml");
    wingetPreparePr->add_option("--metadata", *prMetadata, "Override release metadata path");
    wingetPreparePr->add_option("--branch", *prBranch, "Override winget branch name");
    wingetPreparePr->add_flag("--dry-run,!--no-dry-run", *prDryRun, "Render without creating local branch changes");
    wingetPreparePr->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*prRepo, *prMetadata, *prDryRun);
        const auto plan = release::BuildWingetPrPlan(metadata, *prBranch);
        PrintWingetPrPlan(plan);
        if (!*prDryRun) {
            std::cerr << "Error: WINGET_PREPARE_PR_NON_DRY_RUN_NOT_ENABLED\n";
            std::exit(1);
        }
    });

    auto* wingetCreatePr = wingetCmd->add_subcommand("create-pr", "Create a winget PR after an explicit non-dry-run approval");
    auto* createRepo = new std::string{"."};
    auto* createMetadata = new std::string{};
    auto* createBranch = new std::string{};
    auto* createDryRun = new bool{true};
    auto* createYes = new bool{false};
    wingetCreatePr->add_option("--repo", *createRepo, "Repository root containing .kano/release.toml");
    wingetCreatePr->add_option("--metadata", *createMetadata, "Override release metadata path");
    wingetCreatePr->add_option("--branch", *createBranch, "Override winget branch name");
    wingetCreatePr->add_flag("--dry-run,!--no-dry-run", *createDryRun, "Render without calling gh pr create");
    wingetCreatePr->add_flag("--yes", *createYes, "Required with --no-dry-run");
    wingetCreatePr->callback([=]() {
        const auto metadata = LoadMetadataOrExit(*createRepo, *createMetadata, *createDryRun);
        const auto plan = release::BuildWingetPrPlan(metadata, *createBranch);
        PrintWingetPrPlan(plan);
        if (*createDryRun) {
            return;
        }
        if (!*createYes) {
            std::cerr << "Error: WINGET_CREATE_PR_REQUIRES_YES\n";
            std::exit(1);
        }
        std::cerr << "Error: WINGET_CREATE_PR_EXECUTION_NOT_ENABLED_IN_THIS_BUILD\n";
        std::exit(1);
    });
}

} // namespace kano::git::commands
