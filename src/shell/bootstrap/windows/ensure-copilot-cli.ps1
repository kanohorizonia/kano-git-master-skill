# Add WinGet and Copilot CLI locations to User PATH persistently.
# Also create a stable copilot.cmd shim if WinGet did not create one.

$windowsApps = "$env:LOCALAPPDATA\Microsoft\WindowsApps"
$wingetLinks = "$env:LOCALAPPDATA\Microsoft\WinGet\Links"

# Ensure WinGet Links dir exists.
New-Item -ItemType Directory -Force -Path $wingetLinks | Out-Null

# Add paths to persistent User PATH.
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$pathList = @()
if ($userPath) {
    $pathList = $userPath -split ';' | Where-Object { $_ -ne "" }
}

foreach ($p in @($windowsApps, $wingetLinks)) {
    if ($pathList -notcontains $p) {
        $pathList += $p
    }
}

[Environment]::SetEnvironmentVariable("Path", ($pathList -join ';'), "User")

# Find installed Copilot CLI executable under WinGet package folder.
$copilotExe = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" `
    -Recurse `
    -Filter "copilot.exe" `
    -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 -ExpandProperty FullName

if (-not $copilotExe) {
    Write-Host "copilot.exe not found under WinGet packages."
    Write-Host "Try reinstalling:"
    Write-Host "  & `"$windowsApps\winget.exe`" install GitHub.Copilot"
    exit 1
}

# Create stable shim: %LOCALAPPDATA%\Microsoft\WinGet\Links\copilot.cmd
$copilotShim = Join-Path $wingetLinks "copilot.cmd"

@"
@echo off
"$copilotExe" %*
"@ | Set-Content -Encoding ASCII $copilotShim

# Refresh current PowerShell session PATH.
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")

Write-Host "Added to User PATH:"
Write-Host "  $windowsApps"
Write-Host "  $wingetLinks"
Write-Host ""
Write-Host "Copilot shim:"
Write-Host "  $copilotShim"
Write-Host ""
Write-Host "Copilot target:"
Write-Host "  $copilotExe"
Write-Host ""
Write-Host "Test:"
winget --version
copilot --version