#pragma once

#include <filesystem>
#include <string>

namespace kano::git::commands::runtime_path {

// Stable KOG workspace and asset paths belong here. Command code may select a
// filename or honor a command-specific override, but it must not reconstruct
// the product layout one path segment at a time.
class Layout {
public:
    static auto Resolve(const std::filesystem::path& InWorkspaceRoot) -> Layout;
    static auto ForRoots(const std::filesystem::path& InWorkspaceRoot,
                         const std::filesystem::path& InSkillRoot) -> Layout;

    [[nodiscard]] auto WorkspaceRoot() const -> const std::filesystem::path&;
    [[nodiscard]] auto SkillRoot() const -> const std::filesystem::path&;

    [[nodiscard]] auto WorkspaceKanoRoot() const -> std::filesystem::path;
    [[nodiscard]] auto WorkspaceTemporaryRoot() const -> std::filesystem::path;
    [[nodiscard]] auto WorkspaceTemporaryPath(const std::string& InFilename) const -> std::filesystem::path;
    [[nodiscard]] auto WorkspaceGitTemporaryRoot() const -> std::filesystem::path;
    [[nodiscard]] auto WorkspacePlanRoot() const -> std::filesystem::path;
    [[nodiscard]] auto WorkspaceCacheRoot() const -> std::filesystem::path;
    [[nodiscard]] auto WorkspacePlanCacheRoot() const -> std::filesystem::path;
    [[nodiscard]] auto DefaultPlanPath() const -> std::filesystem::path;
    [[nodiscard]] auto SharedPlanPath() const -> std::filesystem::path;
    [[nodiscard]] auto PlanPath(const std::string& InFilename) const -> std::filesystem::path;
    [[nodiscard]] auto CachedPlanPath(const std::string& InFilename) const -> std::filesystem::path;
    [[nodiscard]] auto ProviderPromptRoot() const -> std::filesystem::path;
    [[nodiscard]] auto AiResponseRoot() const -> std::filesystem::path;
    [[nodiscard]] auto CodexResponseRoot() const -> std::filesystem::path;
    [[nodiscard]] auto ExportRoot() const -> std::filesystem::path;
    [[nodiscard]] auto DebugLogRoot() const -> std::filesystem::path;

    [[nodiscard]] auto UsesLegacyIgnoreAssetLayout() const -> bool;
    [[nodiscard]] auto IgnoreAssetRoot() const -> std::filesystem::path;
    [[nodiscard]] auto IgnoreDatasourceRoot() const -> std::filesystem::path;
    [[nodiscard]] auto IgnoreDatasourceManifest() const -> std::filesystem::path;
    [[nodiscard]] auto IgnoreLocalRules() const -> std::filesystem::path;
    [[nodiscard]] auto IgnoreGateAllowlist() const -> std::filesystem::path;
    [[nodiscard]] auto IgnoreUpstreamCorpus() const -> std::filesystem::path;
    [[nodiscard]] auto IgnoreUpstreamCorpusRelativeToSkill() const -> std::filesystem::path;
    [[nodiscard]] auto RegressionAssetRoot() const -> std::filesystem::path;
    [[nodiscard]] auto RegressionIncidentManifest() const -> std::filesystem::path;
    [[nodiscard]] auto RegressionCaseTemplate() const -> std::filesystem::path;
    [[nodiscard]] auto AuditSchemaRoot() const -> std::filesystem::path;
    [[nodiscard]] auto AuditEventSchemaV1() const -> std::filesystem::path;
    [[nodiscard]] auto RunReceiptSchemaV1() const -> std::filesystem::path;

private:
    Layout(std::filesystem::path InWorkspaceRoot, std::filesystem::path InSkillRoot);

    std::filesystem::path workspaceRoot_;
    std::filesystem::path skillRoot_;
};

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path;
auto ResolveSkillRootFromBinaryPath(const std::filesystem::path& InBinaryPath)
    -> std::filesystem::path;
auto GlobalCacheRoot(const std::filesystem::path& InHomeDirectory) -> std::filesystem::path;
auto NativeIoPath(const std::filesystem::path& InPath) -> std::filesystem::path;

} // namespace kano::git::commands::runtime_path
