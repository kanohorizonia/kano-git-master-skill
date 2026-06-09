#include "release_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <system_error>
#include <toml++/toml.h>

namespace kano::git::commands::release {
namespace {

auto ToLower(std::string value) -> std::string {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

auto NormalizeForCompare(const std::filesystem::path& path) -> std::string {
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = std::filesystem::absolute(path, ec);
        if (ec) {
            normalized = path;
        }
        normalized = normalized.lexically_normal();
    }
    std::string out = normalized.generic_string();
#if defined(_WIN32)
    out = ToLower(out);
#endif
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

auto IsPathInsideOrSame(const std::filesystem::path& child, const std::filesystem::path& parent) -> bool {
    const auto childText = NormalizeForCompare(child);
    const auto parentText = NormalizeForCompare(parent);
    if (childText == parentText) {
        return true;
    }
    return childText.rfind(parentText + "/", 0) == 0;
}

auto StringAt(const toml::table& root, std::initializer_list<const char*> keys) -> std::string {
    const toml::node* node = &root;
    for (const char* key : keys) {
        const toml::table* table = node->as_table();
        if (table == nullptr) {
            return {};
        }
        node = table->get(key);
        if (node == nullptr) {
            return {};
        }
    }
    if (const auto value = node->value<std::string>(); value.has_value()) {
        return TrimCopy(*value);
    }
    return {};
}

auto BoolAt(const toml::table& root, std::initializer_list<const char*> keys, bool defaultValue) -> bool {
    const toml::node* node = &root;
    for (const char* key : keys) {
        const toml::table* table = node->as_table();
        if (table == nullptr) {
            return defaultValue;
        }
        node = table->get(key);
        if (node == nullptr) {
            return defaultValue;
        }
    }
    if (const auto value = node->value<bool>(); value.has_value()) {
        return *value;
    }
    return defaultValue;
}

auto StringArrayAt(const toml::table& root, std::initializer_list<const char*> keys) -> std::vector<std::string> {
    const toml::node* node = &root;
    for (const char* key : keys) {
        const toml::table* table = node->as_table();
        if (table == nullptr) {
            return {};
        }
        node = table->get(key);
        if (node == nullptr) {
            return {};
        }
    }
    const auto* array = node->as_array();
    if (array == nullptr) {
        return {};
    }
    std::vector<std::string> out;
    out.reserve(array->size());
    for (const auto& item : *array) {
        if (const auto value = item.value<std::string>(); value.has_value()) {
            auto trimmed = TrimCopy(*value);
            if (!trimmed.empty()) {
                out.push_back(trimmed);
            }
        }
    }
    return out;
}

auto JsonPath(const std::filesystem::path& path) -> std::string {
    return path.generic_string();
}

auto JoinUrl(std::string base, const std::string& fileName) -> std::string {
    base = TrimCopy(std::move(base));
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.empty()) {
        return fileName;
    }
    return base + "/" + fileName;
}

auto DetectInstallerType(const std::filesystem::path& installerPath) -> std::string {
    auto ext = ToLower(installerPath.extension().string());
    if (ext == ".msi") {
        return "wix";
    }
    if (ext == ".exe") {
        return "exe";
    }
    return {};
}

auto BranchTokenFromPackageIdentifier(const std::string& packageIdentifier) -> std::string {
    std::string leaf = packageIdentifier;
    const auto dot = leaf.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < leaf.size()) {
        leaf = leaf.substr(dot + 1);
    }
    std::string out;
    for (const unsigned char c : leaf) {
        if (std::isalnum(c) != 0) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out.empty() ? "package" : out;
}

auto YamlQuote(const std::string& value) -> std::string {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::filesystem::path HomeDirectory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && std::string(home).size() > 0) {
        return std::filesystem::path(home);
    }
    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile != nullptr && std::string(userProfile).size() > 0) {
        return std::filesystem::path(userProfile);
    }
    return std::filesystem::current_path();
}

std::filesystem::path ExpandUserPath(const std::string& rawValue) {
    const auto raw = TrimCopy(rawValue);
    if (raw == "~") {
        return HomeDirectory();
    }
    if (raw.rfind("~/", 0) == 0) {
        return (HomeDirectory() / raw.substr(2)).lexically_normal();
    }
    return std::filesystem::path(raw).lexically_normal();
}

std::string TrimCopy(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        start += 1;
    }
    return value.substr(start);
}

std::string ReadTrimmedFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return TrimCopy(buffer.str());
}

std::string EnsureVersionWithoutPrefix(std::string version) {
    version = TrimCopy(std::move(version));
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        return version.substr(1);
    }
    return version;
}

std::string EnsureReleaseTag(std::string version) {
    version = TrimCopy(std::move(version));
    if (version.empty()) {
        return {};
    }
    if (version[0] == 'v' || version[0] == 'V') {
        return version;
    }
    return "v" + version;
}

LoadMetadataResult LoadReleaseMetadata(const std::filesystem::path& repoRootInput,
                                       const std::filesystem::path& metadataPathInput) {
    LoadMetadataResult result;
    std::error_code ec;
    auto repoRoot = std::filesystem::weakly_canonical(repoRootInput, ec);
    if (ec) {
        repoRoot = std::filesystem::absolute(repoRootInput, ec).lexically_normal();
    }
    const auto metadataPath = metadataPathInput.empty()
        ? (repoRoot / ".kano" / "release.toml").lexically_normal()
        : (metadataPathInput.is_relative()
            ? (repoRoot / metadataPathInput).lexically_normal()
            : std::filesystem::absolute(metadataPathInput, ec).lexically_normal());

    if (!std::filesystem::exists(metadataPath)) {
        result.code = "RELEASE_METADATA_MISSING";
        result.message = "release metadata not found: " + metadataPath.generic_string();
        return result;
    }

    toml::table parsed;
    try {
        parsed = toml::parse_file(metadataPath.string());
    } catch (const toml::parse_error& ex) {
        result.code = "RELEASE_METADATA_PARSE_ERROR";
        result.message = ex.description();
        return result;
    }

    ReleaseMetadata metadata;
    metadata.repoRoot = repoRoot;
    metadata.metadataPath = metadataPath;
    metadata.packageId = StringAt(parsed, {"release", "package_id"});
    metadata.packageName = StringAt(parsed, {"release", "package_name"});
    metadata.publisher = StringAt(parsed, {"release", "publisher"});
    metadata.moniker = StringAt(parsed, {"release", "moniker"});
    metadata.description = StringAt(parsed, {"release", "description"});
    metadata.license = StringAt(parsed, {"release", "license"});
    metadata.homepage = StringAt(parsed, {"release", "homepage"});
    metadata.version = EnsureVersionWithoutPrefix(ReadTrimmedFile(repoRoot / "VERSION"));
    metadata.skill.skillName = StringAt(parsed, {"release", "skill", "skill_name"});
    metadata.skill.defaultInstallTarget = StringAt(parsed, {"release", "skill", "default_install_target"});
    metadata.skill.developerProtectedPrefix = StringAt(parsed, {"release", "skill", "developer_protected_prefix"});
    metadata.windows.enabled = BoolAt(parsed, {"release", "windows", "enabled"}, false);
    metadata.windows.architectures = StringArrayAt(parsed, {"release", "windows", "architectures"});
    metadata.winget.enabled = BoolAt(parsed, {"release", "winget", "enabled"}, false);
    metadata.winget.packageIdentifier = StringAt(parsed, {"release", "winget", "package_identifier"});
    metadata.winget.forkRepo = StringAt(parsed, {"release", "winget", "fork_repo"});
    metadata.winget.upstreamRepo = StringAt(parsed, {"release", "winget", "upstream_repo"});

    if (metadata.skill.skillName.empty()) {
        metadata.skill.skillName = metadata.packageId;
    }
    if (metadata.skill.defaultInstallTarget.empty() && !metadata.skill.skillName.empty()) {
        metadata.skill.defaultInstallTarget = "~/agents/skills/" + metadata.skill.skillName;
    }
    if (metadata.skill.developerProtectedPrefix.empty()) {
        metadata.skill.developerProtectedPrefix = "~/.agents/skills/kano";
    }
    if (metadata.windows.architectures.empty()) {
        metadata.windows.architectures.push_back("x64");
    }
    if (metadata.winget.packageIdentifier.empty()) {
        metadata.winget.packageIdentifier = metadata.packageId;
    }

    if (metadata.packageId.empty() || metadata.packageName.empty() || metadata.version.empty()) {
        result.code = "RELEASE_METADATA_INCOMPLETE";
        result.message = "release metadata requires release.package_id, release.package_name, and VERSION";
        return result;
    }

    result.ok = true;
    result.code = "OK";
    result.metadata = std::move(metadata);
    return result;
}

SkillInstallPlan BuildSkillInstallPlan(const ReleaseMetadata& metadata,
                                       const std::filesystem::path& targetOverride,
                                       bool allowDeveloperTarget) {
    SkillInstallPlan plan;
    plan.sourceRepoRoot = metadata.repoRoot;
    const auto targetRaw = targetOverride.empty() ? metadata.skill.defaultInstallTarget : targetOverride.generic_string();
    plan.targetRoot = ExpandUserPath(targetRaw);
    plan.protectedPrefix = ExpandUserPath(metadata.skill.developerProtectedPrefix);
    plan.developerTargetProtected = IsPathInsideOrSame(plan.targetRoot, plan.protectedPrefix);
    plan.allowed = allowDeveloperTarget || !plan.developerTargetProtected;
    plan.statusCode = plan.allowed ? "OK" : "DEVELOPER_TARGET_PROTECTED";
    plan.actions.push_back("copy skill payload metadata");
    plan.actions.push_back("write .kog-install.json");
    plan.actions.push_back("install scripts/kog shim to delegate to package-managed binary root");
    return plan;
}

WindowsPackagePlan BuildWindowsPackagePlan(const ReleaseMetadata& metadata,
                                           const std::filesystem::path& outputRootInput) {
    WindowsPackagePlan plan;
    const auto outputRoot = outputRootInput.empty()
        ? (metadata.repoRoot / "artifacts" / "release" / "windows")
        : outputRootInput;
    plan.packageDirectoryName = metadata.packageName + "-" + metadata.version + "-windows-x64";
    plan.packageRoot = (outputRoot / plan.packageDirectoryName).lexically_normal();

    const std::vector<std::filesystem::path> candidates = {
        metadata.repoRoot / "src" / "cpp" / "build" / "bin" / "windows-ninja-msvc" / "release" / "kog.exe",
        metadata.repoRoot / "src" / "cpp" / "build" / "bin" / "windows-ninja-msvc" / "release" / "kano-git.exe",
        metadata.repoRoot / "out" / "bin" / "release" / "kog.exe",
        metadata.repoRoot / "out" / "bin" / "release" / "kano-git.exe",
    };
    plan.expectedBinaries = candidates;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            plan.foundBinaries.push_back(candidate.lexically_normal());
        }
    }
    const auto hasNamedBinary = [&](const std::string& name) {
        return std::any_of(plan.foundBinaries.begin(), plan.foundBinaries.end(), [&](const auto& path) {
            return ToLower(path.filename().string()) == name;
        });
    };
    plan.missingKogBinary = !hasNamedBinary("kog.exe");
    plan.missingKanoGitBinary = !hasNamedBinary("kano-git.exe");
    return plan;
}

std::string WingetManifestRelativeDirectory(const std::string& packageIdentifier,
                                            const std::string& versionInput) {
    const auto version = EnsureVersionWithoutPrefix(versionInput);
    std::vector<std::string> parts;
    std::stringstream ss(packageIdentifier);
    std::string item;
    while (std::getline(ss, item, '.')) {
        const auto trimmed = TrimCopy(item);
        if (!trimmed.empty()) {
            parts.push_back(trimmed);
        }
    }
    const std::string first = parts.empty() || parts.front().empty()
        ? "_"
        : ToLower(std::string(1, parts.front().front()));
    std::ostringstream out;
    out << "manifests/" << first;
    for (const auto& part : parts) {
        out << "/" << part;
    }
    out << "/" << version;
    return out.str();
}

WingetPlan BuildWingetPlan(const ReleaseMetadata& metadata,
                           const std::filesystem::path& installerPathInput,
                           const std::string& installerSha256Input,
                           const std::string& releaseAssetBaseUrlInput,
                           const std::filesystem::path& outputRootInput) {
    WingetPlan plan;
    plan.packageIdentifier = metadata.winget.packageIdentifier;
    plan.version = EnsureVersionWithoutPrefix(metadata.version);
    const auto outputRoot = outputRootInput.empty()
        ? (metadata.repoRoot / "Release" / "package-managers" / "winget")
        : outputRootInput;
    plan.manifestDirectory = (outputRoot / WingetManifestRelativeDirectory(plan.packageIdentifier, plan.version)).lexically_normal();
    plan.installerPath = installerPathInput;

    if (!metadata.winget.enabled) {
        plan.blockedReason = "WINGET_DISABLED";
        return plan;
    }
    if (installerPathInput.empty() || !std::filesystem::exists(installerPathInput)) {
        plan.blockedReason = "BLOCKED_INSTALLER_MISSING";
        return plan;
    }
    plan.installerFileName = installerPathInput.filename().string();
    plan.installerType = DetectInstallerType(installerPathInput);
    if (plan.installerType.empty()) {
        plan.blockedReason = "BLOCKED_INSTALLER_UNSUPPORTED";
        return plan;
    }
    plan.installerSha256 = TrimCopy(installerSha256Input);
    if (plan.installerSha256.empty()) {
        plan.blockedReason = "BLOCKED_INSTALLER_SHA256_MISSING";
        return plan;
    }

    std::string baseUrl = TrimCopy(releaseAssetBaseUrlInput);
    if (baseUrl.empty() && !metadata.homepage.empty()) {
        const auto tag = EnsureReleaseTag(plan.version);
        const std::string prefix = "https://github.com/";
        if (metadata.homepage.rfind(prefix, 0) == 0) {
            baseUrl = metadata.homepage + "/releases/download/" + tag;
        }
    }
    plan.installerUrl = JoinUrl(baseUrl, plan.installerFileName);

    const auto versionYamlName = plan.packageIdentifier + ".yaml";
    const auto installerYamlName = plan.packageIdentifier + ".installer.yaml";
    const auto localeYamlName = plan.packageIdentifier + ".locale.en-US.yaml";

    std::ostringstream versionYaml;
    versionYaml << "PackageIdentifier: " << plan.packageIdentifier << "\n"
                << "PackageVersion: " << plan.version << "\n"
                << "DefaultLocale: en-US\n"
                << "ManifestType: version\n"
                << "ManifestVersion: 1.6.0\n";

    std::ostringstream installerYaml;
    installerYaml << "PackageIdentifier: " << plan.packageIdentifier << "\n"
                  << "PackageVersion: " << plan.version << "\n"
                  << "Installers:\n"
                  << "  - Architecture: x64\n"
                  << "    InstallerType: " << plan.installerType << "\n"
                  << "    InstallerUrl: " << plan.installerUrl << "\n"
                  << "    InstallerSha256: " << plan.installerSha256 << "\n"
                  << "ManifestType: installer\n"
                  << "ManifestVersion: 1.6.0\n";

    std::ostringstream localeYaml;
    localeYaml << "PackageIdentifier: " << plan.packageIdentifier << "\n"
               << "PackageVersion: " << plan.version << "\n"
               << "PackageLocale: en-US\n"
               << "Publisher: " << metadata.publisher << "\n"
               << "PackageName: " << metadata.packageName << "\n"
               << "Moniker: " << metadata.moniker << "\n"
               << "ShortDescription: " << YamlQuote(metadata.description) << "\n"
               << "License: " << metadata.license << "\n"
               << "ManifestType: defaultLocale\n"
               << "ManifestVersion: 1.6.0\n";

    plan.manifestFiles[versionYamlName] = versionYaml.str();
    plan.manifestFiles[installerYamlName] = installerYaml.str();
    plan.manifestFiles[localeYamlName] = localeYaml.str();
    return plan;
}

WingetPrPlan BuildWingetPrPlan(const ReleaseMetadata& metadata,
                               const std::string& branchOverride) {
    WingetPrPlan plan;
    plan.forkRepo = metadata.winget.forkRepo;
    plan.upstreamRepo = metadata.winget.upstreamRepo;
    plan.branchName = TrimCopy(branchOverride);
    if (plan.branchName.empty()) {
        plan.branchName = "release/" + BranchTokenFromPackageIdentifier(metadata.winget.packageIdentifier) +
            "-winget-" + EnsureVersionWithoutPrefix(metadata.version);
    }
    plan.packagePath = WingetManifestRelativeDirectory(metadata.winget.packageIdentifier, metadata.version);
    plan.commitMessage = "New version: " + metadata.winget.packageIdentifier +
        " version " + EnsureVersionWithoutPrefix(metadata.version);
    plan.prTitle = "New version: " + metadata.winget.packageIdentifier +
        " version " + EnsureVersionWithoutPrefix(metadata.version);
    plan.prBody = "Adds winget manifests for " + metadata.packageName + " " + EnsureVersionWithoutPrefix(metadata.version) + ".";
    plan.commands.push_back("git clone " + plan.forkRepo + " winget-pkgs");
    plan.commands.push_back("git remote add upstream " + plan.upstreamRepo);
    plan.commands.push_back("git checkout -B " + plan.branchName);
    plan.commands.push_back("copy generated manifests to " + plan.packagePath);
    plan.commands.push_back("git commit -m \"" + plan.commitMessage + "\"");
    plan.commands.push_back("git push --force-with-lease origin " + plan.branchName);
    plan.commands.push_back("gh pr create --repo microsoft/winget-pkgs --head kanohorizonia:" + plan.branchName +
                            " --title \"" + plan.prTitle + "\" --body \"" + plan.prBody + "\"");
    return plan;
}

std::string RenderReleaseManifestJson(const ReleaseMetadata& metadata) {
    nlohmann::json j;
    j["schemaVersion"] = 1;
    j["packageId"] = metadata.packageId;
    j["packageName"] = metadata.packageName;
    j["publisher"] = metadata.publisher;
    j["moniker"] = metadata.moniker;
    j["version"] = EnsureVersionWithoutPrefix(metadata.version);
    j["releaseTag"] = EnsureReleaseTag(metadata.version);
    j["skill"] = {
        {"skillName", metadata.skill.skillName},
        {"defaultInstallTarget", metadata.skill.defaultInstallTarget},
        {"developerProtectedPrefix", metadata.skill.developerProtectedPrefix},
    };
    j["windows"] = {
        {"enabled", metadata.windows.enabled},
        {"architectures", metadata.windows.architectures},
    };
    j["winget"] = {
        {"enabled", metadata.winget.enabled},
        {"packageIdentifier", metadata.winget.packageIdentifier},
        {"forkRepo", metadata.winget.forkRepo},
        {"upstreamRepo", metadata.winget.upstreamRepo},
    };
    return j.dump(2) + "\n";
}

std::string RenderSkillInstallJson(const ReleaseMetadata& metadata,
                                   const std::filesystem::path& packageBinaryRoot) {
    nlohmann::json j;
    j["schemaVersion"] = 1;
    j["packageId"] = metadata.packageId;
    j["packageName"] = metadata.packageName;
    j["version"] = EnsureVersionWithoutPrefix(metadata.version);
    j["skillName"] = metadata.skill.skillName;
    j["packageBinaryRoot"] = JsonPath(packageBinaryRoot);
    j["kogBinary"] = JsonPath(packageBinaryRoot / "kog.exe");
    j["kanoGitBinary"] = JsonPath(packageBinaryRoot / "kano-git.exe");
    return j.dump(2) + "\n";
}

} // namespace kano::git::commands::release
