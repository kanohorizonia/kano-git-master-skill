#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${KOG_CPP_ROOT:-}" ]]; then
  echo "KOG_CPP_ROOT is not set." >&2
  exit 1
fi

kog_run_linux_preset_via_docker() {
  local InConfigurePreset="$1"
  local InBuildPreset="$2"

  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required." >&2
    exit 1
  fi

  if ! command -v powershell >/dev/null 2>&1; then
    echo "powershell is required." >&2
    exit 1
  fi

  local RepoRootWin
  RepoRootWin="$(cd "$KOG_CPP_ROOT/../.." && pwd -W)"
  RepoRootWin="${RepoRootWin//\'/\'\'}"

  # --security-opt seccomp=unconfined: required for sanitizer builds (TSan uses
  # personality(ADDR_NO_RANDOMIZE) which the default Docker seccomp profile blocks)
  powershell -NoProfile -ExecutionPolicy Bypass -Command "& { docker run --rm --security-opt seccomp=unconfined -v '$RepoRootWin:/work' -w /work/src/cpp ubuntu:25.10 bash -lc 'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y cmake ninja-build gcc-15 g++-15 git && rm -rf /work/src/cpp/out/obj/$InConfigurePreset && cmake --preset $InConfigurePreset && cmake --build --preset $InBuildPreset'; exit \$LASTEXITCODE }"
}
