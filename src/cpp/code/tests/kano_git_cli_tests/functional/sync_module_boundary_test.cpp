#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto ReadFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

auto CountOccurrences(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = InText.find(InNeedle, offset)) != std::string::npos) {
        ++count;
        offset += InNeedle.size();
    }
    return count;
}

} // namespace

TEST_CASE("sync registration stays behind a typed CLI boundary",
          "[architecture][sync][module-boundary][KG-TSK-0117]") {
    const auto cppRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto moduleRoot =
        cppRoot / "code" / "systems" / "kano_git_command" / "repo_sync";
    const auto commandSource = ReadFile(moduleRoot / "private" / "sync_cmd.cpp");
    const auto cliSource = ReadFile(moduleRoot / "private" / "sync_cli.cpp");
    const auto commandHeader = ReadFile(moduleRoot / "private" / "sync_cmd.hpp");
    const auto moduleCMake = ReadFile(moduleRoot / "CMakeLists.txt");

    const auto runtimeBoundary = commandSource.find("auto MakeSyncCommandOptions()");
    REQUIRE(runtimeBoundary != std::string::npos);
    const auto syncRuntime = commandSource.substr(runtimeBoundary);

    REQUIRE(commandSource.find("void RegisterSync(") == std::string::npos);
    REQUIRE(syncRuntime.find("add_subcommand(") == std::string::npos);
    REQUIRE(syncRuntime.find("new std::string") == std::string::npos);
    REQUIRE(syncRuntime.find("new bool") == std::string::npos);
    REQUIRE(syncRuntime.find("new int") == std::string::npos);
    REQUIRE(cliSource.find("add_subcommand(\n        \"sync\"") != std::string::npos);
    REQUIRE(cliSource.find("const auto options = MakeSyncCommandOptions()") != std::string::npos);
    REQUIRE(CountOccurrences(cliSource, "->callback(MakeSync") == 6);
    REQUIRE(cliSource.find("MakeDefaultSyncCommandCallback(*cmd, options)") != std::string::npos);
    REQUIRE(commandHeader.find("struct SyncCommandOptions") != std::string::npos);
    REQUIRE(CountOccurrences(syncRuntime, "optionsOwner = InOptions") == 7);
    REQUIRE(moduleCMake.find("private/sync_cli.cpp") != std::string::npos);
}

} // namespace kano::git::tests::functional
