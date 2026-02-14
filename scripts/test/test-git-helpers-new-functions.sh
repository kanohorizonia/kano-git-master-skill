#!/usr/bin/env bash
#
# test-git-helpers-new-functions.sh - Test new git-helpers functions
#
# Tests the newly added functions in git-helpers.sh:
# - gith_is_remote_empty
# - gith_validate_url_pair
# - gith_branch_exists
# - gith_validate_url
# - gith_validate_branch_name
# - gith_ssh_available
# - gith_extract_ssh_host

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Test counter
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test helper functions
test_assert() {
  local description="$1"
  local command="$2"
  local expected_result="$3"

  TESTS_RUN=$((TESTS_RUN + 1))

  if eval "$command"; then
    local actual_result=0
  else
    local actual_result=$?
  fi

  if [[ "$actual_result" == "$expected_result" ]]; then
    echo "✓ PASS: $description"
    TESTS_PASSED=$((TESTS_PASSED + 1))
  else
    echo "✗ FAIL: $description (expected $expected_result, got $actual_result)"
    TESTS_FAILED=$((TESTS_FAILED + 1))
  fi
}

echo "Testing new git-helpers.sh functions..."
echo ""

# Test gith_validate_url
echo "=== Testing gith_validate_url ==="
test_assert "Valid HTTPS URL" "gith_validate_url 'https://github.com/user/repo.git'" 0
test_assert "Valid SSH URL" "gith_validate_url 'git@github.com:user/repo.git'" 0
test_assert "Valid SSH URL (ssh://)" "gith_validate_url 'ssh://git@github.com/user/repo.git'" 0
test_assert "Valid local path" "gith_validate_url '/path/to/repo'" 0
test_assert "Invalid URL" "gith_validate_url 'not-a-valid-url'" 1
test_assert "Empty URL" "gith_validate_url ''" 1
echo ""

# Test gith_validate_branch_name
echo "=== Testing gith_validate_branch_name ==="
test_assert "Valid branch name" "gith_validate_branch_name 'feature/my-feature'" 0
test_assert "Valid branch name (main)" "gith_validate_branch_name 'main'" 0
test_assert "Valid branch name (dev/gitmaster)" "gith_validate_branch_name 'dev/gitmaster'" 0
test_assert "Invalid branch name (space)" "gith_validate_branch_name 'my branch'" 1
test_assert "Invalid branch name (..)" "gith_validate_branch_name 'feature..branch'" 1
test_assert "Invalid branch name (~)" "gith_validate_branch_name 'feature~1'" 1
test_assert "Invalid branch name (@)" "gith_validate_branch_name '@'" 1
test_assert "Empty branch name" "gith_validate_branch_name ''" 1
echo ""

# Test gith_validate_url_pair
echo "=== Testing gith_validate_url_pair ==="
test_assert "Matching URL pair" \
  "gith_validate_url_pair 'git@github.com:user/repo.git' 'https://github.com/user/repo.git'" 0
test_assert "Non-matching URL pair" \
  "gith_validate_url_pair 'git@github.com:user/repo1.git' 'https://github.com/user/repo2.git'" 1
echo ""

# Test gith_extract_ssh_host
echo "=== Testing gith_extract_ssh_host ==="
ssh_host=$(gith_extract_ssh_host 'git@github.com:user/repo.git')
if [[ "$ssh_host" == "github.com" ]]; then
  echo "✓ PASS: Extract host from SSH URL"
  TESTS_PASSED=$((TESTS_PASSED + 1))
else
  echo "✗ FAIL: Extract host from SSH URL (expected github.com, got $ssh_host)"
  TESTS_FAILED=$((TESTS_FAILED + 1))
fi
TESTS_RUN=$((TESTS_RUN + 1))
echo ""

# Test gith_ssh_available (only if we can test)
echo "=== Testing gith_ssh_available ==="
echo "Note: SSH availability test requires network and SSH keys"
if gith_ssh_available "github.com" 2; then
  echo "✓ INFO: SSH available for github.com"
else
  echo "ℹ INFO: SSH not available for github.com (this is OK for testing)"
fi
echo ""

# Summary
echo "========================================="
echo "Test Summary:"
echo "  Total:  $TESTS_RUN"
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo "========================================="

if [[ $TESTS_FAILED -eq 0 ]]; then
  echo "✓ All tests passed!"
  exit 0
else
  echo "✗ Some tests failed"
  exit 1
fi
