#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
REPORT_ROOT="${KOG_BDD_SMOKE_REPORT_ROOT:-${ROOT_DIR}/.kano/tmp/kog-bdd-smoke}"
SKILL_ROOT_DEFAULT="${ROOT_DIR}/../kano-cpp-test-skill"

if [[ -z "${KANO_CPP_TEST_SKILL_ROOT:-}" ]]; then
  export KANO_CPP_TEST_SKILL_ROOT="$SKILL_ROOT_DEFAULT"
fi

FIXTURE_XML="${KANO_CPP_TEST_SKILL_ROOT}/fixtures/bdd-schema/test-reports/demo/tests.xml"

rm -rf "$REPORT_ROOT"
mkdir -p "$REPORT_ROOT/test-reports/demo" "$REPORT_ROOT/coverage-reports/demo" "$REPORT_ROOT/raw/bdd-metadata"
cp -f "$FIXTURE_XML" "$REPORT_ROOT/test-reports/demo/tests.xml"

cat > "$REPORT_ROOT/coverage-reports/demo/cobertura.xml" <<'XML'
<?xml version='1.0'?><coverage line-rate='1' branch-rate='0' lines-covered='1' lines-valid='1' branches-covered='0' branches-valid='0'><packages><package name='Demo' line-rate='1' branch-rate='0'><classes><class name='Demo.cpp' filename='src/Demo.cpp' line-rate='1' branch-rate='0'/></classes></package></packages></coverage>
XML

cat > "$REPORT_ROOT/coverage-reports/demo/summary.txt" <<'TXT'
line-coverage-percent=100
branch-coverage-percent=0
lines-covered=1
lines-valid=1
branches-covered=0
branches-valid=0
TXT

python "$ROOT_DIR/src/cpp/shared/infra/scripts/tools/generate-bdd-metadata-from-junit.py" \
  "$REPORT_ROOT/test-reports/demo/tests.xml" \
  "$REPORT_ROOT/raw/bdd-metadata" \
  "kano_git_cli_tests"

export KANO_REPORT_ROOT="$REPORT_ROOT"
export KANO_REPORT_SLUG="demo"
export KANO_TEST_XML="$REPORT_ROOT/test-reports/demo/tests.xml"
export KANO_COVERAGE_XML="$REPORT_ROOT/coverage-reports/demo/cobertura.xml"
export KANO_COVERAGE_REPORT_DIR="$REPORT_ROOT/coverage-reports/demo"

bash "$ROOT_DIR/src/cpp/shared/infra/scripts/lib/package-reports-with-skill.sh"

python - <<'PY'
from pathlib import Path
import json
import os

root = Path(os.environ["KANO_REPORT_ROOT"])
summary = json.loads((root / "bdd" / "bdd-summary.json").read_text(encoding="utf-8"))
scenario_ids = sorted({item.get("scenarioId", "") for item in summary.get("scenarios", []) if item.get("scenarioId", "").startswith("KOG-BDD-")})
expected = [
    "KOG-BDD-AI-001",
    "KOG-BDD-CONVERGE-001",
    "KOG-BDD-DISCOVERY-001",
    "KOG-BDD-STATUS-001",
]
if scenario_ids != expected:
    raise SystemExit(f"scenario id mismatch: expected={expected} got={scenario_ids}")

required = [
    root / "bdd" / "index.md",
    root / "bdd" / "bdd-summary.md",
    root / "bdd" / "feature-highlights-source.json",
    root / "bdd" / "feature-highlights-source.md",
    root / "bdd" / "tdd-bdd-summary.json",
    root / "bdd" / "tdd-bdd-summary.md",
    root / "bdd" / "diagrams" / "KOG-BDD-AI-001.mmd",
]
missing = [str(p) for p in required if not p.is_file()]
if missing:
    raise SystemExit(f"missing outputs: {missing}")

print("BDD reporting smoke: PASS")
print("scenario_ids", scenario_ids)
PY
