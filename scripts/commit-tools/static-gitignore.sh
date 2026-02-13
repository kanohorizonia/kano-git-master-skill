#!/usr/bin/env bash
#
# static-gitignore.sh - Deterministic static .gitignore pattern management
#
# Purpose:
#   Add common static gitignore patterns to repositories using managed block
#   markers for idempotent execution. Does not use AI - just rules.
#
# Features:
#   - Managed block markers (# >>> STATIC-GITIGNORE / # <<< STATIC-GITIGNORE)
#   - Idempotent: running twice produces identical .gitignore
#   - Warns about patterns that would hide tracked files
#   - Dry-run mode to preview changes
#   - Multi-repo support (--repo flag)
#
# Usage:
#   ./static-gitignore.sh [options]
#
# Options:
#   --repo <path>      Target repository (default: current repo)
#   --dry-run          Preview changes without modifying files
#   -h, --help         Show this help message
#
# Examples:
#   # Update current repo
#   ./static-gitignore.sh
#
#   # Update specific repo
#   ./static-gitignore.sh --repo /path/to/repo
#
#   # Preview changes
#   ./static-gitignore.sh --repo /path/to/repo --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
TARGET_REPO="${PWD}"
DRY_RUN=0
SCOPE="untracked"  # Default scope: untracked files

# Managed block markers
BLOCK_START="# >>> STATIC-GITIGNORE"
BLOCK_END="# <<< STATIC-GITIGNORE"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: static-gitignore.sh [options]

Deterministic static .gitignore pattern management with managed block markers.

Options:
  --repo <path>          Target repository (default: current repo)
  --scope <type>         File scope for pattern selection (default: untracked)
                         Types: untracked, staged, worktree, all
  --dry-run              Preview changes without modifying files
  -h, --help             Show this help message

Examples:
  # Update current repo (default scope: untracked)
  ./static-gitignore.sh

  # Update specific repo with custom scope
  ./static-gitignore.sh --repo /path/to/repo --scope staged

  # Preview changes
  ./static-gitignore.sh --repo /path/to/repo --dry-run

Static Pattern Categories:
  - Dependencies: node_modules/, .pnpm/, .modules.yaml, vendor/, .venv/, venv/
  - Build Artifacts: dist/, build/, coverage/, .next/, out/, *.pyc, __pycache__/
  - IDE/Editor: .idea/, .vscode/, *.code-workspace, *.swp, *.swo
  - Environment: .env, .env.*, *.local
  - OS Files: .DS_Store, Thumbs.db
  - Logs/Temp: *.log, *.tmp, *.bak, tmp/, .tmp/
  - Cache: .cache/, .pytest_cache/, .mypy_cache/, .ruff_cache/, .turbo/
  - pnpm Specific: .pnpm-workspace-state-v1.json, pnpm-debug.log
EOF
}

# Parse arguments
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
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# Verify repo exists
if [[ ! -d "$TARGET_REPO" ]]; then
  echo "ERROR: Repository not found: $TARGET_REPO" >&2
  exit 1
fi

# Verify it's a git repo
if ! git -C "$TARGET_REPO" rev-parse --git-dir >/dev/null 2>&1; then
  echo "ERROR: Not a git repository: $TARGET_REPO" >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Static Pattern Definitions (as newline-separated list for deterministic output)
#------------------------------------------------------------------------------

get_all_patterns() {
  cat <<'PATTERNS_EOF'
.cache/
.DS_Store
.env
.env.*
*.local
.idea/
.modules.yaml
.mypy_cache/
.next/
.pnpm/
.pnpm-workspace-state-v1.json
.pytest_cache/
.ruff_cache/
.tmp/
.turbo/
.vscode/
*.bak
*.code-workspace
*.log
*.pyc
*.swp
*.swo
*.tmp
__pycache__/
build/
coverage/
dist/
node_modules/
out/
pnpm-debug.log
Thumbs.db
tmp/
vendor/
venv/
.venv/
PATTERNS_EOF
}

#------------------------------------------------------------------------------
# Core Functions
#------------------------------------------------------------------------------

# Collect candidate files based on scope
get_candidate_files() {
  local repo="$1"
  local scope="$2"

  case "$scope" in
    untracked)
      git -C "$repo" ls-files --others --exclude-standard 2>/dev/null || true
      ;;
    staged)
      git -C "$repo" diff --cached --name-only --diff-filter=ACMR 2>/dev/null || true
      ;;
    worktree)
      git -C "$repo" status --porcelain 2>/dev/null | awk '{print $2}' || true
      ;;
    all)
      {
        git -C "$repo" ls-files --others --exclude-standard 2>/dev/null || true
        git -C "$repo" diff --cached --name-only --diff-filter=ACMR 2>/dev/null || true
        git -C "$repo" status --porcelain 2>/dev/null | awk '{print $2}' || true
      } | sort -u
      ;;
    *)
      echo "ERROR: Unknown scope: $scope" >&2
      return 1
      ;;
  esac
}

# Select relevant patterns based on candidate files
select_relevant_patterns() {
  local candidates="$1"

  if [[ -z "$candidates" ]]; then
    get_all_patterns
    return 0
  fi

  local selected_patterns=()

  get_all_patterns | while IFS= read -r pattern; do
    [[ -z "$pattern" ]] && continue

    local pattern_matched=0
    while IFS= read -r file; do
      [[ -z "$file" ]] && continue

      if [[ "$pattern" == */ ]]; then
        local pattern_dir="${pattern%/}"
        if [[ "$file" == "$pattern_dir"/* || "$file" == "$pattern_dir" ]]; then
          pattern_matched=1
          break
        fi
      else
        local file_base="${file##*/}"
        local file_dir="${file%/*}"

        if [[ "$pattern" == *"*"* ]]; then
          if [[ "$file" == $pattern || "$file_base" == $pattern ]]; then
            pattern_matched=1
            break
          fi
        else
          if [[ "$file" == "$pattern" || "$file_base" == "$pattern" ]]; then
            pattern_matched=1
            break
          fi
        fi
      fi
    done <<< "$candidates"

    if [[ "$pattern_matched" -eq 1 ]]; then
      echo "$pattern"
    fi
  done | sort -u
}

# Find managed block in .gitignore
find_managed_block() {
  local gitignore="$1"

  if [[ ! -f "$gitignore" ]]; then
    return 1
  fi

  local start_line end_line
  start_line=$(grep -n "^$BLOCK_START$" "$gitignore" 2>/dev/null | cut -d: -f1 | head -n 1 || true)
  end_line=$(grep -n "^$BLOCK_END$" "$gitignore" 2>/dev/null | cut -d: -f1 | head -n 1 || true)

  if [[ -n "$start_line" && -n "$end_line" ]]; then
    echo "$start_line:$end_line"
    return 0
  fi

  return 1
}

# Check if a pattern would hide tracked files
check_tracked_files() {
  local repo="$1"
  local pattern="$2"
  local warned=0

  # Get all tracked files
  local tracked_files
  tracked_files=$(git -C "$repo" ls-files 2>/dev/null || echo "")

  if [[ -z "$tracked_files" ]]; then
    return 0
  fi

  # Check each tracked file against the pattern
  while IFS= read -r file; do
    [[ -z "$file" ]] && continue

    local pattern_dir="${pattern%/}"

    # Pattern matching logic:
    # "node_modules/" matches "node_modules/tracked-file.txt" or "node_modules/"
    # ".env" matches ".env" exactly
    # ".env.*" matches ".env.local"
    if [[ "$pattern" == */ ]]; then
      # Directory pattern like "node_modules/"
      if [[ "$file" == "$pattern_dir"/* ]]; then
        # File is under the directory
        if ! git -C "$repo" check-ignore -q "$file" 2>/dev/null; then
          echo "WARNING: Pattern '$pattern' would hide tracked file: $file" >&2
          warned=1
        fi
      fi
    else
      # File pattern like ".env" or ".env.*"
      if [[ "$file" == "$pattern" ]] || [[ "$pattern" == *"*"* && "$file" == ${pattern/\*/+([^\/])} ]]; then
        if ! git -C "$repo" check-ignore -q "$file" 2>/dev/null; then
          echo "WARNING: Pattern '$pattern' would hide tracked file: $file" >&2
          warned=1
        fi
      fi
    fi
  done <<< "$tracked_files"

  return $([ "$warned" -eq 0 ] && echo 0 || echo 1)
}

# Build the new managed block content
build_managed_block() {
  local repo="$1"
  local candidates="$2"

  echo "$BLOCK_START"

  select_relevant_patterns "$candidates" | while IFS= read -r pattern; do
    [[ -z "$pattern" ]] && continue
    echo "$pattern"
  done

  echo "$BLOCK_END"
}

# Check all patterns for tracked file conflicts
check_all_patterns_for_conflicts() {
  local repo="$1"

  get_all_patterns | sort -u | while IFS= read -r pattern; do
    [[ -z "$pattern" ]] && continue
    check_tracked_files "$repo" "$pattern" >/dev/null || true
  done
}

# Extract patterns from existing managed block
extract_existing_patterns() {
  local gitignore="$1"
  local block_info

  if ! block_info=$(find_managed_block "$gitignore"); then
    return 0
  fi

  local start_line end_line
  IFS=: read -r start_line end_line <<< "$block_info"

  sed -n "$((start_line + 1)),$((end_line - 1))p" "$gitignore" | grep -v '^#' | grep -v '^[[:space:]]*$' || true
}

# Update .gitignore with managed block
update_gitignore() {
  local repo="$1"
  local gitignore="$repo/.gitignore"
  local temp_file new_patterns existing_patterns missing_patterns candidates

  if [[ ! -f "$gitignore" ]]; then
    touch "$gitignore"
  fi

  check_all_patterns_for_conflicts "$repo"

  candidates=$(get_candidate_files "$repo" "$SCOPE")
  new_patterns=$(select_relevant_patterns "$candidates" | sort -u)

  local block_info
  if block_info=$(find_managed_block "$gitignore"); then
    existing_patterns=$(extract_existing_patterns "$gitignore")

    missing_patterns=$(comm -13 <(echo "$existing_patterns" | sort -u) <(echo "$new_patterns" | sort -u))

    if [[ -z "$missing_patterns" ]]; then
      if [[ "$DRY_RUN" -eq 0 ]]; then
        echo "[$repo] .gitignore already up-to-date (no missing patterns)"
      else
        echo "[$repo] Would not change .gitignore (no missing patterns)"
      fi
      return 0
    fi

    local start_line end_line
    IFS=: read -r start_line end_line <<< "$block_info"

    temp_file=$(mktemp)
    {
      if [[ $((start_line - 1)) -gt 0 ]]; then
        sed -n "1,$((start_line - 1))p" "$gitignore" 2>/dev/null || true
      fi

      echo "$BLOCK_START"
      echo "$existing_patterns"
      echo "$missing_patterns"
      echo "$BLOCK_END"

      local total_lines
      total_lines=$(wc -l < "$gitignore")
      if [[ $((end_line)) -lt $((total_lines)) ]]; then
        sed -n "$((end_line + 1)),\$p" "$gitignore" 2>/dev/null || true
      fi
    } > "$temp_file"
  else
    temp_file=$(mktemp)
    {
      cat "$gitignore"
      if [[ -s "$gitignore" ]]; then
        echo ""
      fi
      echo "$BLOCK_START"
      echo "$new_patterns"
      echo "$BLOCK_END"
    } > "$temp_file"
  fi

  if [[ "$DRY_RUN" -eq 0 ]]; then
    mv "$temp_file" "$gitignore"
    echo "[$repo] .gitignore updated"
  else
    echo "[$repo] Would update .gitignore"
    if diff -u "$gitignore" "$temp_file" 2>/dev/null | head -n 20; then
      :
    fi
    rm -f "$temp_file"
  fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

echo "Static .gitignore Manager"
echo "========================="
echo "Target: $TARGET_REPO"
echo "Scope: $SCOPE"
[[ "$DRY_RUN" -eq 1 ]] && echo "Mode: DRY-RUN"

# Update .gitignore
update_gitignore "$TARGET_REPO"

exit 0
