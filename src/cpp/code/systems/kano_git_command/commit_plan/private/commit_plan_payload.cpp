#include "commit_plan_payload.hpp"
#include "commit_plan_payload_parser.hpp"

#include "plan_utils.hpp"
#include "audit_contract.hpp"

#include <array>
#include <format>
#include <set>
#include <utility>
#include <nlohmann/json.hpp>

namespace kano::git::commands {

auto NormalizePlanKey(std::string InValue) -> std::string {
    auto key = Trim(std::move(InValue));
    for (auto& ch : key) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    while (key.size() > 1 && key.back() == '/') {
        key.pop_back();
    }
    if (key.empty()) {
        return ".";
    }
    return key;
}

namespace {

auto UnescapeJsonString(std::string InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size());
    for (std::size_t i = 0; i < InValue.size(); ++i) {
        const char ch = InValue[i];
        if (ch != '\\' || i + 1 >= InValue.size()) {
            out.push_back(ch);
            continue;
        }
        const char next = InValue[i + 1];
        switch (next) {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default:
            // Be permissive for AI-emitted JSON-like payloads (e.g. "\s" in Windows paths):
            // preserve the backslash instead of silently dropping it.
            out.push_back('\\');
            out.push_back(next);
            break;
        }
        i += 1;
    }
    return out;
}

auto SkipJsonWhitespace(const std::string& InText, std::size_t InPos) -> std::size_t {
    std::size_t pos = InPos;
    while (pos < InText.size()) {
        const char ch = InText[pos];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            break;
        }
        pos += 1;
    }
    return pos;
}

auto ParseJsonStringAt(const std::string& InText, std::size_t InPos) -> std::optional<std::pair<std::string, std::size_t>> {
    if (InPos >= InText.size() || InText[InPos] != '"') {
        return std::nullopt;
    }
    std::string raw;
    std::size_t pos = InPos + 1;
    while (pos < InText.size()) {
        const char ch = InText[pos];
        if (ch == '\\') {
            if (pos + 1 >= InText.size()) {
                return std::nullopt;
            }
            raw.push_back(ch);
            raw.push_back(InText[pos + 1]);
            pos += 2;
            continue;
        }
        if (ch == '"') {
            return std::make_pair(UnescapeJsonString(raw), pos + 1);
        }
        raw.push_back(ch);
        pos += 1;
    }
    return std::nullopt;
}

auto FindJsonKeyValueStart(const std::string& InText,
                           const std::string& InKey,
                           std::size_t InFrom = 0) -> std::optional<std::size_t> {
    std::size_t pos = InFrom;
    while (pos < InText.size()) {
        pos = InText.find('"', pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const auto parsed = ParseJsonStringAt(InText, pos);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        const auto& [key, nextPos] = *parsed;
        pos = SkipJsonWhitespace(InText, nextPos);
        if (key == InKey && pos < InText.size() && InText[pos] == ':') {
            return SkipJsonWhitespace(InText, pos + 1);
        }
    }
    return std::nullopt;
}

auto ExtractBracketBody(const std::string& InText,
                        std::size_t InStart,
                        char InOpen,
                        char InClose) -> std::optional<std::string> {
    if (InStart >= InText.size() || InText[InStart] != InOpen) {
        return std::nullopt;
    }
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t pos = InStart; pos < InText.size(); ++pos) {
        const char ch = InText[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == InOpen) {
            depth += 1;
            continue;
        }
        if (ch == InClose) {
            depth -= 1;
            if (depth == 0) {
                return InText.substr(InStart + 1, pos - InStart - 1);
            }
        }
    }
    return std::nullopt;
}

auto ExtractObjectBodyForKey(const std::string& InText,
                             const std::string& InKey) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    return ExtractBracketBody(InText, *valuePos, '{', '}');
}

auto ExtractArrayBodyForKey(const std::string& InText,
                            const std::string& InKey) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InText, InKey);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    return ExtractBracketBody(InText, *valuePos, '[', ']');
}

auto SplitTopLevelObjects(const std::string& InArrayBody) -> std::vector<std::string> {
    std::vector<std::string> objects;
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    std::size_t objectStart = std::string::npos;

    for (std::size_t pos = 0; pos < InArrayBody.size(); ++pos) {
        const char ch = InArrayBody[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }
        if (ch == '"') {
            inString = true;
            continue;
        }
        if (ch == '{') {
            if (depth == 0) {
                objectStart = pos;
            }
            depth += 1;
            continue;
        }
        if (ch == '}') {
            depth -= 1;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(InArrayBody.substr(objectStart, pos - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return objects;
}

auto ExtractStringField(const std::string& InObjectText,
                        const std::string& InField) -> std::optional<std::string> {
    const auto valuePos = FindJsonKeyValueStart(InObjectText, InField);
    if (!valuePos.has_value()) {
        return std::nullopt;
    }
    const auto parsed = ParseJsonStringAt(InObjectText, *valuePos);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->first;
}

auto NormalizePlanPathspecToken(std::string InValue) -> std::string {
    auto value = Trim(std::move(InValue));
    if (value.empty()) {
        return value;
    }

    for (auto& ch : value) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    std::string cleaned;
    cleaned.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            continue;
        }
        cleaned.push_back(ch);
    }

    return Trim(cleaned);
}

auto ExtractStringArrayForKey(const std::string& InObjectText,
                              const std::string& InField) -> std::vector<std::string> {
    std::vector<std::string> out;
    const auto arrayBody = ExtractArrayBodyForKey(InObjectText, InField);
    if (!arrayBody.has_value()) {
        return out;
    }
    std::size_t pos = 0;
    while (pos < arrayBody->size()) {
        pos = SkipJsonWhitespace(*arrayBody, pos);
        if (pos >= arrayBody->size()) {
            break;
        }
        if ((*arrayBody)[pos] == ',') {
            pos += 1;
            continue;
        }
        const auto parsed = ParseJsonStringAt(*arrayBody, pos);
        if (!parsed.has_value()) {
            break;
        }
        auto value = NormalizePlanPathspecToken(parsed->first);
        if (!value.empty()) {
            out.push_back(std::move(value));
        }
        pos = parsed->second;
    }
    return out;
}

auto ParseStageEntries(const std::string& InStageArrayBody) -> std::vector<RepoCommitPlanEntry> {
    std::vector<RepoCommitPlanEntry> entries;
    for (const auto& repoObject : SplitTopLevelObjects(InStageArrayBody)) {
        const auto repoField = ExtractStringField(repoObject, "repo");
        if (!repoField.has_value()) {
            continue;
        }

        RepoCommitPlanEntry entry;
        entry.repoKey = NormalizePlanKey(*repoField);

        const auto commitsArrayBody = ExtractArrayBodyForKey(repoObject, "commits");
        if (commitsArrayBody.has_value()) {
            for (const auto& commitObject : SplitTopLevelObjects(*commitsArrayBody)) {
                const auto messageField = ExtractStringField(commitObject, "message");
                if (!messageField.has_value()) {
                    continue;
                }
                const auto message = CompactSingleLine(Trim(*messageField), 200);
                if (!message.empty()) {
                    RepoCommitPlanEntry::CommitItem item;
                    item.message = message;
                    item.include = ExtractStringArrayForKey(commitObject, "include");
                    item.exclude = ExtractStringArrayForKey(commitObject, "exclude");
                    if (const auto reviewObject = ExtractObjectBodyForKey(commitObject, "review");
                        reviewObject.has_value()) {
                        if (const auto value = ExtractStringField(*reviewObject, "verdict"); value.has_value()) {
                            item.review.verdict = ToLower(Trim(*value));
                        }
                        if (const auto value = ExtractStringField(*reviewObject, "reason"); value.has_value()) {
                            item.review.reason = Trim(*value);
                        }
                    }
                    entry.commits.push_back(std::move(item));
                }
            }
        }

        if (!entry.repoKey.empty() && !entry.commits.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

auto ParseCommitPlanTextImpl(const std::string& InText,
                             std::string* OutError) -> std::optional<CommitPlanPayload> {
    const auto text = Trim(InText);
    if (text.empty()) {
        if (OutError != nullptr) {
            *OutError = "plan file is empty";
        }
        return std::nullopt;
    }

    bool duplicateKey = false;
    std::vector<std::set<std::string>> objectKeys;
    const auto rejectDuplicate = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::object_start) objectKeys.emplace_back();
        else if (event == nlohmann::json::parse_event_t::key) {
            if (objectKeys.empty() || !objectKeys.back().insert(parsed.get<std::string>()).second)
                duplicateKey = true;
        } else if (event == nlohmann::json::parse_event_t::object_end && !objectKeys.empty()) objectKeys.pop_back();
        return true;
    };
    nlohmann::json strictDocument;
    try {
        strictDocument = nlohmann::json::parse(text, rejectDuplicate);
    } catch (const nlohmann::json::parse_error&) {
        if (OutError != nullptr) *OutError = "invalid or trailing JSON document";
        return std::nullopt;
    }
    if (duplicateKey) {
        if (OutError != nullptr) *OutError = "duplicate JSON object field";
        return std::nullopt;
    }
    if (!strictDocument.is_object()) {
        if (OutError != nullptr) *OutError = "plan document must be an object";
        return std::nullopt;
    }

    const auto stagesIterator = strictDocument.find("stages");
    if (stagesIterator == strictDocument.end() || !stagesIterator->is_object()) {
        if (OutError != nullptr) {
            *OutError = "missing \"stages\" object";
        }
        return std::nullopt;
    }
    CommitPlanPayload out;
    const auto metaIterator = strictDocument.find("meta");
    if (metaIterator != strictDocument.end() && !metaIterator->is_object()) {
        if (OutError != nullptr) *OutError = "meta must be an object";
        return std::nullopt;
    }
    if (metaIterator != strictDocument.end()) {
        const auto metaText = metaIterator->dump();
        const auto metaObject = ExtractBracketBody(metaText, 0, '{', '}');
        if (!metaObject.has_value()) {
            if (OutError != nullptr) *OutError = "meta must be an object";
            return std::nullopt;
        }
        if (const auto schemaVersion = ExtractStringField(*metaObject, "schema_version"); schemaVersion.has_value()) {
            out.meta.schemaVersion = Trim(*schemaVersion);
        }
        if (const auto planId = ExtractStringField(*metaObject, "plan_id"); planId.has_value()) {
            out.meta.planId = Trim(*planId);
        }
        if (const auto generatedAtUtc = ExtractStringField(*metaObject, "generated_at_utc"); generatedAtUtc.has_value()) {
            out.meta.generatedAtUtc = Trim(*generatedAtUtc);
        }
        if (const auto executedAtUtc = ExtractStringField(*metaObject, "executed_at_utc"); executedAtUtc.has_value()) {
            out.meta.executedAtUtc = Trim(*executedAtUtc);
        }
        if (const auto baseHeadSha = ExtractStringField(*metaObject, "base_head_sha"); baseHeadSha.has_value()) {
            out.meta.baseHeadSha = Trim(*baseHeadSha);
        }
        if (const auto dirtyFingerprintPreIgnore = ExtractStringField(*metaObject, "dirty_fingerprint_pre_ignore");
            dirtyFingerprintPreIgnore.has_value()) {
            out.meta.dirtyFingerprintPreIgnore = Trim(*dirtyFingerprintPreIgnore);
        }
        if (const auto dirtyFingerprint = ExtractStringField(*metaObject, "dirty_fingerprint"); dirtyFingerprint.has_value()) {
            out.meta.dirtyFingerprint = Trim(*dirtyFingerprint);
        }
        if (const auto freshnessScope = ExtractStringField(*metaObject, "freshness_scope"); freshnessScope.has_value()) {
            out.meta.freshnessScope = Trim(*freshnessScope);
        }
        if (const auto scopeRepo = ExtractStringField(*metaObject, "scope_repo"); scopeRepo.has_value()) {
            out.meta.scopeRepo = NormalizePlanKey(*scopeRepo);
        }
        if (const auto plannerObject = ExtractObjectBodyForKey(*metaObject, "planner"); plannerObject.has_value()) {
            if (const auto value = ExtractStringField(*plannerObject, "provider"); value.has_value()) {
                out.meta.planner.provider = Trim(*value);
            }
            if (const auto value = ExtractStringField(*plannerObject, "ai-model"); value.has_value()) {
                out.meta.planner.model = Trim(*value);
            } else if (const auto valueLegacy = ExtractStringField(*plannerObject, "model"); valueLegacy.has_value()) {
                // Backward compatibility for older plan schema.
                out.meta.planner.model = Trim(*valueLegacy);
            }
            if (const auto value = ExtractStringField(*plannerObject, "request_id"); value.has_value()) {
                out.meta.planner.requestId = Trim(*value);
            }
        }
        if (const auto reviewObject = ExtractObjectBodyForKey(*metaObject, "review"); reviewObject.has_value()) {
            if (const auto value = ExtractStringField(*reviewObject, "verdict"); value.has_value()) {
                out.meta.review.verdict = ToLower(Trim(*value));
            }
            if (const auto value = ExtractStringField(*reviewObject, "reason"); value.has_value()) {
                out.meta.review.reason = Trim(*value);
            }
        }
        const auto correlationIterator = metaIterator->find("correlation");
        if (correlationIterator != metaIterator->end()) {
            if (!correlationIterator->is_object()) {
                if (OutError != nullptr) *OutError = "meta.correlation must be an object";
                return std::nullopt;
            }
            auto& outCorrelation = out.meta.correlation;
            outCorrelation.present = true;
            const auto& typed = *correlationIterator;
            static const std::array<std::string_view, 11> keys = {"mode", "product_id", "topic_id", "item_id", "work_order_id", "request_id", "run_id", "parent_run_id", "producer_id", "route_id", "attempt"};
            if (!typed.is_object() || typed.size() != keys.size()) {
                if (OutError != nullptr) *OutError = "meta.correlation must be a closed 11-field object";
                return std::nullopt;
            }
            for (const auto key : keys) if (!typed.contains(key)) {
                if (OutError != nullptr) *OutError = "meta.correlation is missing required fields";
                return std::nullopt;
            }
            if (!typed.at("attempt").is_number_unsigned() || typed.at("attempt").get<std::uint64_t>() == 0 || typed.at("attempt").get<std::uint64_t>() > UINT32_MAX) {
                if (OutError != nullptr) *OutError = "meta.correlation.attempt must be a positive uint32";
                return std::nullopt;
            }
            outCorrelation.attempt = static_cast<std::uint32_t>(typed.at("attempt").get<std::uint64_t>());
            if (!typed.at("mode").is_string()) {
                if (OutError != nullptr) *OutError = "meta.correlation.mode must be a string";
                return std::nullopt;
            }
            outCorrelation.mode = typed.at("mode").get<std::string>();
            const auto loadId = [&](const char* key, std::string& target) -> bool {
                const auto& value = typed.at(key);
                if (value.is_null()) { target.clear(); return true; }
                if (!value.is_string()) return false;
                const auto id = value.get<std::string>();
                if (!audit::IsStableAuditId(id)) return false;
                target = id;
                return true;
            };
            if (!loadId("product_id", outCorrelation.productId) || !loadId("topic_id", outCorrelation.topicId) ||
                !loadId("item_id", outCorrelation.itemId) || !loadId("work_order_id", outCorrelation.workOrderId) ||
                !loadId("request_id", outCorrelation.requestId) || !loadId("run_id", outCorrelation.runId) ||
                !loadId("parent_run_id", outCorrelation.parentRunId) || !loadId("producer_id", outCorrelation.producerId) ||
                !loadId("route_id", outCorrelation.routeId)) {
                if (OutError != nullptr) *OutError = "meta.correlation identifiers must be null or stable IDs";
                return std::nullopt;
            }
        }
    }

    const auto commitIterator = stagesIterator->find("commit");
    const auto postSyncIterator = stagesIterator->find("post_sync");
    if ((commitIterator != stagesIterator->end() && !commitIterator->is_array()) ||
        (postSyncIterator != stagesIterator->end() && !postSyncIterator->is_array())) {
        if (OutError != nullptr) *OutError = "commit and post_sync stages must be arrays";
        return std::nullopt;
    }
    const auto commitText = commitIterator == stagesIterator->end() ? std::string{} : commitIterator->dump();
    const auto postSyncText = postSyncIterator == stagesIterator->end() ? std::string{} : postSyncIterator->dump();
    const auto commitArray = commitText.empty() ? std::nullopt : ExtractBracketBody(commitText, 0, '[', ']');
    const auto postSyncArray = postSyncText.empty() ? std::nullopt : ExtractBracketBody(postSyncText, 0, '[', ']');
    if (commitArray.has_value()) {
        out.commitEntries = ParseStageEntries(*commitArray);
    }
    if (postSyncArray.has_value()) {
        out.postSyncEntries = ParseStageEntries(*postSyncArray);
    }

    if (out.commitEntries.empty() && out.postSyncEntries.empty()) {
        // A push-only plan intentionally has no commit entries.  Preserve that
        // shape for the execution pipeline; AI-mode validation remains the
        // stricter gate when a commit message is actually required.
        const bool explicitEmptyStage =
            commitArray.has_value() && Trim(*commitArray).empty() &&
            postSyncArray.has_value() && Trim(*postSyncArray).empty();
        if (explicitEmptyStage) {
            return out;
        }
        if (OutError != nullptr) {
            *OutError = "no valid stage entries found";
        }
        return std::nullopt;
    }
    return out;
}

auto IsPlaceholderValue(const std::string& InValue) -> bool {
    const auto value = Trim(InValue);
    return value.rfind("replace-with-", 0) == 0;
}

auto IsValidRequiredValue(const std::string& InValue) -> bool {
    const auto value = Trim(InValue);
    return !value.empty() && !IsPlaceholderValue(value);
}

} // namespace

auto ParseCommitPlanText(const std::string& InText,
                         std::string* OutError) -> std::optional<CommitPlanPayload> {
    return ParseCommitPlanTextImpl(InText, OutError);
}

auto ParseCommitPlanStage(const std::string& InValue) -> std::optional<CommitPlanStage> {
    const auto value = ToLower(Trim(InValue));
    if (value.empty() || value == "commit") {
        return CommitPlanStage::Commit;
    }
    if (value == "post_sync" || value == "post-sync") {
        return CommitPlanStage::PostSync;
    }
    if (value == "both") {
        return CommitPlanStage::Both;
    }
    return std::nullopt;
}

auto PlanStageNeedsPreCommit(const CommitPlanStage InStage) -> bool {
    return InStage == CommitPlanStage::Commit || InStage == CommitPlanStage::Both;
}

auto ParseCommitPlan(const std::filesystem::path& InFile,
                     std::string* OutError) -> std::optional<CommitPlanPayload> {
    const auto payload = ReadFileText(InFile);
    if (!payload.has_value()) {
        if (OutError != nullptr) {
            *OutError = "cannot read plan file";
        }
        return std::nullopt;
    }

    return ParseCommitPlanText(*payload, OutError);
}

auto LoadNormalizedCommitPlan(const std::filesystem::path& InWorkspaceRoot,
                              const std::filesystem::path& InPlanFile,
                              std::string* OutError) -> std::optional<CommitPlanPayload> {
    const auto payload = ReadFileText(InPlanFile);
    if (!payload.has_value()) {
        if (OutError != nullptr) {
            *OutError = "cannot read plan file";
        }
        return std::nullopt;
    }

    std::string normalizeError;
    const auto normalized = NormalizeCommitPlanRepoPaths(InWorkspaceRoot, *payload, &normalizeError);
    if (!normalized.has_value()) {
        if (OutError != nullptr) {
            *OutError = normalizeError.empty() ? std::string("invalid plan repo paths") : normalizeError;
        }
        return std::nullopt;
    }

    return ParseCommitPlanText(*normalized, OutError);
}

auto ValidateCommitPlanCorrelation(const CommitPlanPayload& InPlan,
                                   std::string* OutError) -> bool {
    const auto& correlation = InPlan.meta.correlation;
    // Legacy plans predate the optional KOA envelope.  Absence is not KOA
    // identity and therefore remains a safe standalone request.  Audited
    // mutation routes materialize the explicit standalone envelope (including
    // the generated run id) only in their immutable frozen copy; they never
    // rewrite that synthesized provenance into the caller's source plan.
    if (!correlation.present) return true;
    if (correlation.attempt == 0) {
        if (OutError != nullptr) *OutError = "meta.correlation.attempt must be positive";
        return false;
    }
    const bool koa = correlation.mode == "koa";
    if (correlation.mode != "standalone" && !koa) {
        if (OutError != nullptr) *OutError = "meta.correlation.mode must be standalone or koa";
        return false;
    }
    const std::array<const std::string*, 9> identifiers = {
        &correlation.productId, &correlation.topicId, &correlation.itemId,
        &correlation.workOrderId, &correlation.requestId, &correlation.runId,
        &correlation.parentRunId, &correlation.producerId, &correlation.routeId};
    for (const auto* value : identifiers) {
        if (value->empty()) continue;
        if (!audit::IsStableAuditId(*value)) {
            if (OutError != nullptr) *OutError = "meta.correlation identifier violates the stable-ID grammar";
            return false;
        }
    }
    const std::array<const std::string*, 7> required = {&correlation.productId, &correlation.itemId, &correlation.workOrderId, &correlation.requestId, &correlation.runId, &correlation.producerId, &correlation.routeId};
    if (koa) {
        for (const auto* value : required) {
            if (!Trim(*value).empty()) continue;
            if (OutError != nullptr) *OutError = "meta.correlation has contradictory KOA identity fields";
            return false;
        }
    } else if (!correlation.productId.empty() || !correlation.topicId.empty() ||
               !correlation.itemId.empty() || !correlation.workOrderId.empty() ||
               !correlation.requestId.empty() || !correlation.parentRunId.empty() ||
               !correlation.producerId.empty() || !correlation.routeId.empty()) {
        if (OutError != nullptr) *OutError =
            "standalone provenance identities other than resolved run_id must be null";
        return false;
    }
    if (koa && !correlation.parentRunId.empty() && correlation.parentRunId == correlation.runId) {
        if (OutError != nullptr) *OutError = "meta.correlation parent_run_id cannot equal run_id";
        return false;
    }
    return true;
}

auto ValidateCommitPlanForAiMode(const CommitPlanPayload& InPlan,
                                 std::string* OutError) -> bool {
    if (!ValidateCommitPlanCorrelation(InPlan, OutError)) return false;
    if (!IsValidRequiredValue(InPlan.meta.planId)) {
        if (OutError != nullptr) {
            *OutError = "meta.plan_id is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.generatedAtUtc)) {
        if (OutError != nullptr) {
            *OutError = "meta.generated_at_utc is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.baseHeadSha)) {
        if (OutError != nullptr) {
            *OutError = "meta.base_head_sha is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.dirtyFingerprint)) {
        if (OutError != nullptr) {
            *OutError = "meta.dirty_fingerprint is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.planner.provider)) {
        if (OutError != nullptr) {
            *OutError = "meta.planner.provider is missing or placeholder";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.planner.model)) {
        if (OutError != nullptr) {
            *OutError = "meta.planner.ai-model is missing or placeholder";
        }
        return false;
    }
    if (ToLower(Trim(InPlan.meta.review.verdict)) != "pass") {
        if (OutError != nullptr) {
            *OutError = "meta.review.verdict must be \"pass\"";
        }
        return false;
    }
    if (!IsValidRequiredValue(InPlan.meta.review.reason)) {
        if (OutError != nullptr) {
            *OutError = "meta.review.reason is missing or placeholder";
        }
        return false;
    }

    bool hasValidMessage = false;
    auto scanEntries = [&](const std::vector<RepoCommitPlanEntry>& InEntries) {
        for (const auto& entry : InEntries) {
            for (const auto& item : entry.commits) {
                if (IsValidRequiredValue(item.message)) {
                    hasValidMessage = true;
                    return;
                }
            }
        }
    };
    scanEntries(InPlan.commitEntries);
    if (!hasValidMessage) {
        scanEntries(InPlan.postSyncEntries);
    }
    if (!hasValidMessage) {
        if (OutError != nullptr) {
            *OutError = "no valid non-placeholder commit messages found in stages.commit/post_sync";
        }
        return false;
    }

    auto validateEntryReviews = [&](const std::vector<RepoCommitPlanEntry>& InEntries,
                                    const std::string& InStageName) -> bool {
        for (const auto& entry : InEntries) {
            for (std::size_t idx = 0; idx < entry.commits.size(); ++idx) {
                const auto& item = entry.commits[idx];
                if (!IsValidRequiredValue(item.message)) {
                    if (OutError != nullptr) {
                        *OutError = std::format("{}.repo({}).commits[{}].message is missing or placeholder",
                                                InStageName,
                                                entry.repoKey,
                                                idx);
                    }
                    return false;
                }
                if (ToLower(Trim(item.review.verdict)) != "pass") {
                    if (OutError != nullptr) {
                        *OutError = std::format("{}.repo({}).commits[{}].review.verdict must be \"pass\"",
                                                InStageName,
                                                entry.repoKey,
                                                idx);
                    }
                    return false;
                }
                if (!IsValidRequiredValue(item.review.reason)) {
                    if (OutError != nullptr) {
                        *OutError = std::format("{}.repo({}).commits[{}].review.reason is missing or placeholder",
                                                InStageName,
                                                entry.repoKey,
                                                idx);
                    }
                    return false;
                }
            }
        }
        return true;
    };

    if (!validateEntryReviews(InPlan.commitEntries, "stages.commit")) {
        return false;
    }
    if (!validateEntryReviews(InPlan.postSyncEntries, "stages.post_sync")) {
        return false;
    }

    return true;
}

auto UsesRepoScopedFreshness(const CommitPlanPayload& InPlan) -> bool {
    if (ToLower(Trim(InPlan.meta.freshnessScope)) != "repo") {
        return false;
    }
    if (Trim(InPlan.meta.scopeRepo).empty()) {
        return false;
    }
    return ToLower(Trim(InPlan.meta.planner.provider)) == "native" &&
           ToLower(Trim(InPlan.meta.planner.model)) == "converge-intent-classifier-v1";
}

auto HumanAutoPlanLooksDeterministic(const std::filesystem::path& InPlanPath,
                                     std::string* OutReason) -> bool {
    const auto payload = ReadFileText(InPlanPath);
    if (!payload.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = "cannot read plan file";
        }
        return true;
    }

    const auto meta = ExtractObjectBodyForKey(*payload, "meta");
    if (!meta.has_value()) {
        if (OutReason != nullptr) {
            *OutReason = "plan meta missing";
        }
        return true;
    }

    const auto planner = ExtractObjectBodyForKey(*meta, "planner");
    const auto provider = ToLower(planner.has_value() ? ExtractStringField(*planner, "provider").value_or("") : std::string{});
    const auto model = ToLower(planner.has_value() ? ExtractStringField(*planner, "ai-model").value_or("") : std::string{});

    const bool deterministicPlannerMeta = provider == "agent" ||
                                          model == "external-agent" ||
                                          model == "deterministic" ||
                                          provider == "native";
    if (deterministicPlannerMeta && OutReason != nullptr) {
        *OutReason = std::format("provider={} model={}", provider, model);
    }
    return deterministicPlannerMeta;
}

} // namespace kano::git::commands
