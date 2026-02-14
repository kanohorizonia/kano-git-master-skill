#!/usr/bin/env bash
#
# test-error-handling-properties.sh - Property-based tests for error handling
#
# This file implements property-based tests for the error handling functions
# in git-helpers.sh. Each property test runs 100+ iterations with generated
# inputs to verify universal correctness properties.
#
# Properties tested:
# - Property 34: Error Message Completeness
# - Property 35: Stash Recovery Instructions
# - Property 36: Remote Error Classification
# - Property 37: Operation Logging
#
# **Validates: Requirements 8.1, 8.2, 8.3, 8.4, 8.5**

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Test configuration
PROPERTY_TEST_ITERATIONS=${PROPERTY_TEST_ITERATIONS:-100}
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
  TESTS_PASSED=$((TESTS_PASSED + 1))
  TESTS_RUN=$((TESTS_RUN + 1))
}

test_fail() {
  local test_name="$1"
  local reason="${2:-}"
  echo "  ✗ FAIL: $test_name"
  if [[ -n "$reason" ]]; then
    echo "    Reason: $reason"
  fi
  TESTS_FAILED=$((TESTS_FAILED + 1))
  TESTS_RUN=$((TESTS_RUN + 1))
}

#------------------------------------------------------------------------------
# Random Data Generators
#------------------------------------------------------------------------------

# Generate random error type
generate_error_type() {
  local types=("validation" "state" "network" "git")
  echo "${types[$((RANDOM % ${#types[@]}))]}"
}

# Generate random error message
generate_error_message() {
  local messages=(
    "Operation failed"
    "Invalid input provided"
    "Network connection lost"
    "Git command failed"
    "Repository not found"
    "Permission denied"
  )
  echo "${messages[$((RANDOM % ${#messages[@]}))]}"
}

# Generate random stash operation
generate_stash_operation() {
  local operations=("create" "pop" "apply")
  echo "${operations[$((RANDOM % ${#operations[@]}))]}"
}

# Generate random remote error output
generate_remote_error_output() {
  local error_type=$((RANDOM % 4))

  case $error_type in
    0) echo "fatal: could not resolve host: github.com" ;;
    1) echo "fatal: Authentication failed for 'https://github.com/user/repo.git'" ;;
    2) echo "fatal: repository 'https://github.com/user/repo.git' not found" ;;
    3) echo "fatal: some unknown error occurred" ;;
  esac
}

# Generate random operation name
generate_operation_name() {
  local operations=(
    "clone-repository"
    "create-branch"
    "push-changes"
    "fetch-remote"
    "merge-branches"
  )
  echo "${operations[$((RANDOM % ${#operations[@]}))]}"
}

#------------------------------------------------------------------------------
# Property 34: Error Message Completeness
# Feature: repo-initialization-workflow, Property 34: Error Message Completeness
#
# For any error handled by the system, the error message should include both
# a description of what went wrong and specific recovery steps.
#
# **Validates: Requirements 8.1, 8.2**
#------------------------------------------------------------------------------

test_property_34_error_message_completeness() {
  echo ""
  echo "=== Property 34: Error Message Completeness ==="
  echo "Testing that error messages include description and recovery steps..."

  local passed=0
  local failed=0

  for i in $(seq 1 "$PROPERTY_TEST_ITERATIONS"); do
    local error_type
    error_type="$(generate_error_type)"
    local error_message
    error_message="$(generate_error_message)"

    # Capture error output (gith_handle_error exits, so run in subshell)
    # Redirect stderr to stdout, suppress exit code
    local error_output
    error_output=$(bash -c "source '$SCRIPT_DIR/../lib/git-helpers.sh' 2>/dev/null && gith_handle_error '$error_type' '$error_message' 'context info' 2>&1" || true)

    # Verify error message contains the original message
    if ! echo "$error_output" | grep -qF "$error_message"; then
      test_fail "Property 34 iteration $i" "Error message not found in output"
      failed=$((failed + 1))
      continue
    fi

    # Verify error output contains recovery instructions
    if ! echo "$error_output" | grep -qi "recovery"; then
      test_fail "Property 34 iteration $i" "No recovery instructions found"
      failed=$((failed + 1))
      continue
    fi

    # Verify error output contains context (if provided)
    if ! echo "$error_output" | grep -qF "context info"; then
      test_fail "Property 34 iteration $i" "Context information not included"
      failed=$((failed + 1))
      continue
    fi

    passed=$((passed + 1))
  done

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 34: Error Message Completeness ($passed/$PROPERTY_TEST_ITERATIONS iterations)"
  else
    test_fail "Property 34: Error Message Completeness" "$failed/$PROPERTY_TEST_ITERATIONS iterations failed"
  fi
}

#------------------------------------------------------------------------------
# Property 35: Stash Recovery Instructions
# Feature: repo-initialization-workflow, Property 35: Stash Recovery Instructions
#
# For any stash operation failure, the error message should include specific
# Git commands for manually inspecting and recovering the stash.
#
# **Validates: Requirements 8.3**
#------------------------------------------------------------------------------

test_property_35_stash_recovery_instructions() {
  echo ""
  echo "=== Property 35: Stash Recovery Instructions ==="
  echo "Testing that stash errors include recovery commands..."

  local passed=0
  local failed=0

  for i in $(seq 1 "$PROPERTY_TEST_ITERATIONS"); do
    local operation
    operation="$(generate_stash_operation)"
    local stash_ref="stash@{0}"
    local repo_path="$TEST_TMP_DIR/test-repo-$i"

    # Capture error output (gith_handle_stash_error exits, so run in subshell)
    local error_output
    error_output=$(bash -c "source '$SCRIPT_DIR/../lib/git-helpers.sh' 2>/dev/null && gith_handle_stash_error '$operation' '$stash_ref' '$repo_path' 2>&1" || true)

    # Verify error output contains manual recovery steps
    if ! echo "$error_output" | grep -q "Manual recovery steps:"; then
      test_fail "Property 35 iteration $i" "No manual recovery steps found"
      failed=$((failed + 1))
      continue
    fi

    # Verify error output contains git commands
    if ! echo "$error_output" | grep -q "git stash"; then
      test_fail "Property 35 iteration $i" "No git stash commands found"
      failed=$((failed + 1))
      continue
    fi

    # Verify error output contains the repository path
    if ! echo "$error_output" | grep -qF "$repo_path"; then
      test_fail "Property 35 iteration $i" "Repository path not included"
      failed=$((failed + 1))
      continue
    fi

    # For pop/apply operations, verify stash ref is mentioned
    if [[ "$operation" == "pop" || "$operation" == "apply" ]]; then
      if ! echo "$error_output" | grep -qF "$stash_ref"; then
        test_fail "Property 35 iteration $i" "Stash reference not included for $operation"
        failed=$((failed + 1))
        continue
      fi
    fi

    passed=$((passed + 1))
  done

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 35: Stash Recovery Instructions ($passed/$PROPERTY_TEST_ITERATIONS iterations)"
  else
    test_fail "Property 35: Stash Recovery Instructions" "$failed/$PROPERTY_TEST_ITERATIONS iterations failed"
  fi
}

#------------------------------------------------------------------------------
# Property 36: Remote Error Classification
# Feature: repo-initialization-workflow, Property 36: Remote Error Classification
#
# For any remote operation failure, the system should correctly classify the
# error as network error, authentication error, or repository error based on
# the failure symptoms.
#
# **Validates: Requirements 8.4**
#------------------------------------------------------------------------------

test_property_36_remote_error_classification() {
  echo ""
  echo "=== Property 36: Remote Error Classification ==="
  echo "Testing remote error classification accuracy..."

  local passed=0
  local failed=0

  # Test network errors
  local network_errors=(
    "fatal: could not resolve host: github.com"
    "fatal: unable to access 'https://github.com/': Connection timed out"
    "fatal: unable to access 'https://github.com/': Connection refused"
    "fatal: unable to access 'https://github.com/': Network is unreachable"
  )

  for error in "${network_errors[@]}"; do
    local classification
    classification="$(gith_classify_remote_error "$error")"

    if [[ "$classification" != "network" ]]; then
      test_fail "Property 36 network classification" "Expected 'network', got '$classification'"
      failed=$((failed + 1))
    else
      passed=$((passed + 1))
    fi
  done

  # Test authentication errors
  local auth_errors=(
    "fatal: Authentication failed for 'https://github.com/user/repo.git'"
    "fatal: unable to access 'https://github.com/': The requested URL returned error: 403"
    "fatal: unable to access 'https://github.com/': The requested URL returned error: 401"
    "Permission denied (publickey)"
  )

  for error in "${auth_errors[@]}"; do
    local classification
    classification="$(gith_classify_remote_error "$error")"

    if [[ "$classification" != "auth" ]]; then
      test_fail "Property 36 auth classification" "Expected 'auth', got '$classification'"
      failed=$((failed + 1))
    else
      passed=$((passed + 1))
    fi
  done

  # Test repository errors
  local repo_errors=(
    "fatal: repository 'https://github.com/user/repo.git' not found"
    "fatal: 'https://github.com/user/repo.git' does not appear to be a git repository"
    "fatal: unable to access 'https://github.com/user/repo.git/': The requested URL returned error: 404"
  )

  for error in "${repo_errors[@]}"; do
    local classification
    classification="$(gith_classify_remote_error "$error")"

    if [[ "$classification" != "repository" ]]; then
      test_fail "Property 36 repository classification" "Expected 'repository', got '$classification'"
      failed=$((failed + 1))
    else
      passed=$((passed + 1))
    fi
  done

  # Test unknown errors
  local unknown_errors=(
    "fatal: some completely unknown error"
    "error: something went wrong"
  )

  for error in "${unknown_errors[@]}"; do
    local classification
    classification="$(gith_classify_remote_error "$error")"

    if [[ "$classification" != "unknown" ]]; then
      test_fail "Property 36 unknown classification" "Expected 'unknown', got '$classification'"
      failed=$((failed + 1))
    else
      passed=$((passed + 1))
    fi
  done

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 36: Remote Error Classification ($passed tests)"
  else
    test_fail "Property 36: Remote Error Classification" "$failed tests failed"
  fi
}

#------------------------------------------------------------------------------
# Property 37: Operation Logging
# Feature: repo-initialization-workflow, Property 37: Operation Logging
#
# For any operation performed by the system, there should be corresponding
# log entries that record what was attempted and the result.
#
# **Validates: Requirements 8.5**
#------------------------------------------------------------------------------

test_property_37_operation_logging() {
  echo ""
  echo "=== Property 37: Operation Logging ==="
  echo "Testing operation logging completeness..."

  local passed=0
  local failed=0

  for i in $(seq 1 "$PROPERTY_TEST_ITERATIONS"); do
    local operation
    operation="$(generate_operation_name)"
    local details="detail-$i"

    # Test operation start logging
    local start_output
    start_output=$(gith_log_operation_start "$operation" "$details" 2>&1)

    if ! echo "$start_output" | grep -q "Operation started: $operation"; then
      test_fail "Property 37 iteration $i (start)" "Operation name not in start log"
      failed=$((failed + 1))
      continue
    fi

    if ! echo "$start_output" | grep -q "$details"; then
      test_fail "Property 37 iteration $i (start)" "Details not in start log"
      failed=$((failed + 1))
      continue
    fi

    if ! echo "$start_output" | grep -qE "\[20[0-9]{2}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\]"; then
      test_fail "Property 37 iteration $i (start)" "Timestamp not found in start log"
      failed=$((failed + 1))
      continue
    fi

    # Test operation result logging (success)
    local success_output
    success_output=$(gith_log_operation_result "$operation" "success" "$details" 2>&1)

    if ! echo "$success_output" | grep -q "Operation completed: $operation"; then
      test_fail "Property 37 iteration $i (success)" "Operation name not in success log"
      failed=$((failed + 1))
      continue
    fi

    if ! echo "$success_output" | grep -q "SUCCESS"; then
      test_fail "Property 37 iteration $i (success)" "SUCCESS marker not found"
      failed=$((failed + 1))
      continue
    fi

    # Test operation result logging (failure)
    local failure_output
    failure_output=$(gith_log_operation_result "$operation" "failure" "$details" 2>&1)

    if ! echo "$failure_output" | grep -q "Operation failed: $operation"; then
      test_fail "Property 37 iteration $i (failure)" "Operation name not in failure log"
      failed=$((failed + 1))
      continue
    fi

    if ! echo "$failure_output" | grep -q "FAILURE"; then
      test_fail "Property 37 iteration $i (failure)" "FAILURE marker not found"
      failed=$((failed + 1))
      continue
    fi

    passed=$((passed + 1))
  done

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 37: Operation Logging ($passed/$PROPERTY_TEST_ITERATIONS iterations)"
  else
    test_fail "Property 37: Operation Logging" "$failed/$PROPERTY_TEST_ITERATIONS iterations failed"
  fi
}

#------------------------------------------------------------------------------
# Main Test Execution
#------------------------------------------------------------------------------

main() {
  echo "=========================================="
  echo "Error Handling Property-Based Tests"
  echo "=========================================="
  echo "Iterations per property: $PROPERTY_TEST_ITERATIONS"

  setup_test_env

  # Run all property tests
  test_property_34_error_message_completeness
  test_property_35_stash_recovery_instructions
  test_property_36_remote_error_classification
  test_property_37_operation_logging

  # Summary
  echo ""
  echo "=========================================="
  echo "Test Summary"
  echo "=========================================="
  echo "Total tests run: $TESTS_RUN"
  echo "Passed: $TESTS_PASSED"
  echo "Failed: $TESTS_FAILED"
  echo ""

  if [[ $TESTS_FAILED -eq 0 ]]; then
    echo "✓ All tests passed!"
    exit 0
  else
    echo "✗ Some tests failed"
    exit 1
  fi
}

# Run main if executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
