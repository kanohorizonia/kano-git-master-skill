param(
    [string]$UpgradeCode = "11111111-1111-1111-1111-111111111111",
    [string]$OutputDir = (Join-Path $PSScriptRoot "out"),
    [string]$OutputName = "kano-git-master-skill.msi",
    [string]$Architecture = "x64",
    [switch]$StageOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-MsiProductVersion {
    param(
        [Parameter(Mandatory)]
        [string]$CanonicalVersion
    )

    $match = [regex]::Match($CanonicalVersion.Trim(), '^(?<major>0|[1-9]\d*)\.(?<minor>0|[1-9]\d*)\.(?<patch>0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$')
    if (-not $match.Success) {
        throw "Canonical VERSION must be semver-like '<major>.<minor>.<patch>[-prerelease][+build]': $CanonicalVersion"
    }

    $major = [int]$match.Groups['major'].Value
    $minor = [int]$match.Groups['minor'].Value
    $patch = [int]$match.Groups['patch'].Value

    if ($major -gt 255) {
        throw "MSI ProductVersion major field must be <= 255: $CanonicalVersion"
    }

    if ($minor -gt 255) {
        throw "MSI ProductVersion minor field must be <= 255: $CanonicalVersion"
    }

    if ($patch -gt 65535) {
        throw "MSI ProductVersion build field must be <= 65535: $CanonicalVersion"
    }

    # Windows Installer ProductVersion comparisons use only three numeric fields.
    return "$major.$minor.$patch"
}

function Resolve-WixCommand {
    $toolPath = Join-Path $PSScriptRoot ".tools"
    foreach ($localLeaf in @("wix.exe", "wix.cmd")) {
        $localWix = Join-Path $toolPath $localLeaf
        if (Test-Path -LiteralPath $localWix -PathType Leaf) {
            return (Get-Item -LiteralPath $localWix).FullName
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($env:WIX)) {
        $candidate = $env:WIX.Trim()
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Get-Item -LiteralPath $candidate).FullName
        }

        if (Test-Path -LiteralPath $candidate -PathType Container) {
            foreach ($leaf in @("wix.exe", "wix.cmd", "bin\wix.exe", "bin\wix.cmd")) {
                $fullPath = Join-Path $candidate $leaf
                if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
                    return (Get-Item -LiteralPath $fullPath).FullName
                }
            }
        }
    }

    $command = Get-Command -Name "wix" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $dotnet = Get-Command -Name "dotnet" -ErrorAction SilentlyContinue
    if (-not $dotnet) {
        throw "WiX CLI not found, and dotnet is unavailable for repo-local bootstrap. Install WiX v4+ so 'wix' is on PATH, or set WIX to the wix executable path."
    }

    New-Item -ItemType Directory -Force -Path $toolPath | Out-Null
    & $dotnet.Source tool install --tool-path $toolPath wix --version 6.0.2
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install repo-local WiX CLI via dotnet tool install."
    }

    foreach ($localLeaf in @("wix.exe", "wix.cmd")) {
        $localWix = Join-Path $toolPath $localLeaf
        if (Test-Path -LiteralPath $localWix -PathType Leaf) {
            return (Get-Item -LiteralPath $localWix).FullName
        }
    }

    throw "WiX CLI bootstrap completed, but no wix executable was found in $toolPath."
}

function Ensure-WixExtension {
    param(
        [Parameter(Mandatory)]
        [string]$WixCommand,

        [Parameter(Mandatory)]
        [string]$ExtensionId
    )

    $extensionRef = "$ExtensionId/6.0.2"

    $global:LASTEXITCODE = 0
    & $WixCommand extension list | Out-String | Select-String -SimpleMatch $ExtensionId | Out-Null
    $listExitCode = $LASTEXITCODE
    if ($listExitCode -eq 0) {
        return
    }

    $global:LASTEXITCODE = 0
    & $WixCommand extension add $extensionRef
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install WiX extension: $extensionRef"
    }
}

function Remove-DirectoryIfExists {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function New-CmdWrapper {
    param(
        [Parameter(Mandatory)]
        [string]$WrapperPath,

        [Parameter(Mandatory)]
        [string]$TargetScriptName
    )

    $content = @(
        '@echo off',
        'setlocal',
        'set "SCRIPT_DIR=%~dp0"',
        'if defined GIT_BASH_PATH if exist "%GIT_BASH_PATH%" goto run',
        'if exist "%ProgramFiles%\Git\bin\bash.exe" set "GIT_BASH_PATH=%ProgramFiles%\Git\bin\bash.exe"',
        'if not defined GIT_BASH_PATH if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" set "GIT_BASH_PATH=%ProgramFiles(x86)%\Git\bin\bash.exe"',
        'if not defined GIT_BASH_PATH (',
        '  echo Error: Git for Windows bash.exe was not found.>&2',
        '  exit /b 1',
        ')',
        ':run',
        ('"%GIT_BASH_PATH%" "%SCRIPT_DIR%' + $TargetScriptName + '" %*')
    ) -join [Environment]::NewLine

    Set-Content -LiteralPath $WrapperPath -Value $content -NoNewline
}

function Stage-Payload {
    param(
        [Parameter(Mandatory)]
        [string]$RepoRoot,

        [Parameter(Mandatory)]
        [string]$PayloadRoot,

        [Parameter(Mandatory)]
        [string]$ArchitectureName
    )

    Remove-DirectoryIfExists -Path $PayloadRoot
    New-Item -ItemType Directory -Path $PayloadRoot | Out-Null

    foreach ($fileName in @("VERSION", "README.md", "SKILL.md")) {
        $sourcePath = Join-Path $RepoRoot $fileName
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Required payload file missing: $sourcePath"
        }
        Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $PayloadRoot $fileName)
    }

    $copyDirs = @(
        @{ Name = "assets"; Exclude = @() },
        @{ Name = "scripts"; Exclude = @() }
    )

    foreach ($dirSpec in $copyDirs) {
        $sourceDir = Join-Path $RepoRoot $dirSpec.Name
        if (-not (Test-Path -LiteralPath $sourceDir -PathType Container)) {
            continue
        }

        $destDir = Join-Path $PayloadRoot $dirSpec.Name
        Copy-Item -LiteralPath $sourceDir -Destination $destDir -Recurse

        foreach ($excludeRelative in $dirSpec.Exclude) {
            $excludePath = Join-Path $destDir $excludeRelative
            Remove-DirectoryIfExists -Path $excludePath
        }
    }

    $binarySource = Join-Path $RepoRoot "src\cpp\build\bin\windows-ninja-msvc\release\kano-git.exe"
    if ($ArchitectureName -ne "x64") {
        throw "Unsupported MSI architecture for payload staging: $ArchitectureName"
    }
    if (-not (Test-Path -LiteralPath $binarySource -PathType Leaf)) {
        throw "Required native binary is missing: $binarySource. Build the Windows release binary before running src/wix/build.ps1."
    }

    $binDir = Join-Path $PayloadRoot "bin"
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    Copy-Item -LiteralPath $binarySource -Destination (Join-Path $binDir "kano-git.exe")

    $scriptsDir = Join-Path $PayloadRoot "scripts"
    New-Item -ItemType Directory -Force -Path $scriptsDir | Out-Null
    New-CmdWrapper -WrapperPath (Join-Path $scriptsDir "kano-git.cmd") -TargetScriptName "kano-git"
    New-CmdWrapper -WrapperPath (Join-Path $scriptsDir "kog.cmd") -TargetScriptName "kog"
    New-CmdWrapper -WrapperPath (Join-Path $scriptsDir "kano-git-installer.cmd") -TargetScriptName "kano-git-installer"
    New-CmdWrapper -WrapperPath (Join-Path $scriptsDir "kog-installer.cmd") -TargetScriptName "kog-installer"
}

$productFile = Join-Path $PSScriptRoot "Product.wxs"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$canonicalVersionFile = Join-Path $repoRoot "VERSION"
$payloadRoot = Join-Path $OutputDir "payload"

if (-not (Test-Path -LiteralPath $productFile)) {
    throw "WiX entrypoint not found: $productFile"
}

if (-not (Test-Path -LiteralPath $canonicalVersionFile)) {
    throw "Canonical VERSION file not found: $canonicalVersionFile"
}

$canonicalVersion = (Get-Content -LiteralPath $canonicalVersionFile -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($canonicalVersion)) {
    throw "Canonical VERSION file is empty: $canonicalVersionFile"
}

$productVersion = ConvertTo-MsiProductVersion -CanonicalVersion $canonicalVersion

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
Stage-Payload -RepoRoot $repoRoot -PayloadRoot $payloadRoot -ArchitectureName $Architecture
$outputPath = Join-Path $OutputDir $OutputName

$arguments = @(
    "build"
    $productFile
    "-ext"
    "WixToolset.Util.wixext"
    "-arch"
    $Architecture
    "-o"
    $outputPath
    "-d"
    "ProductVersion=$productVersion"
    "-d"
    "UpgradeCode=$UpgradeCode"
    "-d"
    "PayloadRoot=$payloadRoot"
)

Write-Host "WiX v4 build scaffold"
Write-Host "  Entrypoint : $productFile"
Write-Host "  Output     : $outputPath"
Write-Host "  Payload    : $payloadRoot"
Write-Host "  VERSION    : $canonicalVersion -> MSI $productVersion"
Write-Host "  Rule       : MSI ProductVersion uses only the first three numeric fields"
Write-Host "  Scope      : per-user only"
Write-Host "  UpgradeCode: $UpgradeCode"

if ($StageOnly) {
    Write-Host "Payload staged only; skipping wix build."
    return
}

$wixCommand = Resolve-WixCommand
Ensure-WixExtension -WixCommand $wixCommand -ExtensionId "WixToolset.Util.wixext"
$global:LASTEXITCODE = 0
& $wixCommand @arguments
$wixExitCodeVariable = Get-Variable -Name LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
if ($wixExitCodeVariable -and $wixExitCodeVariable.Value -ne 0) {
    throw "wix build failed with exit code $($wixExitCodeVariable.Value)"
}

Write-Host "MSI created: $outputPath"
