param(
    [string]$CodexHome = (Join-Path $env:USERPROFILE ".codex"),
    [string]$Filter = "",
    [int]$Limit = 30,
    [switch]$IncludeArchived,
    [switch]$AsJson
)

$ErrorActionPreference = "Stop"

function Get-EntryString {
    param(
        [object]$Entry,
        [string[]]$Names
    )

    foreach ($name in $Names) {
        if ($Entry.PSObject.Properties.Name -contains $name -and $null -ne $Entry.$name) {
            return [string]$Entry.$name
        }
    }
    return ""
}

function Get-EntryBool {
    param(
        [object]$Entry,
        [string[]]$Names
    )

    foreach ($name in $Names) {
        if ($Entry.PSObject.Properties.Name -contains $name -and $null -ne $Entry.$name) {
            return [bool]$Entry.$name
        }
    }
    return $false
}

function Get-EntryUpdatedAt {
    param([object]$Entry)

    $value = Get-EntryString $Entry @("updated_at", "updatedAt", "last_updated_at", "lastUpdatedAt")
    if ([string]::IsNullOrWhiteSpace($value)) {
        return [datetime]::MinValue
    }

    try {
        return [datetime]$value
    } catch {
        return [datetime]::MinValue
    }
}

$indexPath = Join-Path $CodexHome "session_index.jsonl"
if (-not (Test-Path -LiteralPath $indexPath -PathType Leaf)) {
    throw "Codex session index not found: $indexPath"
}

$latestById = @{}
$lineNumber = 0
foreach ($line in Get-Content -LiteralPath $indexPath -Encoding UTF8) {
    $lineNumber++
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }

    try {
        $entry = $line | ConvertFrom-Json
    } catch {
        Write-Warning "Skipping malformed session_index.jsonl line $lineNumber"
        continue
    }

    $sessionId = Get-EntryString $entry @("id", "session_id", "sessionId", "thread_id", "threadId")
    if ([string]::IsNullOrWhiteSpace($sessionId)) {
        Write-Warning "Skipping session_index.jsonl line $lineNumber without a session id"
        continue
    }

    $updatedAt = Get-EntryUpdatedAt $entry
    $name = Get-EntryString $entry @("thread_name", "name", "title")
    $cwd = Get-EntryString $entry @("cwd", "repo", "repo_root", "workspace")
    $archived = Get-EntryBool $entry @("archived", "is_archived", "isArchived")
    $record = [pscustomobject][ordered]@{
        updated_at = if ($updatedAt -eq [datetime]::MinValue) { "" } else { $updatedAt.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffffffZ") }
        session_id = $sessionId
        thread_name = $name
        cwd = $cwd
        archived = $archived
        source_line = $lineNumber
    }

    if (-not $latestById.ContainsKey($sessionId)) {
        $latestById[$sessionId] = $record
        continue
    }

    $currentUpdatedAt = Get-EntryUpdatedAt $latestById[$sessionId]
    if ($updatedAt -ge $currentUpdatedAt) {
        $latestById[$sessionId] = $record
    }
}

$sessions = @($latestById.Values)
if (-not $IncludeArchived) {
    $sessions = @($sessions | Where-Object { -not $_.archived })
}
if (-not [string]::IsNullOrWhiteSpace($Filter)) {
    $sessions = @($sessions | Where-Object {
        $_.session_id -like "*$Filter*" -or
        $_.thread_name -like "*$Filter*" -or
        $_.cwd -like "*$Filter*"
    })
}

$ordered = @($sessions | Sort-Object @{ Expression = {
    if ([string]::IsNullOrWhiteSpace($_.updated_at)) {
        [datetime]::MinValue
    } else {
        [datetime]$_.updated_at
    }
}; Descending = $true }, session_id)

if ($Limit -gt 0) {
    $ordered = @($ordered | Select-Object -First $Limit)
}

if ($AsJson) {
    [pscustomobject][ordered]@{
        status = "ok"
        index_path = $indexPath
        include_archived = [bool]$IncludeArchived
        limit = $Limit
        count = @($ordered).Count
        sessions = $ordered
        private_path_exposed = $false
    } | ConvertTo-Json -Depth 8
    exit 0
}

$ordered | Format-Table -AutoSize updated_at, session_id, thread_name, archived
