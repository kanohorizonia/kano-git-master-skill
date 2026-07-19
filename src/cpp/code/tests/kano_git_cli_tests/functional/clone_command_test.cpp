#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "KOG Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kog-test@example.invalid"}, InRepo), "config user.email");
}

auto WriteFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto InitRepo(const std::filesystem::path& InRepo, const std::string& InFilename) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init repo");
    ConfigureIdentity(InRepo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, InRepo), "checkout main");
    WriteFile(InRepo / InFilename, "fixture\n");
    RequireSuccess(RunGit({"add", InFilename}, InRepo), "add fixture");
    RequireSuccess(RunGit({"commit", "-m", "fixture"}, InRepo), "commit fixture");
}

} // namespace

TEST_CASE("clone initializes recursive submodules by default", "[functional][clone][submodule]") {
    const auto sandbox = CreateSandboxWorkspace("clone-recursive-submodules");
    const auto child = sandbox.root / "child";
    const auto parent = sandbox.root / "parent";
    const auto destination = sandbox.root / "destination";

    InitRepo(child, "child.txt");
    InitRepo(parent, "parent.txt");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", child.string(), "deps/child"}, parent),
        "add child submodule");
    RequireSuccess(RunGit({"commit", "-am", "add child"}, parent), "commit child submodule");

    const auto clone = RunKogWithEnv(
        {"clone", parent.string(), "--dir", destination.filename().string()},
        sandbox.root,
        {{"GIT_ALLOW_PROTOCOL", "file"}});
    RequireSuccess(clone, "kog clone with recursive submodules");
    REQUIRE(std::filesystem::exists(destination / "deps" / "child" / "child.txt"));
}

TEST_CASE("clone no-submodules preserves an uninitialized submodule", "[functional][clone][submodule]") {
    const auto sandbox = CreateSandboxWorkspace("clone-no-submodules");
    const auto child = sandbox.root / "child";
    const auto parent = sandbox.root / "parent";
    const auto destination = sandbox.root / "destination";

    InitRepo(child, "child.txt");
    InitRepo(parent, "parent.txt");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", child.string(), "deps/child"}, parent),
        "add child submodule");
    RequireSuccess(RunGit({"commit", "-am", "add child"}, parent), "commit child submodule");

    const auto clone = RunKog(
        {"clone", parent.string(), "--dir", destination.filename().string(), "--no-submodules"},
        sandbox.root);
    RequireSuccess(clone, "kog clone without submodules");
    REQUIRE_FALSE(std::filesystem::exists(destination / "deps" / "child" / "child.txt"));
}

} // namespace kano::git::tests::functional
