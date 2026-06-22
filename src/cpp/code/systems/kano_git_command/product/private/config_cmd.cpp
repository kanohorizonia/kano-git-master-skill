// config command — read/write/unset layered kog_config.toml settings

#include <CLI/CLI.hpp>
#include "kog_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace kano::git::commands {

namespace {

auto ResolveSkillRoot(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    if (const char* envRoot = std::getenv("KANO_GIT_SKILL_ROOT"); envRoot != nullptr && std::string(envRoot).size() > 0) {
        return std::filesystem::path(envRoot).lexically_normal();
    }

    const auto workspaceRoot = InWorkspaceRoot.lexically_normal();
    const auto localSystemConfig = (workspaceRoot / ".kano" / "kog_config.toml").lexically_normal();
    std::error_code ec;
    if (std::filesystem::exists(localSystemConfig, ec) && !ec) {
        return workspaceRoot;
    }

    auto tryHomeCandidate = [&](const char* baseRaw) -> std::filesystem::path {
        if (baseRaw == nullptr || *baseRaw == '\0') {
            return {};
        }
        const auto candidate = (std::filesystem::path(baseRaw)
            / ".agents" / "skills" / "kano" / "kano-git-master-skill").lexically_normal();
        const auto skillMarker = (candidate / "SKILL.md").lexically_normal();
        std::error_code candidateEc;
        if (std::filesystem::exists(skillMarker, candidateEc) && !candidateEc
            && std::filesystem::is_regular_file(skillMarker, candidateEc)) {
            return candidate;
        }
        return {};
    };

    if (const auto candidate = tryHomeCandidate(std::getenv("USERPROFILE")); !candidate.empty()) {
        return candidate;
    }
    if (const auto candidate = tryHomeCandidate(std::getenv("HOME")); !candidate.empty()) {
        return candidate;
    }

    return {};
}

auto ResolveTargetConfigPath(const std::filesystem::path& InWorkspaceRoot,
                             bool InLocal, bool InGlobal, bool InSystem) -> std::filesystem::path {
    if (InSystem) {
        return kog_config::SystemConfigPath(ResolveSkillRoot(InWorkspaceRoot));
    }
    if (InGlobal) {
        return kog_config::GlobalConfigPath();
    }
    // Default to local
    return kog_config::LocalConfigPath(InWorkspaceRoot);
}

auto ScopeLabel(bool InLocal, bool InGlobal, bool InSystem) -> std::string {
    if (InSystem) {
        return "system";
    }
    if (InGlobal) {
        return "global";
    }
    return "local";
}

} // namespace

void RegisterConfig(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("config",
        "Read, write, or unset layered kog_config.toml settings.\n\n"
        "Usage:\n"
        "  kog config <key>               Get effective value (layered)\n"
        "  kog config <key> <value>        Set value (default: --local)\n"
        "  kog config --get <key>          Explicit get\n"
        "  kog config --set <key> <value>  Explicit set\n"
        "  kog config --unset <key>        Remove key from target layer\n"
        "  kog config --list               List effective config (all layers merged)\n\n"
        "Layers (system < global < local):\n"
        "  system: <skill_root>/.kano/kog_config.toml\n"
        "  global: ~/.kano/kog_config.toml\n"
        "  local:  .kano/kog_config.toml\n\n"
        "Known Keys & Values:\n"
        "  ai.model.selection\n"
        "      \"auto\"             Use provider-native auto first; fallback to KOG auto when unsupported (default)\n"
        "      \"provider-auto\"    Force provider-native auto mode\n"
        "      \"kog-auto\"         Force KOG change-count auto routing\n"
        "      \"provider-default\" Use the provider's built-in recommended fixed model\n"
        "      \"provider/model\"   Exact model reference (e.g., \"copilot/gpt-5.4\")\n"
        "      \"model\"            Bare model name for the active provider\n"
        "  ai.model.kog_auto.change_thresholds\n"
        "      Array of integers defining change volume boundaries (e.g., [5, 10])\n"
        "  ai.model.kog_auto.models\n"
        "      Array of strings matching thresholds (e.g., [\"...-mini\", \"...-haiku\", \"...-gpt-5.4\"])\n"
        "  ai.model.auto.* (deprecated)\n"
        "      Backward-compatible legacy keys. Prefer ai.model.kog_auto.*\n"
        "  workspace.external.roots\n"
        "      Array of external repo roots merged into workspace discovery (e.g., [\"~/.agents/skills\"])\n"
        "  workspace.external.inherit\n"
        "      true|false. When false, the current layer replaces inherited external roots instead of appending\n"
        "  export.upload.default_target\n"
        "      Target name selected by `kog export upload` when --target is omitted\n"
  "  export.upload.targets.<name>.*\n"
        "      Upload target config: type, path, remote, destination, layout, copy_manifest, copy_sha256, return_url, public_link\n"
        "  plan.ai.commit_generation_mode\n"
        "      \"single\"           One AI pass generates all commit entries\n"
        "      \"per-commit\"       One AI pass per commit entry\n"
        "      \"adaptive\"         Per-commit with fallback for simple changes (default)");

    // Positional arguments: key [value]
    auto* positionalArgs = new std::vector<std::string>{};
    cmd->add_option("args", *positionalArgs, "Config key and optional value")
        ->expected(0, 2);

    // Scope flags
    auto* flagLocal  = new bool{false};
    auto* flagGlobal = new bool{false};
    auto* flagSystem = new bool{false};

    auto* optLocal  = cmd->add_flag("--local",  *flagLocal,  "Target .kano/kog_config.toml (default for writes)");
    auto* optGlobal = cmd->add_flag("--global", *flagGlobal, "Target ~/.kano/kog_config.toml");
    auto* optSystem = cmd->add_flag("--system", *flagSystem, "Target system kog_config.toml");
    optLocal->excludes(optGlobal)->excludes(optSystem);
    optGlobal->excludes(optLocal)->excludes(optSystem);
    optSystem->excludes(optLocal)->excludes(optGlobal);

    // Operation flags
    auto* flagGet    = new bool{false};
    auto* flagSet    = new bool{false};
    auto* flagUnset  = new bool{false};
    auto* flagList   = new bool{false};
    auto* flagShowOrigin = new bool{false};

    cmd->add_flag("--get",   *flagGet,   "Read a config value");
    cmd->add_flag("--set",   *flagSet,   "Write a config value");
    cmd->add_flag("--unset", *flagUnset, "Remove a config key");
    cmd->add_flag("--list,-l",  *flagList,  "List config keys");
    cmd->add_flag("--show-origin", *flagShowOrigin, "Prefix output with layer label");

    cmd->set_help_flag("");
    auto* flagHelp = new bool{false};
    cmd->add_flag("-h,--help", *flagHelp, "Print this help message and exit");

    cmd->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto skillRoot = ResolveSkillRoot(workspaceRoot);
        const auto& args = *positionalArgs;

        if (*flagHelp) {
            if (!args.empty()) {
                const auto& key = args[0];
                if (key == "ai.model.selection") {
                    std::cout << "ai.model.selection\n"
                              << "  \"auto\"             Use provider-native auto first; fallback to kog-auto (default)\n"
                              << "  \"provider-auto\"    Force provider-native auto mode\n"
                              << "  \"kog-auto\"         Force KOG change-count auto routing\n"
                              << "  \"provider-default\" Use the provider's built-in recommended fixed default model\n"
                              << "  \"provider/model\"   Exact model reference (e.g., \"copilot/gpt-5.4\")\n"
                              << "  \"model\"            Bare model name for the active provider\n\n"
                              << "Available Copilot Models (fetched live from 'copilot --help'):\n";
                    
                    const auto models = kog_config::GetKnownModelsForProvider("copilot");
                    if (models.empty()) {
                        std::cout << "  <none identified>\n";
                    } else {
                        std::cout << "  ";
                        for (std::size_t i = 0; i < models.size(); ++i) {
                            if (i != 0) std::cout << ", ";
                            std::cout << models[i];
                        }
                        std::cout << "\n";
                    }
                    std::cout << "\nExperimental Providers (hidden from catalog):\n"
                              << "  codex, opencode\n";
                } else if (key == "ai.model.kog_auto.change_thresholds" || key == "ai.model.auto.change_thresholds") {
                    std::cout << "ai.model.kog_auto.change_thresholds\n"
                              << "  Array of integers defining change volume boundaries (e.g., [5, 10])\n";
                } else if (key == "ai.model.kog_auto.models" || key == "ai.model.auto.models") {
                    std::cout << "ai.model.kog_auto.models\n"
                              << "  Array of strings matching thresholds (e.g., [\"...-mini\", \"...-haiku\", \"...-gpt-5.4\"])\n";
                } else if (key == "plan.ai.commit_generation_mode") {
                    std::cout << "plan.ai.commit_generation_mode\n"
                              << "  \"single\"           One AI pass generates all commit entries\n"
                              << "  \"per-commit\"       One AI pass per commit entry\n"
                              << "  \"adaptive\"         Per-commit with fallback for simple changes (default)\n";
                } else if (key == "workspace.external.roots") {
                    std::cout << "workspace.external.roots\n"
                              << "  Array of external repo roots merged into workspace discovery.\n"
                              << "  Example: [\"~/.agents/skills\"]\n"
                              << "  Commands like `kog status` will scan each configured root and merge repo candidates as\n"
                              << "  external-root, external-registered, or external-unregistered depending on how they relate\n"
                              << "  to the discovered external workspace root. Plain `external` is no longer emitted.\n"
                              << "  Here, `root` means the matched external discovery root repo, not the current workspace root.\n";
                } else if (key == "workspace.external.inherit") {
                    std::cout << "workspace.external.inherit\n"
                              << "  true  -> append this layer's roots to inherited ones (default)\n"
                              << "  false -> replace inherited external roots at this layer\n";
                } else if (key == "export.upload" || key == "export.upload.default_target" ||
                           key.rfind("export.upload.targets.", 0) == 0) {
                    std::cout << "export.upload\n"
                              << "  User config: ~/.kano/kog_config.toml\n"
                              << "  Repo config: .kano/kog_config.toml\n"
                              << "  Precedence: user < repo < CLI\n"
                              << "  default_target selects a named export.upload.targets.<name> table.\n\n"
                              << "Example local sync target:\n"
                              << "  [export.upload]\n"
                              << "  default_target = \"drive_sync\"\n"
                              << "  [export.upload.targets.drive_sync]\n"
                              << "  type = \"local-sync-folder\"\n"
                              << "  # Existing Google Drive/Desktop sync root; kog creates only layout subdirs below it.\n"
                              << "  path = \"/path/to/sync/root\"\n"
                              << "  layout = \"Kano/kog\"\n"
                              << "  copy_manifest = true\n"
                              << "  copy_sha256 = true\n"
                              << "  return_url = false\n\n"
                              << "Example rclone target:\n"
                              << "  [export.upload.targets.gdrive]\n"
                              << "  type = \"rclone\"\n"
                              << "  remote = \"kog-drive\"\n"
                              << "  destination = \"exports/kog\"\n"
                              << "  layout = \"ChatGPT_Export\"\n"
                              << "  copy_manifest = true\n"
                              << "  copy_sha256 = true\n"
                              << "  return_url = true\n\n"
                              << "ChatGPT Google connector URL behavior:\n"
                              << "  local-sync-folder never invents a cloud URL; use the synced folder in Google Drive/ChatGPT.\n"
                              << "  rclone returns a private Google Drive file URL only when lsjson --stat -M exposes a Drive ID; otherwise URL_UNAVAILABLE is recorded.\n\n"
                              << "Safety:\n"
                              << "  Uploads preserve private/default backend permissions.\n"
                              << "  Public links require explicit CLI flags: kog export upload --public-link --yes.\n"
                              << "  rclone link is never called by default because it may create/retrieve public sharing permissions.\n"
                              << "  Config-file public_link/yes values do not enable permission mutation.\n";
                } else {
                    std::cout << "No detailed help available for config key: " << key << "\n"
                              << "Run 'kog config --help' to see known keys.\n";
                }
            } else {
                std::cout << cmd->help();
            }
            std::exit(0);
        }

        // --- --list mode ---
        if (*flagList) {
            if (*flagLocal || *flagGlobal || *flagSystem) {
                // List a specific layer
                const auto targetPath = ResolveTargetConfigPath(workspaceRoot, *flagLocal, *flagGlobal, *flagSystem);
                if (targetPath.empty()) {
                    std::cerr << "ERROR: Could not resolve config path for scope "
                              << ScopeLabel(*flagLocal, *flagGlobal, *flagSystem) << "\n";
                    std::exit(1);
                }
                const auto entries = kog_config::ListTomlKeys(targetPath);
                for (const auto& [key, value] : entries) {
                    if (*flagShowOrigin) {
                        std::cout << ScopeLabel(*flagLocal, *flagGlobal, *flagSystem)
                                  << ":" << targetPath.generic_string() << "\t";
                    }
                    std::cout << key << "=" << value << "\n";
                }
                return;
            }

            // No scope flag: show effective merged config from all layers
            struct LayerInfo {
                std::string label;
                std::filesystem::path path;
            };
            const std::vector<LayerInfo> layers = {
                {"system", kog_config::SystemConfigPath(skillRoot)},
                {"global", kog_config::GlobalConfigPath()},
                {"local",  kog_config::LocalConfigPath(workspaceRoot)},
            };

            if (*flagShowOrigin) {
                // Show all entries per layer with origin labels
                for (const auto& layer : layers) {
                    if (layer.path.empty()) {
                        continue;
                    }
                    const auto entries = kog_config::ListTomlKeys(layer.path);
                    for (const auto& [key, value] : entries) {
                        std::cout << layer.label << ":" << layer.path.generic_string()
                                  << "\t" << key << "=" << value << "\n";
                    }
                }
            } else {
                // Merge: later layers override earlier ones (system < global < local)
                std::map<std::string, std::string> merged;
                for (const auto& layer : layers) {
                    if (layer.path.empty()) {
                        continue;
                    }
                    const auto entries = kog_config::ListTomlKeys(layer.path);
                    for (const auto& [key, value] : entries) {
                        merged[key] = value;
                    }
                }
                for (const auto& [key, value] : merged) {
                    std::cout << key << "=" << value << "\n";
                }
            }
            return;
        }

        // --- --unset mode ---
        if (*flagUnset) {
            if (args.empty()) {
                std::cerr << "ERROR: --unset requires a <key> argument.\n";
                std::exit(2);
            }
            if (*flagSystem) {
                std::cerr << "ERROR: --system config is read-only. "
                          << "Edit " << kog_config::SystemConfigPath(skillRoot).generic_string()
                          << " manually if needed.\n";
                std::exit(1);
            }
            const auto& key = args[0];
            const auto targetPath = ResolveTargetConfigPath(workspaceRoot, *flagLocal, *flagGlobal, *flagSystem);
            if (targetPath.empty()) {
                std::cerr << "ERROR: Could not resolve config path for scope "
                          << ScopeLabel(*flagLocal, *flagGlobal, *flagSystem) << "\n";
                std::exit(1);
            }
            if (!kog_config::UnsetTomlKey(targetPath, key)) {
                std::cerr << "ERROR: Failed to unset key '" << key << "' from "
                          << targetPath.generic_string() << "\n";
                std::exit(1);
            }
            return;
        }

        // --- --set mode (explicit) ---
        if (*flagSet) {
            if (args.size() < 2) {
                std::cerr << "ERROR: --set requires <key> <value> arguments.\n";
                std::exit(2);
            }
            if (*flagSystem) {
                std::cerr << "ERROR: --system config is read-only. "
                          << "Edit " << kog_config::SystemConfigPath(skillRoot).generic_string()
                          << " manually if needed.\n";
                std::exit(1);
            }
            const auto& key = args[0];
            const auto& value = args[1];
            const auto targetPath = ResolveTargetConfigPath(workspaceRoot, *flagLocal, *flagGlobal, *flagSystem);
            if (targetPath.empty()) {
                std::cerr << "ERROR: Could not resolve config path for scope "
                          << ScopeLabel(*flagLocal, *flagGlobal, *flagSystem) << "\n";
                std::exit(1);
            }
            if (!kog_config::WriteTomlValue(targetPath, key, value)) {
                std::cerr << "ERROR: Failed to write key '" << key << "' to "
                          << targetPath.generic_string() << "\n";
                std::exit(1);
            }
            return;
        }

        // --- --get mode (explicit) ---
        if (*flagGet) {
            if (args.empty()) {
                std::cerr << "ERROR: --get requires a <key> argument.\n";
                std::exit(2);
            }
            const auto& key = args[0];
            if (*flagLocal || *flagGlobal || *flagSystem) {
                const auto targetPath = ResolveTargetConfigPath(workspaceRoot, *flagLocal, *flagGlobal, *flagSystem);
                const auto value = kog_config::ReadTomlValue(targetPath, key);
                if (value.empty()) {
                    std::exit(1);
                }
                std::cout << value << "\n";
            } else {
                const auto value = kog_config::ReadEffectiveValue(workspaceRoot, skillRoot, key);
                if (value.empty()) {
                    std::exit(1);
                }
                std::cout << value << "\n";
            }
            return;
        }

        // --- Positional: kog config <key> [<value>] ---
        if (args.empty()) {
            // No arguments: show help
            std::cout << cmd->help() << "\n";
            return;
        }

        if (args.size() == 1) {
            // Read mode: kog config <key>
            const auto& key = args[0];
            if (*flagLocal || *flagGlobal || *flagSystem) {
                const auto targetPath = ResolveTargetConfigPath(workspaceRoot, *flagLocal, *flagGlobal, *flagSystem);
                const auto value = kog_config::ReadTomlValue(targetPath, key);
                if (value.empty()) {
                    std::exit(1);
                }
                std::cout << value << "\n";
            } else {
                const auto value = kog_config::ReadEffectiveValue(workspaceRoot, skillRoot, key);
                if (value.empty()) {
                    std::exit(1);
                }
                std::cout << value << "\n";
            }
            return;
        }

        // Write mode: kog config <key> <value>
        {
            if (*flagSystem) {
                std::cerr << "ERROR: --system config is read-only. "
                          << "Edit " << kog_config::SystemConfigPath(skillRoot).generic_string()
                          << " manually if needed.\n";
                std::exit(1);
            }
            const auto& key = args[0];
            const auto& value = args[1];
            // Default to local for writes
            const bool effectiveLocal = !*flagGlobal && !*flagSystem;
            const auto targetPath = ResolveTargetConfigPath(workspaceRoot, effectiveLocal, *flagGlobal, *flagSystem);
            if (targetPath.empty()) {
                std::cerr << "ERROR: Could not resolve config path for scope "
                          << ScopeLabel(effectiveLocal, *flagGlobal, *flagSystem) << "\n";
                std::exit(1);
            }
            if (!kog_config::WriteTomlValue(targetPath, key, value)) {
                std::cerr << "ERROR: Failed to write key '" << key << "' to "
                          << targetPath.generic_string() << "\n";
                std::exit(1);
            }
        }
    });
}

} // namespace kano::git::commands
