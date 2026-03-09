param(
    [string]$WorkspaceRoot = "",
    [string]$KogPath = "",
    [switch]$VerboseLog
)

$ErrorActionPreference = "Stop"

function Resolve-WorkspaceRoot {
    param([string]$InputRoot)
    if (-not [string]::IsNullOrWhiteSpace($InputRoot)) {
        return (Resolve-Path $InputRoot).Path
    }
    $cursor = (Resolve-Path $PSScriptRoot).Path
    while ($true) {
        $candidate = Join-Path $cursor "kog"
        if (Test-Path $candidate) {
            return $cursor
        }
        $parent = Split-Path -Parent $cursor
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cursor) {
            break
        }
        $cursor = (Resolve-Path $parent).Path
    }
    throw "Cannot resolve workspace root from script path: $PSScriptRoot"
}

function Resolve-KogPath {
    param([string]$Root, [string]$InputKog)
    if (-not [string]::IsNullOrWhiteSpace($InputKog)) {
        return (Resolve-Path $InputKog).Path
    }
    if ($IsWindows -and (Test-Path (Join-Path $Root "kog.ps1"))) {
        return (Join-Path $Root "kog.ps1")
    }
    if ($IsWindows -and (Test-Path (Join-Path $Root "kog.cmd"))) {
        return (Join-Path $Root "kog.cmd")
    }
    return (Join-Path $Root "kog")
}

function Invoke-Kog {
    param(
        [string]$Kog,
        [string[]]$CommandArgs,
        [hashtable]$EnvVars = @{},
        [string]$WorkingDir = ""
    )
    $saved = @{}
    foreach ($k in $EnvVars.Keys) {
        $saved[$k] = (Get-Item -Path ("Env:" + $k) -ErrorAction SilentlyContinue).Value
        Set-Item -Path ("Env:" + $k) -Value ([string]$EnvVars[$k])
    }
    try {
        $cwd = $null
        if (-not [string]::IsNullOrWhiteSpace($WorkingDir)) {
            $cwd = Get-Location
            Set-Location $WorkingDir
        }
        $raw = (& $Kog @CommandArgs 2>&1)
        $code = $LASTEXITCODE
        $output = ($raw | Out-String)
        return [pscustomobject]@{
            ExitCode = $code
            Output = $output
        }
    } finally {
        if ($null -ne $cwd) {
            Set-Location $cwd
        }
        foreach ($k in $EnvVars.Keys) {
            if ($null -eq $saved[$k]) {
                Remove-Item -Path ("Env:" + $k) -ErrorAction SilentlyContinue
            } else {
                Set-Item -Path ("Env:" + $k) -Value $saved[$k]
            }
        }
    }
}

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function New-E2ESandbox {
    param([string]$Root)
    $base = Join-Path $Root ".kano/tmp/git/e2e"
    New-Item -ItemType Directory -Force -Path $base | Out-Null
    $name = "plan-commit-regression-" + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $sandbox = Join-Path $base $name
    $remote = Join-Path $sandbox "remote.git"
    $repo = Join-Path $sandbox "work"
    New-Item -ItemType Directory -Force -Path $sandbox | Out-Null

    $null = & git init --bare $remote 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to init bare remote: $remote" }
    $null = & git init --initial-branch main $repo 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Failed to init repo: $repo" }
    Push-Location $repo
    try {
        $null = & git config user.name "kano e2e" 2>&1
        $null = & git config user.email "kano-e2e@example.invalid" 2>&1
        Set-Content -Path "README.md" -Value "seed`n" -NoNewline
        $null = & git add README.md 2>&1
        $null = & git commit -m "test(e2e): seed" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Failed to create seed commit" }
        $null = & git remote add origin $remote 2>&1
        $null = & git push -u origin main 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Failed to push seed commit" }
        Add-Content -Path "README.md" -Value "dirty"
    } finally {
        Pop-Location
    }
    return $repo
}

$root = Resolve-WorkspaceRoot -InputRoot $WorkspaceRoot
$kog = Resolve-KogPath -Root $root -InputKog $KogPath
if (-not (Test-Path $kog)) {
    throw "kog launcher not found: $kog"
}

Push-Location $root
try {
    $sandboxRepo = New-E2ESandbox -Root $root
    $planDir = ".kano/tmp/git/plans"
    New-Item -ItemType Directory -Force -Path (Join-Path $sandboxRepo $planDir) | Out-Null
    $planPath = Join-Path $sandboxRepo (Join-Path $planDir "e2e-regression-plan.json")
    $mutPath = Join-Path $sandboxRepo (Join-Path $planDir "e2e-regression-plan-mutated.json")

    $results = @()

    # T1: agent mode cpa guard
    $r1 = Invoke-Kog -Kog $kog -CommandArgs @("cpa", "--dry-run") -EnvVars @{ "KANO_AGENT_MODE" = "1" } -WorkingDir $sandboxRepo
    if ($VerboseLog) { Write-Host $r1.Output }
    Assert-True ($r1.Output -match "requires either --plan-file or --message/-m") "T1 expected guard message"
    $results += [pscustomobject]@{ Test = "T1_agent_guard"; Pass = $true; Detail = "exit=$($r1.ExitCode) (message-asserted)" }

    # T2: plan new should have non-placeholder hashes
    $r2 = Invoke-Kog -Kog $kog -CommandArgs @("plan", "new", "-f", "-o", $planPath) -WorkingDir $sandboxRepo
    if ($VerboseLog) { Write-Host $r2.Output }
    Assert-True ($r2.ExitCode -eq 0) "T2 plan new failed: $($r2.Output)"
    $j2 = Get-Content -Raw $planPath | ConvertFrom-Json
    $bh = [string]$j2.meta.base_head_sha
    $df = [string]$j2.meta.dirty_fingerprint
    Assert-True (-not [string]::IsNullOrWhiteSpace($bh)) "T2 base_head_sha empty"
    Assert-True (-not [string]::IsNullOrWhiteSpace($df)) "T2 dirty_fingerprint empty"
    Assert-True (-not $bh.StartsWith("replace-with-")) "T2 base_head_sha placeholder"
    Assert-True (-not $df.StartsWith("replace-with-")) "T2 dirty_fingerprint placeholder"
    $results += [pscustomobject]@{ Test = "T2_plan_new_hash"; Pass = $true; Detail = "base=$bh dirty=$df" }

    # T3: pre-apply verify should reject base_head_sha drift
    Copy-Item $planPath $mutPath -Force
    $raw = Get-Content -Raw $mutPath
    $raw = [regex]::Replace($raw, '("base_head_sha"\s*:\s*")[^"]*(")', '$1deadbeef$2', 1)
    Set-Content -Path $mutPath -Value $raw -NoNewline
    $r3 = Invoke-Kog -Kog $kog -CommandArgs @("plan", "verify", "pre-apply", "--plan-file", $mutPath) -WorkingDir $sandboxRepo
    if ($VerboseLog) { Write-Host $r3.Output }
    Assert-True ($r3.ExitCode -ne 0) "T3 expected non-zero exit code"
    Assert-True ($r3.Output -match "workspace state drift detected") "T3 expected drift message"
    $results += [pscustomobject]@{ Test = "T3_preapply_drift"; Pass = $true; Detail = "exit=$($r3.ExitCode)" }

    # T4: agent mode cpa with explicit message should work in dry-run
    $r4 = Invoke-Kog -Kog $kog -CommandArgs @("cpa", "-m", "test(e2e): dry-run smoke", "--dry-run") -EnvVars @{ "KANO_AGENT_MODE" = "1" } -WorkingDir $sandboxRepo
    if ($VerboseLog) { Write-Host $r4.Output }
    Assert-True ($r4.ExitCode -eq 0) "T4 expected success: $($r4.Output)"
    $results += [pscustomobject]@{ Test = "T4_cpa_dryrun_smoke"; Pass = $true; Detail = "exit=0" }

    Write-Host ""
    Write-Host "E2E regression tests passed:"
    $results | Format-Table -AutoSize
    exit 0
}
finally {
    Pop-Location
}
