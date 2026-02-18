#!/usr/bin/env bash
#
# test-prerequisite-validation-properties.sh - Property-based tests for prerequisite validation
#
# Tests Properties:
#   - Property 38: Independent Script Prerequisites
#
# Requirements: 9.4
#

# Disable errexit to allow test failures without exiting
set +e

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"
CORE_DIR="$(cd "$SCRIPT_DIR/../core" && pwd)"
SUBMODULES_DIR="$(cd "$SCRIPT_DIR/../submodules" && pwd)"

# Disable debug logging to speed up tests
export GITH_DEBUG=0

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

# Create a temporary Git repository
create_temp_git_repo() {
  local repo_dir
  repo_dir=$(mktemp -d)
  (cd "$repo_dir" && git init >/dev/null 2>&1)
  echo "$repo_dir"
}

# Create a temporary non-Git directory
create_temp_non_git_dir() {
  mktemp -d
}

#------------------------------------------------------------------------------
# Property 38: Independent Script Prerequisites
#------------------------------------------------------------------------------

# Feature: repo-initialization-workflow, Property 38: Independent Script Prerequisites
test_property_38_independent_script_prerequisites() {
  print_test_header "Property 38: Independent Script Prerequisites"

  local passed=true
  local scripts=(
    "$CORE_DIR/setup-multi-remote.sh"
    "$CORE_DIR/create-orphan-branch.sh"
    "$CORE_DIR/init-repo-workflow.sh"
    "$SUBMODULES_DIR/kog-submodule.sh"
    "$SUBMODULES_DIR/add-submodule.sh"
  )

  # Test 1: Scripts should have prerequisite validation calls
  echo "  Test 1: Scripts contain prerequisite validation calls"
  local validation_count=0

  for script in "${scripts[@]}"; do
    if [[ ! -f "$script" ]]; then
      echo "    Warning: Script not found: $script"
      continue
    fi

    # Check if script calls gith_validate_prerequisites
    if grep -q "gith_validate_prerequisites" "$script"; then
      ((validation_count++))
    else
      echo "    Failed: Script missing prerequisite validation: $(basename "$script")"
      passed=false
    fi
  done

  echo "    Scripts with prerequisite validation: $validation_count/${#scripts[@]}"

  if [[ $validation_count -lt ${#scripts[@]} ]]; then
    passed=false
  fi

  # Test 2: Scripts requiring Git repo should fail when not in Git repo
  echo "  Test 2: Scripts reject execution when not in Git repository (where applicable)"
  local repo_check_count=0
  local non_git_dir
  non_git_dir=$(mktemp -d)

  # Scripts that require Git repo
  local repo_required_scripts=(
    "$CORE_DIR/setup-multi-remote.sh"
    "$CORE_DIR/create-orphan-branch.sh"
    "$SUBMODULES_DIR/kog-submodule.sh"
    "$SUBMODULES_DIR/add-submodule.sh"
  )

  for script in "${repo_required_scripts[@]}"; do
    if [[ ! -f "$script" ]]; then
      echo "    Warning: Script not found: $script"
      continue
    fi

    # Run script in non-Git directory with minimal valid arguments
    local output
    local exit_code=0

    (
      set +e  # Disable errexit for this subshell
      case "$(basename "$script")" in
        "setup-multi-remote.sh")
          output=$(cd "$non_git_dir" && bash "$script" --origin-ssh "git@example.com:user/repo.git" 2>&1)
          exit_code=$?
          ;;
        "create-orphan-branch.sh")
          output=$(cd "$non_git_dir" && bash "$script" --branch "test-branch" 2>&1)
          exit_code=$?
          ;;
        "kog-submodule.sh")
          output=$(cd "$non_git_dir" && bash "$script" sync 2>&1)
          exit_code=$?
          ;;
        "add-submodule.sh")
          output=$(cd "$non_git_dir" && bash "$script" --url "https://example.com/repo.git" --path "test" 2>&1)
          exit_code=$?
          ;;
      esac

      # Script should exit with error when not in Git repo
      if [[ $exit_code -ne 0 ]]; then
        # Check if error message mentions Git repository
        if echo "$output" | grep -qiE "not.*git repository|git.*not found|missing prerequisites"; then
          echo "PASS"
        else
          echo "UNCLEAR:$output"
        fi
      else
        echo "FAIL"
      fi
    ) > "$non_git_dir/result.txt"

    local result
    result=$(cat "$non_git_dir/result.txt")

    if [[ "$result" == "PASS" ]]; then
      ((repo_check_count++))
    elif [[ "$result" == "FAIL" ]]; then
      echo "    Failed: Script did not check for Git repository: $(basename "$script")"
      passed=false
    else
      echo "    Warning: Script failed but error message unclear: $(basename "$script")"
      echo "    Output: ${result#UNCLEAR:}"
    fi
  done

  rm -rf "$non_git_dir"

  echo "    Scripts that check for Git repo: $repo_check_count/${#repo_required_scripts[@]}"

  # Test 3: Scripts should provide clear error messages for missing prerequisites
  echo "  Test 3: Scripts provide clear error messages for missing prerequisites"
  local clear_message_count=0

  non_git_dir=$(mktemp -d)

  for script in "${repo_required_scripts[@]}"; do
    if [[ ! -f "$script" ]]; then
      continue
    fi

    # Run script and capture error output
    (
      set +e  # Disable errexit for this subshell
      local output
      local exit_code=0

      case "$(basename "$script")" in
        "setup-multi-remote.sh")
          output=$(cd "$non_git_dir" && bash "$script" --origin-ssh "git@example.com:user/repo.git" 2>&1)
          exit_code=$?
          ;;
        "create-orphan-branch.sh")
          output=$(cd "$non_git_dir" && bash "$script" --branch "test-branch" 2>&1)
          exit_code=$?
          ;;
        "kog-submodule.sh")
          output=$(cd "$non_git_dir" && bash "$script" sync 2>&1)
          exit_code=$?
          ;;
        "add-submodule.sh")
          output=$(cd "$non_git_dir" && bash "$script" --url "https://example.com/repo.git" --path "test" 2>&1)
          exit_code=$?
          ;;
      esac

      # Check if error message contains helpful information
      if echo "$output" | grep -qiE "missing prerequisites|recovery|install git|git init|git clone"; then
        echo "PASS"
      else
        echo "UNCLEAR:$output"
      fi
    ) > "$non_git_dir/result.txt"

    local result
    result=$(cat "$non_git_dir/result.txt")

    if [[ "$result" == "PASS" ]]; then
      ((clear_message_count++))
    else
      echo "    Warning: Error message not clear for: $(basename "$script")"
      echo "    Output: ${result#UNCLEAR:}"
    fi
  done

  rm -rf "$non_git_dir"

  echo "    Scripts with clear error messages: $clear_message_count/${#repo_required_scripts[@]}"

  # Test 4: Scripts should work correctly when all prerequisites are met
  echo "  Test 4: Scripts work correctly when all prerequisites are met"
  local success_count=0
  local git_repo
  git_repo=$(create_temp_git_repo)

  for script in "${scripts[@]}"; do
    if [[ ! -f "$script" ]]; then
      continue
    fi

    # Run script with --help (should succeed when prerequisites are met)
    (
      set +e  # Disable errexit for this subshell
      local output
      local exit_code=0
      output=$(cd "$git_repo" && bash "$script" --help 2>&1)
      exit_code=$?

      # Script should succeed with --help when prerequisites are met
      if [[ $exit_code -eq 0 ]]; then
        echo "PASS"
      else
        echo "FAIL:$output"
      fi
    ) > "$git_repo/result.txt"

    local result
    result=$(cat "$git_repo/result.txt")

    if [[ "$result" == "PASS" ]]; then
      ((success_count++))
    else
      echo "    Failed: Script failed with valid prerequisites: $(basename "$script")"
      echo "    Output: ${result#FAIL:}"
      passed=false
    fi
  done

  rm -rf "$git_repo"

  echo "    Scripts that work with valid prerequisites: $success_count/${#scripts[@]}"

  # Overall validation - check if all tests passed
  if [[ $validation_count -lt ${#scripts[@]} ]] || \
     [[ $repo_check_count -lt ${#repo_required_scripts[@]} ]] || \
     [[ $clear_message_count -lt ${#repo_required_scripts[@]} ]] || \
     [[ $success_count -lt ${#scripts[@]} ]]; then
    passed=false
  fi

  print_test_result "Property 38: Independent Script Prerequisites" "$passed"
}

#------------------------------------------------------------------------------
# Main Test Runner
#------------------------------------------------------------------------------

main() {
  echo "=========================================="
  echo "Prerequisite Validation Property-Based Tests"
  echo "=========================================="
  echo "Iterations per property: $TEST_ITERATIONS"
  echo ""

  # Run all property tests
  test_property_38_independent_script_prerequisites

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
