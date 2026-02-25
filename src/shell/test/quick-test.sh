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
echo "=============================="
echo "All quick tests passed! ✓"
echo "=============================="
echo ""
echo "Run full test suite with:"
echo "  ./src/shell/test/run-all-tests.sh --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git"
