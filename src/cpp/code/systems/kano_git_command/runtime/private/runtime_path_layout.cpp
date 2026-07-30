#include "runtime_path_layout.hpp"

#include <array>
#include <cstdlib>
#include <system_error>

namespace kano::git::commands::runtime_path {

namespace {

auto Normalize(const std::filesystem::path& InPath) -> std::filesystem::path {
    return InPath.empty() ? std::filesystem::path{} : InPath.lexically_normal();
}

auto IsSkillRoot(const std::filesystem::path& InCandidate) -> bool {
    if (InCandidate.empty()) {
        return false;
    }

    std::error_code ec;
    const auto marker = (InCandidate / "SKILL.md").lexically_normal();
    if (!std::filesystem::is_regular_file(marker, ec) || ec) {
        return false;
    }

    if (InCandidate.filename() != "kano-git-master-skill") {
        return false;
    }

    ec.clear();
    const auto regressionManifest =
        (InCandidate / "assets" / "regression" / "incidents.json").lexically_normal();
    const bool hasRegressionManifest =
        std::filesystem::is_regular_file(regressionManifest, ec) && !ec;
    ec.clear();
    const auto regressionTemplate =
        (InCandidate / "assets" / "regression" / "case-template.json").lexically_normal();
    const bool hasRegressionTemplate =
        std::filesystem::is_regular_file(regressionTemplate, ec) && !ec;
    if (hasRegressionManifest && hasRegressionTemplate) {
        return true;
    }

    ec.clear();
    const auto launcher = (InCandidate / "scripts" / "kog").lexically_normal();
    if (std::filesystem::is_regular_file(launcher, ec) && !ec) {
        return true;
    }

    ec.clear();
    const auto planAsset = (InCandidate / "assets" / "plan" / "default-plan.json").lexically_normal();
    const auto securityAsset =
        (InCandidate / "assets" / "security" / "secret-blacklist.rules").lexically_normal();
    const bool hasPlanAsset = std::filesystem::is_regular_file(planAsset, ec) && !ec;
    ec.clear();
    const bool hasSecurityAsset = std::filesystem::is_regular_file(securityAsset, ec) && !ec;
    return hasPlanAsset && hasSecurityAsset;
}

auto IsPortableRuntimeRoot(const std::filesystem::path& InCandidate) -> bool {
    if (InCandidate.empty()) {
        return false;
    }

    std::error_code ec;
    const auto skillMarker = (InCandidate / "SKILL.md").lexically_normal();
    const bool hasSkillMarker =
        std::filesystem::is_regular_file(skillMarker, ec) && !ec;
    ec.clear();
    const auto regressionManifest =
        (InCandidate / "assets" / "regression" / "incidents.json").lexically_normal();
    const bool hasRegressionManifest =
        std::filesystem::is_regular_file(regressionManifest, ec) && !ec;
    ec.clear();
    const auto regressionTemplate =
        (InCandidate / "assets" / "regression" / "case-template.json").lexically_normal();
    const bool hasRegressionTemplate =
        std::filesystem::is_regular_file(regressionTemplate, ec) && !ec;
    return hasSkillMarker && hasRegressionManifest && hasRegressionTemplate;
}

auto EnvPath(const char* InName) -> std::filesystem::path {
    const char* raw = std::getenv(InName);
    if (raw == nullptr || *raw == '\0') {
        return {};
    }
    return Normalize(std::filesystem::path(raw));
}

auto ExistingSkillCandidate(const std::filesystem::path& InCandidate) -> std::filesystem::path {
    const auto normalized = Normalize(InCandidate);
    return IsSkillRoot(normalized) ? normalized : std::filesystem::path{};
}

auto ExistingPortableRuntimeCandidate(const std::filesystem::path& InCandidate)
    -> std::filesystem::path {
    const auto normalized = Normalize(InCandidate);
    return IsPortableRuntimeRoot(normalized) ? normalized : std::filesystem::path{};
}

auto HomeDirectory() -> std::filesystem::path {
    if (const auto userProfile = EnvPath("USERPROFILE"); !userProfile.empty()) {
        return userProfile;
    }
    return EnvPath("HOME");
}

} // namespace

Layout::Layout(std::filesystem::path InWorkspaceRoot, std::filesystem::path InSkillRoot)
    : workspaceRoot_(Normalize(InWorkspaceRoot)),
      skillRoot_(Normalize(InSkillRoot)) {}

auto Layout::Resolve(const std::filesystem::path& InWorkspaceRoot) -> Layout {
    return Layout(InWorkspaceRoot, ResolveSkillRoot(InWorkspaceRoot));
}

auto Layout::ForRoots(const std::filesystem::path& InWorkspaceRoot,
                      const std::filesystem::path& InSkillRoot) -> Layout {
    return Layout(InWorkspaceRoot, InSkillRoot);
}

auto Layout::WorkspaceRoot() const -> const std::filesystem::path& {
    return workspaceRoot_;
}

auto Layout::SkillRoot() const -> const std::filesystem::path& {
    return skillRoot_;
}

auto Layout::WorkspaceKanoRoot() const -> std::filesystem::path {
    return (workspaceRoot_ / ".kano").lexically_normal();
}

auto Layout::WorkspaceTemporaryRoot() const -> std::filesystem::path {
    return (WorkspaceKanoRoot() / "tmp").lexically_normal();
}

auto Layout::WorkspaceTemporaryPath(const std::string& InFilename) const -> std::filesystem::path {
    return (WorkspaceTemporaryRoot() / InFilename).lexically_normal();
}

auto Layout::WorkspaceGitTemporaryRoot() const -> std::filesystem::path {
    return (WorkspaceTemporaryRoot() / "git").lexically_normal();
}

auto Layout::WorkspacePlanRoot() const -> std::filesystem::path {
    return (WorkspaceGitTemporaryRoot() / "plans").lexically_normal();
}

auto Layout::WorkspaceCacheRoot() const -> std::filesystem::path {
    return (WorkspaceKanoRoot() / "cache" / "git").lexically_normal();
}

auto Layout::WorkspacePlanCacheRoot() const -> std::filesystem::path {
    return (WorkspaceCacheRoot() / "plans").lexically_normal();
}

auto Layout::DefaultPlanPath() const -> std::filesystem::path {
    return PlanPath("default-plan.json");
}

auto Layout::SharedPlanPath() const -> std::filesystem::path {
    if (const auto overridePath = EnvPath("KOG_PLAN_FILE"); !overridePath.empty()) {
        return overridePath;
    }
    return DefaultPlanPath();
}

auto Layout::PlanPath(const std::string& InFilename) const -> std::filesystem::path {
    return (WorkspacePlanRoot() / InFilename).lexically_normal();
}

auto Layout::CachedPlanPath(const std::string& InFilename) const -> std::filesystem::path {
    return (WorkspacePlanCacheRoot() / InFilename).lexically_normal();
}

auto Layout::ProviderPromptRoot() const -> std::filesystem::path {
    return (WorkspaceGitTemporaryRoot() / "provider-prompts").lexically_normal();
}

auto Layout::AiResponseRoot() const -> std::filesystem::path {
    return (WorkspaceGitTemporaryRoot() / "ai-responses").lexically_normal();
}

auto Layout::CodexResponseRoot() const -> std::filesystem::path {
    return (WorkspaceGitTemporaryRoot() / "codex-responses").lexically_normal();
}

auto Layout::ExportRoot() const -> std::filesystem::path {
    return (WorkspaceGitTemporaryRoot() / "export").lexically_normal();
}

auto Layout::DebugLogRoot() const -> std::filesystem::path {
    return (WorkspaceGitTemporaryRoot() / "log").lexically_normal();
}

auto Layout::UsesLegacyIgnoreAssetLayout() const -> bool {
    std::error_code ec;
    const auto canonical = (skillRoot_ / "assets" / "ignore").lexically_normal();
    if (std::filesystem::is_directory(canonical, ec) && !ec) {
        return false;
    }

    ec.clear();
    const auto legacy = (skillRoot_ / "assets" / "ignore-sources").lexically_normal();
    return std::filesystem::is_directory(legacy, ec) && !ec;
}

auto Layout::IgnoreAssetRoot() const -> std::filesystem::path {
    const char* folder = UsesLegacyIgnoreAssetLayout() ? "ignore-sources" : "ignore";
    return (skillRoot_ / "assets" / folder).lexically_normal();
}

auto Layout::IgnoreDatasourceRoot() const -> std::filesystem::path {
    if (UsesLegacyIgnoreAssetLayout()) {
        return IgnoreAssetRoot();
    }
    return (IgnoreAssetRoot() / "datasource").lexically_normal();
}

auto Layout::IgnoreDatasourceManifest() const -> std::filesystem::path {
    if (UsesLegacyIgnoreAssetLayout()) {
        return (IgnoreAssetRoot() / "local" / "datasource.manifest.json").lexically_normal();
    }
    return (IgnoreDatasourceRoot() / "manifest.json").lexically_normal();
}

auto Layout::IgnoreLocalRules() const -> std::filesystem::path {
    if (UsesLegacyIgnoreAssetLayout()) {
        return (IgnoreAssetRoot() / "local" / "custom.gitignore").lexically_normal();
    }
    return (IgnoreAssetRoot() / "local-rules" / "kano.gitignore").lexically_normal();
}

auto Layout::IgnoreGateAllowlist() const -> std::filesystem::path {
    if (UsesLegacyIgnoreAssetLayout()) {
        return (IgnoreAssetRoot() / "local" / "ignore-gate-allowlist.txt").lexically_normal();
    }
    return (IgnoreAssetRoot() / "policy" / "ignore-gate-allowlist.txt").lexically_normal();
}

auto Layout::IgnoreUpstreamCorpus() const -> std::filesystem::path {
    const auto relative = UsesLegacyIgnoreAssetLayout()
        ? std::filesystem::path("upstream") / "github-gitignore"
        : std::filesystem::path("upstream") / "github-gitignore";
    return (IgnoreDatasourceRoot() / relative).lexically_normal();
}

auto Layout::IgnoreUpstreamCorpusRelativeToSkill() const -> std::filesystem::path {
    return IgnoreUpstreamCorpus().lexically_relative(skillRoot_).lexically_normal();
}

auto Layout::RegressionAssetRoot() const -> std::filesystem::path {
    return (skillRoot_ / "assets" / "regression").lexically_normal();
}

auto Layout::RegressionIncidentManifest() const -> std::filesystem::path {
    return (RegressionAssetRoot() / "incidents.json").lexically_normal();
}

auto Layout::RegressionCaseTemplate() const -> std::filesystem::path {
    return (RegressionAssetRoot() / "case-template.json").lexically_normal();
}

auto Layout::AuditSchemaRoot() const -> std::filesystem::path {
    return (skillRoot_ / "assets" / "audit" / "schemas").lexically_normal();
}

auto Layout::AuditEventSchemaV1() const -> std::filesystem::path {
    return (AuditSchemaRoot() / "kog.auditEvent.v1.schema.json").lexically_normal();
}

auto Layout::RunReceiptSchemaV1() const -> std::filesystem::path {
    return (AuditSchemaRoot() / "kog.runReceipt.v1.schema.json").lexically_normal();
}

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    // Explicit launcher/package contracts take precedence and intentionally do
    // not require probing: callers may resolve assets before materialization.
    if (const auto explicitRoot = EnvPath("KANO_GIT_SKILL_ROOT"); !explicitRoot.empty()) {
        return explicitRoot;
    }
    if (const auto launcherRoot = EnvPath("KANO_GIT_MASTER_ROOT"); !launcherRoot.empty()) {
        return launcherRoot;
    }
    if (const auto binaryPath = EnvPath("KANO_GIT_BINARY_PATH"); !binaryPath.empty()) {
        if (const auto binarySkillRoot = ResolveSkillRootFromBinaryPath(binaryPath);
            !binarySkillRoot.empty()) {
            return binarySkillRoot;
        }
    }

    const auto workspace = Normalize(InWorkspaceRoot);
    const std::array workspaceCandidates = {
        workspace,
        workspace / ".agents" / "skills" / "kano" / "kano-git-master-skill",
        workspace / ".agents" / "kano" / "kano-git-master-skill",
        workspace / "kano-git-master-skill",
    };
    for (const auto& candidate : workspaceCandidates) {
        if (const auto found = ExistingSkillCandidate(candidate); !found.empty()) {
            return found;
        }
    }

    auto current = workspace;
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        if (const auto found = ExistingSkillCandidate(current); !found.empty()) {
            return found;
        }
        if (const auto found = ExistingSkillCandidate(current / "kano-git-master-skill"); !found.empty()) {
            return found;
        }
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    if (const auto home = HomeDirectory(); !home.empty()) {
        if (const auto found = ExistingSkillCandidate(
                home / ".agents" / "skills" / "kano" / "kano-git-master-skill");
            !found.empty()) {
            return found;
        }
    }

    // Preserve the historical dev-mode fallback even when the skill has not
    // been initialized yet. The resulting deterministic path gives callers a
    // useful missing-asset diagnostic.
    return (workspace / ".agents" / "skills" / "kano" / "kano-git-master-skill").lexically_normal();
}

auto ResolveSkillRootFromBinaryPath(const std::filesystem::path& InBinaryPath)
    -> std::filesystem::path {
    if (InBinaryPath.empty()) {
        return {};
    }

    std::error_code ec;
    auto binaryPath = InBinaryPath;
    if (binaryPath.is_relative()) {
        binaryPath = std::filesystem::absolute(binaryPath, ec);
        if (ec) {
            binaryPath = InBinaryPath;
        }
    }
    binaryPath = Normalize(binaryPath);
    auto current = binaryPath.parent_path();
    if (current.empty()) {
        return {};
    }

    // Installed Windows packages place binaries and skills under sibling
    // roots: <package>/bin and <package>/skills/kano-git-master-skill.
    if (const auto found = ExistingPortableRuntimeCandidate(
            current.parent_path() / "skills" / "kano-git-master-skill");
        !found.empty()) {
        return found;
    }

    // Runtime artifacts keep bin/ below the skill payload root. Continue
    // probing ancestors so the same contract also serves portable archives.
    for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
        if (const auto found = ExistingPortableRuntimeCandidate(current); !found.empty()) {
            return found;
        }
        if (const auto found = ExistingPortableRuntimeCandidate(
                current / "skills" / "kano-git-master-skill");
            !found.empty()) {
            return found;
        }
        const auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return {};
}

auto GlobalCacheRoot(const std::filesystem::path& InHomeDirectory) -> std::filesystem::path {
    if (InHomeDirectory.empty()) {
        return {};
    }
    return (InHomeDirectory / ".kano" / "cache" / "git").lexically_normal();
}

} // namespace kano::git::commands::runtime_path
