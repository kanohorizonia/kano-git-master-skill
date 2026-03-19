#!/usr/bin/env bash
set -euo pipefail

if ! command -v powershell >/dev/null 2>&1; then
  echo "powershell is required." >&2
  exit 1
fi

powershell -NoProfile -ExecutionPolicy Bypass -Command - <<'POWERSHELL'
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-LauncherStatus {
  param(
    [string]$Step,
    [string]$Status,
    [string]$Message = ''
  )

  if ([string]::IsNullOrWhiteSpace($Message)) {
    Write-Host "[launcher][$Step][$Status]"
    return
  }

  Write-Host "[launcher][$Step][$Status] $Message"
}

function Invoke-LauncherStep {
  param(
    [string]$Step,
    [scriptblock]$Action
  )

  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  Write-LauncherStatus -Step $Step -Status 'start'
  try {
    & $Action
    $sw.Stop()
    Write-LauncherStatus -Step $Step -Status 'ok' -Message "elapsed=$([int]$sw.Elapsed.TotalSeconds)s"
  } catch {
    $sw.Stop()
    $msg = $_.Exception.Message
    Write-LauncherStatus -Step $Step -Status 'fail' -Message "elapsed=$([int]$sw.Elapsed.TotalSeconds)s error=$msg"
    throw
  }
}

function Ensure-WingetPackage {
  param([string]$Id)

  Invoke-LauncherStep -Step "prereq:$Id" -Action {
    $noUpgradeCode = -1978335189
    $installed = winget list --id $Id --exact --accept-source-agreements 2>$null
    if ($LASTEXITCODE -ne 0) {
      Write-LauncherStatus -Step "prereq:$Id" -Status 'info' -Message 'mode=install'
      winget install --id $Id --exact --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
      if ($LASTEXITCODE -ne 0) {
        throw "winget install failed for $Id with exit code $LASTEXITCODE"
      }
      return
    }

    Write-LauncherStatus -Step "prereq:$Id" -Status 'info' -Message 'mode=upgrade'
    winget upgrade --id $Id --exact --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $noUpgradeCode) {
      throw "winget upgrade failed for $Id with exit code $LASTEXITCODE"
    }
  }
}

function Get-PixiEnvironmentRoot {
  if (-not [string]::IsNullOrWhiteSpace($env:PIXI_PROJECT_ROOT)) {
    return $env:PIXI_PROJECT_ROOT
  }
  if (-not [string]::IsNullOrWhiteSpace($env:CONDA_PREFIX)) {
    return $env:CONDA_PREFIX
  }
  return $null
}

function Test-CommandAvailable {
  param([string[]]$Names)

  foreach ($name in $Names) {
    if ([string]::IsNullOrWhiteSpace($name)) {
      continue
    }
    if (Get-Command $name -ErrorAction SilentlyContinue) {
      return $true
    }
  }

  return $false
}

function Ensure-WingetPackageUnlessCommandAvailable {
  param(
    [string]$Id,
    [string[]]$CommandNames
  )

  $pixiRoot = Get-PixiEnvironmentRoot
  if ($pixiRoot -and (Test-CommandAvailable -Names $CommandNames)) {
    $commandLabel = ($CommandNames | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join ','
    Write-LauncherStatus -Step "prereq:$Id" -Status 'info' -Message "mode=skip-pixi-env root=$pixiRoot commands=$commandLabel"
    return
  }

  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget is required to install $Id automatically. Install App Installer from Microsoft Store, or provide the tool via pixi/system PATH."
  }

  Ensure-WingetPackage -Id $Id
}

function Resolve-VcvarsallPath {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) {
    return $null
  }

  $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Auxiliary\Build\vcvarsall.bat' 2>$null | Select-Object -First 1
  if ([string]::IsNullOrWhiteSpace($path)) {
    return $null
  }
  return $path
}

Write-LauncherStatus -Step 'prerequisite' -Status 'start' -Message 'windows dependency bootstrap'

Ensure-WingetPackageUnlessCommandAvailable -Id 'Kitware.CMake' -CommandNames @('cmake')
Ensure-WingetPackageUnlessCommandAvailable -Id 'Ninja-build.Ninja' -CommandNames @('ninja')
Ensure-WingetPackageUnlessCommandAvailable -Id 'Python.Python.3.12' -CommandNames @('python', 'python3')

Invoke-LauncherStep -Step 'prereq:Microsoft.VisualStudio.2022.BuildTools' -Action {
  $existingVcvarsall = Resolve-VcvarsallPath
  if ($existingVcvarsall) {
    Write-LauncherStatus -Step 'prereq:Microsoft.VisualStudio.2022.BuildTools' -Status 'info' -Message "mode=skip-existing-msvc vcvarsall=$existingVcvarsall"
    return
  }

  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget is required to auto-install Microsoft.VisualStudio.2022.BuildTools. Please install App Installer from Microsoft Store.'
  }

  Write-LauncherStatus -Step 'prereq:Microsoft.VisualStudio.2022.BuildTools' -Status 'info' -Message 'mode=install-components'
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --override '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.CMake.Project' --accept-source-agreements --accept-package-agreements --disable-interactivity
  if ($LASTEXITCODE -ne 0) {
    throw "winget install failed for Microsoft.VisualStudio.2022.BuildTools with exit code $LASTEXITCODE"
  }
}

Write-LauncherStatus -Step 'prerequisite' -Status 'ok' -Message 'windows dependency bootstrap complete'
POWERSHELL
