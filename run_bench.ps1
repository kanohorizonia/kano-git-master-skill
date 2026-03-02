$testRoot = "D:\_work\_Kano\kano-benchmark-env\benchmark-root"
$cppBin = "D:\_work\_Kano\kano-git-master-skill\src\cpp\build\bin\windows-ninja-msvc\release\kano-git.exe"
$shellRoot = "D:\_work\_Kano\kano-git-master-skill\src\shell"

# Ensure we are in the test root
cd $testRoot

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

$summary | Export-Csv -Path "D:\_work\_Kano\kano-git-master-skill\bench_results.csv" -NoTypeInformation
Write-Host "Benchmark completed. Results saved to bench_results.csv"
