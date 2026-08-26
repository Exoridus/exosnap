#requires -Version 7.0
<#
.SYNOPSIS
    Tests for the client's endpoint naming -- the one thing that has to be right
    before a runner can follow an update handoff across a process boundary.

.DESCRIPTION
    Fixtures only: no application, no updater, no pipe. What is under test is
    that the same run id addresses two DIFFERENT endpoints, and that the names
    match what the two processes create (control::PipeName). A client that
    derived the child's name wrongly would fail by hanging on a connect, which
    is the most expensive way to learn about a typo.

    Deliberately not Pester, matching live-verify-state.tests.ps1.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptsRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptsRoot 'lib/LiveVerifyClient.psm1') -Force

$script:Failures = 0
$script:Ran = 0

function Test-Case([string] $Name, [scriptblock] $Body) {
    $script:Ran++
    try {
        & $Body
        Write-Host "PASS $Name"
    }
    catch {
        $script:Failures++
        Write-Host "FAIL $Name"
        Write-Host "     $($_.Exception.Message)"
    }
}

function Assert-Equal($Expected, $Actual, [string] $What) {
    if ($Expected -ne $Actual) { throw "$What`: expected '$Expected', got '$Actual'" }
}

Test-Case 'the application endpoint name is unchanged' {
    Assert-Equal '\\.\pipe\ExoSnap.LiveVerify.lv-abc' (New-LiveVerifyPipeName -RunId 'lv-abc') 'default role'
}

Test-Case 'the role defaults to the application' {
    Assert-Equal (New-LiveVerifyPipeName -RunId 'lv-abc') `
        (New-LiveVerifyPipeName -RunId 'lv-abc' -Role 'LiveVerify') 'explicit vs default'
}

Test-Case 'the updater endpoint is a different name for the same run id' {
    $app = New-LiveVerifyPipeName -RunId 'lv-abc' -Role 'LiveVerify'
    $updater = New-LiveVerifyPipeName -RunId 'lv-abc' -Role 'Updater'
    Assert-Equal '\\.\pipe\ExoSnap.Updater.lv-abc' $updater 'updater role'
    if ($app -eq $updater) { throw 'one run id must address two endpoints, not one' }
}

Test-Case 'an unknown role is refused rather than silently used' {
    # A typo'd role that produced a plausible name would fail as a connect
    # timeout, which is indistinguishable from "the child never started".
    try {
        New-LiveVerifyPipeName -RunId 'lv-abc' -Role 'Updaters' | Out-Null
        throw 'expected the parameter validation to reject an unknown role'
    }
    catch [System.Management.Automation.ParameterBindingException] {
        # expected
    }
}

Test-Case 'the state comparison names the fields that differ' {
    # A revision that moved is a number. What a reader needs is which observable
    # field moved, because that is what separates a navigation defect from a
    # source that was still resolving.
    $before = [pscustomobject]@{ page = 'record'; canStart = $false; recordingState = 'idle' }
    $after = [pscustomobject]@{ page = 'record'; canStart = $true; recordingState = 'idle' }
    $moved = @(Compare-LiveVerifyStateField -Before $before -After $after)
    Assert-Equal 1 $moved.Count 'exactly one field differs'
    Assert-Equal 'canStart: False -> True' $moved[0] 'the difference is named with both values'
}

Test-Case 'an identical state compares as no difference' {
    $state = [pscustomobject]@{ page = 'settings'; canStart = $true }
    Assert-Equal 0 @(Compare-LiveVerifyStateField -Before $state -After $state).Count `
        'nothing differs between a state and itself'
}

Test-Case 'a field that only one side carries is reported, not skipped' {
    $before = [pscustomobject]@{ page = 'record' }
    $after = [pscustomobject]@{ page = 'record'; blockingSurface = 'recovery' }
    $moved = @(Compare-LiveVerifyStateField -Before $before -After $after)
    Assert-Equal 1 $moved.Count 'the appearing field is a difference'
    Assert-Equal 'blockingSurface:  -> recovery' $moved[0] 'an absent side reads as empty'
}

Write-Host ''
Write-Host "$($script:Ran - $script:Failures)/$($script:Ran) passed"
exit ($script:Failures -gt 0 ? 1 : 0)
