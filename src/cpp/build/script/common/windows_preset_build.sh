#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${KOG_CPP_ROOT:-}" ]]; then
  echo "KOG_CPP_ROOT is not set." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/build_metadata.sh"

KOG_VCVARSALL_DEFAULT="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
KOG_VCVARSALL="${KOG_VCVARSALL:-$KOG_VCVARSALL_DEFAULT}"

kog_run_windows_preset() {
  local InConfigurePreset="$1"
  local InBuildPreset="$2"
  local InVcvarsArch="$3"

  if ! command -v cmd.exe >/dev/null 2>&1; then
    echo "cmd.exe is required." >&2
    exit 1
  fi

  if ! command -v powershell >/dev/null 2>&1; then
    echo "powershell is required." >&2
    exit 1
  fi

  if [[ ! -f "$KOG_VCVARSALL" ]]; then
    echo "vcvarsall.bat not found: $KOG_VCVARSALL" >&2
    exit 1
  fi

  local RootWin

  kog_ensure_ftxui_vendor
  kog_collect_build_metadata

  if command -v cygpath >/dev/null 2>&1; then
    RootWin="$(cygpath -w "$KOG_CPP_ROOT")"
  else
    RootWin="$(cd "$KOG_CPP_ROOT" && pwd -W)"
  fi

  local PSEscapedRoot
  local PSEscapedVcvars
  PSEscapedRoot="${RootWin//\'/\'\'}"
  PSEscapedVcvars="${KOG_VCVARSALL//\'/\'\'}"

  powershell -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location '$PSEscapedRoot'; \$cmd = 'call \"$PSEscapedVcvars\" $InVcvarsArch -vcvars_ver=14.44.35207 && cmake --preset $InConfigurePreset && cmake --build --preset $InBuildPreset'; cmd.exe /d /s /c \$cmd; exit \$LASTEXITCODE }"
}
