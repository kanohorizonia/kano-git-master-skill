#!/usr/bin/env bash
#
# test-native-planner-contract.sh - Validate native planner JSON contract
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$SKILL_ROOT/../.." && pwd)"

REQUIRE_BINARY=0

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Validate native planner JSON contract output for workspace update and foreach.

Options:
  --require-binary   Fail if C++ binary is not found
  -h, --help         Show help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --require-binary)
      REQUIRE_BINARY=1
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

if [[ -z "$KOG_BIN" ]]; then
  if [[ "$REQUIRE_BINARY" -eq 1 ]]; then
    echo "ERROR: C++ binary not found (required)." >&2
    exit 1
  fi
  echo "SKIP: C++ binary not found."
  exit 0
fi

echo "Using binary: $KOG_BIN"

UPDATE_PLAN_OUTPUT="$($KOG_BIN workspace update --native-plan-only)"
FOREACH_PLAN_OUTPUT="$($KOG_BIN workspace foreach --native-plan-only --command "git status --porcelain")"

python -c 'import json,sys
up,fp=sys.stdin.read().split("\n===\n")
u=json.loads(up)
f=json.loads(fp)

assert u["planner"]=="native-submodule-update"
assert isinstance(u["operations"], list)
assert "waves" in u and "shell_adapter" in u
assert u["shell_adapter"]["script"]=="workspace/update-workspace-repos.sh"
assert "manifest" in u["shell_adapter"]
for op in u["operations"]:
    assert set(("order","wave","path","type","action")).issubset(op.keys())

assert f["planner"]=="native-foreach"
assert isinstance(f["operations"], list)
assert "waves" in f and "shell_adapter" in f
assert f["shell_adapter"]["script"]=="workspace/foreach-repo.sh"
assert "command" in f["shell_adapter"]
for op in f["operations"]:
    assert set(("order","wave","path","type","action","command")).issubset(op.keys())
' <<<"$UPDATE_PLAN_OUTPUT
===
$FOREACH_PLAN_OUTPUT"

echo "PASS: Native planner JSON contract is valid."
