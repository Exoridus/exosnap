#requires -Version 7.0
<#
.SYNOPSIS
    Tests that Invoke-LiveVerifyCommand hands out a response of a fixed shape.

.DESCRIPTION
    The control protocol omits `result` on a refusal and `error` on a success,
    which is correct on the wire and a trap for every caller. These scripts run
    under Set-StrictMode, so reading `.result` off a refusal does not evaluate to
    null -- it throws "the property 'result' cannot be found on this object", and
    the scenario then reports a PowerShell message where a verdict belongs. That
    happened in the rc16 campaign, in a gate that had never before reached the code
    in question.

    There are ~86 places that read `.result`, so guarding them one at a time is a
    defect waiting at each of them. The shape is normalised once, in the client,
    and this file is what keeps it normalised.

    Fixtures only: a fake connection object, no pipe and no application.

    Deliberately not Pester, matching the other script tests here.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptsRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptsRoot 'lib/LiveVerifyClient.psm1') -Force -DisableNameChecking

$script:Failures = 0
$script:Ran = 0

function Test-Case([string] $Name, [scriptblock] $Body) {
    $script:Ran++
    try {
        & $Body
        Write-Host "  PASS  $Name"
    }
    catch {
        $script:Failures++
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Assert-Equal($Expected, $Actual, [string] $What = 'value') {
    if ("$Expected" -ne "$Actual") { throw "$What`: expected '$Expected', got '$Actual'" }
}

function Assert-True($Condition, [string] $What = 'condition') {
    if (-not $Condition) { throw "$What was false" }
}

# A connection whose Request() returns whatever the test puts in $script:Reply.
function New-FakeConnection {
    $connection = [pscustomobject]@{}
    Add-Member -InputObject $connection -MemberType ScriptMethod -Name Request -Value {
        param($command, $parameters, $timeoutMs)
        return $script:Reply
    }
    return $connection
}

Write-Host 'Live Verify response shape'

Test-Case 'a refusal still carries a result, so .result cannot throw' {
    # Exactly the shape the protocol sends for a refused command: no `result`.
    $script:Reply = [pscustomobject]@{
        id = 1; ok = $false
        error = [pscustomobject]@{ message = 'A recordingError surface is open'; code = 'blocked' }
    }
    $response = Invoke-LiveVerifyCommand -Connection (New-FakeConnection) -Command 'record.start'
    Assert-Equal $false $response.ok 'ok'
    Assert-True ($null -ne $response.result) 'result exists on a refusal'
    Assert-Equal 'A recordingError surface is open' $response.error.message 'error survives'
}

Test-Case 'a success still carries an error object, so .error.message cannot throw' {
    # And the shape for a successful command: no `error`.
    $script:Reply = [pscustomobject]@{ id = 2; ok = $true; result = [pscustomobject]@{ recording = $true } }
    $response = Invoke-LiveVerifyCommand -Connection (New-FakeConnection) -Command 'record.snapshot'
    Assert-Equal $true $response.ok 'ok'
    Assert-Equal $true $response.result.recording 'result survives'
    Assert-Equal '' $response.error.message 'error.message is empty rather than absent'
}

Test-Case 'a response without ok is treated as success' {
    # Some replies carry only a result. Absent must not read as failure.
    $script:Reply = [pscustomobject]@{ id = 3; result = [pscustomobject]@{ value = 7 } }
    $response = Invoke-LiveVerifyCommand -Connection (New-FakeConnection) -Command 'settings.get'
    Assert-Equal $true $response.ok 'ok defaults to true'
    Assert-Equal 7 $response.result.value 'result survives'
}

Test-Case 'an existing result is never replaced' {
    $script:Reply = [pscustomobject]@{ id = 4; ok = $true; result = [pscustomobject]@{ sequence = 42 } }
    $response = Invoke-LiveVerifyCommand -Connection (New-FakeConnection) -Command 'notification.raise'
    Assert-Equal 42 $response.result.sequence 'the caller keeps its own result'
}

Test-Case 'no response at all is still an error, not an empty result' {
    # The one case that must keep throwing: silence is not an answer, and turning
    # it into an empty result would let a timeout read as a successful command.
    $script:Reply = $null
    $threw = $false
    try { Invoke-LiveVerifyCommand -Connection (New-FakeConnection) -Command 'record.stop' }
    catch { $threw = $true }
    Assert-True $threw 'a missing response throws'
}

Write-Host ''
Write-Host "$($script:Ran - $script:Failures)/$($script:Ran) passed"
if ($script:Failures -gt 0) { exit 1 }
exit 0
