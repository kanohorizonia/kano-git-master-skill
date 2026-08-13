#!/usr/bin/env python3
"""Run the bounded, release-built TUI PR focus suite.

This runner deliberately has no broad fallback selector.  A missing or empty
``[tui_pr_focus]`` suite is a CI configuration error, not a successful run.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


MAX_JUNIT_BYTES = 2 * 1024 * 1024


def run(command: list[str], *, timeout_seconds: int, label: str) -> None:
    try:
        completed = subprocess.run(command, check=False, timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"{label} timed out after {timeout_seconds}s") from error
    if completed.returncode != 0:
        raise SystemExit(f"{label} exited with status {completed.returncode}")


def junit_test_count(path: Path) -> int:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        raise SystemExit(f"invalid JUnit XML: {path}: {error}") from error

    if root.tag not in {"testsuite", "testsuites"}:
        raise SystemExit(f"unexpected JUnit root element in {path}: {root.tag}")
    # Catch2's JUnit reporter emits testcase elements. Count the concrete
    # elements rather than trusting a suite-level summary attribute, which is
    # optional in JUnit variants and can be omitted by a reporter on failure.
    return len(root.findall(".//testcase"))


def junit_case_names(path: Path) -> set[str]:
    """Return the exact Catch2 case names present in a bounded JUnit report."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        raise SystemExit(f"invalid JUnit XML: {path}: {error}") from error
    return {case.attrib["name"] for case in root.findall(".//testcase") if "name" in case.attrib}


def verify_junit_artifact(*, label: str, junit_path: Path) -> None:
    """Enforce the artifact cap even when a test process fails or times out."""
    if junit_path.exists() and junit_path.stat().st_size > MAX_JUNIT_BYTES:
        junit_path.unlink()
        raise SystemExit(f"{label} JUnit XML exceeds {MAX_JUNIT_BYTES} bytes")


def write_status(path: Path, result: str) -> None:
    """Write a bounded, non-sensitive result marker for failed CI runs."""
    path.write_text(f"result={result}\n", encoding="utf-8")


def run_focus_suite(
    *, label: str, binary: Path, selector: str, junit_path: Path, timeout_seconds: int,
    required_cases: set[str],
) -> None:
    if not binary.is_file():
        raise SystemExit(f"required release-built {label} binary is missing: {binary}")
    try:
        run(
            [str(binary), selector, "--reporter", "junit", "--out", str(junit_path)],
            timeout_seconds=timeout_seconds, label=f"{label} focus suite",
        )
    finally:
        verify_junit_artifact(label=label, junit_path=junit_path)
    if not junit_path.is_file():
        raise SystemExit(f"{label} focus suite did not write JUnit XML: {junit_path}")
    tests = junit_test_count(junit_path)
    if tests < 1:
        raise SystemExit(f"{selector} selected zero tests; refusing a false-green {label} PR gate")
    missing_cases = required_cases - junit_case_names(junit_path)
    if missing_cases:
        raise SystemExit(
            f"{label} focus suite missed required case(s): {', '.join(sorted(missing_cases))}"
        )
    print(f"{label} PR focus suite passed: {tests} test case(s); required inventory present")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--application-dir", type=Path, required=True)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--audit-test-binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=1_200)
    parser.add_argument("--required-tui-test", action="append", default=[])
    parser.add_argument("--required-audit-test", action="append", default=[])
    args = parser.parse_args()

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    status_path = output_dir / "status.txt"

    suffix = ".exe" if sys.platform == "win32" else ""
    app_dir = args.application_dir.resolve()
    test_binary = args.test_binary.resolve()
    audit_test_binary = args.audit_test_binary.resolve()
    tui_junit_path = output_dir / "tui-pr-focus.junit.xml"
    audit_junit_path = output_dir / "audit-pr-focus.junit.xml"

    try:
        for required in (app_dir / f"kano-git{suffix}", app_dir / f"kano-git-tui{suffix}"):
            if not required.is_file():
                raise SystemExit(f"required release-built binary is missing: {required}")
        run([str(app_dir / f"kano-git{suffix}"), "--version"], timeout_seconds=60,
            label="kano-git version smoke")
        run([str(app_dir / f"kano-git-tui{suffix}"), "--help"], timeout_seconds=60,
            label="kano-git-tui help smoke")
        run_focus_suite(
            label="TUI",
            binary=test_binary,
            selector="[tui_pr_focus]",
            junit_path=tui_junit_path,
            timeout_seconds=args.timeout_seconds,
            required_cases=set(args.required_tui_test),
        )
        run_focus_suite(
            label="audit-reader/pinned",
            binary=audit_test_binary,
            selector="[audit_pr_focus]",
            junit_path=audit_junit_path,
            timeout_seconds=args.timeout_seconds,
            required_cases=set(args.required_audit_test),
        )
    except BaseException:
        write_status(status_path, "failed")
        raise
    write_status(status_path, "passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
