#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="${1:-$(cd "$SCRIPT_DIR/../../../../../../../../.." && pwd)}"
KOG_PATH="${2:-$WORKSPACE_ROOT/kog}"

if [[ ! -f "$KOG_PATH" ]]; then
  echo "Error: kog launcher not found: $KOG_PATH" >&2
  exit 1
fi

cd "$WORKSPACE_ROOT"

PLAN_DIR=".kano/cache/git/plans"
PLAN_PATH="$PLAN_DIR/e2e-regression-plan.json"
MUT_PATH="$PLAN_DIR/e2e-regression-plan-mutated.json"
mkdir -p "$PLAN_DIR"

run_kog() {
  local out
  out="$("$KOG_PATH" "$@" 2>&1)" || {
    local ec=$?
    printf '%s\n' "$out"
    return "$ec"
  }
  printf '%s\n' "$out"
  return 0
}

echo "[E2E] T1 agent guard"
set +e
T1_OUT="$(KANO_AGENT_MODE=1 run_kog cpa --dry-run)"
T1_EC=$?
set -e
if [[ $T1_EC -eq 0 ]]; then
  echo "T1 failed: expected non-zero exit" >&2
  exit 1
fi
if [[ "$T1_OUT" != *"requires either --plan-file or --message/-m"* ]]; then
  echo "T1 failed: missing guard message" >&2
  exit 1
fi

echo "[E2E] T2 plan new hash fields"
run_kog plan new -f -o "$PLAN_PATH" >/dev/null
python - <<'PY'
import json
from pathlib import Path
p = Path(".kano/cache/git/plans/e2e-regression-plan.json")
j = json.loads(p.read_text(encoding="utf-8"))
bh = str(j["meta"]["base_head_sha"])
df = str(j["meta"]["dirty_fingerprint"])
assert bh and not bh.startswith("replace-with-"), f"invalid base_head_sha: {bh}"
assert df and not df.startswith("replace-with-"), f"invalid dirty_fingerprint: {df}"
PY

echo "[E2E] T3 pre-apply drift rejection"
cp "$PLAN_PATH" "$MUT_PATH"
python - <<'PY'
import re
from pathlib import Path
p = Path(".kano/cache/git/plans/e2e-regression-plan-mutated.json")
t = p.read_text(encoding="utf-8")
t = re.sub(r'("base_head_sha"\s*:\s*")[^"]*(")', r'\1deadbeef\2', t, count=1)
p.write_text(t, encoding="utf-8")
PY
set +e
T3_OUT="$(run_kog plan verify pre-apply --plan-file "$MUT_PATH")"
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
KANO_AGENT_MODE=1 run_kog cpa -m "test(e2e): dry-run smoke" --dry-run >/dev/null

echo "E2E regression tests passed: T1 T2 T3 T4"

