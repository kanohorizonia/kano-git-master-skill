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

  export KOG_PS_ROOT="$RootWin"
  export KOG_PS_VCVARS="$KOG_VCVARSALL"
  export KOG_PS_SUBST_PURPOSE="$InSubstPurpose"
  export KOG_PS_SUBST_DRIVE="$InPreferredSubstDrive"
  export KOG_PS_CONFIG_PRESET="$InConfigurePreset"
  export KOG_PS_BUILD_PRESET="$InBuildPreset"
  export KOG_PS_VCVARS_ARCH="$InVcvarsArch"

  local temp_ps1
  temp_ps1="$(mktemp -t kog_windows_preset_build.XXXXXX.ps1)"
  cat >"$temp_ps1" <<'POWERSHELL'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$rootDir = $env:KOG_PS_ROOT
$vcvarsPath = $env:KOG_PS_VCVARS
$substPurpose = $env:KOG_PS_SUBST_PURPOSE
$preferredDrive = $env:KOG_PS_SUBST_DRIVE
$configurePreset = $env:KOG_PS_CONFIG_PRESET
$buildPreset = $env:KOG_PS_BUILD_PRESET
$vcvarsArch = $env:KOG_PS_VCVARS_ARCH
$ownerDir = Join-Path $env:TEMP 'kano-git-subst-owners'

if (-not (Test-Path $ownerDir)) {
  New-Item -ItemType Directory -Path $ownerDir -Force | Out-Null
}

function Get-OwnerFilePath {
  param([string]$driveLetter)
  return (Join-Path $ownerDir ("{0}.json" -f $driveLetter.ToUpperInvariant()))
}

$defaultDriveCandidates = @('X', 'Y', 'Z', 'W', 'V', 'U', 'T', 'S', 'R', 'Q', 'P')
$preferredDrive = [string]$preferredDrive
$preferredDrive = $preferredDrive.Trim().TrimEnd(':').ToUpperInvariant()

if ([string]::IsNullOrWhiteSpace($preferredDrive)) {
  $driveCandidates = $defaultDriveCandidates
} else {
  $driveCandidates = @($preferredDrive) + ($defaultDriveCandidates | Where-Object { $_ -ne $preferredDrive })
}

$substList = cmd.exe /d /s /c subst
if ($LASTEXITCODE -eq 0) {
  foreach ($line in $substList) {
    if ($line -match '^([A-Z]):\\: => (.+)$') {
      $mappedDrive = $matches[1].ToUpperInvariant()
      $mappedPath = $matches[2].Trim()
      if (($driveCandidates -contains $mappedDrive) -and ($mappedPath -ieq $rootDir)) {
        $ownerFile = Get-OwnerFilePath -driveLetter $mappedDrive
        if (Test-Path $ownerFile) {
          try {
            $owner = Get-Content $ownerFile -Raw | ConvertFrom-Json
            if ($owner.tool -eq 'kano-git' -and $owner.rootDir -ieq $mappedPath) {
              Write-Host "[launcher][windows-preset-build] cleanup stale owned subst ${mappedDrive}: -> $mappedPath"
              subst ($mappedDrive + ':') /D 2>&1 | Out-Null
              Remove-Item $ownerFile -Force -ErrorAction SilentlyContinue
            }
          } catch {
            Write-Host "[launcher][windows-preset-build] skip stale cleanup for ${mappedDrive}: invalid owner marker"
          }
        }
      }
    }
  }
}

$selectedDrive = $null
foreach ($candidate in $driveCandidates) {
  if (-not (Get-PSDrive -Name $candidate -ErrorAction SilentlyContinue)) {
    $selectedDrive = $candidate
    break
  }
}

if (-not $selectedDrive) {
  Write-Host '[launcher][windows-preset-build] ERROR: no free drive letter for subst'
  exit 1
}

$substPath = $selectedDrive + ':'
Write-Host "[launcher][windows-preset-build] subst map $substPath -> $rootDir (purpose=$substPurpose)"
subst $substPath "$rootDir" 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Host "[launcher][windows-preset-build] ERROR: subst failed for $substPath"
  exit 1
}
Write-Host "[launcher][windows-preset-build] subst ready"

$ownerFile = Get-OwnerFilePath -driveLetter $selectedDrive
$ownerRecord = [PSCustomObject]@{
  tool = 'kano-git'
  rootDir = $rootDir
  drive = $substPath
  purpose = $substPurpose
  pid = $PID
  createdAtUtc = [DateTime]::UtcNow.ToString('o')
}
$ownerRecord | ConvertTo-Json -Depth 4 | Set-Content -Path $ownerFile -Encoding UTF8
Write-Host "[launcher][windows-preset-build] owner marker written: $ownerFile"

try {
  $cmd = ('cd /d {5} && call "{0}" {1} -vcvars_ver=14.44.35207 && cmake --preset {2} -S {3} && cmake --build --preset {4}' -f $vcvarsPath, $vcvarsArch, $configurePreset, $substPath, $buildPreset, $substPath)
  Write-Host "[launcher][windows-preset-build] run cmd: $cmd"
  cmd.exe /d /s /c $cmd
  exit $LASTEXITCODE
} finally {
  subst $substPath /D 2>&1 | Out-Null
  if (Test-Path $ownerFile) {
    Remove-Item $ownerFile -Force -ErrorAction SilentlyContinue
  }
}
POWERSHELL

  powershell -NoProfile -ExecutionPolicy Bypass -File "$temp_ps1"
  local ps_exit=$?
  rm -f "$temp_ps1"
  return $ps_exit
}
