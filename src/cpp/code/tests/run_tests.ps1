# Quick script to build and run kano-git C++ tests (Windows)

param(
    [string]$Preset = "windows-ninja-msvc-release",
    [switch]$WithE2E
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CppRoot = Resolve-Path (Join-Path $ScriptDir "../..")
$TestXmlOutput = $env:KANO_TEST_XML
$TestXmlDir = ""

if (-not [string]::IsNullOrWhiteSpace($env:KANO_REPORT_ROOT)) {
    if ([string]::IsNullOrWhiteSpace($env:KANO_BDD_METADATA_DIR)) {
        $env:KANO_BDD_METADATA_DIR = Join-Path $env:KANO_REPORT_ROOT "raw/bdd-metadata"
    }
    if (Test-Path -LiteralPath $env:KANO_BDD_METADATA_DIR) {
        Remove-Item -LiteralPath $env:KANO_BDD_METADATA_DIR -Recurse -Force
    }
    New-Item -ItemType Directory -Path $env:KANO_BDD_METADATA_DIR -Force | Out-Null
}

if (-not [string]::IsNullOrWhiteSpace($TestXmlOutput)) {
    $TestXmlParent = Split-Path -Parent $TestXmlOutput
    if ([string]::IsNullOrWhiteSpace($TestXmlParent)) {
        $TestXmlParent = "."
    }
    $TestXmlDir = Join-Path $TestXmlParent ".tmp-test-result"
    New-Item -ItemType Directory -Path $TestXmlDir -Force | Out-Null
}

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

function Invoke-TestBinary {
    param(
        [string]$BinaryName,
        [string]$ExecutablePath,
        [string[]]$Arguments = @()
    )

    $env:KANO_TEST_BINARY_NAME = $BinaryName
    & $ExecutablePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Test binary failed with exit code ${LASTEXITCODE}: $ExecutablePath"
    }
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
if (-not [string]::IsNullOrWhiteSpace($TestXmlDir)) {
    Invoke-TestBinary -BinaryName "kano_git_cli_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_cli_tests.exe") -Arguments @("--reporter", "junit", "--out", (Join-Path $TestXmlDir "kano_git_cli_tests.xml"))
} else {
    Invoke-TestBinary -BinaryName "kano_git_cli_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_cli_tests.exe")
}

Write-Host ""
Write-Host "Running TUI tests..."
if (-not [string]::IsNullOrWhiteSpace($TestXmlDir)) {
    Invoke-TestBinary -BinaryName "kano_git_tui_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_tui_tests.exe") -Arguments @("--reporter", "junit", "--out", (Join-Path $TestXmlDir "kano_git_tui_tests.xml"))
} else {
    Invoke-TestBinary -BinaryName "kano_git_tui_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_tui_tests.exe")
}

Write-Host ""
Write-Host "Running shell executor focused TUI tests..."
if (-not [string]::IsNullOrWhiteSpace($TestXmlDir)) {
    Invoke-TestBinary -BinaryName "kano_git_tui_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_tui_tests.exe") -Arguments @("[shell-executor]", "--reporter", "junit", "--out", (Join-Path $TestXmlDir "kano_git_tui_tests_shell_executor.xml"))
} else {
    Invoke-TestBinary -BinaryName "kano_git_tui_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_tui_tests.exe") -Arguments @("[shell-executor]")
}

Write-Host ""
Write-Host "Running commit plan tests..."
if (-not [string]::IsNullOrWhiteSpace($TestXmlDir)) {
    Invoke-TestBinary -BinaryName "kano_git_commit_plan_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_commit_plan_tests.exe") -Arguments @("--reporter", "junit", "--out", (Join-Path $TestXmlDir "kano_git_commit_plan_tests.xml"))
} else {
    Invoke-TestBinary -BinaryName "kano_git_commit_plan_tests" -ExecutablePath (Join-Path $ExeDir "kano_git_commit_plan_tests.exe")
}

Write-Host ""
Write-Host "All tests completed successfully!"

if ($WithE2E) {
    Write-Host ""
    Write-Host "Running E2E regression tests..."
    & ".\code\tests\e2e\plan_commit_regression\run.ps1" -WorkspaceRoot $WorkspaceRoot
}

if (-not [string]::IsNullOrWhiteSpace($TestXmlOutput)) {
    $outDoc = New-Object System.Xml.XmlDocument
    $declaration = $outDoc.CreateXmlDeclaration("1.0", "utf-8", $null)
    $outDoc.AppendChild($declaration) | Out-Null
    $root = $outDoc.CreateElement("testsuites")
    $outDoc.AppendChild($root) | Out-Null

    Get-ChildItem -LiteralPath $TestXmlDir -Filter "*.xml" | Sort-Object Name | ForEach-Object {
        $doc = New-Object System.Xml.XmlDocument
        $doc.Load($_.FullName)
        if ($doc.DocumentElement.Name -eq "testsuite") {
            $imported = $outDoc.ImportNode($doc.DocumentElement, $true)
            $root.AppendChild($imported) | Out-Null
        } elseif ($doc.DocumentElement.Name -eq "testsuites") {
            foreach ($suite in $doc.DocumentElement.SelectNodes("testsuite")) {
                $imported = $outDoc.ImportNode($suite, $true)
                $root.AppendChild($imported) | Out-Null
            }
        }
    }

    $TestXmlOutputParent = Split-Path -Parent $TestXmlOutput
    if ([string]::IsNullOrWhiteSpace($TestXmlOutputParent)) {
        $TestXmlOutputParent = "."
    }
    New-Item -ItemType Directory -Path $TestXmlOutputParent -Force | Out-Null
    $outDoc.Save($TestXmlOutput)
}
