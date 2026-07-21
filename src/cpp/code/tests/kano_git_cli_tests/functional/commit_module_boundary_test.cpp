#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto ReadText(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

auto CountLines(const std::string& InText) -> std::size_t {
    std::size_t lines = 0;
    std::istringstream input(InText);
    std::string line;
    while (std::getline(input, line)) {
        ++lines;
    }
    return lines;
}

} // namespace

TEST_CASE("commit command keeps implementation behind commit utils module", "[architecture][commit][module-boundary]") {
    const auto codeRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto moduleRoot = codeRoot / "systems/kano_git_command/commit_plan";
    const auto commitCommandPath = moduleRoot / "private/commit_cmd.cpp";
    const auto commitUtilsHeaderPath = moduleRoot / "private/commit_utils.hpp";
    const auto commitUtilsSourcePath = moduleRoot / "private/commit_utils.cpp";
    const auto cmakePath = moduleRoot / "CMakeLists.txt";

    const auto commitCommand = ReadText(commitCommandPath);
    const auto commitUtilsHeader = ReadText(commitUtilsHeaderPath);
    const auto commitUtilsSource = ReadText(commitUtilsSourcePath);
    const auto cmake = ReadText(cmakePath);

    REQUIRE(CountLines(commitCommand) < 2000);
    REQUIRE(commitCommand.find("#include \"commit_utils.hpp\"") != std::string::npos);
    REQUIRE(commitCommand.find("void RegisterCommit(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitCommand.find("void RegisterAmend(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitUtilsHeader.find("ConfigureCommitCommand") != std::string::npos);
    REQUIRE(commitUtilsHeader.find("ConfigureAmendCommand") != std::string::npos);
    REQUIRE(commitUtilsSource.find("void ConfigureCommitCommand(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitUtilsSource.find("void ConfigureAmendCommand(CLI::App& InApp)") != std::string::npos);
    REQUIRE(cmake.find("private/commit_utils.cpp") != std::string::npos);
}

} // namespace kano::git::tests::functional
