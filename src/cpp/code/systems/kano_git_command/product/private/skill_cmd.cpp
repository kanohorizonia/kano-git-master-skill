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

auto LoadSkillMetadataOrExit(const std::string& repoRoot,
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

auto DefaultPackageBinaryRoot() -> std::filesystem::path {
#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData != nullptr && std::string(localAppData).size() > 0) {
        return std::filesystem::path(localAppData) / "Programs" / "Kano" / "KanoGit" / "bin";
    }
    return release::HomeDirectory() / "AppData" / "Local" / "Programs" / "Kano" / "KanoGit" / "bin";
#else
    return release::HomeDirectory() / ".local" / "share" / "kano" / "kanogit" / "bin";
#endif
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

void PrintSkillPlan(const release::SkillInstallPlan& plan) {
    std::cout << "sourceRepoRoot=" << plan.sourceRepoRoot.generic_string() << "\n";
    std::cout << "targetRoot=" << plan.targetRoot.generic_string() << "\n";
    std::cout << "developerProtectedPrefix=" << plan.protectedPrefix.generic_string() << "\n";
    std::cout << "developerTargetProtected=" << (plan.developerTargetProtected ? "true" : "false") << "\n";
    std::cout << "status=" << plan.statusCode << "\n";
    for (const auto& action : plan.actions) {
        std::cout << "planAction=" << action << "\n";
    }
}

} // namespace

void RegisterSkill(CLI::App& InApp) {
    auto* skillCmd = InApp.add_subcommand("skill", "Install and inspect released Kano skill payloads");
    skillCmd->require_subcommand(1);

    auto* installCmd = skillCmd->add_subcommand("install", "Materialize the released skill payload outside the developer checkout");
    auto* installRepo = new std::string{"."};
    auto* installMetadata = new std::string{};
    auto* installTarget = new std::string{};
    auto* installBinaryRoot = new std::string{};
    auto* installDryRun = new bool{false};
    auto* installAllowDeveloperTarget = new bool{false};
    installCmd->add_option("--repo", *installRepo, "Repository root containing .kano/release.toml");
    installCmd->add_option("--metadata", *installMetadata, "Override release metadata path");
    installCmd->add_option("--target", *installTarget, "Override released skill target");
    installCmd->add_option("--package-binary-root", *installBinaryRoot, "Installed package binary root");
    installCmd->add_flag("--dry-run", *installDryRun, "Render install plan without writing files");
    installCmd->add_flag("--allow-developer-target", *installAllowDeveloperTarget, "Allow installing into ~/.agents/skills/kano");
    installCmd->callback([=]() {
        const auto metadata = LoadSkillMetadataOrExit(*installRepo, *installMetadata, *installDryRun);
        const auto target = installTarget->empty() ? std::filesystem::path{} : std::filesystem::path(*installTarget);
        const auto plan = release::BuildSkillInstallPlan(metadata, target, *installAllowDeveloperTarget);
        PrintSkillPlan(plan);
        if (!plan.allowed) {
            std::exit(3);
        }
        if (*installDryRun) {
            return;
        }
        const auto binaryRoot = installBinaryRoot->empty() ? DefaultPackageBinaryRoot() : std::filesystem::path(*installBinaryRoot);
        try {
            std::error_code ec;
            std::filesystem::create_directories(plan.targetRoot / "scripts", ec);
            WriteTextFile(plan.targetRoot / ".kog-install.json", release::RenderSkillInstallJson(metadata, binaryRoot));
            WriteTextFile(plan.targetRoot / "VERSION", release::EnsureVersionWithoutPrefix(metadata.version) + "\n");
#if defined(_WIN32)
            WriteTextFile(plan.targetRoot / "scripts" / "kog.bat",
                          "@echo off\r\n\"" + (binaryRoot / "kog.exe").string() + "\" %*\r\n");
#else
            WriteTextFile(plan.targetRoot / "scripts" / "kog",
                          "#!/usr/bin/env sh\nexec \"" + (binaryRoot / "kog").string() + "\" \"$@\"\n");
#endif
            std::cout << "installedSkillTarget=" << plan.targetRoot.generic_string() << "\n";
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            std::exit(1);
        }
    });

    auto* upgradeCmd = skillCmd->add_subcommand("upgrade", "Render upgrade plan for the released skill payload");
    auto* upgradeRepo = new std::string{"."};
    auto* upgradeMetadata = new std::string{};
    auto* upgradeDryRun = new bool{true};
    upgradeCmd->add_option("--repo", *upgradeRepo, "Repository root containing .kano/release.toml");
    upgradeCmd->add_option("--metadata", *upgradeMetadata, "Override release metadata path");
    upgradeCmd->add_flag("--dry-run,!--no-dry-run", *upgradeDryRun, "Render upgrade plan without writing files");
    upgradeCmd->callback([=]() {
        const auto metadata = LoadSkillMetadataOrExit(*upgradeRepo, *upgradeMetadata, *upgradeDryRun);
        const auto plan = release::BuildSkillInstallPlan(metadata, {}, false);
        PrintSkillPlan(plan);
        std::cout << "planAction=refresh released skill payload from installed Kano Git package\n";
        if (!*upgradeDryRun) {
            std::cerr << "Error: SKILL_UPGRADE_NON_DRY_RUN_NOT_ENABLED\n";
            std::exit(1);
        }
    });

    auto* doctorCmd = skillCmd->add_subcommand("doctor", "Inspect released skill install state");
    auto* doctorRepo = new std::string{"."};
    auto* doctorMetadata = new std::string{};
    doctorCmd->add_option("--repo", *doctorRepo, "Repository root containing .kano/release.toml");
    doctorCmd->add_option("--metadata", *doctorMetadata, "Override release metadata path");
    doctorCmd->callback([=]() {
        const auto metadata = LoadSkillMetadataOrExit(*doctorRepo, *doctorMetadata, true);
        const auto plan = release::BuildSkillInstallPlan(metadata, {}, false);
        PrintSkillPlan(plan);
        std::cout << "installStateFile=" << (plan.targetRoot / ".kog-install.json").generic_string() << "\n";
        std::cout << "installStateExists="
                  << (std::filesystem::exists(plan.targetRoot / ".kog-install.json") ? "true" : "false") << "\n";
    });
}

} // namespace kano::git::commands
