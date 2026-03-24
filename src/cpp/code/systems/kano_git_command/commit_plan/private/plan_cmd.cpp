// plan/ignore commands - native plan pipeline and ignore doctor

#include <CLI/CLI.hpp>
#include "plan_utils.hpp"
#include "command_runtime_ops.hpp"
#include "discovery.hpp"
#include "shell_executor.hpp"
#include "auto_model_policy.hpp"
#include "kog_config.hpp"
#include "secret_scan_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kano::git::commands {
void RegisterPlan(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("plan", "Plan pipeline commands");
    const auto configRoot = std::filesystem::current_path().lexically_normal();
    const auto defaultCommitGenerationMode = kog_config::ResolvePlanCommitGenerationMode(configRoot,
                                                                                         ResolveSkillRoot(configRoot),
                                                                                         "adaptive");

    auto* init = cmd->add_subcommand("new", "Write plan template");
    auto* initOut = new std::string{};
    auto* initForce = new bool{false};
    auto* initAiAuto = new bool{false};
    auto* initAiProvider = new std::string{"auto"};
    auto* initAiModel = new std::string{};
    auto* initAiFillMode = new std::string{defaultCommitGenerationMode};
    auto* initDebugAi = new bool{false};
    auto* initAllowIgnoreGate = new bool{false};
    auto* initDatasourceRoot = new std::string{};
    auto* initDatasourceManifest = new std::string{};
    init->add_option("--output,-o", *initOut, "Plan output path (default: .kano/tmp/git/plans/default-plan.json)");
    init->add_flag("--force,-f", *initForce, "Overwrite existing output");
    init->add_flag("--ai-auto,--ai", *initAiAuto, "Generate and fill plan by AI");
    init->add_option("--ai-provider,--provider", *initAiProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    init->add_option("--ai-model,--model", *initAiModel, "AI model (default: layered kog_config -> auto policy)");
    init->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                     *initAiFillMode,
                     "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    init->add_flag("--debug-ai", *initDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    init->add_flag("--allow-ignore-gate", *initAllowIgnoreGate, "Compatibility flag (currently no-op in native plan new)");
    init->add_option("--ignore-datasource-root",
                     *initDatasourceRoot,
                     "Override ignore datasource root in plan meta.ignore_datasource.root");
    init->add_option("--ignore-datasource-manifest",
                     *initDatasourceManifest,
                     "Override ignore datasource manifest in plan meta.ignore_datasource.manifest");
    init->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto outPath = initOut->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*initOut).lexically_normal();
        const auto datasourceRoot = initDatasourceRoot->empty()
                                        ? std::optional<std::filesystem::path>{}
                                        : std::optional<std::filesystem::path>{ResolvePath(workspaceRoot, *initDatasourceRoot)};
        const auto datasourceManifest = initDatasourceManifest->empty()
                                            ? std::optional<std::filesystem::path>{}
                                            : std::optional<std::filesystem::path>{ResolvePath(workspaceRoot, *initDatasourceManifest)};
        if (std::filesystem::exists(outPath) && !*initForce) {
            std::cerr << "Error: output already exists: " << outPath.generic_string() << "\n";
            std::cerr << "Hint: pass --force to overwrite.\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(outPath, BuildDefaultPlanTemplate(workspaceRoot, datasourceRoot, datasourceManifest), &error)) {
            std::cerr << "Error: failed to write plan template: " << outPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }

        if (*initAiAuto) {
            std::string aiError;
            if (!FillPlanByAi(workspaceRoot, outPath, *initAiProvider, *initAiModel, *initAiFillMode, *initDebugAi, &aiError)) {
                std::cerr << aiError << "\n";
                std::exit(2);
            }
        }

        std::cout << "Wrote plan template: " << outPath.generic_string() << "\n";
    });

    auto* setAiModel = cmd->add_subcommand("set-ai-model", "Set local default AI model in .kano/kog_config.toml for auto plan/commit flows");
    setAiModel->footer([]() {
        return BuildSetAiModelHelpFooter();
    });
    auto* setAiProvider = new std::string{"auto"};
    auto* setAiModelValue = new std::string{};
    setAiModel->add_option("--ai-provider,--provider", *setAiProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    setAiModel->add_option("--ai-model,--model", *setAiModelValue, "AI model to write to local kog_config; see --help footer for detected models")->required();
    setAiModel->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto provider = ResolveAiProvider(*setAiProvider);
        const auto model = Trim(*setAiModelValue);
        if (provider.empty()) {
            std::cerr << "Error: no AI provider found to write config for.\n";
            std::exit(2);
        }
        const auto modelLower = NormalizeAiModelKeyword(model);
        if (model.empty()) {
            std::cerr << "Error: --ai-model must be a model name or special keyword (provider-default|auto).\n";
            std::exit(2);
        }
        const auto configValue = (modelLower == "auto" || modelLower == "provider-default")
            ? modelLower
            : provider + "/" + model;
        const auto localConfig = kog_config::LocalConfigPath(workspaceRoot);
        if (!kog_config::WriteTomlValue(localConfig, "ai.model.selection", configValue)) {
            std::cerr << "Error: failed to persist local AI model config.\n";
            std::exit(2);
        }
        std::cout << "Updated local AI model config: key=ai.model.selection value=" << configValue
                  << " file=" << localConfig.generic_string() << "\n";
    });

    auto* commitSeed = cmd->add_subcommand("commit-seed", "Populate stages.commit skeleton from current dirty repos");
    auto* commitSeedFile = new std::string{};
    auto* commitSeedForce = new bool{false};
    auto* commitSeedDeterministic = new bool{false};
    commitSeed->add_option("--plan-file", *commitSeedFile, "Plan file path");
    commitSeed->add_flag("--force,-f", *commitSeedForce, "Overwrite existing stages.commit even when populated");
    commitSeed->add_flag("--deterministic", *commitSeedDeterministic, "Seed deterministic messages/review instead of placeholders");
    commitSeed->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            commitSeedFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*commitSeedFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::string error;
            if (!WriteFileText(planPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
                std::cerr << "Error: failed to create plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            payload = ReadFileText(planPath);
        }
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto seeded = SeedCommitStage(workspaceRoot, *payload, *commitSeedForce, !*commitSeedDeterministic);
        if (!seeded.has_value()) {
            std::cout << "Plan commit-seed skipped: stages.commit already populated.\n";
            return;
        }
        std::string error;
        if (!WriteFileText(planPath, *seeded, &error)) {
            std::cerr << "Error: failed to write seeded commit stage: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        const auto stages = ExtractObjectBodyForKey(*seeded, "stages").value_or(std::string{});
        const auto commitArray = ExtractArrayBodyForKey(stages, "commit").value_or(std::string{});
        std::cout << std::format("Plan commit-seed complete: repos={} file={}\n",
                                 CountTopLevelObjects(commitArray),
                                 planPath.generic_string());
    });

    auto* needsRefresh = cmd->add_subcommand("refresh-check", "Return 0 when plan should be regenerated");
    auto* needsRefreshFile = new std::string{};
    auto* needsRefreshVerbose = new bool{false};
    needsRefresh->add_option("--plan-file", *needsRefreshFile, "Plan file path");
    needsRefresh->add_flag("--verbose", *needsRefreshVerbose, "Print reason");
    needsRefresh->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = needsRefreshFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*needsRefreshFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: missing-or-unreadable\n";
            }
            std::exit(0);
        }
        if (PlanNeedsRefresh(*payload)) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: placeholder-or-empty\n";
            }
            std::exit(0);
        }
        std::string planBaseHeadSha;
        std::string planDirtyFingerprint;
        if (!ExtractPlanWorkspaceHashes(*payload, &planBaseHeadSha, &planDirtyFingerprint)) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: missing-or-placeholder-workspace-hash\n";
            }
            std::exit(0);
        }
        const auto currentBaseHeadSha = ComputeWorkspaceBaseHeadSha(workspaceRoot);
        const auto currentDirtyFingerprint = ComputeWorkspaceDirtyFingerprint(workspaceRoot);
        if (planBaseHeadSha != currentBaseHeadSha || planDirtyFingerprint != currentDirtyFingerprint) {
            if (*needsRefreshVerbose) {
                std::cout << "refresh-needed: workspace-state-changed\n";
            }
            std::exit(0);
        }
        if (*needsRefreshVerbose) {
            std::cout << "refresh-not-needed\n";
        }
        std::exit(1);
    });

    auto* prepare = cmd->add_subcommand("prepare", "Prepare-stage plan editing utilities");
    auto* addCommitEntry = prepare->add_subcommand("add-commit-entry", "Add or append one commit entry to stages.commit");
    auto* addCommitFile = new std::string{};
    auto* addCommitRepo = new std::string{};
    auto* addCommitMessage = new std::string{};
    auto* addCommitInclude = new std::vector<std::string>{};
    auto* addCommitExclude = new std::vector<std::string>{};
    auto* addCommitReviewVerdict = new std::string{"pass"};
    auto* addCommitReviewReason = new std::string{};
    addCommitEntry->add_option("--plan-file", *addCommitFile, "Plan file path");
    addCommitEntry->add_option("--repo", *addCommitRepo, "Target repo path/key (e.g. . or .agents/kano)")->required();
    addCommitEntry->add_option("--commit-message,--commit.message", *addCommitMessage, "Commit message")->required();
    addCommitEntry->add_option("--commit-include,--commit.include", *addCommitInclude, "Include pathspec (repeatable)");
    addCommitEntry->add_option("--commit-exclude,--commit.exclude", *addCommitExclude, "Exclude pathspec (repeatable)");
    addCommitEntry->add_option("--commit-review-verdict,--commit.review.verdict",
                               *addCommitReviewVerdict,
                               "Review verdict (default: pass)")
        ->default_str("pass");
    addCommitEntry->add_option("--commit-review-reason,--commit.review.reason", *addCommitReviewReason, "Review reason")->required();
    addCommitEntry->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            addCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*addCommitFile).lexically_normal();
        auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::string error;
            if (!WriteFileText(planPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
                std::cerr << "Error: failed to create plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                std::exit(2);
            }
            payload = ReadFileText(planPath);
        }
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto updated = UpsertCommitEntry(*payload,
                                               Trim(*addCommitRepo),
                                               Trim(*addCommitMessage),
                                               *addCommitInclude,
                                               *addCommitExclude,
                                               Trim(*addCommitReviewVerdict),
                                               Trim(*addCommitReviewReason));
        if (!updated.has_value()) {
            std::cerr << "Error: failed to update stages.commit (schema missing?)\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(planPath, *updated, &error)) {
            std::cerr << "Error: failed to write plan file: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::cout << "Plan prepare add-commit-entry complete: " << planPath.generic_string() << "\n";
    });

    auto* fillCommit = cmd->add_subcommand("fill-commit", "Fill/update one stages.commit entry by global index");
    auto* fillCommitFile = new std::string{};
    auto* fillCommitIndex = new int{-1};
    auto* fillCommitMessage = new std::string{};
    auto* fillCommitReviewVerdict = new std::string{};
    auto* fillCommitReviewReason = new std::string{};
    fillCommit->add_option("--plan-file", *fillCommitFile, "Plan file path");
    fillCommit->add_option("index", *fillCommitIndex, "Global commit index from `kog plan finish-report`")->required();
    fillCommit->add_option("--commit-message,--commit.message,--message", *fillCommitMessage, "Commit message");
    fillCommit->add_option("--review-verdict,--review.verdict", *fillCommitReviewVerdict, "Review verdict");
    fillCommit->add_option("--review-reason,--review.reason", *fillCommitReviewReason, "Review reason");
    fillCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            fillCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*fillCommitFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }

        const auto message = Trim(*fillCommitMessage);
        const auto reviewVerdict = Trim(*fillCommitReviewVerdict);
        const auto reviewReason = Trim(*fillCommitReviewReason);
        const bool hasMessage = !message.empty();
        const bool hasVerdict = !reviewVerdict.empty();
        const bool hasReason = !reviewReason.empty();
        if (!hasMessage && !hasVerdict && !hasReason) {
            std::cerr << "Error: no fill fields provided. Use --commit-message and/or --review-verdict/--review-reason\n";
            std::exit(2);
        }

        std::string fillError;
        const auto updated = FillCommitEntryByFlatIndex(*payload,
                                                         *fillCommitIndex,
                                                         hasMessage ? std::optional<std::string>{message} : std::nullopt,
                                                         hasVerdict ? std::optional<std::string>{reviewVerdict} : std::nullopt,
                                                         hasReason ? std::optional<std::string>{reviewReason} : std::nullopt,
                                                         std::nullopt,
                                                         std::nullopt,
                                                         &fillError);
        if (!updated.has_value()) {
            std::cerr << "Error: failed to fill commit entry";
            if (!fillError.empty()) {
                std::cerr << ": " << fillError;
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::string error;
        if (!WriteFileText(planPath, *updated, &error)) {
            std::cerr << "Error: failed to write plan file: " << planPath.generic_string();
            if (!error.empty()) {
                std::cerr << " (" << error << ")";
            }
            std::cerr << "\n";
            std::exit(2);
        }
        std::cout << "Plan fill-commit complete: index=" << *fillCommitIndex << " file=" << planPath.generic_string() << "\n";
    });

    auto* getCommit = cmd->add_subcommand("get-commit", "Get one stages.commit entry by global index");
    auto* getCommitFile = new std::string{};
    auto* getCommitIndex = new int{-1};
    auto* getCommitJson = new bool{false};
    getCommit->add_option("--plan-file", *getCommitFile, "Plan file path");
    getCommit->add_option("index", *getCommitIndex, "Global commit index from `kog plan finish-report`")->required();
    getCommit->add_flag("--json", *getCommitJson, "Print machine-readable JSON output");
    getCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            getCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*getCommitFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        std::string lookupError;
        const auto entry = FindCommitEntryByFlatIndex(*payload, *getCommitIndex, &lookupError);
        if (!entry.has_value()) {
            std::cerr << "Error: failed to read commit entry";
            if (!lookupError.empty()) {
                std::cerr << ": " << lookupError;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        if (*getCommitJson) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"index\": " << entry->index << ",\n";
            oss << "  \"repo\": \"" << JsonEscape(entry->repo) << "\",\n";
            oss << "  \"message\": \"" << JsonEscape(entry->message) << "\",\n";
            oss << "  \"include\": [";
            for (std::size_t i = 0; i < entry->include.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << "\"" << JsonEscape(entry->include[i]) << "\"";
            }
            oss << "],\n";
            oss << "  \"exclude\": [";
            for (std::size_t i = 0; i < entry->exclude.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << "\"" << JsonEscape(entry->exclude[i]) << "\"";
            }
            oss << "],\n";
            oss << "  \"review\": {\n";
            oss << "    \"verdict\": \"" << JsonEscape(entry->reviewVerdict) << "\",\n";
            oss << "    \"reason\": \"" << JsonEscape(entry->reviewReason) << "\"\n";
            oss << "  }\n";
            oss << "}\n";
            std::cout << oss.str();
            return;
        }

        std::cout << "[plan] commit[" << entry->index << "] repo=" << entry->repo << "\n";
        std::cout << "[plan] message: " << entry->message << "\n";
        std::cout << "[plan] review.verdict: " << entry->reviewVerdict << "\n";
        std::cout << "[plan] review.reason: " << entry->reviewReason << "\n";
        std::cout << "[plan] include:\n";
        for (const auto& path : entry->include) {
            std::cout << "  - " << path << "\n";
        }
        std::cout << "[plan] exclude:\n";
        for (const auto& path : entry->exclude) {
            std::cout << "  - " << path << "\n";
        }
    });

    auto* countCommits = cmd->add_subcommand("count-commits", "Count commit entries and review-needed items in stages.commit");
    auto* countCommitsFile = new std::string{};
    auto* countCommitsJson = new bool{false};
    countCommits->add_option("--plan-file", *countCommitsFile, "Plan file path");
    countCommits->add_flag("--json", *countCommitsJson, "Print machine-readable JSON output");
    countCommits->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = countCommitsFile->empty() ? DefaultPlanPath(workspaceRoot)
                                                        : std::filesystem::path(*countCommitsFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto entries = CollectCommitPlanEntries(*payload);
        const auto reviewNeededIndexes = CollectCommitIndexesNeedingReview(entries);
        if (*countCommitsJson) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"total_commits\": " << entries.size() << ",\n";
            oss << "  \"review_needed_commits\": " << reviewNeededIndexes.size() << ",\n";
            oss << "  \"review_needed_indexes\": [";
            for (std::size_t i = 0; i < reviewNeededIndexes.size(); ++i) {
                if (i != 0) {
                    oss << ",";
                }
                oss << reviewNeededIndexes[i];
            }
            oss << "]\n";
            oss << "}\n";
            std::cout << oss.str();
            return;
        }
        std::cout << "[plan] total_commits: " << entries.size() << "\n";
        std::cout << "[plan] review_needed_commits: " << reviewNeededIndexes.size() << "\n";
        std::cout << "[plan] review_needed_indexes:";
        if (reviewNeededIndexes.empty()) {
            std::cout << " <none>\n";
            return;
        }
        for (const auto index : reviewNeededIndexes) {
            std::cout << " " << index;
        }
        std::cout << "\n";
    });

    auto* summary = cmd->add_subcommand("finish-report", "Print compact plan summary");
    auto* summaryFile = new std::string{};
    auto* summaryMax = new int{10};
    summary->add_option("--plan-file", *summaryFile, "Plan file path");
    summary->add_option("--max-commits", *summaryMax, "Max commit lines to print")->default_val(10);
    summary->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = summaryFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*summaryFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        if (!payload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }
        const auto text = *payload;
        const auto meta = ExtractObjectBodyForKey(text, "meta");
        const auto stages = ExtractObjectBodyForKey(text, "stages");
        if (!meta.has_value() || !stages.has_value()) {
            std::cerr << "Error: plan schema invalid: missing meta/stages\n";
            std::exit(2);
        }
        const auto planner = ExtractObjectBodyForKey(*meta, "planner");
        const auto planId = ExtractStringField(*meta, "plan_id").value_or("-");
        const auto generated = ExtractStringField(*meta, "generated_at_utc").value_or("-");
        const auto provider = planner.has_value() ? ExtractStringField(*planner, "provider").value_or("-") : "-";
        const auto model = planner.has_value() ? ExtractStringField(*planner, "ai-model").value_or("-") : "-";
        std::cout << std::format("[plan] meta: plan_id={} generated={} provider={} ai-model={}\n", planId, generated, provider, model);

        const auto commitArray = ExtractArrayBodyForKey(*stages, "commit").value_or(std::string{});
        std::size_t repoCount = 0;
        std::size_t commitCount = 0;
        std::size_t flatIndex = 0;
        std::vector<std::string> lines;
        for (const auto& repoObj : SplitTopLevelObjects(commitArray)) {
            const auto repo = ExtractStringField(repoObj, "repo").value_or("?");
            const auto commits = ExtractArrayBodyForKey(repoObj, "commits").value_or(std::string{});
            const auto commitObjects = SplitTopLevelObjects(commits);
            if (!commitObjects.empty()) {
                repoCount += 1;
            }
            commitCount += commitObjects.size();
            for (const auto& commitObj : commitObjects) {
                const auto msg = ExtractStringField(commitObj, "message").value_or("");
                lines.push_back(std::format("[plan] - [{}] {}: {}", flatIndex, repo, msg));
                flatIndex += 1;
            }
        }
        std::cout << std::format("[plan] commits: repos={} total={}\n", repoCount, commitCount);
        if (*summaryMax < 0) {
            *summaryMax = 0;
        }
        const auto limit = std::min<std::size_t>(lines.size(), static_cast<std::size_t>(*summaryMax));
        for (std::size_t i = 0; i < limit; ++i) {
            std::cout << lines[i] << "\n";
        }
    });

    auto* ensureAiReady = cmd->add_subcommand("ensure-prepare-ready", "Ensure plan is prepared and AI-ready");
    auto* ensureFile = new std::string{};
    auto* ensureProvider = new std::string{"auto"};
    auto* ensureModel = new std::string{};
    auto* ensureFillMode = new std::string{defaultCommitGenerationMode};
    auto* ensureDebugAi = new bool{false};
    auto* ensureAllowIgnoreGate = new bool{false};
    auto* ensureForce = new bool{false};
    ensureAiReady->add_option("--plan-file", *ensureFile, "Plan file path");
    ensureAiReady->add_option("--ai-provider,--provider", *ensureProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    ensureAiReady->add_option("--ai-model,--model", *ensureModel, "AI model (default: layered kog_config -> auto policy)");
    ensureAiReady->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                              *ensureFillMode,
                              "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    ensureAiReady->add_flag("--debug-ai", *ensureDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    ensureAiReady->add_flag("--allow-ignore-gate", *ensureAllowIgnoreGate, "Compatibility flag (currently no-op in prepare)");
    ensureAiReady->add_flag("--force,-f", *ensureForce, "Force regenerate even if existing plan looks complete");
    ensureAiReady->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = ensureFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*ensureFile).lexically_normal();
        const auto payload = ReadFileText(planPath);
        const bool needs = *ensureForce || !payload.has_value() || PlanNeedsRefresh(*payload) ||
                           (payload.has_value() && PlanWorkspaceStateDrifted(workspaceRoot, *payload));
        std::optional<std::string> latestPayload = payload;
        auto regenerateOnce = [&]() -> bool {
            std::string error;
            if (!WriteFileText(planPath, BuildDefaultPlanTemplate(workspaceRoot), &error)) {
                std::cerr << "Error: failed to write plan template: " << planPath.generic_string();
                if (!error.empty()) {
                    std::cerr << " (" << error << ")";
                }
                std::cerr << "\n";
                return false;
            }
            if (const auto seeded = SeedCommitStage(workspaceRoot, ReadFileText(planPath).value_or(std::string{}), true, true);
                seeded.has_value()) {
                std::string seedError;
                if (!WriteFileText(planPath, *seeded, &seedError)) {
                    std::cerr << "Error: failed to seed commit stage: " << planPath.generic_string();
                    if (!seedError.empty()) {
                        std::cerr << " (" << seedError << ")";
                    }
                    std::cerr << "\n";
                    return false;
                }
            }
            std::string aiError;
            if (!FillPlanByAi(workspaceRoot, planPath, *ensureProvider, *ensureModel, *ensureFillMode, *ensureDebugAi, &aiError)) {
                std::cerr << aiError << "\n";
                return false;
            }
            latestPayload = ReadFileText(planPath);
            return latestPayload.has_value();
        };
        if (needs) {
            if (!regenerateOnce()) {
                std::exit(2);
            }
        }
        if (!latestPayload.has_value()) {
            std::cerr << "Error: plan file not found/readable: " << planPath.generic_string() << "\n";
            std::exit(2);
        }

        std::string reason;
        if (!ValidateAiReadyPlan(*latestPayload, &reason)) {
            std::cerr << std::format("[plan] validation failed ({}), regenerating once...\n", reason);
            if (!regenerateOnce()) {
                std::exit(2);
            }
            reason.clear();
            if (!latestPayload.has_value() || !ValidateAiReadyPlan(*latestPayload, &reason)) {
                if (reason == "no commit entries in stages.commit" && latestPayload.has_value() &&
                    AllowDeterministicCommitFallbackForMode(defaultCommitGenerationMode)) {
                    if (const auto fallback = TryInjectFallbackCommits(workspaceRoot, *latestPayload); fallback.has_value()) {
                        std::string writeError;
                        if (!WriteFileText(planPath, *fallback, &writeError)) {
                            std::cerr << "Error: failed to write fallback commit plan: " << planPath.generic_string();
                            if (!writeError.empty()) {
                                std::cerr << " (" << writeError << ")";
                            }
                            std::cerr << "\n";
                            std::exit(2);
                        }
                        latestPayload = ReadFileText(planPath);
                        reason.clear();
                        if (latestPayload.has_value() && ValidateAiReadyPlan(*latestPayload, &reason)) {
                            std::cerr << "[plan] fallback commit entries injected after empty AI commit stage.\n";
                            std::cout << "Plan ensure-prepare-ready passed: " << planPath.generic_string() << "\n";
                            return;
                        }
                    }
                }
                std::cerr << "Error: AI-ready plan validation failed: " << reason << "\n";
                std::exit(2);
            }
        }
        std::cout << "Plan ensure-prepare-ready passed: " << planPath.generic_string() << "\n";
    });

    auto* preflightAiCommit = cmd->add_subcommand("runbook-commit", "Run commit runbook (prepare + summary + validate)");
    preflightAiCommit->group("");
    auto* preflightFile = new std::string{};
    auto* preflightProvider = new std::string{"auto"};
    auto* preflightModel = new std::string{};
    auto* preflightFillMode = new std::string{defaultCommitGenerationMode};
    auto* preflightDebugAi = new bool{false};
    auto* preflightAllowIgnoreGate = new bool{false};
    auto* preflightMaxCommits = new int{10};
    preflightAiCommit->add_option("--plan-file", *preflightFile, "Plan file path");
    preflightAiCommit->add_option("--ai-provider,--provider", *preflightProvider, "AI provider (copilot|codex|opencode|auto)")
        ->default_str("auto");
    preflightAiCommit->add_option("--ai-model,--model", *preflightModel, "AI model (default: layered kog_config -> auto policy)");
    preflightAiCommit->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                                  *preflightFillMode,
                                  "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    preflightAiCommit->add_flag("--debug-ai", *preflightDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    preflightAiCommit->add_flag("--allow-ignore-gate", *preflightAllowIgnoreGate, "Compatibility flag (currently no-op in runbook-commit)");
    preflightAiCommit->add_option("--max-commits", *preflightMaxCommits, "Max commit lines to print in summary")->default_val(10);

    preflightAiCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = preflightFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*preflightFile).lexically_normal();
        const auto code = RunCommitRunbook(
            workspaceRoot, planPath, *preflightProvider, *preflightModel, *preflightFillMode, *preflightDebugAi, *preflightMaxCommits);
        std::exit(code);
    });



    auto* runbookIgnore = cmd->add_subcommand("runbook-ignore", "Run ignore runbook (init + pre-apply verify)");
    runbookIgnore->group("");
    auto* runbookIgnoreFile = new std::string{};
    auto* runbookIgnoreForce = new bool{false};
    auto* runbookIgnoreMaxPerRepo = new int{200};
    auto* runbookIgnoreDatasourceRoot = new std::string{};
    auto* runbookIgnoreDatasourceManifest = new std::string{};
    runbookIgnore->add_option("--plan-file", *runbookIgnoreFile, "Plan file path");
    runbookIgnore->add_flag("--force,-f", *runbookIgnoreForce, "Create default plan when file missing");
    runbookIgnore->add_option("--max-per-repo", *runbookIgnoreMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookIgnore->add_option("--ignore-datasource-root", *runbookIgnoreDatasourceRoot, "Override plan meta.ignore_datasource.root");
    runbookIgnore->add_option("--ignore-datasource-manifest", *runbookIgnoreDatasourceManifest, "Override plan meta.ignore_datasource.manifest");
    runbookIgnore->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            runbookIgnoreFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*runbookIgnoreFile).lexically_normal();
        const auto code = RunIgnoreRunbook(workspaceRoot,
                                           planPath,
                                           *runbookIgnoreForce,
                                           *runbookIgnoreMaxPerRepo,
                                           *runbookIgnoreDatasourceRoot,
                                           *runbookIgnoreDatasourceManifest);
        std::exit(code);
    });

    auto* runbookFull = cmd->add_subcommand("runbook-full", "Run full runbook (ignore + commit + pre-apply verify)");
    runbookFull->group("");
    auto* runbookFullFile = new std::string{};
    auto* runbookFullProvider = new std::string{"auto"};
    auto* runbookFullModel = new std::string{};
    auto* runbookFullFillMode = new std::string{defaultCommitGenerationMode};
    auto* runbookFullDebugAi = new bool{false};
    auto* runbookFullAllowIgnoreGate = new bool{false};
    auto* runbookFullForce = new bool{false};
    auto* runbookFullMaxCommits = new int{10};
    auto* runbookFullMaxPerRepo = new int{200};
    runbookFull->add_option("--plan-file", *runbookFullFile, "Plan file path");
    runbookFull->add_option("--ai-provider,--provider", *runbookFullProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookFull->add_option("--ai-model,--model", *runbookFullModel, "AI model (default: config -> remembered -> auto)");
    runbookFull->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                            *runbookFullFillMode,
                            "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    runbookFull->add_flag("--debug-ai", *runbookFullDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookFull->add_flag("--allow-ignore-gate", *runbookFullAllowIgnoreGate, "Compatibility flag (forwarded to commit runbook)");
    runbookFull->add_flag("--force,-f", *runbookFullForce, "Create default plan when file missing during ignore runbook");
    runbookFull->add_option("--max-commits", *runbookFullMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookFull->add_option("--max-per-repo", *runbookFullMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookFull->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath =
            runbookFullFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*runbookFullFile).lexically_normal();
        const auto ignoreCode = RunIgnoreRunbook(workspaceRoot, planPath, *runbookFullForce, *runbookFullMaxPerRepo, "", "");
        if (ignoreCode != 0) {
            std::exit(ignoreCode);
        }
        const auto commitCode = RunCommitRunbook(workspaceRoot,
                                                 planPath,
                                                 *runbookFullProvider,
                                                 *runbookFullModel,
                                                 *runbookFullFillMode,
                                                 *runbookFullDebugAi,
                                                 *runbookFullMaxCommits);
        if (commitCode != 0) {
            std::exit(commitCode);
        }

        const auto verifyCode = RunPreApplyVerify(workspaceRoot, planPath, "all");
        std::exit(verifyCode);
    });

    auto* runbook = cmd->add_subcommand("runbook", "Plan runbooks");
    auto* runbookCommit = runbook->add_subcommand("commit", "Run commit runbook (prepare + summary + pre-apply verify)");
    auto* rbCommitFile = new std::string{};
    auto* rbCommitProvider = new std::string{"auto"};
    auto* rbCommitModel = new std::string{};
    auto* rbCommitFillMode = new std::string{defaultCommitGenerationMode};
    auto* rbCommitDebugAi = new bool{false};
    auto* rbCommitAllowIgnoreGate = new bool{false};
    auto* rbCommitMaxCommits = new int{10};
    runbookCommit->add_option("--plan-file", *rbCommitFile, "Plan file path");
    runbookCommit->add_option("--ai-provider,--provider", *rbCommitProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookCommit->add_option("--ai-model,--model", *rbCommitModel, "AI model (default: config -> remembered -> auto)");
    runbookCommit->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                              *rbCommitFillMode,
                              "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    runbookCommit->add_flag("--debug-ai", *rbCommitDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookCommit->add_flag("--allow-ignore-gate", *rbCommitAllowIgnoreGate, "Forward allow-ignore-gate to commit runbook");
    runbookCommit->add_option("--max-commits", *rbCommitMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookCommit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbCommitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbCommitFile).lexically_normal();
        const auto code =
            RunCommitRunbook(workspaceRoot, planPath, *rbCommitProvider, *rbCommitModel, *rbCommitFillMode, *rbCommitDebugAi, *rbCommitMaxCommits);
        std::exit(code);
    });

    auto* runbookIgnorePublic = runbook->add_subcommand("ignore", "Run ignore runbook (init + pre-apply verify)");
    auto* rbIgnoreFile = new std::string{};
    auto* rbIgnoreForce = new bool{false};
    auto* rbIgnoreMaxPerRepo = new int{200};
    auto* rbIgnoreDatasourceRoot = new std::string{};
    auto* rbIgnoreDatasourceManifest = new std::string{};
    runbookIgnorePublic->add_option("--plan-file", *rbIgnoreFile, "Plan file path");
    runbookIgnorePublic->add_flag("--force,-f", *rbIgnoreForce, "Create default plan when file missing");
    runbookIgnorePublic->add_option("--max-per-repo", *rbIgnoreMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookIgnorePublic->add_option("--ignore-datasource-root", *rbIgnoreDatasourceRoot, "Override plan meta.ignore_datasource.root");
    runbookIgnorePublic->add_option("--ignore-datasource-manifest", *rbIgnoreDatasourceManifest, "Override plan meta.ignore_datasource.manifest");
    runbookIgnorePublic->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbIgnoreFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbIgnoreFile).lexically_normal();
        const auto code = RunIgnoreRunbook(
            workspaceRoot, planPath, *rbIgnoreForce, *rbIgnoreMaxPerRepo, *rbIgnoreDatasourceRoot, *rbIgnoreDatasourceManifest);
        std::exit(code);
    });

    auto* runbookFullPublic = runbook->add_subcommand("full", "Run full runbook (ignore + commit + pre-apply verify)");
    auto* rbFullFile = new std::string{};
    auto* rbFullProvider = new std::string{"auto"};
    auto* rbFullModel = new std::string{};
    auto* rbFullFillMode = new std::string{defaultCommitGenerationMode};
    auto* rbFullDebugAi = new bool{false};
    auto* rbFullAllowIgnoreGate = new bool{false};
    auto* rbFullForce = new bool{false};
    auto* rbFullMaxCommits = new int{10};
    auto* rbFullMaxPerRepo = new int{200};
    runbookFullPublic->add_option("--plan-file", *rbFullFile, "Plan file path");
    runbookFullPublic->add_option("--ai-provider,--provider", *rbFullProvider, "AI provider (copilot|codex|opencode|auto)")->default_str("auto");
    runbookFullPublic->add_option("--ai-model,--model", *rbFullModel, "AI model (default: config -> remembered -> auto)");
    runbookFullPublic->add_option("--ai-commit-generation-mode,--ai-fill-mode",
                                  *rbFullFillMode,
                                  "AI commit generation mode: single=one workspace-wide pass, per-commit=one AI pass per commit, adaptive=per-commit + deterministic gitlink fallback (single|per-commit|adaptive)")
        ->default_str(defaultCommitGenerationMode);
    runbookFullPublic->add_flag("--debug-ai", *rbFullDebugAi, "Write AI prompt/raw/extracted debug artifacts");
    runbookFullPublic->add_flag("--allow-ignore-gate", *rbFullAllowIgnoreGate, "Forward allow-ignore-gate to commit runbook");
    runbookFullPublic->add_flag("--force,-f", *rbFullForce, "Create default plan when file missing during ignore runbook");
    runbookFullPublic->add_option("--max-commits", *rbFullMaxCommits, "Max commit lines to print in summary")->default_val(10);
    runbookFullPublic->add_option("--max-per-repo", *rbFullMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    runbookFullPublic->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = rbFullFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*rbFullFile).lexically_normal();
        const auto ignoreCode = RunIgnoreRunbook(workspaceRoot, planPath, *rbFullForce, *rbFullMaxPerRepo, "", "");
        if (ignoreCode != 0) {
            std::exit(ignoreCode);
        }
        const auto commitCode =
            RunCommitRunbook(workspaceRoot, planPath, *rbFullProvider, *rbFullModel, *rbFullFillMode, *rbFullDebugAi, *rbFullMaxCommits);
        if (commitCode != 0) {
            std::exit(commitCode);
        }
        const auto verifyCode = RunPreApplyVerify(workspaceRoot, planPath, "all");
        std::exit(verifyCode);
    });

    auto* ignoreInit = cmd->add_subcommand("ignore-init", "Populate stages.ignore from current working tree");
    auto* ignoreInitFile = new std::string{};
    auto* ignoreInitForce = new bool{false};
    auto* ignoreInitMaxPerRepo = new int{200};
    auto* ignoreInitDatasourceRoot = new std::string{};
    auto* ignoreInitDatasourceManifest = new std::string{};
    ignoreInit->add_option("--plan-file", *ignoreInitFile, "Plan file path");
    ignoreInit->add_flag("--force,-f", *ignoreInitForce, "Create default plan when file missing");
    ignoreInit->add_option("--max-per-repo", *ignoreInitMaxPerRepo, "Max ignore candidates per repo")->default_val(200);
    ignoreInit->add_option("--ignore-datasource-root",
                           *ignoreInitDatasourceRoot,
                           "Override plan meta.ignore_datasource.root");
    ignoreInit->add_option("--ignore-datasource-manifest",
                           *ignoreInitDatasourceManifest,
                           "Override plan meta.ignore_datasource.manifest");
    ignoreInit->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = ignoreInitFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*ignoreInitFile).lexically_normal();
        const auto datasourceRoot = ignoreInitDatasourceRoot->empty() ? "" : *ignoreInitDatasourceRoot;
        const auto datasourceManifest = ignoreInitDatasourceManifest->empty() ? "" : *ignoreInitDatasourceManifest;
        std::exit(RunIgnoreInit(workspaceRoot, planPath, *ignoreInitForce, *ignoreInitMaxPerRepo, datasourceRoot, datasourceManifest));
    });

    auto* datasourceSync = cmd->add_subcommand("datasource-sync", "Update ignore-plan reference datasource (upstream templates)");
    auto* datasourceSyncSource = new std::string{"github-gitignore"};
    auto* datasourceSyncDryRun = new bool{false};
    datasourceSync->add_option("--source", *datasourceSyncSource, "Datasource source id")->default_str("github-gitignore");
    datasourceSync->add_flag("--dry-run", *datasourceSyncDryRun, "Print revision metadata without updating");
    datasourceSync->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        std::exit(RunDatasourceSync(workspaceRoot, *datasourceSyncSource, *datasourceSyncDryRun));
    });

    auto* dirtyScope = cmd->add_subcommand("prepare-scope", "Count dirty repos and total changed entries");
    auto* dirtyScopeRoot = new std::string{};
    dirtyScope->add_option("--workspace-root", *dirtyScopeRoot, "Workspace root path (default: cwd)");
    dirtyScope->callback([=]() {
        const auto workspaceRoot =
            dirtyScopeRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*dirtyScopeRoot).lexically_normal();
        const auto [dirtyRepos, totalChanges] = CountDirtyScope(workspaceRoot);
        std::cout << std::format("{} {}\n", dirtyRepos, totalChanges);
    });

    auto* checkIgnoreGate = cmd->add_subcommand("ignore-gate", "Check unresolved artifact-like untracked files");
    checkIgnoreGate->group("");
    auto* ignoreGateRoot = new std::string{};
    auto* ignoreGateContext = new std::string{"plan"};
    auto* ignoreGateAllowlist = new std::string{};
    auto* ignoreGateLimit = new int{20};
    checkIgnoreGate->add_option("--workspace-root", *ignoreGateRoot, "Workspace root path (default: cwd)");
    checkIgnoreGate->add_option("--context", *ignoreGateContext, "Context label (plan|ai-commit)")->default_str("plan");
    checkIgnoreGate->add_option("--allowlist", *ignoreGateAllowlist, "Allowlist file path");
    checkIgnoreGate->add_option("--limit", *ignoreGateLimit, "Max listed candidates")->default_val(20);
    checkIgnoreGate->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        std::exit(RunIgnoreGate(workspaceRoot, *ignoreGateContext, *ignoreGateAllowlist, *ignoreGateLimit));
    });

    auto* checkSecretGate = cmd->add_subcommand("secret-gate", "Check changed files for secret/token patterns");
    checkSecretGate->group("");
    auto* secretGateRoot = new std::string{};
    auto* secretGateContext = new std::string{"plan"};
    auto* secretGateRules = new std::string{};
    auto* secretGateLimit = new int{20};
    checkSecretGate->add_option("--workspace-root", *secretGateRoot, "Workspace root path (default: cwd)");
    checkSecretGate->add_option("--context", *secretGateContext, "Context label (plan|ai-commit|commit-push)")->default_str("plan");
    checkSecretGate->add_option("--rules-file", *secretGateRules, "Rule file path (format: id|regex)");
    checkSecretGate->add_option("--limit", *secretGateLimit, "Max listed findings")->default_val(20);
    checkSecretGate->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        std::exit(RunSecretGate(workspaceRoot, *secretGateContext, *secretGateRules, *secretGateLimit));
    });

    auto* verify = cmd->add_subcommand("verify", "Plan verification stages");

    auto* verifyPreApply = verify->add_subcommand("pre-apply", "Verify plan schema before apply");
    auto* verifyPreFile = new std::string{};
    auto* verifyPreStage = new std::string{"all"};
    verifyPreApply->add_option("--plan-file", *verifyPreFile, "Plan file path");
    verifyPreApply->add_option("--stage", *verifyPreStage, "Stage: ignore|commit|all")->default_str("all");
    verifyPreApply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyPreFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyPreFile).lexically_normal();
        std::exit(RunPreApplyVerify(workspaceRoot, planPath, *verifyPreStage));
    });

    auto* verifyPostApply = verify->add_subcommand("post-apply", "Verify result state after apply");
    auto* verifyPostFile = new std::string{};
    auto* verifyPostStage = new std::string{"all"};
    verifyPostApply->add_option("--plan-file", *verifyPostFile, "Plan file path");
    verifyPostApply->add_option("--stage", *verifyPostStage, "Stage: ignore|commit|all")->default_str("all");
    verifyPostApply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyPostFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyPostFile).lexically_normal();
        std::exit(RunPostApplyVerify(planPath, *verifyPostStage));
    });

    auto* verifyIgnore = verify->add_subcommand("ignore", "Run ignore gate verification");
    auto* verifyIgnoreRoot = new std::string{};
    auto* verifyIgnoreContext = new std::string{"plan"};
    auto* verifyIgnoreAllowlist = new std::string{};
    auto* verifyIgnoreLimit = new int{20};
    verifyIgnore->add_option("--workspace-root", *verifyIgnoreRoot, "Workspace root path (default: cwd)");
    verifyIgnore->add_option("--context", *verifyIgnoreContext, "Context label (plan|ai-commit)")->default_str("plan");
    verifyIgnore->add_option("--allowlist", *verifyIgnoreAllowlist, "Allowlist file path");
    verifyIgnore->add_option("--limit", *verifyIgnoreLimit, "Max listed candidates")->default_val(20);
    verifyIgnore->callback([=]() {
        const auto workspaceRoot =
            verifyIgnoreRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*verifyIgnoreRoot).lexically_normal();
        std::exit(RunIgnoreGate(workspaceRoot, *verifyIgnoreContext, *verifyIgnoreAllowlist, *verifyIgnoreLimit));
    });

    auto* verifySecret = verify->add_subcommand("secret", "Run secret/token gate verification");
    auto* verifySecretRoot = new std::string{};
    auto* verifySecretContext = new std::string{"plan"};
    auto* verifySecretRules = new std::string{};
    auto* verifySecretLimit = new int{20};
    verifySecret->add_option("--workspace-root", *verifySecretRoot, "Workspace root path (default: cwd)");
    verifySecret->add_option("--context", *verifySecretContext, "Context label (plan|ai-commit|commit-push)")->default_str("plan");
    verifySecret->add_option("--rules-file", *verifySecretRules, "Rule file path (format: id|regex)");
    verifySecret->add_option("--limit", *verifySecretLimit, "Max listed findings")->default_val(20);
    verifySecret->callback([=]() {
        const auto workspaceRoot =
            verifySecretRoot->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*verifySecretRoot).lexically_normal();
        std::exit(RunSecretGate(workspaceRoot, *verifySecretContext, *verifySecretRules, *verifySecretLimit));
    });

    auto* schemaVerify = cmd->add_subcommand("schema-verify", "Verify plan schema (pre-apply)");
    schemaVerify->group("");
    auto* verifyFile = new std::string{};
    auto* verifyStage = new std::string{"all"};
    schemaVerify->add_option("--plan-file", *verifyFile, "Plan file path");
    schemaVerify->add_option("--stage", *verifyStage, "Stage: ignore|commit|all")->default_str("all");
    schemaVerify->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = verifyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*verifyFile).lexically_normal();
        std::exit(RunPreApplyVerify(workspaceRoot, planPath, *verifyStage));
    });

    auto* resultVerify = cmd->add_subcommand("result-verify", "Verify apply result state (post-apply)");
    resultVerify->group("");
    auto* resultVerifyFile = new std::string{};
    auto* resultVerifyStage = new std::string{"all"};
    resultVerify->add_option("--plan-file", *resultVerifyFile, "Plan file path");
    resultVerify->add_option("--stage", *resultVerifyStage, "Stage: ignore|commit|all")->default_str("all");
    resultVerify->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = resultVerifyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*resultVerifyFile).lexically_normal();
        std::exit(RunPostApplyVerify(planPath, *resultVerifyStage));
    });

    auto* apply = cmd->add_subcommand("apply", "Apply plan stages");
    apply->allow_extras();
    auto* applyFile = new std::string{};
    auto* applyStage = new std::string{"all"};
    apply->add_option("--plan-file", *applyFile, "Plan file path");
    apply->add_option("--stage", *applyStage, "Stage: ignore|commit|all")->default_str("all");
    apply->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto planPath = applyFile->empty() ? DefaultPlanPath(workspaceRoot) : std::filesystem::path(*applyFile).lexically_normal();
        std::exit(RunPlanApply(workspaceRoot, planPath, *applyStage, apply->remaining()));
    });
}

void RegisterIgnore(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("ignore", "Ignore management commands");
    auto* doctor = cmd->add_subcommand("doctor", "Scan tracked files for likely ignore candidates");
    auto* repo = new std::string{};
    auto* limit = new int{200};
    auto* asJson = new bool{false};
    auto* apply = new bool{false};
    auto* dryRun = new bool{false};
    auto* yes = new bool{false};
    doctor->add_option("--repo", *repo, "Workspace/repo root path");
    doctor->add_option("--limit", *limit, "Max findings")->default_val(200);
    doctor->add_flag("--json", *asJson, "Output JSON");
    doctor->add_flag("--apply", *apply, "Apply untrack (git rm --cached) to findings");
    doctor->add_flag("--dry-run", *dryRun, "Print apply actions only");
    doctor->add_flag("--yes,-y", *yes, "Skip interactive confirmation gate");
    doctor->callback([=]() {
        const auto root = repo->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*repo).lexically_normal();
        std::exit(RunIgnoreDoctor(root, *limit, *asJson, *apply, *dryRun, *yes));
    });
}

} // namespace kano::git::commands
