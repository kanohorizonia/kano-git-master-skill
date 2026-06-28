param(
    [Parameter(Mandatory = $true)]
    [string]$ThreadId,

    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Url = "ws://127.0.0.1:4510"
)

$ErrorActionPreference = "Stop"
[Console]::InputEncoding  = [System.Text.UTF8Encoding]::new()
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

$ct = [Threading.CancellationToken]::None
$ws = [System.Net.WebSockets.ClientWebSocket]::new()

function Send-Json {
    param([object]$Object)
    $json = $Object | ConvertTo-Json -Depth 50 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    $ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).GetAwaiter().GetResult() | Out-Null
}

function Receive-Json {
    $buffer = New-Object byte[] 65536
    $ms = [System.IO.MemoryStream]::new()
    do {
        $segment = [ArraySegment[byte]]::new($buffer)
        $result = $ws.ReceiveAsync($segment, $ct).GetAwaiter().GetResult()
        if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { return $null }
        $ms.Write($buffer, 0, $result.Count)
    } while (-not $result.EndOfMessage)
    $text = [Text.Encoding]::UTF8.GetString($ms.ToArray())
    if (-not $text.Trim()) { return $null }
    return $text | ConvertFrom-Json
}

function Wait-Response {
    param([string]$Id)
    while ($true) {
        $msg = Receive-Json
        if ($null -eq $msg) { throw "app-server connection closed while waiting for response id=$Id" }
        if (($msg.PSObject.Properties.Name -contains "id") -and "$($msg.id)" -eq "$Id") {
            if ($msg.PSObject.Properties.Name -contains "error" -and $null -ne $msg.error) {
                throw "app-server error for id=${Id}: $($msg.error | ConvertTo-Json -Depth 20 -Compress)"
            }
            return $msg.result
        }
        if ($msg.method -eq "thread/name/updated") {
            Write-Host "[event] thread/name/updated: $($msg.params.name)" -ForegroundColor DarkGreen
        }
    }
}

try {
    $ws.ConnectAsync([Uri]$Url, $ct).GetAwaiter().GetResult() | Out-Null

    Send-Json @{
        id = "1"
        method = "initialize"
        params = @{
            clientInfo = @{
                name = "kano_thread_name_client"
                title = "Kano Thread Name Client"
                version = "0.1.0"
            }
            capabilities = @{ experimentalApi = $true }
        }
    }
    Wait-Response "1" | Out-Null
    Send-Json @{ method = "initialized"; params = @{} }

    Send-Json @{
        id = "2"
        method = "thread/resume"
        params = @{ threadId = $ThreadId }
    }
    $resume = Wait-Response "2"
    Write-Host "[thread] loaded: $($resume.thread.id)" -ForegroundColor Cyan

    Send-Json @{
        id = "3"
        method = "thread/name/set"
        params = @{
            threadId = $ThreadId
            name = $Name
        }
    }
    Wait-Response "3" | Out-Null
    Write-Host "[name] set: $Name" -ForegroundColor Cyan

    Send-Json @{
        id = "4"
        method = "thread/read"
        params = @{
            threadId = $ThreadId
            includeTurns = $false
        }
    }
    $read = Wait-Response "4"

    [pscustomobject]@{
        ThreadId = $read.thread.id
        Name     = $read.thread.name
        Cwd      = $read.thread.cwd
        Updated  = $read.thread.updatedAt
    } | Format-List
} finally {
    try {
        if ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
            $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", $ct).GetAwaiter().GetResult() | Out-Null
        }
    } catch {
        try { $ws.Abort() } catch { }
        Write-Host "[ws] remote closed before close handshake; cleanup ignored" -ForegroundColor DarkGray
    }
    $ws.Dispose()
}
