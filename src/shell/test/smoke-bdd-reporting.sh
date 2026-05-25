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
import re

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

tests_root = Path(os.environ.get("KANO_GIT_TESTS_ROOT", Path.cwd() / "src" / "cpp" / "code" / "tests"))
source_ids: set[str] = set()
for cpp in tests_root.rglob("*.cpp"):
    text = cpp.read_text(encoding="utf-8", errors="ignore")
    source_ids.update(re.findall(r"scenario:(KOG-BDD-[A-Z]+-\d+)", text))
required_source = {
    "KOG-BDD-AI-001",
    "KOG-BDD-AI-002",
    "KOG-BDD-AI-003",
    "KOG-BDD-CONVERGE-001",
    "KOG-BDD-CONVERGE-002",
    "KOG-BDD-STATUS-001",
    "KOG-BDD-DISCOVERY-001",
}
missing_source = sorted(required_source - source_ids)
if missing_source:
    raise SystemExit(f"required source scenario tags missing: missing={missing_source} got={sorted(source_ids)}")
if not set(scenario_ids).issubset(source_ids):
    raise SystemExit(f"generated IDs are not subset of source-tag IDs: generated={scenario_ids} source={sorted(source_ids)}")

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

def assert_contains(path: Path, text: str) -> None:
    content = path.read_text(encoding="utf-8")
    if text not in content:
        raise SystemExit(f"expected {text!r} in {path}")

assert_contains(root / "bdd" / "index.md", "Auto-generated from executable metadata. Do not edit manually.")
assert_contains(root / "bdd" / "index.md", "[ai-provider-bootstrap](features/ai-provider-bootstrap.md)")
assert_contains(root / "bdd" / "bdd-summary.md", "KOG-BDD-AI-001")
assert_contains(root / "bdd" / "scenarios" / "KOG-BDD-AI-001.md", "- Scenario ID: `KOG-BDD-AI-001`")
assert_contains(root / "bdd" / "scenarios" / "KOG-BDD-AI-001.md", "- Feature: `ai-provider-bootstrap`")
assert_contains(root / "bdd" / "scenarios" / "KOG-BDD-AI-001.md", "- Title: AI bootstrap happy path")
assert_contains(root / "bdd" / "scenarios" / "KOG-BDD-AI-001.md", "## Given / When / Then")
assert_contains(root / "bdd" / "scenarios" / "KOG-BDD-AI-001.md", "- Status: `passed`")
assert_contains(root / "bdd" / "scenarios" / "KOG-BDD-AI-001.md", "- Mermaid diagram: `../diagrams/KOG-BDD-AI-001.mmd`")
assert_contains(root / "bdd" / "features" / "ai-provider-bootstrap.md", "KOG-BDD-AI-001")
assert_contains(root / "bdd" / "diagrams" / "KOG-BDD-AI-001.mmd", "flowchart TD")
assert_contains(root / "coverage" / "test-gap-backlog.md", "## BDD / functional suggestions")
assert_contains(root / "coverage-reports" / "demo" / "index.html", "Coverage: VALID")

highlights = json.loads((root / "bdd" / "feature-highlights-source.json").read_text(encoding="utf-8"))
highlighted_ids = {item.get("scenarioId") for item in highlights.get("highlights", [])}
if highlighted_ids:
    raise SystemExit(f"JUnit-derived non-featured scenarios leaked into highlights: {sorted(highlighted_ids)}")

tdd_bdd = json.loads((root / "bdd" / "tdd-bdd-summary.json").read_text(encoding="utf-8"))
artifacts = tdd_bdd.get("artifacts", {})
if artifacts.get("coverageHealth") != "VALID":
    raise SystemExit(f"expected VALID coverage health in report artifacts: {artifacts}")
if artifacts.get("bddDocsPath") != "bdd/index.md" or artifacts.get("mermaidDiagramCount", 0) < 1:
    raise SystemExit(f"expected BDD docs and Mermaid artifact fields: {artifacts}")
assert_contains(root / "bdd" / "tdd-bdd-summary.md", "## Report Artifacts")
assert_contains(root / "bdd" / "tdd-bdd-summary.md", "Coverage health")

print("BDD reporting smoke: PASS")
print("scenario_ids", scenario_ids)
print("source_tag_ids", sorted(source_ids))
PY
