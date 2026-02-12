#!/usr/bin/env bash
#
# run-all-tests.sh - Comprehensive test suite for Git Master Skill
#
# Purpose:
#   Test all scripts with a real repository to ensure functionality
#
# Usage:
#   ./run-all-tests.sh [options]
#
# Options:
#   --test-repo <url>     Test repository URL (default: git@github.com:dorgonman/kano-git-master-skill-demo.git)
#   --cleanup             Clean up test directory after tests
#   --verbose             Show detailed output
#   -h, --help            Show help
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Configuration
TEST_REPO="git@github.com:dorgonman/kano-git-master-skill-demo.git"
TEST_DIR="$(mktemp -d)"
CLEANUP=0
VERBOSE=0
PASSED=0
FAILED=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Comprehensive test suite for Git Master Skill scripts.

Options:
  --test-repo <url>     Test repository URL
  --cleanup             Clean up test directory after tests
  --verbose             Show detailed output
  -h, --help            Show help

Default test repo: git@github.com:dorgonman/kano-git-master-skill-demo.git
EOF
}

log_info() {
  echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
  echo -e "${GREEN}[PASS]${NC} $*"
  ((PASSED++))
}

log_error() {
  echo -e "${RED}[FAIL]${NC} $*"
  ((FAILED++))
}

log_warn() {
  echo -e "${YELLOW}[WARN]${NC} $*"
}

run_test() {
  local test_name="$1"
  shift
  
  log_info "Running: $test_name"
  
  if [[ "$VERBOSE" -eq 1 ]]; then
    if "$@"; then
      log_success "$test_name"
      return 0
    else
      log_error "$test_name"
      return 1
    fi
  else
    if "$@" >/dev/null 2>&1; then
      log_success "$test_name"
      return 0
    else
      log_error "$test_name"
      return 1
    fi
  fi
}

#------------------------------------------------------------------------------
# Test Cases
#------------------------------------------------------------------------------

test_clone_with_upstream() {
  log_info "Test: clone-with-upstream.sh"
  
  local test_clone_dir="$TEST_DIR/clone-test"
  
  # Test basic clone
  if bash "$SKILL_ROOT/scripts/repo-management/clone-with-upstream.sh" \
    "$TEST_REPO" \
    --dir "$test_clone_dir" >/dev/null 2>&1; then
    
    if [[ -d "$test_clone_dir/.git" ]]; then
      log_success "clone-with-upstream.sh: Basic clone"
    else
      log_error "clone-with-upstream.sh: Clone directory not created"
      return 1
    fi
  else
    log_error "clone-with-upstream.sh: Clone failed"
    return 1
  fi
  
  return 0
}

test_update_repo() {
  log_info "Test: update-repo.sh"
  
  local test_repo_dir="$TEST_DIR/update-test"
  
  # Clone first
  git clone "$TEST_REPO" "$test_repo_dir" >/dev/null 2>&1
  
  # Test update with dry-run
  if bash "$SKILL_ROOT/scripts/repo-management/update-repo.sh" \
    "$test_repo_dir" \
    --dry-run >/dev/null 2>&1; then
    log_success "update-repo.sh: Dry-run mode"
  else
    log_error "update-repo.sh: Dry-run failed"
    return 1
  fi
  
  # Test actual update
  if bash "$SKILL_ROOT/scripts/repo-management/update-repo.sh" \
    "$test_repo_dir" >/dev/null 2>&1; then
    log_success "update-repo.sh: Update"
  else
    log_error "update-repo.sh: Update failed"
    return 1
  fi
  
  return 0
}

test_discover_repos() {
  log_info "Test: discover-repos.sh"
  
  local test_workspace="$TEST_DIR/discover-test"
  mkdir -p "$test_workspace"
  
  # Create test structure
  git clone "$TEST_REPO" "$test_workspace/repo1" >/dev/null 2>&1
  git clone "$TEST_REPO" "$test_workspace/repo2" >/dev/null 2>&1
  
  # Test discovery
  if bash "$SKILL_ROOT/scripts/repo-management/discover-repos.sh" \
    --root "$test_workspace" >/dev/null 2>&1; then
    log_success "discover-repos.sh: Discovery"
  else
    log_error "discover-repos.sh: Discovery failed"
    return 1
  fi
  
  # Test JSON output
  local manifest="$test_workspace/manifest.json"
  if bash "$SKILL_ROOT/scripts/repo-management/discover-repos.sh" \
    --root "$test_workspace" \
    --format json \
    --save "$manifest" >/dev/null 2>&1; then
    
    if [[ -f "$manifest" ]]; then
      log_success "discover-repos.sh: JSON manifest"
    else
      log_error "discover-repos.sh: Manifest not created"
      return 1
    fi
  else
    log_error "discover-repos.sh: JSON output failed"
    return 1
  fi
  
  return 0
}

test_status_all_repos() {
  log_info "Test: status-all-repos.sh"
  
  local test_workspace="$TEST_DIR/status-test"
  mkdir -p "$test_workspace"
  
  # Create test repos
  git clone "$TEST_REPO" "$test_workspace/repo1" >/dev/null 2>&1
  
  # Test table format
  if bash "$SKILL_ROOT/scripts/workspace/status-all-repos.sh" \
    --root "$test_workspace" >/dev/null 2>&1; then
    log_success "status-all-repos.sh: Table format"
  else
    log_error "status-all-repos.sh: Table format failed"
    return 1
  fi
  
  # Test JSON format
  if bash "$SKILL_ROOT/scripts/workspace/status-all-repos.sh" \
    --root "$test_workspace" \
    --format json >/dev/null 2>&1; then
    log_success "status-all-repos.sh: JSON format"
  else
    log_error "status-all-repos.sh: JSON format failed"
    return 1
  fi
  
  # Test markdown format
  local status_md="$test_workspace/status.md"
  if bash "$SKILL_ROOT/scripts/workspace/status-all-repos.sh" \
    --root "$test_workspace" \
    --format markdown \
    --output "$status_md" >/dev/null 2>&1; then
    
    if [[ -f "$status_md" ]]; then
      log_success "status-all-repos.sh: Markdown output"
    else
      log_error "status-all-repos.sh: Markdown file not created"
      return 1
    fi
  else
    log_error "status-all-repos.sh: Markdown format failed"
    return 1
  fi
  
  return 0
}

test_foreach_repo() {
  log_info "Test: foreach-repo.sh"
  
  local test_workspace="$TEST_DIR/foreach-test"
  mkdir -p "$test_workspace"
  
  # Create test repos
  git clone "$TEST_REPO" "$test_workspace/repo1" >/dev/null 2>&1
  git clone "$TEST_REPO" "$test_workspace/repo2" >/dev/null 2>&1
  
  # Test command execution
  if bash "$SKILL_ROOT/scripts/workspace/foreach-repo.sh" \
    "git status --short" \
    --root "$test_workspace" >/dev/null 2>&1; then
    log_success "foreach-repo.sh: Command execution"
  else
    log_error "foreach-repo.sh: Command execution failed"
    return 1
  fi
  
  return 0
}

test_update_workspace_repos() {
  log_info "Test: update-workspace-repos.sh"
  
  local test_workspace="$TEST_DIR/workspace-update-test"
  mkdir -p "$test_workspace"
  
  # Create test repos
  git clone "$TEST_REPO" "$test_workspace/repo1" >/dev/null 2>&1
  
  # Test dry-run
  if bash "$SKILL_ROOT/scripts/workspace/update-workspace-repos.sh" \
    --root "$test_workspace" \
    --dry-run >/dev/null 2>&1; then
    log_success "update-workspace-repos.sh: Dry-run"
  else
    log_error "update-workspace-repos.sh: Dry-run failed"
    return 1
  fi
  
  # Test actual update
  if bash "$SKILL_ROOT/scripts/workspace/update-workspace-repos.sh" \
    --root "$test_workspace" >/dev/null 2>&1; then
    log_success "update-workspace-repos.sh: Update"
  else
    log_error "update-workspace-repos.sh: Update failed"
    return 1
  fi
  
  return 0
}

test_compare_branches() {
  log_info "Test: compare-branches.sh"
  
  local test_repo_dir="$TEST_DIR/compare-test"
  
  # Clone and create test branches
  git clone "$TEST_REPO" "$test_repo_dir" >/dev/null 2>&1
  cd "$test_repo_dir"
  
  # Create a test branch with a commit
  git checkout -b test-branch >/dev/null 2>&1
  echo "test" > test-file.txt
  git add test-file.txt
  git commit -m "test: Add test file" >/dev/null 2>&1
  
  # Test table format
  if bash "$SKILL_ROOT/scripts/branch-operations/compare-branches.sh" \
    main test-branch \
    --repo "$test_repo_dir" >/dev/null 2>&1; then
    log_success "compare-branches.sh: Table format"
  else
    log_error "compare-branches.sh: Table format failed"
    return 1
  fi
  
  # Test JSON format
  if bash "$SKILL_ROOT/scripts/branch-operations/compare-branches.sh" \
    main test-branch \
    --repo "$test_repo_dir" \
    --format json >/dev/null 2>&1; then
    log_success "compare-branches.sh: JSON format"
  else
    log_error "compare-branches.sh: JSON format failed"
    return 1
  fi
  
  # Test markdown format
  local diff_md="$test_repo_dir/diff.md"
  if bash "$SKILL_ROOT/scripts/branch-operations/compare-branches.sh" \
    main test-branch \
    --repo "$test_repo_dir" \
    --format markdown \
    --output "$diff_md" >/dev/null 2>&1; then
    
    if [[ -f "$diff_md" ]]; then
      log_success "compare-branches.sh: Markdown output"
    else
      log_error "compare-branches.sh: Markdown file not created"
      return 1
    fi
  else
    log_error "compare-branches.sh: Markdown format failed"
    return 1
  fi
  
  # Test bidirectional
  if bash "$SKILL_ROOT/scripts/branch-operations/compare-branches.sh" \
    main test-branch \
    --repo "$test_repo_dir" \
    --bidirectional >/dev/null 2>&1; then
    log_success "compare-branches.sh: Bidirectional"
  else
    log_error "compare-branches.sh: Bidirectional failed"
    return 1
  fi
  
  cd - >/dev/null
  return 0
}

test_cherry_pick_batch() {
  log_info "Test: cherry-pick-batch.sh"
  
  local test_repo_dir="$TEST_DIR/cherry-pick-test"
  
  # Clone and create test commits
  git clone "$TEST_REPO" "$test_repo_dir" >/dev/null 2>&1
  cd "$test_repo_dir"
  
  # Create test commits
  echo "commit1" > file1.txt
  git add file1.txt
  git commit -m "test: Commit 1" >/dev/null 2>&1
  local hash1=$(git rev-parse HEAD)
  
  echo "commit2" > file2.txt
  git add file2.txt
  git commit -m "test: Commit 2" >/dev/null 2>&1
  local hash2=$(git rev-parse HEAD)
  
  # Create a new branch without these commits
  git checkout -b cherry-pick-target HEAD~2 >/dev/null 2>&1
  
  # Create JSON file
  cat > commits.json <<EOF
{
  "commits": [
    {
      "hash": "$hash1",
      "title": "test: Commit 1"
    },
    {
      "hash": "$hash2",
      "title": "test: Commit 2"
    }
  ]
}
EOF
  
  # Test dry-run
  if bash "$SKILL_ROOT/scripts/branch-operations/cherry-pick-batch.sh" \
    commits.json \
    --repo "$test_repo_dir" \
    --dry-run >/dev/null 2>&1; then
    log_success "cherry-pick-batch.sh: Dry-run"
  else
    log_error "cherry-pick-batch.sh: Dry-run failed"
    return 1
  fi
  
  # Test actual cherry-pick
  if bash "$SKILL_ROOT/scripts/branch-operations/cherry-pick-batch.sh" \
    commits.json \
    --repo "$test_repo_dir" >/dev/null 2>&1; then
    log_success "cherry-pick-batch.sh: Cherry-pick"
  else
    log_error "cherry-pick-batch.sh: Cherry-pick failed"
    return 1
  fi
  
  # Verify files exist
  if [[ -f "file1.txt" ]] && [[ -f "file2.txt" ]]; then
    log_success "cherry-pick-batch.sh: Files verified"
  else
    log_error "cherry-pick-batch.sh: Files not found after cherry-pick"
    return 1
  fi
  
  # Test text format
  cd "$test_repo_dir"
  git checkout -b text-format-test HEAD~2 >/dev/null 2>&1
  
  cat > commits.txt <<EOF
$hash1 test: Commit 1
$hash2 test: Commit 2
EOF
  
  if bash "$SKILL_ROOT/scripts/branch-operations/cherry-pick-batch.sh" \
    commits.txt \
    --repo "$test_repo_dir" >/dev/null 2>&1; then
    log_success "cherry-pick-batch.sh: Text format"
  else
    log_error "cherry-pick-batch.sh: Text format failed"
    return 1
  fi
  
  cd - >/dev/null
  return 0
}

test_rebase_to_upstream() {
  log_info "Test: rebase-to-upstream-latest.sh"
  
  local test_repo_dir="$TEST_DIR/rebase-test"
  
  # Clone repo
  git clone "$TEST_REPO" "$test_repo_dir" >/dev/null 2>&1
  cd "$test_repo_dir"
  
  # Add upstream remote (use same repo for testing)
  git remote add upstream "$TEST_REPO" >/dev/null 2>&1
  git fetch upstream >/dev/null 2>&1
  
  # Test dry-run
  if bash "$SKILL_ROOT/scripts/branch-operations/rebase-to-upstream-latest.sh" \
    --dry-run 2>&1 | grep -q "DRY-RUN"; then
    log_success "rebase-to-upstream-latest.sh: Dry-run"
  else
    log_error "rebase-to-upstream-latest.sh: Dry-run failed"
    cd - >/dev/null
    return 1
  fi
  
  cd - >/dev/null
  return 0
}

test_smart_commit() {
  log_info "Test: smart-commit.sh"
  
  # Note: This test only checks if the script runs with --help
  # Full testing requires Copilot CLI which may not be available
  
  if bash "$SKILL_ROOT/scripts/commit-tools/smart-commit.sh" --help >/dev/null 2>&1; then
    log_success "smart-commit.sh: Help output"
  else
    log_error "smart-commit.sh: Help failed"
    return 1
  fi
  
  log_warn "smart-commit.sh: Full testing requires GitHub Copilot CLI"
  
  return 0
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --test-repo)
        TEST_REPO="${2:-}"
        shift 2
        ;;
      --cleanup)
        CLEANUP=1
        shift
        ;;
      --verbose)
        VERBOSE=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 1
        ;;
    esac
  done
  
  echo ""
  echo "=========================================="
  echo "  Git Master Skill - Test Suite"
  echo "=========================================="
  echo ""
  log_info "Test repository: $TEST_REPO"
  log_info "Test directory: $TEST_DIR"
  log_info "Skill root: $SKILL_ROOT"
  echo ""
  
  # Run tests
  test_clone_with_upstream || true
  test_update_repo || true
  test_discover_repos || true
  test_status_all_repos || true
  test_foreach_repo || true
  test_update_workspace_repos || true
  test_compare_branches || true
  test_cherry_pick_batch || true
  test_rebase_to_upstream || true
  test_smart_commit || true
  
  # Summary
  echo ""
  echo "=========================================="
  echo "  Test Summary"
  echo "=========================================="
  echo -e "${GREEN}Passed: $PASSED${NC}"
  echo -e "${RED}Failed: $FAILED${NC}"
  echo ""
  
  # Cleanup
  if [[ "$CLEANUP" -eq 1 ]]; then
    log_info "Cleaning up test directory: $TEST_DIR"
    rm -rf "$TEST_DIR"
  else
    log_info "Test directory preserved: $TEST_DIR"
    log_info "Run with --cleanup to remove it"
  fi
  
  # Exit code
  if [[ "$FAILED" -gt 0 ]]; then
    exit 1
  fi
  
  exit 0
}

# Trap cleanup on exit
trap 'if [[ "$CLEANUP" -eq 1 ]]; then rm -rf "$TEST_DIR"; fi' EXIT

# Run main
main "$@"
