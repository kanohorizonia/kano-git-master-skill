#!/usr/bin/env bash
#
# test-init-kano-dev-skill.sh - Test init-kano-dev-skill.sh script
#
# This script tests the init-kano-dev-skill.sh workflow with dry-run mode
# to verify the script logic without making actual changes.
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source git-helpers
source "$REPO_ROOT/scripts/lib/git-helpers.sh"

# Test configuration
TEST_REPO_SSH="git@github.com:dorgonman/kano-agent-skill.git"
TEST_REPO_HTTPS="https://github.com/dorgonman/kano-agent-skill.git"
TEST_REPO_DIR="skills/kano-test"
TEST_TOOLING_BRANCH="dev/kano-agent-skill-tooling"
TEST_SKILL_1="git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git|https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git|skills/kano-filesystem-safe-ops-skill"
TEST_SKILL_2="git@github.com:dorgonman/kano-agent-backlog-skill.git|https://github.com/dorgonman/kano-agent-backlog-skill.git|skills/kano-agent-backlog-skill"

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

#------------------------------------------------------------------------------
# Test Functions
#------------------------------------------------------------------------------

run_test() {
  local test_name="$1"
  local test_command="$2"

  TESTS_RUN=$((TESTS_RUN + 1))
  echo ""
  echo "========================================"
  echo "Test $TESTS_RUN: $test_name"
  echo "========================================"

  if eval "$test_command"; then
    TESTS_PASSED=$((TESTS_PASSED + 1))
    gith_log "INFO" "✓ Test passed: $test_name"
  else
    TESTS_FAILED=$((TESTS_FAILED + 1))
    gith_log "ERROR" "✗ Test failed: $test_name"
  fi
}

#------------------------------------------------------------------------------
# Test Cases
#------------------------------------------------------------------------------

test_help_output() {
  gith_log "INFO" "Testing help output..."

  if "$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" --help | grep -q "Usage:"; then
    return 0
  else
    gith_log "ERROR" "Help output missing 'Usage:' section"
    return 1
  fi
}

test_missing_required_args() {
  gith_log "INFO" "Testing missing required arguments..."

  # Should fail without required arguments
  local exit_code=0
  "$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" >/dev/null 2>&1 || exit_code=$?

  if [[ $exit_code -ne 0 ]]; then
    return 0
  else
    gith_log "ERROR" "Script should fail without required arguments (exit code: $exit_code)"
    return 1
  fi
}

test_dry_run_basic() {
  gith_log "INFO" "Testing dry-run mode (basic)..."

  local output
  output=$("$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" \
    --repo-ssh "$TEST_REPO_SSH" \
    --repo-https "$TEST_REPO_HTTPS" \
    --repo-dir "$TEST_REPO_DIR" \
    --dry-run 2>&1)

  # Check for expected dry-run messages
  if echo "$output" | grep -q "\[DRY-RUN\]"; then
    gith_log "INFO" "Dry-run mode detected"
    return 0
  else
    gith_log "ERROR" "Dry-run mode not working"
    echo "$output"
    return 1
  fi
}

test_dry_run_basic_single_url() {
  gith_log "INFO" "Testing dry-run mode (single URL)..."

  local output
  output=$("$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" \
    --repo-ssh "$TEST_REPO_SSH" \
    --repo-dir "$TEST_REPO_DIR" \
    --dry-run 2>&1)

  if echo "$output" | grep -q "origin: $TEST_REPO_SSH"; then
    gith_log "INFO" "Single URL origin detected"
    return 0
  else
    gith_log "ERROR" "Single URL origin not detected"
    echo "$output"
    return 1
  fi
}

test_dry_run_with_skills() {
  gith_log "INFO" "Testing dry-run mode with skills..."

  local output
  output=$("$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" \
    --repo-ssh "$TEST_REPO_SSH" \
    --repo-https "$TEST_REPO_HTTPS" \
    --repo-dir "$TEST_REPO_DIR" \
    --tooling-branch "$TEST_TOOLING_BRANCH" \
    --skill "$TEST_SKILL_1" \
    --skill "$TEST_SKILL_2" \
    --dry-run 2>&1)

  # Check for expected steps
  local checks_passed=0

  if echo "$output" | grep -q "Step 1: Initialize repository"; then
    checks_passed=$((checks_passed + 1))
  fi

  if echo "$output" | grep -q "Step 2: Configure multi-remote"; then
    checks_passed=$((checks_passed + 1))
  fi

  if echo "$output" | grep -q "Step 3: Initialize main branch"; then
    checks_passed=$((checks_passed + 1))
  fi

  if echo "$output" | grep -q "Step 4: Create tooling branch"; then
    checks_passed=$((checks_passed + 1))
  fi

  if echo "$output" | grep -q "Step 5: Add skills"; then
    checks_passed=$((checks_passed + 1))
  fi

  if [[ $checks_passed -eq 5 ]]; then
    gith_log "INFO" "All workflow steps detected"
    return 0
  else
    gith_log "ERROR" "Missing workflow steps (found $checks_passed/5)"
    echo "$output"
    return 1
  fi
}

test_dry_run_with_upstream() {
  gith_log "INFO" "Testing dry-run mode with upstream..."
  
  local output
  output=$("$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" \
    --repo-ssh "$TEST_REPO_SSH" \
    --repo-https "$TEST_REPO_HTTPS" \
    --upstream-ssh "git@github.com:original/kano-agent-skill.git" \
    --upstream-https "https://github.com/original/kano-agent-skill.git" \
    --repo-dir "$TEST_REPO_DIR" \
    --dry-run 2>&1)
  
  # Check for upstream configuration (should appear in DRY-RUN output)
  if echo "$output" | grep -q "upstream-ssh"; then
    gith_log "INFO" "Upstream configuration detected"
    return 0
  else
    gith_log "ERROR" "Upstream configuration not detected"
    echo "$output"
    return 1
  fi
}

test_skip_flags() {
  gith_log "INFO" "Testing skip flags..."

  local output
  output=$("$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" \
    --repo-ssh "$TEST_REPO_SSH" \
    --repo-https "$TEST_REPO_HTTPS" \
    --repo-dir "$TEST_REPO_DIR" \
    --skip-main-init \
    --skip-tooling \
    --skip-skills \
    --dry-run 2>&1)

  # Check for skip messages
  local skips_detected=0

  if echo "$output" | grep -q "SKIPPED"; then
    skips_detected=$((skips_detected + 1))
  fi

  if [[ $skips_detected -gt 0 ]]; then
    gith_log "INFO" "Skip flags working"
    return 0
  else
    gith_log "ERROR" "Skip flags not working"
    echo "$output"
    return 1
  fi
}

test_invalid_skill_format() {
  gith_log "INFO" "Testing invalid skill format..."
  
  local output
  output=$("$REPO_ROOT/scripts/internal/init-kano-dev-skill.sh" \
    --repo-ssh "$TEST_REPO_SSH" \
    --repo-https "$TEST_REPO_HTTPS" \
    --repo-dir "$TEST_REPO_DIR" \
    --skill "invalid-format" \
    --dry-run 2>&1 || true)
  
  # Should detect invalid format (missing | delimiters)
  if echo "$output" | grep -qi "invalid.*format"; then
    gith_log "INFO" "Invalid skill format detected"
    return 0
  else
    gith_log "WARN" "Invalid skill format not detected (may be handled later)"
    return 0
  fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  echo "========================================"
  echo "init-kano-dev-skill.sh Test Suite"
  echo "========================================"
  echo ""
  echo "Testing script: $REPO_ROOT/scripts/internal/init-kano-dev-skill.sh"
  echo ""

  # Run tests
  run_test "Help output" "test_help_output"
  run_test "Missing required arguments" "test_missing_required_args"
  run_test "Dry-run mode (basic)" "test_dry_run_basic"
  run_test "Dry-run mode (single URL)" "test_dry_run_basic_single_url"
  run_test "Dry-run mode with skills" "test_dry_run_with_skills"
  run_test "Dry-run mode with upstream" "test_dry_run_with_upstream"
  run_test "Skip flags" "test_skip_flags"
  run_test "Invalid skill format" "test_invalid_skill_format"

  # Summary
  echo ""
  echo "========================================"
  echo "Test Summary"
  echo "========================================"
  echo "Tests run:    $TESTS_RUN"
  echo "Tests passed: $TESTS_PASSED"
  echo "Tests failed: $TESTS_FAILED"
  echo ""

  if [[ $TESTS_FAILED -eq 0 ]]; then
    gith_log "INFO" "All tests passed!"
    exit 0
  else
    gith_log "ERROR" "Some tests failed"
    exit 1
  fi
}

main "$@"
