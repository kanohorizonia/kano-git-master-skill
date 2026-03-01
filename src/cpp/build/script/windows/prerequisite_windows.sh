#!/usr/bin/env bash
set -euo pipefail

if ! command -v powershell >/dev/null 2>&1; then
  echo "powershell is required." >&2
  exit 1
fi

powershell -NoProfile -ExecutionPolicy Bypass -Command "& {
  Set-StrictMode -Version Latest
  $ErrorActionPreference = 'Stop'

  function Ensure-WingetPackage {
    param([string]$Id)

    $installed = winget list --id $Id --exact --accept-source-agreements 2>$null
    if ($LASTEXITCODE -ne 0) {
      Write-Host \"Installing $Id ...\"
      winget install --id $Id --exact --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
      return
    }

    Write-Host \"Upgrading $Id ...\"
    winget upgrade --id $Id --exact --source winget --accept-source-agreements --accept-package-agreements --silent --disable-interactivity
  }

  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget is required to auto-install prerequisites. Please install App Installer from Microsoft Store.'
  }

  Ensure-WingetPackage -Id 'Kitware.CMake'
  Ensure-WingetPackage -Id 'Ninja-build.Ninja'
  Ensure-WingetPackage -Id 'Git.Git'
  Ensure-WingetPackage -Id 'Python.Python.3.12'

  Write-Host 'Ensuring Visual Studio Build Tools components (MSVC x64/arm64 + CMake) ...'
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --override '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.CMake.Project' --accept-source-agreements --accept-package-agreements --disable-interactivity

  Write-Host 'Windows prerequisites setup complete.'
}" 
