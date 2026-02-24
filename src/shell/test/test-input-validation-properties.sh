#!/usr/bin/env bash
#
# test-input-validation-properties.sh - Property-based tests for input validation
#
# Tests Properties:
#   - Property 30: URL Format Validation
#   - Property 31: Branch Name Validation
#   - Property 32: Path Conflict Detection
#   - Property 33: Git Repository Validation
#
# Requirements: 7.3, 7.4, 7.5, 7.6
#

# Note: Not using 'set -euo pipefail' to allow test failures without exiting

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

# Disable debug logging to speed up tests
export GITH_DEBUG=0

# Source git-helpers (only if not already sourced)
if [[ -z "${GITH_VERSION:-}" ]]; then
  source "$LIB_DIR/git-helpers.sh"
fi

# Test configuration
TEST_ITERATIONS=20
TESTS_PASSED=0
TESTS_FAILED=0
FAILED_TESTS=()

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

#------------------------------------------------------------------------------
# Test Utilities
#------------------------------------------------------------------------------

# Print test header
print_test_header() {
  local test_name="$1"
  echo ""
  echo "=========================================="
  echo "Test: $test_name"
  echo "=========================================="
}

# Print test result
print_test_result() {
  local test_name="$1"
  local passed="$2"

  if [[ "$passed" == "true" ]]; then
    echo -e "${GREEN}✓ PASSED${NC}: $test_name"
    ((TESTS_PASSED++))
  else
    echo -e "${RED}✗ FAILED${NC}: $test_name"
    ((TESTS_FAILED++))
    FAILED_TESTS+=("$test_name")
  fi
}

# Generate random valid SSH URL
generate_valid_ssh_url() {
  local hosts=("github.com" "gitlab.com" "bitbucket.org" "example.com")
  local host="${hosts[$((RANDOM % ${#hosts[@]}))]}"
  local user="user$((RANDOM % 1000))"
  local repo="repo$((RANDOM % 1000))"
  echo "git@$host:$user/$repo.git"
}

# Generate random valid HTTPS URL
generate_valid_https_url() {
  local hosts=("github.com" "gitlab.com" "bitbucket.org" "example.com")
  local host="${hosts[$((RANDOM % ${#hosts[@]}))]}"
  local user="user$((RANDOM % 1000))"
  local repo="repo$((RANDOM % 1000))"
  echo "https://$host/$user/$repo.git"
}

# Generate random valid local path URL
generate_valid_local_url() {
  echo "file:///tmp/repo$((RANDOM % 1000)).git"
}

# Generate random invalid URL
generate_invalid_url() {
  local invalid_urls=(
    "not-a-url"
    "ftp://invalid.com/repo.git"
    "javascript:alert(1)"
    "http://"
    "git@"
    ":"
    ""
    "   "
  )
  echo "${invalid_urls[$((RANDOM % ${#invalid_urls[@]}))]}"
}

# Generate random valid branch name
generate_valid_branch_name() {
  local prefixes=("feature" "bugfix" "hotfix" "dev" "main" "master")
  local prefix="${prefixes[$((RANDOM % ${#prefixes[@]}))]}"
  local suffix="$((RANDOM % 1000))"
  echo "$prefix/$suffix"
}

# Generate random invalid branch name
generate_invalid_branch_name() {
  local invalid_names=(
    "branch with spaces"
    "branch~1"
    "branch^1"
    "branch:name"
    "branch?name"
    "branch*name"
    "branch[name"
    "branch.."
    "branch@{"
    "branch//name"
    "/branch"
    "branch/"
    "branch.lock"
    "@"
  )
  echo "${invalid_names[$((RANDOM % ${#invalid_names[@]}))]}"
}

#------------------------------------------------------------------------------
# Property 30: URL Format Validation
#------------------------------------------------------------------------------

# Feature: repo-initialization-workflow, Property 30: URL Format Validation
test_property_30_url_format_validation() {
  print_test_header "Property 30: URL Format Validation"

  local passed=true
  local valid_count=0
  local invalid_count=0
  local iterations=$((TEST_ITERATIONS / 3))

  # Test valid URLs
  for ((i=1; i<=iterations; i++)); do
    # Test SSH URL
    local ssh_url
    ssh_url=$(generate_valid_ssh_url)
    if (gith_validate_url "$ssh_url") 2>/dev/null; then
      ((valid_count++))
    else
      echo "  Failed: Valid SSH URL rejected: $ssh_url"
      passed=false
    fi

    # Test HTTPS URL
    local https_url
    https_url=$(generate_valid_https_url)
    if (gith_validate_url "$https_url") 2>/dev/null; then
      ((valid_count++))
    else
      echo "  Failed: Valid HTTPS URL rejected: $https_url"
      passed=false
    fi

    # Test local path URL
    local local_url
    local_url=$(generate_valid_local_url)
    if (gith_validate_url "$local_url") 2>/dev/null; then
      ((valid_count++))
    else
      echo "  Failed: Valid local URL rejected: $local_url"
      passed=false
    fi
  done

  # Test invalid URLs
  for ((i=1; i<=iterations; i++)); do
    local invalid_url
    invalid_url=$(generate_invalid_url)
    if (gith_validate_url "$invalid_url") 2>/dev/null; then
      echo "  Failed: Invalid URL accepted: $invalid_url"
      passed=false
    else
      ((invalid_count++))
    fi
  done

  echo "  Valid URLs tested: $valid_count"
  echo "  Invalid URLs tested: $invalid_count"

  print_test_result "Property 30: URL Format Validation" "$passed"
}

#------------------------------------------------------------------------------
# Property 31: Branch Name Validation
#------------------------------------------------------------------------------

# Feature: repo-initialization-workflow, Property 31: Branch Name Validation
test_property_31_branch_name_validation() {
  print_test_header "Property 31: Branch Name Validation"

  local passed=true
  local valid_count=0
  local invalid_count=0
  local iterations=$((TEST_ITERATIONS / 2))

  # Test valid branch names
  for ((i=1; i<=iterations; i++)); do
    local branch_name
    branch_name=$(generate_valid_branch_name)
    if (gith_validate_branch_name "$branch_name") 2>/dev/null; then
      ((valid_count++))
    else
      echo "  Failed: Valid branch name rejected: $branch_name"
      passed=false
    fi
  done

  # Test invalid branch names
  for ((i=1; i<=iterations; i++)); do
    local branch_name
    branch_name=$(generate_invalid_branch_name)
    if (gith_validate_branch_name "$branch_name") 2>/dev/null; then
      echo "  Failed: Invalid branch name accepted: $branch_name"
      passed=false
    else
      ((invalid_count++))
    fi
  done

  echo "  Valid branch names tested: $valid_count"
  echo "  Invalid branch names tested: $invalid_count"

  print_test_result "Property 31: Branch Name Validation" "$passed"
}

#------------------------------------------------------------------------------
# Property 32: Path Conflict Detection
#------------------------------------------------------------------------------

# Feature: repo-initialization-workflow, Property 32: Path Conflict Detection
test_property_32_path_conflict_detection() {
  print_test_header "Property 32: Path Conflict Detection"

  local passed=true
  local test_dir
  test_dir=$(mktemp -d)

  local iterations=$((TEST_ITERATIONS / 2))
  local conflict_iterations=$((TEST_ITERATIONS / 4))

  # Test non-existent paths (should be valid)
  local valid_count=0
  for ((i=1; i<=iterations; i++)); do
    local path="$test_dir/nonexistent-$i"
    if (gith_validate_path "$path") 2>/dev/null; then
      ((valid_count++))
    else
      echo "  Failed: Non-existent path rejected: $path"
      passed=false
    fi
  done

  # Test existing paths (should be invalid)
  local conflict_count=0
  for ((i=1; i<=conflict_iterations; i++)); do
    # Create a file
    local file_path="$test_dir/existing-file-$i"
    touch "$file_path"
    if (gith_validate_path "$file_path") 2>/dev/null; then
      echo "  Failed: Existing file path accepted: $file_path"
      passed=false
    else
      ((conflict_count++))
    fi

    # Create a directory
    local dir_path="$test_dir/existing-dir-$i"
    mkdir -p "$dir_path"
    if (gith_validate_path "$dir_path") 2>/dev/null; then
      echo "  Failed: Existing directory path accepted: $dir_path"
      passed=false
    else
      ((conflict_count++))
    fi
  done

  echo "  Valid paths tested: $valid_count"
  echo "  Conflicting paths tested: $conflict_count"

  # Cleanup
  rm -rf "$test_dir"

  print_test_result "Property 32: Path Conflict Detection" "$passed"
}

#------------------------------------------------------------------------------
# Property 33: Git Repository Validation
#------------------------------------------------------------------------------

# Feature: repo-initialization-workflow, Property 33: Git Repository Validation
test_property_33_git_repository_validation() {
  print_test_header "Property 33: Git Repository Validation"

  local passed=true
  local test_base_dir
  test_base_dir=$(mktemp -d)

  local iterations=$((TEST_ITERATIONS / 2))

  # Test valid Git repositories
  local valid_count=0
  for ((i=1; i<=iterations; i++)); do
    local repo_dir="$test_base_dir/repo-$i"
    mkdir -p "$repo_dir"
    (cd "$repo_dir" && git init >/dev/null 2>&1)

    if ! gith_is_git_repo "$repo_dir"; then
      echo "  Failed: Valid Git repository rejected: $repo_dir"
      passed=false
    else
      ((valid_count++))
    fi
  done

  # Test non-Git directories
  local invalid_count=0
  for ((i=1; i<=iterations; i++)); do
    local non_repo_dir="$test_base_dir/non-repo-$i"
    mkdir -p "$non_repo_dir"

    if gith_is_git_repo "$non_repo_dir"; then
      echo "  Failed: Non-Git directory accepted: $non_repo_dir"
      passed=false
    else
      ((invalid_count++))
    fi
  done

  echo "  Valid Git repositories tested: $valid_count"
  echo "  Non-Git directories tested: $invalid_count"

  # Cleanup
  rm -rf "$test_base_dir"

  print_test_result "Property 33: Git Repository Validation" "$passed"
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "=========================================="
  echo "Input Validation Property-Based Tests"
  echo "=========================================="
  echo "Iterations per property: $TEST_ITERATIONS"
  echo ""

  # Run all property tests
  test_property_30_url_format_validation
  test_property_31_branch_name_validation
  test_property_32_path_conflict_detection
  test_property_33_git_repository_validation

  # Print summary
  echo ""
  echo "=========================================="
  echo "Test Summary"
  echo "=========================================="
  echo -e "Tests passed: ${GREEN}$TESTS_PASSED${NC}"
  echo -e "Tests failed: ${RED}$TESTS_FAILED${NC}"

  if [[ $TESTS_FAILED -gt 0 ]]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
      echo "  - $test"
    done
    exit 1
  else
    echo ""
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
  fi
}

# Run tests
main "$@"
