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
  --repo <path>      Target repository (default: current repo)
  --dry-run          Preview changes without modifying files
  -h, --help         Show this help message

Examples:
  # Update current repo
  ./static-gitignore.sh

  # Update specific repo
  ./static-gitignore.sh --repo /path/to/repo

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

  # Start block
  echo "$BLOCK_START"

  # Add all patterns (already sorted by get_all_patterns)
  get_all_patterns | sort -u | while IFS= read -r pattern; do
    [[ -z "$pattern" ]] && continue
    echo "$pattern"
  done

  # End block
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

# Update .gitignore with managed block
update_gitignore() {
  local repo="$1"
  local gitignore="$repo/.gitignore"
  local temp_file new_content

  # Create .gitignore if it doesn't exist
  if [[ ! -f "$gitignore" ]]; then
    touch "$gitignore"
  fi

  # Check for conflicts first (warnings to stderr)
  check_all_patterns_for_conflicts "$repo"

  # Build new block
  new_content=$(build_managed_block "$repo")

  # Check if managed block exists
  local block_info
  if block_info=$(find_managed_block "$gitignore"); then
    # Block exists, replace it
    local start_line end_line
    IFS=: read -r start_line end_line <<< "$block_info"

    # Create temp file with replaced block
    temp_file=$(mktemp)
    {
      # Lines before the block
      if [[ $((start_line - 1)) -gt 0 ]]; then
        sed -n "1,$((start_line - 1))p" "$gitignore"
      fi

      # New block
      echo "$new_content"

      # Lines after the block
      local total_lines
      total_lines=$(wc -l < "$gitignore")
      if [[ $((end_line)) -lt $((total_lines)) ]]; then
        sed -n "$((end_line + 1)),\$p" "$gitignore"
      fi
    } > "$temp_file"
  else
    # Block doesn't exist, append it
    temp_file=$(mktemp)
    {
      cat "$gitignore"
      if [[ -s "$gitignore" ]]; then
        echo ""
      fi
      echo "$new_content"
    } > "$temp_file"
  fi

  # Apply changes
  if [[ "$DRY_RUN" -eq 0 ]]; then
    mv "$temp_file" "$gitignore"
    echo "[$repo] .gitignore updated"
  else
    echo "[$repo] Would update .gitignore"
    # Show diff in dry-run mode
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
[[ "$DRY_RUN" -eq 1 ]] && echo "Mode: DRY-RUN"

# Update .gitignore
update_gitignore "$TARGET_REPO"

exit 0
