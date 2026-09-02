#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the release verification runner and the environment orchestrator.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use, so CTest runs
    all of them the same way and a contributor reads one style.

    Everything here runs against a FAKE envctl -- a script that reports whatever the
    test needs it to report, including lying about success. That is the point. The
    failures worth pinning are the ones where a setter claims it worked and the
    read-back disagrees, and no real display can be talked into that on demand.

    Nothing in this file mutates the machine.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptRoot 'lib/LiveVerifyState.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $scriptRoot 'lib/EnvironmentOrchestrator.psm1') -Force -DisableNameChecking

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
function Assert-Throws {
    param([scriptblock] $Body, [string] $Message)
    try { & $Body } catch { return }
    throw $Message
}

function New-TestDirectory {
    $path = Join-Path ([IO.Path]::GetTempPath()) "release-verify-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

function New-FakeEnvctl {
    <#
    .SYNOPSIS
        Writes a fake exosnap-envctl whose every answer comes from a JSON script file.
    .DESCRIPTION
        The fake reads `behaviour.json` from its own directory on every invocation, so
        a test can change what the next call returns -- which is how "the setter said
        yes and the read-back said no" is reproduced without a real device.
    #>
    param([Parameter(Mandatory)] [string] $Directory)

    $path = Join-Path $Directory 'fake-envctl.ps1'
    $body = @'
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $Arguments)
$behaviourPath = Join-Path $PSScriptRoot 'behaviour.json'
$behaviour = Get-Content -LiteralPath $behaviourPath -Raw | ConvertFrom-Json
$verb = $Arguments[0]
# Every invocation is appended to a call log so a test can assert what was NOT done --
# "no other device was touched" is only checkable against a record of the calls.
$logPath = Join-Path $PSScriptRoot 'calls.jsonl'
Add-Content -LiteralPath $logPath -Value (@{ verb = $verb; args = $Arguments } | ConvertTo-Json -Compress)
$node = $behaviour.$verb
if ($null -eq $node) { Write-Output (@{ error = "no behaviour for '$verb'" } | ConvertTo-Json -Compress); exit 3 }
Write-Output ($node.output | ConvertTo-Json -Depth 20 -Compress)
exit ([int]$node.exit)
'@
    Set-Content -LiteralPath $path -Value $body -Encoding utf8NoBOM
    return $path
}

function Set-FakeBehaviour {
    param([Parameter(Mandatory)] [string] $Directory, [Parameter(Mandatory)] [hashtable] $Behaviour)
    Set-Content -LiteralPath (Join-Path $Directory 'behaviour.json') `
        -Value ($Behaviour | ConvertTo-Json -Depth 20) -Encoding utf8NoBOM
}

function Get-FakeCalls {
    param([Parameter(Mandatory)] [string] $Directory)
    $path = Join-Path $Directory 'calls.jsonl'
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    return @(Get-Content -LiteralPath $path | ForEach-Object { $_ | ConvertFrom-Json })
}

function New-TestCatalogEntry {
    param([string] $Id = 'T-001', [hashtable] $Desired = @{}, [scriptblock] $Run, [hashtable] $Requires = @{})
    return [pscustomobject]@{
        Id                  = $Id
        Title               = "test scenario $Id"
        Class               = 'test'
        Layer               = 'FULL_AUTO'
        Source              = 'tests'
        ArtifactBound       = $true
        RequiresInstallTree = $false
        EnvironmentKeys     = @('probe')
        Requires            = $Requires
        Desired             = $Desired
        Run                 = $Run
    }
}

Write-Host ''
Write-Host 'Release verification runner tests' -ForegroundColor Cyan
Write-Host ''

# ---------------------------------------------------------------------------
# Result taxonomy
# ---------------------------------------------------------------------------

Test-Case 'DEFERRED and UNAVAILABLE are first-class terminal states' {
    $states = Get-LiveVerifyCheckStates
    Assert-True ($states -contains 'DEFERRED') 'DEFERRED must be a check state'
    Assert-True ($states -contains 'UNAVAILABLE') 'UNAVAILABLE must be a check state'
}

Test-Case 'a restore verdict is recorded separately from the product verdict' {
    $directory = New-TestDirectory
    $catalog = @(New-TestCatalogEntry)
    $run = New-LiveVerifyRun -RunDirectory $directory -RunId 'r1' -Catalog $catalog `
        -Artifact @{ fingerprint = 'a' } -Environment @{ probe = '1' }
    Complete-LiveVerifyCheck -Run $run -Id 'T-001' -Result 'PASS' -Message 'product fine' `
        -RestoreResult 'RESTORE_FAILED' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $directory
    Assert-Equal 'PASS' $reloaded.State.checks.'T-001'.state 'the product verdict must survive'
    Assert-Equal 'RESTORE_FAILED' $reloaded.State.checks.'T-001'.restoreResult 'the restore verdict must survive'
}

Test-Case 'a PASS with a broken restore is a failure in the machine-readable report' {
    $directory = New-TestDirectory
    $catalog = @(New-TestCatalogEntry)
    $run = New-LiveVerifyRun -RunDirectory $directory -RunId 'r1' -Catalog $catalog `
        -Artifact @{ fingerprint = 'a' } -Environment @{ probe = '1' }
    Complete-LiveVerifyCheck -Run $run -Id 'T-001' -Result 'PASS' -RestoreResult 'RESTORE_FAILED' | Out-Null
    Write-LiveVerifyReport -Run (Get-LiveVerifyRun -RunDirectory $directory) | Out-Null
    $junit = Get-Content -LiteralPath (Join-Path $directory 'junit.xml') -Raw
    Assert-True ($junit -match '<failure') 'a product PASS with a failed restore must not report as green'
    $report = Get-Content -LiteralPath (Join-Path $directory 'report.json') -Raw | ConvertFrom-Json
    Assert-Equal 1 $report.restoreSummary.RESTORE_FAILED 'the restore summary must count it'
}

Test-Case 'a deferred gate is rerunnable and an unavailable one is not' {
    $directory = New-TestDirectory
    $catalog = @(New-TestCatalogEntry -Id 'T-DEF'), (New-TestCatalogEntry -Id 'T-UNAVAIL')
    $run = New-LiveVerifyRun -RunDirectory $directory -RunId 'r1' -Catalog $catalog `
        -Artifact @{ fingerprint = 'a' } -Environment @{ probe = '1' }
    Complete-LiveVerifyCheck -Run $run -Id 'T-DEF' -Result 'DEFERRED' | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-UNAVAIL' -Result 'UNAVAILABLE' | Out-Null
    $runnable = @(Get-LiveVerifyRunnableChecks -Run (Get-LiveVerifyRun -RunDirectory $directory) -Catalog $catalog)
    $ids = @($runnable | ForEach-Object { $_.Id })
    Assert-True ($ids -contains 'T-DEF') 'a gate nobody answered is an open question'
    Assert-True (-not ($ids -contains 'T-UNAVAIL')) 'absent hardware is still absent; do not retry it every run'
}

Test-Case 'UNAVAILABLE goes stale when the environment changes' {
    $directory = New-TestDirectory
    $catalog = @(New-TestCatalogEntry -Id 'T-HW')
    $run = New-LiveVerifyRun -RunDirectory $directory -RunId 'r1' -Catalog $catalog `
        -Artifact @{ fingerprint = 'a' } -Environment @{ probe = 'no-hdr' }
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-HW' -ArtifactFingerprint 'a' `
        -EnvironmentFingerprint (Get-LiveVerifyEnvironmentFingerprint -Entry $catalog[0] -Environment @{ probe = 'no-hdr' }) | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-HW' -Result 'UNAVAILABLE' -Message 'no HDR display' | Out-Null

    $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog -ArtifactFingerprint 'a' `
        -Environment @{ probe = 'hdr-attached' }
    Assert-True ($stale -contains 'T-HW') 'plugging the display in must make the scenario runnable again'
}

Test-Case 'an artifact change invalidates a release PASS' {
    $directory = New-TestDirectory
    $catalog = @(New-TestCatalogEntry)
    $run = New-LiveVerifyRun -RunDirectory $directory -RunId 'r1' -Catalog $catalog `
        -Artifact @{ fingerprint = 'sha-old' } -Environment @{ probe = '1' }
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-001' -ArtifactFingerprint 'sha-old' `
        -EnvironmentFingerprint (Get-LiveVerifyEnvironmentFingerprint -Entry $catalog[0] -Environment @{ probe = '1' }) | Out-Null
    Complete-LiveVerifyCheck -Run $run -Id 'T-001' -Result 'PASS' | Out-Null
    $stale = Update-LiveVerifyStaleness -Run $run -Catalog $catalog -ArtifactFingerprint 'sha-new' `
        -Environment @{ probe = '1' }
    Assert-True ($stale -contains 'T-001') 'a PASS must not be inherited by a binary nobody tested'
}

Test-Case 'an interrupted scenario resumes as UNVERIFIED, never as PASS' {
    $directory = New-TestDirectory
    $catalog = @(New-TestCatalogEntry)
    $run = New-LiveVerifyRun -RunDirectory $directory -RunId 'r1' -Catalog $catalog `
        -Artifact @{ fingerprint = 'a' } -Environment @{ probe = '1' }
    Set-LiveVerifyCheckRunning -Run $run -Id 'T-001' -ArtifactFingerprint 'a' -EnvironmentFingerprint 'e' | Out-Null
    $reloaded = Get-LiveVerifyRun -RunDirectory $directory
    Assert-Equal 'RUNNING' $reloaded.State.checks.'T-001'.state 'RUNNING is persisted before execution'
    Resolve-LiveVerifyInterrupted -Run $reloaded | Out-Null
    Assert-Equal 'UNVERIFIED' (Get-LiveVerifyRun -RunDirectory $directory).State.checks.'T-001'.state `
        'a killed runner leaves an unknown outcome, not a good one'
}

# ---------------------------------------------------------------------------
# Environment orchestration
# ---------------------------------------------------------------------------

Test-Case 'a no-op desired state mutates nothing and needs no restore' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{ recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } } }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake

    $ran = $false
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'noop' -Desired @{} `
        -Body { param($t) $script:noopRan = $true; return @{ Result = 'PASS' } }
    Assert-Equal 'NOT_APPLICABLE' $outcome.RestoreResult 'a scenario that mutated nothing has no restore verdict'
    Assert-Equal 'PASS' $outcome.Product.Result 'the body still runs'
    $calls = @(Get-FakeCalls -Directory $directory | Where-Object { $_.verb -eq 'begin' })
    Assert-Equal 0 $calls.Count 'no transaction may be opened for an empty desired state'
    [void]$ran
}

Test-Case 'a setter that claims success but reads back differently is a failure' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    # envctl is the component that compares; the contract asserted here is that the
    # orchestrator surfaces its verdict instead of trusting the exit code of `begin`.
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        # `state` is what real envctl reports about ITS OWN rollback, and it always
        # emits it. Restored here means the rollback verified, so nothing is owed.
        begin   = @{ exit = 1; output = @{ ok = $false; errorCode = 'verify_mismatch'; state = 'Restored'; error = 'read-back mismatch: requested 60, actual 144' } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $script:bodyRan = $false
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'mismatch' `
        -Desired @{ 'display.main-hdr:refresh-hz' = '60' } `
        -Body { param($t) $script:bodyRan = $true; @{ Result = 'PASS' } }
    Assert-True (-not $script:bodyRan) 'an apply whose read-back disagreed must not reach the scenario body'
    Assert-Equal 'verify_mismatch' $outcome.SetupErrorCode 'the failure must be reported with its typed code'
    Assert-True ($null -eq $outcome.Product) 'there is no product verdict when the product was never exercised'
    # Typed rather than thrown, because the caller has to tell "this display does not
    # offer 60 Hz" (UNAVAILABLE) from "the setter lied" (FAIL), and a thrown string
    # collapses both into a scenario crash.
    Assert-Equal 'NOT_APPLICABLE' $outcome.RestoreResult 'a transaction that never opened has no restore verdict'
}

Test-Case 'a display that does not offer the requested mode is UNAVAILABLE, not FAIL' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 1; output = @{ ok = $false; errorCode = 'apply_rejected'; state = 'Restored'; error = 'the display does not offer 60 Hz' } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'unsupported-mode' `
        -Desired @{ 'display.main-hdr:refresh-hz' = '60' } -Body { param($t) @{ Result = 'PASS' } }
    Assert-Equal 'apply_rejected' $outcome.SetupErrorCode 'the refusal must keep its code so the runner can classify it'
    Assert-True (-not $orchestrator.Dirty) 'a refused apply leaves nothing to restore and nothing dirty'
}

Test-Case 'the restore runs even when the scenario body throws' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 0; output = @{ ok = $true; transactionId = 't1'; journalPath = 'j1'; state = 'Active'; applied = @() } }
        restore = @{ exit = 0; output = @{ ok = $true; state = 'Restored'; evidence = @{}; pending = @() } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'boom' `
        -Desired @{ 'display.main-hdr:hdr' = 'off' } -Body { param($t) throw 'the product blew up' }
    Assert-Equal 'RESTORED' $outcome.RestoreResult 'a body that threw must still leave the machine restored'
    Assert-True ($outcome.Error -match 'blew up') 'the body failure must be reported, not swallowed'
    $calls = @(Get-FakeCalls -Directory $directory | Where-Object { $_.verb -eq 'restore' })
    Assert-Equal 1 $calls.Count 'restore must have been called exactly once'
}

Test-Case 'a restore whose read-back disagrees is RESTORE_FAILED and marks the run dirty' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 0; output = @{ ok = $true; transactionId = 't1'; journalPath = 'j1'; state = 'Active'; applied = @() } }
        restore = @{ exit = 4; output = @{ ok = $false; state = 'RestoreFailed'; evidence = @{}; pending = @() } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'restore-broken' `
        -Desired @{ 'display.main-hdr:hdr' = 'off' } -Body { param($t) @{ Result = 'PASS' } }
    Assert-Equal 'PASS' $outcome.Product.Result 'the product verdict is independent'
    Assert-Equal 'RESTORE_FAILED' $outcome.RestoreResult 'the restore verdict is its own answer'
    Assert-True $orchestrator.Dirty 'a failed restore must leave the orchestrator dirty'
}

Test-Case 'a device that vanished before restore yields RESTORE_PENDING_DEVICE_UNAVAILABLE' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 0; output = @{ ok = $true; transactionId = 't1'; journalPath = 'j1'; state = 'Active'; applied = @() } }
        restore = @{ exit = 4; output = @{
                state = 'RestorePendingDeviceUnavailable'
                evidence      = @{ 'audio.render.normal:default' = @{ before = 'on'; requested = 'off'; applied = 'off'; afterRestore = '<device absent>' } }
            }
        }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'device-gone' `
        -Desired @{ 'audio.render.normal:default' = 'off' } -Body { param($t) @{ Result = 'PASS' } }
    Assert-Equal 'RESTORE_PENDING_DEVICE_UNAVAILABLE' $outcome.RestoreResult 'a missing device is its own outcome'
    Assert-True $orchestrator.Dirty 'a pending restore blocks the next mutating scenario'
    Assert-True ($null -ne $outcome.Evidence) 'the evidence naming the unrestored property must survive'
}

Test-Case 'a begin whose own rollback failed leaves the environment dirty' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    # The premise "begin rolls back what it applied, so there is nothing to restore"
    # is false exactly when that rollback is what failed. envctl says so in `state`,
    # and the orchestrator has to believe it: a journal is still on disk with an
    # outstanding debt, and the next mutating scenario must not run.
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 4; output = @{
                ok        = $false; errorCode = 'verify_mismatch'; state = 'RestoreFailed'
                error     = 'read-back mismatch, and the rollback did not verify either'
                pending   = @(@{ alias = 'display.main-hdr'; property = 'display.main-hdr:hdr'; originalValue = 'on' })
            }
        }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory `
        -JournalPath (Join-Path $directory 'env-journal.json') -EnvctlPath $fake
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'rollback-broke' `
        -Desired @{ 'display.main-hdr:hdr' = 'off' } -Body { param($t) @{ Result = 'PASS' } }
    Assert-Equal 'RESTORE_FAILED' $outcome.RestoreResult 'a failed rollback is not NOT_APPLICABLE'
    Assert-True $orchestrator.Dirty 'a failed rollback must block the next mutating scenario'
    Assert-True ($null -ne $outcome.Pending) 'what is still owed must survive into the report'
}

Test-Case 'a begin that answers with no state at all is treated as unknown, not as clean' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 1; output = @{ ok = $false; errorCode = 'apply_rejected'; error = 'no state field at all' } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory `
        -JournalPath (Join-Path $directory 'env-journal.json') -EnvctlPath $fake
    $outcome = Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'no-state' `
        -Desired @{ 'display.main-hdr:hdr' = 'off' } -Body { param($t) @{ Result = 'PASS' } }
    Assert-Equal 'RESTORE_FAILED' $outcome.RestoreResult 'nothing can be concluded, so nothing may be claimed'
    Assert-True $orchestrator.Dirty 'an unknown machine state is the strongest reason not to mutate again'
}

Test-Case 'a restore reporting Clean while ok is false is never RESTORED' {
    # envctl answers a journal it cannot PARSE with its default state (Clean) and
    # ok=false, because no transaction was ever constructed to have a state. A
    # state-only mapping turned "the machine may be mutated and I cannot say how"
    # into RESTORED.
    Assert-Equal 'RESTORE_FAILED' (ConvertTo-RestoreResult -State 'Clean' -Ok $false) `
        'ok=false must beat a benign-looking state'
    Assert-Equal 'RESTORED' (ConvertTo-RestoreResult -State 'Clean' -Ok $true) `
        'a genuinely clean transaction is still restored'
    Assert-Equal 'RESTORED' (ConvertTo-RestoreResult -State 'Restored') `
        'without an ok flag the state alone still decides'
}

Test-Case 'the journal is machine-wide, not filed under the campaign' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
    }
    # No -JournalPath: this is the default every campaign gets.
    $orchestrator = New-EnvironmentOrchestrator -RunId 'rel-20260817-1' -JournalDirectory $directory -EnvctlPath $fake
    Assert-True ($orchestrator.JournalPath -notlike "*$directory*") `
        'a journal inside the campaign directory is invisible to the next campaign'
    Assert-True ($orchestrator.JournalPath -like '*env-journal.json') 'it is still envctl''s journal file'

    # Two campaigns, one machine, one journal -- otherwise a crashed campaign
    # snapshots the already-mutated value as its "original" and reports RESTORED.
    $second = New-EnvironmentOrchestrator -RunId 'rel-20260817-2' -JournalDirectory (New-TestDirectory) -EnvctlPath $fake
    Assert-Equal $orchestrator.JournalPath $second.JournalPath 'two campaigns must recover from the same journal'
}

Test-Case 'every envctl call that resolves an alias carries the profile' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        begin   = @{ exit = 0; output = @{ ok = $true; transactionId = 't1'; journalPath = 'j1'; state = 'Active'; applied = @() } }
        restore = @{ exit = 0; output = @{ ok = $true; state = 'Restored'; evidence = @{}; pending = @() } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory `
        -JournalPath (Join-Path $directory 'env-journal.json') -EnvctlPath $fake -AliasProfile 'C:\profile.json'
    [void](Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'profile' `
            -Desired @{ 'display.main-hdr:hdr' = 'off' } -Body { param($t) @{ Result = 'PASS' } })
    # The restore is the call that lost it. Without --profile no aliases load,
    # DevicePresent() is false for everything, and the restore refuses to put back a
    # device that is plainly attached -- on the one path where giving up is worst.
    foreach ($verb in @('recover', 'begin', 'restore')) {
        $call = @(Get-FakeCalls -Directory $directory | Where-Object { $_.verb -eq $verb }) | Select-Object -First 1
        Assert-True ($null -ne $call) "$verb was never called"
        Assert-True (@($call.args) -contains '--profile') "$verb must carry --profile"
    }
}

Test-Case 'a dirty environment blocks the next mutating scenario' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $true; recovered = $false; mutationAllowed = $false; state = 'RestoreFailed'; error = 'HDR left off on display.main-hdr' } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    Assert-True $orchestrator.Dirty 'startup recovery reported the environment as dirty'
    Assert-Throws { Assert-EnvironmentClean -Orchestrator $orchestrator } `
        'a dirty environment must refuse a new mutation, not warn about one'
    Assert-Throws {
        Invoke-EnvironmentTransaction -Orchestrator $orchestrator -Scenario 'x' `
            -Desired @{ 'display.main-hdr:hdr' = 'off' } -Body { param($t) @{ Result = 'PASS' } }
    } 'the transaction itself must enforce the same gate'
}

Test-Case 'startup recovery restores before anything else runs' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover = @{ exit = 0; output = @{ journalPresent = $true; recovered = $true; mutationAllowed = $true; state = 'Restored'; evidence = @{ properties = @(@{ property = 'display.main-hdr:hdr'; before = 'on'; afterRestore = 'on' }) } } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    Assert-True (-not $orchestrator.Dirty) 'a completed recovery leaves a clean environment'
    Assert-True (@($orchestrator.Recovered) -contains 'display.main-hdr:hdr') 'what was restored must be reported'
    $calls = @(Get-FakeCalls -Directory $directory)
    Assert-Equal 'recover' $calls[0].verb 'recovery must be the FIRST thing the orchestrator does'
}

Test-Case 'a recovery pass that itself fails leaves the run dirty rather than throwing' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{ recover = @{ exit = 1; output = @{ ok = $false; errorCode = 'journal_read_failed'; error = 'journal unreadable' } } }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    Assert-True $orchestrator.Dirty 'a failed recovery is the strongest reason not to mutate anything'
    Assert-True ($orchestrator.DirtyDetail -match 'journal unreadable') 'the reason must be carried, not lost'
}

Test-Case 'an unbound alias is UNAVAILABLE, not a product failure' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover           = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        'resolve-aliases' = @{ exit = 1; output = @{ ok = $false; bindings = @(); candidates = @(); errors = @(@{ code = 'unbound_alias'; alias = 'display.main-hdr'; message = 'bind it with exosnap-envctl bind-alias' }) } }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $verdict = Test-EnvironmentRequirement -Orchestrator $orchestrator -Requirement @{ display = 'display.main-hdr' }
    Assert-True (-not $verdict.Satisfied) 'an unbound alias cannot satisfy a requirement'
    Assert-True ($verdict.Reason -match 'unbound_alias') 'the reason must name the failure class'
}

Test-Case 'an ambiguous alias is refused rather than resolved' {
    $directory = New-TestDirectory
    $fake = New-FakeEnvctl -Directory $directory
    Set-FakeBehaviour -Directory $directory -Behaviour @{
        recover           = @{ exit = 0; output = @{ journalPresent = $false; recovered = $false; mutationAllowed = $true; state = 'Clean' } }
        'resolve-aliases' = @{ exit = 1; output = @{ ok = $false; errors = @(); candidates = @(); bindings = @(
                    @{ alias = 'display.main-hdr'; kind = 'display'; stableId = 'x'; status = 'ambiguous_device' }
                )
            }
        }
    }
    $orchestrator = New-EnvironmentOrchestrator -RunId 'r1' -JournalDirectory $directory -EnvctlPath $fake
    $verdict = Test-EnvironmentRequirement -Orchestrator $orchestrator -Requirement @{ display = 'display.main-hdr' }
    Assert-True (-not $verdict.Satisfied) 'ambiguity must never be resolved by picking one'
    Assert-True ($verdict.Reason -match 'ambiguous_device') 'the reason must name the failure class'
}

Test-Case 'without envctl a mutating scenario is UNAVAILABLE, not silently skipped' {
    $orchestrator = [pscustomobject]@{
        RunId = 'r1'; JournalDirectory = 'x'; EnvctlPath = $null; AliasProfile = $null
        Available = $false; Dirty = $false; DirtyDetail = $null; Recovered = @()
    }
    $verdict = Test-EnvironmentRequirement -Orchestrator $orchestrator -Requirement @{ display = 'display.main-hdr' }
    Assert-True (-not $verdict.Satisfied) 'no tool means no resolvable requirement'
    Assert-True ($verdict.Reason -match 'envctl') 'the reason must say the tool is missing'
}

# ---------------------------------------------------------------------------
# Runner surface
# ---------------------------------------------------------------------------

Test-Case 'prepare refuses to run without an explicit artifact' {
    $runner = Join-Path $scriptRoot 'release-verify.ps1'
    $output = & pwsh -NoProfile -File $runner prepare 2>&1 | Out-String
    Assert-True ($output -match 'ExePath') 'a release campaign must not resolve a default binary'
}

Test-Case 'the catalog loads and every scenario declares what it needs' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $catalog = Get-ReleaseScenarioCatalog
    Assert-True ($catalog.Count -gt 0) 'the catalog must not be empty'
    $ids = @($catalog | ForEach-Object { $_.Id })
    Assert-Equal $ids.Count (@($ids | Select-Object -Unique).Count) 'scenario ids must be unique'
    foreach ($entry in $catalog) {
        foreach ($field in @('Id', 'Title', 'Class', 'Layer', 'Source', 'Run')) {
            Assert-True ($entry.PSObject.Properties.Name -contains $field) "$($entry.Id) is missing $field"
        }
        Assert-True ($entry.Layer -in @('FULL_AUTO', 'EXTERNAL_TOOL', 'CONTROL_CHANNEL', 'UI_AUTOMATION',
                'SEMI_AUTO', 'MANUAL_VISUAL', 'MANUAL_PHYSICAL', 'SECURE')) "$($entry.Id) has an unknown layer '$($entry.Layer)'"
    }
}

Test-Case 'no scenario hardcodes a device friendly name' {
    # The rule the whole catalog rests on: a scenario names an ALIAS, and the alias
    # profile is the only machine-specific file. A model number in a Requires block
    # is a scenario that can only run at one desk.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $catalog = Get-ReleaseScenarioCatalog
    foreach ($entry in $catalog) {
        if ($entry.PSObject.Properties.Name -notcontains 'Requires') { continue }
        foreach ($alias in $entry.Requires.Values) {
            Assert-True ($alias -match '^(display|audio)\.[a-z0-9.-]+$') `
                "$($entry.Id) requires '$alias', which is not an alias of the documented shape"
        }
    }
}

Test-Case 'no Verify block relies on a closure the runner cannot resolve' {
    # A `.GetNewClosure()` script block is bound to a synthetic module that does NOT
    # inherit this script's functions, so such a block can capture a variable and then
    # fail to call Connect-LiveVerify or Save-LiveVerifyEvidence at all -- and only at
    # the moment a human is standing there waiting for the gate to be verified.
    # Comment lines are stripped first: the file EXPLAINS this trap in prose, and a
    # naive match would fail on the explanation rather than on a real call.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    Assert-True ($code -notmatch '\.GetNewClosure\(\)') `
        'a gate must hand values forward in $Gate.State, never in a closure'
    # `\s*` before the newline would swallow it, so the line break is matched by
    # excluding it first: [^\n]* to the end of the opening line, then one newline.
    $pattern = 'Verify\s+=\s+\{[^' + "`n" + ']*' + "`n" + '\s*param\(([^)]*)\)'
    $verifyBlocks = [regex]::Matches($code, $pattern)
    Assert-True ($verifyBlocks.Count -gt 0) 'the catalog is expected to contain Verify blocks'
    foreach ($match in $verifyBlocks) {
        $parameters = $match.Groups[1].Value
        Assert-True ($parameters -match '\$context\s*,\s*\$gate') `
            "a Verify block takes ($parameters); it must take (`$context, `$gate) to read `$gate.State"
    }
}

Test-Case 'every human gate declares why it is manual and how it will be verified' {
    # A gate without a Verify block cannot tell "done" from "typed done", and a gate
    # without a VerifyDescription asks the operator to act on faith.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $source = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') -Raw
    $gateCount = ([regex]::Matches($source, '\$ctx\.HumanGate')).Count
    Assert-True ($gateCount -gt 0) 'the catalog is expected to contain human gates'
    $whyCount = ([regex]::Matches($source, '(?m)^\s+Why\s+=')).Count
    $verifyDescriptionCount = ([regex]::Matches($source, '(?m)^\s+VerifyDescription\s+=')).Count
    $verifyCount = ([regex]::Matches($source, '(?m)^\s+Verify\s+=')).Count
    Assert-Equal $whyCount $verifyDescriptionCount 'every gate that states Why must state VerifyDescription'
    Assert-Equal $whyCount $verifyCount 'every gate must carry a Verify block'
}

. (Join-Path $scriptRoot 'lib/ReleaseArtifactIdentity.ps1')

function New-FakeExeItem {
    # Only the three members the identity helpers read. A real Get-Item would need a
    # real signed, versioned binary; what is under test is the reasoning, not Win32.
    param([Parameter(Mandatory)] [string] $Directory, [string] $ProductVersion = '0.9.0-dev')
    return [pscustomobject]@{
        Directory     = (Get-Item -LiteralPath $Directory)
        DirectoryName = $Directory
        VersionInfo   = [pscustomobject]@{ ProductVersion = $ProductVersion }
    }
}

Test-Case 'the artifact commit comes from the build manifest, and only when it describes these bytes' {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    $staging = Join-Path $root 'staging/ExoSnap-0.9.0-dev-windows-x64-portable'
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    try {
        $manifest = Join-Path $root 'artifact-manifest.json'
        # No manifest anywhere: unknown provenance, and it says so.
        Assert-True ($null -eq (Get-ReleaseArtifactSourceCommit -ExeItem (New-FakeExeItem -Directory $staging))) `
            'without a manifest the source commit must be unknown, not guessed'

        # Found two levels up, as the release build actually lays it out.
        @{ version = '0.9.0-dev'; sourceCommit = 'c9384511' } | ConvertTo-Json |
            Set-Content -LiteralPath $manifest -Encoding utf8
        Assert-Equal 'c9384511' (Get-ReleaseArtifactSourceCommit -ExeItem (New-FakeExeItem -Directory $staging)) `
            'the commit must be read from the manifest beside the staging tree'

        # A manifest describing a DIFFERENT build is not evidence about these bytes.
        @{ version = '0.8.1'; sourceCommit = 'deadbeef' } | ConvertTo-Json |
            Set-Content -LiteralPath $manifest -Encoding utf8
        Assert-True ($null -eq (Get-ReleaseArtifactSourceCommit -ExeItem (New-FakeExeItem -Directory $staging))) `
            "a manifest for another version must not lend its commit to this artifact"

        # Unparseable paperwork is unknown provenance, never a runner failure.
        Set-Content -LiteralPath $manifest -Value '{ this is not json' -Encoding utf8
        Assert-True ($null -eq (Get-ReleaseArtifactSourceCommit -ExeItem (New-FakeExeItem -Directory $staging))) `
            'a corrupt manifest must read as unknown, not throw'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the shipped Qt runtime is read from the package, not from the machine' {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    try {
        $item = New-FakeExeItem -Directory $root
        Assert-True ($null -eq (Get-ReleaseArtifactQtRuntimeVersion -ExeItem $item)) `
            'a package with no Qt6Core.dll must report no Qt runtime, not the developer machine''s'

        # Any versioned binary proves the reading; a real Qt DLL would only add a
        # dependency on which Qt this machine has installed -- the opposite of the
        # point. Taken from the environment, never a hardcoded system path.
        #
        # The expectation is read from the COPY, not from the donor. Windows
        # redirects version-resource queries for paths under System32 to the
        # servicing state, so a byte-identical copy outside System32 reports the
        # version that is actually in its resource while the original reports the
        # serviced one -- verified as a hash-identical pair whose FileVersion
        # strings differ. Comparing against the donor asserts a Windows quirk, not
        # this function's behaviour.
        $donor = Join-Path $env:SystemRoot 'System32/kernel32.dll'
        $packaged = Join-Path $root 'Qt6Core.dll'
        Copy-Item -LiteralPath $donor -Destination $packaged
        $expected = (Get-Item -LiteralPath $packaged).VersionInfo.FileVersion
        Assert-True (-not [string]::IsNullOrWhiteSpace($expected)) `
            'the donor binary must carry a version resource for this case to prove anything'
        Assert-Equal $expected (Get-ReleaseArtifactQtRuntimeVersion -ExeItem $item) `
            'the Qt runtime version must come from the DLL beside the executable'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a scenario that launches its own instance ends the shared session first' {
    # The single-instance guard is a machine-wide mutex, so a second exosnap.exe hands
    # focus to the running one and exits 0 without ever opening its Live Verify server.
    # The scenario that launched it then waits for a pipe nobody created and fails on a
    # connect timeout -- a verdict about the runner, not about the product. This is the
    # defect REL-UPD-PORTABLE-001 reported as "the update handoff script exited 1".
    #
    # Checked as source text, per scenario block, with comment lines stripped: the file
    # EXPLAINS the hazard in prose, and a naive match would read the explanation as the
    # call it demands.
    $lines = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' })
    $currentId = '<before the first scenario>'
    $endedSession = $false
    $launches = 0
    foreach ($line in $lines) {
        if ($line -match "^\s+Id\s+=\s+'([^']+)'") {
            $currentId = $Matches[1]
            $endedSession = $false
            continue
        }
        if ($line -match '\$ctx\.EndSession') { $endedSession = $true; continue }
        # Both launch shapes: the scenario starting the binary itself, and the one
        # delegating to a helper script that starts it.
        if ($line -match '\$ctx\.Artifact\.exePath' -and
            ($line -match 'Start-Process' -or $line -match '-AppPath')) {
            $launches++
            Assert-True $endedSession `
                "$currentId launches its own instance without calling & `$ctx.EndSession first"
        }
    }
    Assert-True ($launches -gt 0) 'the catalog is expected to contain scenarios that launch their own instance'
}

Test-Case 'the field contract names only scenarios that exist' {
    # The contract's value is that a vanished field path names, without searching,
    # exactly which gates are about to throw. A UsedBy pointing at a scenario that no
    # longer exists is the contract rotting quietly.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $contract = @(Get-ReleaseFieldContract)
    Assert-True ($contract.Count -gt 0) 'the field contract must not be empty'
    $ids = @((Get-ReleaseScenarioCatalog) | ForEach-Object { $_.Id })
    foreach ($entry in $contract) {
        Assert-True ($entry.Stage -in @('idle', 'recording', 'result')) `
            "$($entry.Command).$($entry.Path) declares an unknown stage '$($entry.Stage)'"
        foreach ($id in ($entry.UsedBy -split ',\s*')) {
            Assert-True ($ids -contains $id) "$($entry.Command).$($entry.Path) is used by '$id', which is not a scenario"
        }
    }
}

Test-Case 'the field-path resolver tells missing apart from empty' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $snapshot = '{"audio":{"sourceDegraded":false},"entries":[],"screens":[{"name":"X"}]}' | ConvertFrom-Json
    Assert-Equal 'present' (Resolve-ReleaseFieldPath -Root $snapshot -Path 'audio.sourceDegraded').Status `
        'an emitted path is present even when its value is false'
    Assert-Equal 'present' (Resolve-ReleaseFieldPath -Root $snapshot -Path 'screens[].name').Status `
        'an element shape is walked through the first element'
    # The exact defect: pipeline.audio.tracks[].degraded, read by two scenarios,
    # emitted by nothing. Under StrictMode it threw INSIDE a human gate.
    Assert-Equal 'missing' (Resolve-ReleaseFieldPath -Root $snapshot -Path 'audio.tracks[].degraded').Status `
        'a path no emitter emits must be reported missing, not thrown on'
    # Empty is its own answer: the NAME is proven, the element shape is not, and
    # calling that a pass would be a false statement about what was checked.
    Assert-Equal 'empty' (Resolve-ReleaseFieldPath -Root $snapshot -Path 'entries[].sequence').Status `
        'an empty collection proves its name and nothing about its elements'
}

Test-Case 'no scenario reads a field the contract does not cover' {
    # The contract only protects what it lists. This keeps the two in step for the
    # paths that actually broke, so a rename cannot quietly reintroduce one of them.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    foreach ($dead in @('\.audio\.tracks', '\$pipeline\.avDriftMs', '\$pipeline\.capture\.presentMode',
            '\$notifications\.notifications', '\$after\.notifications', '\$identity\.version\b')) {
        Assert-True ($code -notmatch $dead) "the catalog still reads '$dead', which no emitter emits"
    }
    Assert-True ($code -notmatch "moveToScreen'\s+-Parameters\s+@\{\s*index") `
        'window.moveToScreen takes a screen NAME, never an index'
}

Test-Case 'a momentarily unrendered publish is not a frozen preview' {
    # The rc11 defect in the gate itself: `owed` is
    # published_generation > presented_generation, which a live preview crosses
    # between every publish and the render pass that follows it. One sample of it
    # therefore fails a healthy preview at whatever rate that window occupies, and
    # passes a preview that has published nothing at all.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $live = @(
        [pscustomobject]@{ consumedFrames = 429; updateGate = [pscustomobject]@{ owed = $true; renderPasses = 502 } }
        [pscustomobject]@{ consumedFrames = 441; updateGate = [pscustomobject]@{ owed = $true; renderPasses = 515 } }
    )
    Assert-Equal 'PASS' (Get-PreviewFreezeVerdict -Samples $live).Result `
        'frames kept being consumed, so the preview is not frozen'
}

Test-Case 'a preview that stops consuming while a publish is owed is a FAIL' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $frozen = @(
        [pscustomobject]@{ consumedFrames = 429; updateGate = [pscustomobject]@{ owed = $true; renderPasses = 502 } }
        [pscustomobject]@{ consumedFrames = 429; updateGate = [pscustomobject]@{ owed = $true; renderPasses = 502 } }
        [pscustomobject]@{ consumedFrames = 429; updateGate = [pscustomobject]@{ owed = $true; renderPasses = 502 } }
    )
    Assert-Equal 'FAIL' (Get-PreviewFreezeVerdict -Samples $frozen).Result `
        'a standing debt with no progress is the freeze this gate exists for'
}

Test-Case 'an idle preview is not reported as a pass' {
    # The other half of the same defect. A preview that published nothing can never
    # owe a render pass, so a bare `-not owed` reads as green for a gate nobody ran.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $idle = @(
        [pscustomobject]@{ consumedFrames = 0; updateGate = [pscustomobject]@{ owed = $false; renderPasses = 9 } }
        [pscustomobject]@{ consumedFrames = 0; updateGate = [pscustomobject]@{ owed = $false; renderPasses = 11 } }
    )
    Assert-Equal 'UNVERIFIED' (Get-PreviewFreezeVerdict -Samples $idle).Result `
        'no frame was consumed, so nothing was proven about presenting one'
}

Test-Case 'one sample cannot decide whether a preview is frozen' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $single = @([pscustomobject]@{ consumedFrames = 429; updateGate = [pscustomobject]@{ owed = $true; renderPasses = 502 } })
    Assert-Equal 'UNVERIFIED' (Get-PreviewFreezeVerdict -Samples $single).Result `
        'a transient boolean needs at least two observations'
}

Test-Case 'the mixed-display scenario decides through the sampled verdict' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $source = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') -Raw
    Assert-True ($source -match 'Get-PreviewFreezeVerdict -Samples') `
        'REL-DISP-MIXED-001 must decide on a series of samples, never on one'
}

function New-HealthySoakReport {
    # The shape session.latest returns, with every post-check of
    # docs/release-checklist.md section 7 satisfied.
    return [pscustomobject]@{
        segments = @([pscustomobject]@{ index = 0; duration_seconds = 1802.278; finalized = $true })
        audio = [pscustomobject]@{
            degraded_occurred = $false
            resampler_drain = @(
                [pscustomobject]@{ track = 0; undrained_frames = 0 }
                [pscustomobject]@{ track = 1; undrained_frames = 0 })
        }
        counters = [pscustomobject]@{
            audio_discontinuities = 0
            audio_discontinuity_ms_total = 0
            audio_discontinuity_ms_longest = 0
            mux_failures = 0
            encoder_keyframe_prediction_mismatches = 0
            av_drift_ms = 12.5
            peak_av_drift_ms = 31.0
            duration_skew_ms = 320.5
            frames_dropped = [pscustomobject]@{ processing_failure = 0; backpressure = 0; coalesced = 66021; cfr = 0 }
        }
    }
}

Test-Case 'a clean soak report passes' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseSoakVerdict -Report (New-HealthySoakReport) -ContainerSeconds 1802.278 `
        -ExpectedSeconds 1800 -AudioSpanSeconds @(1802.254, 1802.030)
    Assert-Equal 'PASS' $verdict.Result "a clean report must pass: $($verdict.Message)"
}

Test-Case 'the soak gate reads the post-checks the checklist names' {
    # Measured against rc11: the scenario collected 60 drift samples and a session
    # report carrying audio_discontinuities = 2, and returned PASS on container
    # duration alone. Every counter below was already in the evidence, unread.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $cases = @(
        @{ Name = 'audio gap budget'; Apply = { param($r) $r.counters.audio_discontinuity_ms_total = 2500 } }
        @{ Name = 'audible single dropout'; Apply = { param($r) $r.counters.audio_discontinuity_ms_longest = 240 } }
        @{ Name = 'gap counters absent'; Apply = { param($r)
                $r.counters.PSObject.Properties.Remove('audio_discontinuity_ms_total') } }
        @{ Name = 'mux_failures'; Apply = { param($r) $r.counters.mux_failures = 1 } }
        @{ Name = 'processing_failure'; Apply = { param($r) $r.counters.frames_dropped.processing_failure = 3 } }
        @{ Name = 'backpressure'; Apply = { param($r) $r.counters.frames_dropped.backpressure = 4 } }
        @{ Name = 'keyframe'; Apply = { param($r) $r.counters.encoder_keyframe_prediction_mismatches = 1 } }
        @{ Name = 'undrained'; Apply = { param($r) $r.audio.resampler_drain[1].undrained_frames = 5 } }
        @{ Name = 'degraded'; Apply = { param($r) $r.audio.degraded_occurred = $true } }
        @{ Name = 'finalized'; Apply = { param($r) $r.segments[0].finalized = $false } }
    )
    foreach ($case in $cases) {
        $report = New-HealthySoakReport
        & $case.Apply $report
        $verdict = Get-ReleaseSoakVerdict -Report $report -ContainerSeconds 1802.278 `
            -ExpectedSeconds 1800 -AudioSpanSeconds @(1802.254, 1802.030)
        Assert-Equal 'FAIL' $verdict.Result "$($case.Name) must fail the gate"
    }
}

Test-Case 'outages within the budget pass, however many there were' {
    # The deliberate loosening: a machine under load misses capture buffers, and
    # every miss is answered with exactly as much silence. Judging the count
    # would fail the product for handling load correctly, so the criterion is the
    # time lost. Measured on this desk: five minutes under heavy file I/O gave 23
    # outages totalling 566 ms, longest 48 ms -- inaudible, and the track stayed
    # aligned with video.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $report = New-HealthySoakReport
    $report.counters.audio_discontinuities = 138
    $report.counters.audio_discontinuity_ms_total = 1700
    $report.counters.audio_discontinuity_ms_longest = 48
    $verdict = Get-ReleaseSoakVerdict -Report $report -ContainerSeconds 1802.278 `
        -ExpectedSeconds 1800 -AudioSpanSeconds @(1802.254, 1802.030)
    Assert-Equal 'PASS' $verdict.Result "outages under the budget must pass: $($verdict.Message)"
}

Test-Case 'an audio track that stops early fails even with a full container' {
    # The failure a container-duration check cannot see: video runs the whole
    # recording, the audio drain dropped the tail.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseSoakVerdict -Report (New-HealthySoakReport) -ContainerSeconds 1802.278 `
        -ExpectedSeconds 1800 -AudioSpanSeconds @(1802.254, 900.0)
    Assert-Equal 'FAIL' $verdict.Result 'a half-length audio track must fail'
}

Test-Case 'a container much shorter than the recording fails' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseSoakVerdict -Report (New-HealthySoakReport) -ContainerSeconds 1200 `
        -ExpectedSeconds 1800 -AudioSpanSeconds @(1200, 1200)
    Assert-Equal 'FAIL' $verdict.Result 'a 10-minute-short container must fail'
}

Test-Case 'the envelope session.latest actually returns is unwrapped' {
    # session.latest answers { available, report }, not the report itself. Reading
    # the counters off the envelope finds nothing and reports UNVERIFIED, which
    # costs a 30-minute run to discover.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $envelope = [pscustomobject]@{ available = $true; report = (New-HealthySoakReport) }
    $verdict = Get-ReleaseSoakVerdict -Report $envelope -ContainerSeconds 1802.278 `
        -ExpectedSeconds 1800 -AudioSpanSeconds @(1802.254, 1802.030)
    Assert-Equal 'PASS' $verdict.Result "the envelope must be unwrapped: $($verdict.Message)"
}

Test-Case 'a missing session report is unverified, not a pass' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseSoakVerdict -Report $null -ContainerSeconds 1802.278 `
        -ExpectedSeconds 1800 -AudioSpanSeconds @(1802.2)
    Assert-Equal 'UNVERIFIED' $verdict.Result 'no report means the post-checks were not performed'
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
