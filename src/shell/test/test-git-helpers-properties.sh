#!/usr/bin/env bash
#
# test-git-helpers-properties.sh - Property-based tests for git-helpers.sh
#
# This file implements property-based tests for the new git-helpers functions.
# Each property test runs 100+ iterations with generated inputs to verify
# universal correctness properties.
#
# Properties tested:
# - Property 1: Remote Status Detection
# - Property 5: URL Pair Validation
# - Property 27: URL Format Validation
# - Property 28: Branch Name Validation
# - Property 30: Git Repository Validation
#
# **Validates: Requirements 1.1, 1.2, 2.5, 7.3, 7.4, 7.6**

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
# Random Data Generators
#------------------------------------------------------------------------------

# Generate random valid Git URL
generate_valid_url() {
  local url_type=$((RANDOM % 5))
  local user="user$((RANDOM % 100))"
  local repo="repo$((RANDOM % 100))"
  local host="example$((RANDOM % 10)).com"

  case $url_type in
    0) echo "https://$host/$user/$repo.git" ;;
    1) echo "http://$host/$user/$repo.git" ;;
    2) echo "git@$host:$user/$repo.git" ;;
    3) echo "ssh://git@$host/$user/$repo.git" ;;
    4) echo "/tmp/test-repo-$RANDOM" ;;
  esac
}

# Generate random invalid Git URL
generate_invalid_url() {
  local invalid_type=$((RANDOM % 5))

  case $invalid_type in
    0) echo "not-a-url" ;;
    1) echo "ftp://invalid.com/repo.git" ;;
    2) echo "just some text" ;;
    3) echo "http://" ;;
    4) echo "" ;;
  esac
}

# Generate random valid branch name
generate_valid_branch_name() {
  local name_type=$((RANDOM % 5))
  local num=$((RANDOM % 1000))

  case $name_type in
    0) echo "feature/branch-$num" ;;
    1) echo "main" ;;
    2) echo "dev/gitmaster-$num" ;;
    3) echo "bugfix-$num" ;;
    4) echo "release/v1.$num.0" ;;
  esac
}

# Generate random invalid branch name
generate_invalid_branch_name() {
  local invalid_type=$((RANDOM % 10))

  case $invalid_type in
    0) echo "branch with spaces" ;;
    1) echo "branch..name" ;;
    2) echo "branch~1" ;;
    3) echo "branch^1" ;;
    4) echo "branch:name" ;;
    5) echo "branch?name" ;;
    6) echo "branch*name" ;;
    7) echo "branch[name]" ;;
    8) echo "@" ;;
    9) echo "/leading-slash" ;;
  esac
}

# Generate matching URL pair (SSH and HTTPS for same repo)
generate_matching_url_pair() {
  local user="user$((RANDOM % 100))"
  local repo="repo$((RANDOM % 100))"
  local host="github.com"

  echo "git@$host:$user/$repo.git"
  echo "https://$host/$user/$repo.git"
}

# Generate non-matching URL pair
generate_nonmatching_url_pair() {
  local user1="user$((RANDOM % 100))"
  local user2="user$((RANDOM % 100 + 100))"
  local repo1="repo$((RANDOM % 100))"
  local repo2="repo$((RANDOM % 100 + 100))"
  local host="github.com"

  echo "git@$host:$user1/$repo1.git"
  echo "https://$host/$user2/$repo2.git"
}

#------------------------------------------------------------------------------
# Property 27: URL Format Validation
# Feature: repo-initialization-workflow, Property 27: URL Format Validation
#------------------------------------------------------------------------------

test_property_url_format_validation() {
  echo ""
  echo "=== Property 27: URL Format Validation ==="
  echo "Testing that gith_validate_url correctly classifies valid and invalid URLs"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local valid_count=0
  local invalid_count=0
  local failed=0

  # Test valid URLs
  echo "Testing valid URLs..."
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
    local url
    url=$(generate_valid_url)

    local result
    set +e
    gith_validate_url "$url" 2>/dev/null
    result=$?
    set -e

    if [[ $result -eq 0 ]]; then
      ((valid_count++)) || true
    else
      test_fail "Property 27 iteration $i" "Valid URL rejected: $url"
      failed=1
      break
    fi
  done

  # Test invalid URLs (only if not failed)
  if [[ $failed -eq 0 ]]; then
    echo "Testing invalid URLs..."
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
      local url
      url=$(generate_invalid_url)

      local result
      set +e
      gith_validate_url "$url" 2>/dev/null
      result=$?
      set -e

      if [[ $result -ne 0 ]]; then
        ((invalid_count++)) || true
      else
        test_fail "Property 27 iteration $i" "Invalid URL accepted: $url"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 27: URL Format Validation ($valid_count valid, $invalid_count invalid)"
  fi
}

#------------------------------------------------------------------------------
# Property 28: Branch Name Validation
# Feature: repo-initialization-workflow, Property 28: Branch Name Validation
#------------------------------------------------------------------------------

test_property_branch_name_validation() {
  echo ""
  echo "=== Property 28: Branch Name Validation ==="
  echo "Testing that gith_validate_branch_name correctly validates branch names"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local valid_count=0
  local invalid_count=0
  local failed=0

  # Test valid branch names
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
    local branch_name
    branch_name=$(generate_valid_branch_name)

    set +e
      local result
    gith_validate_branch_name "$branch_name" 2>/dev/null
      result=$?
    set -e

    if [[ $result -eq 0 ]]; then
      ((valid_count++)) || true
    else
      test_fail "Property 28 iteration $i" "Valid branch name rejected: $branch_name"
      failed=1
      break
    fi
  done

  # Test invalid branch names (only if not failed)
  if [[ $failed -eq 0 ]]; then
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
      local branch_name
      branch_name=$(generate_invalid_branch_name)

      set +e
      local result
      gith_validate_branch_name "$branch_name" 2>/dev/null
      result=$?
      set -e

      if [[ $result -ne 0 ]]; then
        ((invalid_count++)) || true
      else
        test_fail "Property 28 iteration $i" "Invalid branch name accepted: $branch_name"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 28: Branch Name Validation ($valid_count valid, $invalid_count invalid)"
  fi
}

#------------------------------------------------------------------------------
# Property 5: URL Pair Validation
# Feature: repo-initialization-workflow, Property 5: URL Pair Validation
#------------------------------------------------------------------------------

test_property_url_pair_validation() {
  echo ""
  echo "=== Property 5: URL Pair Validation ==="
  echo "Testing that gith_validate_url_pair correctly identifies matching/non-matching pairs"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local matching_count=0
  local nonmatching_count=0
  local failed=0

  # Test matching URL pairs
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
    local ssh_url https_url
    read -r ssh_url https_url < <(generate_matching_url_pair)

    set +e
      local result
    gith_validate_url_pair "$ssh_url" "$https_url" 2>/dev/null
      result=$?
    set -e

    if [[ $result -eq 0 ]]; then
      ((matching_count++)) || true
    else
      test_fail "Property 5 iteration $i" "Matching pair rejected: $ssh_url <-> $https_url"
      failed=1
      break
    fi
  done

  # Test non-matching URL pairs (only if not failed)
  if [[ $failed -eq 0 ]]; then
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
      local ssh_url https_url
      read -r ssh_url https_url < <(generate_nonmatching_url_pair)

      set +e
      local result
      gith_validate_url_pair "$ssh_url" "$https_url" 2>/dev/null
      result=$?
      set -e

      if [[ $result -ne 0 ]]; then
        ((nonmatching_count++)) || true
      else
        test_fail "Property 5 iteration $i" "Non-matching pair accepted: $ssh_url <-> $https_url"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 5: URL Pair Validation ($matching_count matching, $nonmatching_count non-matching)"
  fi
}

#------------------------------------------------------------------------------
# Property 30: Git Repository Validation
# Feature: repo-initialization-workflow, Property 30: Git Repository Validation
#------------------------------------------------------------------------------

test_property_git_repository_validation() {
  echo ""
  echo "=== Property 30: Git Repository Validation ==="
  echo "Testing that gith_is_git_repo correctly identifies Git repositories"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local git_repo_count=0
  local non_git_count=0
  local failed=0

  # Test with actual Git repositories
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
    local test_repo="$TEST_TMP_DIR/git-repo-$i"
    mkdir -p "$test_repo"
    (cd "$test_repo" && git init -q)

    set +e
      local result
    gith_is_git_repo "$test_repo" 2>/dev/null
      result=$?
    set -e

    if [[ $result -eq 0 ]]; then
      ((git_repo_count++)) || true
    else
      test_fail "Property 30 iteration $i" "Git repo not detected: $test_repo"
      failed=1
      break
    fi
  done

  # Test with non-Git directories (only if not failed)
  if [[ $failed -eq 0 ]]; then
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
      local test_dir="$TEST_TMP_DIR/non-git-$i"
      mkdir -p "$test_dir"

      set +e
      local result
      gith_is_git_repo "$test_dir" 2>/dev/null
      result=$?
      set -e

      if [[ $result -ne 0 ]]; then
        ((non_git_count++)) || true
      else
        test_fail "Property 30 iteration $i" "Non-Git directory incorrectly identified as Git repo: $test_dir"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 30: Git Repository Validation ($git_repo_count repos, $non_git_count non-repos)"
  fi
}

#------------------------------------------------------------------------------
# Property 1: Remote Status Detection
# Feature: repo-initialization-workflow, Property 1: Remote Status Detection
#------------------------------------------------------------------------------

test_property_remote_status_detection() {
  echo ""
  echo "=== Property 1: Remote Status Detection ==="
  echo "Testing that gith_is_remote_empty correctly classifies remote repositories"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local empty_count=0
  local non_empty_count=0
  local inaccessible_count=0
  local failed=0

  # Test with empty remote repositories
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 3))); do
    local remote_repo="$TEST_TMP_DIR/remote-empty-$i.git"
    git init --bare -q "$remote_repo"

    set +e
      local result
    gith_is_remote_empty "$remote_repo" 2>/dev/null
      result=$?
    set -e

    if [[ $result -eq 0 ]]; then
      ((empty_count++)) || true
    elif [[ $result -eq 2 ]]; then
      test_fail "Property 1 iteration $i" "Empty remote classified as inaccessible: $remote_repo"
      failed=1
      break
    else
      test_fail "Property 1 iteration $i" "Empty remote classified as non-empty: $remote_repo"
      failed=1
      break
    fi
  done

  # Test with non-empty remote repositories (only if not failed)
  if [[ $failed -eq 0 ]]; then
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 3))); do
      local remote_repo="$TEST_TMP_DIR/remote-nonempty-$i.git"
      local work_repo="$TEST_TMP_DIR/work-$i"

      # Create bare repo
      git init --bare -q "$remote_repo"

      # Create working repo and push to bare repo
      git init -q "$work_repo"
      (cd "$work_repo" && \
        git config user.email "test@example.com" && \
        git config user.name "Test User" && \
        echo "test" > README.md && \
        git add README.md && \
        git commit -q -m "Initial commit" && \
        git remote add origin "$remote_repo" && \
        (git push -q origin main 2>/dev/null || git push -q origin master 2>/dev/null)) 2>/dev/null || true

      set +e
      local result
      gith_is_remote_empty "$remote_repo" 2>/dev/null
      result=$?
      set -e

      if [[ $result -eq 1 ]]; then
        ((non_empty_count++)) || true
      elif [[ $result -eq 2 ]]; then
        test_fail "Property 1 iteration $i" "Non-empty remote classified as inaccessible: $remote_repo"
        failed=1
        break
      else
        test_fail "Property 1 iteration $i" "Non-empty remote classified as empty: $remote_repo"
        failed=1
        break
      fi
    done
  fi

  # Test with inaccessible remotes (only if not failed)
  if [[ $failed -eq 0 ]]; then
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 3))); do
      local fake_remote="https://nonexistent-host-$RANDOM.invalid/repo.git"

      set +e
      local result
      gith_is_remote_empty "$fake_remote" 2>/dev/null
      result=$?
      set -e

      if [[ $result -eq 2 ]]; then
        ((inaccessible_count++)) || true
      elif [[ $result -eq 0 ]]; then
        test_fail "Property 1 iteration $i" "Inaccessible remote classified as empty: $fake_remote"
        failed=1
        break
      else
        test_fail "Property 1 iteration $i" "Inaccessible remote classified as non-empty: $fake_remote"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 1: Remote Status Detection ($empty_count empty, $non_empty_count non-empty, $inaccessible_count inaccessible)"
  fi
}

#------------------------------------------------------------------------------
# Main Test Execution
#------------------------------------------------------------------------------

main() {
  echo "========================================"
  echo "Property-Based Tests for git-helpers.sh"
  echo "========================================"
  echo ""
  echo "Setting up test environment..."
  setup_test_env

  # Run all property tests
  test_property_url_format_validation
  test_property_branch_name_validation
  test_property_url_pair_validation
  test_property_git_repository_validation
  test_property_remote_status_detection

  # Summary
  echo ""
  echo "========================================="
  echo "Test Summary:"
  echo "  Total:  $TESTS_RUN"
  echo "  Passed: $TESTS_PASSED"
  echo "  Failed: $TESTS_FAILED"
  echo "========================================="

  if [[ $TESTS_FAILED -eq 0 ]]; then
    echo "✓ All property tests passed!"
    exit 0
  else
    echo "✗ Some property tests failed"
    exit 1
  fi
}

main "$@"
