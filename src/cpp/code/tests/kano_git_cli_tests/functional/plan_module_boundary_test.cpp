#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto ReadPlanModuleFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

auto CountPlanModuleLines(const std::string& InText) -> std::size_t {
    std::size_t lines = 0;
    std::istringstream input(InText);
    std::string line;
    while (std::getline(input, line)) {
        ++lines;
    }
    return lines;
}

} // namespace

TEST_CASE("plan command keeps callbacks behind focused modules", "[architecture][plan][module-boundary]") {
    const auto codeRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto moduleRoot = codeRoot / "systems/kano_git_command/commit_plan";
    const auto planCommand = ReadPlanModuleFile(moduleRoot / "private/plan_cmd.cpp");
    const auto ignoreCommand = ReadPlanModuleFile(moduleRoot / "private/ignore_cmd.cpp");
    const auto planUtilsHeader = ReadPlanModuleFile(moduleRoot / "private/plan_utils.hpp");
    const auto planUtilsSource = ReadPlanModuleFile(moduleRoot / "private/plan_utils.cpp");
    const auto cmake = ReadPlanModuleFile(moduleRoot / "CMakeLists.txt");

    REQUIRE(CountPlanModuleLines(planCommand) < 1000);
    REQUIRE(planCommand.find("#include \"plan_utils.hpp\"") != std::string::npos);
    REQUIRE(planCommand.find("RunFullRunbook") != std::string::npos);
    REQUIRE(planCommand.find("void RegisterIgnore") == std::string::npos);
    REQUIRE(ignoreCommand.find("void RegisterIgnore(CLI::App& InApp)") != std::string::npos);
    REQUIRE(ignoreCommand.find("RunIgnoreDoctor") != std::string::npos);
    REQUIRE(planUtilsHeader.find("RunFullRunbook") != std::string::npos);
    REQUIRE(planUtilsSource.find("RunFullRunbook") != std::string::npos);
    REQUIRE(cmake.find("private/ignore_cmd.cpp") != std::string::npos);
}

} // namespace kano::git::tests::functional
