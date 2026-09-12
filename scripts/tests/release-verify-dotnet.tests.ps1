#Requires -Version 7.0
<#
.SYNOPSIS
    Contract tests for the PowerShell-to-.NET release-verify bootstrap.

.DESCRIPTION
    These start only the typed CLI. They do not prepare a campaign, launch ExoSnap,
    mutate the machine, or publish anything. The missing-run case deliberately stops
    before services are opened; its echoed command is the wrapper contract under test.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $scriptRoot 'release-verify.ps1'
$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [scriptblock] $Body)
    try {
        & $Body
        Write-Host "  PASS  $Name" -ForegroundColor Green
        $script:Passed++
    }
    catch {
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed++
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }
function Assert-Equal {
    param($Expected, $Actual, [string] $Message)
    if ("$Expected" -ne "$Actual") { throw "$Message (expected '$Expected', got '$Actual')" }
}

function Invoke-Runner {
    param([Parameter(Mandatory)] [string[]] $Arguments)

    $output = & pwsh -NoProfile -NonInteractive -File $runner @Arguments 2>&1 | Out-String
    return @{ ExitCode = $LASTEXITCODE; Output = $output }
}

Write-Host ''
Write-Host 'The release-verify wrapper selects the typed harness'

Test-Case 'list is forwarded to the typed catalog' {
    $result = Invoke-Runner -Arguments @('list', '-Engine', 'DotNet')
    Assert-Equal 0 $result.ExitCode "typed list failed: $($result.Output)"
    Assert-True ($result.Output -match '\d+ scenarios') 'the typed catalog was not printed'
    # The exact migrated count moves with every slice; assert the line is printed,
    # not the number, so this wrapper test does not need editing on each migration.
    Assert-True ($result.Output -match '\d+ with a migrated body') 'the migration count was not printed'
}

Test-Case 'the typed catalog contains every scenario the PowerShell catalog declares' {
    # The reason the two sizes differ, as an invariant rather than as two numbers
    # nobody can compare. The PowerShell catalog is the set the legacy orchestrator
    # runs; while both exist, the typed one may only ever be a superset of it, so the
    # migration cannot quietly drop a scenario on the way across.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $legacy = @(Get-ReleaseScenarioCatalog | ForEach-Object { $_.Id })

    # Read off the printed catalog rather than through a --json the wrapper does not
    # forward: the point here is the set of ids, and adding a wrapper parameter to
    # make one test tidier would be a contract change for no reason.
    $result = Invoke-Runner -Arguments @('list', '-Engine', 'DotNet')
    Assert-Equal 0 $result.ExitCode "typed list failed: $($result.Output)"
    $typed = @([regex]::Matches($result.Output, '(?m)^(REL-[A-Z0-9-]+)\s') |
            ForEach-Object { $_.Groups[1].Value })
    Assert-True ($typed.Count -gt 0) 'no scenario ids could be read out of the typed catalog'

    $missing = @($legacy | Where-Object { $_ -notin $typed })
    Assert-True ($missing.Count -eq 0) `
        "the typed catalog is missing $($missing.Count) PowerShell scenario(s): $($missing -join ', ')"

    # And the other direction is named rather than forbidden: the typed catalog is
    # ahead by exactly the scenarios that were added after the rewrite began, and a
    # new one has to be added here on purpose.
    $ahead = @($typed | Where-Object { $_ -notin $legacy } | Sort-Object)
    Assert-Equal 'REL-INSTALL-CLEAN-001, REL-PRESENT-XCHECK-001' ($ahead -join ', ') `
        'the typed catalog gained or lost a scenario the PowerShell catalog does not have'
}

Test-Case 'an explicit opt-in class remains selected after translation' {
    $runId = 'wrapper-test-' + [guid]::NewGuid().ToString('n')
    $result = Invoke-Runner -Arguments @(
        'run', '-Engine', 'DotNet', '-RunId', $runId, '-IncludeClass', 'audio-long')
    Assert-Equal 2 $result.ExitCode 'a nonexistent campaign must be refused before any gate runs'

    $invocation = @($result.Output -split "`r?`n" |
            Where-Object { $_ -match '^\s+ExoSnap\.Verify run ' })[0]
    Assert-True ($invocation -match [regex]::Escape('--class audio-long')) 'the class was not forwarded'
    Assert-True ($invocation -match [regex]::Escape('--include-opt-in')) `
        'the wrapper silently excluded the opt-in class the caller explicitly selected'
}

Test-Case 'a command the typed harness does not implement is refused' {
    $result = Invoke-Runner -Arguments @('status', '-Engine', 'DotNet')
    Assert-True ($result.ExitCode -ne 0) 'unsupported status unexpectedly succeeded'
    Assert-True ($result.Output -match "does not carry 'status'") 'the refusal did not name the unsupported command'
}

Test-Case 'the typed bootstrap tolerates an inherited MSVC platform' {
    $priorPlatform = $env:Platform
    try {
        $env:Platform = 'x64'
        $result = Invoke-Runner -Arguments @('list', '-Engine', 'DotNet')
        Assert-Equal 0 $result.ExitCode "the MSVC environment broke the typed bootstrap: $($result.Output)"
        Assert-True ($result.Output -match '\d+ scenarios') 'the typed catalog was not printed'
    }
    finally {
        $env:Platform = $priorPlatform
    }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
