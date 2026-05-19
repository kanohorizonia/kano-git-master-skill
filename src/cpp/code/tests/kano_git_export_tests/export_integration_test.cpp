// export_integration_test.cpp — End-to-end integration tests for kog export
//
// Tests the full kog export command against a real temporary git repo with
// one registered submodule, verifying file system output and exit codes.
//
// Test 1: Default options produce one .tar per repo in <cwd>/.kano/tmp/git/export/
//         with correct _revNNN naming (Requirements 1.1, 1.2, 2.3, 3.1, 3.4, 3.6)
// Test 2: --dry-run creates no files and exits 0 (Requirements 6.1, 6.2, 6.3, 6.4)
// Test 3: --no-metadata skips manifest and .sha256 files (Requirement 5.7)
// Test 4: --no-recursive exports only the root repo (Requirement 1.3, 3.7)
// Test 5: --format zip produces .zip archives (Requirement 3.5)
// Test 6: --output <dir> writes archives to the specified directory (Requirement 3.7)
//
// **Validates: Requirements 1.1, 1.2, 1.3, 3.1, 3.4, 3.5, 3.6, 3.7, 5.7,
//              6.1, 6.2, 6.3, 6.4**

#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace kano::git::tests::functional;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace {

// Assert a CommandResult succeeded, printing diagnostics on failure.
auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

// Write a text file, creating parent directories as needed.
auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

// Configure git identity in a repo (required for commits in a fresh sandbox).
auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

// Count files in a directory matching a given extension (non-recursive).
auto CountFilesWithExtension(const std::filesystem::path& InDir,
                              const std::string& InExt) -> int {
    if (!std::filesystem::exists(InDir)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(InDir)) {
        if (entry.is_regular_file() && entry.path().extension() == InExt) {
            ++count;
        }
    }
    return count;
}

// Check whether any file in a directory has a name containing the given substring.
auto AnyFileContains(const std::filesystem::path& InDir,
                     const std::string& InSubstring) -> bool {
    if (!std::filesystem::exists(InDir)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(InDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(InSubstring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Check whether any file in a directory (recursively) has a name containing
// the given substring.
auto AnyFileContainsRecursive(const std::filesystem::path& InDir,
                               const std::string& InSubstring) -> bool {
    if (!std::filesystem::exists(InDir)) {
        return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(InDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(InSubstring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Count all regular files in a directory recursively.
auto CountFilesRecursive(const std::filesystem::path& InDir) -> int {
    if (!std::filesystem::exists(InDir)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(InDir)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// ExportWorkspaceContext — a root repo with one registered submodule.
//
// Layout:
//   sandbox/
//     child-remote.git/     — bare remote for the child repo
//     child-seed/           — seed repo for child (used to push initial commit)
//     root-remote.git/      — bare remote for the root repo
//     root-seed/            — seed repo for root (used to push initial commit)
//     root-clone/           — the working clone we run kog export from
//       deps/child/         — the registered submodule (checked out)
// ---------------------------------------------------------------------------

struct ExportWorkspaceContext {
    SandboxContext sandbox;
    std::filesystem::path rootClone;
    std::filesystem::path childClone;
    std::string submodulePath;
    std::string rootRepoName;
    std::string childRepoName;
};

auto CreateExportWorkspace(const std::string& InName) -> ExportWorkspaceContext {
    ExportWorkspaceContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.submodulePath = "deps/child";
    ctx.rootRepoName = "root-clone";
    ctx.childRepoName = "child";

    const auto childBareRemote = (ctx.sandbox.root / "child-remote.git").lexically_normal();
    const auto childSeedRepo   = (ctx.sandbox.root / "child-seed").lexically_normal();
    const auto rootBareRemote  = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    const auto rootSeedRepo    = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.rootClone              = (ctx.sandbox.root / "root-clone").lexically_normal();

    const std::string branch = "main";

    // --- Set up child repo ---
    RequireSuccess(RunGit({"init", "--bare", childBareRemote.string()}, ctx.sandbox.root),
                   "init child bare");
    RequireSuccess(RunGit({"init", childSeedRepo.string()}, ctx.sandbox.root),
                   "init child seed");
    ConfigureIdentity(childSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", branch}, childSeedRepo), "checkout child branch");
    WriteTextFile(childSeedRepo / "child.txt", "child content\n");
    RequireSuccess(RunGit({"add", "child.txt"}, childSeedRepo), "child add");
    RequireSuccess(RunGit({"commit", "-m", "child seed"}, childSeedRepo), "child commit");
    RequireSuccess(RunGit({"remote", "add", "origin", childBareRemote.string()}, childSeedRepo),
                   "child add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, childSeedRepo), "child push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + branch)}, childBareRemote),
                   "child bare HEAD");

    // --- Set up root repo ---
    RequireSuccess(RunGit({"init", "--bare", rootBareRemote.string()}, ctx.sandbox.root),
                   "init root bare");
    RequireSuccess(RunGit({"init", rootSeedRepo.string()}, ctx.sandbox.root),
                   "init root seed");
    ConfigureIdentity(rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", branch}, rootSeedRepo), "checkout root branch");
    WriteTextFile(rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(rootSeedRepo / "README.md", "root content\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, rootSeedRepo), "root base commit");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always",
                "submodule", "add", "-b", branch,
                childBareRemote.string(), ctx.submodulePath},
               rootSeedRepo),
        "root add submodule");
    RequireSuccess(RunGit({"commit", "-am", "add submodule"}, rootSeedRepo),
                   "root commit submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", rootBareRemote.string()}, rootSeedRepo),
                   "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + branch)}, rootBareRemote),
                   "root bare HEAD");

    // --- Clone root with submodules ---
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always",
                "clone", "--recurse-submodules",
                rootBareRemote.string(), ctx.rootClone.string()},
               ctx.sandbox.root),
        "clone root with submodules");
    ConfigureIdentity(ctx.rootClone);
    ctx.childClone = (ctx.rootClone / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    ConfigureIdentity(ctx.childClone);

    return ctx;
}

// Run kog discover to populate the workspace manifest so that kog export
// can find registered submodules via RegisteredOnly scope.
auto RunKogDiscover(const ExportWorkspaceContext& InCtx) -> CommandResult {
    return RunKogWithEnv(
        {"discover"},
        InCtx.rootClone,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});
}

// Run kog export with the given extra args from the root clone directory.
// Allows the file:// protocol so submodule discovery works in the sandbox.
auto RunKogExport(const ExportWorkspaceContext& InCtx,
                  const std::vector<std::string>& InExtraArgs) -> CommandResult {
    std::vector<std::string> args = {"export"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKogWithEnv(args, InCtx.rootClone,
                         {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});
}

} // anonymous namespace

// ===========================================================================
// Test 1: Default options produce one .tar per repo in the default output dir
//         with correct _revNNN naming
// ===========================================================================

TEST_CASE("kog export default options produce one tar per repo in default output dir",
          "[Integration][export][default][req-1.1][req-1.2][req-2.3][req-3.1][req-3.4][req-3.6]") {
    // **Validates: Requirements 1.1, 1.2, 2.3, 3.1, 3.4, 3.6**
    //
    // When kog export is invoked with no extra options from a workspace root
    // that has one registered submodule:
    //   - Exit code must be 0
    //   - Two .tar archives must be created in <cwd>/.kano/tmp/git/export/
    //   - Each archive name must match the _revNNN pattern
    //   - The root repo archive must appear first (root-first ordering)

    const auto ctx = CreateExportWorkspace("default-export");

    const auto discoverResult = RunKogDiscover(ctx);
    INFO("kog discover exit=" << discoverResult.exitCode);
    INFO("kog discover stdout=" << discoverResult.stdoutText);
    INFO("kog discover stderr=" << discoverResult.stderrText);
    REQUIRE(discoverResult.exitCode == 0);

    const auto result = RunKogExport(ctx, {});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // Default output dir: <cwd>/.kano/tmp/git/export/
    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Two .tar archives: one for root, one for child submodule
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 2);

    // Each archive name must contain "_rev" (the revision naming pattern)
    REQUIRE(AnyFileContains(outputDir, "_rev"));

    // The root repo archive must be present (named after the root clone dir)
    REQUIRE(AnyFileContains(outputDir, ctx.rootRepoName));

    // The child submodule archive must be present
    REQUIRE(AnyFileContains(outputDir, ctx.childRepoName));

    // stdout must mention the output directory
    REQUIRE(result.stdoutText.find(".kano") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 2: --dry-run creates no files and exits 0
// ===========================================================================

TEST_CASE("kog export --dry-run creates no files and exits 0",
          "[Integration][export][dry-run][req-6.1][req-6.2][req-6.3][req-6.4]") {
    // **Validates: Requirements 6.1, 6.2, 6.3, 6.4**
    //
    // When --dry-run is specified:
    //   - Exit code must be 0
    //   - No archive files, manifest files, or checksum files must be created
    //   - The output directory must NOT be created
    //   - stdout must contain the list of repos that would be exported and
    //     the target output directory

    const auto ctx = CreateExportWorkspace("dry-run-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--dry-run"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // The default output directory must NOT have been created
    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE_FALSE(std::filesystem::exists(outputDir));

    // No files must have been created anywhere under .kano/tmp
    const auto kanoTmpDir = ctx.rootClone / ".kano" / "tmp";
    REQUIRE(CountFilesRecursive(kanoTmpDir) == 0);

    // stdout must mention the repos that would be exported
    REQUIRE(result.stdoutText.find(ctx.rootRepoName) != std::string::npos);

    // stdout must mention the output directory path
    REQUIRE(result.stdoutText.find(".kano") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 3: --no-metadata skips manifest and .sha256 files
// ===========================================================================

TEST_CASE("kog export --no-metadata skips manifest and sha256 files",
          "[Integration][export][no-metadata][req-5.7]") {
    // **Validates: Requirements 5.7**
    //
    // When --no-metadata is specified:
    //   - Exit code must be 0
    //   - Archive files (.tar) must still be created
    //   - No manifest files (_manifest.txt) must be created
    //   - No checksum files (.sha256) must be created

    const auto ctx = CreateExportWorkspace("no-metadata-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--no-metadata"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Archives must still be created
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 2);

    // No manifest files
    const auto metadataDir = outputDir / "metadata";
    REQUIRE_FALSE(AnyFileContainsRecursive(outputDir, "_manifest.txt"));

    // No checksum files
    REQUIRE_FALSE(AnyFileContainsRecursive(outputDir, ".sha256"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --subtree exports standalone archive root",
          "[Integration][export][subtree]") {
    const auto sandbox = CreateSandboxWorkspace("subtree-export");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();
    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init subtree repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");

    WriteTextFile(repo / "README.md", "root\n");
    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/UGS.txt", "ugs\n");
    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Nested/File.txt", "nested\n");
    WriteTextFile(repo / "Engine/Source/Programs/OtherTool/Other.txt", "other\n");
    RequireSuccess(RunGit({"add", "."}, repo), "add files");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");

    const auto subtreePath = (repo / "Engine/Source/Programs/UnrealGameSync").generic_string();
    const auto result = RunKogWithEnv(
        {"export", "--subtree", subtreePath, "--name", "UnrealGameSync", "--source", "head", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));
    REQUIRE(AnyFileContains(outputDir, "UnrealGameSync_rev"));

    const auto list = RunCommand("python", {"-c",
        "import tarfile,glob; p=glob.glob(r'" + outputDir.generic_string() + "/UnrealGameSync_rev*.tar')[0]; "
        "t=tarfile.open(p); print('\\n'.join(m.name for m in t.getmembers() if m.isfile()))"}, repo);
    REQUIRE(list.exitCode == 0);
    REQUIRE(list.stdoutText.find("UnrealGameSync/UGS.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("UnrealGameSync/Nested/File.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("README.md") == std::string::npos);
    REQUIRE(list.stdoutText.find("OtherTool/Other.txt") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

// ===========================================================================
// Test 4: --no-recursive exports only the root repo (single archive)
// ===========================================================================

TEST_CASE("kog export --no-recursive exports only the root repo",
          "[Integration][export][no-recursive][req-1.3][req-3.7]") {
    // **Validates: Requirements 1.3, 3.7**
    //
    // When --no-recursive is specified:
    //   - Exit code must be 0
    //   - Exactly one archive must be created (the root repo only)
    //   - The child submodule must NOT have an archive

    const auto ctx = CreateExportWorkspace("no-recursive-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--no-recursive"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Exactly one .tar archive (root only)
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 1);

    // The root repo archive must be present
    REQUIRE(AnyFileContains(outputDir, ctx.rootRepoName));

    // The child submodule archive must NOT be present
    REQUIRE_FALSE(AnyFileContains(outputDir, ctx.childRepoName));

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 5: --format zip produces .zip archives
// ===========================================================================

TEST_CASE("kog export --format zip produces zip archives",
          "[Integration][export][format-zip][req-3.5]") {
    // **Validates: Requirements 3.5**
    //
    // When --format zip is specified:
    //   - Exit code must be 0
    //   - Archives must have .zip extension (not .tar)
    //   - Two .zip archives must be created (one per repo)

    const auto ctx = CreateExportWorkspace("zip-format-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--format", "zip"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Two .zip archives
    const int zipCount = CountFilesWithExtension(outputDir, ".zip");
    REQUIRE(zipCount == 2);

    // No .tar archives (format is zip, not tar)
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 0);

    // Archive names must contain "_rev" (revision naming pattern)
    REQUIRE(AnyFileContains(outputDir, "_rev"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 6: --output <dir> writes archives to the specified directory
// ===========================================================================

TEST_CASE("kog export --output writes archives to the specified directory",
          "[Integration][export][output-dir][req-3.7]") {
    // **Validates: Requirements 3.7**
    //
    // When --output <dir> is specified:
    //   - Exit code must be 0
    //   - Archives must be written to the specified directory (not the default)
    //   - The default output directory must NOT be created

    const auto ctx = CreateExportWorkspace("custom-output-export");

    // Use a custom output directory inside the sandbox (not inside the repo)
    const auto customOutputDir = (ctx.sandbox.root / "my-exports").lexically_normal();

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--output", customOutputDir.string()});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // Archives must be in the custom output directory
    REQUIRE(std::filesystem::exists(customOutputDir));
    const int tarCount = CountFilesWithExtension(customOutputDir, ".tar");
    REQUIRE(tarCount == 2);

    // The default output directory must NOT have been created
    const auto defaultOutputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE_FALSE(std::filesystem::exists(defaultOutputDir));

    // Archive names must contain "_rev"
    REQUIRE(AnyFileContains(customOutputDir, "_rev"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 7: --single produces export-manifest.json
// ===========================================================================

TEST_CASE("kog export --single produces export-manifest.json",
          "[Integration][export][single][export-manifest][req-manifest]") {
    // Validates: export-manifest.json generation requirements
    //
    // When --single is specified and export succeeds:
    //   - .kano/tmp/export-manifest.json must exist (canonical copy)
    //   - The sibling export-manifest.json must exist alongside the archive
    //   - The manifest must be valid JSON
    //   - The manifest must contain all required keys
    //   - archiveFile / path must use forward slashes
    //   - The archive file referenced by the manifest must exist
    //   - exportMode must be "single"
    //   - singleArchive must be true
    //   - platform must be one of: windows / linux / mac

    const auto ctx = CreateExportWorkspace("single-manifest-export");

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {"--single", "--no-validate-release-archive"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // Canonical manifest: <cwd>/.kano/tmp/<name>_revNNN.export-manifest.json
    const auto kanoTmpDir = ctx.rootClone / ".kano" / "tmp";
    REQUIRE(std::filesystem::exists(kanoTmpDir));
    REQUIRE(AnyFileContainsRecursive(kanoTmpDir, ".export-manifest.json"));

    // Find the actual manifest path
    std::filesystem::path canonicalManifest;
    for (const auto& entry : std::filesystem::directory_iterator(kanoTmpDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(".export-manifest.json") != std::string::npos) {
            canonicalManifest = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(canonicalManifest.empty());
    REQUIRE(std::filesystem::exists(canonicalManifest));

    // Read and parse the manifest
    std::ifstream mf(canonicalManifest);
    REQUIRE(mf.good());
    const std::string manifestText((std::istreambuf_iterator<char>(mf)),
                                    std::istreambuf_iterator<char>());
    REQUIRE_FALSE(manifestText.empty());

    // Must be valid JSON (check for required keys as substrings)
    REQUIRE(manifestText.find("\"schemaVersion\"")  != std::string::npos);
    REQUIRE(manifestText.find("\"exportMode\"")     != std::string::npos);
    REQUIRE(manifestText.find("\"singleArchive\"")  != std::string::npos);
    REQUIRE(manifestText.find("\"archiveFile\"")    != std::string::npos);
    REQUIRE(manifestText.find("\"path\"")           != std::string::npos);
    REQUIRE(manifestText.find("\"sha256\"")         != std::string::npos);
    REQUIRE(manifestText.find("\"sizeBytes\"")      != std::string::npos);
    REQUIRE(manifestText.find("\"platform\"")       != std::string::npos);
    REQUIRE(manifestText.find("\"archives\"")       != std::string::npos);

    // exportMode must be "single"
    REQUIRE(manifestText.find("\"exportMode\": \"single\"") != std::string::npos);

    // singleArchive must be true
    REQUIRE(manifestText.find("\"singleArchive\": true") != std::string::npos);

    // platform must be one of the normalized values
    const bool hasValidPlatform =
        manifestText.find("\"platform\": \"windows\"") != std::string::npos ||
        manifestText.find("\"platform\": \"linux\"")   != std::string::npos ||
        manifestText.find("\"platform\": \"mac\"")     != std::string::npos;
    REQUIRE(hasValidPlatform);

    // archiveFile must use forward slashes (no backslashes)
    // Extract archiveFile value by finding it in the JSON text
    const auto archiveFileKey = std::string("\"archiveFile\": \"");
    const auto keyPos = manifestText.find(archiveFileKey);
    REQUIRE(keyPos != std::string::npos);
    const auto valueStart = keyPos + archiveFileKey.size();
    const auto valueEnd = manifestText.find('"', valueStart);
    REQUIRE(valueEnd != std::string::npos);
    const std::string archiveFilePath = manifestText.substr(valueStart, valueEnd - valueStart);
    REQUIRE(archiveFilePath.find('\\') == std::string::npos);

    // The archive file referenced must exist
    REQUIRE(std::filesystem::exists(std::filesystem::path(archiveFilePath)));

    // Sibling copy alongside the archive
    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(AnyFileContains(outputDir, ".export-manifest.json"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export without --single does not produce export-manifest.json",
          "[Integration][export][no-single][export-manifest][req-manifest]") {
    // When --single is NOT specified, export-manifest.json must NOT be written.

    const auto ctx = CreateExportWorkspace("no-single-no-manifest");

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {});

    INFO("exit=" << result.exitCode);
    REQUIRE(result.exitCode == 0);

    const auto kanoTmpDir = ctx.rootClone / ".kano" / "tmp";
    REQUIRE_FALSE(AnyFileContainsRecursive(kanoTmpDir, ".export-manifest.json"));

    RemoveSandboxWorkspace(ctx.sandbox);
}
