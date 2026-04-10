#!/usr/bin/env python3
from __future__ import annotations

import csv
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=str(cwd), env=env, text=True, capture_output=True)


def load_csv_rows(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit("usage: run_matrix.py <matrix.json> <tmp-root> <repo-root> <cpp-root>")

    matrix_path = Path(sys.argv[1])
    tmp_root = Path(sys.argv[2])
    repo_root = Path(sys.argv[3])
    cpp_root = Path(sys.argv[4])
    matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    host_os = {"Windows": "windows", "Darwin": "macos"}.get(platform.system(), "linux")
    host_arch = "arm64" if platform.machine().lower() in {"arm64", "aarch64"} else "x64"

    matrix_name = str(matrix.get("name") or matrix_path.stem)
    defaults = dict(matrix.get("defaults") or {})
    report_slug = str(matrix.get("reportSlug") or matrix_name)
    matrix_root = tmp_root / matrix_name
    matrix_root.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, object]] = []
    for case in matrix.get("cases", []):
        case_id = str(case["id"])
        case_root = matrix_root / case_id
        case_root.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["KOG_CPP_ROOT"] = str(cpp_root)
        env["KOG_PROFILE_CASE_ID"] = case_id
        env["KOG_PROFILE_MATRIX"] = matrix_name
        env["KOG_PROFILE_REPORT_SLUG"] = report_slug

        launcher = str(case.get("launcher", defaults.get("launcher", "auto")))
        if launcher:
            env["KOG_COMPILER_LAUNCHER"] = launcher

        cache_args: dict[str, str] = {}
        modules = str(case.get("modules", defaults.get("modules", "off"))).lower()
        cache_args["KOG_ENABLE_MODULES"] = "ON" if modules == "on" else "OFF"

        unity = str(case.get("unity", defaults.get("unity", "off"))).lower()
        cache_args["KOG_ENABLE_UNITY_BUILD"] = "OFF" if unity == "off" else "ON"
        if unity in {"full", "changed"}:
            cache_args["KOG_UNITY_BUILD_MODE"] = unity

        if str(case.get("coverage", defaults.get("coverage", "off"))).lower() == "on":
            cache_args["KOG_ENABLE_COVERAGE"] = "ON"

        pgo_mode = str(case.get("pgo", defaults.get("pgo", "off"))).lower()
        workflow = str(case.get("workflow", defaults.get("workflow", "baseline"))).lower()

        if pgo_mode == "collect":
            cache_args["KOG_PGO_MODE"] = "collect"
        elif pgo_mode == "use":
            cache_args["KOG_PGO_MODE"] = "use"

        env["KOG_CMAKE_CACHE_ARGS_JSON"] = json.dumps(cache_args)

        output_csv = case_root / "baseline.csv"
        artifacts: dict[str, str] = {"caseRoot": str(case_root)}
        result: dict[str, object] = {
            "id": case_id,
            "hostOs": host_os,
            "hostArch": host_arch,
            "launcher": launcher,
            "unity": unity,
            "modules": modules,
            "pgo": pgo_mode,
            "workflow": workflow,
            "cacheArgs": cache_args,
            "status": "pending",
            "artifacts": artifacts,
        }

        if workflow == "pgo":
            command = ["bash", "src/cpp/scripts/workflows/pgo-rebuild.sh"]
        else:
            command = [
                "bash",
                "src/cpp/scripts/common/measure_iteration_baseline.sh",
                "--configure-preset",
                str(case.get("configurePreset", defaults["configurePreset"])),
                "--build-preset",
                str(case.get("buildPreset", defaults["buildPreset"])),
                "--build-dir",
                str(case.get("buildDir", defaults["buildDir"])),
                "--config",
                str(case.get("config", defaults.get("config", "Release"))),
                "--output",
                str(output_csv),
            ]

        completed = run(command, repo_root, env)
        (case_root / "stdout.log").write_text(completed.stdout, encoding="utf-8")
        (case_root / "stderr.log").write_text(completed.stderr, encoding="utf-8")
        result["exitCode"] = completed.returncode

        if completed.returncode == 0:
            result["status"] = "passed"
        else:
            result["status"] = "failed"

        if output_csv.is_file():
            rows = load_csv_rows(output_csv)
            result["baselineRows"] = rows
            artifacts["baselineCsv"] = str(output_csv)

        (case_root / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        results.append(result)

    summary = {
        "name": matrix_name,
        "reportSlug": report_slug,
        "hostOs": host_os,
        "hostArch": host_arch,
        "cases": results,
    }
    (matrix_root / "profile.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(matrix_root / "profile.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
