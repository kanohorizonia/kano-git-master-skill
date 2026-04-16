// Feature: plan-json-library-refactor, Property 1: Plan schema invariants
//
// Property 1: For any workspace root and datasource paths, the output of
// BuildDefaultPlanTemplate SHALL contain all required schema_version "3" fields
// with correct types and invariant values:
//   - meta.schema_version == "3"
//   - meta.plan_id        (non-empty string)
//   - meta.generated_at_utc (non-empty string)
//   - meta.executed_at_utc == ""
//   - meta.base_head_sha  (string)
//   - meta.dirty_fingerprint_pre_ignore (string)
//   - meta.dirty_fingerprint (string)
//   - meta.planner        (object with "provider" and "ai-model" string fields)
//   - meta.review         (object with "verdict" and "reason" string fields)
//   - meta.ignore_datasource.prefer_sources == ["kano-local-rules", "github-gitignore"]
//   - stages.ignore    == []
//   - stages.commit    == []
//   - stages.post_sync == []
//
// **Validates: Requirements 2.2, 4.1, 4.2, 4.3, 4.4**

#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>
#include <rapidcheck.h>
#include <rapidcheck/catch.h>

#include <filesystem>
#include <string>

using namespace kano::git::commands;

namespace {

// Verify all required schema_version "3" fields are present with correct types
// and invariant values.
void AssertPlanSchemaInvariants(const nlohmann::json& doc) {
    // meta must be an object
    RC_ASSERT(doc.contains("meta"));
    RC_ASSERT(doc["meta"].is_object());

    const auto& meta = doc["meta"];

    // schema_version must be "3"
    RC_ASSERT(meta.contains("schema_version"));
    RC_ASSERT(meta["schema_version"].is_string());
    RC_ASSERT(meta["schema_version"].get<std::string>() == "3");

    // plan_id must be a non-empty string
    RC_ASSERT(meta.contains("plan_id"));
    RC_ASSERT(meta["plan_id"].is_string());
    RC_ASSERT(!meta["plan_id"].get<std::string>().empty());

    // generated_at_utc must be a non-empty string
    RC_ASSERT(meta.contains("generated_at_utc"));
    RC_ASSERT(meta["generated_at_utc"].is_string());
    RC_ASSERT(!meta["generated_at_utc"].get<std::string>().empty());

    // executed_at_utc must be "" (Requirement 4.3)
    RC_ASSERT(meta.contains("executed_at_utc"));
    RC_ASSERT(meta["executed_at_utc"].is_string());
    RC_ASSERT(meta["executed_at_utc"].get<std::string>() == "");

    // base_head_sha must be a string
    RC_ASSERT(meta.contains("base_head_sha"));
    RC_ASSERT(meta["base_head_sha"].is_string());

    // dirty_fingerprint_pre_ignore must be a string
    RC_ASSERT(meta.contains("dirty_fingerprint_pre_ignore"));
    RC_ASSERT(meta["dirty_fingerprint_pre_ignore"].is_string());

    // dirty_fingerprint must be a string
    RC_ASSERT(meta.contains("dirty_fingerprint"));
    RC_ASSERT(meta["dirty_fingerprint"].is_string());

    // planner must be an object with "provider" and "ai-model" string fields
    RC_ASSERT(meta.contains("planner"));
    RC_ASSERT(meta["planner"].is_object());
    RC_ASSERT(meta["planner"].contains("provider"));
    RC_ASSERT(meta["planner"]["provider"].is_string());
    RC_ASSERT(meta["planner"].contains("ai-model"));
    RC_ASSERT(meta["planner"]["ai-model"].is_string());

    // review must be an object with "verdict" and "reason" string fields
    RC_ASSERT(meta.contains("review"));
    RC_ASSERT(meta["review"].is_object());
    RC_ASSERT(meta["review"].contains("verdict"));
    RC_ASSERT(meta["review"]["verdict"].is_string());
    RC_ASSERT(meta["review"].contains("reason"));
    RC_ASSERT(meta["review"]["reason"].is_string());

    // ignore_datasource must have prefer_sources == ["kano-local-rules", "github-gitignore"]
    // (Requirement 4.2)
    RC_ASSERT(meta.contains("ignore_datasource"));
    RC_ASSERT(meta["ignore_datasource"].is_object());
    RC_ASSERT(meta["ignore_datasource"].contains("prefer_sources"));
    const auto& ps = meta["ignore_datasource"]["prefer_sources"];
    RC_ASSERT(ps.is_array());
    RC_ASSERT(ps.size() == 2);
    RC_ASSERT(ps[0].get<std::string>() == "kano-local-rules");
    RC_ASSERT(ps[1].get<std::string>() == "github-gitignore");

    // stages must be an object (Requirement 4.4)
    RC_ASSERT(doc.contains("stages"));
    RC_ASSERT(doc["stages"].is_object());

    // stages.ignore must be [] (Requirement 4.4)
    RC_ASSERT(doc["stages"].contains("ignore"));
    RC_ASSERT(doc["stages"]["ignore"].is_array());
    RC_ASSERT(doc["stages"]["ignore"].empty());

    // stages.commit must be []
    RC_ASSERT(doc["stages"].contains("commit"));
    RC_ASSERT(doc["stages"]["commit"].is_array());
    RC_ASSERT(doc["stages"]["commit"].empty());

    // stages.post_sync must be [] (Requirement 4.4)
    RC_ASSERT(doc["stages"].contains("post_sync"));
    RC_ASSERT(doc["stages"]["post_sync"].is_array());
    RC_ASSERT(doc["stages"]["post_sync"].empty());
}

} // namespace

// ---------------------------------------------------------------------------
// Property 1: Plan schema invariants — fixed workspace root
// ---------------------------------------------------------------------------

TEST_CASE("Property 1: BuildDefaultPlanTemplate output satisfies schema_version 3 invariants",
          "[Feature: plan-json-library-refactor]"
          "[Property 1: Plan schema invariants]"
          "[property][BuildDefaultPlanTemplate][req-2.2][req-4.1][req-4.2][req-4.3][req-4.4]") {
    // **Validates: Requirements 2.2, 4.1, 4.2, 4.3, 4.4**
    //
    // For any workspace root path, BuildDefaultPlanTemplate SHALL produce a
    // JSON document that satisfies all schema_version "3" invariants.

    rc::prop("BuildDefaultPlanTemplate satisfies schema invariants", []() {
        // Use the temp directory as a stable workspace root to avoid
        // filesystem side-effects while still exercising the full code path.
        const auto output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());

        // Must be non-empty and parseable
        RC_ASSERT(!output.empty());
        nlohmann::json doc;
        bool parseOk = true;
        try {
            doc = nlohmann::json::parse(output);
        } catch (...) {
            parseOk = false;
        }
        RC_ASSERT(parseOk);

        // All schema invariants must hold
        AssertPlanSchemaInvariants(doc);
    });
}

// ---------------------------------------------------------------------------
// Property 1b: Schema invariants hold regardless of KOG_DEBUG_PLAN mode
// ---------------------------------------------------------------------------

TEST_CASE("Property 1: Schema invariants hold in both compact and pretty output modes",
          "[Feature: plan-json-library-refactor]"
          "[Property 1: Plan schema invariants]"
          "[property][BuildDefaultPlanTemplate][req-2.2][req-4.1]") {
    // **Validates: Requirements 2.2, 4.1**
    //
    // The schema invariants must hold regardless of whether compact or pretty
    // output is selected.

    // Compact mode
    {
#if defined(_WIN32)
        _putenv_s("KOG_DEBUG_PLAN", "");
#else
        unsetenv("KOG_DEBUG_PLAN");
#endif
        rc::prop("schema invariants hold in compact mode", []() {
            const auto output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());
            RC_ASSERT(!output.empty());
            const nlohmann::json doc = nlohmann::json::parse(output);
            AssertPlanSchemaInvariants(doc);
        });
    }

    // Pretty mode
    {
#if defined(_WIN32)
        _putenv_s("KOG_DEBUG_PLAN", "1");
#else
        setenv("KOG_DEBUG_PLAN", "1", 1);
#endif
        rc::prop("schema invariants hold in pretty mode", []() {
            const auto output = BuildDefaultPlanTemplate(std::filesystem::temp_directory_path());
            RC_ASSERT(!output.empty());
            const nlohmann::json doc = nlohmann::json::parse(output);
            AssertPlanSchemaInvariants(doc);
        });
#if defined(_WIN32)
        _putenv_s("KOG_DEBUG_PLAN", "");
#else
        unsetenv("KOG_DEBUG_PLAN");
#endif
    }
}
