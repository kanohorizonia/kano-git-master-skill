// version command — project version management (show, set, bump, tag, next)

#include <CLI/CLI.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "shell_executor.hpp"

namespace kano::git::commands {

namespace {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;
    std::string build;

    std::string ToString() const {
        std::string out = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        if (!prerelease.empty()) {
            out += "-" + prerelease;
        }
        if (!build.empty()) {
            out += "+" + build;
        }
        return out;
    }
};

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto ParseSemVer(const std::string& InValue) -> std::optional<SemVer> {
    static const std::regex re(R"(^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z\.-]+))?(?:\+([0-9A-Za-z\.-]+))?$)");
    std::smatch match;
    if (!std::regex_match(InValue, match, re)) {
        return std::nullopt;
    }

    try {
        SemVer v;
        v.major = std::stoi(match[1].str());
        v.minor = std::stoi(match[2].str());
        v.patch = std::stoi(match[3].str());
        v.prerelease = match[4].str();
        v.build = match[5].str();
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

auto ReadVersionFile(const std::filesystem::path& InRepo) -> std::optional<std::string> {
    const auto versionPath = InRepo / "VERSION";
    if (!std::filesystem::exists(versionPath)) {
        return std::nullopt;
    }
    std::ifstream in(versionPath);
    if (!in) {
        return std::nullopt;
    }
    std::string version;
    std::getline(in, version);
    return Trim(version);
}

auto WriteVersionFile(const std::filesystem::path& InRepo, const std::string& InVersion) -> bool {
    const auto versionPath = InRepo / "VERSION";
    std::ofstream out(versionPath, std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << InVersion << "\n";
    return static_cast<bool>(out);
}

} // namespace

void RegisterVersion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("version", "Project version management");
    
    auto* repo = new std::string{"."};
    cmd->add_option("--repo", *repo, "Project repository root path")->default_str(".");

    auto* show = cmd->add_subcommand("show", "Show current project version (default)");
    auto* set = cmd->add_subcommand("set", "Set project version");
    auto* bump = cmd->add_subcommand("bump", "Bump project version (major, minor, or patch)");
    auto* tag = cmd->add_subcommand("tag", "Create a git tag for the current version");
    auto* next = cmd->add_subcommand("next", "Show what the next version would be");

    // set arguments
    auto* newVersion = new std::string{};
    set->add_option("version", *newVersion, "New version string")->required();

    // bump/next arguments
    auto* bumpPart = new std::string{"patch"};
    bump->add_option("part", *bumpPart, "Part to bump: major|minor|patch")->default_str("patch");
    next->add_option("part", *bumpPart, "Part to bump: major|minor|patch")->default_str("patch");

    auto getVersionOrExit = [=](const std::filesystem::path& repoPath) -> SemVer {
        auto verStr = ReadVersionFile(repoPath);
        if (!verStr) {
            std::cerr << "Error: VERSION file not found in " << repoPath.generic_string() << "\n";
            std::exit(1);
        }
        auto v = ParseSemVer(*verStr);
        if (!v) {
            std::cerr << "Error: Invalid version format in VERSION file: " << *verStr << "\n";
            std::exit(1);
        }
        return *v;
    };

    auto bumpVersion = [](SemVer v, const std::string& part) -> SemVer {
        if (part == "major") {
            v.major += 1;
            v.minor = 0;
            v.patch = 0;
        } else if (part == "minor") {
            v.minor += 1;
            v.patch = 0;
        } else {
            v.patch += 1;
        }
        v.prerelease.clear();
        v.build.clear();
        return v;
    };

    show->callback([=]() {
        const auto repoPath = std::filesystem::absolute(*repo).lexically_normal();
        auto verStr = ReadVersionFile(repoPath);
        if (verStr) {
            std::cout << *verStr << "\n";
        } else {
            std::cerr << "Error: VERSION file not found\n";
            std::exit(1);
        }
    });

    set->callback([=]() {
        const auto repoPath = std::filesystem::absolute(*repo).lexically_normal();
        auto v = ParseSemVer(*newVersion);
        if (!v) {
            std::cerr << "Error: Invalid version format: " << *newVersion << "\n";
            std::exit(1);
        }
        if (!WriteVersionFile(repoPath, v->ToString())) {
            std::cerr << "Error: Failed to write VERSION file\n";
            std::exit(1);
        }
        std::cout << "Version set to " << v->ToString() << "\n";
    });

    bump->callback([=]() {
        const auto repoPath = std::filesystem::absolute(*repo).lexically_normal();
        auto v = getVersionOrExit(repoPath);
        auto nextV = bumpVersion(v, *bumpPart);
        if (!WriteVersionFile(repoPath, nextV.ToString())) {
            std::cerr << "Error: Failed to write VERSION file\n";
            std::exit(1);
        }
        std::cout << "Version bumped from " << v.ToString() << " to " << nextV.ToString() << "\n";
    });

    next->callback([=]() {
        const auto repoPath = std::filesystem::absolute(*repo).lexically_normal();
        auto v = getVersionOrExit(repoPath);
        auto nextV = bumpVersion(v, *bumpPart);
        std::cout << nextV.ToString() << "\n";
    });

    tag->callback([=]() {
        const auto repoPath = std::filesystem::absolute(*repo).lexically_normal();
        auto v = getVersionOrExit(repoPath);
        std::string tagName = "v" + v.ToString();
        
        std::cout << "Creating git tag " << tagName << "...\n";
        auto result = shell::ExecuteCommand("git", {"tag", "-a", tagName, "-m", "Release " + tagName}, shell::ExecMode::PassThrough, repoPath);
        if (result.exitCode != 0) {
            std::cerr << "Error: Failed to create git tag\n";
            std::exit(1);
        }
        std::cout << "Tag created successfully.\n";
    });

    // Default action: show
    cmd->callback([=]() {
        if (cmd->get_subcommands().empty()) {
            show->execute();
        }
    });
}

} // namespace kano::git::commands
