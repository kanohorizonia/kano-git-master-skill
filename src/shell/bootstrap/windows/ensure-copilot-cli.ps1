param(
    [switch]$Install,
    [switch]$DryRun,
    [string]$LocalAppDataOverride,
    [string]$WingetCommandOverride,
    [string]$CopilotExeOverride,
    [string]$UserPathSinkFile,
    [string]$MachinePathSinkFile,
    [string]$WingetLogFile
)

$ErrorActionPreference = "Stop"

function Get-LocalAppData {
    if ($LocalAppDataOverride) {
        return $LocalAppDataOverride
    }
    return $env:LOCALAPPDATA
}

function Split-PathList([string]$PathValue) {
    if (-not $PathValue) { return @() }
    return $PathValue -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
}

function Join-PathList([string[]]$Items) {
    return (($Items | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join ';')
}

function Write-UserPath([string]$PathValue) {
    if ($UserPathSinkFile) {
        Set-Content -LiteralPath $UserPathSinkFile -Value $PathValue -Encoding UTF8
        return
    }
    if (-not $DryRun) {
        [Environment]::SetEnvironmentVariable("Path", $PathValue, "User")
    }
}

function Write-MachinePathSink([string]$PathValue) {
    if ($MachinePathSinkFile) {
        Set-Content -LiteralPath $MachinePathSinkFile -Value $PathValue -Encoding UTF8
    }
}

function Detect-Winget {
    if ($WingetCommandOverride) {
        return $WingetCommandOverride
    }
    try {
        $null = & winget --version
        if ($LASTEXITCODE -eq 0) {
            return "winget"
        }
    } catch {
    }

    $candidate = Join-Path $script:WindowsApps "winget.exe"
    if (Test-Path -LiteralPath $candidate) {
        try {
            $null = & $candidate --version
            if ($LASTEXITCODE -eq 0) {
                return $candidate
            }
        } catch {
        }
    }

    return $null
}

function Resolve-CopilotExe {
    if ($CopilotExeOverride) {
        return $CopilotExeOverride
    }

    if ($LocalAppDataOverride) {
        foreach ($candidate in @(
            (Join-Path $script:WinGetLinks "copilot.exe"),
            (Join-Path $script:WinGetLinks "copilot.cmd")
        )) {
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }

        if (Test-Path -LiteralPath $script:WinGetPackages) {
            $packageCopilot = Get-ChildItem -LiteralPath $script:WinGetPackages -Recurse -Filter "copilot.exe" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1 -ExpandProperty FullName
            if ($packageCopilot) {
                return $packageCopilot
            }
        }

        return $null
    }

    try {
        $cmd = Get-Command copilot -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Path) {
            return $cmd.Path
        }
    } catch {
    }

    foreach ($candidate in @(
        (Join-Path $script:WinGetLinks "copilot.exe"),
        (Join-Path $script:WinGetLinks "copilot.cmd")
    )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    if (Test-Path -LiteralPath $script:WinGetPackages) {
        $packageCopilot = Get-ChildItem -LiteralPath $script:WinGetPackages -Recurse -Filter "copilot.exe" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if ($packageCopilot) {
            return $packageCopilot
        }
    }

    return $null
}

function Ensure-CopilotShim([string]$TargetExe) {
    $shimPath = Join-Path $script:WinGetLinks "copilot.cmd"
    if (Test-Path -LiteralPath $shimPath) {
        return $shimPath
    }

    if (-not $TargetExe -or -not (Test-Path -LiteralPath $TargetExe)) {
        return $null
    }

    if (-not $DryRun) {
        @"
@echo off
"$TargetExe" %*
"@ | Set-Content -LiteralPath $shimPath -Encoding ASCII
    }
    return $shimPath
}

function Ensure-UserPathEntries([string[]]$Entries) {
    $existing = [Environment]::GetEnvironmentVariable("Path", "User")
    $pathItems = [System.Collections.Generic.List[string]]::new()
    foreach ($item in (Split-PathList $existing)) {
        $pathItems.Add($item)
    }

    foreach ($entry in $Entries) {
        if (-not $entry) { continue }
        if (-not $pathItems.Contains($entry)) {
            $pathItems.Add($entry)
        }
    }

    $updated = Join-PathList $pathItems
    Write-UserPath $updated
    return $updated
}

function Invoke-WingetInstall([string]$WingetCommand) {
    $argv = @("install", "GitHub.Copilot", "--accept-source-agreements", "--accept-package-agreements")
    if ($WingetLogFile) {
        Add-Content -LiteralPath $WingetLogFile -Value ("{0} {1}" -f $WingetCommand, ($argv -join ' '))
    }
    if ($DryRun) {
        Write-Host "[dry-run] $WingetCommand $($argv -join ' ')"
        return $true
    }

    & $WingetCommand @argv
    return ($LASTEXITCODE -eq 0)
}

$localAppData = Get-LocalAppData
if (-not $localAppData) {
    Write-Host "LOCALAPPDATA is not set; cannot continue."
    exit 1
}

$WindowsApps = Join-Path $localAppData "Microsoft\WindowsApps"
$WinGetLinks = Join-Path $localAppData "Microsoft\WinGet\Links"
$WinGetPackages = Join-Path $localAppData "Microsoft\WinGet\Packages"

if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path $WinGetLinks | Out-Null
}

$wingetCommand = Detect-Winget
$copilotPath = Resolve-CopilotExe

if (-not $copilotPath -and $Install) {
    if (-not $wingetCommand) {
        Write-Host "Copilot CLI is not installed and WinGet is unavailable."
        Write-Host "WinGet is provided by Windows Package Manager / App Installer."
        Write-Host "Install or repair App Installer / WinGet, then run:"
        Write-Host "  winget install GitHub.Copilot"
        exit 1
    }

    if (-not (Invoke-WingetInstall -WingetCommand $wingetCommand)) {
        Write-Host "WinGet install failed for GitHub.Copilot."
        exit 1
    }

    $copilotPath = Resolve-CopilotExe
}

if (-not $copilotPath) {
    Write-Host "Copilot CLI is not installed."
    Write-Host ""
    if ($wingetCommand) {
        Write-Host "Recommended Windows install:"
        Write-Host "  winget install GitHub.Copilot"
        Write-Host ""
        Write-Host "Or run:"
        Write-Host "  kog ai bootstrap copilot"
    } else {
        Write-Host "WinGet is unavailable. Install or repair App Installer / WinGet, then run:"
        Write-Host "  winget install GitHub.Copilot"
    }
    exit 1
}

$shimPath = Ensure-CopilotShim -TargetExe $copilotPath
$userPathValue = Ensure-UserPathEntries @($WindowsApps, $WinGetLinks)
$machinePathValue = [Environment]::GetEnvironmentVariable("Path", "Machine")
Write-MachinePathSink $machinePathValue

# Refresh current process PATH only; do not mutate Machine PATH.
$env:Path = Join-PathList @($machinePathValue, $userPathValue)

$resolvedNow = Resolve-CopilotExe

Write-Host "WindowsApps path:" $WindowsApps
Write-Host "WinGet Links path:" $WinGetLinks
if ($wingetCommand) {
    Write-Host "WinGet command:" $wingetCommand
} else {
    Write-Host "WinGet command: <unavailable>"
}
Write-Host "Copilot target:" $copilotPath
if ($shimPath) {
    Write-Host "Copilot shim:" $shimPath
}

if (-not $resolvedNow) {
    Write-Host "Copilot is still not resolvable in this process. Restart your shell and retry."
}

Write-Host ""
Write-Host "Verification commands:"
Write-Host "  copilot --version"
Write-Host "  copilot -s --model auto --no-color --stream off --no-ask-user -p \"Reply exactly: hello world\""