#!/usr/bin/env bash
#
# test-worktree-scripts.sh - Test worktree management scripts
#
# This script tests all worktree-related functionality
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKTREE_DIR="$SCRIPT_DIR/../worktree"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test result tracking
test_pass() {
  echo -e "${GREEN}[✓]${NC} $1"
  ((TESTS_PASSED++))
  ((TESTS_RUN++))
}

test_fail() {
  echo -e "${RED}[✗]${NC} $1"
  ((TESTS_FAILED++))
  ((TESTS_RUN++))
}

test_info() {
  echo -e "${YELLOW}[i]${NC} $1"
}

# Create temporary test repository
setup_test_repo() {
  test_info "Setting up test repository..."
  
  TEST_REPO=$(mktemp -d)
  cd "$TEST_REPO"
  
  git init
  git config user.email "test@example.com"
  git config user.name "Test User"
  
  echo "# Test Repository" > README.md
  git add README.md
  git commit -m "Initial commit"
  
  test_info "Test repository created at: $TEST_REPO"
}

# Cleanup test repository
cleanup_test_repo() {
  test_info "Cleaning up test repository..."
  
  # Remove all worktrees first
  cd "$TEST_REPO"
  git worktree list --porcelain | grep "^worktree " | cut -d' ' -f2 | while read -r path; do
    if [[ "$path" != "$TEST_REPO" ]]; then
      git worktree remove --force "$path" 2>/dev/null || true
    fi
  done
  
  cd /
  rm -rf "$TEST_REPO"
  
  test_info "Cleanup complete"
}

# Test 1: Create worktree for existing branch
test_create_worktree_existing_branch() {
  test_info "Test 1: Create worktree for existing branch"
  
  cd "$TEST_REPO"
  
  # Create a new branch
  git checkout -b feature-test
  echo "Feature content" > feature.txt
  git add feature.txt
  git commit -m "Add feature"
  git checkout main
  
  # Create worktree
  if "$WORKTREE_DIR/create-worktree.sh" feature-test; then
    # Check if worktree was created
    if [[ -d "../$(basename "$TEST_REPO")-feature-test" ]]; then
      test_pass "Worktree created successfully"
    else
      test_fail "Worktree directory not found"
    fi
  else
    test_fail "Failed to create worktree"
  fi
}

# Test 2: Create worktree with custom path
test_create_worktree_custom_path() {
  test_info "Test 2: Create worktree with custom path"
  
  cd "$TEST_REPO"
  
  # Create branch
  git checkout -b custom-path-test
  git checkout main
  
  CUSTOM_PATH="/tmp/test-worktree-custom"
  
  # Create worktree with custom path
  if "$WORKTREE_DIR/create-worktree.sh" custom-path-test --path "$CUSTOM_PATH"; then
    if [[ -d "$CUSTOM_PATH" ]]; then
      test_pass "Worktree created at custom path"
      rm -rf "$CUSTOM_PATH"
    else
      test_fail "Custom path not found"
    fi
  else
    test_fail "Failed to create worktree with custom path"
  fi
}

# Test 3: Create worktree for new branch
test_create_worktree_new_branch() {
  test_info "Test 3: Create worktree for new branch"
  
  cd "$TEST_REPO"
  
  # Create worktree with new branch
  if "$WORKTREE_DIR/create-worktree.sh" new-branch-test --new-branch; then
    if [[ -d "../$(basename "$TEST_REPO")-new-branch-test" ]]; then
      test_pass "Worktree created with new branch"
    else
      test_fail "Worktree directory not found"
    fi
  else
    test_fail "Failed to create worktree with new branch"
  fi
}

# Test 4: Create orphan branch with worktree
test_create_orphan_worktree() {
  test_info "Test 4: Create orphan branch with worktree"
  
  cd "$TEST_REPO"
  
  # Create orphan worktree
  if "$WORKTREE_DIR/create-orphan-worktree.sh" docs; then
    DOCS_PATH="../$(basename "$TEST_REPO")-docs"
    if [[ -d "$DOCS_PATH" ]]; then
      # Check if it's an orphan branch
      cd "$DOCS_PATH"
      PARENT_COUNT=$(git rev-list --parents HEAD | tail -1 | wc -w)
      if [[ "$PARENT_COUNT" -eq 1 ]]; then
        test_pass "Orphan worktree created successfully"
      else
        test_fail "Branch is not orphan"
      fi
      cd "$TEST_REPO"
    else
      test_fail "Orphan worktree directory not found"
    fi
  else
    test_fail "Failed to create orphan worktree"
  fi
}

# Test 5: List worktrees
test_list_worktrees() {
  test_info "Test 5: List worktrees"
  
  cd "$TEST_REPO"
  
  # List worktrees
  if "$WORKTREE_DIR/list-worktrees.sh" > /dev/null; then
    test_pass "List worktrees succeeded"
  else
    test_fail "Failed to list worktrees"
  fi
  
  # Test JSON format
  if "$WORKTREE_DIR/list-worktrees.sh" --format json > /dev/null; then
    test_pass "List worktrees JSON format succeeded"
  else
    test_fail "Failed to list worktrees in JSON format"
  fi
}

# Test 6: Remove worktree
test_remove_worktree() {
  test_info "Test 6: Remove worktree"
  
  cd "$TEST_REPO"
  
  # Create a worktree to remove
  git checkout -b remove-test
  git checkout main
  "$WORKTREE_DIR/create-worktree.sh" remove-test
  
  REMOVE_PATH="../$(basename "$TEST_REPO")-remove-test"
  
  # Remove worktree
  if "$WORKTREE_DIR/remove-worktree.sh" remove-test; then
    if [[ ! -d "$REMOVE_PATH" ]]; then
      test_pass "Worktree removed successfully"
    else
      test_fail "Worktree directory still exists"
    fi
  else
    test_fail "Failed to remove worktree"
  fi
}

# Test 7: Remove worktree with branch deletion
test_remove_worktree_with_branch() {
  test_info "Test 7: Remove worktree with branch deletion"
  
  cd "$TEST_REPO"
  
  # Create a worktree to remove
  git checkout -b remove-with-branch-test
  git checkout main
  "$WORKTREE_DIR/create-worktree.sh" remove-with-branch-test
  
  # Remove worktree and branch
  if "$WORKTREE_DIR/remove-worktree.sh" remove-with-branch-test --delete-branch; then
    # Check if branch was deleted
    if ! git show-ref --verify --quiet refs/heads/remove-with-branch-test; then
      test_pass "Worktree and branch removed successfully"
    else
      test_fail "Branch still exists"
    fi
  else
    test_fail "Failed to remove worktree with branch"
  fi
}

# Test 8: Dry-run mode
test_dry_run_mode() {
  test_info "Test 8: Dry-run mode"
  
  cd "$TEST_REPO"
  
  # Test dry-run for create-worktree
  if "$WORKTREE_DIR/create-worktree.sh" main --dry-run > /dev/null; then
    test_pass "Dry-run mode works for create-worktree"
  else
    test_fail "Dry-run mode failed for create-worktree"
  fi
  
  # Test dry-run for create-orphan-worktree
  if "$WORKTREE_DIR/create-orphan-worktree.sh" test-orphan --dry-run > /dev/null; then
    test_pass "Dry-run mode works for create-orphan-worktree"
  else
    test_fail "Dry-run mode failed for create-orphan-worktree"
  fi
}

# Test 9: Error handling - worktree already exists
test_error_worktree_exists() {
  test_info "Test 9: Error handling - worktree already exists"
  
  cd "$TEST_REPO"
  
  # Create worktree
  git checkout -b exists-test
  git checkout main
  "$WORKTREE_DIR/create-worktree.sh" exists-test
  
  # Try to create again (should fail)
  if ! "$WORKTREE_DIR/create-worktree.sh" exists-test 2>/dev/null; then
    test_pass "Correctly prevents duplicate worktree creation"
  else
    test_fail "Should have failed when worktree already exists"
  fi
}

# Test 10: Error handling - branch doesn't exist
test_error_branch_not_exists() {
  test_info "Test 10: Error handling - branch doesn't exist"
  
  cd "$TEST_REPO"
  
  # Try to create worktree for non-existent branch
  if ! "$WORKTREE_DIR/create-worktree.sh" non-existent-branch 2>/dev/null; then
    test_pass "Correctly fails when branch doesn't exist"
  else
    test_fail "Should have failed when branch doesn't exist"
  fi
}

# Main test execution
main() {
  echo "=========================================="
  echo "Worktree Scripts Test Suite"
  echo "=========================================="
  echo ""
  
  # Setup
  setup_test_repo
  
  # Run tests
  test_create_worktree_existing_branch
  test_create_worktree_custom_path
  test_create_worktree_new_branch
  test_create_orphan_worktree
  test_list_worktrees
  test_remove_worktree
  test_remove_worktree_with_branch
  test_dry_run_mode
  test_error_worktree_exists
  test_error_branch_not_exists
  
  # Cleanup
  cleanup_test_repo
  
  # Summary
  echo ""
  echo "=========================================="
  echo "Test Summary"
  echo "=========================================="
  echo "Total tests: $TESTS_RUN"
  echo -e "${GREEN}Passed: $TESTS_PASSED${NC}"
  echo -e "${RED}Failed: $TESTS_FAILED${NC}"
  echo ""
  
  if [[ "$TESTS_FAILED" -eq 0 ]]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
  else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
  fi
}

# Run tests
main
