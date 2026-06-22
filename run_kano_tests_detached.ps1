$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$out = Join-Path $repo 'functional_test_summary.txt'
$log = Join-Path $repo 'functional_test_log.txt'
Set-Location $repo

$exe = Get-ChildItem -Path $repo -Recurse -File -Filter kano_git_cli_tests.exe | Select-Object -First 1
if (-not $exe) { throw 'kano_git_cli_tests.exe not found' }
$exePath = $exe.FullName
$exeDir = $exe.Directory.FullName

$listOutput = & $exePath --list-tests 2>&1
$lines = $listOutput -split "`r?`n"
$tests = New-Object System.Collections.Generic.List[string]
$pendingName = $null
foreach ($raw in $lines) {
  $line = $raw.Trim()
  if ([string]::IsNullOrWhiteSpace($line)) { continue }
  if ($line -match '^(=+|-+)$') { continue }
  if ($line -match '^(All available test cases:|Matching test cases:|Available test cases:|Test cases:|Tags:|Filters:|None)$') { continue }
  if ($line -match '^\[.*\]$') {
    if ($pendingName -and $line -match '\[functional\]') { [void]$tests.Add($pendingName) }
    $pendingName = $null
    continue
  }
  $pendingName = $line
}

$unique = New-Object System.Collections.Generic.List[string]
$seen = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($t in $tests) { if ($seen.Add($t)) { [void]$unique.Add($t) } }

$results = New-Object System.Collections.Generic.List[object]
foreach ($test in $unique) {
  $outFile = [System.IO.Path]::GetTempFileName()
  $errFile = [System.IO.Path]::GetTempFileName()
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = Start-Process -FilePath $exePath -ArgumentList @($test) -WorkingDirectory $exeDir -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $timedOut = $false
  while (-not $proc.HasExited) {
    if ($sw.Elapsed.TotalSeconds -ge 60) {
      $timedOut = $true
      try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
      break
    }
    Start-Sleep -Milliseconds 200
  }
  if (-not $timedOut) { $proc.WaitForExit() }
  $sw.Stop()
  $exitCode = $null
  if (-not $timedOut) {
    try { $exitCode = $proc.ExitCode } catch { $exitCode = $null }
  }
  [void]$results.Add([pscustomobject]@{ Test = $test; Timeout = $timedOut; ExitCode = $exitCode; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 3) })
  Remove-Item $outFile, $errFile -Force -ErrorAction SilentlyContinue
}

$failed = $results | Where-Object { -not $_.Timeout -and $_.ExitCode -ne 0 }
$timeouts = $results | Where-Object { $_.Timeout }
$slowest = $results | Sort-Object Seconds -Descending | Select-Object -First 5

$summary = @()
$summary += ('Total functional tests: {0}' -f $results.Count)
$summary += ('Failed ({0}): {1}' -f @($failed).Count, ($(if (@($failed).Count -gt 0) { (@($failed | ForEach-Object { '{0} [exit {1}]' -f $_.Test, $_.ExitCode }) -join ', ') } else { 'none' })))
$summary += ('Timeouts ({0}): {1}' -f @($timeouts).Count, ($(if (@($timeouts).Count -gt 0) { (@($timeouts | ForEach-Object { $_.Test }) -join ', ') } else { 'none' })))
$summary += 'Top 5 slowest tests:'
$rank = 1
foreach ($r in $slowest) { $summary += ('{0}. {1} - {2}s' -f $rank, $r.Test, $r.Seconds); $rank++ }
$summary -join "`r`n" | Set-Content -Path $out -NoNewline
