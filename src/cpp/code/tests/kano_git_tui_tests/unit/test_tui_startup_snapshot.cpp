#include <catch2/catch_test_macros.hpp>

#include "tui_startup_snapshot.hpp"

#include <filesystem>
#include <string>
#include <string_view>

using namespace kano::git::commands;

namespace {

auto FixtureAbsolutePath(const std::filesystem::path& InRelative)
    -> std::filesystem::path {
#if defined(_WIN32)
    const auto root = std::filesystem::path{"C:/kog-tui-startup-tests"};
#else
    const auto root = std::filesystem::path{"/kog-tui-startup-tests"};
#endif
    return (root / InRelative).lexically_normal();
}

auto FixtureWorkspaceRoot() -> std::filesystem::path {
    return FixtureAbsolutePath("workspace");
}

auto QuoteJson(const std::string_view InValue) -> std::string {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out{"\""};
    out.reserve(InValue.size() + 2);
    for (const unsigned char ch : InValue) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20U) {
                out += "\\u00";
                out.push_back(kHexDigits[(ch >> 4U) & 0x0FU]);
                out.push_back(kHexDigits[ch & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

auto RepoJsonWithQuotedPath(
    const std::string_view InQuotedPath,
    const std::string_view InType,
    const std::string_view InBranch = "main",
    const std::string_view InTracking = "cached",
    const std::string_view InDirtyJson = "false",
    const bool bInWorktreeDirty = false) -> std::string {
    std::string out{"{\"path\":"};
    out += InQuotedPath;
    out += ",\"type\":";
    out += QuoteJson(InType);
    out += ",\"branch\":";
    out += QuoteJson(InBranch);
    out += ",\"tracking\":";
    out += QuoteJson(InTracking);
    out += ",\"dirty\":";
    out += InDirtyJson;
    out += ",\"worktree_dirty\":";
    out += bInWorktreeDirty ? "true}" : "false}";
    return out;
}

auto RepoJson(
    const std::filesystem::path& InPath,
    const std::string_view InType,
    const std::string_view InBranch = "main",
    const std::string_view InTracking = "cached",
    const std::string_view InDirtyJson = "false",
    const bool bInWorktreeDirty = false) -> std::string {
    return RepoJsonWithQuotedPath(
        QuoteJson(InPath.generic_string()),
        InType,
        InBranch,
        InTracking,
        InDirtyJson,
        bInWorktreeDirty);
}

auto ValidSnapshotWithRepos(
    const std::string_view InRepos,
    const std::string_view InAllowedExternalRoots = "[]") -> std::string {
    std::string out =
        "{\"schemaName\":\"kog.repoOverview\","
        "\"schemaVersion\":1,"
        "\"source\":\"trusted-workspace-manifest\","
        "\"completeness\":\"workspace-inventory\","
        "\"probeMode\":\"none\","
        "\"statusKnown\":true,"
        "\"allowedExternalRoots\":";
    out += InAllowedExternalRoots;
    out += ",\"summary\":{},\"repos\":";
    out += InRepos;
    out.push_back('}');
    return out;
}

auto RootFallbackPayload(const std::filesystem::path& InRoot) -> std::string {
    std::string out =
        "{\"schemaName\":\"kog.repoOverview\","
        "\"schemaVersion\":1,"
        "\"source\":\"root-fallback\","
        "\"completeness\":\"root-only\","
        "\"probeMode\":\"none\","
        "\"statusKnown\":false,"
        "\"allowedExternalRoots\":[],"
        "\"summary\":{},\"repos\":[";
    out += RepoJson(InRoot, "root", "(unknown)", "-");
    out += "]}";
    return out;
}

}  // namespace

TEST_CASE(
    "TUI startup snapshot parser preserves bounded repository audit fields",
    "[unit][tui_startup_snapshot][KG-BUG-0091]") {
    const auto root = FixtureWorkspaceRoot();
    const auto nested = root / "vendor" / "repo";
    const auto payload = ValidSnapshotWithRepos(
        "[" + RepoJson(root, "root", "main", "cached", "true") + "," +
        RepoJson(
            nested,
            "registered",
            "release",
            "origin/release",
            "false",
            true) +
        "]");

    std::string error;
    TuiStartupSnapshotMetadata metadata;
    const auto rows = ParseTuiStartupSnapshotJson(
        payload,
        root,
        &error,
        &metadata);

    REQUIRE(error.empty());
    CHECK(metadata.source == "trusted-workspace-manifest");
    CHECK(metadata.completeness == "workspace-inventory");
    CHECK(metadata.probeMode == "none");
    CHECK(metadata.statusKnown);
    CHECK(metadata.allowedExternalRoots.empty());
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].path == root);
    CHECK(rows[0].parentPath.empty());
    CHECK(rows[0].relativePath == ".");
    CHECK(rows[0].branch == "main");
    CHECK(rows[0].tracking == "cached");
    CHECK(rows[0].repoDirty);
    CHECK_FALSE(rows[0].worktreeDirty);
    CHECK(rows[1].path == nested);
    CHECK(rows[1].parentPath == root);
    CHECK(rows[1].relativePath == "vendor/repo");
    CHECK(rows[1].branch == "release");
    CHECK_FALSE(rows[1].repoDirty);
    CHECK(rows[1].worktreeDirty);
}

TEST_CASE(
    "TUI startup snapshot parser distinguishes empty from malformed",
    "[unit][tui_startup_snapshot][KG-BUG-0091]") {
    const auto root = FixtureWorkspaceRoot();
    std::string error;
    const auto empty = ParseTuiStartupSnapshotJson(
        ValidSnapshotWithRepos("[]"),
        root,
        &error);
    REQUIRE(error.empty());
    REQUIRE(empty.empty());

    const auto malformed = ParseTuiStartupSnapshotJson(
        "{\"schemaName\":\"wrong\",\"schemaVersion\":1,\"repos\":[]}",
        root,
        &error);
    REQUIRE(malformed.empty());
    REQUIRE(error.find("schema") != std::string::npos);

    const auto wrongSchemaType = ParseTuiStartupSnapshotJson(
        "{\"schemaName\":\"kog.repoOverview\","
        "\"schemaVersion\":\"1\",\"repos\":[]}",
        root,
        &error);
    REQUIRE(wrongSchemaType.empty());
    REQUIRE(error.find("schema") != std::string::npos);

    auto liveProbePayload = ValidSnapshotWithRepos("[]");
    const auto probePosition = liveProbePayload.find("\"probeMode\":\"none\"");
    REQUIRE(probePosition != std::string::npos);
    liveProbePayload.replace(
        probePosition,
        std::string("\"probeMode\":\"none\"").size(),
        "\"probeMode\":\"revision-count\"");
    const auto liveProbe = ParseTuiStartupSnapshotJson(
        liveProbePayload,
        root,
        &error);
    REQUIRE(liveProbe.empty());
    REQUIRE(error.find("no-probe") != std::string::npos);
}

TEST_CASE(
    "TUI startup snapshot parser rejects unsafe types and paths",
    "[unit][tui_startup_snapshot][KG-BUG-0091]") {
    const auto root = FixtureWorkspaceRoot();
    std::string error;
    const auto wrongType = ParseTuiStartupSnapshotJson(
        ValidSnapshotWithRepos(
            "[" + RepoJson(root, "root", "main", "cached", "\"false\"") +
            "]"),
        root,
        &error);
    REQUIRE(wrongType.empty());
    REQUIRE(error.find("dirty") != std::string::npos);

    auto quotedNulPath = QuoteJson(root.generic_string());
    quotedNulPath.insert(quotedNulPath.size() - 1, "/\\u0000repo");
    const auto nulPath = ParseTuiStartupSnapshotJson(
        ValidSnapshotWithRepos(
            "[" + RepoJsonWithQuotedPath(quotedNulPath, "root") + "]"),
        root,
        &error);
    REQUIRE(nulPath.empty());
    REQUIRE(error.find("NUL") != std::string::npos);

    const auto escapedRoot = ParseTuiStartupSnapshotJson(
        ValidSnapshotWithRepos(
            "[" + RepoJson(FixtureAbsolutePath("outside/repo"), "registered") +
            "]"),
        root,
        &error);
    REQUIRE(escapedRoot.empty());
    REQUIRE(error.find("escapes") != std::string::npos);
}

TEST_CASE(
    "TUI startup snapshot parser accepts bounded configured external roots",
    "[unit][tui_startup_snapshot][KG-BUG-0091]") {
    const auto root = FixtureWorkspaceRoot();
    const auto externalRoot = FixtureAbsolutePath("external/repos");
    const auto externalRepo = externalRoot / "tool";
    const auto payload = ValidSnapshotWithRepos(
        "[" + RepoJson(externalRepo, "external-root") + "]",
        "[" + QuoteJson(externalRoot.generic_string()) + "]");

    std::string error;
    TuiStartupSnapshotMetadata metadata;
    const auto rows = ParseTuiStartupSnapshotJson(
        payload,
        root,
        &error,
        &metadata);
    REQUIRE(error.empty());
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().path == externalRepo);
    REQUIRE(metadata.allowedExternalRoots.size() == 1);
    CHECK(metadata.allowedExternalRoots.front() == externalRoot);
}

TEST_CASE(
    "TUI startup snapshot parser rejects unconfigured external repository paths",
    "[unit][tui_startup_snapshot][KG-BUG-0091]") {
    const auto root = FixtureWorkspaceRoot();
    const auto externalRepo = FixtureAbsolutePath("external/repos/tool");
    std::string error;
    const auto rows = ParseTuiStartupSnapshotJson(
        ValidSnapshotWithRepos(
            "[" + RepoJson(externalRepo, "external-root") + "]"),
        root,
        &error);
    REQUIRE(rows.empty());
    REQUIRE(error.find("configured external roots") != std::string::npos);
}

TEST_CASE(
    "TUI root fallback is exact and never fabricates audit booleans",
    "[unit][tui_startup_snapshot][KG-BUG-0091]") {
    const auto root = FixtureWorkspaceRoot();
    const auto payload = RootFallbackPayload(root);

    std::string error;
    const auto rows = ParseTuiStartupSnapshotJson(
        payload,
        root,
        &error);
    REQUIRE(error.empty());
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows.front().statusKnown);
    CHECK(std::string(TuiAuditBooleanLabel(
              rows.front().statusKnown,
              rows.front().repoDirty)) == "unknown");

    auto wrongRoot = payload;
    const auto quotedRoot = QuoteJson(root.generic_string());
    const auto rootPosition = wrongRoot.find(quotedRoot);
    REQUIRE(rootPosition != std::string::npos);
    wrongRoot.replace(
        rootPosition,
        quotedRoot.size(),
        QuoteJson((root / "other").generic_string()));
    const auto rejected = ParseTuiStartupSnapshotJson(
        wrongRoot,
        root,
        &error);
    REQUIRE(rejected.empty());
    REQUIRE(error.find("exactly the requested root") != std::string::npos);
}
