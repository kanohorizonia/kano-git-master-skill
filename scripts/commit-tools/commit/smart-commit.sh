#!/usr/bin/env bash
#
# smart-commit.sh - AI-powered safe commit across all repositories
#
# Purpose:
#   Commit changes across root repo, submodules, and nested repos with:
#   - AI-generated commit messages (multi-provider support)
#   - Safety checks (secrets, large files, conflicts)
#   - Auto .gitignore updates (smart-ignore + legacy fallback)
#   - Optional AI review gate
#
# Supported Providers:
#   - OpenCode (opencode)
#   - Codex (codex)
#   - Copilot (copilot)
#
# Usage:
#   ./smart-commit.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --agent <name>             Execution identity (manual|codex|copilot|cursor|kiro|claude|...)
#   --ai-review                 Enable AI safety review (default: on)
#   --no-ai-review              Disable AI safety review
#   -m, --message <text>        Fixed commit message (skip AI generation)
#   -f, --push                  Push after commit with --force-with-lease
#   --max-file-size-mb <int>    Block files larger than this (default: 5)
#   --list-models [provider]    List available models
#   --clear-cache [provider]    Clear model cache (all or specific provider)
#   -h, --help                  Show help
#
# Examples:
#   # List all available models
#   ./smart-commit.sh --list-models
#
#   # Use OpenCode with auto model
#   ./smart-commit.sh --provider opencode --model auto
#
#   # Use Codex with specific model
#   ./smart-commit.sh --provider codex --model gpt-5.3-codex
#
#   # Use Copilot with gpt-5-mini
#   ./smart-commit.sh --provider copilot --model gpt-5-mini
#
#   # Commit and push
#   ./smart-commit.sh --provider opencode --model auto --push
#
#   # Clear model cache
#   ./smart-commit.sh --clear-cache
#
# Wrapper Scripts (Convenience):
#   ./smart-commit-opencode.sh  # Uses OpenCode with auto model
#   ./smart-commit-codex.sh     # Uses Codex with gpt-5.3-codex
#   ./smart-commit-copilot.sh   # Uses Copilot with gpt-5-mini-mini
#

set -euo pipefail

# Detect if running under agent control (Copilot, Cursor, etc.)
# Agent mode should suppress interactive prompts
if [[ "${KANO_AGENT_MODE:-}" != "1" ]]; then
  # Set agent mode if we detect running via agent protocol (non-TTY, stdin pipe, etc.)
  if ! [[ -t 0 && -t 1 ]]; then
    export KANO_AGENT_MODE=1
  fi
fi

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

# Find repository root (go up from commit-tools/commit/)
if [[ -n "${KANO_GIT_MASTER_ROOT:-}" ]]; then
  ROOT="$(cd "$KANO_GIT_MASTER_ROOT" && pwd)"
else
  ROOT="$(cd "$SCRIPT_DIR/../../.." && git rev-parse --show-toplevel 2>/dev/null || true)"
  if [[ -z "$ROOT" ]]; then
    # Fallback: assume we're in skills/kano-git-master-skill/scripts/commit-tools/commit/
    ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
  fi
fi

# Load AI provider library
LIB_DIR="$SCRIPT_DIR/../lib"
if [[ ! -f "$LIB_DIR/ai-providers.sh" ]]; then
  echo "ERROR: AI provider library not found: $LIB_DIR/ai-providers.sh" >&2
  exit 1
fi
source "$LIB_DIR/ai-providers.sh"

# Configuration
AI_PROVIDER=""  # Required: opencode, codex, copilot, claude
AI_MODEL=""     # Required: model name
COMMIT_MESSAGE=""
DO_PUSH=0
MAX_FILE_SIZE_MB=5
AI_REVIEW=1
CUSTOM_RULES=""
RULES_FILE=""
REPO_FILTER=""  # Comma-separated list of repo paths to include
USE_SMART_IGNORE=1
VERBOSE=0        # Show all repos (default: show only repos with changes)
AGENT_ID=""      # Execution identity: manual or agent name
PROMPT_MODE="auto"   # auto|dev|user
PROMPT_ROOT="$SKILL_ROOT/prompts"
LIST_REPOS_ONLY=0

# Environment variable overrides (CLI args still take precedence):
#   KOG_RULES_TEXT    -> same as --rules
#   KOG_RULES_FILE    -> same as --rules-file
#   KOG_PROMPT_MODE   -> same as --prompt-mode
#   KOG_PROMPT_ROOT   -> same as --prompt-root
if [[ -n "${KOG_RULES_TEXT:-}" ]]; then
  CUSTOM_RULES="$KOG_RULES_TEXT"
fi
if [[ -n "${KOG_RULES_FILE:-}" ]]; then
  RULES_FILE="$KOG_RULES_FILE"
fi
if [[ -n "${KOG_PROMPT_MODE:-}" ]]; then
  PROMPT_MODE="$KOG_PROMPT_MODE"
fi
if [[ -n "${KOG_PROMPT_ROOT:-}" ]]; then
  PROMPT_ROOT="$KOG_PROMPT_ROOT"
fi

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit.sh --provider <name> --model <name> [options]

AI-powered safe commit across all repositories (root + submodules + nested repos).

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --agent <name>             Execution identity. Use "manual" for human-run commands.
  --ai-review                 Enable AI safety review (default: on)
  --no-ai-review              Disable AI safety review
  -m, --message <text>        Fixed commit message (skip AI generation)
  -f, --push                  Push after commit with --force-with-lease
  --max-file-size-mb <int>    Block files larger than this (default: 5)
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  --prompt-mode <mode>        Prompt mode: auto|dev|user (default: auto)
  --prompt-root <path>        Prompt templates root (default: <skill>/prompts)
  --repos <paths>             Only process specific repos (comma-separated paths)
  --smart-ignore              Use smart-ignore.sh for .gitignore updates (default: on)
  --no-smart-ignore           Use legacy inline .gitignore updater only
  --verbose                   Show all repos (default: show only repos with changes)
  --list-repos                List discovered repos (with type) and exit
  --list-models [provider]    List available models (all or specific provider)
  --clear-cache [provider]    Clear model cache (all or specific provider)
  -h, --help                  Show help

Examples:
  # List all available models
  ./smart-commit.sh --list-models

  # List models for specific provider
  ./smart-commit.sh --list-models copilot

  # Use OpenCode with auto model
  ./smart-commit.sh --provider opencode --model auto

  # Use Codex with specific model
  ./smart-commit.sh --provider codex --model gpt-5.3-codex

  # Use Copilot with gpt-5-mini
  ./smart-commit.sh --provider copilot --model gpt-5-mini

  # Custom message (no AI needed)
  ./smart-commit.sh --provider copilot --model gpt-5-mini -m "feat: Add feature"

  # Agent proxy mode (代理模式) (cost-safe): requires --message and auto-disables AI review
  ./smart-commit.sh --agent codex -m "chore: update workspace"

  # With custom rules (inline)
  ./smart-commit.sh --provider copilot --model gpt-5-mini --rules "Use emoji prefixes"

  # With custom rules (from file)
  ./smart-commit.sh --provider copilot --model gpt-5-mini --rules-file .git/commit-rules.md

  # File-based auto rules (no args):
  # - kano-git-master-skill repo -> dev.rule.md
  # - other repos                -> commit-convention skill auto-discovery, then default.rule.md

  # Prompt mode override
  ./smart-commit.sh --provider copilot --model gpt-5-mini --prompt-mode user

  # Only process specific repos
  ./smart-commit.sh --provider copilot --model gpt-5-mini --repos ".,submodules/my-lib"

  # Commit and push
  ./smart-commit.sh --provider opencode --model auto --push

  # Skip AI review
  ./smart-commit.sh --provider codex --model gpt-5.3-codex --no-ai-review

  # Clear model cache
  ./smart-commit.sh --clear-cache

  # Clear cache for specific provider
  ./smart-commit.sh --clear-cache copilot

Requirements:
  Install at least one AI provider:
  - OpenCode: https://opencode.ai/docs/cli/
  - Codex: npm install -g @openai/codex
  - Copilot: npm install -g @githubnext/github-copilot-cli

Wrapper Scripts:
  Use provider-specific wrappers for convenience:
  - ./smart-commit-opencode.sh
  - ./smart-commit-codex.sh
  - ./smart-commit-copilot.sh

Environment variables (optional):
  KOG_RULES_TEXT              Default value for --rules
  KOG_RULES_FILE              Default value for --rules-file
  KOG_PROMPT_MODE             Default value for --prompt-mode (auto|dev|user)
  KOG_PROMPT_ROOT             Default value for --prompt-root

Precedence:
  CLI arguments > environment variables > built-in defaults
EOF
}

# Show helpful error message if provider not available
show_provider_error() {
  cat >&2 <<'EOF'
ERROR: AI provider not specified or not available

This script requires an AI provider for commit message generation.

Usage:
  ./smart-commit.sh --provider <name> --model <name>

Available providers:
  - opencode: https://opencode.ai/docs/cli/
  - codex: npm install -g @openai/codex
  - copilot: npm install -g @githubnext/github-copilot-cli

List available models:
  ./smart-commit.sh --list-models

Or use manual commit message:
  ./smart-commit.sh --provider copilot --model gpt-5-mini -m "your message"

Or use wrapper scripts:
  ./smart-commit-opencode.sh
  ./smart-commit-codex.sh
  ./smart-commit-copilot.sh
EOF
  exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --provider)
      AI_PROVIDER="${2:-}"
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      shift 2
      ;;
    --list-models)
      # List models and exit
      list_available_models "${2:-}" "false"
      exit 0
      ;;
    --clear-cache)
      # Clear cache and exit
      clear_cache "${2:-}"
      exit 0
      ;;
    --ai-review)
      AI_REVIEW=1
      shift
      ;;
    --agent)
      AGENT_ID="${2:-}"
      shift 2
      ;;
    --no-ai-review)
      AI_REVIEW=0
      shift
      ;;
    -m|--message)
      COMMIT_MESSAGE="${2:-}"
      shift 2
      ;;
    -f|--push)
      DO_PUSH=1
      shift
      ;;
    --max-file-size-mb)
      MAX_FILE_SIZE_MB="${2:-}"
      shift 2
      ;;
    --rules)
      CUSTOM_RULES="${2:-}"
      shift 2
      ;;
    --rules-file)
      RULES_FILE="${2:-}"
      shift 2
      ;;
    --prompt-mode)
      PROMPT_MODE="${2:-}"
      shift 2
      ;;
    --prompt-root)
      PROMPT_ROOT="${2:-}"
      shift 2
      ;;
    --repos)
      REPO_FILTER="${2:-}"
      shift 2
      ;;
    --smart-ignore)
      USE_SMART_IGNORE=1
      shift
      ;;
    --no-smart-ignore)
      USE_SMART_IGNORE=0
      shift
      ;;
    --verbose)
      VERBOSE=1
      shift
      ;;
    --list-repos)
      LIST_REPOS_ONLY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# Agent Proxy Contract:
# - --agent <name> where name != manual means agent proxy execution.
# - agent proxy mode (代理模式) must provide fixed commit message.
# - agent proxy mode (代理模式) forces --no-ai-review to avoid duplicate model invocation.
# - intent: agent-handled workflow should not trigger a second in-script review model.
if [[ -n "$AGENT_ID" ]]; then
  AGENT_ID="$(printf '%s' "$AGENT_ID" | tr '[:upper:]' '[:lower:]')"
fi

if [[ -n "$AGENT_ID" && "$AGENT_ID" != "manual" ]]; then
  if [[ -z "$COMMIT_MESSAGE" ]]; then
    echo "ERROR: agent proxy run requires -m/--message (agent: $AGENT_ID)" >&2
    echo "       In proxy mode, commit message/review are handled by the current agent model." >&2
    exit 1
  fi

  if [[ "$AI_REVIEW" -eq 1 ]]; then
    echo "INFO: agent proxy run detected (agent: $AGENT_ID), forcing --no-ai-review"
  fi
  AI_REVIEW=0
fi

# Validate required parameters
if [[ "$LIST_REPOS_ONLY" -eq 0 ]] && { [[ -z "$COMMIT_MESSAGE" ]] || [[ "$AI_REVIEW" -eq 1 ]]; }; then
  # AI features needed, validate provider and model
  if [[ -z "$AI_PROVIDER" ]]; then
    echo "ERROR: --provider is required" >&2
    show_provider_error
  fi

  if [[ -z "$AI_MODEL" ]]; then
    echo "ERROR: --model is required" >&2
    show_provider_error
  fi

  # Validate provider is available
  case "$AI_PROVIDER" in
    opencode)
      if ! detect_opencode; then
        echo "ERROR: opencode not found. Install: https://opencode.ai/docs/cli/" >&2
        exit 1
      fi
      ;;
    codex)
      if ! detect_codex; then
        echo "ERROR: codex not found. Install: npm install -g @openai/codex" >&2
        exit 1
      fi
      ;;
    copilot)
      if ! detect_copilot; then
        echo "ERROR: copilot not found. Install: npm install -g @githubnext/github-copilot-cli" >&2
        exit 1
      fi
      ;;
    *)
      echo "ERROR: Unknown provider: $AI_PROVIDER" >&2
      echo "Valid providers: opencode, codex, copilot" >&2
      exit 1
      ;;
  esac

  echo "Using Provider: $AI_PROVIDER"
  echo "Using Model: $AI_MODEL"
fi

# Validate max file size
if ! [[ "$MAX_FILE_SIZE_MB" =~ ^[0-9]+$ ]]; then
  echo "ERROR: --max-file-size-mb must be an integer" >&2
  exit 1
fi

# Validate prompt mode
case "$PROMPT_MODE" in
  auto|dev|user)
    ;;
  *)
    echo "ERROR: --prompt-mode must be one of: auto, dev, user" >&2
    exit 1
    ;;
esac

MAX_FILE_SIZE_BYTES=$((MAX_FILE_SIZE_MB * 1024 * 1024))

# Setup temp directory
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Repository list
declare -a REPOS=()
REPO_LIST_FILE="$TMP_DIR/repos.txt"
touch "$REPO_LIST_FILE"

# Commit statistics: format "repo_name|commit_count|branch"
declare -a COMMIT_STATS=()
TIMER_TOTAL_START=0
TIMER_DISCOVERY=0
TIMER_PROCESS=0
DISCOVERED_COMMIT_SKILL_RULES_FILE=""
DISCOVERED_REVIEW_SKILL_RULES_FILE=""
DISCOVERED_COMMIT_SKILL_RULES_DONE=0
DISCOVERED_REVIEW_SKILL_RULES_DONE=0

resolve_revision_count() {
  local repo="$1"
  local branch="$2"
  if [[ -n "$branch" && "$branch" != "(detached)" ]] && git -C "$repo" show-ref --verify --quiet "refs/heads/$branch" 2>/dev/null; then
    git -C "$repo" rev-list --count "$branch" 2>/dev/null || echo "0"
    return 0
  fi
  git -C "$repo" rev-list --count HEAD 2>/dev/null || echo "0"
}

timer_now() {
  date +%s
}

format_duration() {
  local seconds="${1:-0}"
  local h m s
  h=$((seconds / 3600))
  m=$(((seconds % 3600) / 60))
  s=$((seconds % 60))
  if [[ "$h" -gt 0 ]]; then
    printf '%02dh %02dm %02ds' "$h" "$m" "$s"
  elif [[ "$m" -gt 0 ]]; then
    printf '%02dm %02ds' "$m" "$s"
  else
    printf '%02ds' "$s"
  fi
}

print_timing_summary() {
  local total_elapsed
  total_elapsed=$(( $(timer_now) - TIMER_TOTAL_START ))
  echo ""
  echo "=== Timing Summary ==="
  printf "%-16s  %8s  %s\n" "Phase" "Seconds" "Duration"
  printf "%-16s  %8s  %s\n" "-----" "-------" "-----"
  printf "%-16s  %8s  %s\n" "discovery" "$TIMER_DISCOVERY" "$(format_duration "$TIMER_DISCOVERY")"
  printf "%-16s  %8s  %s\n" "commit-process" "$TIMER_PROCESS" "$(format_duration "$TIMER_PROCESS")"
  printf "%-16s  %8s  %s\n" "total" "$total_elapsed" "$(format_duration "$total_elapsed")"
}

#------------------------------------------------------------------------------
# Repository Discovery
#------------------------------------------------------------------------------

add_repo() {
  local repo="$1"
  if [[ -z "$repo" ]]; then
    return
  fi
  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    return
  fi
  repo="$(cd "$repo" && pwd)"
  if grep -Fxq "$repo" "$REPO_LIST_FILE" 2>/dev/null; then
    return
  fi
  printf '%s\n' "$repo" >>"$REPO_LIST_FILE"
  REPOS+=("$repo")
}

discover_repositories() {
  local discover_script="$SCRIPT_DIR/../../core/discover-repos.sh"
  local repos_json=""
  local repo_obj=""
  local path=""

  # Preferred path: unified discovery implementation with explicit type support.
  if [[ -f "$discover_script" ]]; then
    repos_json="$(bash "$discover_script" --root "$ROOT" --format json --include-types root,registered,unregistered --max-depth 12 2>/dev/null || true)"
    if [[ "$repos_json" == \[*\] ]]; then
      while IFS= read -r repo_obj; do
        [[ -z "$repo_obj" ]] && continue
        path="$(printf '%s' "$repo_obj" | sed -n 's/.*"path":"\([^"]*\)".*/\1/p')"
        [[ -z "$path" ]] && continue
        add_repo "$path"
      done < <(echo "$repos_json" | grep -o '{[^}]*}')
    fi
  fi

  # Fallback: legacy local discovery if shared discover script is unavailable.
  if [[ "${#REPOS[@]}" -eq 0 ]]; then
    add_repo "$ROOT"

    while IFS= read -r line; do
      [[ -z "$line" ]] && continue
      add_repo "$ROOT/$line"
    done < <(git -C "$ROOT" config --file .gitmodules --get-regexp path 2>/dev/null | awk '{print $2}' || true)

    while IFS= read -r git_marker; do
      [[ -z "$git_marker" ]] && continue
      add_repo "$(dirname "$git_marker")"
    done < <(find "$ROOT" -type d -name .git -prune -print -o -type f -name .git -print 2>/dev/null || true)
  fi
}

print_discovery_summary() {
  local root_count=0
  local registered_count=0
  local unregistered_count=0
  local repo=""

  for repo in "${REPOS[@]}"; do
    if [[ "$repo" == "$ROOT" ]]; then
      ((root_count++)) || true
    elif is_registered_repo "$repo"; then
      ((registered_count++)) || true
    else
      ((unregistered_count++)) || true
    fi
  done

  echo "Discovery: total=${#REPOS[@]} root=${root_count} registered=${registered_count} unregistered=${unregistered_count}"
}

is_registered_repo() {
  local repo="$1"
  local rel="${repo#$ROOT/}"
  if [[ "$rel" == "$repo" ]]; then
    return 1
  fi
  git -C "$ROOT" config --file .gitmodules --get-regexp path 2>/dev/null | awk '{print $2}' | grep -Fxq "$rel"
}

print_repo_list_and_exit() {
  local repo=""
  local type=""
  for repo in "${REPOS[@]}"; do
    if [[ "$repo" == "$ROOT" ]]; then
      type="root"
    elif is_registered_repo "$repo"; then
      type="registered"
    else
      type="unregistered"
    fi
    echo "${type}: ${repo}"
  done
  exit 0
}

#------------------------------------------------------------------------------
# .gitignore Management
#------------------------------------------------------------------------------

ensure_gitignore_entry() {
  local repo="$1"
  local entry="$2"
  local gitignore="$repo/.gitignore"

  if [[ -f "$gitignore" ]] && grep -Fxq "$entry" "$gitignore"; then
    return
  fi

  printf '%s\n' "$entry" >>"$gitignore"
}

maybe_update_gitignore() {
  local repo="$1"
  local smart_ignore_script="$SCRIPT_DIR/../ignore/smart-ignore.sh"
  local smart_err=""
  local changed=0

  if [[ "$USE_SMART_IGNORE" -eq 1 && -x "$smart_ignore_script" ]]; then
    # Check if .gitignore exists and get its hash before
    local gitignore_hash_before=""
    if [[ -f "$repo/.gitignore" ]]; then
      gitignore_hash_before="$(md5sum "$repo/.gitignore" 2>/dev/null | awk '{print $1}' || true)"
    fi

    local smart_ignore_args=(--repo "$repo" --scope untracked --no-consolidate)
    if [[ -n "$AI_PROVIDER" && -n "$AI_MODEL" ]]; then
      smart_ignore_args+=(--provider "$AI_PROVIDER" --model "$AI_MODEL")
    else
      smart_ignore_args+=(--no-ai)
    fi

    smart_err="$(mktemp)"
    if "$smart_ignore_script" "${smart_ignore_args[@]}" >/dev/null 2>"$smart_err"; then
      # Check if .gitignore actually changed
      local gitignore_hash_after=""
      if [[ -f "$repo/.gitignore" ]]; then
        gitignore_hash_after="$(md5sum "$repo/.gitignore" 2>/dev/null | awk '{print $1}' || true)"
      fi

      if [[ -n "$gitignore_hash_after" && "$gitignore_hash_before" != "$gitignore_hash_after" ]]; then
        echo "[$repo] .gitignore updated via smart-ignore"
        changed=1
      fi
      rm -f "$smart_err"
      return
    fi

    echo "[$repo] WARNING: smart-ignore failed, falling back to legacy updater" >&2
    if grep -qiE "auth|login|not logged|unauthorized|forbidden|api key|token" "$smart_err" 2>/dev/null; then
      echo "[$repo] smart-ignore likely failed due to AI provider authentication" >&2
      case "$AI_PROVIDER" in
        copilot)
          echo "[$repo] Hint: run 'gh auth login' (or pass --no-ai-review for commit review only)." >&2
          ;;
        codex)
          echo "[$repo] Hint: run 'codex login' or export OPENAI_API_KEY." >&2
          ;;
        opencode)
          echo "[$repo] Hint: run 'opencode auth login' and verify provider credentials." >&2
          ;;
      esac
    fi
    rm -f "$smart_err"
  fi

  local path=""

  # Check untracked files for common patterns
  while IFS= read -r path; do
    case "$path" in
      .env|.env.*|*.local|*.log|*.tmp|*.swp|*.swo|*.bak|*.pid|*.sqlite|*.sqlite3|*.db|.DS_Store|Thumbs.db)
        ensure_gitignore_entry "$repo" "$path"
        changed=1
        ;;
      node_modules/*|dist/*|build/*|coverage/*|.venv/*|venv/*|__pycache__/*|.pytest_cache/*|.mypy_cache/*|.ruff_cache/*|tmp/*)
        ensure_gitignore_entry "$repo" "${path%%/*}/"
        changed=1
        ;;
    esac
  done < <(git -C "$repo" ls-files --others --exclude-standard 2>/dev/null || true)

  # Check for nested git repos
  local nested_git=""
  local nested_repo=""
  local rel=""
  while IFS= read -r nested_git; do
    nested_repo="$(dirname "$nested_git")"
    if [[ "$nested_repo" == "$repo" ]]; then
      continue
    fi
    rel="${nested_repo#$repo/}"
    if [[ "$rel" == "$nested_repo" ]]; then
      continue
    fi
    if git -C "$repo" check-ignore -q "$rel/" 2>/dev/null; then
      continue
    fi
    if git -C "$repo" ls-files --error-unmatch "$rel" >/dev/null 2>&1; then
      continue
    fi
    ensure_gitignore_entry "$repo" "$rel/"
    changed=1
  done < <(find "$repo" -type d -name .git -prune -print -o -type f -name .git -print 2>/dev/null || true)

  if [[ "$changed" -eq 1 ]]; then
    # Only print .gitignore updated if from legacy method (smart-ignore already printed if it changed)
    if [[ "$USE_SMART_IGNORE" -ne 1 ]]; then
      echo "[$repo] .gitignore updated"
    fi
  fi
}

provider_auth_hint() {
  case "$AI_PROVIDER" in
    copilot)
      echo "Hint: run 'gh auth login' (and ensure Copilot access is enabled)."
      ;;
    codex)
      echo "Hint: run 'codex login' or export OPENAI_API_KEY."
      ;;
    opencode)
      echo "Hint: run 'opencode auth login' and verify provider credentials."
      ;;
    *)
      echo "Hint: verify provider login/credentials, then retry."
      ;;
  esac
}

provider_auth_likely_missing() {
  case "$AI_PROVIDER" in
    copilot)
      # Check standalone copilot first (modern installations often use this)
      if have_cmd copilot \
         && copilot -p "PASS" --no-color --stream off --no-ask-user 2>/dev/null \
         | grep -qi "PASS"; then
        return 1
      fi
      # Then check gh extension auth
      if have_cmd gh \
         && gh auth status >/dev/null 2>&1 \
         && gh copilot --version >/dev/null 2>&1; then
        return 1
      fi
      return 0
      ;;
    codex)
      if [[ -z "${OPENAI_API_KEY:-}" ]]; then
        return 0
      fi
      ;;
    opencode)
      if [[ -z "${OPENAI_API_KEY:-}" && -z "${OPENROUTER_API_KEY:-}" && -z "${ANTHROPIC_API_KEY:-}" ]]; then
        return 0
      fi
      ;;
  esac
  return 1
}

#------------------------------------------------------------------------------
# Safety Checks
#------------------------------------------------------------------------------

run_safety_checks() {
  local repo="$1"
  local check_file="$TMP_DIR/check-$(echo "$repo" | sed 's#[^a-zA-Z0-9]#_#g').txt"

  # Ensure core.fileMode is disabled to prevent accidental chmod commits
  local current_filemode
  current_filemode="$(git -C "$repo" config --get core.fileMode 2>/dev/null || echo "")"
  if [[ "$current_filemode" != "false" ]]; then
    git -C "$repo" config core.fileMode false 2>/dev/null || true
  fi

  # Unstage mode-only changes (defensive measure in case fileMode was enabled before)
  local mode_only_count=0
  while IFS= read -r file; do
    if git -C "$repo" diff --cached --numstat "$file" 2>/dev/null | grep -q "^0[[:space:]]0[[:space:]]"; then
      git -C "$repo" restore --staged "$file" 2>/dev/null || true
      ((mode_only_count++)) || true
    fi
  done < <(git -C "$repo" diff --cached --name-only --diff-filter=M 2>/dev/null || true)

  if [[ "$mode_only_count" -gt 0 ]]; then
    echo "[$repo] Unstaged $mode_only_count mode-only change(s) (chmod ignored)" >&2
  fi

  # Check for conflict markers and whitespace issues
  if ! git -C "$repo" diff --cached --check >"$check_file" 2>&1; then
    # Check if it's only trailing whitespace (not conflict markers)
    if grep -q "^<<<<<<< \|^=======$\|^>>>>>>> " "$check_file"; then
      echo "[$repo] FAILED: Conflict markers detected" >&2
      cat "$check_file" >&2
      return 1
    else
      # Only trailing whitespace - auto-fix it
      echo "[$repo] Auto-fixing trailing whitespace..." >&2

      # Get list of staged files with trailing whitespace
      local fixed_count=0
      while IFS= read -r file; do
        if [[ -f "$repo/$file" ]]; then
          # Remove trailing whitespace from the file
          sed -i 's/[[:space:]]*$//' "$repo/$file" 2>/dev/null || \
            sed -i '' 's/[[:space:]]*$//' "$repo/$file" 2>/dev/null || true

          # Re-stage the fixed file
          git -C "$repo" add "$file" 2>/dev/null || true
          ((fixed_count++)) || true
        fi
      done < <(git -C "$repo" diff --cached --name-only 2>/dev/null || true)

      if [[ "$fixed_count" -gt 0 ]]; then
        echo "[$repo] Fixed trailing whitespace in $fixed_count file(s)" >&2
      fi
    fi
  fi

  # Check staged files
  local path=""
  local blob_size=0
  while IFS= read -r path; do
    # Check for sensitive file patterns
    case "$path" in
      *.pem|*.key|*.p12|*.pfx|*.jks|*.kubeconfig|*id_rsa*|*id_dsa*|*id_ed25519*)
        echo "[$repo] FAILED: Private key file staged: $path" >&2
        return 1
        ;;
      .env|.env.*|*secrets*.env|*secret*.env)
        echo "[$repo] FAILED: Secret env file staged: $path" >&2
        return 1
        ;;
      *auth-profiles.json|*credentials*.json)
        echo "[$repo] WARNING: Sensitive file staged: $path" >&2
        ;;
    esac

    # Check file size
    blob_size="$(git -C "$repo" cat-file -s ":$path" 2>/dev/null || echo 0)"
    if (( blob_size > MAX_FILE_SIZE_BYTES )); then
      echo "[$repo] FAILED: File too large (${blob_size} bytes > ${MAX_FILE_SIZE_BYTES}): $path" >&2
      return 1
    fi
  done < <(git -C "$repo" diff --cached --name-only --diff-filter=ACMR 2>/dev/null || true)

  # Check for secrets only in added lines to avoid false positives when removing old examples.
  if git -C "$repo" diff --cached --text --unified=0 2>/dev/null \
    | grep -E '^\+' \
    | grep -v -E '^\+\+\+' \
    | grep -E -i \
      "(AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|ghp_[0-9A-Za-z]{36}|github_pat_[0-9A-Za-z_]{80,}|xox[baprs]-[0-9A-Za-z-]{10,}|-----BEGIN (RSA|EC|OPENSSH|DSA|PGP) PRIVATE KEY-----|api[_-]?key[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,}|token[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,}|password[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,}|secret[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,})" \
    >"$check_file" 2>&1; then
    echo "[$repo] FAILED: Possible secret detected in diff" >&2
    cat "$check_file" >&2
    return 1
  fi

  return 0
}

#------------------------------------------------------------------------------
# AI Functions
#------------------------------------------------------------------------------

sanitize_message() {
  local msg="$1"
  msg="$(printf '%s' "$msg" | tr '\r\n' ' ' | sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//')"
  printf '%s' "$msg"
}

is_git_master_repo() {
  local repo="$1"
  local name
  name="$(basename "$repo")"
  if [[ "$name" == "kano-git-master-skill" ]]; then
    return 0
  fi
  if [[ "$repo" == *"/kano-git-master-skill"* ]]; then
    return 0
  fi
  return 1
}

# Auto-discover commit/review rules from installed "commit convention" skills.
# Priority is still lower than explicit --rules/--rules-file and repo/root defaults.
discover_skill_rules_file() {
  local purpose="${1:-commit}"
  local cached_value=""
  local cached_done=0
  local skills_root="$ROOT/skills"
  local skill_md=""
  local skill_dir=""
  local skill_name=""
  local references_dir=""
  local candidate=""
  local -a exact_candidates=()

  if [[ "$purpose" == "review" ]]; then
    cached_done="$DISCOVERED_REVIEW_SKILL_RULES_DONE"
    cached_value="$DISCOVERED_REVIEW_SKILL_RULES_FILE"
  else
    cached_done="$DISCOVERED_COMMIT_SKILL_RULES_DONE"
    cached_value="$DISCOVERED_COMMIT_SKILL_RULES_FILE"
  fi

  if [[ "$cached_done" -eq 1 ]]; then
    if [[ -n "$cached_value" ]]; then
      printf '%s' "$cached_value"
      return 0
    fi
    return 1
  fi

  if [[ ! -d "$skills_root" ]]; then
    if [[ "$purpose" == "review" ]]; then
      DISCOVERED_REVIEW_SKILL_RULES_DONE=1
    else
      DISCOVERED_COMMIT_SKILL_RULES_DONE=1
    fi
    return 1
  fi

  while IFS= read -r skill_md; do
    skill_dir="$(dirname "$skill_md")"
    skill_name="$(basename "$skill_dir" | tr '[:upper:]' '[:lower:]')"
    references_dir="$skill_dir/references"

    # Only consider skills that look like commit-convention providers.
    if ! [[ "$skill_name" =~ commit.*convention|conventional.*commit ]]; then
      if ! grep -Eiq 'commit[[:space:]-]*convention|conventional[[:space:]-]*commit|commit message' "$skill_md" 2>/dev/null; then
        continue
      fi
    fi

    if [[ "$purpose" == "review" ]]; then
      exact_candidates=(
        "$references_dir/review.rule.md"
        "$references_dir/review-rules.md"
        "$references_dir/default.rule.md"
      )
    else
      exact_candidates=(
        "$references_dir/commit.rule.md"
        "$references_dir/commit-rules.md"
        "$references_dir/default.rule.md"
      )
    fi

    for candidate in "${exact_candidates[@]}"; do
      if [[ -f "$candidate" ]]; then
        if [[ "$purpose" == "review" ]]; then
          DISCOVERED_REVIEW_SKILL_RULES_FILE="$candidate"
          DISCOVERED_REVIEW_SKILL_RULES_DONE=1
        else
          DISCOVERED_COMMIT_SKILL_RULES_FILE="$candidate"
          DISCOVERED_COMMIT_SKILL_RULES_DONE=1
        fi
        printf '%s' "$candidate"
        return 0
      fi
    done

    if [[ -d "$references_dir" ]]; then
      if [[ "$purpose" == "review" ]]; then
        candidate="$(find "$references_dir" -maxdepth 2 -type f \
          \( -iname "*review*rule*.md" -o -iname "*review*rules*.md" \) | sort | head -n 1)"
      else
        candidate="$(find "$references_dir" -maxdepth 2 -type f \
          \( -iname "*commit*convention*.md" -o -iname "*conventional*commit*.md" -o -iname "*commit*rule*.md" -o -iname "*commit*rules*.md" \) | sort | head -n 1)"
      fi
      if [[ -n "$candidate" && -f "$candidate" ]]; then
        if [[ "$purpose" == "review" ]]; then
          DISCOVERED_REVIEW_SKILL_RULES_FILE="$candidate"
          DISCOVERED_REVIEW_SKILL_RULES_DONE=1
        else
          DISCOVERED_COMMIT_SKILL_RULES_FILE="$candidate"
          DISCOVERED_COMMIT_SKILL_RULES_DONE=1
        fi
        printf '%s' "$candidate"
        return 0
      fi
    fi
  done < <(find "$skills_root" -mindepth 2 -maxdepth 4 -type f -name "SKILL.md" | sort)

  if [[ "$purpose" == "review" ]]; then
    DISCOVERED_REVIEW_SKILL_RULES_DONE=1
  else
    DISCOVERED_COMMIT_SKILL_RULES_DONE=1
  fi
  return 1
}

resolve_rules_file_for_repo() {
  local repo="$1"
  local purpose="${2:-commit}"
  local default_rule_name=""
  local candidate=""

  # Inline rules bypass file resolution.
  if [[ -n "$CUSTOM_RULES" ]]; then
    return 1
  fi

  # Explicit file takes precedence.
  if [[ -n "$RULES_FILE" ]]; then
    for candidate in "$RULES_FILE" "$repo/$RULES_FILE" "$ROOT/$RULES_FILE"; do
      if [[ -f "$candidate" ]]; then
        printf '%s' "$candidate"
        return 0
      fi
    done
    return 1
  fi

  # Git-master repo keeps explicit dev rules first.
  if is_git_master_repo "$repo"; then
    for candidate in "$repo/dev.rule.md" "$ROOT/dev.rule.md" "$SKILL_ROOT/references/dev.rule.md"; do
      if [[ -f "$candidate" ]]; then
        printf '%s' "$candidate"
        return 0
      fi
    done
  fi

  # Skill-level convention fallback (auto-discovered) for non-dev repos.
  candidate="$(discover_skill_rules_file "$purpose" || true)"
  if [[ -n "$candidate" && -f "$candidate" ]]; then
    printf '%s' "$candidate"
    return 0
  fi

  # Final default fallback.
  default_rule_name="default.rule.md"
  for candidate in "$repo/$default_rule_name" "$ROOT/$default_rule_name" "$SKILL_ROOT/references/$default_rule_name"; do
    if [[ -f "$candidate" ]]; then
      printf '%s' "$candidate"
      return 0
    fi
  done

  return 1
}

load_rules_text_for_repo() {
  local repo="$1"
  local purpose="${2:-commit}"
  local rules_path=""

  if [[ -n "$CUSTOM_RULES" ]]; then
    printf '%s' "$CUSTOM_RULES"
    return 0
  fi

  rules_path="$(resolve_rules_file_for_repo "$repo" "$purpose" || true)"
  if [[ -z "$rules_path" ]]; then
    return 1
  fi

  cat "$rules_path"
  return 0
}

resolve_prompt_mode_for_repo() {
  local repo="$1"
  if [[ "$PROMPT_MODE" != "auto" ]]; then
    printf '%s' "$PROMPT_MODE"
    return 0
  fi
  if is_git_master_repo "$repo"; then
    printf 'dev'
  else
    printf 'user'
  fi
}

load_prompt_template_for_stage() {
  local repo="$1"
  local stage="$2"
  local mode=""
  local prompt_root="$PROMPT_ROOT"
  local base_file=""
  local mode_file=""
  local combined=""

  if [[ -n "$prompt_root" && "$prompt_root" != /* ]]; then
    prompt_root="$ROOT/$prompt_root"
  fi

  mode="$(resolve_prompt_mode_for_repo "$repo")"
  base_file="$prompt_root/base/$stage.md"
  mode_file="$prompt_root/$mode/$stage.md"

  if [[ ! -f "$base_file" ]]; then
    return 1
  fi

  combined="$(cat "$base_file")"
  if [[ -f "$mode_file" ]]; then
    combined="$combined

--- MODE OVERLAY ($mode) ---
$(cat "$mode_file")"
  fi

  printf '%s' "$combined"
  return 0
}

build_prompt() {
  local repo="$1"
  local stat files custom_rules_text rules_source_path prompt_template prompt_mode
  stat="$(git -C "$repo" diff --cached --shortstat 2>/dev/null || echo "no stats")"
  files="$(git -C "$repo" diff --cached --name-status 2>/dev/null || echo "no files")"
  prompt_mode="$(resolve_prompt_mode_for_repo "$repo")"
  prompt_template="$(load_prompt_template_for_stage "$repo" "commit-message" || true)"

  custom_rules_text="$(load_rules_text_for_repo "$repo" "commit" || true)"
  rules_source_path="$(resolve_rules_file_for_repo "$repo" "commit" || true)"
  if [[ -n "$prompt_template" ]]; then
    echo "[$repo] Using commit prompt mode: $prompt_mode" >&2
  fi
  if [[ -n "$rules_source_path" ]]; then
    echo "[$repo] Using rules from: $rules_source_path" >&2
  fi

  # Preferred: file-based prompt template
  if [[ -n "$prompt_template" ]]; then
    cat <<EOF
$prompt_template

$(if [[ -n "$custom_rules_text" ]]; then
  cat <<RULES
Commit Rules:
$custom_rules_text

RULES
fi)

Repository: $(basename "$repo")
Stats: $stat
Files changed:
$files
EOF
  # Fallback: legacy in-script prompt
  elif [[ -n "$custom_rules_text" ]]; then
    cat <<EOF
Generate one concise commit message for this git change.

Custom Rules:
$custom_rules_text

Default Rules (if not overridden above):
- Output only one line, no quotes, no markdown
- Use format: type(scope): summary
- Choose type from: feat, fix, refactor, chore, docs, test, ci, build, perf, style
- Keep summary under 72 characters

Repository: $(basename "$repo")
Stats: $stat
Files changed:
$files
EOF
  else
    cat <<EOF
Generate one concise Conventional Commit message for this git change.

Rules:
- Output only one line, no quotes, no markdown
- Use format: type(scope): summary
- Choose type from: feat, fix, refactor, chore, docs, test, ci, build, perf, style
- Keep summary under 72 characters

Repository: $(basename "$repo")
Stats: $stat
Files changed:
$files
EOF
  fi
}

ai_message_from_provider() {
  local prompt="$1"
  local response=""

  response="$(ai_generate_message_first_line "$AI_PROVIDER" "$AI_MODEL" "$prompt" || true)"
  printf '%s' "$response"
}

fallback_message() {
  local repo="$1"
  local scope
  scope="$(basename "$repo")"
  printf 'chore(%s): update changes' "$scope"
}

generate_message() {
  local repo="$1"
  local prompt raw msg

  # Use fixed message if provided
  if [[ -n "$COMMIT_MESSAGE" ]]; then
    printf '%s' "$COMMIT_MESSAGE"
    return 0
  fi

  # Generate with AI
  prompt="$(build_prompt "$repo")"
  raw="$(ai_message_from_provider "$prompt" || true)"

  msg="$(sanitize_message "$raw")"
  if [[ -z "$msg" ]]; then
    msg="$(fallback_message "$repo")"
  fi

  printf '%s' "$msg"
}

build_review_prompt() {
  local repo="$1"
  local stat files diff_preview review_rules_text review_rules_source prompt_template prompt_mode
  stat="$(git -C "$repo" diff --cached --shortstat 2>/dev/null || echo "no stats")"
  files="$(git -C "$repo" diff --cached --name-status 2>/dev/null || echo "no files")"
  diff_preview="$(git -C "$repo" diff --cached --unified=0 --text 2>/dev/null | head -n 300 || echo "no diff")"
  review_rules_text="$(load_rules_text_for_repo "$repo" "review" || true)"
  review_rules_source="$(resolve_rules_file_for_repo "$repo" "review" || true)"
  prompt_mode="$(resolve_prompt_mode_for_repo "$repo")"
  prompt_template="$(load_prompt_template_for_stage "$repo" "review" || true)"

  if [[ -n "$prompt_template" ]]; then
    echo "[$repo] Using review prompt mode: $prompt_mode" >&2
  fi
  if [[ -n "$review_rules_source" ]]; then
    echo "[$repo] Using review rules from: $review_rules_source" >&2
  fi

  if [[ -n "$prompt_template" ]]; then
    cat <<EOF
$prompt_template

$(if [[ -n "$review_rules_text" ]]; then
  cat <<RULES
Review Rules:
$review_rules_text

RULES
fi)

Repository: $(basename "$repo")
Stats: $stat
Files:
$files

Patch preview (first 300 lines):
$diff_preview
EOF
  else
    cat <<EOF
You are a git commit safety reviewer.
Decide if this staged change is safe to commit.

$(if [[ -n "$review_rules_text" ]]; then
  cat <<RULES
Review Rules:
$review_rules_text

RULES
fi)

Context:
- This repository is a Git automation toolkit. It may legitimately add scripts that:
  - discover repos/submodules in a workspace
  - sync a fork with upstream
  - push to origin using --force-with-lease (fork maintenance)
- Do NOT automatically FAIL solely because the patch contains "git push --force-with-lease".
  Instead, verify the change is guarded and intentional.

Focus on:
- Secrets, credentials, tokens, API keys
- Private keys, certificates
- Accidental binaries or large files
- Risky unintended changes
- Generated files that shouldn't be committed

When automation scripts are changed, PASS is reasonable when most of these are true:
- Force pushes use --force-with-lease (not --force) and target a specific branch (not --mirror).
- The script is user-invoked (no hidden background execution), and supports --dry-run for preview.
- The script checks for clean working tree and required remotes before rewriting history or pushing.
- Multi-repo behavior is opt-in or filtered (e.g., only repos with an upstream remote are affected).

FAIL if you see any of these:
- Adds or encourages committing secrets/credentials, or exfiltrating data.
- Uses plain --force / --mirror / pushes to arbitrary remotes/branches without safeguards.
- Removes safety checks (clean tree, remote checks, dry-run) around destructive Git operations.

Output format (strict):
PASS: <short reason>
or
FAIL: <short reason>

Output only one line.

Repository: $(basename "$repo")
Stats: $stat
Files:
$files

Patch preview (first 300 lines):
$diff_preview
EOF
  fi
}

run_ai_review() {
  local repo="$1"
  local prompt raw first verdict
  local first_trimmed="" first_upper="" raw_upper=""

  if [[ "$AI_REVIEW" -ne 1 ]]; then
    return 0
  fi

  prompt="$(build_review_prompt "$repo")"
  raw="$(ai_generate_message_first_line "$AI_PROVIDER" "$AI_MODEL" "$prompt" || true)"

  if [[ -z "$raw" ]]; then
    echo "[$repo] WARNING: AI review unavailable, skipping gate (fail-open)" >&2
    if provider_auth_likely_missing; then
      echo "[$repo] AI provider auth appears missing for: $AI_PROVIDER" >&2
      echo "[$repo] $(provider_auth_hint)" >&2
    else
      echo "[$repo] AI provider returned empty response (provider/model outage or CLI issue)." >&2
    fi
    return 0
  fi

  first="$(printf '%s\n' "$raw" | sed 's/\r$//' | grep -m1 -E '[^[:space:]]' || true)"
  first_trimmed="$(printf '%s' "$first" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
  first_upper="$(printf '%s' "$first_trimmed" | tr '[:lower:]' '[:upper:]')"
  raw_upper="$(printf '%s' "$raw" | tr '[:lower:]' '[:upper:]')"
  verdict=""

  # Preferred strict parsing from first non-empty line.
  if [[ "$first_upper" =~ ^PASS([[:space:]]*[:\-].*)?$ ]]; then
    verdict="PASS"
  elif [[ "$first_upper" =~ ^FAIL([[:space:]]*[:\-].*)?$ ]]; then
    verdict="FAIL"
  fi

  # Fuzzy fallback for small-model formatting glitches.
  if [[ -z "$verdict" ]]; then
    if [[ "$raw_upper" =~ (^|[^A-Z])FAIL([^A-Z]|$) ]] && [[ ! "$raw_upper" =~ (^|[^A-Z])PASS([^A-Z]|$) ]]; then
      verdict="FAIL"
    elif [[ "$raw_upper" =~ (^|[^A-Z])PASS([^A-Z]|$) ]] && [[ ! "$raw_upper" =~ (^|[^A-Z])FAIL([^A-Z]|$) ]]; then
      verdict="PASS"
    fi
  fi

  if [[ "$verdict" == "PASS" ]]; then
    echo "[$repo] AI review: ${first_trimmed:-PASS}"
    return 0
  fi

  if [[ "$verdict" == "FAIL" ]]; then
    echo "[$repo] AI review BLOCKED: ${first_trimmed:-FAIL}" >&2
    return 1
  fi

  echo "[$repo] WARNING: AI review returned invalid verdict, skipping gate (fail-open)" >&2
  if [[ -n "$first_trimmed" ]]; then
    echo "[$repo] AI output: $first_trimmed" >&2
  fi
  return 0
}

#------------------------------------------------------------------------------
# Commit Logic
#------------------------------------------------------------------------------

detect_default_branch() {
  local repo="$1"
  local remote=""
  local head_ref=""
  local branch=""

  for remote in origin upstream; do
    if ! git -C "$repo" remote get-url "$remote" >/dev/null 2>&1; then
      continue
    fi

    head_ref="$(git -C "$repo" symbolic-ref --quiet "refs/remotes/$remote/HEAD" 2>/dev/null || true)"
    if [[ -n "$head_ref" ]]; then
      branch="${head_ref#refs/remotes/$remote/}"
      if [[ -n "$branch" ]]; then
        printf '%s|%s' "$remote" "$branch"
        return 0
      fi
    fi

    for branch in main master dev; do
      if git -C "$repo" show-ref --verify --quiet "refs/remotes/$remote/$branch"; then
        printf '%s|%s' "$remote" "$branch"
        return 0
      fi
    done
  done

  return 1
}

detect_branch_from_superproject_gitmodules() {
  local repo="$1"
  local current=""
  local rel_path=""
  local configured_branch=""

  current="$(dirname "$repo")"
  while [[ -n "$current" ]]; do
    if [[ -f "$current/.gitmodules" ]]; then
      rel_path="${repo#$current/}"
      if [[ "$rel_path" != "$repo" ]]; then
        configured_branch="$(git -C "$current" config -f .gitmodules --get "submodule.$rel_path.branch" 2>/dev/null || true)"
        if [[ -n "$configured_branch" ]]; then
          printf '%s' "$configured_branch"
          return 0
        fi
      fi
    fi

    if [[ "$current" == "/" || "$current" == "." ]]; then
      break
    fi
    current="$(dirname "$current")"
  done

  return 1
}

attach_detached_to_default_branch() {
  local repo="$1"
  local preferred_branch=""
  local detected=""
  local remote=""
  local branch=""

  # 1) Prefer branch explicitly configured in the nearest superproject .gitmodules
  preferred_branch="$(detect_branch_from_superproject_gitmodules "$repo" || true)"
  if [[ -n "$preferred_branch" ]]; then
    if git -C "$repo" show-ref --verify --quiet "refs/heads/$preferred_branch"; then
      git -C "$repo" checkout "$preferred_branch" >/dev/null 2>&1 || true
    elif git -C "$repo" show-ref --verify --quiet "refs/remotes/origin/$preferred_branch"; then
      git -C "$repo" checkout -b "$preferred_branch" "origin/$preferred_branch" >/dev/null 2>&1 || true
    elif git -C "$repo" show-ref --verify --quiet "refs/remotes/upstream/$preferred_branch"; then
      git -C "$repo" checkout -b "$preferred_branch" "upstream/$preferred_branch" >/dev/null 2>&1 || true
    fi

    if git -C "$repo" symbolic-ref --quiet --short HEAD >/dev/null 2>&1; then
      echo "[$repo] Attached detached HEAD to .gitmodules branch '$preferred_branch'"
      return 0
    fi
  fi

  # 2) Fallback to remote default branch detection
  detected="$(detect_default_branch "$repo" || true)"
  if [[ -z "$detected" ]]; then
    return 1
  fi

  remote="${detected%%|*}"
  branch="${detected#*|}"
  if [[ -z "$remote" || -z "$branch" ]]; then
    return 1
  fi

  if git -C "$repo" show-ref --verify --quiet "refs/heads/$branch"; then
    git -C "$repo" checkout "$branch" >/dev/null 2>&1 || return 1
  else
    git -C "$repo" checkout -b "$branch" "$remote/$branch" >/dev/null 2>&1 || return 1
  fi

  echo "[$repo] Attached detached HEAD to $branch (from $remote/$branch)"
  return 0
}

commit_repo() {
  local repo="$1"
  local msg branch

  # Check for ongoing merge/rebase
  local merge_head rebase_merge rebase_apply
  merge_head="$(git -C "$repo" rev-parse --git-path MERGE_HEAD 2>/dev/null || true)"
  rebase_merge="$(git -C "$repo" rev-parse --git-path rebase-merge 2>/dev/null || true)"
  rebase_apply="$(git -C "$repo" rev-parse --git-path rebase-apply 2>/dev/null || true)"

  if [[ -f "$merge_head" || -d "$rebase_merge" || -d "$rebase_apply" ]]; then
    if [[ "$VERBOSE" -eq 1 ]]; then
      echo "[$repo] SKIP: Merge/rebase in progress"
    fi
    return 0
  fi

  # Avoid creating commits on detached HEAD: try to attach to default branch first.
  branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
  if [[ -z "$branch" ]]; then
    if ! attach_detached_to_default_branch "$repo"; then
      echo "[$repo] SKIP: Detached HEAD (could not attach to default branch)" >&2
      return 0
    fi
  fi

  # Update .gitignore
  maybe_update_gitignore "$repo"

  # Stage all changes
  git -C "$repo" add -A 2>/dev/null || true

  # Check if there are staged changes
  if git -C "$repo" diff --cached --quiet 2>/dev/null; then
    if [[ "$VERBOSE" -eq 1 ]]; then
      echo "[$repo] SKIP: No changes"
    fi
    return 0
  fi

  # Print header only when there's work to do
  if [[ "$VERBOSE" -eq 1 ]]; then
    echo ""
    echo "=== Processing: $repo ==="
  else
    echo "[$repo] Processing..."
  fi

  # Run safety checks
  if ! run_safety_checks "$repo"; then
    echo "[$repo] ABORTED: Safety checks failed" >&2
    return 1
  fi

  # Run AI review (only if we have staged changes)
  if ! run_ai_review "$repo"; then
    echo "[$repo] ABORTED: AI review failed" >&2
    return 1
  fi

  # Generate commit message
  msg="$(generate_message "$repo")"
  if [[ -z "$msg" ]]; then
    echo "[$repo] FAILED: Could not generate commit message" >&2
    return 1
  fi

  echo "[$repo] Commit: $msg"
  git -C "$repo" commit -m "$msg" 2>/dev/null || {
    echo "[$repo] FAILED: Commit failed" >&2
    return 1
  }

  # Record statistics
  local branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || echo "(detached)")"
  local revision
  revision="$(resolve_revision_count "$repo" "$branch")"
  local short_repo="$(basename "$repo")"
  COMMIT_STATS+=("$short_repo|1|$branch|$revision")

  # Push if requested
  if [[ "$DO_PUSH" -eq 1 ]]; then
    branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    if [[ -z "$branch" ]]; then
      echo "[$repo] SKIP push: Detached HEAD"
    else
      echo "[$repo] Pushing '$branch' with --force-with-lease..."
      git -C "$repo" push --force-with-lease origin "$branch" 2>/dev/null || {
        echo "[$repo] WARNING: Push failed" >&2
      }
    fi
  fi

  return 0
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------
TIMER_TOTAL_START="$(timer_now)"
discovery_start="$(timer_now)"

# Discover repositories
if [[ "$VERBOSE" -eq 1 ]]; then
  echo "Discovering repositories under: $ROOT"
fi

discover_repositories

if [[ "${#REPOS[@]}" -eq 0 ]]; then
  echo "No git repositories found under: $ROOT"
  exit 0
fi

print_discovery_summary

if [[ "$LIST_REPOS_ONLY" -eq 1 ]]; then
  print_repo_list_and_exit
fi

# Apply repo filter if specified
if [[ -n "$REPO_FILTER" ]]; then
  declare -a FILTERED_REPOS=()
  IFS=',' read -ra FILTER_PATHS <<< "$REPO_FILTER"

  for filter_path in "${FILTER_PATHS[@]}"; do
    # Normalize filter path
    filter_path="${filter_path#./}"  # Remove leading ./

    # Convert to absolute path
    if [[ "$filter_path" == "." ]]; then
      filter_abs="$ROOT"
    elif [[ "$filter_path" == /* ]]; then
      filter_abs="$filter_path"
    else
      filter_abs="$ROOT/$filter_path"
    fi
    filter_abs="$(cd "$filter_abs" 2>/dev/null && pwd || echo "$filter_abs")"

    # Check if this repo is in our list
    for repo in "${REPOS[@]}"; do
      if [[ "$repo" == "$filter_abs" ]]; then
        FILTERED_REPOS+=("$repo")
        break
      fi
    done
  done

  if [[ "${#FILTERED_REPOS[@]}" -eq 0 ]]; then
    echo "No repositories match filter: $REPO_FILTER"
    exit 0
  fi

  REPOS=("${FILTERED_REPOS[@]}")
  if [[ "$VERBOSE" -eq 1 ]]; then
    echo "Filtered to ${#REPOS[@]} repositories"
  fi
fi
TIMER_DISCOVERY=$(( $(timer_now) - discovery_start ))
process_start="$(timer_now)"

# Sort repos by depth (deepest first, root last)
declare -a NON_ROOT=()
for repo in "${REPOS[@]}"; do
  if [[ "$repo" != "$ROOT" ]]; then
    NON_ROOT+=("$repo")
  fi
done

if [[ "${#NON_ROOT[@]}" -gt 0 ]]; then
  IFS=$'\n' NON_ROOT=($(for r in "${NON_ROOT[@]}"; do printf '%s\n' "$r"; done | awk '{print length, $0}' | sort -rn | cut -d' ' -f2-))
  unset IFS
fi

# Process non-root repos first
FAILED_COUNT=0
FAILED_REPOS=()

if [[ "${#NON_ROOT[@]}" -gt 0 ]]; then
  for repo in "${NON_ROOT[@]}"; do
    if ! commit_repo "$repo"; then
      ((FAILED_COUNT++)) || true
      FAILED_REPOS+=("$repo")
    fi
  done
fi

# Process root repo last
if ! commit_repo "$ROOT"; then
  ((FAILED_COUNT++)) || true
  FAILED_REPOS+=("$ROOT")
fi
TIMER_PROCESS=$(( $(timer_now) - process_start ))

echo ""
if [[ "$FAILED_COUNT" -gt 0 ]]; then
  echo "=== Completed with failures (${FAILED_COUNT} failed) ==="
  if [[ "$VERBOSE" -eq 1 ]]; then
    echo "Failed repositories:"
    for repo in "${FAILED_REPOS[@]}"; do
      echo "  - $repo"
    done
  fi
  print_timing_summary
  echo "Fix the errors above and rerun smart-commit." >&2
  exit 1
fi

# Show commit summary
if [[ "${#COMMIT_STATS[@]}" -gt 0 ]]; then
  echo ""
  echo "=== Commit Summary ==="
  printf "%-35s  Commits  Branch               Revision\\n" "Repository"
  printf "%-35s  -------  ------               --------\\n" "-----------"
  for stat in "${COMMIT_STATS[@]}"; do
    IFS='|' read -r repo_name commit_count branch revision <<< "$stat"
    printf "%-35s  %-7s  %-19s %s\\n" "$repo_name" "$commit_count" "$branch" "$revision"
  done
fi

print_timing_summary
echo "=== All done (success) ==="
