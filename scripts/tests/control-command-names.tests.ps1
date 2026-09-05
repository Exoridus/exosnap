#requires -Version 7.0
<#
.SYNOPSIS
    Every control command a script sends must exist on the endpoint it is sent to.

.DESCRIPTION
    There are TWO control endpoints with overlapping vocabularies: the application
    answers `update.getState`, the updater answers `updater.getState`. A gate that
    sends the application's spelling to the updater is refused with "Unknown
    command", and the scenario reports that as a product failure.

    That is exactly what happened in the rc16 campaign, in a gate that had never
    reached its own verify block before. It cost an operator four correctly
    answered UAC prompts, each thrown away by a different defect in code that had
    never run -- and every one of those defects was findable without a human, which
    is what this file is for.

    The command lists are read from the C++ policy tables rather than copied here:
    a second copy of a vocabulary is a copy that goes stale.

    Fixtures only -- no application, no pipe. Deliberately not Pester, matching the
    other script tests here.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptsRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptsRoot

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

function Assert-True($Condition, [string] $What = 'condition') {
    if (-not $Condition) { throw "$What was false" }
}

# The names a policy table declares, as QStringLiteral("x.y") entries.
function Get-DeclaredCommands([string] $RelativePath) {
    $path = Join-Path $repoRoot $RelativePath
    if (-not (Test-Path -LiteralPath $path)) { throw "policy source is missing: $RelativePath" }
    $source = Get-Content -LiteralPath $path -Raw
    $names = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($match in [regex]::Matches($source, 'QStringLiteral\("([a-z][a-zA-Z]*\.[a-zA-Z]+)"\)')) {
        [void]$names.Add($match.Groups[1].Value)
    }
    return $names
}

# Every `-Connection <var> -Command '<name>'` in the runner, with the variable, so
# a command can be attributed to the endpoint it was sent to.
function Get-SentCommands {
    $sent = @()
    foreach ($file in @('lib/ReleaseScenarios.ps1', 'release-verify.ps1', 'lib/LiveVerifyChecks.ps1')) {
        $path = Join-Path $scriptsRoot $file
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $lines = Get-Content -LiteralPath $path
        for ($i = 0; $i -lt $lines.Count; $i++) {
            foreach ($match in [regex]::Matches($lines[$i], "-Connection \`$([A-Za-z_][A-Za-z0-9_]*)[^\n]*?-Command '([a-z][a-zA-Z]*\.[a-zA-Z]+)'")) {
                $sent += [pscustomobject]@{
                    File = $file
                    Line = $i + 1
                    Connection = $match.Groups[1].Value
                    Command = $match.Groups[2].Value
                }
            }
        }
    }
    return $sent
}

Write-Host 'Control command names'

$appCommands = Get-DeclaredCommands 'app/live_verify/LiveVerifyCommandPolicy.cpp'
$updaterCommands = Get-DeclaredCommands 'apps/updater/UpdaterCommandPolicy.cpp'

Test-Case 'both policy tables were readable and non-trivial' {
    Assert-True ($appCommands.Count -gt 30) "the app policy declared only $($appCommands.Count) commands"
    Assert-True ($updaterCommands.Count -gt 5) "the updater policy declared only $($updaterCommands.Count) commands"
    Assert-True ($appCommands.Contains('record.start')) 'the app policy should declare record.start'
    Assert-True ($updaterCommands.Contains('updater.getState')) 'the updater policy should declare updater.getState'
}

Test-Case 'every command the runner sends exists on the endpoint it is sent to' {
    $sent = Get-SentCommands
    Assert-True ($sent.Count -gt 40) "only $($sent.Count) command sites were found; the scan is probably broken"

    $problems = @()
    foreach ($site in $sent) {
        # The variable name says which endpoint: anything called $updater* is the
        # updater's channel, everything else is the application's.
        $isUpdater = $site.Connection -like 'updater*'
        $known = if ($isUpdater) { $updaterCommands } else { $appCommands }
        $endpoint = if ($isUpdater) { 'updater' } else { 'app' }
        if (-not $known.Contains($site.Command)) {
            # An app command sent to the updater (or the reverse) is the specific
            # confusion this test exists for, so it is named as such.
            $other = if ($isUpdater) { $appCommands } else { $updaterCommands }
            $hint = if ($other.Contains($site.Command)) { " -- that is the $(if ($isUpdater) { 'app' } else { 'updater' })'s spelling" } else { '' }
            $problems += "$($site.File):$($site.Line) sends '$($site.Command)' to the $endpoint endpoint$hint"
        }
    }
    Assert-True ($problems.Count -eq 0) ("unknown commands:`n        " + ($problems -join "`n        "))
}

Write-Host ''
Write-Host "$($script:Ran - $script:Failures)/$($script:Ran) passed"
if ($script:Failures -gt 0) { exit 1 }
exit 0
