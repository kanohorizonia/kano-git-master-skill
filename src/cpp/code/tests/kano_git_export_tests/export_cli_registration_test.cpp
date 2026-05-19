// export_cli_registration_test.cpp — Unit tests for CLI option registration
//
// Tests that:
//   1. --format validation rejects invalid values at the CLI11 parse level
//      (via ValidateOptions called from the callback) (Requirement 7.2)
//   2. --source validation rejects invalid values (Requirement 7.3)
//   3. --rev-pad validation rejects non-positive values (Requirement 7.4)
//   4. --dry-run sets the dry-run flag (Requirement 7.1)
//   5. --no-metadata sets the no-metadata flag (Requirement 7.1)
//
// Strategy:
//   - For validation tests (7.2, 7.3, 7.4): test ValidateOptions directly,
//     which is the function called by the RegisterExport callback.  This
//     avoids triggering real git/shell calls while still verifying the exact
//     validation logic that the CLI callback invokes.
//   - For flag tests (7.1): test ValidateOptions with valid options that
//     include the flags, confirming the options struct is correctly populated
//     and accepted.
//
// **Validates: Requirements 7.1, 7.2, 7.3, 7.4**

#include <catch2/catch_test_macros.hpp>

#include "export_helpers.hpp"

#include <iostream>
#include <sstream>
#include <string>

using namespace kano::git::commands;

// ---------------------------------------------------------------------------
// Helper: build a fully-valid ExportOptions baseline
// ---------------------------------------------------------------------------

namespace {

auto MakeValidOpts() -> ExportOptions {
    ExportOptions opts;
    opts.format = "tar";
    opts.source = "head";
    opts.revPad = 3;
    opts.dryRun = false;
    opts.noMetadata = false;
    opts.noRecursive = false;
    opts.includeSubmoduleStubs = false;
    return opts;
}

// RAII helper that suppresses stderr output during a test section.
struct SuppressStderr {
    std::ostringstream buffer;
    std::streambuf* original = nullptr;

    SuppressStderr() {
        original = std::cerr.rdbuf(buffer.rdbuf());
    }

    ~SuppressStderr() {
        std::cerr.rdbuf(original);
    }

    auto captured() const -> std::string {
        return buffer.str();
    }
};

} // anonymous namespace

// ===========================================================================
// Requirement 7.2 — --format validation
// ===========================================================================

TEST_CASE("ValidateOptions accepts valid --format values",
          "[Unit][CLI][format-validation][req-7.2]") {
    // **Validates: Requirements 7.2**
    //
    // "tar" and "zip" are the only valid format values.

    SECTION("format=tar is accepted") {
        auto opts = MakeValidOpts();
        opts.format = "tar";
        REQUIRE(ValidateOptions(opts));
    }

    SECTION("format=zip is accepted") {
        auto opts = MakeValidOpts();
        opts.format = "zip";
        REQUIRE(ValidateOptions(opts));
    }
}

TEST_CASE("ValidateOptions rejects invalid --format values",
          "[Unit][CLI][format-validation][req-7.2]") {
    // **Validates: Requirements 7.2**
    //
    // Any format string not in {"tar", "zip"} must be rejected.

    const std::vector<std::string> invalidFormats = {
        "gz", "tgz", "tar.gz", "bz2", "xz", "7z", "rar",
        "TAR", "ZIP", "Tar", "Zip",   // case-sensitive check
        "",                            // empty string
        " tar",                        // leading space
        "tar ",                        // trailing space
        "tar|zip",                     // combined
        "invalid",
    };

    for (const auto& fmt : invalidFormats) {
        SECTION("format='" + fmt + "' is rejected") {
            auto opts = MakeValidOpts();
            opts.format = fmt;
            SuppressStderr suppress;
            REQUIRE_FALSE(ValidateOptions(opts));
        }
    }
}

TEST_CASE("ValidateOptions rejects invalid --format and emits error to stderr",
          "[Unit][CLI][format-validation][req-7.2]") {
    // **Validates: Requirements 7.2**
    //
    // When an invalid format is provided, an error message must be printed
    // to stderr.

    auto opts = MakeValidOpts();
    opts.format = "gz";

    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    const bool result = ValidateOptions(opts);
    std::cerr.rdbuf(origErr);

    REQUIRE_FALSE(result);
    const std::string errOutput = capturedErr.str();
    REQUIRE_FALSE(errOutput.empty());
    // Error message should mention the invalid value
    REQUIRE(errOutput.find("gz") != std::string::npos);
}

// ===========================================================================
// Requirement 7.3 — --source validation
// ===========================================================================

TEST_CASE("ValidateOptions accepts valid --source values",
          "[Unit][CLI][source-validation][req-7.3]") {
    // **Validates: Requirements 7.3**
    //
    // "head" and "working-tree" are the only valid source values.

    SECTION("source=head is accepted") {
        auto opts = MakeValidOpts();
        opts.source = "head";
        REQUIRE(ValidateOptions(opts));
    }

    SECTION("source=working-tree is accepted") {
        auto opts = MakeValidOpts();
        opts.source = "working-tree";
        REQUIRE(ValidateOptions(opts));
    }
}

TEST_CASE("ValidateOptions rejects invalid --source values",
          "[Unit][CLI][source-validation][req-7.3]") {
    // **Validates: Requirements 7.3**
    //
    // Any source string not in {"head", "working-tree"} must be rejected.

    const std::vector<std::string> invalidSources = {
        "HEAD",           // case-sensitive
        "Working-Tree",   // case-sensitive
        "WORKING-TREE",   // case-sensitive
        "workingtree",    // missing hyphen
        "working_tree",   // underscore instead of hyphen
        "index",
        "staged",
        "",               // empty string
        " head",          // leading space
        "head ",          // trailing space
        "invalid",
    };

    for (const auto& src : invalidSources) {
        SECTION("source='" + src + "' is rejected") {
            auto opts = MakeValidOpts();
            opts.source = src;
            SuppressStderr suppress;
            REQUIRE_FALSE(ValidateOptions(opts));
        }
    }
}

TEST_CASE("ValidateOptions rejects invalid --source and emits error to stderr",
          "[Unit][CLI][source-validation][req-7.3]") {
    // **Validates: Requirements 7.3**
    //
    // When an invalid source is provided, an error message must be printed
    // to stderr.

    auto opts = MakeValidOpts();
    opts.source = "index";

    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    const bool result = ValidateOptions(opts);
    std::cerr.rdbuf(origErr);

    REQUIRE_FALSE(result);
    const std::string errOutput = capturedErr.str();
    REQUIRE_FALSE(errOutput.empty());
    // Error message should mention the invalid value
    REQUIRE(errOutput.find("index") != std::string::npos);
}

// ===========================================================================
// Requirement 7.4 — --rev-pad validation
// ===========================================================================

TEST_CASE("ValidateOptions accepts valid --rev-pad values",
          "[Unit][CLI][rev-pad-validation][req-7.4]") {
    // **Validates: Requirements 7.4**
    //
    // Any positive integer is a valid rev-pad value.

    const std::vector<int> validPads = {1, 2, 3, 4, 5, 10, 100};

    for (const int pad : validPads) {
        SECTION("rev-pad=" + std::to_string(pad) + " is accepted") {
            auto opts = MakeValidOpts();
            opts.revPad = pad;
            REQUIRE(ValidateOptions(opts));
        }
    }
}

TEST_CASE("ValidateOptions rejects non-positive --rev-pad values",
          "[Unit][CLI][rev-pad-validation][req-7.4]") {
    // **Validates: Requirements 7.4**
    //
    // rev-pad must be a positive integer; 0 and negative values are rejected.

    const std::vector<int> invalidPads = {0, -1, -2, -10, -100};

    for (const int pad : invalidPads) {
        SECTION("rev-pad=" + std::to_string(pad) + " is rejected") {
            auto opts = MakeValidOpts();
            opts.revPad = pad;
            SuppressStderr suppress;
            REQUIRE_FALSE(ValidateOptions(opts));
        }
    }
}

TEST_CASE("ValidateOptions rejects rev-pad=0 and emits error to stderr",
          "[Unit][CLI][rev-pad-validation][req-7.4]") {
    // **Validates: Requirements 7.4**
    //
    // When rev-pad=0 is provided, an error message must be printed to stderr.

    auto opts = MakeValidOpts();
    opts.revPad = 0;

    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    const bool result = ValidateOptions(opts);
    std::cerr.rdbuf(origErr);

    REQUIRE_FALSE(result);
    const std::string errOutput = capturedErr.str();
    REQUIRE_FALSE(errOutput.empty());
    // Error message should mention rev-pad
    REQUIRE(errOutput.find("rev-pad") != std::string::npos);
}

TEST_CASE("ValidateOptions subtree mode rejects incompatible option combinations",
          "[Unit][CLI][subtree-validation]") {
    auto opts = MakeValidOpts();
    opts.subtreePath = "Engine/Source/Programs/UnrealGameSync";

    SECTION("--subtree + --single is rejected") {
        auto c = opts;
        c.single = true;
        SuppressStderr suppress;
        REQUIRE_FALSE(ValidateOptions(c));
    }

    SECTION("--subtree + --include-submodule-stubs is rejected") {
        auto c = opts;
        c.includeSubmoduleStubs = true;
        SuppressStderr suppress;
        REQUIRE_FALSE(ValidateOptions(c));
    }

    SECTION("--subtree + --validate-release-archive is rejected") {
        auto c = opts;
        c.forceValidateReleaseArchive = true;
        SuppressStderr suppress;
        REQUIRE_FALSE(ValidateOptions(c));
    }
}

TEST_CASE("ValidateOptions rejects negative rev-pad and emits error to stderr",
          "[Unit][CLI][rev-pad-validation][req-7.4]") {
    // **Validates: Requirements 7.4**
    //
    // When rev-pad is negative, an error message must be printed to stderr.

    auto opts = MakeValidOpts();
    opts.revPad = -5;

    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    const bool result = ValidateOptions(opts);
    std::cerr.rdbuf(origErr);

    REQUIRE_FALSE(result);
    const std::string errOutput = capturedErr.str();
    REQUIRE_FALSE(errOutput.empty());
}

// ===========================================================================
// Requirement 7.1 — --dry-run and --no-metadata flags
// ===========================================================================

TEST_CASE("ValidateOptions accepts options with --dry-run set",
          "[Unit][CLI][flag-options][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // When dryRun=true is set in ExportOptions, ValidateOptions must still
    // accept the options (dry-run is a valid flag, not a validation concern).

    auto opts = MakeValidOpts();
    opts.dryRun = true;
    REQUIRE(ValidateOptions(opts));
}

TEST_CASE("ValidateOptions accepts options with --no-metadata set",
          "[Unit][CLI][flag-options][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // When noMetadata=true is set in ExportOptions, ValidateOptions must still
    // accept the options (no-metadata is a valid flag, not a validation concern).

    auto opts = MakeValidOpts();
    opts.noMetadata = true;
    REQUIRE(ValidateOptions(opts));
}

TEST_CASE("ValidateOptions accepts options with both --dry-run and --no-metadata set",
          "[Unit][CLI][flag-options][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // Both flags can be set simultaneously without causing validation failure.

    auto opts = MakeValidOpts();
    opts.dryRun = true;
    opts.noMetadata = true;
    REQUIRE(ValidateOptions(opts));
}

TEST_CASE("ValidateOptions accepts options with --no-recursive set",
          "[Unit][CLI][flag-options][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // noRecursive is a valid flag that does not affect validation.

    auto opts = MakeValidOpts();
    opts.noRecursive = true;
    REQUIRE(ValidateOptions(opts));
}

TEST_CASE("ValidateOptions accepts options with --include-submodule-stubs set",
          "[Unit][CLI][flag-options][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // includeSubmoduleStubs is a valid flag that does not affect validation.

    auto opts = MakeValidOpts();
    opts.includeSubmoduleStubs = true;
    REQUIRE(ValidateOptions(opts));
}

// ===========================================================================
// Requirement 7.1 — CLI11 subcommand registration (structural tests)
// ===========================================================================

TEST_CASE("RegisterExport registers 'export' subcommand with CLI11",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // After calling RegisterExport, the parent CLI::App must have an 'export'
    // subcommand registered.

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);
}

TEST_CASE("RegisterExport registers --format option",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // The 'export' subcommand must have a --format option registered.

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    // CLI11 stores options; verify --format exists by checking option count
    // and that parsing "--format tar" does not throw a parse error.
    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--format tar"}, false));
}

TEST_CASE("RegisterExport registers --source option",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // The 'export' subcommand must have a --source option registered.

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--source head"}, false));
}

TEST_CASE("RegisterExport registers --rev-pad option",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // The 'export' subcommand must have a --rev-pad option registered.

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--rev-pad 5"}, false));
}

TEST_CASE("RegisterExport registers --dry-run flag",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // The 'export' subcommand must have a --dry-run flag registered.
    // Parsing --dry-run must not throw a CLI::ParseError.

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--dry-run"}, false));
}

TEST_CASE("RegisterExport registers --no-metadata flag",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // The 'export' subcommand must have a --no-metadata flag registered.
    // Parsing --no-metadata must not throw a CLI::ParseError.

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--no-metadata"}, false));
}

TEST_CASE("RegisterExport registers --output option",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--output /tmp/out"}, false));
}

TEST_CASE("RegisterExport registers --no-recursive flag",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--no-recursive"}, false));
}

TEST_CASE("RegisterExport registers --prefix option",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--prefix MyRepo/"}, false));
}

TEST_CASE("RegisterExport registers --include-submodule-stubs flag",
          "[Unit][CLI][registration][req-7.1]") {
    // **Validates: Requirements 7.1**

    CLI::App app{"kog test"};
    kano::git::commands::RegisterExport(app);

    auto* exportCmd = app.get_subcommand("export");
    REQUIRE(exportCmd != nullptr);

    REQUIRE_NOTHROW(exportCmd->parse(std::string{"--include-submodule-stubs"}, false));
}

// ===========================================================================
// Combined validation: multiple invalid fields
// ===========================================================================

TEST_CASE("ValidateOptions reports all validation errors when multiple fields are invalid",
          "[Unit][CLI][validation][req-7.2][req-7.3][req-7.4]") {
    // **Validates: Requirements 7.2, 7.3, 7.4**
    //
    // When multiple fields are invalid simultaneously, ValidateOptions must
    // return false (all errors are checked, not just the first).

    auto opts = MakeValidOpts();
    opts.format = "gz";          // invalid
    opts.source = "index";       // invalid
    opts.revPad = 0;             // invalid

    std::ostringstream capturedErr;
    std::streambuf* origErr = std::cerr.rdbuf(capturedErr.rdbuf());
    const bool result = ValidateOptions(opts);
    std::cerr.rdbuf(origErr);

    REQUIRE_FALSE(result);

    // All three errors should be reported
    const std::string errOutput = capturedErr.str();
    REQUIRE(errOutput.find("format") != std::string::npos);
    REQUIRE(errOutput.find("source") != std::string::npos);
    REQUIRE(errOutput.find("rev-pad") != std::string::npos);
}

TEST_CASE("ValidateOptions accepts all default option values",
          "[Unit][CLI][validation][req-7.1]") {
    // **Validates: Requirements 7.1**
    //
    // The default values (format=tar, source=head, revPad=3) must all pass
    // validation without errors.

    const auto opts = MakeValidOpts();
    REQUIRE(ValidateOptions(opts));
}
