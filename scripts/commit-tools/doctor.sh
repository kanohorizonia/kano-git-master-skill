#!/usr/bin/env bash
#
# doctor.sh - Git hygiene diagnostic and remediation tool
#
# Purpose:
#   Provide subcommand-based health checks and fixes for git repositories.
#   Initially supports ignore-doctor workflow, with architecture ready for
#   future extensions (secrets, repo integrity, etc.).
#
# Features:
#   - ignore subcommand: detect and optionally fix tracked files that match ignore patterns
#   - Safe by default: dry-run reporting unless --fix is explicit
#   - Cleanup-commit mode: untrack files via git rm --cached + commit (non-destructive)
#   - Repair-branch mode stub: prints not-implemented-safe-yet guidance (no rebase/filter-branch)
#
# Usage:
#   ./doctor.sh <subcommand> [options]
#
# Subcommands:
#   ignore                 Run ignore doctor (detect/fix tracked-but-ignorable files)
#
# Global Options:
#   -h, --help            Show this help message
#
# Examples:
#   # Run ignore doctor in dry-run mode
#   ./doctor.sh ignore
#
#   # Fix tracked ignorable files with cleanup commit
#   ./doctor.sh ignore --fix --fix-mode cleanup-commit
#
#   # Check all history for tracked ignorable files
#   ./doctor.sh ignore --all-history
#

set -euo pipefail

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Doctor mode
SUBCOMMAND=""

#------------------------------------------------------------------------------
# Global usage
#------------------------------------------------------------------------------

global_usage() {
  cat <<'EOF'
doctor.sh - Git hygiene diagnostic and remediation tool

USAGE:
  ./doctor.sh <subcommand> [options]

SUBCOMMANDS:
  ignore                 Detect and fix tracked files that match ignore patterns

GLOBAL OPTIONS:
  -h, --help            Show this help message

EXAMPLES:
  # Get help for a specific subcommand
  ./doctor.sh ignore --help

  # Run ignore doctor in dry-run mode
  ./doctor.sh ignore

  # Fix tracked ignorable files with cleanup commit
  ./doctor.sh ignore --fix --fix-mode cleanup-commit

For subcommand-specific help, run:
  ./doctor.sh <subcommand> --help

EOF
}

#------------------------------------------------------------------------------
# Ignore subcommand
#------------------------------------------------------------------------------

# Static catalog of ignore-worthy patterns (duplicated from static-gitignore.sh for now)
get_ignore_patterns_catalog() {
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

ignore_usage() {
  cat <<'EOF'
doctor.sh ignore - Detect and fix tracked files that match ignore patterns

USAGE:
  ./doctor.sh ignore [options]

OPTIONS:
  --repo <path>                 Target repository (default: current repo)
  --range <rev-range>           Git rev-range to analyze (e.g., HEAD~10..HEAD)
  --all-history                 Analyze all commits (overrides --range)
  --include-local-unpushed      Use @{upstream}..HEAD range if upstream exists
  --fix                         Apply fixes (default: dry-run report only)
  --fix-mode <mode>             Fix strategy (cleanup-commit | repair-branch)
                                - cleanup-commit: untrack files + create commit (safe)
                                - repair-branch: NOT IMPLEMENTED (risky, prints guidance)
  --dry-run                     Explicitly report only (same as no --fix)
  -h, --help                    Show this help message

DETECTION:
  - Checks tracked files in chosen range against ignore pattern catalog
  - Pattern sources: static catalog + .gitignore entries (non-comment lines)

FIX MODES:
  cleanup-commit (safe):
    - Updates .gitignore if needed (via static-gitignore.sh)
    - Runs git rm --cached for offending tracked files (preserves working copies)
    - Stages changes and creates cleanup commit

  repair-branch (NOT IMPLEMENTED):
    - Would rewrite history to remove tracked files from all commits in range
    - Requires rebase/filter-branch (destructive operations)
    - Currently prints guidance and exits non-zero

EXAMPLES:
  # Dry-run: report tracked files matching ignore patterns in working tree
  ./doctor.sh ignore

  # Check all history for tracked ignorable files
  ./doctor.sh ignore --all-history

  # Check unpushed commits for tracked ignorable files
  ./doctor.sh ignore --include-local-unpushed

  # Fix via cleanup commit (safe: preserves working files, no history rewrite)
  ./doctor.sh ignore --fix --fix-mode cleanup-commit

  # Attempt repair-branch mode (will print not-implemented message)
  ./doctor.sh ignore --fix --fix-mode repair-branch

EOF
}

run_ignore_doctor() {
  # Configuration
  local repo="${PWD}"
  local range=""
  local all_history=0
  local include_local_unpushed=0
  local do_fix=0
  local fix_mode="cleanup-commit"
  local dry_run=1  # Default to dry-run unless --fix specified

  # Parse ignore-specific options
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --repo)
        repo="${2:-}"
        if [[ -z "$repo" ]]; then
          echo "ERROR: --repo requires a path argument" >&2
          exit 1
        fi
        shift 2
        ;;
      --range)
        range="${2:-}"
        if [[ -z "$range" ]]; then
          echo "ERROR: --range requires a rev-range argument" >&2
          exit 1
        fi
        shift 2
        ;;
      --all-history)
        all_history=1
        shift
        ;;
      --include-local-unpushed)
        include_local_unpushed=1
        shift
        ;;
      --fix)
        do_fix=1
        dry_run=0
        shift
        ;;
      --fix-mode)
        fix_mode="${2:-}"
        if [[ -z "$fix_mode" ]]; then
          echo "ERROR: --fix-mode requires a mode argument (cleanup-commit | repair-branch)" >&2
          exit 1
        fi
        case "$fix_mode" in
          cleanup-commit|repair-branch)
            ;;
          *)
            echo "ERROR: Invalid --fix-mode: $fix_mode. Must be cleanup-commit or repair-branch" >&2
            exit 1
            ;;
        esac
        shift 2
        ;;
      --dry-run)
        dry_run=1
        do_fix=0
        shift
        ;;
      -h|--help)
        ignore_usage
        exit 0
        ;;
      *)
        echo "ERROR: Unknown option for ignore subcommand: $1" >&2
        ignore_usage >&2
        exit 1
        ;;
    esac
  done

  # Validate repository
  if [[ ! -d "$repo" ]]; then
    echo "ERROR: Repository not found: $repo" >&2
    exit 1
  fi

  if ! git -C "$repo" rev-parse --git-dir >/dev/null 2>&1; then
    echo "ERROR: Not a git repository: $repo" >&2
    exit 1
  fi

  # Determine range to analyze
  local resolved_range=""
  if [[ $all_history -eq 1 ]]; then
    # All commits: use root commit to HEAD
    local root_commit
    root_commit=$(git -C "$repo" rev-list --max-parents=0 HEAD 2>/dev/null | head -n 1)
    resolved_range="${root_commit}..HEAD"
  elif [[ -n "$range" ]]; then
    resolved_range="$range"
  elif [[ $include_local_unpushed -eq 1 ]]; then
    local upstream
    upstream=$(git -C "$repo" rev-parse --abbrev-ref --symbolic-full-name @{upstream} 2>/dev/null || echo "")
    if [[ -n "$upstream" ]]; then
      resolved_range="@{upstream}..HEAD"
    else
      echo "WARNING: --include-local-unpushed specified but no upstream tracking branch found" >&2
      echo "Falling back to HEAD working view" >&2
      resolved_range=""
    fi
  fi

  # Collect patterns from catalog + .gitignore
  local all_patterns
  all_patterns=$(get_ignore_patterns_catalog)

  if [[ -f "$repo/.gitignore" ]]; then
    local gitignore_patterns
    gitignore_patterns=$(grep -v '^#' "$repo/.gitignore" | grep -v '^[[:space:]]*$' || true)
    if [[ -n "$gitignore_patterns" ]]; then
      all_patterns=$(printf "%s\n%s" "$all_patterns" "$gitignore_patterns" | sort -u)
    fi
  fi

  # Collect candidate tracked files from range
  local tracked_files=""
  if [[ -z "$resolved_range" ]]; then
    # No range: use currently tracked files in working tree
    tracked_files=$(git -C "$repo" ls-files 2>/dev/null || true)
  else
    # Range specified: collect all tracked files at HEAD (within that range's history)
    # Use git ls-tree to get all files at HEAD, since the range might include root commit
    tracked_files=$(git -C "$repo" ls-tree -r --name-only HEAD 2>/dev/null | sort -u || true)
  fi

  if [[ -z "$tracked_files" ]]; then
    echo "No tracked files found in specified range."
    exit 0
  fi

  # Match tracked files against ignore patterns
  local offending_files=()

  while IFS= read -r file; do
    [[ -z "$file" ]] && continue

    # Check each pattern
    while IFS= read -r pattern; do
      [[ -z "$pattern" ]] && continue

      local matches=0

      # Directory pattern (ends with /)
      if [[ "$pattern" == */ ]]; then
        local pattern_dir="${pattern%/}"
        if [[ "$file" == "$pattern_dir"/* ]] || [[ "$file" == "$pattern_dir" ]]; then
          matches=1
        fi
      else
        # File pattern (exact or wildcard)
        local file_base="${file##*/}"

        if [[ "$pattern" == *"*"* ]]; then
          # Wildcard pattern: use bash glob matching
          if [[ "$file" == $pattern ]] || [[ "$file_base" == $pattern ]]; then
            matches=1
          fi
        else
          # Exact pattern
          if [[ "$file" == "$pattern" ]] || [[ "$file_base" == "$pattern" ]]; then
            matches=1
          fi
        fi
      fi

      if [[ $matches -eq 1 ]]; then
        offending_files+=("$file")
        break  # No need to check other patterns for this file
      fi
    done <<< "$all_patterns"
  done <<< "$tracked_files"

  # Report findings
  echo ""
  echo "Ignore Doctor Report"
  echo "===================="
  echo "Repository: $repo"
  echo "Range: ${resolved_range:-HEAD working view}"
  echo "Patterns checked: $(echo "$all_patterns" | wc -l) (catalog + .gitignore)"
  echo ""

  if [[ ${#offending_files[@]} -eq 0 ]]; then
    echo "✓ No tracked files match ignore patterns"
    exit 0
  fi

  echo "Found ${#offending_files[@]} tracked file(s) matching ignore patterns:"
  echo ""
  for file in "${offending_files[@]}"; do
    echo "  - $file"
  done
  echo ""

  # If dry-run, exit here
  if [[ $dry_run -eq 1 ]]; then
    echo "Run with --fix --fix-mode cleanup-commit to untrack these files safely"
    exit 0
  fi

  # Fix mode
  if [[ $do_fix -eq 1 ]]; then
    case "$fix_mode" in
      cleanup-commit)
        echo "Fix Mode: cleanup-commit (safe)"
        echo "==============================="
        echo ""

        # Check index state
        if ! git -C "$repo" diff-index --quiet --cached HEAD 2>/dev/null; then
          echo "WARNING: Index has staged changes. Please commit or stash them first." >&2
          exit 1
        fi

        # Update .gitignore if needed (run static-gitignore.sh)
        echo "Step 1: Ensuring .gitignore has static patterns..."
        if [[ -x "$SCRIPT_DIR/static-gitignore.sh" ]]; then
          "$SCRIPT_DIR/static-gitignore.sh" --repo "$repo" >/dev/null 2>&1 || true
        fi

        # Untrack files via git rm --cached
        echo "Step 2: Untracking files (preserving working copies)..."
        for file in "${offending_files[@]}"; do
          if git -C "$repo" ls-files --error-unmatch "$file" >/dev/null 2>&1; then
            echo "  Untracking: $file"
            git -C "$repo" rm --cached "$file" 2>/dev/null || true
          fi
        done

        # Stage .gitignore changes if any
        if [[ -f "$repo/.gitignore" ]]; then
          git -C "$repo" add .gitignore 2>/dev/null || true
        fi

        # Create commit
        echo "Step 3: Creating cleanup commit..."
        local commit_msg="chore(ignore): remove tracked files now covered by gitignore

Untracked ${#offending_files[@]} file(s) that match ignore patterns.
Files are preserved in working tree but removed from git index.

Affected files:
$(for f in "${offending_files[@]}"; do echo "  - $f"; done)"

        git -C "$repo" commit -m "$commit_msg" 2>/dev/null || {
          echo "ERROR: Failed to create commit. Check git status for details." >&2
          exit 1
        }

        echo ""
        echo "✓ Cleanup commit created successfully"
        echo "✓ Working files preserved, git index updated"
        echo ""
        git -C "$repo" log -1 --oneline
        ;;

      repair-branch)
        echo "Fix Mode: repair-branch (NOT IMPLEMENTED - RISKY)"
        echo "=================================================="
        echo ""
        echo "NOTICE: repair-branch mode is not implemented for safety reasons."
        echo ""
        echo "This mode would require destructive git operations like:"
        echo "  - git filter-branch"
        echo "  - git rebase --interactive --exec"
        echo "  - git cherry-pick with history rewriting"
        echo ""
        echo "These operations are risky and can cause:"
        echo "  - Loss of commit history"
        echo "  - Force-push requirements"
        echo "  - Conflicts with collaborators"
        echo ""
        echo "Recommendation: Use --fix-mode cleanup-commit instead for safe untracking."
        echo ""
        echo "If you absolutely need history rewriting, use manual git commands:"
        echo "  git filter-branch --index-filter 'git rm --cached --ignore-unmatch <file>' HEAD"
        echo ""
        exit 1
        ;;
    esac
  fi
}

#------------------------------------------------------------------------------
# Main entry point
#------------------------------------------------------------------------------

# Parse global options and subcommand
if [[ $# -eq 0 ]]; then
  global_usage
  exit 0
fi

case "$1" in
  -h|--help)
    global_usage
    exit 0
    ;;
  ignore)
    shift
    run_ignore_doctor "$@"
    ;;
  *)
    echo "ERROR: Unknown subcommand: $1" >&2
    echo "" >&2
    global_usage >&2
    exit 1
    ;;
esac
