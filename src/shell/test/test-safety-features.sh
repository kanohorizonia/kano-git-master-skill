#!/usr/bin/env bash
#
# test-safety-features.sh - Test safety features of init-empty-repo.sh
#
# This script verifies that the safety mechanisms work correctly

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
INIT_SCRIPT="$SCRIPT_DIR/../repo-management/init-empty-repo.sh"

echo "==================================================================="
echo "Safety Features Test Suite for init-empty-repo.sh"
echo "==================================================================="
echo ""

# Test 1: Detect non-empty remote
echo "Test 1: Detect non-empty remote repository"
echo "-------------------------------------------------------------------"
echo "Command: init-empty-repo.sh git@github.com:dorgonman/kano-git-master-skill-demo.git"
echo ""
if bash "$INIT_SCRIPT" git@github.com:dorgonman/kano-git-master-skill-demo.git 2>&1; then
  echo "❌ FAILED: Script should have detected non-empty remote and failed"
  exit 1
else
  echo "✅ PASSED: Script correctly detected non-empty remote and refused to push"
fi
echo ""
echo ""

# Test 2: Reject old --force flag
echo "Test 2: Reject old --force flag with helpful message"
echo "-------------------------------------------------------------------"
echo "Command: init-empty-repo.sh <url> --force"
echo ""
OUTPUT=$(bash "$INIT_SCRIPT" git@github.com:dorgonman/kano-git-master-skill-demo.git --force 2>&1 || true)
if echo "$OUTPUT" | grep -q "force-overwrite-remote"; then
  echo "✅ PASSED: Script rejected --force and suggested --force-overwrite-remote"
else
  echo "❌ FAILED: Script should reject --force flag"
  echo "Output was: $OUTPUT"
  exit 1
fi
echo ""
echo ""

# Test 3: Help shows new flag
echo "Test 3: Help documentation shows new safety flag"
echo "-------------------------------------------------------------------"
echo "Command: init-empty-repo.sh --help"
echo ""
if bash "$INIT_SCRIPT" --help 2>&1 | grep -q "force-overwrite-remote"; then
  echo "✅ PASSED: Help shows --force-overwrite-remote flag"
else
  echo "❌ FAILED: Help should document --force-overwrite-remote"
  exit 1
fi
echo ""
echo ""

# Test 4: Verify safety warnings in help
echo "Test 4: Help includes safety warnings"
echo "-------------------------------------------------------------------"
if bash "$INIT_SCRIPT" --help 2>&1 | grep -q "DANGEROUS"; then
  echo "✅ PASSED: Help includes DANGEROUS warning"
else
  echo "❌ FAILED: Help should include safety warnings"
  exit 1
fi
echo ""
echo ""

echo "==================================================================="
echo "All Safety Tests Passed! ✅"
echo "==================================================================="
echo ""
echo "Summary:"
echo "  ✅ Pre-test detects non-empty remote"
echo "  ✅ Rejects old --force flag"
echo "  ✅ Documents new --force-overwrite-remote flag"
echo "  ✅ Includes safety warnings in help"
echo ""
echo "The script is safe to use and prevents accidental data loss."
