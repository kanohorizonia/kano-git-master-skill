#!/usr/bin/env bash
set -euo pipefail

if ! command -v powershell >/dev/null 2>&1; then
  echo "powershell is required." >&2
  exit 1
fi

powershell -NoProfile -ExecutionPolicy Bypass -Command "& {
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
      $installed = winget list --id $Id --exact --accept-source-agreements 2>$null
      if ($LASTEXITCODE -ne 0) {
        Write-LauncherStatus -Step "prereq:$Id" -Status 'info' -Message 'mode=install'
        winget install --id $Id --exact --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
        return
      }

      Write-LauncherStatus -Step "prereq:$Id" -Status 'info' -Message 'mode=upgrade'
      winget upgrade --id $Id --exact --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
    }
  }

  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget is required to auto-install prerequisites. Please install App Installer from Microsoft Store.'
  }

  Write-LauncherStatus -Step 'prerequisite' -Status 'start' -Message 'windows dependency bootstrap'

  Ensure-WingetPackage -Id 'Kitware.CMake'
  Ensure-WingetPackage -Id 'Ninja-build.Ninja'
  Ensure-WingetPackage -Id 'Git.Git'
  Ensure-WingetPackage -Id 'Python.Python.3.12'

  Invoke-LauncherStep -Step 'prereq:Microsoft.VisualStudio.2022.BuildTools' -Action {
    Write-LauncherStatus -Step 'prereq:Microsoft.VisualStudio.2022.BuildTools' -Status 'info' -Message 'mode=install-or-repair-components'
    winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --override '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.CMake.Project' --accept-source-agreements --accept-package-agreements --disable-interactivity
  }

  Write-LauncherStatus -Step 'prerequisite' -Status 'ok' -Message 'windows dependency bootstrap complete'
}" 
