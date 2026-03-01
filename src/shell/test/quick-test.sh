#!/usr/bin/env bash
#
# quick-test.sh - Quick smoke tests for Git Master Skill
#
# Purpose:
#   Quick validation of core functionality
#
# Usage:
#   ./quick-test.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$SKILL_ROOT/../.." && pwd)"

echo "Git Master Skill - Quick Test"
echo "=============================="
echo ""

# Test 1: Check all scripts exist
echo "Test 1: Checking script files..."
SCRIPTS=(
  "core/update-repo.sh"
  "core/smart-clone.sh"
  "core/discover-repos.sh"
  "workspace/update-workspace-repos.sh"
  "workspace/foreach-repo.sh"
  "workspace/status-all-repos.sh"
  "branches/rebase-to-upstream-latest.sh"
  "branches/compare-branches.sh"
  "branches/cherry-pick-batch.sh"
  "commit-tools/commit/smart-commit.sh"
  "lib/git-helpers.sh"
)

MISSING=0
for script in "${SCRIPTS[@]}"; do
  if [[ ! -f "$SKILL_ROOT/$script" ]]; then
    echo "  ✗ Missing: $script"
    MISSING=1
  else
    echo "  ✓ Found: $script"
  fi
done

if [[ "$MISSING" -eq 1 ]]; then
  echo ""
  echo "ERROR: Some scripts are missing!"
  exit 1
fi

echo ""
echo "Test 2: Checking help output..."
HELP_SCRIPTS=(
  "core/update-repo.sh"
  "core/smart-clone.sh"
  "core/discover-repos.sh"
  "workspace/update-workspace-repos.sh"
  "workspace/foreach-repo.sh"
  "workspace/status-all-repos.sh"
  "branches/rebase-to-upstream-latest.sh"
  "branches/compare-branches.sh"
  "branches/cherry-pick-batch.sh"
  "commit-tools/commit/smart-commit.sh"
)

HELP_FAILED=0
for script in "${HELP_SCRIPTS[@]}"; do
  if bash "$SKILL_ROOT/$script" --help >/dev/null 2>&1; then
    echo "  ✓ Help works: $script"
  else
    echo "  ✗ Help failed: $script"
    HELP_FAILED=1
  fi
done

if [[ "$HELP_FAILED" -eq 1 ]]; then
  echo ""
  echo "ERROR: Some help commands failed!"
  exit 1
fi

echo ""
echo "Test 3: Checking git-helpers.sh..."
if bash -c "source '$SKILL_ROOT/lib/git-helpers.sh' && type gith_log >/dev/null 2>&1"; then
  echo "  ✓ git-helpers.sh loads correctly"
else
  echo "  ✗ git-helpers.sh failed to load"
  exit 1
fi

echo ""
echo "Test 4: Checking C++ meta JSON output (if binary exists)..."
KOG_BIN=""
BIN_CANDIDATES=(
  "$PROJECT_ROOT/src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe"
  "$PROJECT_ROOT/src/cpp/build/bin/windows-ninja-msvc-arm64/release/kano-git.exe"
  "$PROJECT_ROOT/src/cpp/build/bin/linux-ninja-gcc/release/kano-git"
  "$PROJECT_ROOT/src/cpp/build/bin/macos-ninja-clang/release/kano-git"
  "$PROJECT_ROOT/src/cpp/build/bin/macos-ninja-clang-x64/release/kano-git"
  "$PROJECT_ROOT/src/cpp/build/bin/macos-ninja-clang-arm64/release/kano-git"
)

for candidate in "${BIN_CANDIDATES[@]}"; do
  if [[ -f "$candidate" ]]; then
    KOG_BIN="$candidate"
    break
  fi
done

if [[ -n "$KOG_BIN" ]]; then
  META_OUTPUT="$($KOG_BIN meta --format json)"
  if [[ "$META_OUTPUT" == *'"schema_version":"v1"'* && "$META_OUTPUT" == *'"name":"meta"'* && "$META_OUTPUT" == *'"name":"commit"'* ]]; then
    echo "  ✓ meta JSON output is valid"
  else
    echo "  ✗ meta JSON output is invalid"
    exit 1
  fi

  echo ""
  echo "Test 5: Checking C++ internal completion output..."
  COMPLETE_OUTPUT="$($KOG_BIN __complete --current c)"
  COMPLETE_NESTED="$($KOG_BIN __complete --context workspace --current st)"
  COMPLETE_OPTION="$($KOG_BIN __complete --context workspace --context status --current --na)"
  if [[ "$COMPLETE_OUTPUT" == *"commit"* && "$COMPLETE_NESTED" == *"status"* && "$COMPLETE_OPTION" == *"--native"* ]]; then
    echo "  ✓ __complete returns expected candidates"
  else
    echo "  ✗ __complete output missing expected candidates"
    exit 1
  fi

  echo ""
  echo "Test 6: Checking completion script generation..."
  COMP_BASH="$($KOG_BIN completion bash)"
  COMP_ZSH="$($KOG_BIN completion zsh)"
  COMP_FISH="$($KOG_BIN completion fish)"
  COMP_PWSH="$($KOG_BIN completion powershell)"
  if [[ "$COMP_BASH" == *"__complete"* && "$COMP_BASH" == *"complete -F"* && "$COMP_ZSH" == *"compdef"* && "$COMP_ZSH" == *"__complete"* && "$COMP_FISH" == *"complete -c kano-git"* && "$COMP_FISH" == *"__complete"* && "$COMP_PWSH" == *"Register-ArgumentCompleter"* && "$COMP_PWSH" == *"__complete"* ]]; then
    echo "  ✓ completion scripts generated for all shells"
  else
    echo "  ✗ completion script generation failed"
    exit 1
  fi

  if "$KOG_BIN" completion nope >/dev/null 2>&1; then
    echo "  ✗ unsupported shell unexpectedly succeeded"
    exit 1
  else
    echo "  ✓ unsupported shell is rejected"
  fi

  echo ""
  echo "Test 7: Checking TUI command availability..."
  TUI_HELP_OUTPUT="$($KOG_BIN --help)"
  if [[ "$TUI_HELP_OUTPUT" == *"tui                         Launch interactive KOG terminal dashboard"* ]]; then
    echo "  ✓ tui command is available in top-level help"
  else
    echo "  ✗ tui command missing from top-level help"
    exit 1
  fi

  TUI_DEMO_OUTPUT="$($KOG_BIN tui --demo)"
  if [[ "$TUI_DEMO_OUTPUT" == *"KOG TUI demo mode"* && "$TUI_DEMO_OUTPUT" == *"FTXUI dashboard enabled"* ]]; then
    echo "  ✓ tui demo confirms ftxui-backed UI mode"
  else
    echo "  ✗ tui demo did not report ftxui-backed mode"
    exit 1
  fi

  echo ""
  echo "Test 8: Checking guide command output..."
  GUIDE_OUTPUT="$($KOG_BIN guide --flow workspace --checklist)"
  if [[ "$GUIDE_OUTPUT" == *"workspace update --native-plan-only"* && "$GUIDE_OUTPUT" == *"Checklist before execution"* ]]; then
    echo "  ✓ guide command provides actionable flow output"
  else
    echo "  ✗ guide command output is missing expected content"
    exit 1
  fi

  echo ""
  echo "Test 9: Checking global status command output..."
  STATUS_OUTPUT="$($KOG_BIN status --format json --max-depth 2)"
  if [[ "$STATUS_OUTPUT" == *'"repos"'* && "$STATUS_OUTPUT" == *'"branch"'* && "$STATUS_OUTPUT" == *'"upstream"'* && "$STATUS_OUTPUT" == *'"tracking"'* && "$STATUS_OUTPUT" == *'"worktree_dirty"'* ]]; then
    echo "  ✓ global status includes branch/upstream/tracking/worktree fields"
  else
    echo "  ✗ global status output missing expected fields"
    exit 1
  fi

  echo ""
  echo "Test 10: Checking native planner JSON contract output..."
  UPDATE_PLAN_OUTPUT="$($KOG_BIN workspace update --native-plan-only)"
  FOREACH_PLAN_OUTPUT="$($KOG_BIN workspace foreach --native-plan-only --command "git status --porcelain")"
  if [[ "$UPDATE_PLAN_OUTPUT" == *'"planner":"native-submodule-update"'* && "$UPDATE_PLAN_OUTPUT" == *'"shell_adapter"'* && "$UPDATE_PLAN_OUTPUT" == *'"script":"workspace/update-workspace-repos.sh"'* && "$UPDATE_PLAN_OUTPUT" == *'"operations"'* && "$FOREACH_PLAN_OUTPUT" == *'"planner":"native-foreach"'* && "$FOREACH_PLAN_OUTPUT" == *'"shell_adapter"'* && "$FOREACH_PLAN_OUTPUT" == *'"script":"workspace/foreach-repo.sh"'* && "$FOREACH_PLAN_OUTPUT" == *'"command"'* && "$FOREACH_PLAN_OUTPUT" == *'"operations"'* ]]; then
    echo "  ✓ native planner contract output is valid"
  else
    echo "  ✗ native planner contract output is invalid"
    exit 1
  fi
else
  echo "  - skipped (no built C++ binary found)"
fi

echo ""
echo "=============================="
echo "All quick tests passed! ✓"
echo "=============================="
echo ""
echo "Run full test suite with:"
echo "  ./src/shell/test/run-all-tests.sh --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git"
