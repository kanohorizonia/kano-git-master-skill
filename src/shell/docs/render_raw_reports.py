#!/usr/bin/env python3
from __future__ import annotations

import html
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath
from typing import Iterable


PAGE_STYLE = """
:root {
  color-scheme: light;
  --bg: #f4f1ea;
  --card: #fffdfa;
  --ink: #1f2a2e;
  --muted: #5b676d;
  --line: #d7cebf;
  --accent: #7d4e31;
  --accent-soft: #efe0d4;
  --good: #2f6b3c;
  --warn: #8a5a00;
  --bad: #8e2c2c;
}

* { box-sizing: border-box; }
body {
  margin: 0;
  font-family: "Segoe UI", "Helvetica Neue", sans-serif;
  color: var(--ink);
  background: linear-gradient(180deg, #ece4d8 0%, var(--bg) 100%);
}
main {
  max-width: 1180px;
  margin: 0 auto;
  padding: 40px 20px 72px;
}
h1, h2, h3, p { margin-top: 0; }
a { color: var(--accent); }
.lede { color: var(--muted); line-height: 1.7; max-width: 860px; }
.card {
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: 18px;
  box-shadow: 0 14px 30px rgba(44, 35, 20, 0.08);
}
.hero { padding: 24px; margin-bottom: 24px; }
.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 16px;
  margin: 24px 0;
}
.stat { padding: 18px; }
.eyebrow {
  font-size: 0.78rem;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--muted);
  margin-bottom: 8px;
}
.value { font-size: 2rem; font-weight: 700; }
.good { color: var(--good); }
.warn { color: var(--warn); }
.bad { color: var(--bad); }
.table-card { padding: 18px 18px 12px; margin-top: 20px; overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-size: 0.96rem; }
th, td { padding: 10px 12px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; }
th { font-size: 0.84rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--muted); }
code, pre { font-family: "Cascadia Code", Consolas, monospace; }
.mono { font-family: "Cascadia Code", Consolas, monospace; font-size: 0.93em; }
.pill {
  display: inline-block;
  padding: 4px 10px;
  border-radius: 999px;
  background: var(--accent-soft);
  color: var(--accent);
  font-size: 0.8rem;
}
.note {
  margin-top: 18px;
  padding: 16px 18px;
  border-left: 6px solid var(--accent);
  background: var(--accent-soft);
  border-radius: 16px;
}
.source-table { width: 100%; border-collapse: collapse; font-size: 0.95rem; }
.source-table th { position: sticky; top: 0; background: #f7f2ea; z-index: 2; }
.source-table td { padding: 0; border-bottom: 1px solid var(--line); vertical-align: top; }
.line-no, .line-hits {
  width: 88px;
  padding: 0.2rem 0.7rem;
  text-align: right;
  color: var(--muted);
  background: #f7f2ea;
  border-right: 1px solid var(--line);
  font-family: "Cascadia Code", Consolas, monospace;
}
.line-code {
  padding: 0.2rem 0.85rem;
  font-family: "Cascadia Code", Consolas, monospace;
  white-space: pre;
}
.line-covered .line-code { background: #e7f5ea; }
.line-missed .line-code { background: #fae5e5; }
.line-neutral .line-code { background: #fffdfa; }
.line-diff .line-code { background: #fff1cc; }
.line-diff .line-hits { color: var(--warn); }
.line-covered .line-hits { color: var(--good); }
.line-missed .line-hits { color: var(--bad); }
.legend { display: flex; gap: 12px; flex-wrap: wrap; margin: 10px 0 0; color: var(--muted); }
.legend span { display: inline-flex; align-items: center; gap: 8px; }
.swatch { width: 14px; height: 14px; border-radius: 4px; border: 1px solid var(--line); }
.swatch.covered { background: #e7f5ea; }
.swatch.missed { background: #fae5e5; }
.swatch.neutral { background: #fffdfa; }
.toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  align-items: center;
  margin: 14px 0 0;
}
.toolbar input, .toolbar select {
  padding: 0.65rem 0.8rem;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: #fffdfa;
  color: var(--ink);
  min-width: 220px;
}
.toolbar button {
  padding: 0.6rem 0.85rem;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: #fffdfa;
  color: var(--ink);
  cursor: pointer;
}
.toolbar button.active { background: var(--accent-soft); color: var(--accent); }
.muted { color: var(--muted); }
.small { font-size: 0.92rem; }
ul.inline-list { margin: 8px 0 0; padding-left: 18px; line-height: 1.7; color: var(--muted); }
""".strip()


@dataclass
class JUnitCase:
    classname: str
    name: str
    time: str
    failed: bool
    errored: bool
    skipped: bool
    details: str


@dataclass
class JUnitSuite:
    name: str
    tests: int
    failures: int
    errors: int
    skipped: int
    time: str


@dataclass
class JUnitReport:
    slug: str
    xml_path: Path
    href: str
    tests: int
    failures: int
    errors: int
    skipped: int
    duration: str
    suites: list[JUnitSuite]
    interesting_cases: list[JUnitCase]


@dataclass
class CoverageFile:
    filename: str
    display_name: str
    line_rate: float
    branch_rate: float
    covered_lines: int
    valid_lines: int
    missed_lines: int
    line_hits: dict[int, int]
    report_href: str
    source_path: str


@dataclass
class CoberturaReport:
    slug: str
    xml_path: Path
    line_rate: float
    branch_rate: float
    lines_covered: int
    lines_valid: int
    files: list[CoverageFile]


@dataclass
class CoverageSummary:
    slug: str
    href: str
    line_coverage: str
    branch_coverage: str
    notes: str


@dataclass
class CoverageComparison:
    display_name: str
    source_path: str
    microsoft: CoverageFile
    opencpp: CoverageFile
    compare_href: str


def page(title: str, body: str) -> str:
    return f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"UTF-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>{html.escape(title)}</title>
  <style>{PAGE_STYLE}</style>
</head>
<body>
  <main>
{body}
  </main>
</body>
</html>
"""


def pct(value: float) -> str:
    return f"{value * 100:.2f}%"


def read_xml(path: Path) -> ET.Element:
    text = path.read_text(encoding="utf-8-sig")
    return ET.fromstring(text)


def find_first(paths: Iterable[Path]) -> Path | None:
    for path in paths:
        if path.exists():
            return path
    return None


def parse_int(value: str | None, default: int = 0) -> int:
    if not value:
        return default
    try:
        return int(value)
    except ValueError:
        try:
            return int(float(value))
        except ValueError:
            return default


def normalize_source_path(raw_filename: str, source_root: str) -> str:
    normalized = raw_filename.replace("/", "\\")
    if re.match(r"^[A-Za-z]:\\", normalized):
        return str(PureWindowsPath(normalized))
    if normalized.startswith("\\"):
        return normalized
    if source_root:
        root = source_root.replace("/", "\\")
        if root.endswith("\\"):
            return root + normalized.lstrip("\\")
        return root + "\\" + normalized.lstrip("\\")
    return normalized


def display_source_path(source_path: str, repo_root: Path) -> str:
    try:
        relative = Path(source_path).resolve().relative_to(repo_root.resolve())
        return relative.as_posix()
    except Exception:
        return source_path


def source_report_href(display_name: str) -> str:
    safe = display_name.replace(":", "").replace("\\", "/")
    safe = safe.replace("..", "__")
    return f"files/{safe}.html"


def compare_report_href(display_name: str) -> str:
    safe = display_name.replace(":", "").replace("\\", "/")
    safe = safe.replace("..", "__")
    return f"files/{safe}.compare.html"


def parse_junit_report(slug: str, xml_path: Path, href: str) -> JUnitReport:
    root = read_xml(xml_path)
    suites: list[JUnitSuite] = []
    cases: list[JUnitCase] = []

    suite_nodes = root.findall("testsuite") if root.tag == "testsuites" else [root]
    total_tests = total_failures = total_errors = total_skipped = 0
    duration = 0.0

    for suite in suite_nodes:
        tests = parse_int(suite.get("tests"))
        failures = parse_int(suite.get("failures"))
        errors = parse_int(suite.get("errors"))
        skipped = parse_int(suite.get("skipped"))
        suite_time = suite.get("time") or "0"
        total_tests += tests
        total_failures += failures
        total_errors += errors
        total_skipped += skipped
        try:
            duration += float(suite_time)
        except ValueError:
            pass
        suites.append(
            JUnitSuite(
                name=suite.get("name") or slug,
                tests=tests,
                failures=failures,
                errors=errors,
                skipped=skipped,
                time=suite_time,
            )
        )

        for case in suite.findall("testcase"):
            failure = case.find("failure")
            error = case.find("error")
            skipped_node = case.find("skipped")
            details_parts = []
            for node in (failure, error, skipped_node):
                if node is None:
                    continue
                text = (node.text or "").strip()
                message = (node.get("message") or "").strip()
                piece = "\n".join(part for part in (message, text) if part)
                if piece:
                    details_parts.append(piece)
            is_interesting = failure is not None or error is not None or skipped_node is not None
            if is_interesting or len(cases) < 80:
                cases.append(
                    JUnitCase(
                        classname=case.get("classname") or "",
                        name=case.get("name") or "",
                        time=case.get("time") or "0",
                        failed=failure is not None,
                        errored=error is not None,
                        skipped=skipped_node is not None,
                        details="\n\n".join(details_parts),
                    )
                )

    interesting = [c for c in cases if c.failed or c.errored or c.skipped]
    if not interesting:
        interesting = cases[:80]

    return JUnitReport(
        slug=slug,
        xml_path=xml_path,
        href=href,
        tests=total_tests,
        failures=total_failures,
        errors=total_errors,
        skipped=total_skipped,
        duration=f"{duration:.3f}s",
        suites=suites,
        interesting_cases=interesting,
    )


def render_junit_platform(report: JUnitReport) -> str:
    suite_rows = "\n".join(
        f"<tr><td class=\"mono\">{html.escape(s.name)}</td><td>{s.tests}</td><td>{s.failures}</td><td>{s.errors}</td><td>{s.skipped}</td><td>{html.escape(s.time)}s</td></tr>"
        for s in report.suites
    )
    case_rows = "\n".join(
        (
            "<tr>"
            f"<td class=\"mono\">{html.escape(c.classname)}</td>"
            f"<td>{html.escape(c.name)}</td>"
            f"<td>{'failed' if c.failed else 'error' if c.errored else 'skipped' if c.skipped else 'passed sample'}</td>"
            f"<td>{html.escape(c.time)}s</td>"
            f"<td><pre>{html.escape(c.details)}</pre></td>"
            "</tr>"
        )
        for c in report.interesting_cases
    )
    note = "Only failing, errored, skipped, or a small sample of passing cases are expanded here to keep the page responsive. Use the raw XML for the full testcase list."
    return page(
        f"{report.slug} Test Report",
        f"""
<section class=\"card hero\">
  <span class=\"pill\">JUnit XML</span>
  <h1>{html.escape(report.slug)} Test Report</h1>
  <p class=\"lede\">Generated from <code>{html.escape(report.href)}</code>. This page summarizes suite-level counts and surfaces the most interesting testcase details directly in HTML.</p>
</section>
<section class=\"grid\">
  <div class=\"card stat\"><div class=\"eyebrow\">Tests</div><div class=\"value good\">{report.tests:,}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Failures</div><div class=\"value {'bad' if report.failures else 'good'}\">{report.failures}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Errors</div><div class=\"value {'bad' if report.errors else 'good'}\">{report.errors}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Skipped</div><div class=\"value {'warn' if report.skipped else 'good'}\">{report.skipped}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Duration</div><div class=\"value\">{html.escape(report.duration)}</div></div>
</section>
<section class=\"card table-card\">
  <h2>Suite Summary</h2>
  <table>
    <thead><tr><th>Suite</th><th>Tests</th><th>Failures</th><th>Errors</th><th>Skipped</th><th>Time</th></tr></thead>
    <tbody>{suite_rows}</tbody>
  </table>
</section>
<section class=\"card table-card\">
  <h2>Interesting Testcases</h2>
  <p class=\"muted small\">{html.escape(note)}</p>
  <table>
    <thead><tr><th>Class</th><th>Testcase</th><th>Status</th><th>Time</th><th>Details</th></tr></thead>
    <tbody>{case_rows or '<tr><td colspan="5">No testcase details available.</td></tr>'}</tbody>
  </table>
</section>
<div class=\"note\"><strong>Raw artifact:</strong> <a href=\"{html.escape(report.href)}\">{html.escape(report.href)}</a></div>
""",
    )


def render_junit_index(reports: list[JUnitReport]) -> str:
    cards = []
    for report in sorted(reports, key=lambda item: item.slug):
        badge = "0 failures" if report.failures == 0 and report.errors == 0 else f"{report.failures + report.errors} issues"
        cards.append(
            f"""
<a class=\"card stat\" href=\"{html.escape(report.slug)}/index.html\">
  <div class=\"eyebrow\">{html.escape(report.slug)}</div>
  <div class=\"value good\">{report.tests:,}</div>
  <p class=\"muted small\">tests, {badge}, {html.escape(report.duration)}</p>
</a>
"""
        )
    return page(
        "Raw Test Reports",
        f"""
<section class=\"card hero\">
  <span class=\"pill\">Generated index</span>
  <h1>Raw Test Reports</h1>
  <p class=\"lede\">These pages are generated from the published JUnit XML artifacts. Each platform page shows suite-level metrics and the interesting testcase details pulled from XML at build time.</p>
</section>
<section class=\"grid\">{''.join(cards)}</section>
""",
    )


def parse_cobertura(slug: str, xml_path: Path, repo_root: Path) -> CoberturaReport:
    root = read_xml(xml_path)
    source_root = ""
    source_node = root.find("./sources/source")
    if source_node is not None and source_node.text:
        source_root = source_node.text.strip()
    file_map: dict[str, dict[str, object]] = {}
    for class_node in root.findall("./packages/package/classes/class"):
        filename = class_node.get("filename") or class_node.get("name") or "(unknown)"
        source_path = normalize_source_path(filename, source_root)
        if "_deps" in filename or "\\out\\obj\\" in filename or "/out/obj/" in filename:
            continue
        lines = class_node.findall("./lines/line")
        entry = file_map.setdefault(
            source_path,
            {
                "lines": {},
                "branch_rates": [],
                "filename": source_path,
            },
        )
        line_hits = entry["lines"]
        assert isinstance(line_hits, dict)
        for line in lines:
            number = parse_int(line.get("number"), -1)
            if number < 0:
                continue
            hits = parse_int(line.get("hits"))
            current = line_hits.get(number, 0)
            line_hits[number] = max(current, hits)
        branch_rates = entry["branch_rates"]
        assert isinstance(branch_rates, list)
        branch_rates.append(float(class_node.get("branch-rate") or 0.0))

    files: list[CoverageFile] = []
    for filename, entry in file_map.items():
        line_hits = entry["lines"]
        branch_rates = entry["branch_rates"]
        assert isinstance(line_hits, dict)
        assert isinstance(branch_rates, list)
        valid_lines = len(line_hits)
        covered_lines = sum(1 for hits in line_hits.values() if hits > 0)
        line_rate = covered_lines / valid_lines if valid_lines else 0.0
        branch_rate = sum(branch_rates) / len(branch_rates) if branch_rates else 0.0
        display_name = display_source_path(filename, repo_root)
        files.append(
            CoverageFile(
                filename=filename,
                display_name=display_name,
                line_rate=line_rate,
                branch_rate=branch_rate,
                covered_lines=covered_lines,
                valid_lines=valid_lines,
                missed_lines=max(valid_lines - covered_lines, 0),
                line_hits=dict(sorted(line_hits.items())),
                report_href=source_report_href(display_name),
                source_path=filename,
            )
        )
    files.sort(key=lambda item: (item.line_rate, item.filename.lower()))
    return CoberturaReport(
        slug=slug,
        xml_path=xml_path,
        line_rate=float(root.get("line-rate") or 0.0),
        branch_rate=float(root.get("branch-rate") or 0.0),
        lines_covered=parse_int(root.get("lines-covered")),
        lines_valid=parse_int(root.get("lines-valid")),
        files=files,
    )


def render_cobertura_platform(report: CoberturaReport) -> str:
    compare_link = ""
    sibling_slug = f"{report.slug}-opencppcoverage"
    sibling_dir = report.xml_path.parent.parent / sibling_slug
    if sibling_dir.exists():
        compare_link = f'<div class="note"><strong>Compare:</strong> <a href="../{html.escape(sibling_slug)}/index.html">OpenCppCoverage HTML report</a></div>'
    rows = "\n".join(
        f"<tr data-file-path=\"{html.escape(item.display_name.lower())}\" data-line-rate=\"{item.line_rate:.6f}\" data-missed-lines=\"{item.missed_lines}\"><td class=\"mono\"><a href=\"{html.escape(item.report_href)}\">{html.escape(item.display_name)}</a></td><td>{pct(item.line_rate)}</td><td>{pct(item.branch_rate)}</td><td>{item.covered_lines}</td><td>{item.valid_lines}</td><td>{item.missed_lines}</td></tr>"
        for item in report.files[:400]
    )
    toolbar = """
<div class=\"toolbar\">
  <input id=\"coverage-file-search\" type=\"search\" placeholder=\"Search file path...\" aria-label=\"Search files\">
  <select id=\"coverage-file-filter\" aria-label=\"Coverage filter\">
    <option value=\"all\">All files</option>
    <option value=\"low\">Below 50% line coverage</option>
    <option value=\"partial\">Has missed executable lines</option>
    <option value=\"full\">100% line coverage</option>
  </select>
  <select id=\"coverage-file-sort\" aria-label=\"Coverage sort\">
    <option value=\"path\">Sort by path</option>
    <option value=\"line-rate-asc\">Line coverage asc</option>
    <option value=\"line-rate-desc\">Line coverage desc</option>
    <option value=\"missed-desc\">Missed lines desc</option>
  </select>
</div>
<script>
document.addEventListener('DOMContentLoaded', () => {
  const search = document.getElementById('coverage-file-search');
  const filter = document.getElementById('coverage-file-filter');
  const sort = document.getElementById('coverage-file-sort');
  const tbody = document.querySelector('tbody');
  const rows = Array.from(document.querySelectorAll('tbody tr[data-file-path]'));
  const applyFilters = () => {
    const query = (search.value || '').toLowerCase().trim();
    const mode = filter.value;
    rows.forEach((row) => {
      const path = row.dataset.filePath || '';
      const lineRate = Number(row.dataset.lineRate || '0');
      const missed = Number(row.dataset.missedLines || '0');
      const matchesQuery = !query || path.includes(query);
      let matchesMode = true;
      if (mode === 'low') matchesMode = lineRate < 0.5;
      if (mode === 'partial') matchesMode = missed > 0;
      if (mode === 'full') matchesMode = lineRate >= 0.999999;
      row.style.display = matchesQuery && matchesMode ? '' : 'none';
    });
  };
  const applySort = () => {
    const mode = sort.value;
    const sorted = [...rows].sort((a, b) => {
      if (mode === 'line-rate-asc') return Number(a.dataset.lineRate || '0') - Number(b.dataset.lineRate || '0');
      if (mode === 'line-rate-desc') return Number(b.dataset.lineRate || '0') - Number(a.dataset.lineRate || '0');
      if (mode === 'missed-desc') return Number(b.dataset.missedLines || '0') - Number(a.dataset.missedLines || '0');
      return (a.dataset.filePath || '').localeCompare(b.dataset.filePath || '');
    });
    sorted.forEach((row) => tbody.appendChild(row));
  };
  search.addEventListener('input', applyFilters);
  filter.addEventListener('change', applyFilters);
  sort.addEventListener('change', applySort);
  applySort();
  applyFilters();
});
</script>
"""
    return page(
        f"{report.slug} Coverage Report",
        f"""
<section class=\"card hero\">
  <span class=\"pill\">Cobertura XML</span>
  <h1>{html.escape(report.slug)} Coverage Report</h1>
  <p class=\"lede\">Generated from <code>{html.escape(report.xml_path.name)}</code>. This is a static HTML view of the native Windows coverage export, so the report stays human-readable even when the original tool only provides XML or <code>.coverage</code> artifacts.</p>
</section>
<section class=\"grid\">
  <div class=\"card stat\"><div class=\"eyebrow\">Line Coverage</div><div class=\"value good\">{pct(report.line_rate)}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Branch Coverage</div><div class=\"value {'good' if report.branch_rate >= 0.5 else 'warn'}\">{pct(report.branch_rate)}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Lines Covered</div><div class=\"value\">{report.lines_covered:,}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Lines Valid</div><div class=\"value\">{report.lines_valid:,}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Files</div><div class=\"value\">{len(report.files):,}</div></div>
</section>
<section class=\"card table-card\">
  <h2>Files</h2>
  <p class=\"muted small\">Showing up to 400 files sorted by lowest line coverage first. Click a file to open the annotated source view.</p>
  {toolbar}
  <table>
    <thead><tr><th>File</th><th>Line Coverage</th><th>Branch Coverage</th><th>Covered</th><th>Valid</th><th>Missed</th></tr></thead>
    <tbody>{rows}</tbody>
  </table>
</section>
<div class=\"note\"><strong>Raw artifacts:</strong> <a href=\"coverage.cobertura.xml\">coverage.cobertura.xml</a>, <a href=\"coverage.xml\">coverage.xml</a>, <a href=\"windows.coverage\">windows.coverage</a></div>
{compare_link}
""",
    )


def render_source_file_page(platform_slug: str, coverage_file: CoverageFile) -> str:
    source_path = Path(coverage_file.source_path)
    source_exists = source_path.exists()
    rows = []
    if source_exists:
        lines = source_path.read_text(encoding="utf-8", errors="replace").splitlines()
        total_lines = max(len(lines), max(coverage_file.line_hits.keys(), default=0))
        for line_no in range(1, total_lines + 1):
            code = lines[line_no - 1] if line_no - 1 < len(lines) else ""
            hits = coverage_file.line_hits.get(line_no)
            line_class = "line-neutral"
            hit_text = ""
            if hits is not None:
                if hits > 0:
                    line_class = "line-covered"
                    hit_text = str(hits)
                else:
                    line_class = "line-missed"
                    hit_text = "0"
            row_mode = "neutral"
            if hits is not None and hits > 0:
                row_mode = "covered"
            elif hits is not None and hits == 0:
                row_mode = "missed"
            rows.append(
                f"<tr class=\"{line_class}\" data-line-mode=\"{row_mode}\"><td class=\"line-no\">{line_no}</td><td class=\"line-hits\">{hit_text}</td><td class=\"line-code\">{html.escape(code) if code else '&nbsp;'}</td></tr>"
            )
    else:
        rows.append("<tr><td colspan=\"3\">Source file is not available in this workspace, so only aggregate metrics can be shown.</td></tr>")

    return page(
        f"{platform_slug} - {coverage_file.display_name}",
        f"""
<section class=\"card hero\">
  <span class=\"pill\">Annotated source</span>
  <h1>{html.escape(coverage_file.display_name)}</h1>
  <p class=\"lede\">This page is generated from Cobertura line-hit data and the checked-out source file. Hits are shown per line so the static renderer can approach the same browsing experience as OpenCppCoverage.</p>
  <div class=\"legend\">
    <span><i class=\"swatch covered\"></i> covered line</span>
    <span><i class=\"swatch missed\"></i> executable but missed</span>
    <span><i class=\"swatch neutral\"></i> no executable hit data</span>
  </div>
  <div class=\"toolbar\">
    <button type=\"button\" class=\"active\" data-line-filter=\"all\">All lines</button>
    <button type=\"button\" data-line-filter=\"covered\">Only covered</button>
    <button type=\"button\" data-line-filter=\"missed\">Only missed</button>
    <button type=\"button\" data-line-filter=\"relevant\">Covered + missed</button>
    <button type=\"button\" id=\"jump-next-missed\">Next missed</button>
  </div>
</section>
<section class=\"grid\">
  <div class=\"card stat\"><div class=\"eyebrow\">Line Coverage</div><div class=\"value good\">{pct(coverage_file.line_rate)}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Branch Coverage</div><div class=\"value {'good' if coverage_file.branch_rate >= 0.5 else 'warn'}\">{pct(coverage_file.branch_rate)}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Covered</div><div class=\"value\">{coverage_file.covered_lines}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Missed</div><div class=\"value\">{coverage_file.missed_lines}</div></div>
</section>
<section class=\"card table-card\">
  <p class=\"muted small\"><a href=\"../index.html\">Back to {html.escape(platform_slug)} coverage summary</a></p>
  <table class=\"source-table\"><tbody>{''.join(rows)}</tbody></table>
</section>
<script>
document.addEventListener('DOMContentLoaded', () => {{
  const buttons = Array.from(document.querySelectorAll('[data-line-filter]'));
  const rows = Array.from(document.querySelectorAll('tr[data-line-mode]'));
  let currentMissedIndex = -1;
  const applyMode = (mode) => {{
    buttons.forEach((button) => button.classList.toggle('active', button.dataset.lineFilter === mode));
    rows.forEach((row) => {{
      const lineMode = row.dataset.lineMode || 'neutral';
      let visible = true;
      if (mode === 'covered') visible = lineMode === 'covered';
      if (mode === 'missed') visible = lineMode === 'missed';
      if (mode === 'relevant') visible = lineMode === 'covered' || lineMode === 'missed';
      row.style.display = visible ? '' : 'none';
    }});
  }};
  buttons.forEach((button) => button.addEventListener('click', () => applyMode(button.dataset.lineFilter)));
  const jumpButton = document.getElementById('jump-next-missed');
  if (jumpButton) {{
    jumpButton.addEventListener('click', () => {{
      const missedRows = rows.filter((row) => row.dataset.lineMode === 'missed' && row.style.display !== 'none');
      if (!missedRows.length) return;
      currentMissedIndex = (currentMissedIndex + 1) % missedRows.length;
      missedRows[currentMissedIndex].scrollIntoView({{ behavior: 'smooth', block: 'center' }});
    }});
  }}
  applyMode('all');
}});
</script>
""",
    )


def write_cobertura_file_pages(platform_dir: Path, report: CoberturaReport, platform_slug: str) -> None:
    for coverage_file in report.files:
        write(platform_dir / coverage_file.report_href, render_source_file_page(platform_slug, coverage_file))


def build_coverage_comparisons(microsoft: CoberturaReport, opencpp: CoberturaReport) -> list[CoverageComparison]:
    opencpp_map = {item.display_name: item for item in opencpp.files}
    comparisons: list[CoverageComparison] = []
    for item in microsoft.files:
        sibling = opencpp_map.get(item.display_name)
        if sibling is None:
            continue
        comparisons.append(
            CoverageComparison(
                display_name=item.display_name,
                source_path=item.source_path,
                microsoft=item,
                opencpp=sibling,
                compare_href=compare_report_href(item.display_name),
            )
        )
    comparisons.sort(key=lambda entry: abs(entry.microsoft.line_rate - entry.opencpp.line_rate), reverse=True)
    return comparisons


def render_compare_file_page(comparison: CoverageComparison) -> str:
    source_path = Path(comparison.source_path)
    source_exists = source_path.exists()
    rows = []
    if source_exists:
        lines = source_path.read_text(encoding="utf-8", errors="replace").splitlines()
        max_line = max(len(lines), max(comparison.microsoft.line_hits.keys(), default=0), max(comparison.opencpp.line_hits.keys(), default=0))
        for line_no in range(1, max_line + 1):
            code = lines[line_no - 1] if line_no - 1 < len(lines) else ""
            m_hits = comparison.microsoft.line_hits.get(line_no)
            o_hits = comparison.opencpp.line_hits.get(line_no)
            line_class = "line-neutral"
            line_mode = "neutral"
            if m_hits != o_hits and (m_hits is not None or o_hits is not None):
                line_class = "line-diff"
                line_mode = "diff"
            elif (m_hits or 0) > 0 or (o_hits or 0) > 0:
                line_class = "line-covered"
                line_mode = "covered"
            elif m_hits == 0 or o_hits == 0:
                line_class = "line-missed"
                line_mode = "missed"
            rows.append(
                f"<tr class=\"{line_class}\" data-line-mode=\"{line_mode}\"><td class=\"line-no\">{line_no}</td><td class=\"line-hits\">{'' if m_hits is None else m_hits}</td><td class=\"line-hits\">{'' if o_hits is None else o_hits}</td><td class=\"line-code\">{html.escape(code) if code else '&nbsp;'}</td></tr>"
            )
    else:
        rows.append("<tr><td colspan=\"4\">Source file is not available in this workspace.</td></tr>")

    return page(
        f"Compare - {comparison.display_name}",
        f"""
<section class=\"card hero\">
  <span class=\"pill\">Same-file compare</span>
  <h1>{html.escape(comparison.display_name)}</h1>
  <p class=\"lede\">Side-by-side line hit comparison for Microsoft native coverage and OpenCppCoverage.</p>
  <div class=\"toolbar\">
    <button type=\"button\" class=\"active\" data-line-filter=\"all\">All lines</button>
    <button type=\"button\" data-line-filter=\"diff\">Only diffs</button>
    <button type=\"button\" data-line-filter=\"relevant\">Relevant lines</button>
    <button type=\"button\" id=\"jump-next-diff\">Next diff</button>
  </div>
</section>
<section class=\"grid\">
  <div class=\"card stat\"><div class=\"eyebrow\">Microsoft Line</div><div class=\"value good\">{pct(comparison.microsoft.line_rate)}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">OpenCpp Line</div><div class=\"value good\">{pct(comparison.opencpp.line_rate)}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">Microsoft Missed</div><div class=\"value\">{comparison.microsoft.missed_lines}</div></div>
  <div class=\"card stat\"><div class=\"eyebrow\">OpenCpp Missed</div><div class=\"value\">{comparison.opencpp.missed_lines}</div></div>
</section>
<section class=\"card table-card\">
  <p class=\"muted small\"><a href=\"../index.html\">Back to Windows compare summary</a></p>
  <table class=\"source-table\"><thead><tr><th>Line</th><th>Microsoft</th><th>OpenCpp</th><th>Code</th></tr></thead><tbody>{''.join(rows)}</tbody></table>
</section>
<script>
document.addEventListener('DOMContentLoaded', () => {{
  const buttons = Array.from(document.querySelectorAll('[data-line-filter]'));
  const rows = Array.from(document.querySelectorAll('tr[data-line-mode]'));
  let currentDiffIndex = -1;
  const applyMode = (mode) => {{
    buttons.forEach((button) => button.classList.toggle('active', button.dataset.lineFilter === mode));
    rows.forEach((row) => {{
      const lineMode = row.dataset.lineMode || 'neutral';
      let visible = true;
      if (mode === 'diff') visible = lineMode === 'diff';
      if (mode === 'relevant') visible = lineMode !== 'neutral';
      row.style.display = visible ? '' : 'none';
    }});
  }};
  buttons.forEach((button) => button.addEventListener('click', () => applyMode(button.dataset.lineFilter)));
  const jumpButton = document.getElementById('jump-next-diff');
  if (jumpButton) {{
    jumpButton.addEventListener('click', () => {{
      const diffRows = rows.filter((row) => row.dataset.lineMode === 'diff' && row.style.display !== 'none');
      if (!diffRows.length) return;
      currentDiffIndex = (currentDiffIndex + 1) % diffRows.length;
      diffRows[currentDiffIndex].scrollIntoView({{ behavior: 'smooth', block: 'center' }});
    }});
  }}
  applyMode('all');
}});
</script>
""",
    )


def write_compare_pages(compare_dir: Path, comparisons: list[CoverageComparison]) -> None:
    for comparison in comparisons:
        write(compare_dir / comparison.compare_href, render_compare_file_page(comparison))


def render_coverage_index(items: list[tuple[str, str, str]]) -> str:
    cards = []
    for slug, href, metric in sorted(items):
        cards.append(
            f"""
<a class=\"card stat\" href=\"{html.escape(href)}\">
  <div class=\"eyebrow\">{html.escape(slug)}</div>
  <div class=\"value good\">{html.escape(metric)}</div>
  <p class=\"muted small\">Open the best available report view for this platform.</p>
</a>
"""
        )
    return page(
        "Raw Coverage Reports",
        f"""
<section class=\"card hero\">
  <span class=\"pill\">Generated index</span>
  <h1>Raw Coverage Reports</h1>
  <p class=\"lede\">This index is generated from the downloaded coverage artifacts. Native llvm-cov HTML is preserved where available, and XML-only platforms receive a static HTML summary generated from their raw exports.</p>
</section>
<section class=\"grid\">{''.join(cards)}</section>
""",
    )


def parse_summary_file(summary_path: Path) -> dict[str, str]:
    data: dict[str, str] = {}
    if not summary_path.exists():
        return data
    for raw_line in summary_path.read_text(encoding="utf-8-sig").splitlines():
        if ":" not in raw_line:
            continue
        key, value = raw_line.split(":", 1)
        data[key.strip().lower()] = value.strip()
    return data


def render_windows_compare_page(raw_coverage_dir: Path, repo_root: Path) -> None:
    microsoft_dir = raw_coverage_dir / "windows-x64"
    opencpp_dir = raw_coverage_dir / "windows-x64-opencppcoverage"
    if not microsoft_dir.exists() or not opencpp_dir.exists():
        return

    microsoft_summary = parse_summary_file(microsoft_dir / "summary.txt")
    opencpp_summary = parse_summary_file(opencpp_dir / "summary.txt")
    compare_items = [
        CoverageSummary(
            slug="Microsoft native coverage",
            href="../windows-x64/index.html",
            line_coverage=microsoft_summary.get("line coverage", "n/a"),
            branch_coverage=microsoft_summary.get("block coverage", microsoft_summary.get("branch coverage", "n/a")),
            notes="MSVC /PROFILE + Microsoft.CodeCoverage.Console; XML is rendered into a static summary page.",
        ),
        CoverageSummary(
            slug="OpenCppCoverage",
            href="../windows-x64-opencppcoverage/index.html",
            line_coverage=opencpp_summary.get("line coverage", "n/a"),
            branch_coverage=opencpp_summary.get("branch coverage", "n/a"),
            notes="Tool-native HTML report from OpenCppCoverage, plus Cobertura and binary exports.",
        ),
    ]

    microsoft_report = parse_cobertura("windows-x64", microsoft_dir / "coverage.cobertura.xml", repo_root)
    opencpp_report = parse_cobertura("windows-x64-opencppcoverage", opencpp_dir / "coverage.cobertura.xml", repo_root)
    comparisons = build_coverage_comparisons(microsoft_report, opencpp_report)
    write_compare_pages(raw_coverage_dir / "windows-x64-compare", comparisons)

    rows = "\n".join(
        f"<tr><td>{html.escape(item.slug)}</td><td>{html.escape(item.line_coverage)}</td><td>{html.escape(item.branch_coverage)}</td><td>{html.escape(item.notes)}</td><td><a href=\"{html.escape(item.href)}\">Open report</a></td></tr>"
        for item in compare_items
    )
    file_rows = "\n".join(
        f"<tr><td class=\"mono\"><a href=\"{html.escape(item.compare_href)}\">{html.escape(item.display_name)}</a></td><td>{pct(item.microsoft.line_rate)}</td><td>{pct(item.opencpp.line_rate)}</td><td>{pct(abs(item.microsoft.line_rate - item.opencpp.line_rate))}</td></tr>"
        for item in comparisons[:200]
    )
    write(
        raw_coverage_dir / "windows-x64-compare" / "index.html",
        page(
            "Windows Coverage Compare",
            f"""
<section class=\"card hero\">
  <span class=\"pill\">Windows compare</span>
  <h1>Windows Coverage Compare</h1>
  <p class=\"lede\">This page lets you compare the two Windows-native coverage lanes side by side: Microsoft native coverage and OpenCppCoverage. Use it to judge both the metric deltas and the browsing experience.</p>
</section>
<section class=\"card table-card\">
  <h2>Top-level Metrics</h2>
  <table>
    <thead><tr><th>Lane</th><th>Line Coverage</th><th>Branch / Block Coverage</th><th>Notes</th><th>Report</th></tr></thead>
    <tbody>{rows}</tbody>
  </table>
</section>
<section class=\"card table-card\">
  <h2>Same-file Diffs</h2>
  <p class=\"muted small\">Top 200 common files sorted by absolute line-coverage delta.</p>
  <table>
    <thead><tr><th>File</th><th>Microsoft</th><th>OpenCpp</th><th>Delta</th></tr></thead>
    <tbody>{file_rows}</tbody>
  </table>
</section>
""",
        ),
    )


def render_redirect(title: str, href: str) -> str:
    return f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"UTF-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <meta http-equiv=\"refresh\" content=\"0; url={html.escape(href)}\">
  <title>{html.escape(title)}</title>
  <style>{PAGE_STYLE}</style>
</head>
<body>
  <main>
    <section class=\"card hero\">
      <h1>{html.escape(title)}</h1>
      <p class=\"lede\">Redirecting to the generated tool report. If it does not load automatically, open <a href=\"{html.escape(href)}\">{html.escape(href)}</a>.</p>
    </section>
  </main>
</body>
</html>
"""


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def render_test_reports(raw_test_dir: Path) -> None:
    reports: list[JUnitReport] = []

    for platform_dir in sorted(path for path in raw_test_dir.iterdir() if path.is_dir()):
        xml_path = platform_dir / "tests.xml"
        if not xml_path.exists():
            continue
        report = parse_junit_report(platform_dir.name, xml_path, "tests.xml")
        write(platform_dir / "index.html", render_junit_platform(report))
        reports.append(report)

    for xml_path in sorted(raw_test_dir.glob("*.xml")):
        slug = xml_path.stem
        platform_dir = raw_test_dir / slug
        target_xml = platform_dir / "tests.xml"
        if not target_xml.exists():
            platform_dir.mkdir(parents=True, exist_ok=True)
            target_xml.write_text(xml_path.read_text(encoding="utf-8-sig"), encoding="utf-8", newline="\n")
        report = parse_junit_report(slug, target_xml, "tests.xml")
        write(platform_dir / "index.html", render_junit_platform(report))
        reports = [existing for existing in reports if existing.slug != slug]
        reports.append(report)

    if reports:
        write(raw_test_dir / "index.html", render_junit_index(reports))


def parse_summary_metric(summary_path: Path) -> str:
    if not summary_path.exists():
        return "n/a"
    first_line = summary_path.read_text(encoding="utf-8-sig").splitlines()[0].strip()
    if first_line:
        return first_line
    return "n/a"


def detect_tool_metric(platform_dir: Path) -> str:
    summary_metric = parse_summary_metric(platform_dir / "summary.txt")
    if summary_metric != "n/a":
        return summary_metric
    html_index = find_first([platform_dir / "html" / "index.html", platform_dir / "index.html"])
    if html_index and html_index.exists():
        text = html_index.read_text(encoding="utf-8-sig", errors="ignore")
        match = re.search(r"(\d+\.\d+%\s+line coverage[^<]*)", text, re.IGNORECASE)
        if match:
            return match.group(1)
    return "tool report"


def render_coverage_reports(raw_coverage_dir: Path, repo_root: Path) -> None:
    items: list[tuple[str, str, str]] = []

    for platform_dir in sorted(path for path in raw_coverage_dir.iterdir() if path.is_dir()):
        slug = platform_dir.name
        cobertura_xml = platform_dir / "coverage.cobertura.xml"
        nested_html = platform_dir / "html" / "index.html"
        root_index = platform_dir / "index.html"

        if nested_html.exists() and slug.endswith("-opencppcoverage"):
            write(root_index, render_redirect(f"{slug} Coverage", "html/index.html"))
            items.append((slug, f"{slug}/index.html", detect_tool_metric(platform_dir)))
            continue

        if cobertura_xml.exists():
            report = parse_cobertura(slug, cobertura_xml, repo_root)
            write_cobertura_file_pages(platform_dir, report, slug)
            write(root_index, render_cobertura_platform(report))
            items.append((slug, f"{slug}/index.html", pct(report.line_rate)))
            continue

        if nested_html.exists():
            write(root_index, render_redirect(f"{slug} Coverage", "html/index.html"))
            items.append((slug, f"{slug}/index.html", detect_tool_metric(platform_dir)))
            continue

        if root_index.exists():
            items.append((slug, f"{slug}/index.html", detect_tool_metric(platform_dir)))

    if items:
        if (raw_coverage_dir / "windows-x64").exists() and (raw_coverage_dir / "windows-x64-opencppcoverage").exists():
            items.append(("windows-x64-compare", "windows-x64-compare/index.html", "compare 2 lanes"))
        write(raw_coverage_dir / "index.html", render_coverage_index(items))
    render_windows_compare_page(raw_coverage_dir, repo_root)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("Usage: render_raw_reports.py <site-src-dir>", file=sys.stderr)
        return 1

    site_src_dir = Path(argv[1]).resolve()
    raw_test_dir = site_src_dir / "raw_test"
    raw_coverage_dir = site_src_dir / "raw_coverage"

    if raw_test_dir.exists():
        render_test_reports(raw_test_dir)
    if raw_coverage_dir.exists():
        render_coverage_reports(raw_coverage_dir, site_src_dir.parent.parent)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
