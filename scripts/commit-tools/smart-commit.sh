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

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find repository root (go up from commit-tools/)
ROOT="$(cd "$SCRIPT_DIR/../.." && git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  # Fallback: assume we're in skills/kano-git-master-skill/scripts/commit-tools/
  ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi

# Load AI provider library
LIB_DIR="$SCRIPT_DIR/lib"
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
  --ai-review                 Enable AI safety review (default: on)
  --no-ai-review              Disable AI safety review
  -m, --message <text>        Fixed commit message (skip AI generation)
  -f, --push                  Push after commit with --force-with-lease
  --max-file-size-mb <int>    Block files larger than this (default: 5)
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  --repos <paths>             Only process specific repos (comma-separated paths)
  --smart-ignore              Use smart-ignore.sh for .gitignore updates (default: on)
  --no-smart-ignore           Use legacy inline .gitignore updater only
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

  # With custom rules (inline)
  ./smart-commit.sh --provider copilot --model gpt-5-mini --rules "Use emoji prefixes"

  # With custom rules (from file)
  ./smart-commit.sh --provider copilot --model gpt-5-mini --rules-file .git/commit-rules.md

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

# Validate required parameters
if [[ -z "$COMMIT_MESSAGE" ]] || [[ "$AI_REVIEW" -eq 1 ]]; then
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

MAX_FILE_SIZE_BYTES=$((MAX_FILE_SIZE_MB * 1024 * 1024))

# Setup temp directory
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Repository list
declare -a REPOS=()
REPO_LIST_FILE="$TMP_DIR/repos.txt"
touch "$REPO_LIST_FILE"

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
  local smart_ignore_script="$SCRIPT_DIR/smart-ignore.sh"
  local smart_err=""

  if [[ "$USE_SMART_IGNORE" -eq 1 && -x "$smart_ignore_script" ]]; then
    local smart_ignore_args=(--repo "$repo" --scope untracked)
    if [[ -n "$AI_PROVIDER" && -n "$AI_MODEL" ]]; then
      smart_ignore_args+=(--provider "$AI_PROVIDER" --model "$AI_MODEL")
    else
      smart_ignore_args+=(--no-ai)
    fi

    smart_err="$(mktemp)"
    if "$smart_ignore_script" "${smart_ignore_args[@]}" >/dev/null 2>"$smart_err"; then
      rm -f "$smart_err"
      echo "[$repo] .gitignore updated via smart-ignore"
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

  local changed=0
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
    echo "[$repo] .gitignore updated"
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
      if have_cmd gh && ! gh auth status >/dev/null 2>&1; then
        return 0
      fi
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

  # Check for secrets in diff
  if git -C "$repo" diff --cached --text 2>/dev/null | grep -E -i \
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

build_prompt() {
  local repo="$1"
  local stat files custom_rules_text
  stat="$(git -C "$repo" diff --cached --shortstat 2>/dev/null || echo "no stats")"
  files="$(git -C "$repo" diff --cached --name-status 2>/dev/null || echo "no files")"

  # Load custom rules
  custom_rules_text=""
  if [[ -n "$RULES_FILE" ]]; then
    if [[ -f "$RULES_FILE" ]]; then
      custom_rules_text="$(cat "$RULES_FILE")"
    elif [[ -f "$repo/$RULES_FILE" ]]; then
      custom_rules_text="$(cat "$repo/$RULES_FILE")"
    else
      echo "WARNING: Rules file not found: $RULES_FILE" >&2
    fi
  elif [[ -n "$CUSTOM_RULES" ]]; then
    custom_rules_text="$CUSTOM_RULES"
  else
    # Auto-detect common rules files
    local auto_rules_files=(
      "$repo/.git/commit-rules.md"
      "$repo/.github/commit-rules.md"
      "$repo/COMMIT_RULES.md"
      "$repo/.commit-rules"
    )
    for rules_file in "${auto_rules_files[@]}"; do
      if [[ -f "$rules_file" ]]; then
        custom_rules_text="$(cat "$rules_file")"
        echo "[$repo] Using rules from: $rules_file" >&2
        break
      fi
    done
  fi

  # Build prompt with or without custom rules
  if [[ -n "$custom_rules_text" ]]; then
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
  local stat files diff_preview
  stat="$(git -C "$repo" diff --cached --shortstat 2>/dev/null || echo "no stats")"
  files="$(git -C "$repo" diff --cached --name-status 2>/dev/null || echo "no files")"
  diff_preview="$(git -C "$repo" diff --cached --unified=0 --text 2>/dev/null | head -n 300 || echo "no diff")"

  cat <<EOF
You are a git commit safety reviewer.
Decide if this staged change is safe to commit.

Focus on:
- Secrets, credentials, tokens, API keys
- Private keys, certificates
- Accidental binaries or large files
- Risky unintended changes
- Generated files that shouldn't be committed

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
}

run_ai_review() {
  local repo="$1"
  local prompt raw first verdict

  if [[ "$AI_REVIEW" -ne 1 ]]; then
    return 0
  fi

  prompt="$(build_review_prompt "$repo")"
  raw="$(ai_generate_message_first_line "$AI_PROVIDER" "$AI_MODEL" "$prompt" || true)"

  if [[ -z "$raw" ]]; then
    echo "[$repo] AI review unavailable, failing closed" >&2
    if provider_auth_likely_missing; then
      echo "[$repo] AI provider auth appears missing for: $AI_PROVIDER" >&2
      echo "[$repo] $(provider_auth_hint)" >&2
    else
      echo "[$repo] AI provider returned empty response (provider/model outage or CLI issue)." >&2
    fi
    echo "[$repo] Use --no-ai-review to bypass" >&2
    return 1
  fi

  first="$(printf '%s\n' "$raw" | head -n 1 | tr -d '\r')"
  verdict="$(printf '%s\n' "$first" | sed -E 's/^([Pp][Aa][Ss][Ss]|[Ff][Aa][Ii][Ll]).*$/\1/')"

  if [[ "$verdict" =~ ^[Pp][Aa][Ss][Ss]$ ]]; then
    echo "[$repo] AI review: $first"
    return 0
  fi

  if [[ "$verdict" =~ ^[Ff][Aa][Ii][Ll]$ ]]; then
    echo "[$repo] AI review BLOCKED: $first" >&2
    return 1
  fi

  echo "[$repo] AI review returned invalid verdict, failing closed" >&2
  if [[ -n "$first" ]]; then
    echo "[$repo] AI output: $first" >&2
  fi
  return 1
}

#------------------------------------------------------------------------------
# Commit Logic
#------------------------------------------------------------------------------

commit_repo() {
  local repo="$1"
  local msg branch

  echo ""
  echo "=== Processing: $repo ==="

  # Check for ongoing merge/rebase
  local merge_head rebase_merge rebase_apply
  merge_head="$(git -C "$repo" rev-parse --git-path MERGE_HEAD 2>/dev/null || true)"
  rebase_merge="$(git -C "$repo" rev-parse --git-path rebase-merge 2>/dev/null || true)"
  rebase_apply="$(git -C "$repo" rev-parse --git-path rebase-apply 2>/dev/null || true)"

  if [[ -f "$merge_head" || -d "$rebase_merge" || -d "$rebase_apply" ]]; then
    echo "[$repo] SKIP: Merge/rebase in progress"
    return 0
  fi

  # Update .gitignore
  maybe_update_gitignore "$repo"

  # Stage all changes
  git -C "$repo" add -A 2>/dev/null || true

  # Check if there are staged changes
  if git -C "$repo" diff --cached --quiet 2>/dev/null; then
    echo "[$repo] SKIP: No changes"
    return 0
  fi

  # Run safety checks
  if ! run_safety_checks "$repo"; then
    echo "[$repo] ABORTED: Safety checks failed" >&2
    return 1
  fi

  # Run AI review
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

# Discover repositories
echo "Discovering repositories under: $ROOT"

# Add root repo
add_repo "$ROOT"

# Add submodules
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  add_repo "$ROOT/$line"
done < <(git -C "$ROOT" config --file .gitmodules --get-regexp path 2>/dev/null | awk '{print $2}' || true)

# Add nested git repos
while IFS= read -r git_marker; do
  [[ -z "$git_marker" ]] && continue
  add_repo "$(dirname "$git_marker")"
done < <(find "$ROOT" -type d -name .git -prune -print -o -type f -name .git -print 2>/dev/null || true)

if [[ "${#REPOS[@]}" -eq 0 ]]; then
  echo "No git repositories found under: $ROOT"
  exit 0
fi

echo "Found ${#REPOS[@]} repositories"

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
  echo "Filtered to ${#REPOS[@]} repositories"
fi

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

echo ""
if [[ "$FAILED_COUNT" -gt 0 ]]; then
  echo "=== Completed with failures ==="
  echo "Failed repositories:"
  for repo in "${FAILED_REPOS[@]}"; do
    echo "  - $repo"
  done
  echo "Fix the errors above and rerun smart-commit." >&2
  exit 1
fi

echo "=== All done (success) ==="
