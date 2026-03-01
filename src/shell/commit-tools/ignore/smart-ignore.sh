#!/usr/bin/env bash
#
# smart-ignore.sh - Orchestrator for static and AI-driven .gitignore management
#
# Purpose:
#   Coordinates execution of smart-ignore-noai.sh and ai-gitignore.sh in sequence,
#   propagating flags appropriately and handling exit codes.
#
# Features:
#   - Executes static patterns first (deterministic)
#   - Optionally follows with AI patterns (context-aware)
#   - Propagates flags to appropriate child scripts
#   - Combined exit code reporting
#   - Verbose execution progress tracking
#
# Usage:
#   ./smart-ignore.sh [options]
#
# Options:
#   --repo <path>              Target repository (default: current repo)
#   --provider <name>          AI provider (opencode, codex, copilot) - passed to AI script
#   --model <name>             AI model name - passed to AI script
#   --scope <type>             File scope for pattern selection (untracked|staged|worktree|all)
#   --dry-run                  Preview changes without modifying files
#   --no-ai                    Skip AI pattern analysis (static only)
#   --no-static                Skip static patterns (AI only)
#   --consolidate              Run consolidation check after static/AI (default: ON)
#   --no-consolidate           Skip consolidation check
#   --verbose                  Show script execution progress to stderr
#   -h, --help                 Show this help message
#
# Examples:
#   # Full run with both static and AI patterns
#   ./smart-ignore.sh --provider copilot --model gpt-5-mini
#
#   # Static patterns only
#   ./smart-ignore.sh --no-ai
#
#   # AI patterns only
#   ./smart-ignore.sh --provider copilot --model gpt-5-mini --no-static
#
#   # Dry-run preview for both
#   ./smart-ignore.sh --provider copilot --model gpt-5-mini --dry-run
#
#   # Verbose output
#   ./smart-ignore.sh --provider copilot --model gpt-5-mini --verbose
#

set -euo pipefail

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default values
TARGET_REPO=""
AI_PROVIDER=""
AI_MODEL=""
DRY_RUN=0
NO_AI=0
NO_STATIC=0
VERBOSE=0
SCOPE=""
CONSOLIDATE=1

#------------------------------------------------------------------------------
# Usage function
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
smart-ignore.sh - Orchestrator for static and AI-driven .gitignore management

USAGE:
  ./smart-ignore.sh [options]

OPTIONS:
  --repo <path>              Target repository (default: current repo)
  --provider <name>          AI provider (opencode, codex, copilot)
  --model <name>             AI model name
  --scope <type>             File scope for pattern selection (untracked|staged|worktree|all)
                             Passed to smart-ignore-noai.sh only
  --dry-run                  Preview changes without modifying files
  --no-ai                    Skip AI pattern analysis (static only)
  --no-static                Skip static patterns (AI only)
  --consolidate              Run consolidation check after static/AI (default: ON)
  --no-consolidate           Skip consolidation check
  --verbose                  Show script execution progress to stderr
  -h, --help                 Show this help message

EXAMPLES:
  # Full run with both static and AI patterns
  ./smart-ignore.sh --provider copilot --model gpt-5-mini

  # Static patterns only
  ./smart-ignore.sh --no-ai

  # Static with specific scope
  ./smart-ignore.sh --no-ai --scope staged

  # AI patterns only
  ./smart-ignore.sh --provider copilot --model gpt-5-mini --no-static

  # Dry-run preview for both
  ./smart-ignore.sh --provider copilot --model gpt-5-mini --dry-run

  # Verbose output showing execution progress
  ./smart-ignore.sh --provider copilot --model gpt-5-mini --verbose

  # Skip consolidation check
  ./smart-ignore.sh --provider copilot --model gpt-5-mini --no-consolidate

EXECUTION ORDER:
  1. Static patterns (deterministic rules)
  2. AI patterns (context-aware, if enabled)
  3. Consolidation check (unless --no-consolidate is set)

CONSOLIDATION:
  Detects duplicate patterns across static and AI blocks and reports them.
  Run with --dry-run or standard mode to see the report.
  No patterns are automatically modified.

EXIT CODE:
  0 = Success
  1 = Both scripts skipped (--no-static + --no-ai)
  N = Exit code from first failing script

EOF
}

#------------------------------------------------------------------------------
# Logging helpers
#------------------------------------------------------------------------------

log_verbose() {
  if [[ $VERBOSE -eq 1 ]]; then
    echo "[smart-ignore.sh] $*" >&2
  fi
}

#------------------------------------------------------------------------------
# Parse arguments
#------------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      TARGET_REPO="${2:-}"
      if [[ -z "$TARGET_REPO" ]]; then
        echo "ERROR: --repo requires a path argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --scope)
      SCOPE="${2:-}"
      if [[ -z "$SCOPE" ]]; then
        echo "ERROR: --scope requires a value (untracked|staged|worktree|all)" >&2
        exit 1
      fi
      case "$SCOPE" in
        untracked|staged|worktree|all)
          ;;
        *)
          echo "ERROR: Invalid --scope value: $SCOPE. Must be one of: untracked, staged, worktree, all" >&2
          exit 1
          ;;
      esac
      shift 2
      ;;
    --provider)
      AI_PROVIDER="${2:-}"
      if [[ -z "$AI_PROVIDER" ]]; then
        echo "ERROR: --provider requires a provider name argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      if [[ -z "$AI_MODEL" ]]; then
        echo "ERROR: --model requires a model name argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-ai)
      NO_AI=1
      shift
      ;;
    --no-static)
      NO_STATIC=1
      shift
      ;;
    --consolidate)
      CONSOLIDATE=1
      shift
      ;;
    --no-consolidate)
      CONSOLIDATE=0
      shift
      ;;
    --verbose)
      VERBOSE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

#------------------------------------------------------------------------------
# Validate configuration
#------------------------------------------------------------------------------

# Check if both scripts are disabled
if [[ $NO_STATIC -eq 1 ]] && [[ $NO_AI -eq 1 ]]; then
  echo "ERROR: Cannot skip both static and AI patterns (--no-static + --no-ai)" >&2
  exit 1
fi

# If AI is not disabled, validate that provider and model are set
if [[ $NO_AI -eq 0 ]]; then
  if [[ -z "$AI_PROVIDER" ]] || [[ -z "$AI_MODEL" ]]; then
    echo "ERROR: AI patterns enabled but --provider and --model are required" >&2
    echo "Use --no-ai to skip AI patterns, or provide --provider and --model" >&2
    exit 1
  fi
fi

#------------------------------------------------------------------------------
# Build argument arrays for child scripts
#------------------------------------------------------------------------------

STATIC_ARGS=()
AI_ARGS=()

# Add --repo to both scripts (if provided)
if [[ -n "$TARGET_REPO" ]]; then
  STATIC_ARGS+=(--repo "$TARGET_REPO")
  AI_ARGS+=(--repo "$TARGET_REPO")
fi

# Add --scope to static script only (if provided)
if [[ -n "$SCOPE" ]]; then
  STATIC_ARGS+=(--scope "$SCOPE")
fi

# Add --dry-run to both scripts (if enabled)
if [[ $DRY_RUN -eq 1 ]]; then
  STATIC_ARGS+=(--dry-run)
  AI_ARGS+=(--dry-run)
fi

# Add AI-specific arguments
AI_ARGS+=(--provider "$AI_PROVIDER")
AI_ARGS+=(--model "$AI_MODEL")

#------------------------------------------------------------------------------
# Consolidation function
#------------------------------------------------------------------------------

consolidate_patterns() {
  local repo="${1:-.}"
  local gitignore="$repo/.gitignore"

  if [[ ! -f "$gitignore" ]]; then
    log_verbose "No .gitignore found, skipping consolidation"
    return 0
  fi

  local static_patterns ai_patterns duplicates

  static_patterns=$(sed -n '/^# >>> STATIC-GITIGNORE$/,/^# <<< STATIC-GITIGNORE$/p' "$gitignore" | \
    grep -v '^#' | grep -v '^[[:space:]]*$' | sort -u)

  ai_patterns=$(sed -n '/^# >>> AI-GITIGNORE$/,/^# <<< AI-GITIGNORE$/p' "$gitignore" | \
    grep -v '^#' | grep -v '^[[:space:]]*$' | sort -u)

  if [[ -z "$static_patterns" ]] || [[ -z "$ai_patterns" ]]; then
    log_verbose "Consolidation: at least one block missing or empty, skipping"
    return 0
  fi

  # Use temporary files to avoid process substitution issues on Windows
  local tmp_static tmp_ai
  tmp_static=$(mktemp)
  tmp_ai=$(mktemp)
  trap "rm -f '$tmp_static' '$tmp_ai'" RETURN

  echo "$static_patterns" | sort -u > "$tmp_static"
  echo "$ai_patterns" | sort -u > "$tmp_ai"

  duplicates=$(comm -12 "$tmp_static" "$tmp_ai")

  if [[ -n "$duplicates" ]]; then
    echo ""
    echo "Consolidation Report:"
    echo "====================="
    local dup_count
    dup_count=$(echo "$duplicates" | wc -l)
    echo "Found $dup_count duplicate pattern(s) across patterns:"
    echo ""
    echo "$duplicates" | sed 's/^/  /'
    echo ""
    echo "Recommendation: Remove duplicate patterns to maintain clean .gitignore."
  else
    log_verbose "Consolidation: no duplicate patterns found"
  fi

  return 0
}

#------------------------------------------------------------------------------
# Execute scripts
#------------------------------------------------------------------------------

EXIT_CODE_STATIC=0
EXIT_CODE_AI=0

# Execute static patterns first
if [[ $NO_STATIC -eq 0 ]]; then
  log_verbose "Executing smart-ignore-noai.sh..."

  if "$SCRIPT_DIR/smart-ignore-noai.sh" ${STATIC_ARGS[@]+"${STATIC_ARGS[@]}"}; then
    EXIT_CODE_STATIC=0
    log_verbose "smart-ignore-noai.sh completed successfully (exit code: 0)"
  else
    EXIT_CODE_STATIC=$?
    log_verbose "smart-ignore-noai.sh failed (exit code: $EXIT_CODE_STATIC)"
  fi
else
  log_verbose "Skipping smart-ignore-noai.sh (--no-static)"
fi

# Execute AI patterns second (if enabled)
if [[ $NO_AI -eq 0 ]]; then
  log_verbose "Executing ai-gitignore.sh..."

  if "$SCRIPT_DIR/../ai-gitignore.sh" ${AI_ARGS[@]+"${AI_ARGS[@]}"}; then
    EXIT_CODE_AI=0
    log_verbose "ai-gitignore.sh completed successfully (exit code: 0)"
  else
    EXIT_CODE_AI=$?
    log_verbose "ai-gitignore.sh failed (exit code: $EXIT_CODE_AI)"
  fi
else
  log_verbose "Skipping ai-gitignore.sh (--no-ai)"
fi

#------------------------------------------------------------------------------
# Determine combined exit code
#------------------------------------------------------------------------------

# Exit code logic:
# - If static failed, use its exit code
# - Otherwise if AI failed, use its exit code
# - Otherwise success (0)

if [[ $EXIT_CODE_STATIC -ne 0 ]]; then
  log_verbose "Exiting with static-gitignore.sh exit code: $EXIT_CODE_STATIC"
  exit $EXIT_CODE_STATIC
elif [[ $EXIT_CODE_AI -ne 0 ]]; then
  log_verbose "Exiting with ai-gitignore.sh exit code: $EXIT_CODE_AI"
  exit $EXIT_CODE_AI
fi

#------------------------------------------------------------------------------
# Run consolidation (if enabled and both scripts succeeded)
#------------------------------------------------------------------------------

if [[ $CONSOLIDATE -eq 1 ]]; then
  log_verbose "Running consolidation check..."
  consolidate_patterns "${TARGET_REPO:-.}"
else
  log_verbose "Skipping consolidation check (--no-consolidate)"
fi

log_verbose "Both scripts completed successfully"
exit 0
