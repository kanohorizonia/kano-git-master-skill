#include <catch2/catch_test_macros.hpp>

#include "audit_run_catalog.hpp"
#include "audit_run_catalog_private.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

using namespace kano::git::commands;

namespace {
class ScopedCatalogTestHooks {
public:
    explicit ScopedCatalogTestHooks(AuditRunCatalogTestHooks InHooks) {
        SetAuditRunCatalogTestHooks(std::move(InHooks));
    }
    ~ScopedCatalogTestHooks() { ResetAuditRunCatalogTestHooks(); }
    ScopedCatalogTestHooks(const ScopedCatalogTestHooks&) = delete;
    auto operator=(const ScopedCatalogTestHooks&) -> ScopedCatalogTestHooks& = delete;
};

auto Root() -> std::filesystem::path {
    static std::atomic_uint64_t sequence = 0;
    const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(sequence++);
    return std::filesystem::temp_directory_path() /
        ("kog-audit-catalog-" + kano::git::audit::Sha256Hex(nonce).substr(0, 20));
}
auto Write(const std::filesystem::path& InPath, const std::string& InBytes) -> void {
    std::ofstream output(InPath, std::ios::binary | std::ios::trunc); REQUIRE(output.good()); output << InBytes;
}
auto Spec(const std::filesystem::path& InRoot, const std::string& InRun, const std::uint32_t InAttempt) -> OperationAuditSpec {
    std::error_code ec;
    REQUIRE((std::filesystem::create_directories(InRoot, ec) || (!ec && std::filesystem::is_directory(InRoot))));
    const auto source = InRoot / "plan.json";
    const auto correlation = nlohmann::json({{"mode", "koa"}, {"product_id", "product"}, {"topic_id", "topic"}, {"item_id", "item"}, {"work_order_id", "work"}, {"request_id", "request"}, {"run_id", InRun}, {"parent_run_id", "parent"}, {"producer_id", "producer"}, {"route_id", "route"}, {"attempt", InAttempt}});
    const auto bytes = nlohmann::json({{"meta", {{"plan_id", "catalog-plan"}, {"correlation", correlation}}}}).dump() + '\n'; Write(source, bytes);
    OperationAuditSpec spec; spec.workspaceRoot = InRoot; spec.sourcePath = source;
    spec.inputIdentity = source.generic_string(); spec.inputKind = "commit-plan"; spec.route = "commit-push.plan";
    spec.planId = "catalog-plan"; spec.sourceBytes = bytes; spec.frozenBytes = bytes; spec.frozenFileName = "frozen-plan.json";
    spec.correlation.mode = "koa"; spec.correlation.productId = "product"; spec.correlation.topicId = "topic"; spec.correlation.itemId = "item"; spec.correlation.workOrderId = "work"; spec.correlation.requestId = "request"; spec.correlation.runId = InRun; spec.correlation.parentRunId = "parent"; spec.correlation.producerId = "producer"; spec.correlation.routeId = "route"; spec.correlation.attempt = InAttempt;
    return spec;
}
auto NowUtc() -> std::string {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32]{}; std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}
void Finalize(const OperationAuditSpec& InSpec) {
    std::string error; auto context = OperationAuditContext::Reserve(InSpec, &error); INFO(error); REQUIRE(context);
    const auto before = context->Capture(InSpec.workspaceRoot);
    REQUIRE(context->Append("catalog.test", InSpec.workspaceRoot, before, NowUtc(), 0, &error));
    REQUIRE(context->Finalize(0, &error));
}
auto CatalogRoot(const std::filesystem::path& InRoot) -> std::filesystem::path {
    return InRoot / ".kano" / "tmp" / "git" / "catalog-v1";
}
auto Read(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}
// Test-only publication of a fully formed immutable generation.  This is
// intentionally not a production hook: faults are exercised at the catalog
// trust boundary exactly as a restart reader sees them.
void ReplaceCurrentCatalog(const std::filesystem::path& InRoot,
                           const nlohmann::json& InCatalog) {
    const auto root = CatalogRoot(InRoot);
    const auto bytes = InCatalog.dump() + '\n';
    const auto generation = "generation-" + kano::git::audit::Sha256Hex(bytes);
    Write(root / (generation + ".json"), bytes);
    Write(root / "current.json", nlohmann::json({
        {"schemaName", "kog.auditRunCatalogPointer"}, {"schemaVersion", 1},
        {"generation", generation}, {"sha256", kano::git::audit::Sha256Hex(bytes)},
    }).dump() + '\n');
}
auto CurrentCatalog(const std::filesystem::path& InRoot) -> nlohmann::json {
    const auto root = CatalogRoot(InRoot);
    const auto pointer = nlohmann::json::parse(Read(root / "current.json"));
    return nlohmann::json::parse(Read(root / (pointer.at("generation").get<std::string>() + ".json")));
}
auto QueryLimits(const std::size_t InRows = 64) -> OperationAuditCatalogQueryLimits {
    OperationAuditCatalogQueryLimits limits;
    limits.maxRows = InRows;
    limits.maxBytes = 256U << 10U;
    limits.maxQueryTime = std::chrono::milliseconds(250);
    return limits;
}

class ScopedHeldCatalogOsWriterLock {
public:
    explicit ScopedHeldCatalogOsWriterLock(const std::filesystem::path& path) {
#if !defined(_WIN32)
        mHandle = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(mHandle >= 0);
        REQUIRE(::flock(mHandle, LOCK_EX | LOCK_NB) == 0);
#else
        mHandle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(mHandle != INVALID_HANDLE_VALUE);
        REQUIRE(LockFileEx(mHandle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &mOverlap) != FALSE);
#endif
    }
    ~ScopedHeldCatalogOsWriterLock() {
#if !defined(_WIN32)
        if (mHandle >= 0) { (void)::flock(mHandle, LOCK_UN); (void)::close(mHandle); }
#else
        if (mHandle != INVALID_HANDLE_VALUE) { (void)UnlockFileEx(mHandle, 0, MAXDWORD, MAXDWORD, &mOverlap); CloseHandle(mHandle); }
#endif
    }
    ScopedHeldCatalogOsWriterLock(const ScopedHeldCatalogOsWriterLock&) = delete;
    auto operator=(const ScopedHeldCatalogOsWriterLock&) -> ScopedHeldCatalogOsWriterLock& = delete;
private:
#if !defined(_WIN32)
    int mHandle = -1;
#else
    HANDLE mHandle = INVALID_HANDLE_VALUE;
    OVERLAPPED mOverlap{};
#endif
};
}

TEST_CASE("[KG-TSK-0135] audit catalog is discoverable but requires pinned revalidation", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-run", 1); Finalize(spec);
    const auto query = QueryOperationAuditCatalog(spec);
    REQUIRE(query.ready()); REQUIRE(query.rows.size() == 1); REQUIRE(query.rows.front().state == OperationAuditCatalogState::Final);
    const auto verified = RevalidateOperationAuditCatalogEntry(spec, spec, query.rows.front());
    INFO(verified.diagnostic); REQUIRE(verified.verified()); REQUIRE(verified.run); REQUIRE(verified.run->runId == "catalog-run");
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] no-Git null-source fallback publishes at the workspace catalog root", "[audit][catalog][KG-TSK-0135]") {
    // Root() is a fresh non-Git temporary workspace. Clearing sourcePath
    // exercises the explicit workspace audit fallback without touching PATH.
    const auto root = Root(); auto spec = Spec(root, "catalog-null-source", 1);
    spec.sourcePath.reset();
    std::string publicationFailure;
    AuditRunCatalogTestHooks hooks;
    hooks.publicationFailure = [&](const std::string_view diagnostic) {
        publicationFailure = diagnostic;
    };
    const ScopedCatalogTestHooks scopedHooks(std::move(hooks));
    Finalize(spec);
    std::string error; const auto paths = ResolveOperationAuditPaths(spec, spec.correlation.runId, spec.correlation.attempt, &error);
    INFO(error); REQUIRE(paths);
    REQUIRE(paths->auditRoot == root / ".kano" / "tmp" / "git" / "audit" /
        ("plan-" + kano::git::audit::Sha256Hex(spec.inputIdentity)));
    INFO(publicationFailure);
    REQUIRE(std::filesystem::is_regular_file(CatalogRoot(root) / "current.json"));
    const auto query = QueryOperationAuditCatalog(spec);
    INFO(query.diagnostic); REQUIRE(query.ready()); REQUIRE(query.rows.size() == 1);
    REQUIRE(query.rows.front().runId == "catalog-null-source");
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] source-adjacent prefix collisions remain fallback paths", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root();
    std::vector<OperationAuditSpec> specs;
    for (const auto& [fileName, runId] : {
             std::pair{"plan-foo.json", "catalog-prefix-plan"},
             std::pair{"operation-foo.json", "catalog-prefix-operation"},
         }) {
        auto spec = Spec(root, runId, 1);
        spec.sourcePath = root / fileName;
        spec.inputIdentity = spec.sourcePath->generic_string();
        Write(*spec.sourcePath, spec.sourceBytes);
        Finalize(spec);
        std::string error;
        const auto paths = ResolveOperationAuditPaths(
            spec, spec.correlation.runId, spec.correlation.attempt, &error);
        INFO(error); REQUIRE(paths);
        REQUIRE(paths->auditRoot == std::filesystem::path(spec.sourcePath->string() + ".audit"));
        specs.push_back(std::move(spec));
    }
    REQUIRE(std::filesystem::is_regular_file(CatalogRoot(root) / "current.json"));
    const auto query = QueryOperationAuditCatalog(specs.front());
    INFO(query.diagnostic); REQUIRE(query.ready()); REQUIRE(query.rows.size() == 2);
    REQUIRE(std::any_of(query.rows.begin(), query.rows.end(), [](const auto& row) {
        return row.runId == "catalog-prefix-plan";
    }));
    REQUIRE(std::any_of(query.rows.begin(), query.rows.end(), [](const auto& row) {
        return row.runId == "catalog-prefix-operation";
    }));
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog cursor binds its immutable generation and filter", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto first = Spec(root, "catalog-first", 1); Finalize(first);
    auto second = Spec(root, "catalog-second", 1); Finalize(second);
    OperationAuditCatalogQueryLimits limits; limits.maxRows = 1;
    const auto page = QueryOperationAuditCatalog(first, {}, std::nullopt, limits);
    REQUIRE(page.ready()); REQUIRE(page.cursor); REQUIRE(page.rows.size() == 1);
    const auto pinnedGeneration = page.generation;
    auto third = Spec(root, "catalog-third", 1); Finalize(third);
    const auto resumed = QueryOperationAuditCatalog(first, {}, *page.cursor, limits);
    REQUIRE(resumed.ready()); REQUIRE(resumed.generation == pinnedGeneration);
    REQUIRE(resumed.rows.size() == 1); REQUIRE(resumed.rows.front().runId == "catalog-first");
    OperationAuditCatalogFilter filter; filter.runId = "catalog-first";
    const auto rejected = QueryOperationAuditCatalog(first, filter, *page.cursor, limits);
    REQUIRE(rejected.code == OperationAuditCatalogQueryCode::InvalidCursor);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] corrupt catalog pointer fails closed without rows", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-corrupt", 1); Finalize(spec);
    const auto pointer = root / ".kano" / "tmp" / "git" / "catalog-v1" / "current.json";
    Write(pointer, "{not-json}\n");
    const auto result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog records pending incomplete and final lifecycle truth", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root();
    const auto pending = Spec(root, "catalog-pending", 1);
    std::string error;
    auto context = OperationAuditContext::Reserve(pending, &error);
    INFO(error); REQUIRE(context);
    auto result = QueryOperationAuditCatalog(pending);
    REQUIRE(result.ready()); REQUIRE(result.rows.size() == 1);
    REQUIRE(result.rows.front().state == OperationAuditCatalogState::Pending);
    REQUIRE(result.rows.front().repositoryIdentityHeadSha256.empty());
    const auto before = context->Capture(pending.workspaceRoot);
    REQUIRE_FALSE(context->Append("Catalog.Invalid", pending.workspaceRoot, before, NowUtc(), 0, &error));
    // An invalid semantic token deterministically poisons the event stream and
    // publishes the incomplete marker without a correctness sleep.
    context.reset();
    result = QueryOperationAuditCatalog(pending);
    REQUIRE(result.ready()); REQUIRE(result.rows.size() == 1);
    REQUIRE(result.rows.front().state == OperationAuditCatalogState::Incomplete);
    REQUIRE_FALSE(result.rows.front().outcome); REQUIRE_FALSE(result.rows.front().receiptSha256);
    REQUIRE(result.rows.front().finishedAtUtc);
    REQUIRE(result.rows.front().repositoryIdentityHeadSha256.empty());
    const auto final = Spec(root, "catalog-final", 1); Finalize(final);
    result = QueryOperationAuditCatalog(final);
    REQUIRE(result.ready());
    REQUIRE(std::any_of(result.rows.begin(), result.rows.end(), [](const auto& row) {
        return row.runId == "catalog-final" && row.state == OperationAuditCatalogState::Final &&
            row.receiptSha256 && row.outcome && row.finishedAtUtc &&
            row.repositoryIdentityHeadSha256.size() == 64;
    }));
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog truncated repository filter is data-less", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-repository-cap", 1);
    std::string error; auto context = OperationAuditContext::Reserve(spec, &error);
    INFO(error); REQUIRE(context);
    // Reserve already records the workspace repository. Sixty-five explicit
    // repositories exercise a requested member beyond the 64-item preview.
    for (std::uint32_t index = 0; index < 65; ++index) {
        const auto indexText = std::to_string(index);
        const auto repository = root / ("repository-" + std::string(3 - indexText.size(), '0') + indexText);
        std::error_code ec; REQUIRE(std::filesystem::create_directories(repository, ec)); REQUIRE_FALSE(ec);
        const auto before = context->Capture(repository);
        REQUIRE(context->Append("catalog.repository", repository, before, NowUtc(), 0, &error));
    }
    REQUIRE(context->Finalize(0, &error));
    const auto query = QueryOperationAuditCatalog(spec);
    INFO(query.diagnostic); REQUIRE(query.ready());
    const auto row = std::find_if(query.rows.begin(), query.rows.end(), [](const auto& value) {
        return value.runId == "catalog-repository-cap";
    });
    REQUIRE(row != query.rows.end()); REQUIRE(row->repositories.size() == 64);
    REQUIRE(row->truncation.omittedRepositories == 2);
    REQUIRE(row->repositoryIdentityHeadSha256.size() == 64);
    REQUIRE(row->repositories.front().repositoryId == "repository-000");
    REQUIRE(row->repositories.back().repositoryId == "repository-063");
    auto knownRepositoryFilter = OperationAuditCatalogFilter{};
    knownRepositoryFilter.repositoryId = "repository-000";
    const auto knownRepository = QueryOperationAuditCatalog(spec, knownRepositoryFilter);
    REQUIRE(knownRepository.ready()); REQUIRE(knownRepository.rows.size() == 1);
    auto omittedRepositoryFilter = OperationAuditCatalogFilter{};
    omittedRepositoryFilter.repositoryId = "repository-064";
    const auto omittedRepository = QueryOperationAuditCatalog(spec, omittedRepositoryFilter);
    REQUIRE(omittedRepository.code == OperationAuditCatalogQueryCode::Unsupported);
    REQUIRE(omittedRepository.rows.empty()); REQUIRE_FALSE(omittedRepository.cursor);
    auto unrelatedFilter = omittedRepositoryFilter;
    unrelatedFilter.runId = "catalog-other-run";
    const auto unrelated = QueryOperationAuditCatalog(spec, unrelatedFilter);
    REQUIRE(unrelated.ready()); REQUIRE(unrelated.rows.empty()); REQUIRE_FALSE(unrelated.cursor);
    const auto verified = RevalidateOperationAuditCatalogEntry(spec, spec, *row);
    INFO(verified.diagnostic); REQUIRE(verified.verified()); REQUIRE(verified.run);
    const auto bindingMismatch = [&](const auto& mutate) {
        auto tampered = *row; mutate(tampered);
        const auto rejected = RevalidateOperationAuditCatalogEntry(spec, spec, tampered);
        INFO(rejected.diagnostic); REQUIRE_FALSE(rejected.verified()); REQUIRE_FALSE(rejected.run);
        REQUIRE(rejected.code == OperationAuditRunReadCode::BindingMismatch);
    };
    bindingMismatch([](auto& value) { value.repositories.front().repositoryId = "repository-tampered"; });
    bindingMismatch([](auto& value) { value.repositories.front().afterHeadSha = std::string(40, 'a'); });
    bindingMismatch([](auto& value) { value.repositoryIdentityHeadSha256 = std::string(64, 'a'); });
    bindingMismatch([](auto& value) { value.finishedAtUtc = "1970-01-01T00:00:00Z"; });
    bindingMismatch([](auto& value) { ++value.truncation.omittedEvents; });
    bindingMismatch([](auto& value) { ++value.truncation.omittedEvidence; });
    bindingMismatch([](auto& value) { ++value.redaction.redacted; });
    bindingMismatch([](auto& value) { ++value.redaction.withheld; });
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog rejects duplicate, conflicting, torn and unsupported data without a partial result", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-closed", 1); Finalize(spec);
    auto catalog = CurrentCatalog(root);
    catalog["entries"].push_back(catalog["entries"].front());
    ReplaceCurrentCatalog(root, catalog);
    auto result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty());
    catalog = CurrentCatalog(root);
    catalog["schemaVersion"] = 2;
    ReplaceCurrentCatalog(root, catalog);
    result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty());
    Write(CatalogRoot(root) / "current.json", "{\"schemaName\":\"kog.auditRunCatalogPointer\"}\n");
    result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty());
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] cursor rejects content hash and offset tampering and returns no data", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto first = Spec(root, "catalog-cursor-a", 1); Finalize(first);
    Finalize(Spec(root, "catalog-cursor-b", 1));
    const auto page = QueryOperationAuditCatalog(first, {}, std::nullopt, QueryLimits(1));
    REQUIRE(page.ready()); REQUIRE(page.cursor);
    auto cursor = nlohmann::json::parse(*page.cursor);
    cursor["generationSha256"] = std::string(64, '0');
    auto result = QueryOperationAuditCatalog(first, {}, cursor.dump(), QueryLimits(1));
    REQUIRE(result.code == OperationAuditCatalogQueryCode::InvalidCursor); REQUIRE(result.rows.empty());
    cursor = nlohmann::json::parse(*page.cursor); cursor["offset"] = 9999;
    result = QueryOperationAuditCatalog(first, {}, cursor.dump(), QueryLimits(1));
    REQUIRE(result.code == OperationAuditCatalogQueryCode::InvalidCursor); REQUIRE(result.rows.empty());
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] numeric schema and cursor fields reject overflow wrapping and fractions", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto first = Spec(root, "catalog-numeric-first", 1); Finalize(first);
    Finalize(Spec(root, "catalog-numeric-second", 1));
    const auto pointerPath = CatalogRoot(root) / "current.json";
    const auto originalPointer = nlohmann::json::parse(Read(pointerPath));
    const auto originalCatalog = CurrentCatalog(root);
    const std::vector<nlohmann::json> invalidVersions = {
        nlohmann::json(std::uint64_t{4294967296ULL}),
        // A narrowing uint32 conversion would wrap this value to version 1.
        nlohmann::json(std::int64_t{-4294967295LL}),
        nlohmann::json(1.5),
    };
    for (const auto& invalidVersion : invalidVersions) {
        auto pointer = originalPointer; pointer["schemaVersion"] = invalidVersion;
        Write(pointerPath, pointer.dump() + '\n');
        const auto rejected = QueryOperationAuditCatalog(first);
        REQUIRE(rejected.code == OperationAuditCatalogQueryCode::Corrupt);
        REQUIRE(rejected.rows.empty()); REQUIRE_FALSE(rejected.cursor);
    }
    Write(pointerPath, originalPointer.dump() + '\n');
    for (const auto& invalidVersion : invalidVersions) {
        auto catalog = originalCatalog; catalog["schemaVersion"] = invalidVersion;
        ReplaceCurrentCatalog(root, catalog);
        const auto rejected = QueryOperationAuditCatalog(first);
        REQUIRE(rejected.code == OperationAuditCatalogQueryCode::Corrupt);
        REQUIRE(rejected.rows.empty()); REQUIRE_FALSE(rejected.cursor);
    }
    ReplaceCurrentCatalog(root, originalCatalog);

    const auto page = QueryOperationAuditCatalog(first, {}, std::nullopt, QueryLimits(1));
    REQUIRE(page.ready()); REQUIRE(page.cursor);
    const auto originalCursor = nlohmann::json::parse(*page.cursor);
    const auto bindCursor = [](nlohmann::json* cursor, const std::string& issued, const std::string& expires, const std::string& offset) {
        (*cursor)["integrity"] = kano::git::audit::Sha256Hex(
            cursor->at("generation").get<std::string>() + "\n" +
            cursor->at("generationSha256").get<std::string>() + "\n" +
            cursor->at("filter").get<std::string>() + "\n" + issued + "\n" + expires + "\n" + offset);
    };
    const auto requireInvalidCursor = [&](const nlohmann::json& cursor) {
        const auto rejected = QueryOperationAuditCatalog(first, {}, cursor.dump(), QueryLimits(1));
        REQUIRE(rejected.code == OperationAuditCatalogQueryCode::InvalidCursor);
        REQUIRE(rejected.rows.empty()); REQUIRE_FALSE(rejected.cursor);
    };
    {
        auto legacy = originalCursor;
        legacy.erase("expiresAtEpoch"); legacy["schemaVersion"] = 1;
        legacy["integrity"] = kano::git::audit::Sha256Hex(
            legacy.at("generation").get<std::string>() + "\n" +
            legacy.at("generationSha256").get<std::string>() + "\n" +
            legacy.at("filter").get<std::string>() + "\n" +
            std::to_string(legacy.at("issuedAtEpoch").get<std::int64_t>()) + "\n" +
            std::to_string(legacy.at("offset").get<std::uint64_t>()));
        requireInvalidCursor(legacy);
    }
    for (const auto& invalidVersion : invalidVersions) {
        auto cursor = originalCursor; cursor["schemaVersion"] = invalidVersion;
        // schemaVersion is outside the cursor digest, so this retains an
        // otherwise valid integrity binding and isolates numeric admission.
        requireInvalidCursor(cursor);
    }
    {
        auto cursor = originalCursor; cursor["issuedAtEpoch"] = 1.5;
        bindCursor(&cursor, "1", std::to_string(cursor.at("expiresAtEpoch").get<std::int64_t>()), std::to_string(cursor.at("offset").get<std::uint64_t>()));
        requireInvalidCursor(cursor);
    }
    {
        auto cursor = originalCursor;
        cursor["issuedAtEpoch"] = std::uint64_t{9223372036854775808ULL};
        bindCursor(&cursor, "-9223372036854775808", std::to_string(cursor.at("expiresAtEpoch").get<std::int64_t>()), std::to_string(cursor.at("offset").get<std::uint64_t>()));
        requireInvalidCursor(cursor);
    }
    {
        auto cursor = originalCursor; cursor["issuedAtEpoch"] = std::int64_t{-1};
        bindCursor(&cursor, "-1", std::to_string(cursor.at("expiresAtEpoch").get<std::int64_t>()), std::to_string(cursor.at("offset").get<std::uint64_t>()));
        requireInvalidCursor(cursor);
    }
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        const auto oversizedOffset =
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) + std::uint64_t{1};
        auto cursor = originalCursor; cursor["offset"] = oversizedOffset;
        bindCursor(&cursor, std::to_string(cursor.at("issuedAtEpoch").get<std::int64_t>()), std::to_string(cursor.at("expiresAtEpoch").get<std::int64_t>()),
                   std::to_string(oversizedOffset));
        requireInvalidCursor(cursor);
    }
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] cursor expiry and unknown catalog fields fail closed without clock sleeps", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto first = Spec(root, "catalog-expiry-a", 1); Finalize(first);
    Finalize(Spec(root, "catalog-expiry-b", 1));
    auto limits = QueryLimits(1); limits.cursorLifetime = std::chrono::seconds(1);
    auto clock = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    ScopedCatalogTestHooks hooks({.systemNow = [&clock] { return clock; }});
    const auto page = QueryOperationAuditCatalog(first, {}, std::nullopt, limits);
    REQUIRE(page.ready()); REQUIRE(page.cursor);
    clock += std::chrono::seconds(2);
    auto cursor = nlohmann::json::parse(*page.cursor);
    auto result = QueryOperationAuditCatalog(first, {}, cursor.dump(), limits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::ExpiredCursor); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    // This cursor remains structurally valid and has a recomputed integrity
    // binding; it is expired solely because its original issue time is old.
    cursor = nlohmann::json::parse(*page.cursor);
    cursor["issuedAtEpoch"] = std::int64_t{998};
    cursor["expiresAtEpoch"] = std::int64_t{999};
    cursor["integrity"] = kano::git::audit::Sha256Hex(
        cursor.at("generation").get<std::string>() + "\n" +
        cursor.at("generationSha256").get<std::string>() + "\n" +
        cursor.at("filter").get<std::string>() + "\n998\n999\n" +
        std::to_string(cursor.at("offset").get<std::uint64_t>()));
    result = QueryOperationAuditCatalog(first, {}, cursor.dump(), limits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::ExpiredCursor); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    // Cursor expiry is issued once and bound into its integrity. A later
    // caller with a larger requested lifetime cannot resurrect it.
    auto longerLimits = QueryLimits(1); longerLimits.cursorLifetime = std::chrono::minutes(10);
    result = QueryOperationAuditCatalog(first, {}, std::string_view(*page.cursor), longerLimits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::ExpiredCursor); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    auto catalog = CurrentCatalog(root); catalog["entries"].front()["unexpected"] = true;
    ReplaceCurrentCatalog(root, catalog);
    result = QueryOperationAuditCatalog(first);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] missing and link-like catalog pointers cannot disclose catalog rows", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-nonregular", 1); Finalize(spec);
    const auto catalogRoot = CatalogRoot(root); std::error_code ec;
    std::filesystem::remove(catalogRoot / "current.json", ec); REQUIRE_FALSE(ec);
    auto result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Missing); REQUIRE(result.rows.empty());
#if !defined(_WIN32)
    const auto target = catalogRoot / "pointer-target.json";
    Write(target, "{}\n");
    std::filesystem::create_symlink(target.filename(), catalogRoot / "current.json", ec);
    REQUIRE_FALSE(ec);
    result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
#endif
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] selected rows bind the expected receipt identity before returning verified evidence", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-pinned", 1); Finalize(spec);
    const auto listed = QueryOperationAuditCatalog(spec); REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 1);
    auto substituted = listed.rows.front(); substituted.receiptSha256 = std::string(64, 'a');
    const auto result = RevalidateOperationAuditCatalogEntry(spec, spec, substituted);
    REQUIRE_FALSE(result.verified()); REQUIRE_FALSE(result.run);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog selection requires matching caller supplied source bytes", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-selected-spec", 1); Finalize(spec);
    const auto listed = QueryOperationAuditCatalog(spec); REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 1);
    auto selected = spec; selected.sourceBytes += "tampered";
    const auto mismatch = RevalidateOperationAuditCatalogEntry(spec, selected, listed.rows.front());
    REQUIRE_FALSE(mismatch.verified()); REQUIRE_FALSE(mismatch.run);
    REQUIRE(mismatch.code == OperationAuditRunReadCode::BindingMismatch);
    const auto matching = RevalidateOperationAuditCatalogEntry(spec, spec, listed.rows.front());
    INFO(matching.diagnostic); REQUIRE(matching.verified()); REQUIRE(matching.run);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] audit-root selector is a confined kind-bound token, never a path", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-selector", 1); Finalize(spec);
    for (const auto& selector : {"../plan-" + std::string(64, 'a'), "/plan-" + std::string(64, 'a'),
                                 "plan-" + std::string(63, 'a') + "/", "operation-" + std::string(64, 'a')}) {
        auto catalog = CurrentCatalog(root);
        catalog["entries"].front()["auditRootSelector"] = selector;
        ReplaceCurrentCatalog(root, catalog);
        const auto result = QueryOperationAuditCatalog(spec);
        REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt);
        REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    }
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] typed filters and caps have closed data-less outcomes", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-filter", 1); Finalize(spec);
    const auto listed = QueryOperationAuditCatalog(spec); REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 1);
    OperationAuditCatalogFilter filter;
    filter.state = OperationAuditCatalogState::Final;
    filter.outcome = kano::git::audit::OutcomeState::Succeeded;
    filter.correlationSha256 = listed.rows.front().correlationSha256;
    filter.observedNotBeforeUtc = "1970-01-01T00:00:00Z";
    filter.observedBeforeUtc = "9999-01-01T00:00:00Z";
    auto result = QueryOperationAuditCatalog(spec, filter);
    REQUIRE(result.ready()); REQUIRE(result.rows.size() == 1);
    auto limits = QueryLimits(); limits.maxRows = 0;
    result = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::InvalidConfiguration); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    limits = QueryLimits(); limits.maxBytes = 1;
    result = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Limit); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    limits = QueryLimits(); limits.cursorLifetime = std::chrono::seconds::zero();
    result = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::InvalidConfiguration); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    limits = QueryLimits(); limits.cursorLifetime = std::chrono::minutes(10) + std::chrono::seconds(1);
    result = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::InvalidConfiguration); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog complete repository absence remains empty", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-complete-repository-filter", 1); Finalize(spec);
    OperationAuditCatalogFilter filter; filter.repositoryId = "repository-not-present";
    const auto result = QueryOperationAuditCatalog(spec, filter);
    REQUIRE(result.ready()); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog rejects malformed nested contracts and lifecycle combinations", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-closed-nested", 1); Finalize(spec);
    const auto original = CurrentCatalog(root);
    const auto corrupt = [&](const auto& mutate) {
        auto catalog = original; mutate(catalog); ReplaceCurrentCatalog(root, catalog);
        return QueryOperationAuditCatalog(spec);
    };
    const auto badPlanHash = corrupt([](auto& catalog) { catalog["entries"][0]["planSha256"] = std::string(64, 'A'); });
    REQUIRE(badPlanHash.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(badPlanHash.rows.empty());
    const auto badUtc = corrupt([](auto& catalog) { catalog["entries"][0]["observedAtUtc"] = "2026-99-99T25:61:61Z"; });
    REQUIRE(badUtc.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(badUtc.rows.empty());
    const auto badRoute = corrupt([](auto& catalog) { catalog["entries"][0]["route"] = "not.a.catalog.route"; });
    REQUIRE(badRoute.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(badRoute.rows.empty());
    const auto badSelector = corrupt([](auto& catalog) { catalog["entries"][0]["auditRootSelector"] = "operation-" + std::string(64, 'a'); });
    REQUIRE(badSelector.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(badSelector.rows.empty());
    const auto nonTerminal = corrupt([](auto& catalog) {
        catalog["entries"][0]["state"] = "incomplete";
        catalog["entries"][0]["outcome"] = "failed";
        catalog["entries"][0]["receiptSha256"] = nullptr;
        catalog["entries"][0]["finishedAtUtc"] = "2026-01-01T00:00:00Z";
        catalog["entries"][0]["repositoryIdentityHeadSha256"] = "";
    });
    REQUIRE(nonTerminal.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(nonTerminal.rows.empty());
    const auto incompleteWithoutTime = corrupt([](auto& catalog) {
        catalog["entries"][0]["state"] = "incomplete";
        catalog["entries"][0]["outcome"] = nullptr;
        catalog["entries"][0]["receiptSha256"] = nullptr;
        catalog["entries"][0]["finishedAtUtc"] = nullptr;
        catalog["entries"][0]["repositoryIdentityHeadSha256"] = "";
    });
    REQUIRE(incompleteWithoutTime.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(incompleteWithoutTime.rows.empty());
    const auto pendingWithTime = corrupt([](auto& catalog) {
        catalog["entries"][0]["state"] = "pending";
        catalog["entries"][0]["outcome"] = nullptr;
        catalog["entries"][0]["receiptSha256"] = nullptr;
        catalog["entries"][0]["finishedAtUtc"] = "2026-01-01T00:00:00Z";
        catalog["entries"][0]["repositoryIdentityHeadSha256"] = "";
    });
    REQUIRE(pendingWithTime.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(pendingWithTime.rows.empty());
    const auto nestedUnknown = corrupt([](auto& catalog) {
        catalog["entries"][0]["repositories"] = nlohmann::json::array({{{"repositoryId", "repo"}, {"afterHeadSha", std::string(40, 'a')}, {"unknown", true}}});
    });
    REQUIRE(nestedUnknown.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(nestedUnknown.rows.empty());
    const auto duplicateRepository = corrupt([](auto& catalog) {
        const auto repo = nlohmann::json{{"repositoryId", "repo"}, {"afterHeadSha", std::string(40, 'a')}};
        catalog["entries"][0]["repositories"] = nlohmann::json::array({repo, repo});
    });
    REQUIRE(duplicateRepository.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(duplicateRepository.rows.empty());
    const auto invalidGitObjectId = corrupt([](auto& catalog) {
        catalog["entries"][0]["repositories"] = nlohmann::json::array({{{"repositoryId", "repo"}, {"afterHeadSha", std::string(40, 'G')}}});
    });
    REQUIRE(invalidGitObjectId.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(invalidGitObjectId.rows.empty());
    constexpr std::uint64_t kUint32Overflow = 4294967296ULL;
    const auto attemptOverflow = corrupt([&](auto& catalog) {
        catalog["entries"][0]["attempt"] = kUint32Overflow;
    });
    REQUIRE(attemptOverflow.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(attemptOverflow.rows.empty());
    for (const std::string& pointerText : {
             "/entries/0/redaction/redacted",
             "/entries/0/redaction/withheld",
             "/entries/0/truncation/omittedEvents",
             "/entries/0/truncation/omittedRepositories",
             "/entries/0/truncation/omittedEvidence",
         }) {
        const auto countOverflow = corrupt([&](auto& catalog) {
            catalog[nlohmann::json::json_pointer(pointerText)] = kUint32Overflow;
        });
        INFO(pointerText);
        REQUIRE(countOverflow.code == OperationAuditCatalogQueryCode::Corrupt);
        REQUIRE(countOverflow.rows.empty()); REQUIRE_FALSE(countOverflow.cursor);
    }
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog byte and deadline limits honor exact boundaries without sleeps", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-boundaries", 1); Finalize(spec);
    const auto pointer = nlohmann::json::parse(Read(CatalogRoot(root) / "current.json"));
    const auto catalog = CurrentCatalog(root);
    const auto responseBytes = nlohmann::json({
        {"generation", pointer.at("generation")}, {"cursor", nullptr}, {"diagnostic", ""},
        {"rows", catalog.at("entries")},
    }).dump().size();
    auto limits = QueryLimits(); limits.maxBytes = responseBytes;
    auto exactBytes = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
    REQUIRE(exactBytes.ready()); REQUIRE(exactBytes.rows.size() == 1);
    limits.maxBytes = responseBytes - 1;
    auto oneByteShort = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
    REQUIRE(oneByteShort.code == OperationAuditCatalogQueryCode::Limit); REQUIRE(oneByteShort.rows.empty()); REQUIRE_FALSE(oneByteShort.cursor);

    const auto epoch = std::chrono::steady_clock::time_point{};
    auto now = epoch;
    int clockCalls = 0;
    limits = QueryLimits(); limits.maxQueryTime = std::chrono::milliseconds(1);
    {
        ScopedCatalogTestHooks hooks({.monotonicNow = [&] { return clockCalls++ == 0 ? epoch : now; }});
        now = epoch + limits.maxQueryTime;
        clockCalls = 0;
        const auto exactDeadline = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
        REQUIRE(exactDeadline.ready()); REQUIRE(exactDeadline.rows.size() == 1);
        now = epoch + limits.maxQueryTime + std::chrono::nanoseconds(1);
        clockCalls = 0;
        const auto expiredDeadline = QueryOperationAuditCatalog(spec, {}, std::nullopt, limits);
        REQUIRE(expiredDeadline.code == OperationAuditCatalogQueryCode::Limit); REQUIRE(expiredDeadline.rows.empty()); REQUIRE_FALSE(expiredDeadline.cursor);
    }
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog publication fault stages are restart-safe", "[audit][catalog][KG-TSK-0135]") {
    for (const std::string_view stage : {"before-generation", "before-pointer", "after-pointer"}) {
        const auto root = Root(); const auto spec = Spec(root, "catalog-fault-" + std::string(stage), 1);
        std::vector<std::string> stages;
        std::unique_ptr<OperationAuditContext> context;
        {
            ScopedCatalogTestHooks hooks({
                .publicationStage = [&](const std::string_view observed) { stages.emplace_back(observed); },
                .failStage = [stage](const std::string_view observed) { return observed == stage; },
            });
            std::string error;
            context = OperationAuditContext::Reserve(spec, &error);
            INFO(error); REQUIRE(context);
        }
        REQUIRE(std::find(stages.begin(), stages.end(), stage) != stages.end());
        const auto restart = QueryOperationAuditCatalog(spec);
        if (stage == "after-pointer") {
            REQUIRE(restart.ready()); REQUIRE(restart.rows.size() == 1);
            std::string error; const auto paths = ResolveOperationAuditPaths(spec, spec.correlation.runId, spec.correlation.attempt, &error);
            INFO(error); REQUIRE(paths);
            // The injected error is truthful: the pointer may already be
            // durable. Retrying its exact row is consequently idempotent.
            REQUIRE(PublishOperationAuditCatalogEntry(spec, *paths, restart.rows.front(), &error));
        } else {
            REQUIRE(restart.code == OperationAuditCatalogQueryCode::Missing); REQUIRE(restart.rows.empty());
        }
        context.reset();
        std::error_code ec; std::filesystem::remove_all(root, ec);
    }
}

TEST_CASE("[KG-TSK-0135] publisher makes exact replay idempotent and rejects receipt conflicts", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-replay", 1); Finalize(spec);
    const auto listed = QueryOperationAuditCatalog(spec); REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 1);
    std::string pathError; const auto paths = ResolveOperationAuditPaths(spec, spec.correlation.runId, spec.correlation.attempt, &pathError);
    INFO(pathError); REQUIRE(paths);
    std::string error;
    REQUIRE(PublishOperationAuditCatalogEntry(spec, *paths, listed.rows.front(), &error));
    const auto replay = QueryOperationAuditCatalog(spec);
    REQUIRE(replay.ready()); REQUIRE(replay.rows.size() == 1);
    auto conflict = listed.rows.front(); conflict.receiptSha256 = std::string(64, 'a');
    REQUIRE_FALSE(PublishOperationAuditCatalogEntry(spec, *paths, std::move(conflict), &error));
    const auto stillOriginal = QueryOperationAuditCatalog(spec);
    REQUIRE(stillOriginal.ready()); REQUIRE(stillOriginal.rows.size() == 1);
    REQUIRE(stillOriginal.rows.front().receiptSha256 == listed.rows.front().receiptSha256);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] competing writers reject busy publication and caller retry retains both rows", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto first = Spec(root, "catalog-concurrent-first", 1); Finalize(first);
    const auto second = Spec(root, "catalog-concurrent-second", 1); Finalize(second);
    const auto seeded = QueryOperationAuditCatalog(first); REQUIRE(seeded.ready()); REQUIRE(seeded.rows.size() == 2);
    const auto findRow = [&](const std::string_view runId) -> OperationAuditCatalogEntry {
        const auto it = std::find_if(seeded.rows.begin(), seeded.rows.end(), [&](const auto& row) { return row.runId == runId; });
        REQUIRE(it != seeded.rows.end()); return *it;
    };
    const auto firstRow = findRow("catalog-concurrent-first");
    const auto secondRow = findRow("catalog-concurrent-second");
    std::string error; const auto firstPaths = ResolveOperationAuditPaths(first, first.correlation.runId, first.correlation.attempt, &error);
    INFO(error); REQUIRE(firstPaths);
    std::error_code ec; std::filesystem::remove_all(CatalogRoot(root), ec); REQUIRE_FALSE(ec);
    REQUIRE_FALSE(std::filesystem::exists(CatalogRoot(root), ec)); REQUIRE_FALSE(ec);

    std::mutex mutex; std::condition_variable condition;
    bool firstWriterLocked = false; bool releaseFirstWriter = false;
    std::atomic_uint32_t barrierCalls = 0;
    bool firstPublished = false; bool secondPublished = false;
    std::string firstPublishError; std::string secondPublishError;
    {
        ScopedCatalogTestHooks hooks({.writerBarrier = [&] {
            if (barrierCalls.fetch_add(1) != 0) return;
            std::unique_lock lock(mutex); firstWriterLocked = true; condition.notify_all();
            condition.wait(lock, [&] { return releaseFirstWriter; });
        }});
        std::thread firstWriter([&] {
            firstPublished = PublishOperationAuditCatalogEntry(
                first, *firstPaths, firstRow, &firstPublishError);
        });
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return firstWriterLocked; });
        }

        // Do not release the first writer until the competing publication has
        // actually returned busy. Synchronizing on call completion (rather than
        // pre-call intent) makes the contention proof independent of scheduling.
        std::thread secondWriter([&] {
            secondPublished = PublishOperationAuditCatalogEntry(
                second, *firstPaths, secondRow, &secondPublishError);
        });
        secondWriter.join();
        {
            std::lock_guard lock(mutex);
            releaseFirstWriter = true;
        }
        condition.notify_all();
        firstWriter.join();
    }
    // A held writer lock must reject the competing best-effort publisher
    // immediately; this deterministic barrier needs no timing assertion.
    INFO(firstPublishError); REQUIRE(firstPublished);
    REQUIRE_FALSE(secondPublished);
    INFO(secondPublishError); REQUIRE(secondPublishError == "audit catalog writer is busy");
    REQUIRE(PublishOperationAuditCatalogEntry(second, *firstPaths, secondRow, &secondPublishError));
    const auto listed = QueryOperationAuditCatalog(first);
    REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 2);
    REQUIRE(std::any_of(listed.rows.begin(), listed.rows.end(), [](const auto& row) { return row.runId == "catalog-concurrent-first"; }));
    REQUIRE(std::any_of(listed.rows.begin(), listed.rows.end(), [](const auto& row) { return row.runId == "catalog-concurrent-second"; }));
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog held OS writer lock rejects publication without blocking", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-held-os-lock", 1); Finalize(spec);
    const auto listed = QueryOperationAuditCatalog(spec); REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 1);
    std::string error; const auto paths = ResolveOperationAuditPaths(spec, spec.correlation.runId, spec.correlation.attempt, &error);
    INFO(error); REQUIRE(paths);
    {
        ScopedHeldCatalogOsWriterLock lock(CatalogRoot(root) / "writer.lock");
        REQUIRE_FALSE(PublishOperationAuditCatalogEntry(spec, *paths, listed.rows.front(), &error));
        REQUIRE(error == "audit catalog writer is busy");
    }
    REQUIRE(PublishOperationAuditCatalogEntry(spec, *paths, listed.rows.front(), &error));
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] held OS writer lock leaves Reserve nonblocking and Finalize reconciles", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto seed = Spec(root, "catalog-held-os-lock-seed", 1); Finalize(seed);
    const auto contended = Spec(root, "catalog-held-os-lock-reserve", 1);
    std::string publicationFailure;
    AuditRunCatalogTestHooks hooks;
    hooks.publicationFailure = [&](const std::string_view diagnostic) {
        publicationFailure = diagnostic;
    };
    const ScopedCatalogTestHooks scopedHooks(std::move(hooks));
    std::unique_ptr<OperationAuditContext> context;
    {
        ScopedHeldCatalogOsWriterLock lock(CatalogRoot(root) / "writer.lock");
        std::string error; context = OperationAuditContext::Reserve(contended, &error);
        INFO(error); REQUIRE(context);
    }
    INFO(publicationFailure);
    REQUIRE(publicationFailure.starts_with("audit catalog writer is busy"));
    std::string error; const auto before = context->Capture(contended.workspaceRoot);
    REQUIRE(context->Append("catalog.held-lock", contended.workspaceRoot, before, NowUtc(), 0, &error));
    REQUIRE(context->Finalize(0, &error)); context.reset();
    const auto listed = QueryOperationAuditCatalog(contended);
    INFO(listed.diagnostic); REQUIRE(listed.ready());
    const auto row = std::find_if(listed.rows.begin(), listed.rows.end(), [](const auto& value) {
        return value.runId == "catalog-held-os-lock-reserve";
    });
    REQUIRE(row != listed.rows.end()); REQUIRE(row->state == OperationAuditCatalogState::Final);
    REQUIRE(row->receiptSha256); REQUIRE(row->outcome);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] only writer repair may visit exactly the retained previous pointer", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto old = Spec(root, "catalog-repair-old", 1); Finalize(old);
    const auto current = Spec(root, "catalog-repair-current", 1); Finalize(current);
    const auto listed = QueryOperationAuditCatalog(old); REQUIRE(listed.ready()); REQUIRE(listed.rows.size() == 2);
    const auto oldIt = std::find_if(listed.rows.begin(), listed.rows.end(), [](const auto& row) { return row.runId == "catalog-repair-old"; });
    const auto currentIt = std::find_if(listed.rows.begin(), listed.rows.end(), [](const auto& row) { return row.runId == "catalog-repair-current"; });
    REQUIRE(oldIt != listed.rows.end()); REQUIRE(currentIt != listed.rows.end());
    auto repairedRow = *oldIt; repairedRow.runId = "catalog-repair-new"; repairedRow.attempt = 2;
    Write(CatalogRoot(root) / "current.json", "{corrupt-current}\n");
    const auto reader = QueryOperationAuditCatalog(old);
    REQUIRE(reader.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(reader.rows.empty()); REQUIRE_FALSE(reader.cursor);
    std::string error; const auto paths = ResolveOperationAuditPaths(old, old.correlation.runId, old.correlation.attempt, &error);
    INFO(error); REQUIRE(paths);
    std::size_t repairVisits = 0;
    {
        ScopedCatalogTestHooks hooks({.repairVisitCounter = &repairVisits});
        // previous.json may contain the same logical current attempt in its
        // Pending state. Repair must replace that lifecycle row, not append a
        // duplicate, before adding a genuinely distinct repaired row.
        REQUIRE(PublishOperationAuditCatalogEntry(old, *paths, *currentIt, &error));
        REQUIRE(PublishOperationAuditCatalogEntry(old, *paths, std::move(repairedRow), &error));
    }
    REQUIRE(repairVisits == 1);
    const auto repaired = QueryOperationAuditCatalog(old);
    REQUIRE(repaired.ready()); REQUIRE(repaired.rows.size() == 3);
    REQUIRE(std::any_of(repaired.rows.begin(), repaired.rows.end(), [](const auto& row) { return row.runId == "catalog-repair-old"; }));
    REQUIRE(std::any_of(repaired.rows.begin(), repaired.rows.end(), [](const auto& row) { return row.runId == "catalog-repair-new"; }));
    REQUIRE(std::count_if(repaired.rows.begin(), repaired.rows.end(), [](const auto& row) {
        return row.runId == "catalog-repair-current";
    }) == 1);
    const auto repairedCurrent = std::find_if(repaired.rows.begin(), repaired.rows.end(), [](const auto& row) {
        return row.runId == "catalog-repair-current";
    });
    REQUIRE(repairedCurrent != repaired.rows.end());
    REQUIRE(repairedCurrent->state == OperationAuditCatalogState::Final);
    REQUIRE(repairedCurrent->receiptSha256 == currentIt->receiptSha256);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] only final catalog rows can enter pinned revalidation", "[audit][catalog][KG-TSK-0135]") {
    const auto root = Root(); const auto spec = Spec(root, "catalog-final-only", 1); Finalize(spec);
    const auto query = QueryOperationAuditCatalog(spec);
    INFO(query.diagnostic); REQUIRE(query.ready()); REQUIRE_FALSE(query.rows.empty());
    auto row = query.rows.front();
    row.state = OperationAuditCatalogState::Incomplete;
    row.outcome.reset(); row.receiptSha256.reset();
    row.finishedAtUtc = "2026-01-01T00:00:00Z";
    row.repositoryIdentityHeadSha256.clear();
    const auto rejected = RevalidateOperationAuditCatalogEntry(spec, spec, row);
    REQUIRE_FALSE(rejected.verified()); REQUIRE_FALSE(rejected.run);
    std::error_code ec; std::filesystem::remove_all(root, ec);
}

TEST_CASE("[KG-TSK-0135] catalog rejects FIFOs as well as symlinks", "[audit][catalog][KG-TSK-0135]") {
#if !defined(_WIN32)
    const auto root = Root(); const auto spec = Spec(root, "catalog-fifo", 1); Finalize(spec);
    const auto pointer = CatalogRoot(root) / "current.json";
    std::error_code ec; std::filesystem::remove(pointer, ec); REQUIRE_FALSE(ec);
    REQUIRE(::mkfifo(pointer.c_str(), 0600) == 0);
    const auto result = QueryOperationAuditCatalog(spec);
    REQUIRE(result.code == OperationAuditCatalogQueryCode::Corrupt); REQUIRE(result.rows.empty()); REQUIRE_FALSE(result.cursor);
    std::filesystem::remove_all(root, ec);
#else
    SUCCEED("Windows reparse-point coverage is exercised by the platform path tests.");
#endif
}
