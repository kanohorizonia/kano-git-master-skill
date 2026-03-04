param(
  [Parameter(Mandatory = $true)]
  [string]$Action,

  [string]$Path = "",
  [string]$Root = "",
  [string]$Preset = "",
  [string]$PreferredDrive = "",
  [string]$Mode = "auto",
  [string]$Drive = "",
  [string]$Vcvars = "",
  [string]$Arch = "x64",
  [string]$ConfigurePreset = "",
  [string]$BuildPreset = "",
  [string]$VcvarsVersion = "14.44.35207"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Normalize-PathKey([string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) {
    return ""
  }

  $candidate = $Value.Trim()
  $resolved = Resolve-Path -LiteralPath $candidate -ErrorAction SilentlyContinue
  if ($resolved) {
    $candidate = $resolved.Path
  }

  $candidate = $candidate -replace "/", "\\"
  while ($candidate.Length -gt 3 -and $candidate.EndsWith("\")) {
    $candidate = $candidate.Substring(0, $candidate.Length - 1)
  }
  return $candidate.ToLowerInvariant()
}

function Resolve-AbsoluteWindowsPath([string]$Value) {
  if ([string]::IsNullOrWhiteSpace($Value)) {
    return ""
  }

  $candidate = $Value.Trim()
  $resolved = Resolve-Path -LiteralPath $candidate -ErrorAction SilentlyContinue
  if ($resolved) {
    $candidate = $resolved.Path
  }

  $candidate = $candidate -replace "/", "\\"
  while ($candidate.Length -gt 3 -and $candidate.EndsWith("\")) {
    $candidate = $candidate.Substring(0, $candidate.Length - 1)
  }
  return $candidate
}

function Detect-Vcvarsall {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path -LiteralPath $vswhere) {
    $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "VC\Auxiliary\Build\vcvarsall.bat" 2>$null |
      Select-Object -First 1
    if ($found) {
      Write-Output $found
      return
    }
  }

  $roots = @()
  if ($env:ProgramFiles) {
    $roots += (Join-Path $env:ProgramFiles "Microsoft Visual Studio")
  }
  if (${env:ProgramFiles(x86)}) {
    $roots += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio")
  }
  $roots = $roots | Where-Object { Test-Path -LiteralPath $_ }
  if ($roots.Count -eq 0) {
    return
  }

  $scan = Get-ChildItem -Path $roots -Recurse -File -Filter vcvarsall.bat -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\VC\\Auxiliary\\Build\\vcvarsall\.bat$" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
  if ($scan) {
    Write-Output $scan
  }
}

function Resolve-SourceRoot([string]$InRoot, [string]$InPreset) {
  $decision = "keep"
  $effectiveRoot = Resolve-AbsoluteWindowsPath $InRoot

  if ([string]::IsNullOrWhiteSpace($effectiveRoot)) {
    $effectiveRoot = $InRoot
  }

  $buildDir = Join-Path $effectiveRoot ("build\_intermediate\" + $InPreset)
  $cacheFile = Join-Path $buildDir "CMakeCache.txt"
  if (Test-Path -LiteralPath $cacheFile) {
    $cacheDirLine = Select-String -Path $cacheFile -Pattern "^CMAKE_CACHEFILE_DIR:INTERNAL=(.+)$" -ErrorAction SilentlyContinue |
      Select-Object -First 1
    $homeDirLine = Select-String -Path $cacheFile -Pattern "^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$" -ErrorAction SilentlyContinue |
      Select-Object -First 1

    $cachedDir = ""
    $cachedHome = ""
    if ($cacheDirLine -and $cacheDirLine.Matches.Count -gt 0) {
      $cachedDir = $cacheDirLine.Matches[0].Groups[1].Value.Trim()
    }
    if ($homeDirLine -and $homeDirLine.Matches.Count -gt 0) {
      $cachedHome = $homeDirLine.Matches[0].Groups[1].Value.Trim()
    }

    $buildKey = Normalize-PathKey $buildDir
    $cachedKey = Normalize-PathKey $cachedDir
    if (-not [string]::IsNullOrWhiteSpace($cachedKey) -and $cachedKey -ne $buildKey) {
      if (-not [string]::IsNullOrWhiteSpace($cachedHome) -and (Test-Path -LiteralPath $cachedHome)) {
        $decision = "use-cache-home"
        $effectiveRoot = $cachedHome
      } else {
        $decision = "clean-cache"
        Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
      }
    }
  }

  Write-Output ($decision + "|" + $effectiveRoot)
}

function Prepare-SubstRoot([string]$InRoot, [string]$InPreferredDrive, [string]$InMode) {
  $tab = [char]9
  $root = Resolve-AbsoluteWindowsPath $InRoot
  if ([string]::IsNullOrWhiteSpace($root)) {
    $root = $InRoot
  }

  $modeNorm = $InMode.Trim().ToLowerInvariant()
  if ([string]::IsNullOrWhiteSpace($modeNorm)) {
    $modeNorm = "auto"
  }

  $shouldMap = $false
  if ($modeNorm -eq "on") {
    $shouldMap = $true
  } elseif ($modeNorm -eq "off") {
    $shouldMap = $false
  } else {
    $shouldMap = ($root.Length -ge 55)
  }
  if (-not [string]::IsNullOrWhiteSpace($InPreferredDrive)) {
    $shouldMap = $true
  }

  if (-not $shouldMap) {
    Write-Output ($root + $tab + $tab + "0")
    return
  }

  $substMap = @{}
  cmd /d /c subst | ForEach-Object {
    if ($_ -match "^([A-Z]):\\:\s*=>\s*(.+)$") {
      $substMap[$matches[1].ToUpperInvariant()] = Resolve-AbsoluteWindowsPath $matches[2].Trim()
    }
  }

  foreach ($k in @($substMap.Keys)) {
    if ($substMap[$k].ToLowerInvariant() -eq $root.ToLowerInvariant()) {
      Write-Output (($k + ":\") + $tab + ($k + ":") + $tab + "0")
      return
    }
  }

  $preferred = $null
  if ($InPreferredDrive -match "^[A-Za-z]$") {
    $preferred = $InPreferredDrive.ToUpperInvariant()
  } elseif ($InPreferredDrive -match "^([A-Za-z]):$") {
    $preferred = $Matches[1].ToUpperInvariant()
  }

  $candidates = New-Object System.Collections.Generic.List[string]
  if ($preferred) {
    [void]$candidates.Add($preferred)
  }
  foreach ($letter in @("Z","Y","X","W","V","U","T","S","R","Q","P","O","N","M","L","K","J","I","H","G","F","E","D")) {
    [void]$candidates.Add($letter)
  }

  $usedLetters = New-Object System.Collections.Generic.HashSet[string]
  Get-PSDrive -PSProvider FileSystem | ForEach-Object { [void]$usedLetters.Add($_.Name.ToUpperInvariant()) }
  foreach ($k in $substMap.Keys) {
    [void]$usedLetters.Add($k)
  }

  foreach ($driveLetter in $candidates) {
    if ($usedLetters.Contains($driveLetter)) {
      continue
    }
    $target = $driveLetter + ":"
    & cmd /d /c subst $target $root | Out-Null
    if (Test-Path -LiteralPath ($target + "\")) {
      Write-Output (($target + "\") + $tab + $target + $tab + "1")
      return
    }
  }

  Write-Output ($root + $tab + $tab + "0")
}

function Run-Preset([string]$InRoot,
                    [string]$InVcvars,
                    [string]$InArch,
                    [string]$InConfigurePreset,
                    [string]$InBuildPreset,
                    [string]$InVcvarsVersion) {
  Set-Location -LiteralPath $InRoot
  $quotedVcvars = '"' + $InVcvars + '"'
  $cmd = "call $quotedVcvars $InArch -vcvars_ver=$InVcvarsVersion && cmake --preset $InConfigurePreset && cmake --build --preset $InBuildPreset"
  cmd.exe /d /s /c $cmd
  exit $LASTEXITCODE
}

switch ($Action.ToLowerInvariant()) {
  "test-path" {
    if (Test-Path -LiteralPath $Path) { exit 0 } else { exit 1 }
  }
  "detect-vcvarsall" {
    Detect-Vcvarsall
    exit 0
  }
  "resolve-source-root" {
    Resolve-SourceRoot $Root $Preset
    exit 0
  }
  "prepare-subst-root" {
    Prepare-SubstRoot $Root $PreferredDrive $Mode
    exit 0
  }
  "cleanup-subst" {
    if (-not [string]::IsNullOrWhiteSpace($Drive)) {
      $targetDrive = $Drive.Trim()
      if ($targetDrive -match "^[A-Za-z]$") {
        $targetDrive = $targetDrive + ":"
      }
      & cmd /d /c subst $targetDrive /d | Out-Null
    }
    exit 0
  }
  "run-preset" {
    Run-Preset $Root $Vcvars $Arch $ConfigurePreset $BuildPreset $VcvarsVersion
  }
  default {
    Write-Error "Unknown action: $Action"
    exit 2
  }
}
