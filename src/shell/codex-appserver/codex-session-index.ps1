param(
    [string]$Filter = "",
    [switch]$IncludeArchived,
    [switch]$AsJson
)

$ErrorActionPreference = "Stop"
[Console]::InputEncoding  = [System.Text.UTF8Encoding]::new()
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

$CodexHome = if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $env:USERPROFILE ".codex" }
$IndexPath = Join-Path $CodexHome "session_index.jsonl"

if (-not (Test-Path $IndexPath)) {
    throw "Codex session index not found: $IndexPath"
}

$items = Get-Content $IndexPath -Encoding UTF8 |
    Where-Object { $_.Trim() } |
    ForEach-Object {
        try { $_ | ConvertFrom-Json -ErrorAction Stop } catch { $null }
    } |
    Where-Object { $null -ne $_ }

if (-not $IncludeArchived) {
    $items = $items | Where-Object {
        -not $_.archived -and
        -not $_.is_archived -and
        -not $_.archived_at
    }
}

if ($Filter) {
    $items = $items | Where-Object {
        $_.thread_name -match $Filter -or $_.id -match $Filter
    }
}

$result = $items |
    Sort-Object { [datetime]$_.updated_at } -Descending |
    Select-Object `
        @{Name="Updated"; Expression={ $_.updated_at }},
        @{Name="SessionId"; Expression={ $_.id }},
        @{Name="Name"; Expression={ $_.thread_name }},
        @{Name="Cwd"; Expression={ $_.cwd }}

if ($AsJson) {
    $result | ConvertTo-Json -Depth 20
} else {
    $result | Format-Table -AutoSize
}
