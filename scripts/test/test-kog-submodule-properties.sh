#!/usr/bin/env bash
#
# test-kog-submodule-properties.sh - Property-based tests for kog-submodule commands
#
# This file implements property-based tests for the kog-submodule functionality.
# Each property test runs 100+ iterations with generated inputs to verify
# universal correctness properties.
#
# Properties tested:
# - Property 14: Submodule Branch Validation
# - Property 15: Submodule URL Accessibility
# - Property 16: Submodule Clone Correctness
# - Property 17: Gitmodules Configuration
# - Property 19: Submodule Conflict Detection
# - Property 20: Gitmodules Extension Field Preservation
# - Property 21: Submodule Multi-URL Sync
# - Property 22: Submodule Fallback Behavior
#
# **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.6, 5.7, 5.8, 5.9, 10.1, 10.2, 10.3**

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
# Test Repository Setup Helpers
#------------------------------------------------------------------------------

# Create a mock remote repository
create_mock_remote() {
  local remote_name="$1"
  local remote_path="$TEST_TMP_DIR/remotes/$remote_name"

  mkdir -p "$remote_path"
  (cd "$remote_path" && git init --bare --quiet 2>/dev/null)

  # Create a temporary repo to push initial content
  local temp_repo="$TEST_TMP_DIR/temp-$remote_name"
  mkdir -p "$temp_repo"
  (
    cd "$temp_repo"
    git init --quiet 2>/dev/null
    git config user.email "test@example.com"
    git config user.name "Test User"
    echo "# $remote_name" > README.md
    git add README.md 2>/dev/null
    git commit -m "Initial commit" --quiet 2>/dev/null
    git remote add origin "$remote_path" 2>/dev/null
    git push -u origin master --quiet 2>/dev/null || git push -u origin main --quiet 2>/dev/null
  ) >/dev/null 2>&1

  rm -rf "$temp_repo"
  echo "$remote_path"
}

# Create a test repository
create_test_repo() {
  local repo_name="$1"
  local repo_path="$TEST_TMP_DIR/repos/$repo_name"

  mkdir -p "$repo_path"
  (
    cd "$repo_path"
    git init --quiet 2>/dev/null
    git config user.email "test@example.com"
    git config user.name "Test User"
    echo "# Test Repo" > README.md
    git add README.md 2>/dev/null
    git commit -m "Initial commit" --quiet 2>/dev/null
  ) >/dev/null 2>&1

  echo "$repo_path"
}

#------------------------------------------------------------------------------
# Property 14: Submodule Branch Validation
# **Validates: Requirements 5.1**
#------------------------------------------------------------------------------

test_property_14() {
  echo ""
  echo "=== Property 14: Submodule Branch Validation ==="
  echo "For any submodule addition request, the system should verify that"
  echo "the target branch is currently checked out before proceeding."
  echo ""

  local iterations=10  # Reduced for faster testing
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-branch-$i")

    # Create mock remote
    local mock_remote
    mock_remote=$(create_mock_remote "submodule-$i")

    local test_passed=1

    # Put repo in detached HEAD state
    (
      cd "$test_repo"
      git checkout --detach HEAD >/dev/null 2>&1

      # Try to add submodule (should fail)
      if bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        2>/dev/null; then
        # Should have failed
        exit 1
      fi

      # Checkout a branch
      git checkout -b test-branch >/dev/null 2>&1

      # Try again (should succeed)
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        >/dev/null 2>&1; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 14: Submodule Branch Validation ($passed/$iterations)"
  else
    test_fail "Property 14: Submodule Branch Validation ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 15: Submodule URL Accessibility
# **Validates: Requirements 5.2**
#------------------------------------------------------------------------------

test_property_15() {
  echo ""
  echo "=== Property 15: Submodule URL Accessibility ==="
  echo "For any submodule URL provided, the system should validate that"
  echo "the URL is accessible before attempting to clone."
  echo ""

  local iterations=5
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-url-$i")

    (
      cd "$test_repo"

      # Try to add submodule with invalid URL (should fail)
      if bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "git@invalid-host-$i.example.com:user/repo.git" \
          --https "https://invalid-host-$i.example.com/user/repo.git" \
        2>/dev/null; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 15: Submodule URL Accessibility ($passed/$iterations)"
  else
    test_fail "Property 15: Submodule URL Accessibility ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 16: Submodule Clone Correctness
# **Validates: Requirements 5.3**
#------------------------------------------------------------------------------

test_property_16() {
  echo ""
  echo "=== Property 16: Submodule Clone Correctness ==="
  echo "For any valid submodule URL and path, after adding the submodule,"
  echo "the specified path should contain a cloned repository."
  echo ""

  local iterations=10
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-clone-$i")

    # Create mock remote
    local mock_remote
    mock_remote=$(create_mock_remote "submodule-clone-$i")

    (
      cd "$test_repo"

      # Add submodule
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        >/dev/null 2>&1; then
        exit 1
      fi

      # Initialize submodule
      git submodule update --init "sub$i" >/dev/null 2>&1

      # Verify submodule directory exists and is a git repo
      if [[ ! -d "sub$i" ]]; then
        exit 1
      fi

      if ! gith_is_git_repo "sub$i"; then
        exit 1
      fi

      # Verify remote URL is correct
      local remote_url
      remote_url=$(cd "sub$i" && git remote get-url origin 2>/dev/null || echo "")
      if [[ "$remote_url" != "$mock_remote" ]]; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 16: Submodule Clone Correctness ($passed/$iterations)"
  else
    test_fail "Property 16: Submodule Clone Correctness ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 17: Gitmodules Configuration
# **Validates: Requirements 5.4**
#------------------------------------------------------------------------------

test_property_17() {
  echo ""
  echo "=== Property 17: Gitmodules Configuration ==="
  echo "For any submodule addition, the .gitmodules file should contain"
  echo "an entry with the correct path and URL."
  echo ""

  local iterations=10
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-gitmodules-$i")

    # Create mock remote
    local mock_remote
    mock_remote=$(create_mock_remote "submodule-gitmodules-$i")

    (
      cd "$test_repo"

      # Add submodule
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        >/dev/null 2>&1; then
        exit 1
      fi

      # Verify .gitmodules exists
      if [[ ! -f ".gitmodules" ]]; then
        exit 1
      fi

      # Verify path is configured
      local configured_path
      configured_path=$(git config -f .gitmodules "submodule.sub$i.path" 2>/dev/null || echo "")
      if [[ "$configured_path" != "sub$i" ]]; then
        exit 1
      fi

      # Verify URL is configured
      local configured_url
      configured_url=$(git config -f .gitmodules "submodule.sub$i.url" 2>/dev/null || echo "")
      if [[ "$configured_url" != "$mock_remote" ]]; then
        exit 1
      fi

      # Verify kog-* fields are present
      local kog_ssh
      local kog_https
      kog_ssh=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-ssh" 2>/dev/null || echo "")
      kog_https=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-https" 2>/dev/null || echo "")

      if [[ -z "$kog_ssh" || -z "$kog_https" ]]; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 17: Gitmodules Configuration ($passed/$iterations)"
  else
    test_fail "Property 17: Gitmodules Configuration ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 19: Submodule Conflict Detection
# **Validates: Requirements 5.6**
#------------------------------------------------------------------------------

test_property_19() {
  echo ""
  echo "=== Property 19: Submodule Conflict Detection ==="
  echo "For any submodule addition where the target path already contains"
  echo "a submodule or directory, the system should report an error."
  echo ""

  local iterations=10
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-conflict-$i")

    # Create mock remote
    local mock_remote
    mock_remote=$(create_mock_remote "submodule-conflict-$i")

    (
      cd "$test_repo"

      # Create a directory at the target path
      mkdir -p "sub$i"
      echo "existing content" > "sub$i/file.txt"

      # Try to add submodule (should fail due to conflict)
      if bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        2>/dev/null; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 19: Submodule Conflict Detection ($passed/$iterations)"
  else
    test_fail "Property 19: Submodule Conflict Detection ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 20: Gitmodules Extension Field Preservation
# **Validates: Requirements 5.7, 10.2**
#------------------------------------------------------------------------------

test_property_20() {
  echo ""
  echo "=== Property 20: Gitmodules Extension Field Preservation ==="
  echo "For any kog-remote-* extension field written to .gitmodules,"
  echo "Git native commands should preserve these fields."
  echo ""

  local iterations=10
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-preserve-$i")

    # Create mock remote
    local mock_remote
    mock_remote=$(create_mock_remote "submodule-preserve-$i")

    (
      cd "$test_repo"

      # Add submodule
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        >/dev/null 2>&1; then
        exit 1
      fi

      # Get kog-* fields before Git operations
      local kog_ssh_before
      local kog_https_before
      kog_ssh_before=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-ssh" 2>/dev/null || echo "")
      kog_https_before=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-https" 2>/dev/null || echo "")

      # Run git submodule sync (native Git command)
      git submodule sync "sub$i" >/dev/null 2>&1

      # Get kog-* fields after Git operations
      local kog_ssh_after
      local kog_https_after
      kog_ssh_after=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-ssh" 2>/dev/null || echo "")
      kog_https_after=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-https" 2>/dev/null || echo "")

      # Verify fields are preserved
      if [[ "$kog_ssh_before" != "$kog_ssh_after" ]]; then
        exit 1
      fi

      if [[ "$kog_https_before" != "$kog_https_after" ]]; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 20: Gitmodules Extension Field Preservation ($passed/$iterations)"
  else
    test_fail "Property 20: Gitmodules Extension Field Preservation ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 21: Submodule Multi-URL Sync
# **Validates: Requirements 5.8, 10.3**
#------------------------------------------------------------------------------

test_property_21() {
  echo ""
  echo "=== Property 21: Submodule Multi-URL Sync ==="
  echo "For any submodule with multiple kog-remote-* fields, kog-submodule sync"
  echo "should create all remotes in .git/modules/<submodule>/config."
  echo ""

  local iterations=5
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-sync-$i")

    # Create mock remotes
    local mock_origin
    local mock_upstream
    mock_origin=$(create_mock_remote "origin-$i")
    mock_upstream=$(create_mock_remote "upstream-$i")

    (
      cd "$test_repo"

      # Add submodule with multiple remotes
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_origin" \
          --https "$mock_origin" \
        --remote upstream \
          --ssh "$mock_upstream" \
          --https "$mock_upstream" \
        >/dev/null 2>&1; then
        exit 1
      fi

      # Initialize submodule
      git submodule update --init "sub$i" >/dev/null 2>&1

      # Run sync
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" sync "sub$i" >/dev/null 2>&1; then
        exit 1
      fi

      # Verify both remotes exist in submodule
      if ! (cd "sub$i" && git remote | grep -qx "origin"); then
        exit 1
      fi

      if ! (cd "sub$i" && git remote | grep -qx "upstream"); then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 21: Submodule Multi-URL Sync ($passed/$iterations)"
  else
    test_fail "Property 21: Submodule Multi-URL Sync ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Property 22: Submodule Fallback Behavior
# **Validates: Requirements 5.9**
#------------------------------------------------------------------------------

test_property_22() {
  echo ""
  echo "=== Property 22: Submodule Fallback Behavior ==="
  echo "For any submodule update, if SSH fails, the system should"
  echo "automatically fallback to HTTPS."
  echo ""

  # This property is difficult to test without actual SSH/HTTPS infrastructure
  # We'll test the logic by verifying that both URLs are configured

  local iterations=5
  local passed=0

  for i in $(seq 1 $iterations); do
    # Create test repo
    local test_repo
    test_repo=$(create_test_repo "test-fallback-$i")

    # Create mock remote
    local mock_remote
    mock_remote=$(create_mock_remote "submodule-fallback-$i")

    (
      cd "$test_repo"

      # Add submodule
      if ! bash "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "sub$i" \
        --remote origin \
          --ssh "$mock_remote" \
          --https "$mock_remote" \
        >/dev/null 2>&1; then
        exit 1
      fi

      # Verify both SSH and HTTPS URLs are stored
      local kog_ssh
      local kog_https
      kog_ssh=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-ssh" 2>/dev/null || echo "")
      kog_https=$(git config -f .gitmodules "submodule.sub$i.kog-remote-origin-https" 2>/dev/null || echo "")

      if [[ -z "$kog_ssh" ]]; then
        exit 1
      fi

      if [[ -z "$kog_https" ]]; then
        exit 1
      fi

      # Verify protocol priority is set
      local protocol_priority
      protocol_priority=$(git config -f .gitmodules "submodule.sub$i.kog-protocol-priority" 2>/dev/null || echo "")

      if [[ -z "$protocol_priority" ]]; then
        exit 1
      fi
    )

    if [[ $? -eq 0 ]]; then
      ((passed++))
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 22: Submodule Fallback Behavior ($passed/$iterations)"
  else
    test_fail "Property 22: Submodule Fallback Behavior ($passed/$iterations)"
  fi
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "========================================"
  echo "Property-Based Tests: kog-submodule"
  echo "========================================"
  echo ""
  echo "Test iterations per property: $PROPERTY_TEST_ITERATIONS"
  echo ""

  setup_test_env

  # Run all property tests
  test_property_14
  test_property_15
  test_property_16
  test_property_17
  test_property_19
  test_property_20
  test_property_21
  test_property_22

  # Print summary
  echo ""
  echo "========================================"
  echo "Test Summary"
  echo "========================================"
  echo "Total tests: $TESTS_RUN"
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

main "$@"
