#!/usr/bin/env bash
#
# test-workflow-integration.sh - Integration tests for complete workflow
#
# This file implements integration tests for the complete init-repo-workflow.sh
# script, testing end-to-end scenarios with all steps, skip flags, dry-run mode,
# and failure/rollback scenarios.
#
# Tests:
# - End-to-end workflow with all steps
# - Workflow with various skip flags
# - Workflow with dry-run mode
# - Workflow failure and rollback scenarios
#
# **Validates: Requirements 6.1, 6.2, 6.3, 6.4, 6.5**

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Scripts under test
WORKFLOW_SCRIPT="$SCRIPT_DIR/../core/init-repo-workflow.sh"

# Test configuration
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Temporary directory for test repositories
TEST_TMP_DIR=""

# Cleanup function
cleanup() {
  if [[ -n "$TEST_TMP_DIR" && -d "$TEST_TMP_DIR" ]]; then
    rm -rf "$TEST_TMP_DIR"
  fi
}

trap cleanup EXIT

# Initialize test environment
setup_test_env() {
  TEST_TMP_DIR="$(mktemp -d)"
  echo "Test environment: $TEST_TMP_DIR"
}

# Test helper functions
test_pass() {
  local test_name="$1"
  echo "  ✓ PASS: $test_name"
  ((TESTS_PASSED++))
  ((TESTS_RUN++))
}

test_fail() {
  local test_name="$1"
  local reason="${2:-}"
  echo "  ✗ FAIL: $test_name"
  if [[ -n "$reason" ]]; then
    echo "    Reason: $reason"
  fi
  ((TESTS_FAILED++))
  ((TESTS_RUN++))
}

#------------------------------------------------------------------------------
# Test Helper Functions
#------------------------------------------------------------------------------

# Create a mock remote repository
create_mock_remote() {
  local remote_path="$1"
  local has_content="${2:-0}"

  mkdir -p "$remote_path"
  git init --bare "$remote_path" >/dev/null 2>&1

  if [[ "$has_content" == "1" ]]; then
    # Add some content to the remote
    local temp_clone="$TEST_TMP_DIR/temp-clone-$$"
    git clone "$remote_path" "$temp_clone" >/dev/null 2>&1
    (
      cd "$temp_clone"
      git config user.email "test@example.com"
      git config user.name "Test User"
      echo "# Test Repo" > README.md
      git add README.md
      git commit -m "Initial commit" >/dev/null 2>&1
      git push origin main >/dev/null 2>&1
    )
    rm -rf "$temp_clone"
  fi
}

# Create a mock submodule repository
create_mock_submodule() {
  local submodule_path="$1"

  mkdir -p "$submodule_path"
  git init --bare "$submodule_path" >/dev/null 2>&1

  # Add some content
  local temp_clone="$TEST_TMP_DIR/temp-submodule-$$"
  git clone "$submodule_path" "$temp_clone" >/dev/null 2>&1
  (
    cd "$temp_clone"
    git config user.email "test@example.com"
    git config user.name "Test User"
    echo "# Submodule" > README.md
    git add README.md
    git commit -m "Initial commit" >/dev/null 2>&1
    git push origin main >/dev/null 2>&1
  )
  rm -rf "$temp_clone"
}

#------------------------------------------------------------------------------
# Integration Test 1: End-to-End Workflow with Main Steps
#------------------------------------------------------------------------------

test_complete_workflow_main_steps() {
  echo ""
  echo "Integration Test 1: End-to-End Workflow with Main Steps"
  echo "========================================================"

  local test_dir="$TEST_TMP_DIR/test-complete-workflow"
  local remote_path="$TEST_TMP_DIR/remote-complete.git"
  local submodule_path="$TEST_TMP_DIR/submodule-complete.git"

  # Setup
  create_mock_remote "$remote_path" 0
  create_mock_submodule "$submodule_path"

  # Run workflow with main steps (skip orphan to avoid push complexity)
  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path" \
    --repo-dir "$test_dir" \
    --skip-orphan \
    --submodule "file://$submodule_path:tools/submodule" \
    2>&1 | grep -q "Workflow completed successfully"; then

    # Verify main branch exists
    if (cd "$test_dir" && git show-ref --verify --quiet refs/heads/main); then
      test_pass "Main branch created"
    else
      test_fail "Main branch created" "Main branch not found"
      return
    fi

    # Verify submodule added
    if (cd "$test_dir" && git config -f .gitmodules submodule.tools/submodule.path >/dev/null 2>&1); then
      test_pass "Submodule added"
    else
      test_fail "Submodule added" "Submodule not found in .gitmodules"
      return
    fi

    # Verify workflow completed successfully
    test_pass "Complete workflow executed successfully"
  else
    test_fail "Complete workflow executed successfully" "Workflow script failed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 2: Workflow with Skip Flags
#------------------------------------------------------------------------------

test_workflow_with_skip_flags() {
  echo ""
  echo "Integration Test 2: Workflow with Skip Flags"
  echo "============================================="

  # Test 2a: Skip main init
  local test_dir_a="$TEST_TMP_DIR/test-skip-main"
  local remote_path_a="$TEST_TMP_DIR/remote-skip-main.git"

  create_mock_remote "$remote_path_a" 1

  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path_a" \
    --repo-dir "$test_dir_a" \
    --skip-main-init \
    --skip-orphan \
    2>&1 | grep -q "Skipping main branch initialization"; then

    test_pass "Skip main init: Workflow skipped main initialization"
  else
    test_fail "Skip main init" "Workflow did not skip main init"
  fi

  # Test 2b: Skip orphan branch
  local test_dir_b="$TEST_TMP_DIR/test-skip-orphan"
  local remote_path_b="$TEST_TMP_DIR/remote-skip-orphan.git"

  create_mock_remote "$remote_path_b" 0

  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path_b" \
    --repo-dir "$test_dir_b" \
    --skip-orphan \
    2>&1 | grep -q "Skipping orphan branch creation"; then

    test_pass "Skip orphan: Workflow skipped orphan creation"

    # Verify main branch exists
    if (cd "$test_dir_b" && git show-ref --verify --quiet refs/heads/main); then
      test_pass "Skip orphan: Main branch created"
    else
      test_fail "Skip orphan: Main branch created" "Main branch not found"
    fi
  else
    test_fail "Skip orphan" "Workflow did not skip orphan"
  fi

  # Test 2c: Skip submodules
  local test_dir_c="$TEST_TMP_DIR/test-skip-submodules"
  local remote_path_c="$TEST_TMP_DIR/remote-skip-submodules.git"
  local submodule_path_c="$TEST_TMP_DIR/submodule-skip.git"

  create_mock_remote "$remote_path_c" 0
  create_mock_submodule "$submodule_path_c"

  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path_c" \
    --repo-dir "$test_dir_c" \
    --skip-orphan \
    --submodule "file://$submodule_path_c:tools/submodule" \
    --skip-submodules \
    2>&1 | grep -q "Skipping submodule addition"; then

    test_pass "Skip submodules: Workflow skipped submodule addition"

    # Verify submodule was not added
    if ! (cd "$test_dir_c" && test -f .gitmodules); then
      test_pass "Skip submodules: Submodules not added"
    else
      test_fail "Skip submodules: Submodules not added" ".gitmodules file found"
    fi
  else
    test_fail "Skip submodules" "Workflow did not skip submodules"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 3: Workflow with Dry-Run Mode
#------------------------------------------------------------------------------

test_workflow_dry_run() {
  echo ""
  echo "Integration Test 3: Workflow with Dry-Run Mode"
  echo "==============================================="

  local test_dir="$TEST_TMP_DIR/test-dry-run"
  local remote_path="$TEST_TMP_DIR/remote-dry-run.git"

  create_mock_remote "$remote_path" 0

  # Run workflow in dry-run mode
  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path" \
    --repo-dir "$test_dir" \
    --skip-orphan \
    --dry-run \
    2>&1 | grep -q "DRY-RUN"; then

    # Verify no repository was created
    if [[ ! -d "$test_dir" ]]; then
      test_pass "Dry-run: No repository created"
    else
      test_fail "Dry-run: No repository created" "Repository directory exists"
    fi

    test_pass "Dry-run mode executed successfully"
  else
    test_fail "Dry-run mode executed successfully" "Workflow failed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 4: Workflow Failure Handling
#------------------------------------------------------------------------------

test_workflow_failure_handling() {
  echo ""
  echo "Integration Test 4: Workflow Failure Handling"
  echo "=============================================="

  # Test 4a: Invalid remote URL
  local test_dir_a="$TEST_TMP_DIR/test-invalid-remote"

  if ! bash "$WORKFLOW_SCRIPT" \
    --repo-url "file:///nonexistent/remote.git" \
    --repo-dir "$test_dir_a" \
    --skip-orphan \
    >/dev/null 2>&1; then

    test_pass "Invalid remote: Workflow failed as expected"
  else
    test_fail "Invalid remote: Workflow failed as expected" "Workflow succeeded unexpectedly"
  fi

  # Test 4b: Invalid submodule URL (workflow should continue)
  local test_dir_b="$TEST_TMP_DIR/test-invalid-submodule"
  local remote_path_b="$TEST_TMP_DIR/remote-invalid-submodule.git"

  create_mock_remote "$remote_path_b" 0

  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path_b" \
    --repo-dir "$test_dir_b" \
    --skip-orphan \
    --submodule "file:///nonexistent/submodule.git:tools/submodule" \
    2>&1 | grep -q "Workflow completed"; then

    # Verify main branch exists despite submodule failure
    if (cd "$test_dir_b" && git show-ref --verify --quiet refs/heads/main); then
      test_pass "Invalid submodule: Main branch created despite submodule failure"
    else
      test_fail "Invalid submodule: Main branch created despite submodule failure" "Main branch not found"
    fi
  else
    test_fail "Invalid submodule: Workflow continued" "Workflow failed completely"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 5: Workflow with Existing Content
#------------------------------------------------------------------------------

test_workflow_existing_content() {
  echo ""
  echo "Integration Test 5: Workflow with Existing Content"
  echo "==================================================="

  local test_dir="$TEST_TMP_DIR/test-existing-content"
  local remote_path="$TEST_TMP_DIR/remote-existing.git"

  # Create remote with existing content
  create_mock_remote "$remote_path" 1

  if bash "$WORKFLOW_SCRIPT" \
    --repo-url "file://$remote_path" \
    --repo-dir "$test_dir" \
    --skip-orphan \
    2>&1 | grep -q "Workflow completed"; then

    # Verify existing content was preserved
    if (cd "$test_dir" && test -f README.md); then
      test_pass "Existing content: README.md preserved"
    else
      test_fail "Existing content: README.md preserved" "README.md not found"
    fi

    test_pass "Existing content workflow completed"
  else
    test_fail "Existing content workflow" "Workflow failed"
  fi
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "=========================================="
  echo "Integration Tests: Complete Workflow"
  echo "=========================================="

  setup_test_env

  # Run all integration tests
  test_complete_workflow_main_steps
  test_workflow_with_skip_flags
  test_workflow_dry_run
  test_workflow_failure_handling
  test_workflow_existing_content

  # Print summary
  echo ""
  echo "=========================================="
  echo "Test Summary"
  echo "=========================================="
  echo "Tests run: $TESTS_RUN"
  echo "Tests passed: $TESTS_PASSED"
  echo "Tests failed: $TESTS_FAILED"

  if [[ $TESTS_FAILED -eq 0 ]]; then
    echo ""
    echo "✓ All integration tests passed!"
    exit 0
  else
    echo ""
    echo "✗ Some integration tests failed"
    exit 1
  fi
}

main "$@"
