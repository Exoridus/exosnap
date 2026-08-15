#requires -Version 7.0
<#
.SYNOPSIS
    Tests for the Live Verify runner's own state machine.

.DESCRIPTION
    Fixtures only -- no application, no pipe, no recording. What is under test is
    the runner's promise: that an interruption is never mistaken for success,
    that a PASS is bound to the artifact and the environment it was produced
    against, and that a corrupt state file is reported rather than quietly reset.
    Those are exactly the properties that are impossible to test by hand, because
    testing them by hand means killing a real acceptance run.

    Deliberately not Pester: it is not a dependency of this repository, and the
    assertions here are simple enough that adding one would cost more than it
    saves.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptsRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptsRoot 'lib/LiveVerifyState.psm1') -Force

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

function Assert-Throws([scriptblock] $Body, [string] $What = 'call') {
    try { & $Body }
    catch { return }
    throw "$What did not throw"
}

function New-TestCatalog {
    return @(
        [pscustomobject]@{ Id = 'T-ART-001'; Title = 'Artifact bound'; Layer = 'FULL_AUTO'
            ArtifactBound = $true; EnvironmentKeys = @() },
        [pscustomobject]@{ Id = 'T-ENV-001'; Title = 'Monitor bound'; Layer = 'CONTROL_CHANNEL'
            ArtifactBound = $true; EnvironmentKeys = @('monitorTopology') },
        [pscustomobject]@{ Id = 'T-FREE-001'; Title = 'Environment free'; Layer = 'FULL_AUTO'
            ArtifactBound = $false; EnvironmentKeys = @() }
    )
}

function New-Sandbox {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("live-verify-tests-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

$sandboxes = [System.Collections.Generic.List[string]]::new()
function New-Run {
    param([hashtable] $Artifact = @{ exeSha256 = 'aaaa'; productVersion = '0.9.0' },
        [hashtable] $Environment = @{ monitorTopology = 'A|B'; primaryScreen = 'A' })
    $sandbox = New-Sandbox
    $sandboxes.Add($sandbox)
    return New-LiveVerifyRun -RunDirectory $sandbox -RunId 'test-run' -Catalog (New-TestCatalog) `
        -Artifact $Artifact -Environment $Environment
}

Write-Host 'live-verify runner state'

Test-Case 'a new run persists every catalog entry as PENDING' {
    $run = New-Run
    Assert-Equal 3 (@($run.State.checks.PSObject.Properties).Count) 'check count'
    foreach ($property in $run.State.checks.PSObject.Properties) {
        Assert-Equal 'PENDING' $property.Value.state $property.Name
    }
    Assert-True (Test-Path (Join-Path $run.Directory 'state.json')) 'state.json exists'
    Assert-True (Test-Path (Join-Path $run.Directory 'artifact-fingerprint.json')) 'artifact file exists'
}

Test-Case 'a PASS survives a reload' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' -ArtifactFingerprint 'fp' -EnvironmentFingerprint 'none' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'PASS' -Message 'ok' -Evidence @('checks/T-ART-001/a.json') | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'PASS' $reloaded.State.checks.'T-ART-001'.state 'state'
    Assert-Equal 'ok' $reloaded.State.checks.'T-ART-001'.message 'message'
    Assert-Equal 1 (@($reloaded.State.checks.'T-ART-001'.evidence).Count) 'evidence count'
    Assert-Equal 1 $reloaded.State.checks.'T-ART-001'.attempts 'attempts'
}

Test-Case 'a FAIL survives a reload and keeps its message' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'FAIL' -Message 'exit 3' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'FAIL' $reloaded.State.checks.'T-ART-001'.state 'state'
    Assert-Equal 'exit 3' $reloaded.State.checks.'T-ART-001'.message 'message'
}

Test-Case 'RUNNING is persisted before the check executes' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ENV-001' -ArtifactFingerprint 'fp' -EnvironmentFingerprint 'env' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'RUNNING' $reloaded.State.checks.'T-ENV-001'.state 'state'
    Assert-True ($null -ne $reloaded.State.checks.'T-ENV-001'.startedUtc) 'startedUtc recorded'
}

Test-Case 'an interrupted RUNNING check becomes UNVERIFIED, never PASS' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ENV-001' | Out-Null
    # Simulates the runner dying: the process goes away, state.json keeps RUNNING.
    $resumed = Get-LiveVerifyRun -RunDirectory $run.Directory
    $stranded = Resolve-LiveVerifyInterrupted -Run $resumed
    Assert-Equal 1 $stranded.Count 'stranded count'
    Assert-Equal 'T-ENV-001' $stranded[0] 'stranded id'
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'UNVERIFIED' $reloaded.State.checks.'T-ENV-001'.state 'state'
    Assert-True $reloaded.State.checks.'T-ENV-001'.interrupted 'interrupted flag'
}

Test-Case 'resume reruns what is unresolved and leaves a PASS alone' {
    $run = New-Run
    $catalog = New-TestCatalog
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'PASS' | Out-Null
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ENV-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ENV-001' -Result 'UNVERIFIED' | Out-Null

    $runnable = @(Get-LiveVerifyRunnableChecks -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) -Catalog $catalog)
    $ids = @($runnable | ForEach-Object { $_.Id })
    Assert-True ($ids -notcontains 'T-ART-001') 'a PASS is not rerun'
    Assert-True ($ids -contains 'T-ENV-001') 'an UNVERIFIED check is rerun'
    Assert-True ($ids -contains 'T-FREE-001') 'a PENDING check is run'

    # The shape, not just the contents. A nested result reads correctly through a
    # pipeline -- member enumeration flattens it -- and still hands the runner one
    # "entry" that is the entire list. That shipped once and stopped every run
    # before it started, with the ids intact in this very assertion.
    Assert-True ($runnable.Count -eq $ids.Count) 'one element per runnable check, not one nested list'
    foreach ($entry in $runnable) {
        Assert-True ($entry.Id -is [string]) 'each element is a single check, so its Id is a string'
    }
}

Test-Case 'a changed artifact makes an artifact-bound PASS stale' {
    $catalog = New-TestCatalog
    $artifact = @{ exeSha256 = 'aaaa'; productVersion = '0.9.0' }
    $environment = @{ monitorTopology = 'A|B'; primaryScreen = 'A' }
    $run = New-Run -Artifact $artifact -Environment $environment
    $fingerprint = Get-LiveVerifyFingerprint -Properties $artifact

    foreach ($id in @('T-ART-001', 'T-FREE-001')) {
        $entry = @($catalog | Where-Object { $_.Id -eq $id })[0]
        $environmentFingerprint = Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment $environment
        Set-LiveVerifyCheckRunning -Run $run -Id $id -ArtifactFingerprint $fingerprint `
            -EnvironmentFingerprint $environmentFingerprint | Out-Null
        Complete-LiveVerifyCheck -Run $run -Id $id -Result 'PASS' | Out-Null
    }

    $rebuilt = @{ exeSha256 = 'bbbb'; productVersion = '0.9.0' }
    $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog -ArtifactFingerprint (Get-LiveVerifyFingerprint -Properties $rebuilt) -Environment $environment
    Assert-True ($stale -contains 'T-ART-001') 'the artifact-bound check went stale'
    Assert-True ($stale -notcontains 'T-FREE-001') 'a check that does not depend on the artifact did not'
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'STALE' $reloaded.State.checks.'T-ART-001'.state 'state'
    Assert-Equal 'PASS' $reloaded.State.checks.'T-FREE-001'.state 'unaffected state'
}

Test-Case 'a changed artifact also invalidates a FAIL, so a fix is not left unobserved' {
    $catalog = New-TestCatalog
    $artifact = @{ exeSha256 = 'aaaa' }
    $environment = @{ monitorTopology = 'A|B'; primaryScreen = 'A' }
    $run = New-Run -Artifact $artifact -Environment $environment
    $fingerprint = Get-LiveVerifyFingerprint -Properties $artifact
    $entry = @($catalog | Where-Object { $_.Id -eq 'T-ART-001' })[0]

    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' -ArtifactFingerprint $fingerprint `
        -EnvironmentFingerprint (Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment $environment) | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'FAIL' -Message 'broken' | Out-Null

    $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog -ArtifactFingerprint (Get-LiveVerifyFingerprint -Properties @{ exeSha256 = 'bbbb' }) -Environment $environment
    Assert-True ($stale -contains 'T-ART-001') 'the FAIL was invalidated'
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'STALE' $reloaded.State.checks.'T-ART-001'.state 'state'
    Assert-True ($reloaded.State.checks.'T-ART-001'.message -match 'FAIL') 'the previous outcome is named'
}

Test-Case 'a skip is a human decision and survives an artifact change' {
    $catalog = New-TestCatalog
    $run = New-Run -Artifact @{ exeSha256 = 'aaaa' } -Environment @{ monitorTopology = 'A'; primaryScreen = 'A' }
    Set-LiveVerifyCheckSkipped -Run $run -Id 'T-ART-001' -Reason 'no second monitor today' | Out-Null
    $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog -ArtifactFingerprint (Get-LiveVerifyFingerprint -Properties @{ exeSha256 = 'bbbb' }) `
        -Environment @{ monitorTopology = 'A'; primaryScreen = 'A' }
    Assert-True ($stale -notcontains 'T-ART-001') 'a skip is not invalidated'
    Assert-Equal 'SKIPPED' (Get-LiveVerifyRun -RunDirectory $run.Directory).State.checks.'T-ART-001'.state 'state'
}

Test-Case 'an unperformed human gate is re-offered by the runnable set' {
    $catalog = New-TestCatalog
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'MANUAL_REQUIRED' | Out-Null
    $ids = @((Get-LiveVerifyRunnableChecks -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) -Catalog $catalog) |
            ForEach-Object { $_.Id })
    Assert-True ($ids -contains 'T-ART-001') 'MANUAL_REQUIRED is runnable again'
}

Test-Case 'a FAIL against the current artifact is not silently rerun' {
    $catalog = New-TestCatalog
    $artifact = @{ exeSha256 = 'aaaa' }
    $run = New-Run -Artifact $artifact
    $entry = @($catalog | Where-Object { $_.Id -eq 'T-ART-001' })[0]
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' `
        -ArtifactFingerprint (Get-LiveVerifyFingerprint -Properties $artifact) `
        -EnvironmentFingerprint (Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment @{}) | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'FAIL' -Message 'a real finding' | Out-Null
    $ids = @((Get-LiveVerifyRunnableChecks -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) -Catalog $catalog) |
            ForEach-Object { $_.Id })
    Assert-True ($ids -notcontains 'T-ART-001') 'a FAIL needs an explicit retry'
}

Test-Case 'an environment change only invalidates the checks that declared it' {
    $catalog = New-TestCatalog
    $artifact = @{ exeSha256 = 'aaaa' }
    $environment = @{ monitorTopology = 'A|B'; primaryScreen = 'A' }
    $run = New-Run -Artifact $artifact -Environment $environment
    $fingerprint = Get-LiveVerifyFingerprint -Properties $artifact

    foreach ($id in @('T-ART-001', 'T-ENV-001')) {
        $entry = @($catalog | Where-Object { $_.Id -eq $id })[0]
        Set-LiveVerifyCheckRunning -Run $run -Id $id -ArtifactFingerprint $fingerprint `
            -EnvironmentFingerprint (Get-LiveVerifyEnvironmentFingerprint -Entry $entry -Environment $environment) | Out-Null
        Complete-LiveVerifyCheck -Run $run -Id $id -Result 'PASS' | Out-Null
    }

    $rearranged = @{ monitorTopology = 'A|B|C'; primaryScreen = 'A' }
    $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog -ArtifactFingerprint (Get-LiveVerifyFingerprint -Properties $artifact) -Environment $rearranged
    Assert-True ($stale -contains 'T-ENV-001') 'the monitor-dependent check went stale'
    Assert-True ($stale -notcontains 'T-ART-001') 'an unrelated check did not'
}

Test-Case 'retry puts a check back to PENDING and drops the old evidence link' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'FAIL' -Message 'nope' -Evidence @('checks/x.json') | Out-Null
    Reset-LiveVerifyCheck -Run $run -Id 'T-ART-001' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'PENDING' $reloaded.State.checks.'T-ART-001'.state 'state'
    Assert-Equal 0 (@($reloaded.State.checks.'T-ART-001'.evidence).Count) 'evidence cleared'
    Assert-Equal 1 $reloaded.State.checks.'T-ART-001'.attempts 'attempt history kept'
}

Test-Case 'a skip records its reason and an empty reason is refused' {
    $run = New-Run
    Set-LiveVerifyCheckSkipped -Run $run -Id 'T-ENV-001' -Reason 'single-monitor machine' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-Equal 'SKIPPED' $reloaded.State.checks.'T-ENV-001'.state 'state'
    Assert-Equal 'single-monitor machine' $reloaded.State.checks.'T-ENV-001'.skipReason 'reason'
    Assert-Throws { Set-LiveVerifyCheckSkipped -Run $run -Id 'T-ART-001' -Reason '   ' } 'an unexplained skip'
}

Test-Case 'notes accumulate' {
    $run = New-Run
    Add-LiveVerifyNote -Run $run -Id 'T-ART-001' -Note 'first' | Out-Null
    Add-LiveVerifyNote -Run $run -Id 'T-ART-001' -Note 'second' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $run.Directory
    Assert-True ($reloaded.State.checks.'T-ART-001'.note -match 'first') 'first note kept'
    Assert-True ($reloaded.State.checks.'T-ART-001'.note -match 'second') 'second note kept'
}

Test-Case 'an unknown check id is refused rather than silently created' {
    $run = New-Run
    Assert-Throws { Get-LiveVerifyCheck -Run $run -Id 'T-NOPE-999' } 'an unknown id'
}

Test-Case 'a corrupt state file is reported, not reset' {
    $run = New-Run
    Set-Content -LiteralPath (Join-Path $run.Directory 'state.json') -Value '{ this is not json' -Encoding utf8NoBOM
    Assert-Throws { Get-LiveVerifyRun -RunDirectory $run.Directory } 'loading a corrupt run'
    # And the damaged file is still there for inspection rather than overwritten.
    Assert-True ((Get-Content -LiteralPath (Join-Path $run.Directory 'state.json') -Raw) -match 'not json') `
        'the corrupt file was left in place'
}

Test-Case 'a missing run directory is an error, not an empty run' {
    Assert-Throws { Get-LiveVerifyRun -RunDirectory (Join-Path ([System.IO.Path]::GetTempPath()) 'no-such-live-verify-run') } 'loading a missing run'
}

Test-Case 'the report separates outcomes and names an evidence-free PASS' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'PASS' -Evidence @('checks/T-ART-001/a.json') | Out-Null
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ENV-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ENV-001' -Result 'FAIL' -Message 'stalled' | Out-Null
    Set-LiveVerifyCheckSkipped -Run $run -Id 'T-FREE-001' -Reason 'not applicable' | Out-Null

    $summary = Write-LiveVerifyReport -Run (Get-LiveVerifyRun -RunDirectory $run.Directory)
    Assert-Equal 1 $summary['PASS'] 'PASS count'
    Assert-Equal 1 $summary['FAIL'] 'FAIL count'
    Assert-Equal 1 $summary['SKIPPED'] 'SKIPPED count'

    $markdown = Get-Content -LiteralPath (Join-Path $run.Directory 'report.md') -Raw
    Assert-True ($markdown -match '## PASS') 'PASS section'
    Assert-True ($markdown -match '## FAIL') 'FAIL section'
    Assert-True ($markdown -match '## SKIPPED') 'SKIPPED section'
    Assert-True ($markdown -match 'not applicable') 'skip reason in the report'

    $junit = Get-Content -LiteralPath (Join-Path $run.Directory 'junit.xml') -Raw
    Assert-True ($junit -match '<failure ') 'junit failure element'
    Assert-True ($junit -match '<skipped ') 'junit skipped element'

    $json = Get-Content -LiteralPath (Join-Path $run.Directory 'report.json') -Raw | ConvertFrom-Json
    Assert-Equal 3 (@($json.checks).Count) 'json check count'
}

Test-Case 'an automated PASS with no evidence is called out in the report' {
    $run = New-Run
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-ART-001' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-ART-001' -Result 'PASS' | Out-Null
    Write-LiveVerifyReport -Run (Get-LiveVerifyRun -RunDirectory $run.Directory) | Out-Null
    $markdown = Get-Content -LiteralPath (Join-Path $run.Directory 'report.md') -Raw
    Assert-True ($markdown -match 'none recorded') 'the missing evidence is named'
}

foreach ($sandbox in $sandboxes) {
    Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "$($script:Ran - $script:Failures)/$($script:Ran) passed"
if ($script:Failures -gt 0) { exit 1 }
exit 0

