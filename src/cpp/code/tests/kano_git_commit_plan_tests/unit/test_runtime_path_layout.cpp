#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"
#include "runtime_path_layout.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_set>

namespace {

namespace runtime_path = kano::git::commands::runtime_path;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& InName) {
        static std::atomic<unsigned long long> counter{0};
        path_ = (std::filesystem::temp_directory_path() /
                 ("kog-runtime-layout-" + InName + "-" + std::to_string(++counter)))
                    .lexically_normal();
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] auto Path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* InName, std::optional<std::string> InValue)
        : name_(InName) {
        if (const char* previous = std::getenv(InName); previous != nullptr) {
            previous_ = std::string(previous);
        }
        Set(std::move(InValue));
    }

    ~ScopedEnvironment() {
        Set(previous_);
    }

private:
    auto Set(const std::optional<std::string>& InValue) -> void {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), InValue.has_value() ? InValue->c_str() : "");
#else
        if (InValue.has_value()) {
            setenv(name_.c_str(), InValue->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

auto WriteMarker(const std::filesystem::path& InPath) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath);
    out << "marker\n";
}

} // namespace

TEST_CASE("runtime layout owns stable workspace plan and cache paths", "[runtime-layout]") {
    const auto layout = runtime_path::Layout::ForRoots("/workspace/project", "/package/kog");

    REQUIRE(layout.WorkspaceKanoRoot() == std::filesystem::path("/workspace/project/.kano"));
    REQUIRE(layout.WorkspaceTemporaryRoot() == std::filesystem::path("/workspace/project/.kano/tmp"));
    REQUIRE(layout.WorkspaceGitTemporaryRoot() == std::filesystem::path("/workspace/project/.kano/tmp/git"));
    REQUIRE(layout.WorkspacePlanRoot() == std::filesystem::path("/workspace/project/.kano/tmp/git/plans"));
    REQUIRE(layout.DefaultPlanPath() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/plans/default-plan.json"));
    REQUIRE(layout.PlanPath("generated.json") ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/plans/generated.json"));
    REQUIRE(layout.WorkspaceCacheRoot() == std::filesystem::path("/workspace/project/.kano/cache/git"));
    REQUIRE(layout.CachedPlanPath("message.json") ==
            std::filesystem::path("/workspace/project/.kano/cache/git/plans/message.json"));
    REQUIRE(layout.ProviderPromptRoot() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/provider-prompts"));
    REQUIRE(layout.AiResponseRoot() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/ai-responses"));
    REQUIRE(layout.CodexResponseRoot() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/codex-responses"));
    REQUIRE(layout.ExportRoot() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/export"));
    REQUIRE(layout.DebugLogRoot() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/log"));
    REQUIRE(runtime_path::GlobalCacheRoot("/users/operator") ==
            std::filesystem::path("/users/operator/.kano/cache/git"));
}

TEST_CASE("shared plan override does not mutate the stable default", "[runtime-layout]") {
    ScopedEnvironment planOverride("KOG_PLAN_FILE", std::string{"custom/operator-plan.json"});
    const auto layout = runtime_path::Layout::ForRoots("/workspace/project", "/package/kog");

    REQUIRE(layout.SharedPlanPath() == std::filesystem::path("custom/operator-plan.json"));
    REQUIRE(layout.DefaultPlanPath() ==
            std::filesystem::path("/workspace/project/.kano/tmp/git/plans/default-plan.json"));
}

TEST_CASE("canonical ignore assets win when canonical and legacy layouts coexist", "[runtime-layout]") {
    TemporaryDirectory temp("canonical-assets");
    const auto skillRoot = temp.Path() / "kano-git-master-skill";
    std::filesystem::create_directories(skillRoot / "assets" / "ignore" / "datasource");
    std::filesystem::create_directories(skillRoot / "assets" / "ignore-sources" / "local");

    const auto layout = runtime_path::Layout::ForRoots(temp.Path() / "workspace", skillRoot);
    REQUIRE_FALSE(layout.UsesLegacyIgnoreAssetLayout());
    REQUIRE(layout.IgnoreDatasourceRoot() == skillRoot / "assets" / "ignore" / "datasource");
    REQUIRE(layout.IgnoreDatasourceManifest() ==
            skillRoot / "assets" / "ignore" / "datasource" / "manifest.json");
    REQUIRE(layout.IgnoreLocalRules() ==
            skillRoot / "assets" / "ignore" / "local-rules" / "kano.gitignore");
    REQUIRE(layout.IgnoreGateAllowlist() ==
            skillRoot / "assets" / "ignore" / "policy" / "ignore-gate-allowlist.txt");
    REQUIRE(layout.IgnoreUpstreamCorpusRelativeToSkill() ==
            std::filesystem::path("assets/ignore/datasource/upstream/github-gitignore"));
}

TEST_CASE("legacy packaged ignore assets remain readable", "[runtime-layout]") {
    TemporaryDirectory temp("legacy-assets");
    const auto skillRoot = temp.Path() / "packaged-kog";
    std::filesystem::create_directories(skillRoot / "assets" / "ignore-sources" / "local");

    const auto layout = runtime_path::Layout::ForRoots(temp.Path() / "workspace", skillRoot);
    REQUIRE(layout.UsesLegacyIgnoreAssetLayout());
    REQUIRE(layout.IgnoreDatasourceRoot() == skillRoot / "assets" / "ignore-sources");
    REQUIRE(layout.IgnoreDatasourceManifest() ==
            skillRoot / "assets" / "ignore-sources" / "local" / "datasource.manifest.json");
    REQUIRE(layout.IgnoreLocalRules() ==
            skillRoot / "assets" / "ignore-sources" / "local" / "custom.gitignore");
    REQUIRE(layout.IgnoreGateAllowlist() ==
            skillRoot / "assets" / "ignore-sources" / "local" / "ignore-gate-allowlist.txt");
}

TEST_CASE("skill root resolver supports dev workspace and launcher package roots", "[runtime-layout]") {
    TemporaryDirectory temp("skill-roots");
    const auto workspace = temp.Path() / "workspace";
    const auto devRoot = workspace / ".agents" / "skills" / "kano" / "kano-git-master-skill";
    WriteMarker(devRoot / "SKILL.md");
    WriteMarker(devRoot / "scripts" / "kog");

    ScopedEnvironment noExplicitRoot("KANO_GIT_SKILL_ROOT", std::nullopt);
    ScopedEnvironment noLauncherRoot("KANO_GIT_MASTER_ROOT", std::nullopt);
    REQUIRE(runtime_path::ResolveSkillRoot(workspace) == devRoot.lexically_normal());

    const auto packageRoot = temp.Path() / "package" / "kano-git-master-skill";
    ScopedEnvironment launcherRoot("KANO_GIT_MASTER_ROOT", packageRoot.generic_string());
    REQUIRE(runtime_path::ResolveSkillRoot(workspace) == packageRoot.lexically_normal());
}

TEST_CASE("ignore gate policy supports exact and shell glob entries", "[ignore-policy]") {
    const std::unordered_set<std::string> policy = {
        "exact/output.log",
        "src/cpp/scripts/**",
        "tools/*/generated?.tmp",
    };

    REQUIRE(kano::git::commands::IsIgnoreGateAllowlisted(policy, "exact/output.log"));
    REQUIRE(kano::git::commands::IsIgnoreGateAllowlisted(policy, "SRC\\CPP\\SCRIPTS\\build\\tool.exe"));
    REQUIRE(kano::git::commands::IsIgnoreGateAllowlisted(policy, "tools/mac/generated1.tmp"));
    REQUIRE_FALSE(kano::git::commands::IsIgnoreGateAllowlisted(policy, "tools/mac/nested/generated1.tmp"));
    REQUIRE_FALSE(kano::git::commands::IsIgnoreGateAllowlisted(policy, "src/cpp/script/build/tool.exe"));
    REQUIRE_FALSE(kano::git::commands::IsIgnoreGateAllowlisted(policy, "exact/output.log.bak"));
}

TEST_CASE("ignore gate candidate keys are workspace-relative for nested repositories",
          "[ignore-policy][nested-repo]") {
    const auto workspace = std::filesystem::path("/workspace");
    const auto nestedRepo = workspace / "modules" / "demo";
    const auto key = kano::git::commands::WorkspaceRelativeIgnoreGatePath(
        workspace,
        nestedRepo,
        "src\\cpp\\scripts\\build\\tool.exe");

    REQUIRE(key == "modules/demo/src/cpp/scripts/build/tool.exe");

    const std::unordered_set<std::string> policy = {
        "modules/demo/src/cpp/scripts/**",
    };
    REQUIRE(kano::git::commands::IsIgnoreGateAllowlisted(policy, key));
    REQUIRE_FALSE(kano::git::commands::IsIgnoreGateAllowlisted(
        policy,
        "src/cpp/scripts/build/tool.exe"));
}
