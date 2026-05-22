#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto StripAnsi(const std::string& InText) -> std::string {
    std::string out;
    out.reserve(InText.size());
    bool inEsc = false;
    for (const char ch : InText) {
        if (!inEsc) {
            if (ch == '\x1b') {
                inEsc = true;
                continue;
            }
            out.push_back(ch);
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            inEsc = false;
        }
    }
    return out;
}

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

auto InitRepo(const std::filesystem::path& InRepo) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init repo");
    ConfigureIdentity(InRepo);
    WriteTextFile(InRepo / "README.md", "repo\n");
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add repo readme");
    RequireSuccess(RunGit({"commit", "-m", "seed repo"}, InRepo), "commit repo readme");
}

auto RunDiscoverJson(const std::filesystem::path& InRoot, const std::vector<std::string>& InExtraArgs) -> std::string {
    std::vector<std::string> args{"discover", "--format", "json", "--repo-root", InRoot.string(), "--no-cache"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    const auto result = RunKog(args, InRoot);
    RequireSuccess(result, "kog discover json");
    return StripAnsi(result.stdoutText + "\n" + result.stderrText);
}

auto JsonPathCount(const std::string& InJson, const std::string& InPathFragment) -> int {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = InJson.find(InPathFragment, pos)) != std::string::npos) {
        count += 1;
        pos += InPathFragment.size();
    }
    return count;
}

auto AddGitmodulesEntry(const std::filesystem::path& InRepo,
                        const std::string& InName,
                        const std::string& InPath,
                        const std::string& InExtraPolicy = {}) -> void {
    std::ofstream out(InRepo / ".gitmodules", std::ios::binary | std::ios::app);
    REQUIRE(out.good());
    out << "[submodule \"" << InName << "\"]\n";
    out << "\tpath = " << InPath << "\n";
    out << "\turl = ./" << InPath << "\n";
    out << InExtraPolicy;
}

auto CommitGitmodules(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"add", ".gitmodules"}, InRepo), "add .gitmodules");
    RequireSuccess(RunGit({"commit", "-m", "configure submodules"}, InRepo), "commit .gitmodules");
}

} // namespace

TEST_CASE("discover keeps registered recursion separate from bounded unregistered probing", "[functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-registered-recursion");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "registered");
    InitRepo(root / "registered" / "nested");
    InitRepo(root / "loose");
    InitRepo(root / "deep" / "too-far");

    AddGitmodulesEntry(root, "registered", "registered", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = false\n\tkog-hygiene = true\n");
    AddGitmodulesEntry(root / "registered", "nested", "nested", "\tkog-sync = true\n");
    CommitGitmodules(root / "registered");
    CommitGitmodules(root);

    const auto registeredOnly = RunDiscoverJson(root, {"--unregistered-depth", "0"});
    INFO(registeredOnly);
    REQUIRE(registeredOnly.find("registered") != std::string::npos);
    REQUIRE(registeredOnly.find("nested") != std::string::npos);
    REQUIRE(registeredOnly.find("\"kogPush\":\"false\"") != std::string::npos);
    REQUIRE(registeredOnly.find("loose") == std::string::npos);

    const auto bounded = RunDiscoverJson(root, {"--full", "--unregistered-depth", "2"});
    INFO(bounded);
    REQUIRE(bounded.find("loose") != std::string::npos);
    REQUIRE(bounded.find("too-far") != std::string::npos);
    REQUIRE(JsonPathCount(bounded, "registered") >= 1);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover no-unregistered-scan keeps trusted unregistered manifest entries", "[functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-no-unregistered-scan");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "registered");
    InitRepo(root / "loose");
    AddGitmodulesEntry(root, "registered", "registered", "\tkog-sync = true\n");
    CommitGitmodules(root);

    const auto full = RunDiscoverJson(root, {"--full", "--unregistered-depth", "2"});
    INFO(full);
    REQUIRE(full.find("loose") != std::string::npos);

    InitRepo(root / "new-loose");

    const auto noScan = RunDiscoverJson(root, {"--no-unregistered-scan"});
    INFO(noScan);
    REQUIRE(noScan.find("registered") != std::string::npos);
    REQUIRE(noScan.find("loose") != std::string::npos);
    REQUIRE(noScan.find("new-loose") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover treats trusted unregistered repo as registered discovery root", "[functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-unregistered-root");
    const auto root = (sandbox.root / "root").lexically_normal();
    const auto loose = root / "loose";
    InitRepo(root);
    InitRepo(loose);
    InitRepo(loose / "child");
    AddGitmodulesEntry(loose, "child", "child", "\tkog-sync = true\n");
    CommitGitmodules(loose);

    const auto full = RunDiscoverJson(root, {"--full", "--unregistered-depth", "2"});
    INFO(full);
    REQUIRE(full.find("loose") != std::string::npos);

    const auto trusted = RunDiscoverJson(root, {});
    INFO(trusted);
    REQUIRE(trusted.find("loose") != std::string::npos);
    REQUIRE(trusted.find("child") != std::string::npos);
    REQUIRE(JsonPathCount(trusted, "child") == 1);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover full scan prunes ignored build cache and temp directories", "[functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-ignored-dirs");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / ".cache" / "cached-repo");
    InitRepo(root / "build" / "build-repo");
    InitRepo(root / "tmp" / "tmp-repo");
    InitRepo(root / "visible-repo");

    const auto json = RunDiscoverJson(root, {"--full", "--unregistered-depth", "3"});
    INFO(json);
    REQUIRE(json.find("visible-repo") != std::string::npos);
    REQUIRE(json.find("cached-repo") == std::string::npos);
    REQUIRE(json.find("build-repo") == std::string::npos);
    REQUIRE(json.find("tmp-repo") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
