#!/usr/bin/env python3
from __future__ import annotations

import html
import json
import sys
from pathlib import Path


def load_profile_json(tmp_root: Path, matrix_name: str) -> dict:
    path = tmp_root / matrix_name / "profile.json"
    if not path.is_file():
        raise SystemExit(f"profile artifact not found: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: render_profile_report.py <matrix.json> <tmp-root> <report-root>")

    matrix_path = Path(sys.argv[1])
    tmp_root = Path(sys.argv[2])
    report_root = Path(sys.argv[3])
    matrix_name = matrix_path.stem
    profile = load_profile_json(tmp_root, matrix_name)

    slug = str(profile.get("reportSlug") or matrix_name)
    out_dir = report_root / slug
    out_dir.mkdir(parents=True, exist_ok=True)

    profile_json_path = out_dir / "profile.json"
    profile_json_path.write_text(json.dumps(profile, indent=2), encoding="utf-8")

    lines = [f"# Profiling Report — {slug}", "", f"Host: `{profile.get('hostOs')}` / `{profile.get('hostArch')}`", ""]
    rows = []
    for case in profile.get("cases", []):
        baseline_rows = case.get("baselineRows") or []
        metrics = ", ".join(f"{row.get('case')}={row.get('elapsed_seconds')}s" for row in baseline_rows)
        lines.append(
            f"- **{case.get('id')}** — status=`{case.get('status')}` launcher=`{case.get('launcher')}` unity=`{case.get('unity')}` "
            f"modules=`{case.get('modules')}` pgo=`{case.get('pgo')}`"
        )
        if metrics:
            lines.append(f"  - metrics: {metrics}")
        rows.append(
            "<tr>"
            f"<td>{html.escape(str(case.get('id')))}</td>"
            f"<td>{html.escape(str(case.get('status')))}</td>"
            f"<td>{html.escape(str(case.get('launcher')))}</td>"
            f"<td>{html.escape(str(case.get('unity')))}</td>"
            f"<td>{html.escape(str(case.get('modules')))}</td>"
            f"<td>{html.escape(str(case.get('pgo')))}</td>"
            f"<td>{html.escape(metrics)}</td>"
            "</tr>"
        )

    markdown = "\n".join(lines) + "\n"
    (out_dir / "summary.md").write_text(markdown, encoding="utf-8")

    html_doc = f"""<!doctype html>
<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
<title>Profiling Report — {html.escape(slug)}</title>
<style>
body {{ font-family: Inter, Segoe UI, Arial, sans-serif; margin: 0; background: #0b1020; color: #edf2ff; }}
main {{ max-width: 1200px; margin: 0 auto; padding: 24px; }}
table {{ width: 100%; border-collapse: collapse; background: #121a31; border: 1px solid #2a3557; }}
th, td {{ border-bottom: 1px solid #2a3557; padding: 10px 12px; text-align: left; vertical-align: top; }}
th {{ color: #9fb0d8; font-size: 12px; text-transform: uppercase; }}
code {{ color: #6a8cff; }}
</style></head><body><main>
<h1>Profiling Report — {html.escape(slug)}</h1>
<p>Host: <code>{html.escape(str(profile.get('hostOs')))}</code> / <code>{html.escape(str(profile.get('hostArch')))}</code></p>
<table><thead><tr><th>Case</th><th>Status</th><th>Launcher</th><th>Unity</th><th>Modules</th><th>PGO</th><th>Metrics</th></tr></thead><tbody>
{''.join(rows)}
</tbody></table>
</main></body></html>"""
    (out_dir / "index.html").write_text(html_doc, encoding="utf-8")
    print(out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
