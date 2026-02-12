#!/usr/bin/env bash
#
# test-revision-offset.sh - Test revision offset functionality
#
# Purpose:
#   Verify that revision offset works correctly for all VCS types
#
# Usage:
#   ./test-revision-offset.sh
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Find a git repository to test with
# The workspace root should be 4 levels up from the test script
# test-revision-offset.sh is in: skills/kano-git-master-skill/scripts/test/
# Workspace root is at: ../../../.. from test script
TEST_REPO="$(cd "$SCRIPT_DIR/../../../.." && pwd)"

if [[ ! -d "$TEST_REPO/.git" ]]; then
  echo "Error: No git repository found at $TEST_REPO"
  echo "Script location: $SCRIPT_DIR"
  echo "Looking for .git in: $TEST_REPO"
  exit 1
fi

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

#------------------------------------------------------------------------------
# Helper Functions
#------------------------------------------------------------------------------

log_info() {
  echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
  echo -e "${GREEN}[✓]${NC} $*"
}

log_error() {
  echo -e "${RED}[✗]${NC} $*"
}

log_test() {
  echo -e "${YELLOW}[TEST]${NC} $*"
}

assert_equals() {
  local expected="$1"
  local actual="$2"
  local message="$3"
  
  TESTS_RUN=$((TESTS_RUN + 1))
  
  if [[ "$expected" == "$actual" ]]; then
    log_success "$message"
    TESTS_PASSED=$((TESTS_PASSED + 1))
    return 0
  else
    log_error "$message"
    log_error "  Expected: $expected"
    log_error "  Actual:   $actual"
    TESTS_FAILED=$((TESTS_FAILED + 1))
    return 1
  fi
}

#------------------------------------------------------------------------------
# Test Cases
#------------------------------------------------------------------------------

test_no_offset() {
  log_test "Test 1: No offset (default behavior)"
  
  # Get version without offset
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export)"
  
  local revision_no_offset="$PROJECT_REVISION"
  local offset_value="$PROJECT_REVISION_OFFSET"
  
  assert_equals "0" "$offset_value" "Offset should be 0 by default"
  
  log_info "Revision without offset: $revision_no_offset"
}

test_negative_offset() {
  log_test "Test 2: Negative offset (-100)"
  
  # Get version without offset first
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export)"
  local revision_no_offset="$PROJECT_REVISION"
  
  # Get version with negative offset
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export --offset -100)"
  
  local revision_with_offset="$PROJECT_REVISION"
  local offset_value="$PROJECT_REVISION_OFFSET"
  local expected_revision=$((revision_no_offset - 100))
  
  assert_equals "-100" "$offset_value" "Offset value should be -100"
  assert_equals "$expected_revision" "$revision_with_offset" "Revision should be reduced by 100"
  
  log_info "Revision without offset: $revision_no_offset"
  log_info "Revision with offset -100: $revision_with_offset"
}

test_positive_offset() {
  log_test "Test 3: Positive offset (+50)"
  
  # Get version without offset first
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export)"
  local revision_no_offset="$PROJECT_REVISION"
  
  # Get version with positive offset
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export --offset 50)"
  
  local revision_with_offset="$PROJECT_REVISION"
  local offset_value="$PROJECT_REVISION_OFFSET"
  local expected_revision=$((revision_no_offset + 50))
  
  assert_equals "50" "$offset_value" "Offset value should be 50"
  assert_equals "$expected_revision" "$revision_with_offset" "Revision should be increased by 50"
  
  log_info "Revision without offset: $revision_no_offset"
  log_info "Revision with offset +50: $revision_with_offset"
}

test_large_negative_offset() {
  log_test "Test 4: Large negative offset (-500000, P4 use case)"
  
  # Get version without offset first
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export)"
  local revision_no_offset="$PROJECT_REVISION"
  
  # Get version with large negative offset (P4 marketplace use case)
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export --offset -500000)"
  
  local revision_with_offset="$PROJECT_REVISION"
  local offset_value="$PROJECT_REVISION_OFFSET"
  local expected_revision=$((revision_no_offset - 500000))
  
  assert_equals "-500000" "$offset_value" "Offset value should be -500000"
  assert_equals "$expected_revision" "$revision_with_offset" "Revision should be reduced by 500000"
  
  log_info "Revision without offset: $revision_no_offset"
  log_info "Revision with offset -500000: $revision_with_offset"
  log_info "This simulates P4 repo with 500300 commits → marketplace version 300"
}

test_offset_in_env_format() {
  log_test "Test 5: Offset in env format output"
  
  # Get env format output with offset
  local env_output
  env_output=$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --format env --offset -100)
  
  # Check if PROJECT_REVISION_OFFSET is in output
  if echo "$env_output" | grep -q "PROJECT_REVISION_OFFSET=-100"; then
    log_success "PROJECT_REVISION_OFFSET found in env output"
    TESTS_PASSED=$((TESTS_PASSED + 1))
  else
    log_error "PROJECT_REVISION_OFFSET not found in env output"
    TESTS_FAILED=$((TESTS_FAILED + 1))
  fi
  TESTS_RUN=$((TESTS_RUN + 1))
}

test_offset_persists_across_formats() {
  log_test "Test 6: Offset consistency across export and env formats"
  
  # Get revision with export format
  eval "$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --export --offset -200)"
  local revision_export="$PROJECT_REVISION"
  
  # Get revision with env format (parse output)
  local env_output
  env_output=$(cd "$TEST_REPO" && "$SCRIPT_DIR/../core/get-version-info.sh" --format env --offset -200)
  local revision_env
  revision_env=$(echo "$env_output" | grep "^PROJECT_REVISION=" | cut -d'=' -f2)
  
  assert_equals "$revision_export" "$revision_env" "Revision should be same in export and env formats"
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  log_info "Starting revision offset tests..."
  log_info "Test repository: $TEST_REPO"
  echo ""
  
  # Run tests
  test_no_offset
  echo ""
  
  test_negative_offset
  echo ""
  
  test_positive_offset
  echo ""
  
  test_large_negative_offset
  echo ""
  
  test_offset_in_env_format
  echo ""
  
  test_offset_persists_across_formats
  echo ""
  
  # Summary
  log_info "Test Summary:"
  log_info "  Total:  $TESTS_RUN"
  log_success "  Passed: $TESTS_PASSED"
  
  if [[ $TESTS_FAILED -gt 0 ]]; then
    log_error "  Failed: $TESTS_FAILED"
    echo ""
    log_error "Some tests failed!"
    exit 1
  else
    echo ""
    log_success "All tests passed!"
    exit 0
  fi
}

# Run main
main "$@"
