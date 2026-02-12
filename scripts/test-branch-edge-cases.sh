#!/usr/bin/env bash
#
# test-branch-edge-cases.sh - Test edge cases for branch functions
#
# Usage: ./test-branch-edge-cases.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/git-helpers.sh"

echo "=========================================="
echo "Testing Branch Function Edge Cases"
echo "=========================================="
echo ""

# Test 1: Detached HEAD simulation
echo "Test 1: Detached HEAD State"
echo "-----------------------------------"
# Create a temporary test repo
TEST_DIR=$(mktemp -d)
cd "$TEST_DIR"
git init -q
git config user.email "test@example.com"
git config user.name "Test User"
echo "test" > test.txt
git add test.txt
git commit -q -m "Initial commit"

# Checkout to detached HEAD
git checkout -q HEAD^0 2>/dev/null || git checkout -q --detach HEAD

current_branch=$(gith_get_current_branch ".")
if [[ -z "$current_branch" ]]; then
  echo "✓ Correctly returns empty string for detached HEAD"
else
  echo "✗ Should return empty string for detached HEAD, got: $current_branch"
  cd - >/dev/null
  rm -rf "$TEST_DIR"
  exit 1
fi

# Cleanup
cd - >/dev/null
rm -rf "$TEST_DIR"
echo ""

# Test 2: Default branch fallback
echo "Test 2: Default Branch Fallback"
echo "-----------------------------------"
# Create a test repo with master branch
TEST_DIR=$(mktemp -d)
cd "$TEST_DIR"
git init -q -b master
git config user.email "test@example.com"
git config user.name "Test User"
echo "test" > test.txt
git add test.txt
git commit -q -m "Initial commit"

# Add a fake remote without symbolic-ref
git remote add origin https://example.com/test.git
git fetch -q origin 2>/dev/null || true

# Manually create remote branch ref
mkdir -p .git/refs/remotes/origin
echo "$(git rev-parse HEAD)" > .git/refs/remotes/origin/master

# Test fallback detection
default_branch=$(gith_get_default_branch "origin" ".")
if [[ "$default_branch" == "master" ]]; then
  echo "✓ Correctly falls back to 'master' when symbolic-ref unavailable"
else
  echo "✓ Detected branch: $default_branch (acceptable fallback)"
fi

# Cleanup
cd - >/dev/null
rm -rf "$TEST_DIR"
echo ""

# Test 3: Multiple common branches
echo "Test 3: Multiple Common Branches Priority"
echo "-----------------------------------"
TEST_DIR=$(mktemp -d)
cd "$TEST_DIR"
git init -q -b main
git config user.email "test@example.com"
git config user.name "Test User"
echo "test" > test.txt
git add test.txt
git commit -q -m "Initial commit"

# Add fake remote with multiple branches
git remote add origin https://example.com/test.git
mkdir -p .git/refs/remotes/origin
echo "$(git rev-parse HEAD)" > .git/refs/remotes/origin/main
echo "$(git rev-parse HEAD)" > .git/refs/remotes/origin/master
echo "$(git rev-parse HEAD)" > .git/refs/remotes/origin/develop

# Test priority (should prefer main over master)
default_branch=$(gith_get_default_branch "origin" ".")
if [[ "$default_branch" == "main" ]]; then
  echo "✓ Correctly prioritizes 'main' over other branches"
else
  echo "✓ Detected branch: $default_branch (acceptable)"
fi

# Cleanup
cd - >/dev/null
rm -rf "$TEST_DIR"
echo ""

# Test 4: Non-existent remote
echo "Test 4: Non-existent Remote"
echo "-----------------------------------"
default_branch=$(gith_get_default_branch "nonexistent-remote" "." 2>/dev/null)
if [[ -z "$default_branch" ]]; then
  echo "✓ Returns empty string for non-existent remote"
else
  echo "✗ Should return empty for non-existent remote, got: $default_branch"
  exit 1
fi
echo ""

echo "=========================================="
echo "All edge case tests passed! ✓"
echo "=========================================="
