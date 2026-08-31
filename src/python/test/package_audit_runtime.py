#!/usr/bin/env python3
# KOG_CONTRACT_TEST: audit runtime artifact rebuild survives SUBST cleanup
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
    "kog.auditCapability.v1.schema.json",
    "kog.auditVerification.v1.schema.json",
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


def configure_preset(build_preset_name: str) -> str:
    presets = load_json(CPP_ROOT / "CMakePresets.json")
    build = next(
        item
        for item in presets["buildPresets"]
        if item["name"] == build_preset_name
    )
    return str(build["configurePreset"])


def artifact_root(preset_name: str) -> Path:
    presets = load_json(CPP_ROOT / "CMakePresets.json")
    configure_name = configure_preset(preset_name)
    configure = next(
        item
        for item in presets["configurePresets"]
        if item["name"] == configure_name
    )
    binary_dir = configure["binaryDir"].replace("${sourceDir}", str(CPP_ROOT))
    return Path(binary_dir) / "runtime-artifact"


def main() -> int:
    preset = build_preset()
    artifact = artifact_root(preset)
    is_windows = sys.platform == "win32"
    if is_windows:
        environment = os.environ.copy()
        environment["KANO_WINDOWS_CONFIGURE_PRESET"] = configure_preset(preset)
        environment["KANO_WINDOWS_BUILD_PRESET"] = preset
        environment["KANO_WINDOWS_VCVARS_ARCH"] = (
            "arm64" if "arm64" in preset else "x64"
        )
        environment["KANO_WINDOWS_BUILD_TARGET"] = "kog_runtime_artifact"
        cache_path = artifact.parent / "CMakeCache.txt"
        if cache_path.is_file():
            prefix = "CMAKE_HOME_DIRECTORY:INTERNAL="
            cache_source = next(
                (
                    line.removeprefix(prefix)
                    for line in cache_path.read_text(
                        encoding="utf-8", errors="strict"
                    ).splitlines()
                    if line.startswith(prefix)
                ),
                "",
            )
            if len(cache_source) >= 2 and cache_source[1] == ":":
                cache_drive = cache_source[:2]
                if cache_drive.casefold() != CPP_ROOT.drive.casefold():
                    environment["KANO_WINDOWS_SUBST_DRIVE"] = cache_drive
        wrapper = (
            CPP_ROOT
            / "shared"
            / "infra"
            / "scripts"
            / "platform"
            / "win64"
            / "ninja-msvc-release.sh"
        )
        git_exec_path = Path(
            subprocess.check_output(
                ["git", "--exec-path"], cwd=CPP_ROOT, text=True
            ).strip()
        )
        git_bash = git_exec_path.parents[2] / "bin" / "bash.exe"
        if not git_bash.is_file():
            raise AssertionError(
                f"Git for Windows bash is missing: {git_bash}"
            )
        subprocess.run(
            [str(git_bash), str(wrapper)],
            cwd=CPP_ROOT,
            check=True,
            env=environment,
        )
    else:
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
    except Exception as error:  # noqa: BLE001  # noqa: BROAD_EXCEPT_OK - CLI boundary
        print(f"audit-runtime-test: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
