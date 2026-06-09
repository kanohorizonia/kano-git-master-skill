#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kano::git::commands::release {

struct ReleaseSkillMetadata {
    std::string skillName;
    std::string defaultInstallTarget;
    std::string developerProtectedPrefix;
};

struct ReleaseWindowsMetadata {
    bool enabled{false};
    std::vector<std::string> architectures;
};

struct ReleaseWingetMetadata {
    bool enabled{false};
    std::string packageIdentifier;
    std::string forkRepo;
    std::string upstreamRepo;
};

struct ReleaseMetadata {
    std::filesystem::path repoRoot;
    std::filesystem::path metadataPath;
    std::string packageId;
    std::string packageName;
    std::string publisher;
    std::string moniker;
    std::string description;
    std::string license;
    std::string homepage;
    std::string version;
    ReleaseSkillMetadata skill;
    ReleaseWindowsMetadata windows;
    ReleaseWingetMetadata winget;
};

struct LoadMetadataResult {
    bool ok{false};
    std::string code;
    std::string message;
    ReleaseMetadata metadata;
};

struct SkillInstallPlan {
    std::filesystem::path sourceRepoRoot;
    std::filesystem::path targetRoot;
    std::filesystem::path protectedPrefix;
    bool developerTargetProtected{false};
    bool allowed{false};
    std::string statusCode;
    std::vector<std::string> actions;
};

struct WindowsPackagePlan {
    std::filesystem::path packageRoot;
    std::string packageDirectoryName;
    std::vector<std::filesystem::path> expectedBinaries;
    std::vector<std::filesystem::path> foundBinaries;
    bool missingKogBinary{false};
    bool missingKanoGitBinary{false};
};

struct WingetPlan {
    std::string packageIdentifier;
    std::string version;
    std::filesystem::path manifestDirectory;
    std::filesystem::path installerPath;
    std::string installerFileName;
    std::string installerType;
    std::string installerSha256;
    std::string installerUrl;
    std::string blockedReason;
    std::map<std::string, std::string> manifestFiles;
};

struct WingetPrPlan {
    std::string forkRepo;
    std::string upstreamRepo;
    std::string branchName;
    std::string packagePath;
    std::string commitMessage;
    std::string prTitle;
    std::string prBody;
    std::vector<std::string> commands;
};

std::filesystem::path HomeDirectory();
std::filesystem::path ExpandUserPath(const std::string& raw);
std::string TrimCopy(std::string value);
std::string ReadTrimmedFile(const std::filesystem::path& path);
std::string EnsureVersionWithoutPrefix(std::string version);
std::string EnsureReleaseTag(std::string version);

LoadMetadataResult LoadReleaseMetadata(const std::filesystem::path& repoRoot,
                                       const std::filesystem::path& metadataPath = {});

SkillInstallPlan BuildSkillInstallPlan(const ReleaseMetadata& metadata,
                                       const std::filesystem::path& targetOverride,
                                       bool allowDeveloperTarget);

WindowsPackagePlan BuildWindowsPackagePlan(const ReleaseMetadata& metadata,
                                           const std::filesystem::path& outputRoot);

WingetPlan BuildWingetPlan(const ReleaseMetadata& metadata,
                           const std::filesystem::path& installerPath,
                           const std::string& installerSha256,
                           const std::string& releaseAssetBaseUrl,
                           const std::filesystem::path& outputRoot);

WingetPrPlan BuildWingetPrPlan(const ReleaseMetadata& metadata,
                               const std::string& branchOverride);

std::string WingetManifestRelativeDirectory(const std::string& packageIdentifier,
                                            const std::string& version);
std::string RenderReleaseManifestJson(const ReleaseMetadata& metadata);
std::string RenderSkillInstallJson(const ReleaseMetadata& metadata,
                                   const std::filesystem::path& packageBinaryRoot);

} // namespace kano::git::commands::release
