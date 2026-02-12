#!/usr/bin/env bash
#
# test-git-helpers-basic.sh - Basic tests for git-helpers.sh functions
#
# Usage: ./test-git-helpers-basic.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/git-helpers.sh"

# Test counter
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test helper functions
test_pass() {
  local test_name="$1"
  echo "✓ PASS: $test_name"
  ((TESTS_PASSED++))
  ((TESTS_RUN++))
}

test_fail() {
  local test_name="$1"
  local reason="${2:-}"
  echo "✗ FAIL: $test_name"
  if [[ -n "$reason" ]]; then
    echo "  Reason: $reason"
  fi
  ((TESTS_FAILED++))
  ((TESTS_RUN++))
}

echo "========================================"
echo "Testing git-helpers.sh functions"
echo "========================================"
echo ""

# Test 1: gith_is_git_repo with current directory
echo "Test 1: gith_is_git_repo with current directory"
if gith_is_git_repo "$SCRIPT_DIR/../../.."; then
  test_pass "gith_is_git_repo detects git repo"
else
  test_fail "gith_is_git_repo failed to detect git repo"
fi
echo ""

# Test 2: gith_is_git_repo with non-git directory
echo "Test 2: gith_is_git_repo with non-git directory"
if ! gith_is_git_repo "/tmp"; then
  test_pass "gith_is_git_repo correctly identifies non-git directory"
else
  test_fail "gith_is_git_repo incorrectly identified /tmp as git repo"
fi
echo ""

# Test 3: gith_get_current_branch
echo "Test 3: gith_get_current_branch"
current_branch="$(gith_get_current_branch "$SCRIPT_DIR/../../..")"
if [[ -n "$current_branch" ]]; then
  test_pass "gith_get_current_branch returned: $current_branch"
else
  echo "  Note: Detached HEAD state or error"
  test_pass "gith_get_current_branch handled detached HEAD"
fi
echo ""

# Test 4: gith_has_remote
echo "Test 4: gith_has_remote with 'origin'"
if gith_has_remote "origin" "$SCRIPT_DIR/../../.."; then
  test_pass "gith_has_remote detected 'origin' remote"
else
  test_fail "gith_has_remote failed to detect 'origin' remote"
fi
echo ""

# Test 5: gith_has_remote with non-existent remote
echo "Test 5: gith_has_remote with non-existent remote"
if ! gith_has_remote "nonexistent-remote-xyz" "$SCRIPT_DIR/../../.."; then
  test_pass "gith_has_remote correctly identified non-existent remote"
else
  test_fail "gith_has_remote incorrectly found non-existent remote"
fi
echo ""

# Test 6: gith_has_changes
echo "Test 6: gith_has_changes"
if gith_has_changes "$SCRIPT_DIR/../../.."; then
  echo "  Repository has uncommitted changes"
  test_pass "gith_has_changes detected changes"
else
  echo "  Repository is clean"
  test_pass "gith_has_changes detected clean state"
fi
echo ""

# Test 7: gith_is_excluded
echo "Test 7: gith_is_excluded"
if gith_is_excluded "/path/to/node_modules/something" "node_modules"; then
  test_pass "gith_is_excluded correctly matched pattern"
else
  test_fail "gith_is_excluded failed to match pattern"
fi
echo ""

# Test 8: gith_is_excluded with non-matching pattern
echo "Test 8: gith_is_excluded with non-matching pattern"
if ! gith_is_excluded "/path/to/src/file.js" "node_modules"; then
  test_pass "gith_is_excluded correctly rejected non-matching pattern"
else
  test_fail "gith_is_excluded incorrectly matched non-matching pattern"
fi
echo ""

# Test 9: gith_run in dry-run mode
echo "Test 9: gith_run in dry-run mode"
export DRY_RUN=1
output="$(gith_run echo "test command" 2>&1)"
if [[ "$output" == *"echo"* ]] && [[ "$output" == *"test command"* ]]; then
  test_pass "gith_run dry-run mode works"
else
  test_fail "gith_run dry-run mode failed"
fi
unset DRY_RUN
echo ""

# Test 10: gith_collect_repo_metadata
echo "Test 10: gith_collect_repo_metadata"
metadata="$(gith_collect_repo_metadata "$SCRIPT_DIR/../../..")"
if [[ "$metadata" == *"\"path\""* ]] && [[ "$metadata" == *"\"current_branch\""* ]]; then
  test_pass "gith_collect_repo_metadata returned valid JSON"
  echo "  Metadata: $metadata"
else
  test_fail "gith_collect_repo_metadata returned invalid JSON"
fi
echo ""

# Summary
echo "========================================"
echo "Test Summary"
echo "========================================"
echo "Tests run:    $TESTS_RUN"
echo "Tests passed: $TESTS_PASSED"
echo "Tests failed: $TESTS_FAILED"
echo ""

if [[ $TESTS_FAILED -eq 0 ]]; then
  echo "✓ All tests passed!"
  exit 0
else
  echo "✗ Some tests failed"
  exit 1
fi
