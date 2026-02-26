#!/usr/bin/env bash
#
# quick-test.sh - Quick smoke tests for Git Master Skill
#
# Purpose:
#   Quick validation of core functionality
#
# Usage:
#   ./quick-test.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$SKILL_ROOT/../.." && pwd)"

echo "Git Master Skill - Quick Test"
echo "=============================="
echo ""

# Test 1: Check all scripts exist
echo "Test 1: Checking script files..."
SCRIPTS=(
  "core/update-repo.sh"
  "core/smart-clone.sh"
  "core/discover-repos.sh"
  "workspace/update-workspace-repos.sh"
  "workspace/foreach-repo.sh"
  "workspace/status-all-repos.sh"
  "branches/rebase-to-upstream-latest.sh"
  "branches/compare-branches.sh"
  "branches/cherry-pick-batch.sh"
  "commit-tools/commit/smart-commit.sh"
  "lib/git-helpers.sh"
)

MISSING=0
for script in "${SCRIPTS[@]}"; do
  if [[ ! -f "$SKILL_ROOT/$script" ]]; then
    echo "  ✗ Missing: $script"
    MISSING=1
  else
    echo "  ✓ Found: $script"
  fi
done

if [[ "$MISSING" -eq 1 ]]; then
  echo ""
  echo "ERROR: Some scripts are missing!"
  exit 1
fi

echo ""
echo "Test 2: Checking help output..."
HELP_SCRIPTS=(
  "core/update-repo.sh"
  "core/smart-clone.sh"
  "core/discover-repos.sh"
  "workspace/update-workspace-repos.sh"
  "workspace/foreach-repo.sh"
  "workspace/status-all-repos.sh"
  "branches/rebase-to-upstream-latest.sh"
  "branches/compare-branches.sh"
  "branches/cherry-pick-batch.sh"
  "commit-tools/commit/smart-commit.sh"
)

HELP_FAILED=0
for script in "${HELP_SCRIPTS[@]}"; do
  if bash "$SKILL_ROOT/$script" --help >/dev/null 2>&1; then
    echo "  ✓ Help works: $script"
  else
    echo "  ✗ Help failed: $script"
    HELP_FAILED=1
  fi
done

if [[ "$HELP_FAILED" -eq 1 ]]; then
  echo ""
  echo "ERROR: Some help commands failed!"
  exit 1
fi

echo ""
echo "Test 3: Checking git-helpers.sh..."
if bash -c "source '$SKILL_ROOT/lib/git-helpers.sh' && type gith_log >/dev/null 2>&1"; then
  echo "  ✓ git-helpers.sh loads correctly"
else
  echo "  ✗ git-helpers.sh failed to load"
  exit 1
fi

echo ""
echo "Test 4: Checking C++ meta JSON output (if binary exists)..."
KOG_BIN=""
BIN_CANDIDATES=(
  "$PROJECT_ROOT/src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe"
  "$PROJECT_ROOT/src/cpp/build/bin/windows-ninja-msvc-arm64/release/kano-git.exe"
  "$PROJECT_ROOT/src/cpp/build/bin/linux-ninja-gcc/release/kano-git"
  "$PROJECT_ROOT/src/cpp/build/bin/macos-ninja-clang/release/kano-git"
  "$PROJECT_ROOT/src/cpp/build/bin/macos-ninja-clang-x64/release/kano-git"
  "$PROJECT_ROOT/src/cpp/build/bin/macos-ninja-clang-arm64/release/kano-git"
)

for candidate in "${BIN_CANDIDATES[@]}"; do
  if [[ -f "$candidate" ]]; then
    KOG_BIN="$candidate"
    break
  fi
done

if [[ -n "$KOG_BIN" ]]; then
  META_OUTPUT="$($KOG_BIN meta --format json)"
  if python -c 'import json,sys; d=json.loads(sys.stdin.read()); assert d["schema_version"]=="v1"; names={c["name"] for c in d["commands"]}; assert "meta" in names and "commit" in names' <<<"$META_OUTPUT"; then
    echo "  ✓ meta JSON output is valid"
  else
    echo "  ✗ meta JSON output is invalid"
    exit 1
  fi

  echo ""
  echo "Test 5: Checking C++ internal completion output..."
  COMPLETE_OUTPUT="$($KOG_BIN __complete --current c)"
  COMPLETE_NESTED="$($KOG_BIN __complete --context workspace --current st)"
  COMPLETE_OPTION="$($KOG_BIN __complete --context workspace --context status --current --na)"
  if python -c 'import sys; root,nested,opt=sys.stdin.read().split("\n===\n"); r=[x.strip() for x in root.splitlines() if x.strip()]; n=[x.strip() for x in nested.splitlines() if x.strip()]; o=[x.strip() for x in opt.splitlines() if x.strip()]; assert "commit" in r; assert "status" in n; assert any(v.startswith("--native") for v in o)' <<<"$COMPLETE_OUTPUT
===
$COMPLETE_NESTED
===
$COMPLETE_OPTION"; then
    echo "  ✓ __complete returns expected candidates"
  else
    echo "  ✗ __complete output missing expected candidates"
    exit 1
  fi

  echo ""
  echo "Test 6: Checking completion script generation..."
  COMP_BASH="$($KOG_BIN completion bash)"
  COMP_ZSH="$($KOG_BIN completion zsh)"
  COMP_FISH="$($KOG_BIN completion fish)"
  COMP_PWSH="$($KOG_BIN completion powershell)"
  if python -c 'import sys; b,z,f,p=sys.stdin.read().split("\n===\n"); assert "__complete" in b and "complete -F" in b; assert "compdef" in z and "__complete" in z; assert "complete -c kano-git" in f and "__complete" in f; assert "Register-ArgumentCompleter" in p and "__complete" in p' <<<"$COMP_BASH
===
$COMP_ZSH
===
$COMP_FISH
===
$COMP_PWSH"; then
    echo "  ✓ completion scripts generated for all shells"
  else
    echo "  ✗ completion script generation failed"
    exit 1
  fi

  if "$KOG_BIN" completion nope >/dev/null 2>&1; then
    echo "  ✗ unsupported shell unexpectedly succeeded"
    exit 1
  else
    echo "  ✓ unsupported shell is rejected"
  fi

  echo ""
  echo "Test 7: Checking native planner JSON contract output..."
  UPDATE_PLAN_OUTPUT="$($KOG_BIN workspace update --native-plan-only)"
  FOREACH_PLAN_OUTPUT="$($KOG_BIN workspace foreach --native-plan-only --command "git status --porcelain")"
  if python -c 'import json,sys; up,fp=sys.stdin.read().split("\n===\n"); u=json.loads(up); f=json.loads(fp); assert u["planner"]=="native-submodule-update"; assert isinstance(u["operations"],list); assert "waves" in u and "shell_adapter" in u; assert u["shell_adapter"]["script"]=="workspace/update-workspace-repos.sh"; assert "manifest" in u["shell_adapter"]; assert all(set(("order","wave","path","type","action")).issubset(op.keys()) for op in u["operations"]); assert f["planner"]=="native-foreach"; assert isinstance(f["operations"],list); assert "waves" in f and "shell_adapter" in f; assert f["shell_adapter"]["script"]=="workspace/foreach-repo.sh"; assert "command" in f["shell_adapter"]; assert all(set(("order","wave","path","type","action","command")).issubset(op.keys()) for op in f["operations"])' <<<"$UPDATE_PLAN_OUTPUT
===
$FOREACH_PLAN_OUTPUT"; then
    echo "  ✓ native planner contract output is valid"
  else
    echo "  ✗ native planner contract output is invalid"
    exit 1
  fi
else
  echo "  - skipped (no built C++ binary found)"
fi

echo ""
echo "=============================="
echo "All quick tests passed! ✓"
echo "=============================="
echo ""
echo "Run full test suite with:"
echo "  ./src/shell/test/run-all-tests.sh --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git"
