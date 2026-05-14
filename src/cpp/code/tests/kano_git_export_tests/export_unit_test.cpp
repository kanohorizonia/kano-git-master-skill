// export_unit_test.cpp — Unit tests for ExportOneRepo continue-on-failure behavior
//                        and revision fallback / SHA-256 tool selection
//
// Tests that:
//   1. ExportOneRepo returns ExportResult{success=false} when the archive
//      command fails (Requirement 8.3)
//   2. RunExportWithExecutor continues to the next repo after a failure
//      (Requirement 8.3)
//   3. RunExportWithExecutor returns non-zero exit code when at least one
//      repo failed (Requirement 8.4)
//   4. ComputeRevision (via ExportOneRepo) falls back to "000" when git
//      returns non-zero exit (Requirement 2.4)
//   5. WriteChecksumFile (via ExportOneRepo) returns false gracefully when
//      no sha tool is available — export still succeeds (Requirement 5.5)
//
// **Validates: Requirements 8.3, 8.4, 2.4, 5.5**

#include <catch2/catch_test_macros.hpp>

#include "export_helpers.hpp"
#include "shell_executor.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace kano::git::commands;
using namespace kano::git::shell;

// ---------------------------------------------------------------------------
// Stub shell executor helpers
// ---------------------------------------------------------------------------

namespace {

// A stub ShellExecutor that always succeeds.
// Returns exitCode=0 with a plausible stdout for each command type.
auto MakeSuccessExecutor() -> ShellExecutor {
    return [](const std::string& InCommand,
              const std::vector<std::string>& InArgs,
              ExecMode /*InMode*/,
              std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;

        // Provide plausible output for git rev-list
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            result.stdoutStr = "42\n";
        }
        // Provide plausible output for git archive
        else if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            result.stdoutStr = "";
        }
        // Provide plausible output for git status --short
        else if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "status") {
            result.stdoutStr = "";
        }
        // Provide plausible output for git ls-files
        else if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "ls-files") {
            result.stdoutStr = "README.md\nsrc/main.cpp\n";
        }
        // sha256sum / shasum / powershell — return failure so no checksum file
        // is written (avoids filesystem side-effects in unit tests)
        else if (InCommand == "sha256sum" || InCommand == "shasum" ||
                 InCommand == "powershell") {
            result.exitCode = 1;
        }

        return result;
    };
}

// A stub ShellExecutor that fails the git archive command for a specific repo
// path, and succeeds for all other commands.
auto MakeFailArchiveForRepoExecutor(const std::filesystem::path& InFailRepoPath) -> ShellExecutor {
    return [InFailRepoPath](const std::string& InCommand,
                            const std::vector<std::string>& InArgs,
                            ExecMode /*InMode*/,
                            std::optional<std::filesystem::path> InWorkingDir) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;

        // Fail git archive when the working directory matches the target repo
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            if (InWorkingDir.has_value() &&
                InWorkingDir->lexically_normal() == InFailRepoPath.lexically_normal()) {
                result.exitCode = 128;
                result.stderrStr = "fatal: not a git repository";
                return result;
            }
            // Succeed for other repos
            result.stdoutStr = "";
            return result;
        }

        // git rev-list — succeed for all repos
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            result.stdoutStr = "10\n";
            return result;
        }

        // git status / ls-files — succeed
        if (InCommand == "git") {
            result.stdoutStr = "";
            return result;
        }

        // sha256sum / shasum / powershell — fail (no checksum side-effects)
        if (InCommand == "sha256sum" || InCommand == "shasum" ||
            InCommand == "powershell") {
            result.exitCode = 1;
        }

        return result;
    };
}

// Build a simple ExportRecord for a named repo under a given root.
auto MakeRecord(const std::filesystem::path& InRepoPath,
                const std::string& InRepoName,
                bool InIsRoot = false) -> ExportRecord {
    ExportRecord rec;
    rec.repoPath = InRepoPath;
    rec.repoName = InRepoName;
    rec.isRoot = InIsRoot;
    return rec;
}

// Build a minimal ExportOptions suitable for unit tests.
// noMetadata=true avoids filesystem writes for manifest/checksum files.
auto MakeTestOpts(bool InNoMetadata = true) -> ExportOptions {
    ExportOptions opts;
    opts.format = "tar";
    opts.source = "head";
    opts.revPad = 3;
    opts.noMetadata = InNoMetadata;
    return opts;
}

// Temporary directory RAII helper — creates a unique temp dir and removes it
// on destruction.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("kog_export_unit_test_" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // anonymous namespace

// ===========================================================================
// ExportOneRepo — archive failure returns success=false
// ===========================================================================

TEST_CASE("ExportOneRepo returns success=false when archive command fails",
          "[Unit][ExportOneRepo][continue-on-failure][req-8.3]") {
    // **Validates: Requirements 8.3**
    //
    // When the archive command returns a non-zero exit code, ExportOneRepo
    // shall return ExportResult{success=false} with a non-empty errorMessage.

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "MyRepo";
    const auto record = MakeRecord(repoPath, "MyRepo", true);
    const auto opts = MakeTestOpts();

    // Executor that fails git archive for this repo
    const auto exec = MakeFailArchiveForRepoExecutor(repoPath);

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result.errorMessage.empty());
    // Archive name should still be computed (it's set before the archive call)
    REQUIRE_FALSE(result.archiveName.empty());
}

TEST_CASE("ExportOneRepo returns success=true when archive command succeeds",
          "[Unit][ExportOneRepo][continue-on-failure][req-8.3]") {
    // **Validates: Requirements 8.3**
    //
    // When the archive command returns exit code 0, ExportOneRepo shall return
    // ExportResult{success=true}.

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "MyRepo";
    const auto record = MakeRecord(repoPath, "MyRepo", true);
    const auto opts = MakeTestOpts();

    const auto exec = MakeSuccessExecutor();

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    REQUIRE(result.success);
    REQUIRE(result.errorMessage.empty());
    REQUIRE_FALSE(result.archiveName.empty());
}

TEST_CASE("ExportOneRepo archive name follows <name>_rev<NNN>.<format> pattern",
          "[Unit][ExportOneRepo][archive-naming][req-8.3]") {
    // **Validates: Requirements 8.3**
    //
    // Even on failure, the archive name should be computed correctly.

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "KTOStudio";
    const auto record = MakeRecord(repoPath, "KTOStudio", true);
    const auto opts = MakeTestOpts();

    const auto exec = MakeSuccessExecutor();

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    REQUIRE(result.success);
    // Archive name should be "KTOStudio_rev042.tar" (rev-list returns "42")
    REQUIRE(result.archiveName == "KTOStudio_rev042.tar");
}

// ===========================================================================
// RunExportWithExecutor — continue-on-failure behavior
// ===========================================================================

TEST_CASE("RunExportWithExecutor continues to next repo after one failure",
          "[Unit][RunExportWithExecutor][continue-on-failure][req-8.3]") {
    // **Validates: Requirements 8.3**
    //
    // When one repo's archive fails, RunExportWithExecutor shall continue
    // exporting the remaining repos.  We verify this by checking that the
    // successful repos produce ExportResult{success=true} entries in the
    // output, even though one repo failed.

    TempDir outputDir;
    TempDir metadataDir;

    const auto rootPath  = std::filesystem::path("workspace") / "Root";
    const auto sub1Path  = std::filesystem::path("workspace") / "Sub1";
    const auto sub2Path  = std::filesystem::path("workspace") / "Sub2";

    // Sub1 will fail; Root and Sub2 will succeed
    const std::vector<ExportRecord> exportList = {
        MakeRecord(rootPath, "Root", true),
        MakeRecord(sub1Path, "Sub1", false),
        MakeRecord(sub2Path, "Sub2", false),
    };

    const auto opts = MakeTestOpts();

    // Executor that fails archive only for Sub1
    const auto exec = MakeFailArchiveForRepoExecutor(sub1Path);

    // Capture stderr to suppress error output during test
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    const int exitCode = RunExportWithExecutor(opts, exportList, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    // Exit code must be non-zero (at least one failure)
    REQUIRE(exitCode != 0);

    // The error output should mention Sub1
    const std::string errOutput = capturedErr.str();
    REQUIRE(errOutput.find("Sub1") != std::string::npos);

    // The stdout should mention Root and Sub2 (they succeeded)
    const std::string stdOutput = capturedOut.str();
    REQUIRE(stdOutput.find("Root") != std::string::npos);
    REQUIRE(stdOutput.find("Sub2") != std::string::npos);
}

TEST_CASE("RunExportWithExecutor returns non-zero exit code when at least one repo fails",
          "[Unit][RunExportWithExecutor][exit-code][req-8.4]") {
    // **Validates: Requirements 8.4**
    //
    // When all exports complete and at least one repo export failed,
    // RunExportWithExecutor shall return a non-zero exit code.

    TempDir outputDir;
    TempDir metadataDir;

    const auto rootPath = std::filesystem::path("workspace") / "Root";
    const auto sub1Path = std::filesystem::path("workspace") / "Sub1";

    const std::vector<ExportRecord> exportList = {
        MakeRecord(rootPath, "Root", true),
        MakeRecord(sub1Path, "Sub1", false),
    };

    const auto opts = MakeTestOpts();

    // Executor that fails archive for Sub1
    const auto exec = MakeFailArchiveForRepoExecutor(sub1Path);

    // Suppress output
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    const int exitCode = RunExportWithExecutor(opts, exportList, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    REQUIRE(exitCode != 0);
}

TEST_CASE("RunExportWithExecutor returns zero exit code when all repos succeed",
          "[Unit][RunExportWithExecutor][exit-code][req-8.4]") {
    // **Validates: Requirements 8.4**
    //
    // When all exports complete successfully, RunExportWithExecutor shall
    // return exit code 0.

    TempDir outputDir;
    TempDir metadataDir;

    const auto rootPath = std::filesystem::path("workspace") / "Root";
    const auto sub1Path = std::filesystem::path("workspace") / "Sub1";

    const std::vector<ExportRecord> exportList = {
        MakeRecord(rootPath, "Root", true),
        MakeRecord(sub1Path, "Sub1", false),
    };

    const auto opts = MakeTestOpts();
    const auto exec = MakeSuccessExecutor();

    // Suppress output
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    const int exitCode = RunExportWithExecutor(opts, exportList, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    REQUIRE(exitCode == 0);
}

TEST_CASE("RunExportWithExecutor processes all repos even when first repo fails",
          "[Unit][RunExportWithExecutor][continue-on-failure][req-8.3]") {
    // **Validates: Requirements 8.3**
    //
    // Even when the first repo in the list fails, all subsequent repos must
    // still be processed.

    TempDir outputDir;
    TempDir metadataDir;

    const auto rootPath = std::filesystem::path("workspace") / "Root";
    const auto sub1Path = std::filesystem::path("workspace") / "Sub1";
    const auto sub2Path = std::filesystem::path("workspace") / "Sub2";
    const auto sub3Path = std::filesystem::path("workspace") / "Sub3";

    const std::vector<ExportRecord> exportList = {
        MakeRecord(rootPath, "Root", true),   // fails
        MakeRecord(sub1Path, "Sub1", false),  // succeeds
        MakeRecord(sub2Path, "Sub2", false),  // succeeds
        MakeRecord(sub3Path, "Sub3", false),  // succeeds
    };

    const auto opts = MakeTestOpts();

    // Executor that fails archive only for Root
    const auto exec = MakeFailArchiveForRepoExecutor(rootPath);

    // Suppress output
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    const int exitCode = RunExportWithExecutor(opts, exportList, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    // Non-zero because Root failed
    REQUIRE(exitCode != 0);

    // Sub1, Sub2, Sub3 should appear in stdout (they succeeded)
    const std::string stdOutput = capturedOut.str();
    REQUIRE(stdOutput.find("Sub1") != std::string::npos);
    REQUIRE(stdOutput.find("Sub2") != std::string::npos);
    REQUIRE(stdOutput.find("Sub3") != std::string::npos);

    // Root should appear in stderr (it failed)
    const std::string errOutput = capturedErr.str();
    REQUIRE(errOutput.find("Root") != std::string::npos);
}

TEST_CASE("RunExportWithExecutor returns non-zero when all repos fail",
          "[Unit][RunExportWithExecutor][exit-code][req-8.4]") {
    // **Validates: Requirements 8.4**
    //
    // When every repo fails, the exit code must still be non-zero.

    TempDir outputDir;
    TempDir metadataDir;

    const auto rootPath = std::filesystem::path("workspace") / "Root";
    const auto sub1Path = std::filesystem::path("workspace") / "Sub1";

    const std::vector<ExportRecord> exportList = {
        MakeRecord(rootPath, "Root", true),
        MakeRecord(sub1Path, "Sub1", false),
    };

    const auto opts = MakeTestOpts();

    // Executor that always fails git archive
    ShellExecutor alwaysFailArchive = [](const std::string& InCommand,
                                         const std::vector<std::string>& InArgs,
                                         ExecMode /*InMode*/,
                                         std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            result.exitCode = 128;
            result.stderrStr = "fatal: not a git repository";
        } else if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            result.exitCode = 0;
            result.stdoutStr = "5\n";
        } else {
            result.exitCode = 1;
        }
        return result;
    };

    // Suppress output
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    const int exitCode = RunExportWithExecutor(opts, exportList, outputDir.path, metadataDir.path, alwaysFailArchive);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    REQUIRE(exitCode != 0);
}

// ===========================================================================
// ComputeRevision — fallback to "000" when git returns non-zero exit
// (Requirement 2.4)
// ===========================================================================

namespace {

// A stub executor where git rev-list returns non-zero exit (simulates a repo
// with no commits / HEAD does not exist).
auto MakeRevListFailExecutor() -> ShellExecutor {
    return [](const std::string& InCommand,
              const std::vector<std::string>& InArgs,
              ExecMode /*InMode*/,
              std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;

        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            // Simulate git failure (e.g. no commits / no HEAD)
            result.exitCode = 128;
            result.stderrStr = "fatal: your current branch 'main' does not have any commits yet";
            result.stdoutStr = "";
            return result;
        }
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            result.stdoutStr = "";
            return result;
        }
        if (InCommand == "git") {
            result.stdoutStr = "";
            return result;
        }
        // sha tools — fail to avoid filesystem side-effects
        result.exitCode = 1;
        return result;
    };
}

// A stub executor where git rev-list returns empty stdout (another failure mode).
auto MakeRevListEmptyOutputExecutor() -> ShellExecutor {
    return [](const std::string& InCommand,
              const std::vector<std::string>& InArgs,
              ExecMode /*InMode*/,
              std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;

        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            // Exit 0 but empty stdout — also triggers fallback
            result.exitCode = 0;
            result.stdoutStr = "";
            return result;
        }
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            result.stdoutStr = "";
            return result;
        }
        if (InCommand == "git") {
            result.stdoutStr = "";
            return result;
        }
        result.exitCode = 1;
        return result;
    };
}

// A stub executor where git rev-list returns non-numeric output (parse error).
auto MakeRevListGarbageOutputExecutor() -> ShellExecutor {
    return [](const std::string& InCommand,
              const std::vector<std::string>& InArgs,
              ExecMode /*InMode*/,
              std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;

        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            result.exitCode = 0;
            result.stdoutStr = "not-a-number\n";
            return result;
        }
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            result.stdoutStr = "";
            return result;
        }
        if (InCommand == "git") {
            result.stdoutStr = "";
            return result;
        }
        result.exitCode = 1;
        return result;
    };
}

} // anonymous namespace

TEST_CASE("ComputeRevision falls back to '000' when git rev-list returns non-zero exit",
          "[Unit][ComputeRevision][revision-fallback][req-2.4]") {
    // **Validates: Requirements 2.4**
    //
    // When git rev-list --count --first-parent HEAD returns a non-zero exit
    // code (e.g. repo has no commits), ComputeRevision shall use revision 0
    // zero-padded to revPad digits (default 3 → "000").
    // We verify this indirectly via ExportOneRepo: the archive name must
    // contain "_rev000." when the default revPad=3 is used.

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "MyRepo";
    const auto record = MakeRecord(repoPath, "MyRepo", true);
    const auto opts = MakeTestOpts(/*noMetadata=*/true);

    const auto exec = MakeRevListFailExecutor();

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    // Archive should succeed (git archive stub returns 0)
    REQUIRE(result.success);
    // Archive name must use the fallback revision "000"
    REQUIRE(result.archiveName == "MyRepo_rev000.tar");
}

TEST_CASE("ComputeRevision falls back to '000' when git rev-list returns empty stdout",
          "[Unit][ComputeRevision][revision-fallback][req-2.4]") {
    // **Validates: Requirements 2.4**
    //
    // When git rev-list returns exit 0 but empty stdout, ComputeRevision
    // shall also fall back to revision 0 zero-padded to revPad digits.

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "EmptyRepo";
    const auto record = MakeRecord(repoPath, "EmptyRepo", true);
    const auto opts = MakeTestOpts(/*noMetadata=*/true);

    const auto exec = MakeRevListEmptyOutputExecutor();

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    REQUIRE(result.success);
    REQUIRE(result.archiveName == "EmptyRepo_rev000.tar");
}

TEST_CASE("ComputeRevision falls back to '000' when git rev-list returns non-numeric output",
          "[Unit][ComputeRevision][revision-fallback][req-2.4]") {
    // **Validates: Requirements 2.4**
    //
    // When git rev-list returns output that cannot be parsed as an integer,
    // ComputeRevision shall fall back to revision 0 zero-padded to revPad digits.

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "GarbageRepo";
    const auto record = MakeRecord(repoPath, "GarbageRepo", true);
    const auto opts = MakeTestOpts(/*noMetadata=*/true);

    const auto exec = MakeRevListGarbageOutputExecutor();

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    REQUIRE(result.success);
    REQUIRE(result.archiveName == "GarbageRepo_rev000.tar");
}

TEST_CASE("ComputeRevision fallback respects custom revPad",
          "[Unit][ComputeRevision][revision-fallback][req-2.4]") {
    // **Validates: Requirements 2.4, 2.5**
    //
    // When git rev-list fails and revPad is set to a custom value (e.g. 5),
    // the fallback revision must be zero-padded to that width: "00000".

    TempDir outputDir;
    TempDir metadataDir;

    const auto repoPath = std::filesystem::path("workspace") / "MyRepo";
    const auto record = MakeRecord(repoPath, "MyRepo", true);

    ExportOptions opts = MakeTestOpts(/*noMetadata=*/true);
    opts.revPad = 5;

    const auto exec = MakeRevListFailExecutor();

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    REQUIRE(result.success);
    // With revPad=5, fallback revision should be "00000"
    REQUIRE(result.archiveName == "MyRepo_rev00000.tar");
}

// ===========================================================================
// WriteChecksumFile — graceful false return when no sha tool is available
// (Requirement 5.5)
// ===========================================================================

namespace {

// A stub executor where all sha tools fail but git commands succeed.
// This simulates an environment where no sha256 tool is installed.
auto MakeNoShaToolExecutor() -> ShellExecutor {
    return [](const std::string& InCommand,
              const std::vector<std::string>& InArgs,
              ExecMode /*InMode*/,
              std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;

        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "rev-list") {
            result.stdoutStr = "7\n";
            return result;
        }
        if (InCommand == "git" && !InArgs.empty() && InArgs[0] == "archive") {
            result.stdoutStr = "";
            return result;
        }
        if (InCommand == "git") {
            result.stdoutStr = "";
            return result;
        }
        // All sha tools fail — no sha256sum, no shasum, no powershell
        if (InCommand == "sha256sum" || InCommand == "shasum" ||
            InCommand == "powershell") {
            result.exitCode = 1;
            result.stderrStr = "command not found";
            return result;
        }

        result.exitCode = 1;
        return result;
    };
}

} // anonymous namespace

TEST_CASE("WriteChecksumFile failure is non-fatal: ExportOneRepo still returns success=true",
          "[Unit][WriteChecksumFile][sha256-fallback][req-5.5]") {
    // **Validates: Requirements 5.5**
    //
    // When no sha256 tool is available (sha256sum, shasum, and PowerShell all
    // return non-zero), WriteChecksumFile returns false but this is non-fatal.
    // ExportOneRepo must still return ExportResult{success=true}.

    TempDir outputDir;
    TempDir metadataDir;
    // Create the metadata dir so manifest write doesn't fail on missing dir
    std::filesystem::create_directories(metadataDir.path);

    const auto repoPath = std::filesystem::path("workspace") / "MyRepo";
    const auto record = MakeRecord(repoPath, "MyRepo", true);

    // noMetadata=false so WriteChecksumFile is actually called
    ExportOptions opts = MakeTestOpts(/*noMetadata=*/false);

    const auto exec = MakeNoShaToolExecutor();

    // Suppress the warning printed to stderr by WriteChecksumFile
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());

    const ExportResult result = ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);

    // Export must succeed even though checksum writing failed
    REQUIRE(result.success);
    REQUIRE(result.errorMessage.empty());
    REQUIRE_FALSE(result.archiveName.empty());
}

TEST_CASE("WriteChecksumFile failure emits a warning to stderr",
          "[Unit][WriteChecksumFile][sha256-fallback][req-5.5]") {
    // **Validates: Requirements 5.5**
    //
    // When no sha256 tool is available, WriteChecksumFile should print a
    // warning to stderr (non-fatal). We verify the warning is emitted.

    TempDir outputDir;
    TempDir metadataDir;
    std::filesystem::create_directories(metadataDir.path);

    const auto repoPath = std::filesystem::path("workspace") / "MyRepo";
    const auto record = MakeRecord(repoPath, "MyRepo", true);

    ExportOptions opts = MakeTestOpts(/*noMetadata=*/false);

    const auto exec = MakeNoShaToolExecutor();

    // Capture stderr to check for warning
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    ExportOneRepo(record, opts, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    // A warning about missing sha256 tool should appear in stderr
    const std::string errOutput = capturedErr.str();
    REQUIRE(errOutput.find("sha256") != std::string::npos);
}

TEST_CASE("WriteChecksumFile failure does not affect RunExportWithExecutor exit code",
          "[Unit][WriteChecksumFile][sha256-fallback][req-5.5]") {
    // **Validates: Requirements 5.5**
    //
    // When no sha256 tool is available, the overall export should still
    // succeed (exit code 0) because checksum failure is non-fatal.

    TempDir outputDir;
    TempDir metadataDir;
    std::filesystem::create_directories(metadataDir.path);

    const auto rootPath = std::filesystem::path("workspace") / "Root";
    const auto sub1Path = std::filesystem::path("workspace") / "Sub1";

    const std::vector<ExportRecord> exportList = {
        MakeRecord(rootPath, "Root", true),
        MakeRecord(sub1Path, "Sub1", false),
    };

    // noMetadata=false so WriteChecksumFile is called for each repo
    ExportOptions opts = MakeTestOpts(/*noMetadata=*/false);

    const auto exec = MakeNoShaToolExecutor();

    // Suppress output
    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    std::ostringstream capturedOut;
    std::streambuf* origOut = std::cout.rdbuf(capturedOut.rdbuf());

    const int exitCode = RunExportWithExecutor(opts, exportList, outputDir.path, metadataDir.path, exec);

    std::cerr.rdbuf(origErr);
    std::cout.rdbuf(origOut);

    // Exit code must be 0 — checksum failure is non-fatal
    REQUIRE(exitCode == 0);
}
