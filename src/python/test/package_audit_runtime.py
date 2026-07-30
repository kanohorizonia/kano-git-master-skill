#!/usr/bin/env python3
"""Build and verify the packaged KOG audit-schema runtime artifact."""

from __future__ import annotations

import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[3]
CPP_ROOT = REPO_ROOT / "src" / "cpp"
SCHEMA_ROOT = REPO_ROOT / "assets" / "audit" / "schemas"
EXPECTED_SCHEMAS = (
    "kog.auditEvent.v1.schema.json",
    "kog.runReceipt.v1.schema.json",
)


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def platform_key() -> str:
    explicit = os.environ.get("KANO_PLATFORM", "").strip().lower()
    if explicit:
        return explicit
    machine = platform.machine().lower()
    if sys.platform == "darwin":
        return "macos-arm64" if machine in {"arm64", "aarch64"} else "macos-x64"
    if sys.platform == "win32":
        return "windows-arm64" if machine in {"arm64", "aarch64"} else "windows-x64"
    return "linux-arm64" if machine in {"arm64", "aarch64"} else "linux-x64"


def build_preset() -> str:
    presets = {
        "macos-x64": "macos-ninja-clang-x64-release",
        "macos-arm64": "macos-ninja-clang-arm64-release",
        "windows-x64": "windows-ninja-msvc-release",
        "windows-arm64": "windows-ninja-msvc-arm64-release",
        "linux-x64": "linux-ninja-gcc-release",
        "linux-arm64": "linux-ninja-gcc-release",
    }
    key = platform_key()
    if key not in presets:
        raise AssertionError(f"unsupported KANO_PLATFORM: {key}")
    return presets[key]


def artifact_root(preset_name: str) -> Path:
    presets = load_json(CPP_ROOT / "CMakePresets.json")
    build = next(
        item for item in presets["buildPresets"] if item["name"] == preset_name
    )
    configure_name = build["configurePreset"]
    configure = next(
        item
        for item in presets["configurePresets"]
        if item["name"] == configure_name
    )
    binary_dir = configure["binaryDir"].replace("${sourceDir}", str(CPP_ROOT))
    return Path(binary_dir) / "runtime-artifact"


def main() -> int:
    preset = build_preset()
    subprocess.run(
        [
            "cmake",
            "--build",
            "--preset",
            preset,
            "--target",
            "kog_runtime_artifact",
        ],
        cwd=CPP_ROOT,
        check=True,
    )

    artifact = artifact_root(preset)
    manifest = load_json(artifact / "manifest.json")
    runtime_assets = set(manifest["runtime_assets"])
    for schema_name in EXPECTED_SCHEMAS:
        relative = f"assets/audit/schemas/{schema_name}"
        packaged = artifact / relative
        source = SCHEMA_ROOT / schema_name
        if relative not in runtime_assets:
            raise AssertionError(f"manifest omits {relative}")
        if packaged.read_bytes() != source.read_bytes():
            raise AssertionError(f"packaged schema differs from source: {relative}")

    binary = artifact / "bin" / "kano-git"
    if not binary.is_file() or binary.stat().st_size == 0:
        raise AssertionError(f"runtime binary is missing: {binary}")

    print(
        f"audit-runtime-test: preset={preset} artifact={artifact} "
        "manifest and schema bytes passed"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - release gate must emit one failure
        print(f"audit-runtime-test: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
