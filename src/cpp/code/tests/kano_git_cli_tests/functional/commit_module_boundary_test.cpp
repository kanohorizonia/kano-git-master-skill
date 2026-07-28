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

TEST_CASE("commit command keeps implementation behind commit utils module", "[architecture][commit][module-boundary]") {
    const auto codeRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto moduleRoot = codeRoot / "systems/kano_git_command/commit_plan";
    const auto commitCommandPath = moduleRoot / "private/commit_cmd.cpp";
    const auto commitPlanPayloadHeaderPath = moduleRoot / "private/commit_plan_payload.hpp";
    const auto commitPlanPayloadSourcePath = moduleRoot / "private/commit_plan_payload.cpp";
    const auto commitPlanTaskGraphHeaderPath = moduleRoot / "private/commit_plan_task_graph.hpp";
    const auto commitPlanTaskGraphSourcePath = moduleRoot / "private/commit_plan_task_graph.cpp";
    const auto commitAutoPlanPipelineHeaderPath = moduleRoot / "private/commit_auto_plan_pipeline.hpp";
    const auto commitAutoPlanPipelineSourcePath = moduleRoot / "private/commit_auto_plan_pipeline.cpp";
    const auto commitLockRecoveryHeaderPath = moduleRoot / "private/commit_lock_recovery.hpp";
    const auto commitLockRecoverySourcePath = moduleRoot / "private/commit_lock_recovery.cpp";
    const auto commitUtilsHeaderPath = moduleRoot / "private/commit_utils.hpp";
    const auto commitUtilsSourcePath = moduleRoot / "private/commit_utils.cpp";
    const auto cmakePath = moduleRoot / "CMakeLists.txt";

    const auto commitCommand = ReadText(commitCommandPath);
    const auto commitPlanPayloadHeader = ReadText(commitPlanPayloadHeaderPath);
    const auto commitPlanPayloadSource = ReadText(commitPlanPayloadSourcePath);
    const auto commitPlanTaskGraphHeader = ReadText(commitPlanTaskGraphHeaderPath);
    const auto commitPlanTaskGraphSource = ReadText(commitPlanTaskGraphSourcePath);
    const auto commitAutoPlanPipelineHeader = ReadText(commitAutoPlanPipelineHeaderPath);
    const auto commitAutoPlanPipelineSource = ReadText(commitAutoPlanPipelineSourcePath);
    const auto commitLockRecoveryHeader = ReadText(commitLockRecoveryHeaderPath);
    const auto commitLockRecoverySource = ReadText(commitLockRecoverySourcePath);
    const auto commitUtilsHeader = ReadText(commitUtilsHeaderPath);
    const auto commitUtilsSource = ReadText(commitUtilsSourcePath);
    const auto cmake = ReadText(cmakePath);

    REQUIRE(CountLines(commitCommand) < 2000);
    REQUIRE(CountLines(commitUtilsSource) < 5000);
    REQUIRE(commitCommand.find("#include \"commit_utils.hpp\"") != std::string::npos);
    REQUIRE(commitCommand.find("void RegisterCommit(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitCommand.find("void RegisterAmend(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitUtilsSource.find("#include \"commit_plan_payload.hpp\"") != std::string::npos);
    REQUIRE(commitUtilsSource.find("#include \"commit_plan_task_graph.hpp\"") != std::string::npos);
    REQUIRE(commitUtilsSource.find("#include \"commit_auto_plan_pipeline.hpp\"") != std::string::npos);
    REQUIRE(commitUtilsSource.find("#include \"commit_lock_recovery.hpp\"") != std::string::npos);
    REQUIRE(commitUtilsSource.find("struct CommitPlanPayload") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto ParseCommitPlanText(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("struct RepoCommitRunbook") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto BuildCommitTaskGraph(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto DefaultSharedPlanPath(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto RunPlanNewViaSelf(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto RunCommitSeedViaSelf(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto RunCommitAutoPlanPipeline(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto RunAmendAutoPlanPipeline(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto DetectActiveCommitLockRecoveryProcess(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto CleanupStaleCommitLocksForRepo(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto RunCommitLockRecoveryConvergeProbe(") == std::string::npos);
    REQUIRE(commitUtilsSource.find("auto AttemptCommitLockRecoveryOnce(") == std::string::npos);
    REQUIRE(commitUtilsHeader.find("ConfigureCommitCommand") != std::string::npos);
    REQUIRE(commitUtilsHeader.find("ConfigureAmendCommand") != std::string::npos);
    REQUIRE(commitUtilsSource.find("void ConfigureCommitCommand(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitUtilsSource.find("void ConfigureAmendCommand(CLI::App& InApp)") != std::string::npos);
    REQUIRE(commitPlanPayloadHeader.find("struct CommitPlanPayload") != std::string::npos);
    REQUIRE(commitPlanPayloadHeader.find("ValidateCommitPlanForAiMode") != std::string::npos);
    REQUIRE(commitPlanPayloadHeader.find("ParseCommitPlanText") == std::string::npos);
    REQUIRE(commitPlanPayloadHeader.find("ExtractStringField") == std::string::npos);
    REQUIRE(commitPlanPayloadSource.find("auto ParseCommitPlanText(") != std::string::npos);
    REQUIRE(commitPlanPayloadSource.find("auto ExtractStringField(") != std::string::npos);
    REQUIRE(commitPlanTaskGraphHeader.find("struct RepoCommitRunbook") != std::string::npos);
    REQUIRE(commitPlanTaskGraphHeader.find("struct CommitTaskGraph") != std::string::npos);
    REQUIRE(commitPlanTaskGraphHeader.find("BuildStageMessageMap") != std::string::npos);
    REQUIRE(commitPlanTaskGraphHeader.find("ResolveRepoMessages") == std::string::npos);
    REQUIRE(commitPlanTaskGraphHeader.find("IsParentRepoPath") == std::string::npos);
    REQUIRE(commitPlanTaskGraphSource.find("auto ResolveRepoMessages(") != std::string::npos);
    REQUIRE(commitPlanTaskGraphSource.find("auto IsParentRepoPath(") != std::string::npos);
    REQUIRE(CountOccurrences(commitAutoPlanPipelineHeader, "\nauto ") == 5);
    REQUIRE(commitAutoPlanPipelineHeader.find("DefaultSharedPlanPath") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunPlanNewViaSelf") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunCommitSeedViaSelf") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunCommitAutoPlanPipeline") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunAmendAutoPlanPipeline") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("ResolveSelfBinaryCommand") == std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunIgnorePlanRunbookViaSelf") == std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunIgnorePlanApplyViaSelf") == std::string::npos);
    REQUIRE(commitAutoPlanPipelineHeader.find("RunCommitPlanRunbookViaSelf") == std::string::npos);
    REQUIRE(commitAutoPlanPipelineSource.find("auto ResolveSelfBinaryCommand(") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineSource.find("auto RunIgnorePlanRunbookViaSelf(") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineSource.find("auto RunIgnorePlanApplyViaSelf(") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineSource.find("auto RunCommitPlanRunbookViaSelf(") != std::string::npos);
    REQUIRE(commitAutoPlanPipelineSource.find("KOG_SCOPED_TIMING_LOG_WITH_ELAPSED") != std::string::npos);
    REQUIRE(CountLines(commitLockRecoverySource) < 650);
    REQUIRE(CountOccurrences(commitLockRecoveryHeader, "\nauto ") == 1);
    REQUIRE(commitLockRecoveryHeader.find("struct CommitLockRecoveryState") != std::string::npos);
    REQUIRE(commitLockRecoveryHeader.find("MaybeRecoverCommitLockFailure") != std::string::npos);
    REQUIRE(commitLockRecoverySource.find("auto DetectActiveCommitLockRecoveryProcess(") != std::string::npos);
    REQUIRE(commitLockRecoverySource.find("auto CleanupStaleCommitLocksForRepo(") != std::string::npos);
    REQUIRE(commitLockRecoverySource.find("auto RunCommitLockRecoveryConvergeProbe(") != std::string::npos);
    REQUIRE(commitLockRecoverySource.find("auto AttemptCommitLockRecoveryOnce(") != std::string::npos);
    REQUIRE(cmake.find("private/commit_plan_payload.cpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_plan_payload.hpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_plan_task_graph.cpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_plan_task_graph.hpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_auto_plan_pipeline.cpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_auto_plan_pipeline.hpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_lock_recovery.cpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_lock_recovery.hpp") != std::string::npos);
    REQUIRE(cmake.find("private/commit_utils.cpp") != std::string::npos);
}

} // namespace kano::git::tests::functional
