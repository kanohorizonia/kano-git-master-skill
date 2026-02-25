#!/usr/bin/env bash
#
# test-stash-functions.sh - Test stash management functions
#
# Usage: ./test-stash-functions.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counter
TESTS_PASSED=0
TESTS_FAILED=0

# Helper function to print test results
print_result() {
  local test_name="$1"
  local result="$2"
  
  if [[ "$result" == "PASS" ]]; then
    echo -e "${GREEN}✓${NC} $test_name"
    TESTS_PASSED=$((TESTS_PASSED + 1))
  else
    echo -e "${RED}✗${NC} $test_name"
    TESTS_FAILED=$((TESTS_FAILED + 1))
  fi
}

# Create a temporary test repository
create_test_repo() {
  local test_dir="$1"
  mkdir -p "$test_dir"
  (
    cd "$test_dir"
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test User"
    echo "initial" > file.txt
    git add file.txt
    git commit -q -m "Initial commit"
  )
}

# Cleanup test repository
cleanup_test_repo() {
  local test_dir="$1"
  if [[ -d "$test_dir" ]]; then
    rm -rf "$test_dir"
  fi
}

echo "Testing stash management functions..."
echo ""

# Test 1: gith_has_changes on clean repo
TEST_DIR="/tmp/gith-test-$$"
create_test_repo "$TEST_DIR"

if gith_has_changes "$TEST_DIR"; then
  print_result "gith_has_changes on clean repo" "FAIL"
else
  print_result "gith_has_changes on clean repo" "PASS"
fi

# Test 2: gith_has_changes on dirty repo
echo "modified" > "$TEST_DIR/file.txt"
if gith_has_changes "$TEST_DIR" 2>/dev/null; then
  print_result "gith_has_changes on dirty repo" "PASS"
else
  print_result "gith_has_changes on dirty repo" "FAIL"
fi

# Test 3: gith_stash_create
stash_ref=$(gith_stash_create "$TEST_DIR" "test-stash") || true
if [[ -n "$stash_ref" ]] && [[ "$stash_ref" == stash@* ]]; then
  print_result "gith_stash_create returns stash ref" "PASS"
else
  print_result "gith_stash_create returns stash ref" "FAIL"
fi

# Test 4: Verify repo is clean after stash
if gith_has_changes "$TEST_DIR"; then
  print_result "Repo is clean after stash" "FAIL"
else
  print_result "Repo is clean after stash" "PASS"
fi

# Test 5: gith_stash_pop
if gith_stash_pop "$TEST_DIR" "$stash_ref"; then
  print_result "gith_stash_pop succeeds" "PASS"
else
  print_result "gith_stash_pop succeeds" "FAIL"
fi

# Test 6: Verify changes restored after pop
if gith_has_changes "$TEST_DIR"; then
  print_result "Changes restored after pop" "PASS"
else
  print_result "Changes restored after pop" "FAIL"
fi

# Test 7: gith_stash_create with no changes
(
  cd "$TEST_DIR"
  git add file.txt
  git commit -q -m "Commit changes"
)

stash_ref=$(gith_stash_create "$TEST_DIR" "no-changes-stash") || true
# When there are no changes, stash_ref should be empty
if [[ -z "$stash_ref" ]]; then
  print_result "gith_stash_create with no changes" "PASS"
else
  print_result "gith_stash_create with no changes" "FAIL"
fi

# Test 8: gith_stash_pop with non-existent stash
if gith_stash_pop "$TEST_DIR" "stash@{999}"; then
  print_result "gith_stash_pop with non-existent stash" "PASS"
else
  print_result "gith_stash_pop with non-existent stash" "FAIL"
fi

# Test 9: Test with untracked files
echo "untracked" > "$TEST_DIR/untracked.txt"
stash_ref=$(gith_stash_create "$TEST_DIR" "untracked-test") || true
if [[ -n "$stash_ref" ]] && [[ ! -f "$TEST_DIR/untracked.txt" ]]; then
  print_result "gith_stash_create includes untracked files" "PASS"
else
  print_result "gith_stash_create includes untracked files" "FAIL"
fi

# Restore untracked file
gith_stash_pop "$TEST_DIR" "$stash_ref" || true
if [[ -f "$TEST_DIR/untracked.txt" ]]; then
  print_result "gith_stash_pop restores untracked files" "PASS"
else
  print_result "gith_stash_pop restores untracked files" "FAIL"
fi

# Cleanup
cleanup_test_repo "$TEST_DIR"

# Summary
echo ""
echo "================================"
echo "Test Results:"
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo "================================"

if [[ $TESTS_FAILED -eq 0 ]]; then
  echo -e "${GREEN}All tests passed!${NC}"
  exit 0
else
  echo -e "${RED}Some tests failed!${NC}"
  exit 1
fi
