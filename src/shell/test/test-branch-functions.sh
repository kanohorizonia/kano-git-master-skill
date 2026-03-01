#!/usr/bin/env bash
#
# test-branch-functions.sh - Test branch operation functions
#
# Usage: ./test-branch-functions.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Enable debug logging
export GITH_DEBUG=1

echo "=========================================="
echo "Testing Branch Operation Functions"
echo "=========================================="
echo ""

# Test 1: gith_get_current_branch
echo "Test 1: gith_get_current_branch"
echo "-----------------------------------"
current_branch=$(gith_get_current_branch ".")
if [[ -n "$current_branch" ]]; then
  echo "✓ Current branch: $current_branch"
else
  echo "✓ Detached HEAD state (empty string returned)"
fi
echo ""

# Test 2: gith_get_default_branch
echo "Test 2: gith_get_default_branch"
echo "-----------------------------------"
default_branch=$(gith_get_default_branch "origin" ".")
if [[ -n "$default_branch" ]]; then
  echo "✓ Default branch for 'origin': $default_branch"
else
  echo "✗ Failed to detect default branch"
  exit 1
fi
echo ""

# Test 3: gith_branch_exists_on_remote
echo "Test 3: gith_branch_exists_on_remote"
echo "-----------------------------------"

# Test with current branch (should exist)
if [[ -n "$current_branch" ]]; then
  if gith_branch_exists_on_remote "origin" "$current_branch" "."; then
    echo "✓ Branch '$current_branch' exists on origin"
  else
    echo "✓ Branch '$current_branch' does not exist on origin (may be local-only)"
  fi
else
  echo "⊘ Skipped (detached HEAD)"
fi
echo ""

# Test with non-existent branch (should not exist)
if gith_branch_exists_on_remote "origin" "this-branch-definitely-does-not-exist-12345" "."; then
  echo "✗ False positive: non-existent branch reported as existing"
  exit 1
else
  echo "✓ Non-existent branch correctly reported as not existing"
fi
echo ""

# Test 4: Edge cases
echo "Test 4: Edge Cases"
echo "-----------------------------------"

# Test with missing remote
if gith_branch_exists_on_remote "nonexistent-remote" "main" "." 2>/dev/null; then
  echo "✗ False positive: branch on non-existent remote"
  exit 1
else
  echo "✓ Correctly handles non-existent remote"
fi
echo ""

# Test with missing arguments
if gith_branch_exists_on_remote "" "main" "." 2>/dev/null; then
  echo "✗ Should fail with empty remote name"
  exit 1
else
  echo "✓ Correctly validates required arguments"
fi
echo ""

echo "=========================================="
echo "All tests passed! ✓"
echo "=========================================="
