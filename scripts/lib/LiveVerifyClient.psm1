#requires -Version 7.0
Set-StrictMode -Version Latest

<#
.SYNOPSIS
    Thin client for the ExoSnap Live Verify control channel.

.DESCRIPTION
    One NDJSON request per line over the local named pipe an ExoSnap process
    opened with `--live-verify-control <run-id>`.

    PowerShell rather than a compiled helper on purpose: the orchestrator is
    PowerShell (repository convention), .NET's NamedPipeClientStream needs no new
    dependency, and a client the developer can read line by line is worth more in
    a release-acceptance transcript than a faster one.

    Deliberately NOT in here: retries. A reconnect that happened silently would
    turn "the application restarted under us" -- the single most important thing
    an updater check has to notice -- into a green result. Reconnecting is the
    caller's explicit decision.
#>

# PipeStream does not support ReadTimeout, so every read goes through ReadAsync
# plus an explicit deadline. Without that a wedged application hangs the runner
# instead of failing the check it wedged.
class LiveVerifyConnection {
    [System.IO.Pipes.NamedPipeClientStream] $Pipe
    [System.Text.Decoder] $Decoder
    [System.Text.StringBuilder] $Partial
    [System.Collections.Generic.Queue[string]] $Lines
    [System.Collections.Generic.List[object]] $Events
    [System.Collections.Generic.List[object]] $Transcript
    [byte[]] $Buffer
    [System.Threading.Tasks.Task[int]] $Pending
    [int] $NextId
    [string] $PipeName
    [object] $Identity

    LiveVerifyConnection([string] $pipeName) {
        $this.PipeName = $pipeName
        $this.Decoder = [System.Text.UTF8Encoding]::new($false).GetDecoder()
        $this.Partial = [System.Text.StringBuilder]::new()
        $this.Lines = [System.Collections.Generic.Queue[string]]::new()
        $this.Events = [System.Collections.Generic.List[object]]::new()
        $this.Transcript = [System.Collections.Generic.List[object]]::new()
        $this.Buffer = [byte[]]::new(8192)
        $this.NextId = 1
        $this.Identity = $null
    }

    [void] Connect([int] $timeoutMs) {
        # The server-side name carries the \\.\pipe\ prefix; the .NET client
        # takes server + short name instead.
        $short = $this.PipeName -replace '^\\\\\.\\pipe\\', ''
        $this.Pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
            '.', $short,
            [System.IO.Pipes.PipeDirection]::InOut,
            [System.IO.Pipes.PipeOptions]::Asynchronous)
        $this.Pipe.Connect($timeoutMs)
    }

    [void] Close() {
        if ($null -ne $this.Pipe) {
            try { $this.Pipe.Dispose() } catch { }
            $this.Pipe = $null
        }
    }

    [bool] IsConnected() {
        return ($null -ne $this.Pipe) -and $this.Pipe.IsConnected
    }

    hidden [void] DrainPartial() {
        $text = $this.Partial.ToString()
        $index = $text.IndexOf("`n")
        while ($index -ge 0) {
            $line = $text.Substring(0, $index).TrimEnd("`r")
            if ($line.Trim().Length -gt 0) { $this.Lines.Enqueue($line) }
            $text = $text.Substring($index + 1)
            $index = $text.IndexOf("`n")
        }
        [void]$this.Partial.Clear()
        [void]$this.Partial.Append($text)
    }

    # Next protocol object, or $null on timeout / disconnect.
    [object] ReadObject([int] $timeoutMs) {
        $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
        while ($true) {
            if ($this.Lines.Count -gt 0) {
                $line = $this.Lines.Dequeue()
                $parsed = $null
                try { $parsed = $line | ConvertFrom-Json } catch { $parsed = $null }
                if ($null -ne $parsed) {
                    $this.Transcript.Add([pscustomobject]@{
                        direction = 'in'
                        timestamp = [DateTime]::UtcNow.ToString('o')
                        payload   = $parsed
                    })
                    return $parsed
                }
                continue
            }
            if ([DateTime]::UtcNow -ge $deadline) { return $null }
            if (-not $this.IsConnected()) { return $null }

            if ($null -eq $this.Pending) {
                $this.Pending = $this.Pipe.ReadAsync($this.Buffer, 0, $this.Buffer.Length)
            }
            $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            if (-not $this.Pending.Wait([Math]::Min($remaining, 250))) { continue }

            $count = $this.Pending.Result
            $this.Pending = $null
            if ($count -le 0) { return $null }
            $chars = [char[]]::new($this.Decoder.GetCharCount($this.Buffer, 0, $count))
            [void]$this.Decoder.GetChars($this.Buffer, 0, $count, $chars, 0)
            [void]$this.Partial.Append($chars)
            $this.DrainPartial()
        }
        return $null
    }

    [string] Send([string] $command, [hashtable] $parameters) {
        $id = "$($this.NextId)"
        $this.NextId++
        $request = [ordered]@{
            protocol = 1
            id       = $id
            command  = $command
            params   = if ($null -eq $parameters) { @{} } else { $parameters }
        }
        $json = ($request | ConvertTo-Json -Depth 10 -Compress) + "`n"
        $this.Transcript.Add([pscustomobject]@{
            direction = 'out'
            timestamp = [DateTime]::UtcNow.ToString('o')
            payload   = ($json.TrimEnd("`n") | ConvertFrom-Json)
        })
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
        $this.Pipe.Write($bytes, 0, $bytes.Length)
        $this.Pipe.Flush()
        return $id
    }

    # Sends and waits for the matching response. Events arriving in between are
    # kept, never discarded: a runner that waits for an event AFTER issuing the
    # command that causes it would otherwise race its own request.
    [object] Request([string] $command, [hashtable] $parameters, [int] $timeoutMs) {
        $id = $this.Send($command, $parameters)
        $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
        while ([DateTime]::UtcNow -lt $deadline) {
            $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            $object = $this.ReadObject($remaining)
            if ($null -eq $object) { break }
            if ($object.PSObject.Properties.Name -contains 'event') {
                $this.Events.Add($object)
                continue
            }
            if ($object.id -eq $id) { return $object }
        }
        return $null
    }

    [object] Hello([string] $runId, [int] $timeoutMs) {
        $response = $this.Request('system.hello', @{ runId = $runId }, $timeoutMs)
        if ($null -ne $response -and $response.ok) { $this.Identity = $response.result }
        return $response
    }

    # Waits for a named event whose data satisfies every key/value in $where.
    # Already-buffered events are consulted first.
    [object] WaitForEvent([string] $name, [hashtable] $where, [int] $timeoutMs) {
        $matchesWhere = {
            param($candidate)
            if ($candidate.event -ne $name) { return $false }
            if ($null -eq $where) { return $true }
            foreach ($key in $where.Keys) {
                $names = $candidate.data.PSObject.Properties.Name
                if ($names -notcontains $key) { return $false }
                if ("$($candidate.data.$key)" -ne "$($where[$key])") { return $false }
            }
            return $true
        }
        for ($i = 0; $i -lt $this.Events.Count; $i++) {
            if (& $matchesWhere $this.Events[$i]) {
                $found = $this.Events[$i]
                $this.Events.RemoveAt($i)
                return $found
            }
        }
        $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
        while ([DateTime]::UtcNow -lt $deadline) {
            $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            $object = $this.ReadObject($remaining)
            if ($null -eq $object) { break }
            if ($object.PSObject.Properties.Name -notcontains 'event') { continue }
            if (& $matchesWhere $object) { return $object }
            $this.Events.Add($object)
        }
        return $null
    }
}

function New-LiveVerifyPipeName {
    <#
    .SYNOPSIS
        The endpoint name for a run id. Mirrors LiveVerifyOptions::PipeNameForRunId.
    #>
    param([Parameter(Mandatory)] [string] $RunId)
    return "\\.\pipe\ExoSnap.LiveVerify.$RunId"
}

function New-LiveVerifyRunId {
    <#
    .SYNOPSIS
        An unguessable run id. Doubles as the connection credential, so it is a
        GUID rather than a timestamp.
    #>
    return "lv-" + ([guid]::NewGuid().ToString('N'))
}

function Connect-LiveVerify {
    <#
    .SYNOPSIS
        Connects and performs the mandatory handshake.
    .DESCRIPTION
        Throws on a refused handshake. The caller gets a connected, authenticated
        object or an error -- never a half-open one.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RunId,
        [int] $ConnectTimeoutMs = 15000,
        [int] $RequestTimeoutMs = 20000
    )

    $connection = [LiveVerifyConnection]::new((New-LiveVerifyPipeName -RunId $RunId))
    try {
        $connection.Connect($ConnectTimeoutMs)
    }
    catch {
        throw "Could not connect to the Live Verify endpoint for run '$RunId': $($_.Exception.Message)"
    }

    $hello = $connection.Hello($RunId, $RequestTimeoutMs)
    if ($null -eq $hello) {
        $connection.Close()
        throw "Handshake timed out after ${RequestTimeoutMs} ms"
    }
    if (-not $hello.ok) {
        $connection.Close()
        throw "Handshake refused: $($hello.error.code) - $($hello.error.message)"
    }
    if ($hello.result.protocol -ne 1) {
        $connection.Close()
        throw "Protocol mismatch: server speaks $($hello.result.protocol), client speaks 1"
    }
    return $connection
}

function Invoke-LiveVerifyCommand {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Connection,
        [Parameter(Mandatory)] [string] $Command,
        [hashtable] $Parameters,
        [int] $TimeoutMs = 20000
    )
    $response = $Connection.Request($Command, $Parameters, $TimeoutMs)
    if ($null -eq $response) {
        throw "No response to '$Command' within ${TimeoutMs} ms"
    }
    return $response
}

function Wait-LiveVerifyEvent {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Connection,
        [Parameter(Mandatory)] [string] $EventName,
        [hashtable] $Where,
        [int] $TimeoutMs = 30000
    )
    return $Connection.WaitForEvent($EventName, $Where, $TimeoutMs)
}

function Wait-LiveVerifyState {
    <#
    .SYNOPSIS
        Polls a snapshot until a field reaches a value, or the deadline passes.
    .DESCRIPTION
        The event-driven Wait-LiveVerifyEvent is preferred. This exists for the
        states that have no event of their own, and for re-establishing ground
        truth after a reconnect -- never as a substitute for a sleep. There is no
        "sleep N seconds and assume it worked" anywhere in this client.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Connection,
        [Parameter(Mandatory)] [string] $Command,
        [Parameter(Mandatory)] [string] $Field,
        [Parameter(Mandatory)] $Value,
        [int] $TimeoutMs = 30000,
        [int] $PollMs = 200
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $last = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $response = Invoke-LiveVerifyCommand -Connection $Connection -Command $Command -TimeoutMs 10000
        if ($response.ok) {
            $last = $response.result
            if ($last.PSObject.Properties.Name -contains $Field -and "$($last.$Field)" -eq "$Value") {
                return $last
            }
        }
        Start-Sleep -Milliseconds $PollMs
    }
    return $null
}

function Get-LiveVerifyTranscript {
    <#
    .SYNOPSIS
        Every request and response this connection exchanged, in order.
    .DESCRIPTION
        The evidence a PASS is written against. "PASS - looked fine" is not an
        automated result; this is what replaces it.
    #>
    param([Parameter(Mandatory)] $Connection)
    return $Connection.Transcript
}

Export-ModuleMember -Function New-LiveVerifyPipeName, New-LiveVerifyRunId, Connect-LiveVerify,
    Invoke-LiveVerifyCommand, Wait-LiveVerifyEvent, Wait-LiveVerifyState, Get-LiveVerifyTranscript
