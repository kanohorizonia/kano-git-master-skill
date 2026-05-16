#pragma once

// export_helpers.hpp — Pure helper function declarations for kog export command.
// These functions are implemented in export_cmd.cpp (anonymous namespace) and
// exposed here for property-based and unit testing.

#include "discovery.hpp"
#include "shell_executor.hpp"

#include <CLI/CLI.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kano::git::commands {

// ---------------------------------------------------------------------------
// Shell executor type alias for dependency injection in tests
// ---------------------------------------------------------------------------

// A ShellExecutor is a callable that matches the signature of
// shell::ExecuteCommand.  Production code passes the real implementation;
// unit tests pass a stub that controls which commands succeed or fail.
using ShellExecutor = std::function<shell::ExecResult(
    const std::string& InCommand,
    const std::vector<std::string>& InArgs,
    shell::ExecMode InMode,
    std::optional<std::filesystem::path> InWorkingDir)>;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct ExportOptions {
    std::filesystem::path outputDir;
    std::string format;          // "tar" or "zip"
    std::string prefix;          // empty = use repo name
    bool includeSubmoduleStubs = false;
    bool noRecursive = false;
    bool dryRun = false;
    bool noMetadata = false;
    int revPad = 3;
    std::string source;          // "head" or "working-tree"
    bool single = false;
    int logCount = 10;
};

struct ExportRecord {
    std::filesystem::path repoPath;
    std::string repoName;
    std::string relativeRepoPath;
    bool isRoot = false;
    std::vector<std::filesystem::path> submodulePaths; // populated for root repo only
};

struct ExportResult {
    std::string archiveName;     // e.g. "KTOStudio_rev042.tar"
    std::filesystem::path archivePath;
    bool success = false;
    std::string errorMessage;
};

// ---------------------------------------------------------------------------
// Pure helper functions (no shell calls, no filesystem side-effects)
// ---------------------------------------------------------------------------

// Validates format, source, and rev-pad fields of ExportOptions.
// Returns true if all fields are valid; prints errors to stderr and returns
// false otherwise.
auto ValidateOptions(const ExportOptions& InOpts) -> bool;

// Computes the output directory: if InExplicit is non-empty, returns
// std::filesystem::path(InExplicit); otherwise returns
// InCwd / ".kano" / "tmp" / "git" / "export".
auto ResolveOutputDir(const std::filesystem::path& InCwd,
                      const std::string& InExplicit) -> std::filesystem::path;

// Formats a revision integer as a zero-padded string of at least InPad digits.
// Pure string formatting only — no shell calls.
auto FormatRevision(int InRevision, int InPad) -> std::string;

// Returns "<InRepoName>_rev<InRevision>.<InFormat>".
auto ComputeArchiveName(const std::string& InRepoName,
                        const std::string& InRevision,
                        const std::string& InFormat) -> std::string;

// Returns InExplicitPrefix if non-empty; otherwise returns "<InRepoName>/".
auto ComputePrefix(const std::string& InRepoName,
                   const std::string& InExplicitPrefix) -> std::string;

// Returns the argument vector for:
//   git archive HEAD --format=<InFormat> --prefix=<InPrefix> --output=<InOutputPath>
auto BuildGitArchiveArgs(const std::string& InFormat,
                         const std::string& InPrefix,
                         const std::filesystem::path& InOutputPath) -> std::vector<std::string>;

// Returns the list of files to be included in a working-tree export,
// respecting .gitignore. If InSingle is true, also includes submodule contents.
auto CollectWorkingTreeFiles(const ExportRecord& InRecord,
                             bool InSingle,
                             const ShellExecutor& InExec) -> std::vector<std::string>;

// Returns "<InArchiveBaseName>_manifest.txt".
auto ComputeManifestName(const std::string& InArchiveBaseName) -> std::string;

// Returns "<InFilename>.sha256".
auto ComputeChecksumFilename(const std::string& InFilename) -> std::string;

// Formats the manifest text for one exported repo.
// InStatusOut and InLsFilesOut are the raw outputs of git status --short and
// git ls-files respectively (may be empty strings).
auto FormatManifest(const ExportRecord& InRecord,
                    const ExportResult& InResult,
                    const ExportOptions& InOpts,
                    const std::string& InStatusOut,
                    const std::string& InLsFilesOut) -> std::string;

// Formats the human-readable metadata markdown text.
auto FormatMarkdownMetadata(const ExportRecord& InRecord,
                            const ExportResult& InResult,
                            const ExportOptions& InOpts,
                            const std::string& InRootLog,
                            const std::vector<std::pair<std::string, std::string>>& InSubLogs) -> std::string;

// Formats a single progress line: "Exported <InRepoName> -> <InArchivePath>".
auto FormatProgressLine(const std::string& InRepoName,
                        const std::filesystem::path& InArchivePath) -> std::string;

// Formats the final summary block listing output dir, metadata dir, and all
// successful archive names.
auto FormatSummary(const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const std::vector<ExportResult>& InResults) -> std::string;

// Formats the dry-run plan: lists each repo's computed archive name and the
// resolved output directory.
auto FormatDryRunPlan(const std::vector<ExportRecord>& InRecords,
                      const ExportOptions& InOpts) -> std::string;

// Builds the ordered export list: workspace root first, then discovered
// subrepos (unless InNoRecursive is true).
// InRoot is the workspace root path; InDiscovered is the list returned by
// DiscoverWorkspaceRepos (may include the root or not).
auto BuildExportList(const std::filesystem::path& InRoot,
                      const std::vector<workspace::RepoRecord>& InDiscovered,
                      bool InNoRecursive,
                      bool InSingle,
                      const ShellExecutor& InExec) -> std::vector<ExportRecord>;

// Returns true if the given path is marked with 'export-ignore' attribute
// in the context of InRepoPath.
auto IsPathIgnoredByExport(const std::filesystem::path& InRepoPath,
                           const std::filesystem::path& InTargetPath,
                           const ShellExecutor& InExec) -> bool;

// ---------------------------------------------------------------------------
// Shell-calling helpers (exposed for unit testing via dependency injection)
// ---------------------------------------------------------------------------

// Exports a single repository.  InExec is the shell executor to use for all
// external process invocations; pass shell::ExecuteCommand in production and
// a stub in tests.
auto ExportOneRepo(const ExportRecord& InRecord,
                   const ExportOptions& InOpts,
                   const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const ShellExecutor& InExec) -> ExportResult;

// Top-level export orchestrator.  InExec is the shell executor to use for all
// external process invocations; pass shell::ExecuteCommand in production and
// a stub in tests.  InExportList is the pre-built list of repos to export
// (allows tests to bypass DiscoverWorkspaceRepos).
// Returns 0 on full success, 1 if any repo failed.
auto RunExportWithExecutor(const ExportOptions& InOpts,
                           const std::vector<ExportRecord>& InExportList,
                           const std::filesystem::path& InOutputDir,
                           const std::filesystem::path& InMetadataDir,
                           const ShellExecutor& InExec) -> int;

// ---------------------------------------------------------------------------
// CLI registration (public entry point)
// ---------------------------------------------------------------------------

// Registers the 'export' subcommand with CLI11.
// Declared here so that test targets can call it without depending on
// command_declarations.hpp (which lives in the runtime layer).
void RegisterExport(CLI::App& InApp);

} // namespace kano::git::commands
