#!/usr/bin/env bash
#
# test-root-repo-config.sh - Test root repo multi-remote configuration
#
# This script tests the root repo configuration functionality of kog-submodule.sh
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source helpers
source "$PROJECT_ROOT/scripts/lib/git-helpers.sh"

# Test counter
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test result tracking
declare -a FAILED_TESTS=()

#------------------------------------------------------------------------------
# Test Helpers
#------------------------------------------------------------------------------

run_test() {
  local test_name="$1"
  local test_func="$2"

  echo ""
  echo "Test: $test_name"
  echo "========================================"

  TESTS_RUN=$((TESTS_RUN + 1))

  if $test_func; then
    echo "  ✓ PASS: $test_name"
    TESTS_PASSED=$((TESTS_PASSED + 1))
  else
    echo "  ✗ FAIL: $test_name"
    TESTS_FAILED=$((TESTS_FAILED + 1))
    FAILED_TESTS+=("$test_name")
  fi
}

assert_equal() {
  local expected="$1"
  local actual="$2"
  local message="${3:-}"

  if [[ "$expected" == "$actual" ]]; then
    return 0
  else
    echo "  ✗ Assertion failed: $message"
    echo "    Expected: $expected"
    echo "    Actual:   $actual"
    return 1
  fi
}

assert_file_exists() {
  local file="$1"
  local message="${2:-File should exist: $file}"

  if [[ -f "$file" ]]; then
    return 0
  else
    echo "  ✗ Assertion failed: $message"
    return 1
  fi
}

assert_config_value() {
  local config_file="$1"
  local key="$2"
  local expected="$3"
  local message="${4:-}"

  local actual
  actual=$(git config -f "$config_file" "$key" 2>/dev/null || echo "")

  if [[ "$expected" == "$actual" ]]; then
    return 0
  else
    echo "  ✗ Assertion failed: $message"
    echo "    Key: $key"
    echo "    Expected: $expected"
    echo "    Actual:   $actual"
    return 1
  fi
}

#------------------------------------------------------------------------------
# Test Cases
#------------------------------------------------------------------------------

test_root_repo_add_single_remote() {
  local test_dir
  test_dir=$(mktemp -d)

  cd "$test_dir"
  git init
  git config user.email "test@example.com"
  git config user.name "Test User"

  # Add root repo configuration
  "$PROJECT_ROOT/scripts/submodules/kog-submodule.sh" add \
    --remote origin \
      --ssh git@github.com:user/repo.git \
      --https https://github.com/user/repo.git \
    --push-remote origin \
    --protocol auto

  # Verify .gitmodules was created
  assert_file_exists ".gitmodules" || return 1

  # Verify kog-root-remote-* fields
  assert_config_value ".gitmodules" "kog-root-remote.origin.kog-url-ssh" "git@github.com:user/repo.git" || return 1
  assert_config_value ".gitmodules" "kog-root-remote.origin.kog-url-https" "https://github.com/user/repo.git" || return 1
  assert_config_value ".gitmodules" "kog-root-config.push-remote" "origin" || return 1
  assert_config_value ".gitmodules" "kog-root-config.protocol-priority" "auto" || return 1

  # Verify remote was configured in .git/config
  local remote_url
  remote_url=$(git config remote.origin.url 2>/dev/null || echo "")

  if [[ -z "$remote_url" ]]; then
    echo "  ✗ Remote 'origin' not configured in .git/config"
    cd - >/dev/null
    rm -rf "$test_dir"
    return 1
  fi

  echo "  ✓ Remote 'origin' configured: $remote_url"

  cd - >/dev/null
  rm -rf "$test_dir"
  return 0
}

test_root_repo_add_multiple_remotes() {
  local test_dir
  test_dir=$(mktemp -d)

  cd "$test_dir"
  git init
  git config user.email "test@example.com"
  git config user.name "Test User"

  # Add root repo configuration with multiple remotes
  "$PROJECT_ROOT/scripts/submodules/kog-submodule.sh" add \
    --remote origin \
      --ssh git@github.com:user/repo.git \
      --https https://github.com/user/repo.git \
    --remote upstream \
      --ssh git@github.com:original/repo.git \
      --https https://github.com/original/repo.git \
    --push-remote origin \
    --protocol auto

  # Verify .gitmodules was created
  assert_file_exists ".gitmodules" || return 1

  # Verify origin remote fields
  assert_config_value ".gitmodules" "kog-root-remote.origin.kog-url-ssh" "git@github.com:user/repo.git" || return 1
  assert_config_value ".gitmodules" "kog-root-remote.origin.kog-url-https" "https://github.com/user/repo.git" || return 1

  # Verify upstream remote fields
  assert_config_value ".gitmodules" "kog-root-remote.upstream.kog-url-ssh" "git@github.com:original/repo.git" || return 1
  assert_config_value ".gitmodules" "kog-root-remote.upstream.kog-url-https" "https://github.com/original/repo.git" || return 1

  # Verify configuration fields
  assert_config_value ".gitmodules" "kog-root-config.push-remote" "origin" || return 1
  assert_config_value ".gitmodules" "kog-root-config.protocol-priority" "auto" || return 1

  # Verify both remotes were configured in .git/config
  local origin_url
  local upstream_url
  origin_url=$(git config remote.origin.url 2>/dev/null || echo "")
  upstream_url=$(git config remote.upstream.url 2>/dev/null || echo "")

  if [[ -z "$origin_url" ]]; then
    echo "  ✗ Remote 'origin' not configured in .git/config"
    cd - >/dev/null
    rm -rf "$test_dir"
    return 1
  fi

  if [[ -z "$upstream_url" ]]; then
    echo "  ✗ Remote 'upstream' not configured in .git/config"
    cd - >/dev/null
    rm -rf "$test_dir"
    return 1
  fi

  echo "  ✓ Remote 'origin' configured: $origin_url"
  echo "  ✓ Remote 'upstream' configured: $upstream_url"

  cd - >/dev/null
  rm -rf "$test_dir"
  return 0
}

test_root_repo_sync() {
  local test_dir
  test_dir=$(mktemp -d)

  cd "$test_dir"
  git init
  git config user.email "test@example.com"
  git config user.name "Test User"

  # Manually create .gitmodules with kog-root-remote-* fields
  cat > .gitmodules << 'EOF'
[kog-root-remote "origin"]
	kog-url-ssh = git@github.com:user/repo.git
	kog-url-https = https://github.com/user/repo.git
[kog-root-remote "upstream"]
	kog-url-ssh = git@github.com:original/repo.git
	kog-url-https = https://github.com/original/repo.git
[kog-root-config]
	push-remote = origin
	protocol-priority = https
EOF

  # Run sync
  "$PROJECT_ROOT/scripts/submodules/kog-submodule.sh" sync

  # Verify remotes were configured
  local origin_url
  local upstream_url
  origin_url=$(git config remote.origin.url 2>/dev/null || echo "")
  upstream_url=$(git config remote.upstream.url 2>/dev/null || echo "")

  if [[ -z "$origin_url" ]]; then
    echo "  ✗ Remote 'origin' not configured after sync"
    cd - >/dev/null
    rm -rf "$test_dir"
    return 1
  fi

  if [[ -z "$upstream_url" ]]; then
    echo "  ✗ Remote 'upstream' not configured after sync"
    cd - >/dev/null
    rm -rf "$test_dir"
    return 1
  fi

  # Since protocol priority is 'https', both should use HTTPS URLs
  assert_equal "https://github.com/user/repo.git" "$origin_url" "Origin should use HTTPS" || return 1
  assert_equal "https://github.com/original/repo.git" "$upstream_url" "Upstream should use HTTPS" || return 1

  echo "  ✓ Remote 'origin' synced: $origin_url"
  echo "  ✓ Remote 'upstream' synced: $upstream_url"

  cd - >/dev/null
  rm -rf "$test_dir"
  return 0
}

test_root_repo_dry_run() {
  local test_dir
  test_dir=$(mktemp -d)

  cd "$test_dir"
  git init
  git config user.email "test@example.com"
  git config user.name "Test User"

  # Add root repo configuration with dry-run
  "$PROJECT_ROOT/scripts/submodules/kog-submodule.sh" add \
    --remote origin \
      --ssh git@github.com:user/repo.git \
      --https https://github.com/user/repo.git \
    --dry-run

  # Verify .gitmodules was NOT created
  if [[ -f ".gitmodules" ]]; then
    echo "  ✗ .gitmodules should not exist in dry-run mode"
    cd - >/dev/null
    rm -rf "$test_dir"
    return 1
  fi

  echo "  ✓ Dry-run mode: no changes made"

  cd - >/dev/null
  rm -rf "$test_dir"
  return 0
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "========================================"
  echo "Root Repo Configuration Tests"
  echo "========================================"

  run_test "Root repo add single remote" test_root_repo_add_single_remote
  run_test "Root repo add multiple remotes" test_root_repo_add_multiple_remotes
  run_test "Root repo sync" test_root_repo_sync
  run_test "Root repo dry-run mode" test_root_repo_dry_run

  echo ""
  echo "========================================"
  echo "Test Summary"
  echo "========================================"
  echo "Tests run:    $TESTS_RUN"
  echo "Tests passed: $TESTS_PASSED"
  echo "Tests failed: $TESTS_FAILED"

  if [[ $TESTS_FAILED -gt 0 ]]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
      echo "  - $test"
    done
    exit 1
  else
    echo ""
    echo "All tests passed!"
    exit 0
  fi
}

main "$@"
