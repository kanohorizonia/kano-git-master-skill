param(
    [string]$Name = "",
    [string]$ThreadId = "",
    [string]$CodexHome = "",
    [switch]$Regex,
    [switch]$AllMatches,
    [switch]$IncludeContext,
    [switch]$IncludeReasoning,
    [switch]$IncludeTool,
    [switch]$Raw,
    [switch]$NoColor,
    [int]$MaxMessages = 0
)

$ErrorActionPreference = "Stop"

if (-not $CodexHome) {
    $CodexHome = if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $env:USERPROFILE ".codex" }
}

$SessionIndexPath = Join-Path $CodexHome "session_index.jsonl"
$SessionRoot = Join-Path $CodexHome "sessions"

function Write-Info {
    param([string]$Text, [string]$Color = "Gray")
    if ($NoColor) { Write-Host $Text } else { Write-Host $Text -ForegroundColor $Color }
}

function Get-PropValue {
    param([object]$Object, [string[]]$Names)
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        if ($Object.PSObject.Properties.Name -contains $name) {
            return $Object.$name
        }
    }
    return $null
}

function Get-StringValue {
    param([object]$Object, [string[]]$Names)
    $v = Get-PropValue $Object $Names
    if ($null -eq $v) { return "" }
    return "$v"
}

function Normalize-Text {
    param([string]$Text)

    if (-not $Text) { return "" }
    $s = ($Text -replace "`r`n", "`n").Trim()
    if (-not $s) { return "" }

    if (-not $IncludeContext) {
        $flat = ($s -replace '\s+', ' ').Trim()
        if ($flat -match '^(#\s*)?AGENTS\.md instructions\b') { return "" }
        if ($flat -match '^<environment_context>') { return "" }
        if ($flat -match '^<INSTRUCTIONS>') { return "" }
        if ($flat -match '^You are Codex') { return "" }
        if ($flat -match '<cwd>') { return "" }
        if ($flat -match '<approval_policy>') { return "" }
        if ($flat -match '<sandbox_mode>') { return "" }
        if ($flat -match '<network_access>') { return "" }
    }

    return $s
}

function Extract-TextFromObject {
    param([object]$Object)
    if ($null -eq $Object) { return "" }

    foreach ($key in @("text", "message", "prompt", "input", "output_text", "input_text")) {
        if ($Object.PSObject.Properties.Name -contains $key) {
            $value = $Object.$key
            if ($value -is [string]) {
                $t = Normalize-Text $value
                if ($t) { return $t }
            }
        }
    }

    if ($Object.PSObject.Properties.Name -contains "content") {
        $content = $Object.content
        if ($content -is [string]) {
            $t = Normalize-Text $content
            if ($t) { return $t }
        }

        $parts = @()
        foreach ($part in @($content)) {
            if ($part -is [string]) {
                $parts += $part
                continue
            }
            foreach ($key in @("text", "content", "input_text", "output_text")) {
                if ($null -ne $part -and $part.PSObject.Properties.Name -contains $key) {
                    $v = $part.$key
                    if ($v -is [string] -and $v.Trim()) { $parts += $v }
                }
            }
        }

        if ($parts.Count -gt 0) {
            $t = Normalize-Text ($parts -join "`n")
            if ($t) { return $t }
        }
    }

    return ""
}

function Get-RoleFromObject {
    param([object]$Object, [object]$Event)
    if ($null -eq $Object) { return "" }

    $type = Get-StringValue $Object @("type")
    $role = Get-StringValue $Object @("role")
    $eventType = Get-StringValue $Event @("type", "method")

    switch -Regex ($type) {
        '^userMessage$' { return "user" }
        '^agentMessage$' { return "assistant" }
        '^reasoning$' { return "reasoning" }
        'tool|function|mcp|command' { return "tool" }
    }

    switch -Regex ($role) {
        '^user$' { return "user" }
        '^assistant$' { return "assistant" }
        '^system$' { return "system" }
        '^tool$' { return "tool" }
    }

    switch -Regex ($eventType) {
        'user|prompt|input' { return "user" }
        'assistant|agentMessage' { return "assistant" }
        'reasoning' { return "reasoning" }
        'tool|function|mcp|command' { return "tool" }
    }

    return ""
}

function Get-CandidateObjects {
    param([object]$Event)

    # Keep this as a plain PowerShell array. Do not return a single nested
    # object[]; the caller intentionally enumerates candidates.
    $candidates = @()
    if ($null -ne $Event) { $candidates += $Event }

    $payload = Get-PropValue $Event @("payload", "params")
    if ($null -ne $payload) { $candidates += $payload }

    foreach ($base in @($Event, $payload)) {
        if ($null -eq $base) { continue }
        foreach ($key in @("item", "message", "msg", "event", "turn", "response", "input")) {
            if ($base.PSObject.Properties.Name -contains $key) {
                $v = $base.$key
                if ($null -ne $v) {
                    foreach ($one in @($v)) {
                        if ($null -ne $one) { $candidates += $one }
                    }
                }
            }
        }
    }

    return $candidates
}

function Read-JsonLines {
    param([string]$Path)
    Get-Content -Path $Path -Encoding UTF8 -ErrorAction Stop |
        Where-Object { $_.Trim() } |
        ForEach-Object {
            try { $_ | ConvertFrom-Json -ErrorAction Stop } catch { $null }
        } |
        Where-Object { $null -ne $_ }
}

function Is-ArchivedIndexItem {
    param([object]$Item)
    $archived = Get-PropValue $Item @("archived", "is_archived", "isArchived")
    $archivedAt = Get-StringValue $Item @("archived_at", "archivedAt")
    if ($archived -eq $true) { return $true }
    if ($archivedAt) { return $true }
    return $false
}

function Resolve-ThreadIndexItems {
    if (-not (Test-Path $SessionIndexPath)) {
        Write-Info "Codex session index not found: $SessionIndexPath" "Red"
        exit 2
    }

    $items = @(Read-JsonLines $SessionIndexPath | Where-Object { -not (Is-ArchivedIndexItem $_) })

    if ($ThreadId) {
        $matches = @($items | Where-Object { (Get-StringValue $_ @("id", "threadId", "sessionId")) -eq $ThreadId })
        if ($matches.Count -eq 0) {
            $matches = @([pscustomobject]@{ id = $ThreadId; thread_name = ""; updated_at = ""; path = ""; cwd = "" })
        }
        return $matches
    }

    if (-not $Name) {
        Write-Info "Provide -Name or -ThreadId." "Red"
        Write-Info "Examples:" "Yellow"
        Write-Info "  .\codex-appserver-dump-thread.ps1 -Name kog" "Yellow"
        Write-Info "  .\codex-appserver-dump-thread.ps1 -ThreadId 019e..." "Yellow"
        exit 2
    }

    if ($Regex) {
        $matches = @($items | Where-Object { (Get-StringValue $_ @("thread_name", "threadName", "name")) -match $Name })
    } else {
        $matches = @($items | Where-Object { (Get-StringValue $_ @("thread_name", "threadName", "name")) -eq $Name })
    }

    if ($matches.Count -eq 0) {
        Write-Info "No non-archived Codex thread found for name '$Name' in: $SessionIndexPath" "Red"
        Write-Info "Next checks:" "Yellow"
        Write-Info "  1. Run: codex resume --all" "Yellow"
        Write-Info "  2. Verify spelling/case, or try: .\codex-appserver-dump-thread.ps1 -Name '<regex>' -Regex" "Yellow"
        Write-Info "  3. If this was created by the app-server runner, also check: $env:USERPROFILE\.kano\codex-appserver-thread-index.jsonl" "Yellow"
        Write-Info "  4. If you know the id, use: .\codex-appserver-dump-thread.ps1 -ThreadId <id>" "Yellow"
        exit 3
    }

    $sorted = @($matches | Sort-Object {
        $u = Get-StringValue $_ @("updated_at", "updatedAt")
        try { [datetime]$u } catch { [datetime]::MinValue }
    } -Descending)

    if (-not $AllMatches -and $sorted.Count -gt 1) {
        Write-Info "Found $($sorted.Count) non-archived threads named '$Name'; dumping the latest one. Use -AllMatches to dump all." "DarkYellow"
    }

    if ($AllMatches) { return $sorted }
    return @($sorted | Select-Object -First 1)
}

function Resolve-TranscriptPath {
    param([object]$IndexItem)

    $path = Get-StringValue $IndexItem @("path", "rollout_path", "rolloutPath")
    if ($path -and (Test-Path $path)) { return $path }

    $id = Get-StringValue $IndexItem @("id", "threadId", "sessionId")
    if (-not $id) { return "" }

    if (-not (Test-Path $SessionRoot)) { return "" }

    $files = @(Get-ChildItem $SessionRoot -Recurse -File -Filter "*$id*.jsonl" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    if ($files.Count -gt 0) { return $files[0].FullName }

    return ""
}

function Extract-Messages {
    param([string]$Path)

    $events = @(Read-JsonLines $Path)
    $messages = @()
    $seen = @{}
    $index = 0

    foreach ($e in $events) {
        $index += 1
        $role = ""
        $text = ""

        foreach ($obj in (Get-CandidateObjects $e)) {
            $candidateRole = Get-RoleFromObject $obj $e
            if (-not $candidateRole) { continue }
            if ($candidateRole -eq "reasoning" -and -not $IncludeReasoning) { continue }
            if ($candidateRole -eq "tool" -and -not $IncludeTool) { continue }
            if ($candidateRole -eq "system" -and -not $IncludeContext) { continue }

            $candidateText = Extract-TextFromObject $obj
            if (-not $candidateText) { continue }

            $role = $candidateRole
            $text = $candidateText
            break
        }

        if (-not $role -or -not $text) { continue }

        $hashKey = "$role|$text"
        if ($seen.ContainsKey($hashKey)) { continue }
        $seen[$hashKey] = $true

        $ts = Get-StringValue $e @("timestamp", "created_at", "createdAt", "time")
        $messages += [pscustomobject]@{
            Index = $messages.Count + 1
            Role = $role
            Time = $ts
            Text = $text
        }
    }

    return $messages
}

function Print-Thread {
    param([object]$IndexItem)

    $id = Get-StringValue $IndexItem @("id", "threadId", "sessionId")
    $name = Get-StringValue $IndexItem @("thread_name", "threadName", "name")
    $updated = Get-StringValue $IndexItem @("updated_at", "updatedAt")
    $cwd = Get-StringValue $IndexItem @("cwd")
    $path = Resolve-TranscriptPath $IndexItem

    Write-Host ""
    Write-Info "=== Codex Thread ===" "Cyan"
    Write-Info "Name     : $name" "Cyan"
    Write-Info "ThreadId : $id" "Cyan"
    if ($updated) { Write-Info "Updated  : $updated" "Cyan" }
    if ($cwd) { Write-Info "Cwd      : $cwd" "Cyan" }
    Write-Info "File     : $path" "Cyan"

    if (-not $path -or -not (Test-Path $path)) {
        Write-Info "Transcript file not found for thread id '$id'." "Red"
        return
    }

    if ($Raw) {
        Get-Content $path -Encoding UTF8
        return
    }

    $messages = @(Extract-Messages $path)
    if ($MaxMessages -gt 0 -and $messages.Count -gt $MaxMessages) {
        $messages = @($messages | Select-Object -Last $MaxMessages)
    }

    if ($messages.Count -eq 0) {
        Write-Info "No user/assistant messages extracted." "Yellow"
        Write-Info "Try -IncludeContext, -IncludeTool, or -Raw if you need to inspect the transcript." "Yellow"
        return
    }

    foreach ($m in $messages) {
        # Some Windows PowerShell object paths can surface Role as an array-like
        # value. Normalize it before using it as ConsoleColor input; otherwise
        # Write-Host may receive values such as "Yellow Green Yellow Green".
        $roleParts = @($m.Role) |
            ForEach-Object { "$($_)".Trim() } |
            Where-Object { $_ -and $_ -ne "System.Object[]" }

        if ($roleParts.Count -gt 0) {
            $roleValue = (($roleParts[0] -split '\s+')[0]).ToLowerInvariant()
        } else {
            $roleValue = "unknown"
        }

        # Codex transcript/index paths may abbreviate roles as U/A.
        # Normalize them before choosing display label and color.
        if ($roleValue -eq "u") {
            $roleValue = "user"
        } elseif ($roleValue -eq "a" -or $roleValue -eq "agent") {
            $roleValue = "assistant"
        }

        $roleLabel = $roleValue.ToUpperInvariant()
        $messageIndex = @($m.Index | Select-Object -First 1)[0]
        if (-not $messageIndex) { $messageIndex = 0 }
        $messageTime = @($m.Time | Select-Object -First 1)[0]

        if ($roleValue -eq "user") {
            $color = "Yellow"
        } elseif ($roleValue -eq "assistant") {
            $color = "Green"
        } elseif ($roleValue -eq "reasoning") {
            $color = "DarkGray"
        } elseif ($roleValue -eq "tool") {
            $color = "Magenta"
        } else {
            $color = "Gray"
        }

        Write-Host ""
        if ($messageTime) {
            Write-Info ("--- [{0:000}] {1} {2} ---" -f $messageIndex, $roleLabel, $messageTime) $color
        } else {
            Write-Info ("--- [{0:000}] {1} ---" -f $messageIndex, $roleLabel) $color
        }
        Write-Info "$($m.Text)" $color
    }
}

$items = @(Resolve-ThreadIndexItems)
foreach ($item in $items) {
    Print-Thread $item
}
