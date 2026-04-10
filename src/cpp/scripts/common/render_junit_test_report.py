#!/usr/bin/env python3
from __future__ import annotations

import html
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def as_int(value: str | None) -> int:
    try:
        return int(float(value or "0"))
    except ValueError:
        return 0


def as_float(value: str | None) -> float:
    try:
        return float(value or "0")
    except ValueError:
        return 0.0


def collect_suites(root: ET.Element) -> list[ET.Element]:
    if root.tag == "testsuite":
        return [root]
    if root.tag == "testsuites":
        return [node for node in root.findall("testsuite")]
    return root.findall(".//testsuite")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: render_junit_test_report.py <junit-xml> <output-dir> <title>")

    junit_xml = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    title = sys.argv[3]
    output_dir.mkdir(parents=True, exist_ok=True)

    suites: list[dict] = []
    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0, "time": 0.0}

    if junit_xml.is_file():
        tree = ET.parse(junit_xml)
        for suite in collect_suites(tree.getroot()):
            name = suite.attrib.get("name", "unnamed")
            tests = as_int(suite.attrib.get("tests"))
            failures = as_int(suite.attrib.get("failures"))
            errors = as_int(suite.attrib.get("errors"))
            skipped = as_int(suite.attrib.get("skipped"))
            duration = as_float(suite.attrib.get("time"))
            passed = max(tests - failures - errors - skipped, 0)
            suites.append(
                {
                    "name": name,
                    "tests": tests,
                    "passed": passed,
                    "failures": failures,
                    "errors": errors,
                    "skipped": skipped,
                    "time": duration,
                }
            )
            totals["tests"] += tests
            totals["failures"] += failures
            totals["errors"] += errors
            totals["skipped"] += skipped
            totals["time"] += duration

    passed_total = max(totals["tests"] - totals["failures"] - totals["errors"] - totals["skipped"], 0)
    status = "Passed"
    if totals["failures"] or totals["errors"]:
        status = "Failed"
    elif totals["skipped"]:
        status = "Warnings"

    summary = {
        "title": title,
        "summary": f"{status}: {passed_total}/{totals['tests']} passed",
        "stats": [
            ["Total tests", str(totals["tests"])],
            ["Passed", str(passed_total)],
            ["Failures", str(totals["failures"])],
            ["Errors", str(totals["errors"])],
            ["Skipped", str(totals["skipped"])],
            ["Duration (s)", f"{totals['time']:.3f}"],
        ],
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    rows = "".join(
        f"<tr><td>{html.escape(suite['name'])}</td><td>{suite['tests']}</td><td>{suite['passed']}</td>"
        f"<td>{suite['failures']}</td><td>{suite['errors']}</td><td>{suite['skipped']}</td>"
        f"<td>{suite['time']:.3f}</td></tr>"
        for suite in suites
    ) or '<tr><td colspan="7">No JUnit suites found.</td></tr>'

    page = f"""<!doctype html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>{html.escape(title)}</title>
  <style>
    :root {{ color-scheme: dark; --bg: #0b1020; --panel: #121a31; --text: #edf2ff; --muted: #9fb0d8; --accent: #6a8cff; --border: #2a3557; }}
    body {{ margin: 0; font-family: Inter, Segoe UI, Arial, sans-serif; background: var(--bg); color: var(--text); }}
    main {{ max-width: 1100px; margin: 0 auto; padding: 32px 20px 48px; }}
    .summary {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; margin: 20px 0 28px; }}
    .card, table {{ background: var(--panel); border: 1px solid var(--border); border-radius: 12px; }}
    .card {{ padding: 14px 16px; }}
    .label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .08em; }}
    .value {{ font-size: 24px; font-weight: 700; margin-top: 8px; }}
    table {{ width: 100%; border-collapse: collapse; overflow: hidden; }}
    th, td {{ padding: 10px 12px; border-bottom: 1px solid var(--border); text-align: left; }}
    th {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .08em; }}
    tr:last-child td {{ border-bottom: 0; }}
    .footer {{ margin-top: 18px; color: var(--muted); font-size: 13px; }}
    a {{ color: var(--accent); }}
  </style>
</head>
<body>
  <main>
    <h1>{html.escape(title)}</h1>
    <p>{html.escape(summary['summary'])}</p>
    <div class=\"summary\">
      {''.join(f'<div class="card"><div class="label">{html.escape(label)}</div><div class="value">{html.escape(value)}</div></div>' for label, value in summary['stats'])}
    </div>
    <table>
      <thead><tr><th>Suite</th><th>Tests</th><th>Passed</th><th>Failures</th><th>Errors</th><th>Skipped</th><th>Duration (s)</th></tr></thead>
      <tbody>{rows}</tbody>
    </table>
    <p class=\"footer\">Source XML: <code>{html.escape(junit_xml.name)}</code></p>
  </main>
</body>
</html>
"""
    (output_dir / "index.html").write_text(page, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
