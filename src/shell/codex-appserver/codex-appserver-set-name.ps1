[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ThreadId,

    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Url = "ws://127.0.0.1:4510",

    [int]$ResponseTimeoutSeconds = 10
)

$ErrorActionPreference = "Stop"

[Console]::InputEncoding  = [System.Text.UTF8Encoding]::new()
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()

$ct = [Threading.CancellationToken]::None
$ws = [System.Net.WebSockets.ClientWebSocket]::new()

function Wait-TaskResult {
    param(
        [System.Threading.Tasks.Task]$Task,
        [int]$TimeoutSeconds,
        [string]$Operation
    )

    if ($TimeoutSeconds -gt 0) {
        if (-not $Task.Wait([TimeSpan]::FromSeconds($TimeoutSeconds))) {
            throw "$Operation timed out after $TimeoutSeconds seconds"
        }
    } else {
        $Task.Wait()
    }

    return $Task.GetAwaiter().GetResult()
}

function Send-Json {
    param([object]$Object)

    $json = $Object | ConvertTo-Json -Depth 50 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    Wait-TaskResult $ws.SendAsync(
        $segment,
        [System.Net.WebSockets.WebSocketMessageType]::Text,
        $true,
        $ct
    ) $ResponseTimeoutSeconds "app-server send" | Out-Null
}

function Receive-Json {
    param([string]$Operation)

    $buffer = New-Object byte[] 65536
    $ms = [System.IO.MemoryStream]::new()
    do {
        $segment = [ArraySegment[byte]]::new($buffer)
        $result = Wait-TaskResult $ws.ReceiveAsync($segment, $ct) $ResponseTimeoutSeconds $Operation
        if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
            return $null
        }
        $ms.Write($buffer, 0, $result.Count)
    } while (-not $result.EndOfMessage)

    $text = [Text.Encoding]::UTF8.GetString($ms.ToArray())
    if (-not $text.Trim()) { return $null }
    return $text | ConvertFrom-Json
}

function Wait-Response {
    param([string]$Id)

    while ($true) {
        $msg = Receive-Json "app-server response id=$Id"
        if ($null -eq $msg) {
            throw "app-server connection closed while waiting for response id=$Id"
        }
        if (($msg.PSObject.Properties.Name -contains "id") -and "$($msg.id)" -eq "$Id") {
            if ($msg.PSObject.Properties.Name -contains "error" -and $null -ne $msg.error) {
                throw "app-server error for id=${Id}: $($msg.error | ConvertTo-Json -Depth 20 -Compress)"
            }
            return $msg.result
        }
        if ($msg.method -eq "thread/name/updated") {
            $updatedName = $msg.params.name
            if (-not $updatedName -and $msg.params.thread -and $msg.params.thread.name) {
                $updatedName = $msg.params.thread.name
            }
            Write-Host "[event] thread/name/updated: $updatedName" -ForegroundColor DarkGreen
        }
    }
}

try {
    Wait-TaskResult $ws.ConnectAsync([Uri]$Url, $ct) $ResponseTimeoutSeconds "app-server connect" | Out-Null

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
        method = "thread/name/set"
        params = @{
            threadId = $ThreadId
            name = $Name
        }
    }
    Wait-Response "2" | Out-Null
    Write-Host "[name] set: $Name" -ForegroundColor Cyan
} finally {
    try {
        if ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open -or
            $ws.State -eq [System.Net.WebSockets.WebSocketState]::CloseReceived) {
            $ws.Abort()
        }
    } catch {
        Write-Host "[ws] cleanup ignored: $($_.Exception.Message)" -ForegroundColor DarkGray
    }
    $ws.Dispose()
}
