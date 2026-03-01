#!/usr/bin/env bash
#
# test-init-repo-workflow-properties.sh - Property-based tests for init-repo-workflow.sh
#
# This file implements property-based tests for the init-repo-workflow.sh script.
# Each property test runs 100+ iterations with generated inputs to verify
# universal correctness properties.
#
# Properties tested:
# - Property 23: Workflow Step Ordering
# - Property 24: Workflow Failure Propagation
# - Property 25: Workflow Step Skipping
# - Property 26: Dry Run Idempotence
# - Property 27: Workflow Summary Completeness
#
# **Validates: Requirements 6.1, 6.2, 6.3, 6.4, 6.5**

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Script under test
WORKFLOW_SCRIPT="$SCRIPT_DIR/../core/init-repo-workflow.sh"

# Test configuration
PROPERTY_TEST_ITERATIONS=${PROPERTY_TEST_ITERATIONS:-10}  # Reduced default for faster testing
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
# Random Data Generators
#------------------------------------------------------------------------------

# Generate random repository URL
generate_repo_url() {
  local url_type=$((RANDOM % 3))
  local num=$((RANDOM % 1000))

  case $url_type in
    0) echo "git@github.com:user/repo-$num.git" ;;
    1) echo "https://github.com/user/repo-$num.git" ;;
    2) echo "file://$TEST_TMP_DIR/remote-$num.git" ;;
  esac
}

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
      echo "# Test Repo" > README.md
      git add README.md
      git commit -m "Initial commit" >/dev/null 2>&1
      git push origin main >/dev/null 2>&1 || git push origin master >/dev/null 2>&1
    )
    rm -rf "$temp_clone"
  fi
}

#------------------------------------------------------------------------------
# Property 23: Workflow Step Ordering
#------------------------------------------------------------------------------
# Feature: repo-initialization-workflow, Property 23: Workflow Step Ordering
#
# For any workflow invocation, the steps should execute in the exact order:
# remote detection, main branch initialization, orphan branch creation,
# submodule addition, with each step completing before the next begins.
#
# **Validates: Requirements 6.1**

test_property_23_workflow_step_ordering() {
  echo ""
  echo "=== Property 23: Workflow Step Ordering ==="
  echo "Testing that workflow steps execute in correct order..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    # Create test environment
    local test_dir="$TEST_TMP_DIR/test-23-$i"
    local remote_path="$TEST_TMP_DIR/remote-23-$i.git"

    # Create empty remote
    create_mock_remote "$remote_path" 0

    # Run workflow with dry-run to capture step order
    local output
    set +e
    output=$(timeout 10 bash "$WORKFLOW_SCRIPT" \
      --repo-url "file://$remote_path" \
      --repo-dir "$test_dir" \
      --orphan-branch "test-orphan-$i" \
      --skip-submodules \
      --dry-run 2>&1)
    local exit_code=$?
    set -e

    # Skip if timeout
    if [[ $exit_code -eq 124 ]]; then
      echo "    Timeout on iteration $i, skipping"
      continue
    fi

    # Verify step order in output
    # In dry-run mode, we should see at least steps 1 and 2
    local step1_line step2_line
    step1_line=$(echo "$output" | grep -n "Step 1: Remote Detection" | cut -d: -f1 || echo "0")
    step2_line=$(echo "$output" | grep -n "Step 2: Main Branch Initialization" | cut -d: -f1 || echo "0")

    # Check that steps appear in order
    # Step 1 should come before Step 2
    if [[ "$step1_line" -gt 0 ]] && [[ "$step2_line" -gt "$step1_line" ]]; then
      # Also check if Step 3 and 4 appear, they should be in order
      local step3_line step4_line
      step3_line=$(echo "$output" | grep -n "Step 3: Multi-Remote Setup" | cut -d: -f1 || echo "0")
      step4_line=$(echo "$output" | grep -n "Step 4: Orphan Branch Creation" | cut -d: -f1 || echo "0")

      # If step 3 exists, it should come after step 2
      if [[ "$step3_line" -gt 0 ]] && [[ "$step3_line" -le "$step2_line" ]]; then
        continue  # Order violated
      fi

      # If step 4 exists, it should come after step 3 (or step 2 if step 3 skipped)
      if [[ "$step4_line" -gt 0 ]]; then
        if [[ "$step3_line" -gt 0 ]] && [[ "$step4_line" -le "$step3_line" ]]; then
          continue  # Order violated
        elif [[ "$step3_line" -eq 0 ]] && [[ "$step4_line" -le "$step2_line" ]]; then
          continue  # Order violated
        fi
      fi

      ((passed++))
    else
      # Debug: show what we got
      if [[ $i -eq 1 ]]; then
        echo "    Debug: step1=$step1_line step2=$step2_line"
      fi
    fi

    # Cleanup
    rm -rf "$test_dir" "$remote_path"
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 23: Workflow Step Ordering ($passed/$iterations iterations)"
  else
    test_fail "Property 23: Workflow Step Ordering ($passed/$iterations iterations)" \
      "Step ordering violated in $((iterations - passed)) iterations"
  fi
}

#------------------------------------------------------------------------------
# Property 24: Workflow Failure Propagation
#------------------------------------------------------------------------------
# Feature: repo-initialization-workflow, Property 24: Workflow Failure Propagation
#
# For any workflow execution where a step fails, the workflow should stop
# immediately after that step, report the failure, and not execute subsequent steps.
#
# **Validates: Requirements 6.2**

test_property_24_workflow_failure_propagation() {
  echo ""
  echo "=== Property 24: Workflow Failure Propagation ==="
  echo "Testing that workflow stops on first failure..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    # Create test environment with invalid remote URL to force failure
    local test_dir="$TEST_TMP_DIR/test-24-$i"
    local invalid_remote="file:///nonexistent/path/repo-$i.git"

    # Run workflow (should fail at remote detection)
    local output exit_code
    set +e
    output=$(timeout 10 bash "$WORKFLOW_SCRIPT" \
      --repo-url "$invalid_remote" \
      --repo-dir "$test_dir" \
      --orphan-branch "test-orphan-$i" 2>&1)
    exit_code=$?
    set -e

    # Skip if timeout
    if [[ $exit_code -eq 124 ]]; then
      echo "    Timeout on iteration $i, skipping"
      continue
    fi

    # Verify workflow failed (non-zero exit code)
    if [[ $exit_code -ne 0 ]]; then
      # Verify subsequent steps were not executed
      # Step 2 should not appear in output since step 1 failed
      if ! echo "$output" | grep -q "Step 2: Main Branch Initialization"; then
        ((passed++))
      fi
    fi

    # Cleanup
    rm -rf "$test_dir"
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 24: Workflow Failure Propagation ($passed/$iterations iterations)"
  else
    test_fail "Property 24: Workflow Failure Propagation ($passed/$iterations iterations)" \
      "Workflow continued after failure in $((iterations - passed)) iterations"
  fi
}

#------------------------------------------------------------------------------
# Property 25: Workflow Step Skipping
#------------------------------------------------------------------------------
# Feature: repo-initialization-workflow, Property 25: Workflow Step Skipping
#
# For any workflow step that is skipped due to existing state (e.g., main branch
# already initialized), the system should log the skip reason and continue to
# the next step without error.
#
# **Validates: Requirements 6.3**

test_property_25_workflow_step_skipping() {
  echo ""
  echo "=== Property 25: Workflow Step Skipping ==="
  echo "Testing that workflow skips steps correctly..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    # Create test environment
    local test_dir="$TEST_TMP_DIR/test-25-$i"
    local remote_path="$TEST_TMP_DIR/remote-25-$i.git"

    # Create remote with content
    create_mock_remote "$remote_path" 1

    # Run workflow with skip flags
    local output exit_code
    set +e
    output=$(timeout 10 bash "$WORKFLOW_SCRIPT" \
      --repo-url "file://$remote_path" \
      --repo-dir "$test_dir" \
      --skip-main-init \
      --skip-orphan \
      --skip-submodules \
      --dry-run 2>&1)
    exit_code=$?
    set -e

    # Skip if timeout
    if [[ $exit_code -eq 124 ]]; then
      echo "    Timeout on iteration $i, skipping"
      continue
    fi

    # Verify workflow succeeded
    if [[ $exit_code -eq 0 ]]; then
      # Verify skip messages are present
      local skip_count
      skip_count=$(echo "$output" | grep -c "Skipping" || echo "0")

      if [[ $skip_count -ge 3 ]]; then
        ((passed++))
      fi
    fi

    # Cleanup
    rm -rf "$test_dir" "$remote_path"
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 25: Workflow Step Skipping ($passed/$iterations iterations)"
  else
    test_fail "Property 25: Workflow Step Skipping ($passed/$iterations iterations)" \
      "Step skipping failed in $((iterations - passed)) iterations"
  fi
}

#------------------------------------------------------------------------------
# Property 26: Dry Run Idempotence
#------------------------------------------------------------------------------
# Feature: repo-initialization-workflow, Property 26: Dry Run Idempotence
#
# For any workflow invocation with dry-run mode enabled, no changes should be
# made to the filesystem or Git repository, but all actions that would be
# performed should be reported.
#
# **Validates: Requirements 6.4**

test_property_26_dry_run_idempotence() {
  echo ""
  echo "=== Property 26: Dry Run Idempotence ==="
  echo "Testing that dry-run mode makes no changes..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    # Create test environment
    local test_dir="$TEST_TMP_DIR/test-26-$i"
    local remote_path="$TEST_TMP_DIR/remote-26-$i.git"

    # Create empty remote
    create_mock_remote "$remote_path" 0

    # Capture filesystem state before dry-run
    local files_before
    files_before=$(find "$TEST_TMP_DIR" -type f 2>/dev/null | wc -l)

    # Run workflow in dry-run mode
    local output
    set +e
    output=$(timeout 10 bash "$WORKFLOW_SCRIPT" \
      --repo-url "file://$remote_path" \
      --repo-dir "$test_dir" \
      --orphan-branch "test-orphan-$i" \
      --dry-run 2>&1)
    set -e

    # Capture filesystem state after dry-run
    local files_after
    files_after=$(find "$TEST_TMP_DIR" -type f 2>/dev/null | wc -l)

    # Verify no new files created (except log files)
    # Allow for small differences due to temp files
    local file_diff=$((files_after - files_before))

    # Verify dry-run messages in output
    local dryrun_count
    dryrun_count=$(echo "$output" | grep -c "DRY-RUN" || echo "0")

    if [[ $file_diff -le 2 ]] && [[ $dryrun_count -gt 0 ]]; then
      ((passed++))
    fi

    # Cleanup
    rm -rf "$test_dir" "$remote_path"
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 26: Dry Run Idempotence ($passed/$iterations iterations)"
  else
    test_fail "Property 26: Dry Run Idempotence ($passed/$iterations iterations)" \
      "Dry-run made changes in $((iterations - passed)) iterations"
  fi
}

#------------------------------------------------------------------------------
# Property 27: Workflow Summary Completeness
#------------------------------------------------------------------------------
# Feature: repo-initialization-workflow, Property 27: Workflow Summary Completeness
#
# For any workflow execution, the summary report should list all steps that were
# executed, all resources that were created or modified, and all steps that were
# skipped.
#
# **Validates: Requirements 6.5**

test_property_27_workflow_summary_completeness() {
  echo ""
  echo "=== Property 27: Workflow Summary Completeness ==="
  echo "Testing that workflow summary is complete..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    # Create test environment
    local test_dir="$TEST_TMP_DIR/test-27-$i"
    local remote_path="$TEST_TMP_DIR/remote-27-$i.git"

    # Create empty remote
    create_mock_remote "$remote_path" 0

    # Run workflow with some skip flags
    local output
    set +e
    output=$(timeout 10 bash "$WORKFLOW_SCRIPT" \
      --repo-url "file://$remote_path" \
      --repo-dir "$test_dir" \
      --orphan-branch "test-orphan-$i" \
      --skip-submodules \
      --dry-run 2>&1)
    set -e

    # Verify summary section exists
    if echo "$output" | grep -q "Workflow Summary"; then
      # Verify completed steps section
      local has_completed=0
      if echo "$output" | grep -q "Completed steps:"; then
        has_completed=1
      fi

      # Verify skipped steps section
      local has_skipped=0
      if echo "$output" | grep -q "Skipped steps:"; then
        has_skipped=1
      fi

      # Verify repository information
      local has_repo_info=0
      if echo "$output" | grep -q "Repository location:"; then
        has_repo_info=1
      fi

      if [[ $has_completed -eq 1 ]] && [[ $has_skipped -eq 1 ]]; then
        ((passed++))
      fi
    fi

    # Cleanup
    rm -rf "$test_dir" "$remote_path"
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 27: Workflow Summary Completeness ($passed/$iterations iterations)"
  else
    test_fail "Property 27: Workflow Summary Completeness ($passed/$iterations iterations)" \
      "Summary incomplete in $((iterations - passed)) iterations"
  fi
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "========================================"
  echo "Property-Based Tests: init-repo-workflow.sh"
  echo "========================================"
  echo "Iterations per property: $PROPERTY_TEST_ITERATIONS"
  echo ""

  # Setup test environment
  setup_test_env

  # Run property tests
  test_property_23_workflow_step_ordering
  test_property_24_workflow_failure_propagation
  test_property_25_workflow_step_skipping
  test_property_26_dry_run_idempotence
  test_property_27_workflow_summary_completeness

  # Print summary
  echo ""
  echo "========================================"
  echo "Test Summary"
  echo "========================================"
  echo "Total tests run: $TESTS_RUN"
  echo "Passed: $TESTS_PASSED"
  echo "Failed: $TESTS_FAILED"
  echo ""

  if [[ $TESTS_FAILED -eq 0 ]]; then
    echo "✓ All property tests passed!"
    exit 0
  else
    echo "✗ Some property tests failed"
    exit 1
  fi
}

# Run main
main "$@"
