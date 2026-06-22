param(
    [Parameter(Mandatory = $true)]
    [string]$TestRoot,

    [string]$RepoRoot = $PSScriptRoot,

    [string]$CppBin = (Join-Path $PSScriptRoot 'src\cpp\build\bin\windows-ninja-msvc\release\kano-git.exe'),

    [string]$OutputPath = (Join-Path $PSScriptRoot 'bench_results.csv')
)

$shellRoot = Join-Path $RepoRoot 'src\shell'

# Ensure we are in the test root
Set-Location $TestRoot

function Run-Bench($label, $cmd) {
    Write-Host "Benchmarking $label..."
    $results = @()
    for ($i = 0; $i -lt 3; $i++) {
        $time = Measure-Command { & $cmd | Out-Null }
        $results += $time.TotalMilliseconds
    }
    $avg = ($results | Measure-Object -Average).Average
    Write-Host "$label Average: $avg ms"
    return [PSCustomObject]@{ Label = $label; Average = $avg }
}

$summary = @()

# 1. Discover
$summary += Run-Bench "C++ Discover" { & $cppBin workspace discover }
$summary += Run-Bench "Shell Discover" { bash "$shellRoot/core/discover-repos.sh" }

# 2. Status
$summary += Run-Bench "C++ Status" { & $cppBin workspace status }
$summary += Run-Bench "Shell Status" { bash "$shellRoot/workspace/status-all-repos.sh" }

# 3. Foreach
$summary += Run-Bench "C++ Foreach" { & $cppBin workspace foreach "git rev-parse HEAD" }
$summary += Run-Bench "Shell Foreach" { bash "$shellRoot/workspace/foreach-repo.sh" "git rev-parse HEAD" }
$summary += Run-Bench "Git Submodule Foreach" { git submodule foreach --recursive "git rev-parse HEAD" }

$summary | Export-Csv -Path $OutputPath -NoTypeInformation
Write-Host "Benchmark completed. Results saved to $OutputPath"
