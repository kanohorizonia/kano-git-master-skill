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
        [hashtable]$EnvVars = @{}
    )
    $saved = @{}
    foreach ($k in $EnvVars.Keys) {
        $saved[$k] = (Get-Item -Path ("Env:" + $k) -ErrorAction SilentlyContinue).Value
        Set-Item -Path ("Env:" + $k) -Value ([string]$EnvVars[$k])
    }
    try {
        $raw = (& $Kog @CommandArgs 2>&1)
        $code = $LASTEXITCODE
        $output = ($raw | Out-String)
        return [pscustomobject]@{
            ExitCode = $code
            Output = $output
        }
    } finally {
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

$root = Resolve-WorkspaceRoot -InputRoot $WorkspaceRoot
$kog = Resolve-KogPath -Root $root -InputKog $KogPath
if (-not (Test-Path $kog)) {
    throw "kog launcher not found: $kog"
}

Push-Location $root
try {
    $planDir = ".kano/cache/git/plans"
    New-Item -ItemType Directory -Force -Path $planDir | Out-Null
    $planPath = Join-Path $planDir "e2e-regression-plan.json"
    $mutPath = Join-Path $planDir "e2e-regression-plan-mutated.json"

    $results = @()

    # T1: agent mode cpa guard
    $r1 = Invoke-Kog -Kog $kog -CommandArgs @("cpa", "--dry-run") -EnvVars @{ "KANO_AGENT_MODE" = "1" }
    if ($VerboseLog) { Write-Host $r1.Output }
    Assert-True ($r1.Output -match "requires either --plan-file or --message/-m") "T1 expected guard message"
    $results += [pscustomobject]@{ Test = "T1_agent_guard"; Pass = $true; Detail = "exit=$($r1.ExitCode) (message-asserted)" }

    # T2: plan new should have non-placeholder hashes
    $r2 = Invoke-Kog -Kog $kog -CommandArgs @("plan", "new", "-f", "-o", $planPath)
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
    $r3 = Invoke-Kog -Kog $kog -CommandArgs @("plan", "verify", "pre-apply", "--plan-file", $mutPath)
    if ($VerboseLog) { Write-Host $r3.Output }
    Assert-True ($r3.ExitCode -ne 0) "T3 expected non-zero exit code"
    Assert-True ($r3.Output -match "workspace state drift detected") "T3 expected drift message"
    $results += [pscustomobject]@{ Test = "T3_preapply_drift"; Pass = $true; Detail = "exit=$($r3.ExitCode)" }

    # T4: agent mode cpa with explicit message should work in dry-run
    $r4 = Invoke-Kog -Kog $kog -CommandArgs @("cpa", "-m", "test(e2e): dry-run smoke", "--dry-run") -EnvVars @{ "KANO_AGENT_MODE" = "1" }
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
