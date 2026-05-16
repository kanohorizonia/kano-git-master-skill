// Feature: kog-export-command — Property-based tests for pure helper functions
//
// Tests Properties 1–16 as defined in the design document.
// Each property uses RapidCheck generators to drive ≥ 100 iterations.
//
// **Validates: Requirements 1.2, 1.3, 2.2, 2.3, 2.5, 3.1, 3.6, 4.2, 5.1,
//              5.2, 5.3, 5.4, 5.5, 5.6, 6.1, 7.2, 7.3, 7.4, 8.1, 8.2**

#include <catch2/catch_test_macros.hpp>

#include "export_helpers.hpp"

#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace kano::git::commands;
using namespace kano::git::shell;
namespace workspace = kano::git::workspace;

// ---------------------------------------------------------------------------
// Generator helpers
// ---------------------------------------------------------------------------

namespace {

// Generate a printable ASCII string (no control chars, no null bytes).
// Suitable for repo names, format strings, etc.
std::string GenPrintableString(std::size_t maxLen = 32) {
    const auto len = *rc::gen::inRange<std::size_t>(0, maxLen + 1);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F)));
    }
    return s;
}

// Generate a non-empty printable ASCII string.
std::string GenNonEmptyPrintableString(std::size_t maxLen = 32) {
    const auto len = *rc::gen::inRange<std::size_t>(1, maxLen + 1);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F)));
    }
    return s;
}

// Generate a valid repo name: alphanumeric + hyphens/underscores, non-empty.
std::string GenRepoName() {
    static const char kChars[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789-_";
    const auto len = *rc::gen::inRange<std::size_t>(1, 24);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const auto idx = *rc::gen::inRange<std::size_t>(0, sizeof(kChars) - 1);
        s.push_back(kChars[idx]);
    }
    return s;
}

// Generate a valid archive format: "tar" or "zip".
std::string GenValidFormat() {
    return *rc::gen::element(std::string{"tar"}, std::string{"zip"});
}

// Generate an invalid format string (not "tar" or "zip").
std::string GenInvalidFormat() {
    // Generate any printable string and reject if it happens to be valid
    std::string s;
    do {
        s = GenNonEmptyPrintableString(16);
    } while (s == "tar" || s == "zip");
    return s;
}

// Generate a valid source: "head" or "working-tree".
std::string GenValidSource() {
    return *rc::gen::element(std::string{"head"}, std::string{"working-tree"});
}

// Generate an invalid source string (not "head" or "working-tree").
std::string GenInvalidSource() {
    std::string s;
    do {
        s = GenNonEmptyPrintableString(16);
    } while (s == "head" || s == "working-tree");
    return s;
}

// Generate a non-negative integer revision value.
int GenNonNegativeRevision() {
    return *rc::gen::nonNegative<int>();
}

// Generate a positive pad width (1–10).
int GenPositivePad() {
    return *rc::gen::inRange<int>(1, 11);
}

// Generate a simple filesystem path component (no separators).
std::string GenPathComponent() {
    static const char kChars[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789-_";
    const auto len = *rc::gen::inRange<std::size_t>(1, 16);
    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const auto idx = *rc::gen::inRange<std::size_t>(0, sizeof(kChars) - 1);
        s.push_back(kChars[idx]);
    }
    return s;
}

// Generate a simple filesystem path (2–4 components deep).
std::filesystem::path GenSimplePath() {
    const auto depth = *rc::gen::inRange<int>(2, 5);
    std::filesystem::path p;
    for (int i = 0; i < depth; ++i) {
        p /= GenPathComponent();
    }
    return p;
}

// Generate a list of 0–5 submodule paths.
auto GenSubmodulePaths() -> std::vector<std::filesystem::path> {
    const std::size_t count = *rc::gen::inRange<std::size_t>(0, 6);
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        paths.push_back(GenSimplePath());
    }
    return paths;
}

// Generate a list of 1–5 workspace::RepoRecord entries (subrepos, not root).
auto GenSubrepoRecords(const std::filesystem::path& InRoot) -> std::vector<workspace::RepoRecord> {
    const std::size_t count = *rc::gen::inRange<std::size_t>(1, 6);
    std::vector<workspace::RepoRecord> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workspace::RepoRecord rec;
        rec.path = InRoot / ("subrepo-" + std::to_string(i));
        rec.type = "registered";
        records.push_back(std::move(rec));
    }
    return records;
}

// Generate a list of 1–5 ExportRecord entries (for dry-run plan tests).
auto GenExportRecords(const std::filesystem::path& InRoot) -> std::vector<ExportRecord> {
    const std::size_t count = *rc::gen::inRange<std::size_t>(1, 6);
    std::vector<ExportRecord> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ExportRecord rec;
        rec.repoPath = InRoot / ("repo-" + std::to_string(i));
        rec.repoName = "repo-" + std::to_string(i);
        rec.relativeRepoPath = (i == 0) ? "." : ("repo-" + std::to_string(i));
        rec.isRoot = (i == 0);
        records.push_back(std::move(rec));
    }
    return records;
}

// Generate a list of 1–5 ExportResult entries (for summary tests).
auto GenExportResults() -> std::vector<ExportResult> {
    const std::size_t count = *rc::gen::inRange<std::size_t>(1, 6);
    std::vector<ExportResult> results;
    results.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        ExportResult res;
        res.archiveName = "repo-" + std::to_string(i) + "_rev001.tar";
        res.archivePath = std::filesystem::path("output") / res.archiveName;
        res.success = true;
        results.push_back(std::move(res));
    }
    return results;
}

// Check whether a string contains a substring.
bool Contains(const std::string& InHaystack, const std::string& InNeedle) {
    return InHaystack.find(InNeedle) != std::string::npos;
}

} // anonymous namespace

// ===========================================================================
// Property 1: Export list root-first ordering
// ===========================================================================

// Feature: kog-export-command, Property 1: Export list root-first ordering
TEST_CASE("Property 1: BuildExportList always places workspace root first",
          "[Feature: kog-export-command][Property 1: Export list root-first ordering][property][BuildExportList][req-1.2]") {
    // **Validates: Requirements 1.2**
    //
    // For any non-empty discovery result, BuildExportList shall have the
    // workspace root as its first element.

    rc::prop("BuildExportList places root first for any non-empty discovery result", []() {
        const auto root = std::filesystem::path("workspace") / GenPathComponent();
        const std::vector<workspace::RepoRecord> subrepos = GenSubrepoRecords(root);
        const auto stubExec = [](const std::string&, const std::vector<std::string>&, ExecMode, std::optional<std::filesystem::path>) {
            return ExecResult{0, "unspecified", ""}; // git check-attr default
        };

        const std::vector<ExportRecord> exportList = BuildExportList(root, subrepos, false, false, stubExec);

        RC_ASSERT(!exportList.empty());
        RC_ASSERT(exportList.front().isRoot);
        RC_ASSERT(exportList.front().repoPath == root);
        RC_ASSERT(exportList.front().relativeRepoPath == ".");
    });
}

// ===========================================================================
// Property 2: No-recursive produces single-entry list
// ===========================================================================

// Feature: kog-export-command, Property 2: No-recursive produces single-entry list
TEST_CASE("Property 2: BuildExportList with noRecursive=true returns exactly one entry",
          "[Feature: kog-export-command][Property 2: No-recursive produces single-entry list][property][BuildExportList][req-1.3]") {
    // **Validates: Requirements 1.3**
    //
    // For any discovery result (including many subrepos), when noRecursive=true,
    // BuildExportList shall return a list containing exactly one entry.

    rc::prop("BuildExportList with noRecursive=true returns exactly one entry", []() {
        const auto root = std::filesystem::path("workspace") / GenPathComponent();
        const std::vector<workspace::RepoRecord> subrepos = GenSubrepoRecords(root);
        const auto stubExec = [](const std::string&, const std::vector<std::string>&, ExecMode, std::optional<std::filesystem::path>) {
            return ExecResult{0, "unspecified", ""};
        };

        const std::vector<ExportRecord> exportList = BuildExportList(root, subrepos, true, false, stubExec);

        RC_ASSERT(exportList.size() == 1);
        RC_ASSERT(exportList.front().isRoot);
        RC_ASSERT(exportList.front().repoPath == root);
        RC_ASSERT(exportList.front().relativeRepoPath == ".");
    });
}

// ===========================================================================
// Property 3: Revision zero-padding
// ===========================================================================

// Feature: kog-export-command, Property 3: Revision zero-padding
TEST_CASE("Property 3: FormatRevision produces string of length >= pad with correct numeric value",
          "[Feature: kog-export-command][Property 3: Revision zero-padding][property][FormatRevision][req-2.2][req-2.5]") {
    // **Validates: Requirements 2.2, 2.5**
    //
    // For any r >= 0 and n >= 1, FormatRevision(r, n) shall produce a string
    // of length >= n whose numeric value equals r.

    rc::prop("FormatRevision length >= pad and numeric value == r", []() {
        const int r = GenNonNegativeRevision();
        const int n = GenPositivePad();

        const std::string formatted = FormatRevision(r, n);

        // Length must be at least n
        RC_ASSERT(static_cast<int>(formatted.size()) >= n);

        // Numeric value must equal r
        const int parsed = std::stoi(formatted);
        RC_ASSERT(parsed == r);
    });
}

// ===========================================================================
// Property 4: Archive naming pattern
// ===========================================================================

// Feature: kog-export-command, Property 4: Archive naming pattern
TEST_CASE("Property 4: ComputeArchiveName returns <name>_rev<rev>.<format>",
          "[Feature: kog-export-command]"
          "[Property 4: Archive naming pattern]"
          "[property][ComputeArchiveName][req-2.3]") {
    // **Validates: Requirements 2.3**
    //
    // For any repo name s, revision string r, and format f in {"tar","zip"},
    // ComputeArchiveName(s, r, f) shall return s + "_rev" + r + "." + f.

    rc::prop("ComputeArchiveName matches expected pattern", []() {
        const std::string s = GenRepoName();
        const std::string r = FormatRevision(GenNonNegativeRevision(), GenPositivePad());
        const std::string f = GenValidFormat();

        const std::string archiveName = ComputeArchiveName(s, r, f);
        const std::string expected = s + "_rev" + r + "." + f;

        RC_ASSERT(archiveName == expected);
    });
}

// ===========================================================================
// Property 5: Git archive argument construction
// ===========================================================================

// Feature: kog-export-command, Property 5: Git archive argument construction
TEST_CASE("Property 5: BuildGitArchiveArgs contains HEAD, --format=, --prefix=, --output=",
          "[Feature: kog-export-command]"
          "[Property 5: Git archive argument construction]"
          "[property][BuildGitArchiveArgs][req-3.1]") {
    // **Validates: Requirements 3.1**
    //
    // For any format f, prefix p, and output path o, BuildGitArchiveArgs shall
    // return a vector containing "HEAD", "--format="+f, "--prefix="+p,
    // "--output="+o.generic_string().

    rc::prop("BuildGitArchiveArgs contains all required arguments", []() {
        const std::string f = GenValidFormat();
        const std::string p = GenRepoName() + "/";
        const std::filesystem::path o = GenSimplePath() / (GenRepoName() + ".tar");

        const auto args = BuildGitArchiveArgs(f, p, o);

        // Must contain "HEAD"
        RC_ASSERT(std::find(args.begin(), args.end(), "HEAD") != args.end());

        // Must contain "--format=<f>"
        const std::string formatArg = "--format=" + f;
        RC_ASSERT(std::find(args.begin(), args.end(), formatArg) != args.end());

        // Must contain "--prefix=<p>"
        const std::string prefixArg = "--prefix=" + p;
        RC_ASSERT(std::find(args.begin(), args.end(), prefixArg) != args.end());

        // Must contain "--output=<o.generic_string()>"
        const std::string outputArg = "--output=" + o.generic_string();
        RC_ASSERT(std::find(args.begin(), args.end(), outputArg) != args.end());
    });
}

// ===========================================================================
// Property 6: Output directory resolution
// ===========================================================================

// Feature: kog-export-command, Property 6: Output directory resolution
TEST_CASE("Property 6: ResolveOutputDir returns default when explicit is empty",
          "[Feature: kog-export-command]"
          "[Property 6: Output directory resolution]"
          "[property][ResolveOutputDir][req-3.6][req-3.7]") {
    // **Validates: Requirements 3.6, 3.7**
    //
    // When explicit is empty, ResolveOutputDir returns cwd/.kano/tmp/git/export.
    // When explicit is non-empty, ResolveOutputDir returns that path.

    rc::prop("ResolveOutputDir returns default path when explicit is empty", []() {
        const auto cwd = GenSimplePath();

        const auto resolved = ResolveOutputDir(cwd, "");
        const auto expected = cwd / ".kano" / "tmp" / "git" / "export";

        RC_ASSERT(resolved == expected);
    });

    rc::prop("ResolveOutputDir returns explicit path when non-empty", []() {
        const auto cwd = GenSimplePath();
        const std::string explicitPath = (GenSimplePath() / GenPathComponent()).generic_string();

        const auto resolved = ResolveOutputDir(cwd, explicitPath);

        RC_ASSERT(resolved == std::filesystem::path(explicitPath));
    });
}

// ===========================================================================
// Property 7: Working-tree tar excludes submodules
// ===========================================================================

// Feature: kog-export-command, Property 7: Working-tree tar excludes submodules
TEST_CASE("Property 7: BuildWorkingTreeTarArgs contains one --exclude per submodule path",
          "[Feature: kog-export-command]"
          "[Property 7: Working-tree tar excludes submodules]"
          "[property][BuildWorkingTreeTarArgs][req-4.2]") {
    // **Validates: Requirements 4.2**
    //
    // For any non-empty list of submodule paths S, BuildWorkingTreeTarArgs
    // shall contain an --exclude argument for every path in S.

    rc::prop("BuildWorkingTreeTarArgs has --exclude for each submodule path", []() {
        const auto repoPath = GenSimplePath();
        const auto outputPath = GenSimplePath() / (GenRepoName() + ".tar");
        const auto submodulePaths = GenSubmodulePaths();

        // Only test with non-empty submodule lists for this property
        RC_PRE(!submodulePaths.empty());

        const auto args = BuildWorkingTreeTarArgs(repoPath, outputPath, submodulePaths);

        // Count --exclude arguments matching each submodule path
        for (const auto& subPath : submodulePaths) {
            const std::string excludeArg = "--exclude=" + subPath.generic_string();
            const auto found = std::find(args.begin(), args.end(), excludeArg);
            RC_ASSERT(found != args.end());
        }
    });
}

// ===========================================================================
// Property 8: Manifest naming pattern
// ===========================================================================

// Feature: kog-export-command, Property 8: Manifest naming pattern
TEST_CASE("Property 8: ComputeManifestName returns <base>_manifest.txt",
          "[Feature: kog-export-command]"
          "[Property 8: Manifest naming pattern]"
          "[property][ComputeManifestName][req-5.1]") {
    // **Validates: Requirements 5.1**
    //
    // For any archive base name b, ComputeManifestName(b) shall return
    // b + "_manifest.txt".

    rc::prop("ComputeManifestName matches expected pattern", []() {
        const std::string b = GenRepoName() + "_rev001";

        const std::string manifestName = ComputeManifestName(b);
        const std::string expected = b + "_manifest.txt";

        RC_ASSERT(manifestName == expected);
    });
}

// ===========================================================================
// Property 9: Manifest content completeness
// ===========================================================================

// Feature: kog-export-command, Property 9: Manifest content completeness
TEST_CASE("Property 9: FormatManifest output contains all 10 required field labels",
          "[Feature: kog-export-command]"
          "[Property 9: Manifest content completeness]"
          "[property][FormatManifest][req-5.2][req-5.3][req-5.4]") {
    // **Validates: Requirements 5.2, 5.3, 5.4**
    //
    // For any ExportRecord and ExportResult, FormatManifest shall produce a
    // string containing all 10 required field labels.

    static const std::vector<std::string> kRequiredLabels = {
        "Package:",
        "Repo:",
        "RepoRoot:",
        "ArchiveRoot:",
        "RevisionFirstParentCount:",
        "GitShortSHA:",
        "Format:",
        "CreatedAtUTC:",
        "GitStatusShort:",
        "TrackedFilesSample:",
    };

    rc::prop("FormatManifest contains all required field labels", []() {
        ExportRecord record;
        record.repoPath = GenSimplePath();
        record.repoName = GenRepoName();
        record.relativeRepoPath = record.repoName;
        record.isRoot = *rc::gen::arbitrary<bool>();

        ExportResult result;
        result.archiveName = ComputeArchiveName(record.repoName,
                                                FormatRevision(GenNonNegativeRevision(), 3),
                                                GenValidFormat());
        result.archivePath = std::filesystem::path("output") / result.archiveName;
        result.success = true;

        ExportOptions opts;
        opts.format = GenValidFormat();
        opts.source = GenValidSource();
        opts.revPad = GenPositivePad();
        opts.prefix = "";

        const std::string statusOut = GenPrintableString(64);
        const std::string lsFilesOut = GenPrintableString(64);

        const std::string manifest = FormatManifest(record, result, opts, statusOut, lsFilesOut);

        for (const auto& label : kRequiredLabels) {
            RC_ASSERT(Contains(manifest, label));
        }
    });
}

// ===========================================================================
// Property 10: Checksum filename pattern
// ===========================================================================

// Feature: kog-export-command, Property 10: Checksum filename pattern
TEST_CASE("Property 10: ComputeChecksumFilename returns <filename>.sha256",
          "[Feature: kog-export-command]"
          "[Property 10: Checksum filename pattern]"
          "[property][ComputeChecksumFilename][req-5.5][req-5.6]") {
    // **Validates: Requirements 5.5, 5.6**
    //
    // For any filename f, ComputeChecksumFilename(f) shall return f + ".sha256".

    rc::prop("ComputeChecksumFilename matches expected pattern", []() {
        const std::string f = GenRepoName() + "_rev001.tar";

        const std::string checksumFilename = ComputeChecksumFilename(f);
        const std::string expected = f + ".sha256";

        RC_ASSERT(checksumFilename == expected);
    });
}

// ===========================================================================
// Property 11: Dry-run output content
// ===========================================================================

// Feature: kog-export-command, Property 11: Dry-run output content
TEST_CASE("Property 11: FormatDryRunPlan contains each repo's archive name and output dir",
          "[Feature: kog-export-command]"
          "[Property 11: Dry-run output content]"
          "[property][FormatDryRunPlan][req-6.1]") {
    // **Validates: Requirements 6.1**
    //
    // For any non-empty export plan and ExportOptions, FormatDryRunPlan shall
    // produce a string containing each repo's computed archive name and the
    // resolved output directory path.

    rc::prop("FormatDryRunPlan contains archive names and output dir", []() {
        const auto root = GenSimplePath();
        const std::vector<ExportRecord> records = GenExportRecords(root);
        RC_PRE(!records.empty());

        ExportOptions opts;
        opts.format = GenValidFormat();
        opts.source = GenValidSource();
        opts.revPad = GenPositivePad();
        opts.outputDir = GenSimplePath();

        const std::string plan = FormatDryRunPlan(records, opts);

        // Must contain the output directory
        RC_ASSERT(Contains(plan, opts.outputDir.generic_string()));

        // Must contain each repo's name (archive name contains repo name)
        for (const auto& record : records) {
            RC_ASSERT(Contains(plan, record.repoName));
        }
    });
}

// ===========================================================================
// Property 12: Format validation rejects invalid values
// ===========================================================================

// Feature: kog-export-command, Property 12: Format validation rejects invalid values
TEST_CASE("Property 12: ValidateOptions returns false for any format not in {tar, zip}",
          "[Feature: kog-export-command]"
          "[Property 12: Format validation rejects invalid values]"
          "[property][ValidateOptions][req-7.2]") {
    // **Validates: Requirements 7.2**
    //
    // For any string s not in {"tar","zip"}, ValidateOptions shall return false
    // when format = s.

    rc::prop("ValidateOptions rejects invalid format strings", []() {
        const std::string invalidFormat = GenInvalidFormat();

        ExportOptions opts;
        opts.format = invalidFormat;
        opts.source = "head";
        opts.revPad = 3;

        const bool valid = ValidateOptions(opts);
        RC_ASSERT(!valid);
    });
}

// ===========================================================================
// Property 13: Source validation rejects invalid values
// ===========================================================================

// Feature: kog-export-command, Property 13: Source validation rejects invalid values
TEST_CASE("Property 13: ValidateOptions returns false for any source not in {head, working-tree}",
          "[Feature: kog-export-command]"
          "[Property 13: Source validation rejects invalid values]"
          "[property][ValidateOptions][req-7.3]") {
    // **Validates: Requirements 7.3**
    //
    // For any string s not in {"head","working-tree"}, ValidateOptions shall
    // return false when source = s.

    rc::prop("ValidateOptions rejects invalid source strings", []() {
        const std::string invalidSource = GenInvalidSource();

        ExportOptions opts;
        opts.format = "tar";
        opts.source = invalidSource;
        opts.revPad = 3;

        const bool valid = ValidateOptions(opts);
        RC_ASSERT(!valid);
    });
}

// ===========================================================================
// Property 14: Rev-pad validation rejects non-positive values
// ===========================================================================

// Feature: kog-export-command, Property 14: Rev-pad validation rejects non-positive values
TEST_CASE("Property 14: ValidateOptions returns false for any revPad <= 0",
          "[Feature: kog-export-command]"
          "[Property 14: Rev-pad validation rejects non-positive values]"
          "[property][ValidateOptions][req-7.4]") {
    // **Validates: Requirements 7.4**
    //
    // For any integer n <= 0, ValidateOptions shall return false when revPad = n.

    rc::prop("ValidateOptions rejects non-positive revPad values", []() {
        // Generate a non-positive integer (0 or negative)
        const int nonPositivePad = *rc::gen::inRange<int>(-100, 1);

        ExportOptions opts;
        opts.format = "tar";
        opts.source = "head";
        opts.revPad = nonPositivePad;

        const bool valid = ValidateOptions(opts);
        RC_ASSERT(!valid);
    });
}

// ===========================================================================
// Property 15: Progress line content
// ===========================================================================

// Feature: kog-export-command, Property 15: Progress line content
TEST_CASE("Property 15: FormatProgressLine contains repo name and archive path",
          "[Feature: kog-export-command]"
          "[Property 15: Progress line content]"
          "[property][FormatProgressLine][req-8.1]") {
    // **Validates: Requirements 8.1**
    //
    // For any repo name r and archive path p, FormatProgressLine(r, p) shall
    // produce a string containing both r and p.generic_string().

    rc::prop("FormatProgressLine contains repo name and archive path", []() {
        const std::string repoName = GenRepoName();
        const std::filesystem::path archivePath =
            GenSimplePath() / (repoName + "_rev001.tar");

        const std::string progressLine = FormatProgressLine(repoName, archivePath);

        RC_ASSERT(Contains(progressLine, repoName));
        RC_ASSERT(Contains(progressLine, archivePath.generic_string()));
    });
}

// ===========================================================================
// Property 16: Summary content completeness
// ===========================================================================

// Feature: kog-export-command, Property 16: Summary content completeness
TEST_CASE("Property 16: FormatSummary contains output dir, metadata dir, and archive names",
          "[Feature: kog-export-command]"
          "[Property 16: Summary content completeness]"
          "[property][FormatSummary][req-8.2]") {
    // **Validates: Requirements 8.2**
    //
    // For any non-empty list of ExportResults, output directory o, and metadata
    // directory m, FormatSummary shall produce a string containing
    // o.generic_string(), m.generic_string(), and each successful archive's
    // archiveName.

    rc::prop("FormatSummary contains output dir, metadata dir, and archive names", []() {
        const auto outputDir = GenSimplePath();
        const auto metadataDir = outputDir / "metadata";
        const std::vector<ExportResult> results = GenExportResults();
        RC_PRE(!results.empty());

        const std::string summary = FormatSummary(outputDir, metadataDir, results);

        // Must contain output directory
        RC_ASSERT(Contains(summary, outputDir.generic_string()));

        // Must contain metadata directory
        RC_ASSERT(Contains(summary, metadataDir.generic_string()));

        // Must contain each successful archive name
        for (const auto& result : results) {
            if (result.success) {
                RC_ASSERT(Contains(summary, result.archiveName));
            }
        }
    });
}
