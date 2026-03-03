#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${KOG_CPP_ROOT:-}" ]]; then
  echo "KOG_CPP_ROOT is not set." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/build_metadata.sh"

kog_windows_file_exists() {
  local InPath="$1"
  local EscapedPath="${InPath//\'/\'\'}"
  powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path -LiteralPath '$EscapedPath') { exit 0 } else { exit 1 }" >/dev/null 2>&1
}

kog_detect_vcvarsall() {
  local Found=""

  if command -v powershell >/dev/null 2>&1; then
    Found="$(
      powershell -NoProfile -ExecutionPolicy Bypass -Command "\
        \$vswhere = Join-Path \${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'; \
        if (Test-Path \$vswhere) { \
          \$found = & \$vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvarsall.bat' 2>\$null | Select-Object -First 1; \
          if (\$found) { Write-Output \$found; exit 0 } \
        }; \
        \$roots = @(); \
        if (\$env:ProgramFiles) { \$roots += (Join-Path \$env:ProgramFiles 'Microsoft Visual Studio') }; \
        if (\${env:ProgramFiles(x86)}) { \$roots += (Join-Path \${env:ProgramFiles(x86)} 'Microsoft Visual Studio') }; \
        \$roots = \$roots | Where-Object { Test-Path \$_ }; \
        if (\$roots.Count -gt 0) { \
          \$scan = Get-ChildItem -Path \$roots -Recurse -File -Filter vcvarsall.bat -ErrorAction SilentlyContinue | \
            Where-Object { \$_.FullName -match '\\\\VC\\\\Auxiliary\\\\Build\\\\vcvarsall\.bat$' } | \
            Sort-Object FullName -Descending | \
            Select-Object -First 1 -ExpandProperty FullName; \
          if (\$scan) { Write-Output \$scan } \
        }" \
      | tr -d '\r'
    )"
  fi

  if [[ -n "$Found" ]] && kog_windows_file_exists "$Found"; then
    printf '%s\n' "$Found"
    return 0
  fi

  return 1
}

kog_run_windows_preset() {
  local InConfigurePreset="$1"
  local InBuildPreset="$2"
  local InVcvarsArch="$3"
  local InSubstPurpose="${KOG_SUBST_PURPOSE:-kano-git cpp build}"
  local InPreferredSubstDrive="${KOG_SUBST_DRIVE:-}"

  if ! command -v cmd.exe >/dev/null 2>&1; then
    echo "cmd.exe is required." >&2
    exit 1
  fi

  if ! command -v powershell >/dev/null 2>&1; then
    echo "powershell is required." >&2
    exit 1
  fi

  local RequestedVcvars="${KOG_VCVARSALL:-}"
  local ResolvedVcvars=""
  if [[ -n "$RequestedVcvars" ]]; then
    ResolvedVcvars="$RequestedVcvars"
  else
    ResolvedVcvars="$(kog_detect_vcvarsall || true)"
  fi

  if ! kog_windows_file_exists "$ResolvedVcvars"; then
    echo "vcvarsall.bat not found." >&2
    echo "Set KOG_VCVARSALL explicitly, e.g.:" >&2
    echo "  KOG_VCVARSALL='C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat'" >&2
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
  PSEscapedVcvars="${ResolvedVcvars//\'/\'\'}"

  powershell -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location '$PSEscapedRoot'; \$cmd = 'call \"$PSEscapedVcvars\" $InVcvarsArch -vcvars_ver=14.44.35207 && cmake --preset $InConfigurePreset && cmake --build --preset $InBuildPreset'; cmd.exe /d /s /c \$cmd; exit \$LASTEXITCODE }"
}
