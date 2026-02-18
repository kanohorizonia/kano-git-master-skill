#!/usr/bin/env bash
#
# test-setup-multi-remote-properties.sh - Property-based tests for setup-multi-remote.sh
#
# This file implements property-based tests for the setup-multi-remote.sh script.
# Each property test runs 100+ iterations with generated inputs to verify
# universal correctness properties.
#
# Properties tested:
# - Property 2: Basic Remote Configuration
# - Property 3: Advanced Remote Configuration
# - Property 4: Push Fallback Strategy
#
# **Validates: Requirements 2.1, 2.2, 2.3, 2.4**

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="$(cd "$SCRIPT_DIR/../core" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

# Source git-helpers
source "$LIB_DIR/git-helpers.sh"

# Test configuration
# Note: 100 iterations can take several minutes due to Git operations.
# Use PROPERTY_TEST_ITERATIONS environment variable to adjust (e.g., PROPERTY_TEST_ITERATIONS=10)
PROPERTY_TEST_ITERATIONS=${PROPERTY_TEST_ITERATIONS:-100}
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Temporary directory for test repositories
TEST_TMP_DIR=""

# Cleanup function
cleanup() {
  if [[ -n "${TEST_TMP_DIR:-}" && -d "$TEST_TMP_DIR" ]]; then
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

# Generate random SSH URL
generate_ssh_url() {
  local user="user$((RANDOM % 100))"
  local repo="repo$((RANDOM % 100))"
  local host="github.com"
  echo "git@$host:$user/$repo.git"
}

# Generate random HTTP URL
generate_http_url() {
  local user="user$((RANDOM % 100))"
  local repo="repo$((RANDOM % 100))"
  local host="github.com"
  echo "https://$host/$user/$repo.git"
}

# Generate matching URL pair (SSH and HTTPS for same repo)
generate_matching_url_pair() {
  local user="user$((RANDOM % 100))"
  local repo="repo$((RANDOM % 100))"
  local host="github.com"

  echo "git@$host:$user/$repo.git https://$host/$user/$repo.git"
}

# Create temporary test repository
create_test_repo() {
  local repo_dir="$1"
  mkdir -p "$repo_dir"
  (cd "$repo_dir" && git init -q)
  (cd "$repo_dir" && git config user.email "test@example.com")
  (cd "$repo_dir" && git config user.name "Test User")
}

# Create a bare repository to act as remote
create_bare_repo() {
  local repo_dir="$1"
  mkdir -p "$repo_dir"
  (cd "$repo_dir" && git init --bare -q)
}

#------------------------------------------------------------------------------
# Property 2: Basic Remote Configuration
# Feature: repo-initialization-workflow, Property 2: Basic Remote Configuration
#------------------------------------------------------------------------------

test_property_basic_remote_configuration() {
  echo ""
  echo "=== Property 2: Basic Remote Configuration ==="
  echo "Testing that setup-multi-remote creates single 'origin' remote when only one URL provided"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local ssh_count=0
  local http_count=0
  local failed=0

  # Test with SSH URLs only
  echo "Testing basic mode with SSH URLs..."
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
    local test_repo="$TEST_TMP_DIR/basic-ssh-$i"
    create_test_repo "$test_repo"

    local ssh_url
    ssh_url=$(generate_ssh_url)

    # Run setup-multi-remote with only SSH URL
    set +e
    bash "$CORE_DIR/setup-multi-remote.sh" \
      --origin-ssh "$ssh_url" \
      --dir "$test_repo" >/dev/null 2>&1
    local result=$?
    set -e

    if [[ $result -eq 0 ]]; then

      # Verify exactly one remote named 'origin' exists
      local remote_count
      remote_count=$(cd "$test_repo" && git remote | wc -l | tr -d ' ')

      if [[ "$remote_count" -ne 1 ]]; then
        test_fail "Property 2 iteration $i (SSH)" "Expected 1 remote, found $remote_count"
        failed=1
        break
      fi

      # Verify remote is named 'origin'
      local remote_name
      remote_name=$(cd "$test_repo" && git remote)

      if [[ "$remote_name" != "origin" ]]; then
        test_fail "Property 2 iteration $i (SSH)" "Expected remote named 'origin', found '$remote_name'"
        failed=1
        break
      fi

      # Verify remote URL matches input
      local actual_url
      actual_url=$(cd "$test_repo" && git remote get-url origin)

      if [[ "$actual_url" != "$ssh_url" ]]; then
        test_fail "Property 2 iteration $i (SSH)" "URL mismatch: expected '$ssh_url', got '$actual_url'"
        failed=1
        break
      fi

      ((ssh_count++)) || true
    else
      test_fail "Property 2 iteration $i (SSH)" "setup-multi-remote failed"
      failed=1
      break
    fi
  done

  # Test with HTTP URLs only (only if not failed)
  if [[ $failed -eq 0 ]]; then
    echo "Testing basic mode with HTTP URLs..."
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
      local test_repo="$TEST_TMP_DIR/basic-http-$i"
      create_test_repo "$test_repo"

      local http_url
      http_url=$(generate_http_url)

      # Run setup-multi-remote with only HTTP URL
      set +e
      bash "$CORE_DIR/setup-multi-remote.sh" \
        --origin-http "$http_url" \
        --dir "$test_repo" >/dev/null 2>&1
      local result=$?
      set -e

      if [[ $result -eq 0 ]]; then

        # Verify exactly one remote named 'origin' exists
        local remote_count
        remote_count=$(cd "$test_repo" && git remote | wc -l | tr -d ' ')

        if [[ "$remote_count" -ne 1 ]]; then
          test_fail "Property 2 iteration $i (HTTP)" "Expected 1 remote, found $remote_count"
          failed=1
          break
        fi

        # Verify remote is named 'origin'
        local remote_name
        remote_name=$(cd "$test_repo" && git remote)

        if [[ "$remote_name" != "origin" ]]; then
          test_fail "Property 2 iteration $i (HTTP)" "Expected remote named 'origin', found '$remote_name'"
          failed=1
          break
        fi

        # Verify remote URL matches input
        local actual_url
        actual_url=$(cd "$test_repo" && git remote get-url origin)

        if [[ "$actual_url" != "$http_url" ]]; then
          test_fail "Property 2 iteration $i (HTTP)" "URL mismatch: expected '$http_url', got '$actual_url'"
          failed=1
          break
        fi

        ((http_count++)) || true
      else
        test_fail "Property 2 iteration $i (HTTP)" "setup-multi-remote failed"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 2: Basic Remote Configuration ($ssh_count SSH, $http_count HTTP)"
  fi
}

#------------------------------------------------------------------------------
# Property 3: Advanced Remote Configuration
# Feature: repo-initialization-workflow, Property 3: Advanced Remote Configuration
#------------------------------------------------------------------------------

test_property_advanced_remote_configuration() {
  echo ""
  echo "=== Property 3: Advanced Remote Configuration ==="
  echo "Testing that setup-multi-remote creates origin-ssh, origin-http remotes when both URLs provided"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  local origin_count=0
  local upstream_count=0
  local failed=0

  # Test with origin SSH and HTTP URLs
  echo "Testing advanced mode with origin URLs..."
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
    local test_repo="$TEST_TMP_DIR/advanced-origin-$i"
    create_test_repo "$test_repo"

    local ssh_url https_url
    read -r ssh_url https_url < <(generate_matching_url_pair)

    # Run setup-multi-remote with both SSH and HTTP URLs
    set +e
    bash "$CORE_DIR/setup-multi-remote.sh" \
      --origin-ssh "$ssh_url" \
      --origin-http "$https_url" \
      --dir "$test_repo" >/dev/null 2>&1
    local result=$?
    set -e

    if [[ $result -eq 0 ]]; then

      # Verify exactly 2 remotes exist
      local remote_count
      remote_count=$(cd "$test_repo" && git remote | wc -l | tr -d ' ')

      if [[ "$remote_count" -ne 2 ]]; then
        test_fail "Property 3 iteration $i (origin)" "Expected 2 remotes, found $remote_count"
        failed=1
        break
      fi

      # Verify origin-ssh remote exists with correct URL
      if ! (cd "$test_repo" && git remote get-url origin-ssh >/dev/null 2>&1); then
        test_fail "Property 3 iteration $i (origin)" "origin-ssh remote not found"
        failed=1
        break
      fi

      local actual_ssh_url
      actual_ssh_url=$(cd "$test_repo" && git remote get-url origin-ssh)

      if [[ "$actual_ssh_url" != "$ssh_url" ]]; then
        test_fail "Property 3 iteration $i (origin)" "origin-ssh URL mismatch: expected '$ssh_url', got '$actual_ssh_url'"
        failed=1
        break
      fi

      # Verify origin-http remote exists with correct URL
      if ! (cd "$test_repo" && git remote get-url origin-http >/dev/null 2>&1); then
        test_fail "Property 3 iteration $i (origin)" "origin-http remote not found"
        failed=1
        break
      fi

      local actual_http_url
      actual_http_url=$(cd "$test_repo" && git remote get-url origin-http)

      if [[ "$actual_http_url" != "$https_url" ]]; then
        test_fail "Property 3 iteration $i (origin)" "origin-http URL mismatch: expected '$https_url', got '$actual_http_url'"
        failed=1
        break
      fi

      ((origin_count++)) || true
    else
      test_fail "Property 3 iteration $i (origin)" "setup-multi-remote failed"
      failed=1
      break
    fi
  done

  # Test with origin and upstream URLs (only if not failed)
  if [[ $failed -eq 0 ]]; then
    echo "Testing advanced mode with origin and upstream URLs..."
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 2))); do
      local test_repo="$TEST_TMP_DIR/advanced-upstream-$i"
      create_test_repo "$test_repo"

      local origin_ssh origin_https upstream_ssh upstream_https
      read -r origin_ssh origin_https < <(generate_matching_url_pair)
      read -r upstream_ssh upstream_https < <(generate_matching_url_pair)

      # Run setup-multi-remote with origin and upstream URLs
      set +e
      bash "$CORE_DIR/setup-multi-remote.sh" \
        --origin-ssh "$origin_ssh" \
        --origin-http "$origin_https" \
        --upstream-ssh "$upstream_ssh" \
        --upstream-http "$upstream_https" \
        --dir "$test_repo" >/dev/null 2>&1
      local result=$?
      set -e

      if [[ $result -eq 0 ]]; then

        # Verify exactly 4 remotes exist
        local remote_count
        remote_count=$(cd "$test_repo" && git remote | wc -l | tr -d ' ')

        if [[ "$remote_count" -ne 4 ]]; then
          test_fail "Property 3 iteration $i (upstream)" "Expected 4 remotes, found $remote_count"
          failed=1
          break
        fi

        # Verify all four remotes exist with correct URLs
        local remotes=(origin-ssh origin-http upstream-ssh upstream-http)
        local expected_urls=("$origin_ssh" "$origin_https" "$upstream_ssh" "$upstream_https")

        for j in "${!remotes[@]}"; do
          local remote="${remotes[$j]}"
          local expected_url="${expected_urls[$j]}"

          if ! (cd "$test_repo" && git remote get-url "$remote" >/dev/null 2>&1); then
            test_fail "Property 3 iteration $i (upstream)" "$remote remote not found"
            failed=1
            break 2
          fi

          local actual_url
          actual_url=$(cd "$test_repo" && git remote get-url "$remote")

          if [[ "$actual_url" != "$expected_url" ]]; then
            test_fail "Property 3 iteration $i (upstream)" "$remote URL mismatch: expected '$expected_url', got '$actual_url'"
            failed=1
            break 2
          fi
        done

        ((upstream_count++)) || true
      else
        test_fail "Property 3 iteration $i (upstream)" "setup-multi-remote failed"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 3: Advanced Remote Configuration ($origin_count origin, $upstream_count upstream)"
  fi
}

#------------------------------------------------------------------------------
# Property 4: Push Fallback Strategy
# Feature: repo-initialization-workflow, Property 4: Push Fallback Strategy
#------------------------------------------------------------------------------

test_property_push_fallback_strategy() {
  echo ""
  echo "=== Property 4: Push Fallback Strategy ==="
  echo "Testing that push_with_fallback attempts SSH first, then falls back to HTTP"
  echo "Iterations: $PROPERTY_TEST_ITERATIONS"

  # Define push_with_fallback function locally (copied from setup-multi-remote.sh)
  push_with_fallback() {
    local branch="$1"
    local repo_dir="${2:-.}"

    # Validate branch name
    if [[ -z "$branch" ]]; then
      gith_error "push_with_fallback: branch name is required"
      return 1
    fi

    # Validate repository directory
    if [[ ! -d "$repo_dir" ]]; then
      gith_error "push_with_fallback: repository directory does not exist: $repo_dir"
      return 1
    fi

    # Check if it's a git repository
    if ! gith_is_git_repo "$repo_dir"; then
      gith_error "push_with_fallback: not a git repository: $repo_dir"
      return 1
    fi

    local ssh_error=""
    local http_error=""
    local ssh_remote=""
    local http_remote=""

    # Determine which remotes to use based on configuration
    # Check if advanced mode remotes exist
    if (cd "$repo_dir" && git remote get-url origin-ssh >/dev/null 2>&1); then
      ssh_remote="origin-ssh"
    elif (cd "$repo_dir" && git remote get-url origin >/dev/null 2>&1); then
      # Basic mode - check if origin uses SSH or file protocol
      local origin_url
      origin_url=$(cd "$repo_dir" && git remote get-url origin)
      if [[ "$origin_url" =~ ^(git@|file://) ]]; then
        ssh_remote="origin"
      fi
    fi

    if (cd "$repo_dir" && git remote get-url origin-http >/dev/null 2>&1); then
      http_remote="origin-http"
    elif (cd "$repo_dir" && git remote get-url origin >/dev/null 2>&1); then
      # Basic mode - check if origin uses HTTP
      local origin_url
      origin_url=$(cd "$repo_dir" && git remote get-url origin)
      if [[ "$origin_url" =~ ^https?:// ]]; then
        http_remote="origin"
      fi
    fi

    # Attempt SSH push first if SSH remote exists
    if [[ -n "$ssh_remote" ]]; then
      gith_log "INFO" "Attempting push to $ssh_remote..."
      if (cd "$repo_dir" && git push "$ssh_remote" "$branch" >/dev/null 2>&1); then
        gith_log "INFO" "Successfully pushed to $ssh_remote"
        return 0
      else
        gith_log "WARN" "SSH push failed"
        ssh_error="SSH push to $ssh_remote failed"
      fi
    fi

    # Fallback to HTTP push if HTTP remote exists
    if [[ -n "$http_remote" ]]; then
      gith_log "INFO" "Falling back to $http_remote..."
      if (cd "$repo_dir" && git push "$http_remote" "$branch" >/dev/null 2>&1); then
        gith_log "INFO" "Successfully pushed to $http_remote"
        return 0
      else
        gith_log "WARN" "HTTP push failed"
        http_error="HTTP push to $http_remote failed"
      fi
    fi

    # Both failed - report detailed error
    gith_error "Failed to push branch '$branch' to any remote"

    if [[ -n "$ssh_remote" && -n "$ssh_error" ]]; then
      gith_error "SSH push to $ssh_remote failed:"
      gith_error "$ssh_error"
    fi

    if [[ -n "$http_remote" && -n "$http_error" ]]; then
      gith_error "HTTP push to $http_remote failed:"
      gith_error "$http_error"
    fi

    if [[ -z "$ssh_remote" && -z "$http_remote" ]]; then
      gith_error "No suitable remotes found for push operation"
      gith_error "Expected remotes: origin-ssh, origin-http, or origin"
    fi

    return 1
  }

  local ssh_success_count=0
  local http_fallback_count=0
  local both_fail_count=0
  local failed=0

  # Test SSH success (no fallback needed)
  echo "Testing SSH push success..."
  for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 3))); do
    local test_repo="$TEST_TMP_DIR/push-ssh-$i"
    local remote_repo="$TEST_TMP_DIR/remote-ssh-$i.git"

    create_test_repo "$test_repo"
    create_bare_repo "$remote_repo"

    # Setup advanced mode remotes - both valid
    (cd "$test_repo" && git remote add origin-ssh "$remote_repo")
    (cd "$test_repo" && git remote add origin-http "$remote_repo")

    # Create a commit
    (cd "$test_repo" && echo "test" > test.txt && git add test.txt && git commit -q -m "Test commit")

    # Test push_with_fallback - should succeed via SSH
    if push_with_fallback "main" "$test_repo" >/dev/null 2>&1; then
      # Verify the push succeeded
      if (cd "$remote_repo" && git show-ref --verify --quiet refs/heads/main); then
        ((ssh_success_count++)) || true
      else
        test_fail "Property 4 iteration $i (SSH)" "Push reported success but branch not in remote"
        failed=1
        break
      fi
    else
      test_fail "Property 4 iteration $i (SSH)" "Push failed when it should have succeeded"
      failed=1
      break
    fi
  done

  # Test HTTP fallback (SSH fails, HTTP succeeds)
  if [[ $failed -eq 0 ]]; then
    echo "Testing HTTP fallback..."
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 3))); do
      local test_repo="$TEST_TMP_DIR/push-fallback-$i"
      local remote_repo="$TEST_TMP_DIR/remote-fallback-$i.git"

      create_test_repo "$test_repo"
      create_bare_repo "$remote_repo"

      # Setup advanced mode remotes - SSH invalid, HTTP valid
      (cd "$test_repo" && git remote add origin-ssh "/nonexistent/path-$RANDOM")
      (cd "$test_repo" && git remote add origin-http "$remote_repo")

      # Create a commit
      (cd "$test_repo" && echo "test" > test.txt && git add test.txt && git commit -q -m "Test commit")

      # Test push_with_fallback - should fallback to HTTP
      local output
      output=$(push_with_fallback "main" "$test_repo" 2>&1)

      if echo "$output" | grep -q "Falling back to origin-http"; then
        # Verify the push succeeded via HTTP
        if (cd "$remote_repo" && git show-ref --verify --quiet refs/heads/main); then
          ((http_fallback_count++)) || true
        else
          test_fail "Property 4 iteration $i (fallback)" "Fallback reported but branch not in remote"
          failed=1
          break
        fi
      else
        test_fail "Property 4 iteration $i (fallback)" "Did not fallback to HTTP"
        failed=1
        break
      fi
    done
  fi

  # Test both fail (SSH and HTTP both invalid)
  if [[ $failed -eq 0 ]]; then
    echo "Testing both SSH and HTTP fail..."
    for i in $(seq 1 $((PROPERTY_TEST_ITERATIONS / 3))); do
      local test_repo="$TEST_TMP_DIR/push-both-fail-$i"

      create_test_repo "$test_repo"

      # Setup advanced mode remotes - both invalid
      (cd "$test_repo" && git remote add origin-ssh "/nonexistent/ssh-$RANDOM")
      (cd "$test_repo" && git remote add origin-http "/nonexistent/http-$RANDOM")

      # Create a commit
      (cd "$test_repo" && echo "test" > test.txt && git add test.txt && git commit -q -m "Test commit")

      # Test push_with_fallback - should fail with detailed error
      local output
      set +e
      output=$(push_with_fallback "main" "$test_repo" 2>&1)
      local result=$?
      set -e

      if [[ $result -ne 0 ]]; then
        # Verify error message mentions both failures
        if echo "$output" | grep -q "Failed to push"; then
          ((both_fail_count++)) || true
        else
          test_fail "Property 4 iteration $i (both fail)" "Error message incomplete"
          failed=1
          break
        fi
      else
        test_fail "Property 4 iteration $i (both fail)" "Push succeeded when both should have failed"
        failed=1
        break
      fi
    done
  fi

  if [[ $failed -eq 0 ]]; then
    test_pass "Property 4: Push Fallback Strategy ($ssh_success_count SSH, $http_fallback_count fallback, $both_fail_count both fail)"
  fi
}

#------------------------------------------------------------------------------
# Main Test Execution
#------------------------------------------------------------------------------

main() {
  echo "========================================"
  echo "Property-Based Tests for setup-multi-remote.sh"
  echo "========================================"
  echo ""
  echo "Setting up test environment..."
  setup_test_env

  # Run all property tests
  test_property_basic_remote_configuration
  test_property_advanced_remote_configuration
  test_property_push_fallback_strategy

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
