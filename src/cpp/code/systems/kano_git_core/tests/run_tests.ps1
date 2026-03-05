# Quick script to build and run TUI command input enhancement tests (Windows)

param(
    [string]$Preset = "windows-ninja-msvc"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CppRoot = Resolve-Path (Join-Path $ScriptDir "../../../..")

Write-Host "Building TUI tests with preset: $Preset"
Set-Location $CppRoot

# Configure if needed
$BuildDir = "build/_intermediate/$Preset"
if (-not (Test-Path $BuildDir)) {
    Write-Host "Configuring CMake with preset $Preset..."
    cmake --preset $Preset
}

# Build test targets
Write-Host "Building test targets..."
cmake --build --preset $Preset --target tui_unit_tests tui_property_tests tui_integration_tests

# Run tests
Write-Host ""
Write-Host "Running unit tests..."
& ".\build\bin\$Preset\tui_unit_tests.exe"

Write-Host ""
Write-Host "Running shell executor focused tests..."
& ".\build\bin\$Preset\tui_unit_tests.exe" "[shell-executor]"

Write-Host ""
Write-Host "Running property tests..."
& ".\build\bin\$Preset\tui_property_tests.exe"

Write-Host ""
Write-Host "Running integration tests..."
& ".\build\bin\$Preset\tui_integration_tests.exe"

Write-Host ""
Write-Host "All tests completed successfully!"
