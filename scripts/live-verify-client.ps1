#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('hello', 'query', 'command', 'wait', 'capabilities', 'describe', 'state')]
    [string] $Verb,

    [Parameter(Position = 1)]
    [string] $Name,

    [Parameter(Mandatory)]
    [string] $RunId,

    # JSON object for `command` parameters, e.g. '{"screen":"\\\\.\\DISPLAY2"}'.
    [string] $Params,

    # key=value pairs an awaited event's data must match.
    [string[]] $Where,

    [int] $TimeoutSeconds = 30,

    # The envelope version to speak. 2 is the default; 1 is selectable so the
    # backward-compatible surface can be exercised from the command line.
    [ValidateRange(1, 2)]
    [int] $Protocol = 2
)

<#
.SYNOPSIS
    Command-line front end for the Live Verify control channel.

.DESCRIPTION
    Machine-readable by default: the JSON result goes to stdout and nothing else
    does, so a caller can pipe it straight into ConvertFrom-Json. Diagnostics go
    to stderr.

    Exit codes -- distinct on purpose, because "the application refused the
    command" and "the application never answered" are different acceptance
    outcomes and a runner must not collapse them:

        0  success
        2  usage error
        3  could not connect / handshake refused / protocol mismatch
        4  the command was answered with ok:false
        5  timed out waiting for a response or an event

.EXAMPLE
    ./live-verify-client.ps1 hello -RunId lv-abc...
    ./live-verify-client.ps1 query record -RunId lv-abc...
    ./live-verify-client.ps1 command record.pause -RunId lv-abc...
    ./live-verify-client.ps1 wait record.stateChanged -Where stateText=Paused -RunId lv-abc... -TimeoutSeconds 10
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/LiveVerifyClient.psm1') -Force

# `query <domain>` is sugar for the domain's snapshot command; anything else is
# passed through verbatim so the CLI can never drift from the server allowlist.
$queryCommands = @{
    system      = 'system.snapshot'
    app         = 'app.snapshot'
    window      = 'window.snapshot'
    preview     = 'preview.snapshot'
    record      = 'record.snapshot'
    result      = 'record.result'
    # Protocol 2. Named here as well so `query ui` reads like the others; the
    # dedicated `state` verb below is the one a check should use.
    ui          = 'ui.getState'
    overlay     = 'overlay.snapshot'
    editor      = 'editor.snapshot'
    diagnostics = 'diagnostics.snapshot'
}

function Write-Result([object] $value) {
    $value | ConvertTo-Json -Depth 20
}

$connection = $null
try {
    try {
        $connection = Connect-LiveVerify -RunId $RunId -ConnectTimeoutMs ($TimeoutSeconds * 1000) `
            -Protocol $Protocol
    }
    catch {
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 3
    }

    switch ($Verb) {
        'hello' {
            Write-Result $connection.Identity
            exit 0
        }
        'capabilities' {
            $response = Invoke-LiveVerifyCommand -Connection $connection -Command 'system.capabilities' `
                -TimeoutMs ($TimeoutSeconds * 1000)
            if (-not $response.ok) { Write-Result $response.error; exit 4 }
            Write-Result $response.result
            exit 0
        }
        'describe' {
            $response = Invoke-LiveVerifyCommand -Connection $connection -Command 'ipc.describe' `
                -TimeoutMs ($TimeoutSeconds * 1000)
            if (-not $response.ok) { Write-Result $response.error; exit 4 }
            Write-Result $response.result
            exit 0
        }
        'state' {
            $response = Invoke-LiveVerifyCommand -Connection $connection -Command 'ui.getState' `
                -TimeoutMs ($TimeoutSeconds * 1000)
            if (-not $response.ok) { Write-Result $response.error; exit 4 }
            Write-Result $response.result
            exit 0
        }
        'query' {
            if ([string]::IsNullOrWhiteSpace($Name)) {
                [Console]::Error.WriteLine("query needs a domain: $($queryCommands.Keys -join ', ')")
                exit 2
            }
            $command = if ($queryCommands.ContainsKey($Name)) { $queryCommands[$Name] } else { $Name }
            $response = Invoke-LiveVerifyCommand -Connection $connection -Command $command `
                -TimeoutMs ($TimeoutSeconds * 1000)
            if (-not $response.ok) { Write-Result $response.error; exit 4 }
            Write-Result $response.result
            exit 0
        }
        'command' {
            if ([string]::IsNullOrWhiteSpace($Name)) {
                [Console]::Error.WriteLine('command needs a command name')
                exit 2
            }
            $parameters = @{}
            if (-not [string]::IsNullOrWhiteSpace($Params)) {
                $parsed = $Params | ConvertFrom-Json
                foreach ($property in $parsed.PSObject.Properties) {
                    $parameters[$property.Name] = $property.Value
                }
            }
            $response = Invoke-LiveVerifyCommand -Connection $connection -Command $Name -Parameters $parameters `
                -TimeoutMs ($TimeoutSeconds * 1000)
            if (-not $response.ok) { Write-Result $response.error; exit 4 }
            Write-Result $response.result
            exit 0
        }
        'wait' {
            if ([string]::IsNullOrWhiteSpace($Name)) {
                [Console]::Error.WriteLine('wait needs an event name')
                exit 2
            }
            $predicate = @{}
            foreach ($pair in ($Where ?? @())) {
                $split = $pair.Split('=', 2)
                if ($split.Count -ne 2) {
                    [Console]::Error.WriteLine("-Where entries must be key=value, got '$pair'")
                    exit 2
                }
                $predicate[$split[0]] = $split[1]
            }
            $observed = Wait-LiveVerifyEvent -Connection $connection -EventName $Name -Where $predicate `
                -TimeoutMs ($TimeoutSeconds * 1000)
            if ($null -eq $observed) {
                [Console]::Error.WriteLine("Timed out after ${TimeoutSeconds}s waiting for '$Name'")
                exit 5
            }
            Write-Result $observed
            exit 0
        }
    }
}
finally {
    if ($null -ne $connection) { $connection.Close() }
}
