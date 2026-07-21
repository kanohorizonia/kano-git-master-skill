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
        $candidate = Join-Path $cursor "scripts/kog"
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
    if ($IsWindows -and (Test-Path (Join-Path $Root "scripts/kog.bat"))) {
        return (Join-Path $Root "scripts/kog.bat")
    }
    return (Join-Path $Root "scripts/kog")
}

function Invoke-Kog {
    param(
        [string]$Kog,
        [string[]]$CommandArgs,
        [hashtable]$EnvVars = @{},
        [string]$WorkingDir = ""
    )
    $saved = @{}
    $savedErrorActionPreference = $ErrorActionPreference
    foreach ($k in $EnvVars.Keys) {
        $saved[$k] = (Get-Item -Path ("Env:" + $k) -ErrorAction SilentlyContinue).Value
        Set-Item -Path ("Env:" + $k) -Value ([string]$EnvVars[$k])
    }
    try {
        # Expected non-zero native commands emit useful stderr; capture it without
        # letting PowerShell promote a native stderr record to a terminating error.
        $ErrorActionPreference = "Continue"
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
        $ErrorActionPreference = $savedErrorActionPreference
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

    $null = & git init --quiet --bare --initial-branch main $remote 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Failed to init bare remote: $remote" }
    $null = & git init --quiet --initial-branch main $repo 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Failed to init repo: $repo" }
    Push-Location $repo
    try {
        $null = & git config user.name "kano e2e" 2>$null
        $null = & git config user.email "kano-e2e@example.invalid" 2>$null
        Set-Content -Path "README.md" -Value "seed`n" -NoNewline
        $null = & git add README.md 2>$null
        $null = & git commit --quiet -m "test(e2e): seed" 2>$null
        if ($LASTEXITCODE -ne 0) { throw "Failed to create seed commit" }
        $null = & git remote add origin $remote 2>$null
        $null = & git push --quiet -u origin main 2>$null
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

    # T1: agent mode cpa deterministically bootstraps the shared plan and stops for agent input
    $defaultPlanPath = Join-Path $sandboxRepo (Join-Path $planDir "default-plan.json")
    $r1 = Invoke-Kog -Kog $kog -CommandArgs @("cpa") -EnvVars @{ "KANO_AGENT_MODE" = "1" } -WorkingDir $sandboxRepo
    if ($VerboseLog) { Write-Host $r1.Output }
    Assert-True ($r1.ExitCode -eq 3) "T1 expected explicit agent-plan-required exit 3: $($r1.Output)"
    Assert-True ($r1.Output -match "agent mode \+ --plan-file detected; using plan-driven flow") "T1 expected plan-driven route"
    Assert-True ($r1.Output -match "refresh-needed: missing-or-unreadable") "T1 expected missing-plan refresh diagnostic"
    Assert-True ($r1.Output -match "\[AGENT_PLAN_REQUIRED\]") "T1 expected agent plan required diagnostic"
    Assert-True ($r1.Output -notmatch "stage=commit-runbook") "T1 must not invoke internal provider planning"
    Assert-True (Test-Path $defaultPlanPath) "T1 shared default plan was not created"
    $j1 = Get-Content -Raw $defaultPlanPath | ConvertFrom-Json
    Assert-True (([string]$j1.meta.base_head_sha).StartsWith("ws-head-v2-")) "T1 base_head_sha is not deterministic"
    Assert-True (([string]$j1.meta.dirty_fingerprint).StartsWith("ws-dirty-v2-")) "T1 dirty_fingerprint is not deterministic"
    Assert-True ([string]$j1.meta.planner.provider -eq "agent") "T1 planner provider is not agent"
    Assert-True ([string]$j1.meta.review.verdict -eq "pass") "T1 plan review metadata is incomplete"
    Assert-True ([string]$j1.stages.commit[0].commits[0].message -eq "replace-with-commit-message") "T1 deterministic commit seed is missing"
    $results += [pscustomobject]@{ Test = "T1_agent_shared_plan"; Pass = $true; Detail = "exit=3 metadata=complete" }

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
    Assert-True ($r4.Output -notmatch "--plan-file cannot be combined") "T4 explicit message was combined with a plan file"
    Assert-True ($r4.Output -notmatch "default-plan.json") "T4 explicit message unexpectedly used the shared plan"
    $results += [pscustomobject]@{ Test = "T4_cpa_dryrun_smoke"; Pass = $true; Detail = "exit=0" }

    Write-Host ""
    Write-Host "E2E regression tests passed:"
    $results | Format-Table -AutoSize
    exit 0
}
finally {
    Pop-Location
}
