# Quick script to build and run kano-git C++ tests (Windows)

param(
    [string]$Preset = "windows-ninja-msvc-release",
    [switch]$WithE2E
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CppRoot = Resolve-Path (Join-Path $ScriptDir "../..")

function Resolve-WorkspaceRoot {
    param([string]$StartDir)
    $cursor = (Resolve-Path $StartDir).Path
    while ($true) {
        if (Test-Path (Join-Path $cursor "scripts\kog.bat")) {
            return $cursor
        }
        $parent = Split-Path -Parent $cursor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cursor) {
            break
        }
        $cursor = $parent
    }
    return ""
}

function Resolve-BinDir {
    param(
        [string]$CppDir,
        [string]$PresetName
    )
    $direct = Join-Path $CppDir "out/bin/$PresetName"
    if (Test-Path $direct) {
        return $direct
    }
    $canonical = $PresetName -replace '-(debug|release|relwithdebinfo|minsizerel)$', ''
    $fallback = Join-Path $CppDir "out/bin/$canonical"
    if (Test-Path $fallback) {
        return $fallback
    }
    throw "Cannot resolve out/bin directory for preset: $PresetName"
}

Write-Host "Building kano-git tests with preset: $Preset"
Set-Location $CppRoot

$WorkspaceRoot = Resolve-WorkspaceRoot -StartDir $CppRoot
if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    throw "Cannot resolve workspace root containing kog launcher"
}

Write-Host "Building via kog self build..."
& (Join-Path $WorkspaceRoot "scripts\kog.bat") self build

$BinDir = Resolve-BinDir -CppDir $CppRoot -PresetName $Preset
$ExeDir = Join-Path $BinDir "release"

# Run tests
Write-Host ""
Write-Host "Running CLI tests..."
& (Join-Path $ExeDir "kano_git_cli_tests.exe")

Write-Host ""
Write-Host "Running TUI tests..."
& (Join-Path $ExeDir "kano_git_tui_tests.exe")

Write-Host ""
Write-Host "Running shell executor focused TUI tests..."
& (Join-Path $ExeDir "kano_git_tui_tests.exe") "[shell-executor]"

Write-Host ""
Write-Host "All tests completed successfully!"

if ($WithE2E) {
    Write-Host ""
    Write-Host "Running E2E regression tests..."
    & ".\code\tests\e2e\plan_commit_regression\run.ps1" -WorkspaceRoot $WorkspaceRoot
}
