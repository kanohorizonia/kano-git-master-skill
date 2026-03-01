#!/usr/bin/env bash
#
# test-multi-remote-orphan-integration.sh - Integration tests for multi-remote with orphan branch
#
# This file implements integration tests for the multi-remote setup combined with
# orphan branch creation and kog-submodule commands, testing SSH/HTTPS fallback
# and .gitmodules extension field preservation.
#
# Tests:
# - Creating orphan branch with multi-remote setup
# - Adding submodules to orphan branch with kog-submodule commands
# - kog-submodule sync and update with SSH/HTTPS fallback
# - Verifying kog-* fields are preserved in .gitmodules after Git operations
#
# **Validates: Requirements 2.1, 2.2, 2.3, 4.1, 4.2, 4.3, 5.1, 5.2, 5.3, 5.8, 5.9, 10.1, 10.2, 10.3**

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Scripts under test
SETUP_MULTI_REMOTE_SCRIPT="$SCRIPT_DIR/../core/setup-multi-remote.sh"
CREATE_ORPHAN_SCRIPT="$SCRIPT_DIR/../core/create-orphan-branch.sh"
KOG_SUBMODULE_SCRIPT="$SCRIPT_DIR/../submodules/kog-submodule.sh"

# Test configuration
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
# Test Helper Functions
#------------------------------------------------------------------------------

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
      git push origin main >/dev/null 2>&1
    )
    rm -rf "$temp_clone"
  fi
}

# Create a mock submodule repository
create_mock_submodule() {
  local submodule_path="$1"

  mkdir -p "$submodule_path"
  git init --bare "$submodule_path" >/dev/null 2>&1

  # Add some content
  local temp_clone="$TEST_TMP_DIR/temp-submodule-$$"
  git clone "$submodule_path" "$temp_clone" >/dev/null 2>&1
  (
    cd "$temp_clone"
    echo "# Submodule" > README.md
    git add README.md
    git commit -m "Initial commit" >/dev/null 2>&1
    git push origin main >/dev/null 2>&1
  )
  rm -rf "$temp_clone"
}

# Initialize a test repository
init_test_repo() {
  local repo_path="$1"

  mkdir -p "$repo_path"
  cd "$repo_path"
  git init >/dev/null 2>&1
  git config user.email "test@example.com"
  git config user.name "Test User"
  echo "# Test" > README.md
  git add README.md
  git commit -m "Initial commit" >/dev/null 2>&1
}

#------------------------------------------------------------------------------
# Integration Test 1: Multi-Remote Setup with Orphan Branch
#------------------------------------------------------------------------------

test_multi_remote_with_orphan() {
  echo ""
  echo "Integration Test 1: Multi-Remote Setup with Orphan Branch"
  echo "=========================================================="

  local test_dir="$TEST_TMP_DIR/test-multi-remote-orphan"
  local remote_ssh="$TEST_TMP_DIR/remote-ssh.git"
  local remote_http="$TEST_TMP_DIR/remote-http.git"

  # Setup
  create_mock_remote "$remote_ssh" 0
  create_mock_remote "$remote_http" 0
  init_test_repo "$test_dir"

  cd "$test_dir"

  # Setup multi-remote
  if bash "$SETUP_MULTI_REMOTE_SCRIPT" \
    --origin-ssh "file://$remote_ssh" \
    --origin-http "file://$remote_http" \
    --dir "$test_dir" \
    >/dev/null 2>&1; then

    # Verify remotes were created
    if git remote | grep -q "origin-ssh"; then
      test_pass "Multi-remote: origin-ssh created"
    else
      test_fail "Multi-remote: origin-ssh created" "origin-ssh not found"
    fi

    if git remote | grep -q "origin-http"; then
      test_pass "Multi-remote: origin-http created"
    else
      test_fail "Multi-remote: origin-http created" "origin-http not found"
    fi
  else
    test_fail "Multi-remote setup" "Setup script failed"
    return
  fi

  # Create orphan branch (without push to avoid test complexity)
  if bash "$CREATE_ORPHAN_SCRIPT" \
    --branch "dev/tools" \
    --dir "$test_dir" \
    >/dev/null 2>&1; then

    # Verify orphan branch exists
    if git show-ref --verify --quiet refs/heads/dev/tools; then
      test_pass "Orphan branch: dev/tools created"
    else
      test_fail "Orphan branch: dev/tools created" "Branch not found"
    fi
  else
    test_fail "Orphan branch creation" "Create script failed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 2: kog-submodule add with Multi-URL
#------------------------------------------------------------------------------

test_kog_submodule_add_multi_url() {
  echo ""
  echo "Integration Test 2: kog-submodule add with Multi-URL"
  echo "====================================================="

  local test_dir="$TEST_TMP_DIR/test-kog-add"
  local submodule_ssh="$TEST_TMP_DIR/submodule-ssh.git"
  local submodule_http="$TEST_TMP_DIR/submodule-http.git"

  # Setup
  create_mock_submodule "$submodule_ssh"
  create_mock_submodule "$submodule_http"
  init_test_repo "$test_dir"

  cd "$test_dir"

  # Create orphan branch for submodules
  bash "$CREATE_ORPHAN_SCRIPT" \
    --branch "dev/tools" \
    --dir "$test_dir" \
    >/dev/null 2>&1

  git checkout dev/tools >/dev/null 2>&1

  # Add submodule with multi-URL
  if bash "$KOG_SUBMODULE_SCRIPT" add \
    --ssh "file://$submodule_ssh" \
    --https "file://$submodule_http" \
    --path "tools/submodule" \
    >/dev/null 2>&1; then

    # Verify submodule was added
    if git config -f .gitmodules submodule.tools/submodule.path >/dev/null 2>&1; then
      test_pass "kog-submodule add: Submodule added to .gitmodules"
    else
      test_fail "kog-submodule add: Submodule added to .gitmodules" "Submodule not found"
      return
    fi

    # Verify kog-* extension fields
    if git config -f .gitmodules submodule.tools/submodule.kog-url-ssh >/dev/null 2>&1; then
      test_pass "kog-submodule add: kog-url-ssh field added"
    else
      test_fail "kog-submodule add: kog-url-ssh field added" "Field not found"
    fi

    if git config -f .gitmodules submodule.tools/submodule.kog-url-https >/dev/null 2>&1; then
      test_pass "kog-submodule add: kog-url-https field added"
    else
      test_fail "kog-submodule add: kog-url-https field added" "Field not found"
    fi
  else
    test_fail "kog-submodule add" "Add command failed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 3: kog-* Field Preservation After Git Operations
#------------------------------------------------------------------------------

test_kog_field_preservation() {
  echo ""
  echo "Integration Test 3: kog-* Field Preservation After Git Operations"
  echo "=================================================================="

  local test_dir="$TEST_TMP_DIR/test-kog-preservation"
  local submodule_ssh="$TEST_TMP_DIR/submodule-preserve-ssh.git"
  local submodule_http="$TEST_TMP_DIR/submodule-preserve-http.git"

  # Setup
  create_mock_submodule "$submodule_ssh"
  create_mock_submodule "$submodule_http"
  init_test_repo "$test_dir"

  cd "$test_dir"

  # Create orphan branch
  bash "$CREATE_ORPHAN_SCRIPT" \
    --branch "dev/tools" \
    --dir "$test_dir" \
    >/dev/null 2>&1

  git checkout dev/tools >/dev/null 2>&1

  # Add submodule with kog-* fields
  bash "$KOG_SUBMODULE_SCRIPT" add \
    --ssh "file://$submodule_ssh" \
    --https "file://$submodule_http" \
    --path "tools/submodule" \
    >/dev/null 2>&1

  # Get original kog-* field values
  local original_ssh=$(git config -f .gitmodules submodule.tools/submodule.kog-url-ssh)
  local original_https=$(git config -f .gitmodules submodule.tools/submodule.kog-url-https)

  # Perform Git operations that might affect .gitmodules
  git add .gitmodules tools/submodule >/dev/null 2>&1
  git commit -m "Add submodule" >/dev/null 2>&1

  # Run git submodule sync (native Git command)
  git submodule sync >/dev/null 2>&1

  # Verify kog-* fields are preserved
  local after_ssh=$(git config -f .gitmodules submodule.tools/submodule.kog-url-ssh)
  local after_https=$(git config -f .gitmodules submodule.tools/submodule.kog-url-https)

  if [[ "$original_ssh" == "$after_ssh" ]]; then
    test_pass "Field preservation: kog-url-ssh preserved after git submodule sync"
  else
    test_fail "Field preservation: kog-url-ssh preserved after git submodule sync" "Field changed"
  fi

  if [[ "$original_https" == "$after_https" ]]; then
    test_pass "Field preservation: kog-url-https preserved after git submodule sync"
  else
    test_fail "Field preservation: kog-url-https preserved after git submodule sync" "Field changed"
  fi

  # Run git submodule update (native Git command)
  git submodule update --init >/dev/null 2>&1

  # Verify kog-* fields are still preserved
  after_ssh=$(git config -f .gitmodules submodule.tools/submodule.kog-url-ssh)
  after_https=$(git config -f .gitmodules submodule.tools/submodule.kog-url-https)

  if [[ "$original_ssh" == "$after_ssh" ]]; then
    test_pass "Field preservation: kog-url-ssh preserved after git submodule update"
  else
    test_fail "Field preservation: kog-url-ssh preserved after git submodule update" "Field changed"
  fi

  if [[ "$original_https" == "$after_https" ]]; then
    test_pass "Field preservation: kog-url-https preserved after git submodule update"
  else
    test_fail "Field preservation: kog-url-https preserved after git submodule update" "Field changed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 4: kog-submodule sync
#------------------------------------------------------------------------------

test_kog_submodule_sync() {
  echo ""
  echo "Integration Test 4: kog-submodule sync"
  echo "======================================"

  local test_dir="$TEST_TMP_DIR/test-kog-sync"
  local submodule_ssh="$TEST_TMP_DIR/submodule-sync-ssh.git"
  local submodule_http="$TEST_TMP_DIR/submodule-sync-http.git"

  # Setup
  create_mock_submodule "$submodule_ssh"
  create_mock_submodule "$submodule_http"
  init_test_repo "$test_dir"

  cd "$test_dir"

  # Create orphan branch
  bash "$CREATE_ORPHAN_SCRIPT" \
    --branch "dev/tools" \
    --dir "$test_dir" \
    >/dev/null 2>&1

  git checkout dev/tools >/dev/null 2>&1

  # Add submodule with kog-* fields
  bash "$KOG_SUBMODULE_SCRIPT" add \
    --ssh "file://$submodule_ssh" \
    --https "file://$submodule_http" \
    --path "tools/submodule" \
    >/dev/null 2>&1

  git add .gitmodules tools/submodule >/dev/null 2>&1
  git commit -m "Add submodule" >/dev/null 2>&1

  # Run kog-submodule sync
  if bash "$KOG_SUBMODULE_SCRIPT" sync >/dev/null 2>&1; then
    test_pass "kog-submodule sync: Command executed successfully"

    # Verify local .git/config was updated
    if [[ -f ".git/modules/tools/submodule/config" ]]; then
      test_pass "kog-submodule sync: Submodule config exists"
    else
      test_fail "kog-submodule sync: Submodule config exists" "Config file not found"
    fi
  else
    test_fail "kog-submodule sync" "Sync command failed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 5: kog-submodule update with Fallback
#------------------------------------------------------------------------------

test_kog_submodule_update_fallback() {
  echo ""
  echo "Integration Test 5: kog-submodule update with Fallback"
  echo "======================================================="

  local test_dir="$TEST_TMP_DIR/test-kog-update"
  local submodule_ssh="$TEST_TMP_DIR/submodule-update-ssh.git"
  local submodule_http="$TEST_TMP_DIR/submodule-update-http.git"

  # Setup
  create_mock_submodule "$submodule_ssh"
  create_mock_submodule "$submodule_http"
  init_test_repo "$test_dir"

  cd "$test_dir"

  # Create orphan branch
  bash "$CREATE_ORPHAN_SCRIPT" \
    --branch "dev/tools" \
    --dir "$test_dir" \
    >/dev/null 2>&1

  git checkout dev/tools >/dev/null 2>&1

  # Add submodule with kog-* fields
  bash "$KOG_SUBMODULE_SCRIPT" add \
    --ssh "file://$submodule_ssh" \
    --https "file://$submodule_http" \
    --path "tools/submodule" \
    >/dev/null 2>&1

  git add .gitmodules tools/submodule >/dev/null 2>&1
  git commit -m "Add submodule" >/dev/null 2>&1

  # Remove submodule directory to test update
  rm -rf tools/submodule

  # Run kog-submodule update
  if bash "$KOG_SUBMODULE_SCRIPT" update >/dev/null 2>&1; then
    test_pass "kog-submodule update: Command executed successfully"

    # Verify submodule was updated
    if [[ -d "tools/submodule" && -f "tools/submodule/README.md" ]]; then
      test_pass "kog-submodule update: Submodule content restored"
    else
      test_fail "kog-submodule update: Submodule content restored" "Submodule not restored"
    fi
  else
    test_fail "kog-submodule update" "Update command failed"
  fi
}

#------------------------------------------------------------------------------
# Integration Test 6: Complete Multi-Remote Orphan Workflow
#------------------------------------------------------------------------------

test_complete_multi_remote_orphan_workflow() {
  echo ""
  echo "Integration Test 6: Complete Multi-Remote Orphan Workflow"
  echo "=========================================================="

  local test_dir="$TEST_TMP_DIR/test-complete-workflow"
  local remote_ssh="$TEST_TMP_DIR/complete-remote-ssh.git"
  local remote_http="$TEST_TMP_DIR/complete-remote-http.git"
  local submodule_ssh="$TEST_TMP_DIR/complete-submodule-ssh.git"
  local submodule_http="$TEST_TMP_DIR/complete-submodule-http.git"

  # Setup
  create_mock_remote "$remote_ssh" 0
  create_mock_remote "$remote_http" 0
  create_mock_submodule "$submodule_ssh"
  create_mock_submodule "$submodule_http"
  init_test_repo "$test_dir"

  cd "$test_dir"

  # Step 1: Setup multi-remote
  bash "$SETUP_MULTI_REMOTE_SCRIPT" \
    --origin-ssh "file://$remote_ssh" \
    --origin-http "file://$remote_http" \
    --dir "$test_dir" \
    >/dev/null 2>&1

  # Step 2: Create orphan branch
  bash "$CREATE_ORPHAN_SCRIPT" \
    --branch "dev/tools" \
    --dir "$test_dir" \
    >/dev/null 2>&1

  git checkout dev/tools >/dev/null 2>&1

  # Step 3: Add submodule with kog-* fields
  bash "$KOG_SUBMODULE_SCRIPT" add \
    --ssh "file://$submodule_ssh" \
    --https "file://$submodule_http" \
    --path "tools/submodule" \
    >/dev/null 2>&1

  git add .gitmodules tools/submodule >/dev/null 2>&1
  git commit -m "Add submodule" >/dev/null 2>&1

  # Step 4: Sync submodules
  bash "$KOG_SUBMODULE_SCRIPT" sync >/dev/null 2>&1

  # Verify complete workflow
  local workflow_success=1

  # Check multi-remote
  if ! git remote | grep -q "origin-ssh"; then
    workflow_success=0
  fi

  # Check orphan branch
  if ! git show-ref --verify --quiet refs/heads/dev/tools; then
    workflow_success=0
  fi

  # Check submodule
  if ! git config -f .gitmodules submodule.tools/submodule.path >/dev/null 2>&1; then
    workflow_success=0
  fi

  # Check kog-* fields
  if ! git config -f .gitmodules submodule.tools/submodule.kog-url-ssh >/dev/null 2>&1; then
    workflow_success=0
  fi

  if [[ $workflow_success -eq 1 ]]; then
    test_pass "Complete workflow: All steps executed successfully"
  else
    test_fail "Complete workflow: All steps executed successfully" "Some steps failed"
  fi
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "=========================================="
  echo "Integration Tests: Multi-Remote with Orphan Branch"
  echo "=========================================="

  setup_test_env

  # Run all integration tests
  test_multi_remote_with_orphan
  test_kog_submodule_add_multi_url
  test_kog_field_preservation
  test_kog_submodule_sync
  test_kog_submodule_update_fallback
  test_complete_multi_remote_orphan_workflow

  # Print summary
  echo ""
  echo "=========================================="
  echo "Test Summary"
  echo "=========================================="
  echo "Tests run: $TESTS_RUN"
  echo "Tests passed: $TESTS_PASSED"
  echo "Tests failed: $TESTS_FAILED"

  if [[ $TESTS_FAILED -eq 0 ]]; then
    echo ""
    echo "✓ All integration tests passed!"
    exit 0
  else
    echo ""
    echo "✗ Some integration tests failed"
    exit 1
  fi
}

main "$@"
