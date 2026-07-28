#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

auto EnvironmentValue(const char* InName) -> std::string {
    const char* value = std::getenv(InName);
    return value == nullptr ? std::string{} : std::string(value);
}

auto ParseExitCode(const std::string& InValue) -> int {
    if (InValue.empty()) {
        return 0;
    }
    try {
        return std::stoi(InValue);
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

auto main(int argc, char** argv) -> int {
    std::string joined;
    for (int index = 1; index < argc; ++index) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += argv[index] == nullptr ? "" : argv[index];
    }

    if (const auto logPath = EnvironmentValue("KOG_TEST_FAKE_SELF_LOG"); !logPath.empty()) {
        std::ofstream log(logPath, std::ios::binary | std::ios::app);
        log << "cwd=" << std::filesystem::current_path().generic_string();
        for (int index = 1; index < argc; ++index) {
            log << '\t' << (argv[index] == nullptr ? "" : argv[index]);
        }
        log << '\n';
    }

    const auto stdoutText = EnvironmentValue("KOG_TEST_FAKE_SELF_STDOUT");
    if (!stdoutText.empty()) {
        std::cout << stdoutText;
    }
    const auto stderrText = EnvironmentValue("KOG_TEST_FAKE_SELF_STDERR");
    if (!stderrText.empty()) {
        std::cerr << stderrText;
    }

    const auto failContains = EnvironmentValue("KOG_TEST_FAKE_SELF_FAIL_CONTAINS");
    if (!failContains.empty() && joined.find(failContains) != std::string::npos) {
        return ParseExitCode(EnvironmentValue("KOG_TEST_FAKE_SELF_EXIT_CODE"));
    }
    return 0;
}
