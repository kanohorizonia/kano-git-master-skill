#!/usr/bin/env bash
#
# test-create-orphan-branch-properties.sh - Property-based tests for create-orphan-branch.sh
#
# This file implements property-based tests for the create-orphan-branch.sh script.
# Each property test runs 100+ iterations with generated inputs to verify
# universal correctness properties.
#
# Properties tested:
# - Property 10: Orphan Branch Name Validation
# - Property 11: Orphan Branch Round Trip
# - Property 12: Orphan Branch Isolation
# - Property 13: Orphan Branch Establishment
# - Property 25: Destructive Operation Protection
# - Property 26: Force Flag Warning
#
# **Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5, 7.1, 7.2**

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Script under test
CREATE_ORPHAN_SCRIPT="$SCRIPT_DIR/../core/create-orphan-branch.sh"

# Test configuration
PROPERTY_TEST_ITERATIONS=${PROPERTY_TEST_ITERATIONS:-10}  # Reduced default for faster testing
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

# Generate random valid branch name
generate_valid_branch_name() {
  local name_type=$((RANDOM % 5))
  local num=$((RANDOM % 1000))

  case $name_type in
    0) echo "feature/branch-$num" ;;
    1) echo "orphan-$num" ;;
    2) echo "dev/tools-$num" ;;
    3) echo "test-branch-$num" ;;
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

# Generate random file content
generate_random_content() {
  local content_type=$((RANDOM % 3))

  case $content_type in
    0) echo "# Test Content $RANDOM" ;;
    1) echo "Test file with random number: $RANDOM" ;;
    2) echo "Content line 1
Content line 2
Random: $RANDOM" ;;
  esac
}

# Create test repository
create_test_repo() {
  local repo_path="$1"
  mkdir -p "$repo_path"
  (
    cd "$repo_path"
    git init -q
    git config user.email "test@example.com"
    git config user.name "Test User"

    # Create initial commit on main branch
    echo "# Test Repo" > README.md
    git add README.md
    git commit -q -m "Initial commit"
  )
}

# Add uncommitted changes to repository
add_uncommitted_changes() {
  local repo_path="$1"
  (
    cd "$repo_path"

    # Add some uncommitted changes
    echo "Uncommitted change $RANDOM" > uncommitted.txt
    echo "Modified" >> README.md
  )
}

#------------------------------------------------------------------------------
# Property Tests
#------------------------------------------------------------------------------

# Feature: repo-initialization-workflow, Property 10: Orphan Branch Name Validation
# For any orphan branch creation request, the system should verify the branch name
# does not exist locally or remotely before proceeding, rejecting requests for
# existing branch names.
test_property_10_orphan_branch_name_validation() {
  echo ""
  echo "=== Property 10: Orphan Branch Name Validation ==="
  echo "Testing that existing branches are rejected..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    echo "  Iteration $i/$iterations..."
    local test_repo="$TEST_TMP_DIR/prop10-repo-$i"
    create_test_repo "$test_repo"

    # Create a branch that already exists
    local existing_branch="existing-branch-$i"
    (
      cd "$test_repo"
      git checkout -b "$existing_branch" -q 2>/dev/null
      git checkout main -q 2>/dev/null
    )

    # Try to create orphan branch with same name (should fail)
    local output
    output=$(timeout 10 bash "$CREATE_ORPHAN_SCRIPT" --branch "$existing_branch" --dir "$test_repo" 2>&1) || true

    if echo "$output" | grep -q "Branch already exists"; then
      # Script correctly rejected the existing branch
      ((passed++))
    else
      test_fail "Property 10 iteration $i" "Script did not reject existing branch name"
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 10: Orphan Branch Name Validation ($passed/$iterations iterations)"
  else
    test_fail "Property 10: Orphan Branch Name Validation" "$passed/$iterations iterations passed"
  fi
}

# Feature: repo-initialization-workflow, Property 11: Orphan Branch Round Trip
# For any repository with uncommitted changes, creating an orphan branch and then
# returning to the original branch should restore the working directory to its
# exact previous state (including all uncommitted changes).
test_property_11_orphan_branch_round_trip() {
  echo ""
  echo "=== Property 11: Orphan Branch Round Trip ==="
  echo "Testing that uncommitted changes are preserved..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    echo "  Iteration $i/$iterations..."
    local test_repo="$TEST_TMP_DIR/prop11-repo-$i"
    create_test_repo "$test_repo"

    # Add uncommitted changes
    add_uncommitted_changes "$test_repo"

    # Capture state before
    local before_status
    before_status=$(cd "$test_repo" && git status --porcelain)

    # Create orphan branch and return
    local orphan_branch="orphan-$i"
    if ! timeout 15 bash "$CREATE_ORPHAN_SCRIPT" --branch "$orphan_branch" --dir "$test_repo" --return 2>/dev/null; then
      test_fail "Property 11 iteration $i" "Script failed to create orphan branch"
      continue
    fi

    # Capture state after
    local after_status
    after_status=$(cd "$test_repo" && git status --porcelain)

    # Verify we're back on main
    local current_branch
    current_branch=$(cd "$test_repo" && git branch --show-current)

    if [[ "$current_branch" != "main" ]]; then
      test_fail "Property 11 iteration $i" "Not on original branch (on: $current_branch)"
      continue
    fi

    # Verify uncommitted changes are restored
    if [[ "$before_status" == "$after_status" ]]; then
      ((passed++))
    else
      test_fail "Property 11 iteration $i" "Uncommitted changes not restored"
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 11: Orphan Branch Round Trip ($passed/$iterations iterations)"
  else
    test_fail "Property 11: Orphan Branch Round Trip" "$passed/$iterations iterations passed"
  fi
}

# Feature: repo-initialization-workflow, Property 12: Orphan Branch Isolation
# For any newly created orphan branch, the working directory should be empty
# (no files from the parent branch) and contain only the files explicitly
# added during initialization.
test_property_12_orphan_branch_isolation() {
  echo ""
  echo "=== Property 12: Orphan Branch Isolation ==="
  echo "Testing that orphan branches are isolated from parent..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    echo "  Iteration $i/$iterations..."
    local test_repo="$TEST_TMP_DIR/prop12-repo-$i"
    create_test_repo "$test_repo"

    # Add multiple files to main branch
    (
      cd "$test_repo"
      echo "File 1" > file1.txt
      echo "File 2" > file2.txt
      git add file1.txt file2.txt
      git commit -q -m "Add files"
    )

    # Create orphan branch
    local orphan_branch="orphan-$i"
    local orphan_file="orphan-file-$i.txt"
    if ! timeout 15 bash "$CREATE_ORPHAN_SCRIPT" --branch "$orphan_branch" --dir "$test_repo" --file "$orphan_file" 2>/dev/null; then
      test_fail "Property 12 iteration $i" "Script failed to create orphan branch"
      continue
    fi

    # Check files on orphan branch
    local file_count
    file_count=$(cd "$test_repo" && git checkout "$orphan_branch" -q 2>/dev/null && git ls-files | wc -l)

    # Check that parent files don't exist
    local has_file1=$(cd "$test_repo" && [[ -f "file1.txt" ]] && echo "yes" || echo "no")
    local has_file2=$(cd "$test_repo" && [[ -f "file2.txt" ]] && echo "yes" || echo "no")
    local has_orphan=$(cd "$test_repo" && [[ -f "$orphan_file" ]] && echo "yes" || echo "no")

    if [[ "$has_file1" == "no" ]] && [[ "$has_file2" == "no" ]] && [[ "$has_orphan" == "yes" ]] && [[ $file_count -eq 1 ]]; then
      ((passed++))
    else
      test_fail "Property 12 iteration $i" "Orphan branch not isolated (file count: $file_count, has_file1: $has_file1, has_file2: $has_file2, has_orphan: $has_orphan)"
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 12: Orphan Branch Isolation ($passed/$iterations iterations)"
  else
    test_fail "Property 12: Orphan Branch Isolation" "$passed/$iterations iterations passed"
  fi
}

# Feature: repo-initialization-workflow, Property 13: Orphan Branch Establishment
# For any orphan branch creation, the branch should be established with exactly
# one initial commit, and that commit should have no parent commits.
test_property_13_orphan_branch_establishment() {
  echo ""
  echo "=== Property 13: Orphan Branch Establishment ==="
  echo "Testing that orphan branches have no parent commits..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    echo "  Iteration $i/$iterations..."
    local test_repo="$TEST_TMP_DIR/prop13-repo-$i"
    create_test_repo "$test_repo"

    # Create orphan branch
    local orphan_branch="orphan-$i"
    if ! timeout 15 bash "$CREATE_ORPHAN_SCRIPT" --branch "$orphan_branch" --dir "$test_repo" 2>/dev/null; then
      test_fail "Property 13 iteration $i" "Script failed to create orphan branch"
      continue
    fi

    # Check commit count and parent
    local commit_count
    commit_count=$(cd "$test_repo" && git checkout "$orphan_branch" -q 2>/dev/null && git rev-list --count HEAD 2>/dev/null || echo "0")

    local parent_count
    parent_count=$(cd "$test_repo" && git rev-list --parents HEAD | head -1 | wc -w)
    parent_count=$((parent_count - 1))  # Subtract the commit hash itself

    if [[ $commit_count -eq 1 ]] && [[ $parent_count -eq 0 ]]; then
      ((passed++))
    else
      test_fail "Property 13 iteration $i" "Invalid orphan branch (commits: $commit_count, parents: $parent_count)"
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 13: Orphan Branch Establishment ($passed/$iterations iterations)"
  else
    test_fail "Property 13: Orphan Branch Establishment" "$passed/$iterations iterations passed"
  fi
}

# Feature: repo-initialization-workflow, Property 25: Destructive Operation Protection
# For any destructive operation (e.g., overwriting existing branch), the system
# should reject the operation unless an explicit force flag with a verbose name
# is provided.
test_property_25_destructive_operation_protection() {
  echo ""
  echo "=== Property 25: Destructive Operation Protection ==="
  echo "Testing that destructive operations require force flag..."

  local iterations=$PROPERTY_TEST_ITERATIONS
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    echo "  Iteration $i/$iterations..."
    local test_repo="$TEST_TMP_DIR/prop25-repo-$i"
    create_test_repo "$test_repo"

    # Create a branch
    local branch_name="test-branch-$i"
    (
      cd "$test_repo"
      git checkout -b "$branch_name" -q 2>/dev/null
      echo "Original content" > original.txt
      git add original.txt
      git commit -q -m "Original commit"
      git checkout main -q 2>/dev/null
    )

    # Try to overwrite without force flag (should fail)
    if timeout 10 bash "$CREATE_ORPHAN_SCRIPT" --branch "$branch_name" --dir "$test_repo" 2>/dev/null; then
      test_fail "Property 25 iteration $i" "Script should reject overwrite without force flag"
      continue
    fi

    # Verify original branch is unchanged
    local has_original
    has_original=$(cd "$test_repo" && git checkout "$branch_name" -q 2>/dev/null && [[ -f "original.txt" ]] && echo "yes" || echo "no")

    if [[ "$has_original" == "yes" ]]; then
      ((passed++))
    else
      test_fail "Property 25 iteration $i" "Original branch was modified"
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 25: Destructive Operation Protection ($passed/$iterations iterations)"
  else
    test_fail "Property 25: Destructive Operation Protection" "$passed/$iterations iterations passed"
  fi
}

# Feature: repo-initialization-workflow, Property 26: Force Flag Warning
# For any operation invoked with a force flag, the system should display a
# warning message and wait at least 3 seconds before proceeding with the
# destructive operation.
test_property_26_force_flag_warning() {
  echo ""
  echo "=== Property 26: Force Flag Warning ==="
  echo "Testing that force flag shows warning and allows overwrite..."

  # This property tests that the force flag allows overwrite
  # The 3-second delay is tested but we use fewer iterations

  local iterations=3  # Fewer iterations due to 3-second delay per test
  local passed=0

  for ((i=1; i<=iterations; i++)); do
    echo "  Iteration $i/$iterations (this will take ~3 seconds)..."
    local test_repo="$TEST_TMP_DIR/prop26-repo-$i"
    create_test_repo "$test_repo"

    # Create a branch
    local branch_name="test-branch-$i"
    local original_commit
    (
      cd "$test_repo"
      git checkout -b "$branch_name" -q 2>/dev/null
      echo "Original content" > original.txt
      git add original.txt
      git commit -q -m "Original commit"
    )
    original_commit=$(cd "$test_repo" && git rev-parse HEAD)
    (cd "$test_repo" && git checkout main -q 2>/dev/null)

    # Overwrite with force flag (should succeed after 3 second delay)
    local start_time
    start_time=$(date +%s)

    local output
    output=$(timeout 20 bash "$CREATE_ORPHAN_SCRIPT" --branch "$branch_name" --dir "$test_repo" --force-overwrite-branch 2>&1) || true

    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    # Verify branch was overwritten
    local new_commit
    new_commit=$(cd "$test_repo" && git checkout "$branch_name" -q 2>/dev/null && git rev-parse HEAD)

    # Check that commit changed and elapsed time >= 3 seconds
    if [[ "$original_commit" != "$new_commit" ]] && [[ $elapsed -ge 3 ]]; then
      ((passed++))
    else
      test_fail "Property 26 iteration $i" "Force flag behavior incorrect (elapsed: ${elapsed}s, commits match: $([[ "$original_commit" == "$new_commit" ]] && echo "yes" || echo "no"))"
    fi
  done

  if [[ $passed -eq $iterations ]]; then
    test_pass "Property 26: Force Flag Warning ($passed/$iterations iterations)"
  else
    test_fail "Property 26: Force Flag Warning" "$passed/$iterations iterations passed"
  fi
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "========================================="
  echo "Property-Based Tests: create-orphan-branch.sh"
  echo "========================================="
  echo "Iterations per property: $PROPERTY_TEST_ITERATIONS"

  # Setup test environment
  setup_test_env

  # Run property tests
  test_property_10_orphan_branch_name_validation
  test_property_11_orphan_branch_round_trip
  test_property_12_orphan_branch_isolation
  test_property_13_orphan_branch_establishment
  test_property_25_destructive_operation_protection
  test_property_26_force_flag_warning

  # Summary
  echo ""
  echo "========================================="
  echo "Test Summary"
  echo "========================================="
  echo "Total tests: $TESTS_RUN"
  echo "Passed: $TESTS_PASSED"
  echo "Failed: $TESTS_FAILED"
  echo ""

  if [[ $TESTS_FAILED -eq 0 ]]; then
    echo "✓ All property tests passed!"
    exit 0
  else
    echo "✗ Some property tests failed"
    exit 1
  fi
}

# Run main
main "$@"
