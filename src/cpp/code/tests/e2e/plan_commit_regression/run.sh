#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${1:-}" ]]; then
  WORKSPACE_ROOT="$(cd "$1" && pwd)"
else
  WORKSPACE_ROOT=""
  cursor="$SCRIPT_DIR"
  while [[ "$cursor" != "/" ]]; do
    if [[ -f "$cursor/scripts/kog" ]]; then
      WORKSPACE_ROOT="$cursor"
      break
    fi
    cursor="$(dirname "$cursor")"
  done
  if [[ -z "$WORKSPACE_ROOT" ]]; then
    echo "Error: cannot resolve workspace root from script path: $SCRIPT_DIR" >&2
    exit 1
  fi
fi
KOG_PATH="${2:-$WORKSPACE_ROOT/scripts/kog}"

if [[ ! -f "$KOG_PATH" ]]; then
  echo "Error: kog launcher not found: $KOG_PATH" >&2
  exit 1
fi

cd "$WORKSPACE_ROOT"

PLAN_DIR=".kano/tmp/git/plans"

make_sandbox() {
  local base="$WORKSPACE_ROOT/.kano/tmp/git/e2e"
  local name="plan-commit-regression-$(date +%s%N)"
  local sandbox="$base/$name"
  local remote="$sandbox/remote.git"
  local repo="$sandbox/work"
  mkdir -p "$sandbox"
  git init --bare "$remote" >/dev/null 2>&1
  git init --initial-branch main "$repo" >/dev/null 2>&1
  pushd "$repo" >/dev/null
  git config user.name "kano e2e"
  git config user.email "kano-e2e@example.invalid"
  printf 'seed\n' > README.md
  git add README.md
  git commit -m "test(e2e): seed" >/dev/null 2>&1
  git remote add origin "$remote"
  git push -u origin main >/dev/null 2>&1
  printf 'dirty\n' >> README.md
  popd >/dev/null
  printf '%s\n' "$repo"
}

SANDBOX_REPO="$(make_sandbox)"
PLAN_PATH="$SANDBOX_REPO/$PLAN_DIR/e2e-regression-plan.json"
MUT_PATH="$SANDBOX_REPO/$PLAN_DIR/e2e-regression-plan-mutated.json"
DEFAULT_PLAN_PATH="$SANDBOX_REPO/$PLAN_DIR/default-plan.json"
mkdir -p "$SANDBOX_REPO/$PLAN_DIR"

run_kog() {
  local workdir="$1"
  shift
  local out
  out="$(cd "$workdir" && "$KOG_PATH" "$@" 2>&1)" || {
    local ec=$?
    printf '%s\n' "$out"
    return "$ec"
  }
  printf '%s\n' "$out"
  return 0
}

echo "[E2E] T1 agent shared-plan bootstrap"
set +e
T1_OUT="$(KANO_AGENT_MODE=1 run_kog "$SANDBOX_REPO" cpa)"
T1_EC=$?
set -e
if [[ $T1_EC -ne 3 ]]; then
  echo "T1 failed: expected explicit agent-plan-required exit 3, got $T1_EC" >&2
  exit 1
fi
if [[ "$T1_OUT" != *"agent mode + --plan-file detected; using plan-driven flow"* ||
      "$T1_OUT" != *"refresh-needed: missing-or-unreadable"* ||
      "$T1_OUT" != *"[AGENT_PLAN_REQUIRED]"* ]]; then
  echo "T1 failed: missing shared-plan bootstrap diagnostics" >&2
  exit 1
fi
if [[ "$T1_OUT" == *"stage=commit-runbook"* ]]; then
  echo "T1 failed: agent cpa invoked internal provider planning" >&2
  exit 1
fi
if [[ ! -f "$DEFAULT_PLAN_PATH" ]]; then
  echo "T1 failed: shared default plan was not created" >&2
  exit 1
fi
PLAN_PATH_ENV="$DEFAULT_PLAN_PATH" python - <<'PY'
import json
import os
from pathlib import Path
p = Path(os.environ["PLAN_PATH_ENV"])
j = json.loads(p.read_text(encoding="utf-8"))
assert str(j["meta"]["base_head_sha"]).startswith("ws-head-v2-")
assert str(j["meta"]["dirty_fingerprint"]).startswith("ws-dirty-v2-")
assert j["meta"]["planner"]["provider"] == "agent"
assert j["meta"]["review"]["verdict"] == "pass"
assert j["stages"]["commit"]
assert j["stages"]["commit"][0]["commits"][0]["message"] == "replace-with-commit-message"
PY

echo "[E2E] T2 plan new hash fields"
run_kog "$SANDBOX_REPO" plan new -f -o "$PLAN_PATH" >/dev/null
PLAN_PATH_ENV="$PLAN_PATH" python - <<'PY'
import json
import os
from pathlib import Path
p = Path(os.environ["PLAN_PATH_ENV"])
j = json.loads(p.read_text(encoding="utf-8"))
bh = str(j["meta"]["base_head_sha"])
df = str(j["meta"]["dirty_fingerprint"])
assert bh and not bh.startswith("replace-with-"), f"invalid base_head_sha: {bh}"
assert df and not df.startswith("replace-with-"), f"invalid dirty_fingerprint: {df}"
PY

echo "[E2E] T3 pre-apply drift rejection"
cp "$PLAN_PATH" "$MUT_PATH"
MUT_PATH_ENV="$MUT_PATH" python - <<'PY'
import re
import os
from pathlib import Path
p = Path(os.environ["MUT_PATH_ENV"])
t = p.read_text(encoding="utf-8")
t = re.sub(r'("base_head_sha"\s*:\s*")[^"]*(")', r'\1deadbeef\2', t, count=1)
p.write_text(t, encoding="utf-8")
PY
set +e
T3_OUT="$(run_kog "$SANDBOX_REPO" plan verify pre-apply --plan-file "$MUT_PATH")"
T3_EC=$?
set -e
if [[ $T3_EC -eq 0 ]]; then
  echo "T3 failed: expected non-zero exit" >&2
  exit 1
fi
if [[ "$T3_OUT" != *"workspace state drift detected"* ]]; then
  echo "T3 failed: missing drift message" >&2
  exit 1
fi

echo "[E2E] T4 agent mode cpa dry-run with message"
T4_OUT="$(KANO_AGENT_MODE=1 run_kog "$SANDBOX_REPO" cpa -m "test(e2e): dry-run smoke" --dry-run)"
if [[ "$T4_OUT" == *"--plan-file cannot be combined"* || "$T4_OUT" == *"default-plan.json"* ]]; then
  echo "T4 failed: explicit message was combined with the shared plan" >&2
  exit 1
fi

echo "E2E regression tests passed: T1 T2 T3 T4"
