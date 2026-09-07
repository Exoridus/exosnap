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
Import-Module (Join-Path $scriptRoot 'lib/LiveVerifyClient.psm1') -Force -DisableNameChecking

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

function Get-ReleaseVerifyFunctionText {
    <#
    .SYNOPSIS
        Extracts ONE named function's source out of release-verify.ps1.
    .DESCRIPTION
        release-verify.ps1 is a full script, with a mandatory-artifact param block
        and a command switch that runs on every invocation -- dot-sourcing the
        whole file would launch whatever `run`/`status`/`report` defaults to.
        Every top-level function in it closes on its own unindented `}`, so a
        single function is lifted out by that line alone.

        Returned as TEXT rather than dot-sourced here: dot-sourcing inside this
        function would define it in this function's own scope, which disappears
        the moment it returns. The caller must dot-source the result itself, in
        the Test-Case body that needs it, exactly as the other tests in this file
        dot-source ReleaseScenarios.ps1 directly rather than through a helper.
    #>
    param([Parameter(Mandatory)] [string] $Name)
    $source = Get-Content -LiteralPath (Join-Path $scriptRoot 'release-verify.ps1') -Raw
    $match = [regex]::Match($source, "(?ms)^function $Name \{.*?^\}")
    Assert-True $match.Success "function $Name was not found in release-verify.ps1"
    return $match.Value
}

Test-Case 'a leftover recovery manifest is moved into the campaign, not deleted' {
    # The exact defect: a manifest from a previous campaign (or a real crash) sits
    # under %LOCALAPPDATA%\ExoSnap and opens the recovery surface at the very next
    # launch, before any scenario or cleanup pass gets a chance to run.
    function Write-Step { param($Text) }
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Backup-ReleaseRecoveryManifest')))

    $fakeLocalAppData = New-TestDirectory
    $campaignDirectory = New-TestDirectory
    $previousLocalAppData = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $fakeLocalAppData
        $manifestDir = Join-Path $fakeLocalAppData 'ExoSnap'
        New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null
        $manifestPath = Join-Path $manifestDir 'recovery-manifest.json'
        Set-Content -LiteralPath $manifestPath -Value '{"entries":["leftover"]}' -Encoding utf8NoBOM

        Backup-ReleaseRecoveryManifest -CampaignDirectory $campaignDirectory

        Assert-True (-not (Test-Path -LiteralPath $manifestPath)) `
            'the manifest must not be left where it can open the recovery surface again'
        $backupPath = Join-Path $campaignDirectory 'leftover-recovery-manifest.json'
        Assert-True (Test-Path -LiteralPath $backupPath) 'the manifest must survive inside the campaign directory'
        Assert-True ((Get-Content -LiteralPath $backupPath -Raw) -match 'leftover') 'the manifest content must be preserved, not rewritten'
    }
    finally {
        $env:LOCALAPPDATA = $previousLocalAppData
        Remove-Item -LiteralPath $fakeLocalAppData -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $campaignDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'no manifest is a silent no-op, not an error' {
    function Write-Step { param($Text) }
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Backup-ReleaseRecoveryManifest')))

    $fakeLocalAppData = New-TestDirectory
    $campaignDirectory = New-TestDirectory
    $previousLocalAppData = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $fakeLocalAppData
        Backup-ReleaseRecoveryManifest -CampaignDirectory $campaignDirectory
        Assert-True $true 'reaching here means no manifest did not throw'
    }
    finally {
        $env:LOCALAPPDATA = $previousLocalAppData
        Remove-Item -LiteralPath $fakeLocalAppData -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $campaignDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'prepare moves a leftover manifest aside before anything else runs, and every session start dismisses a fresh one' {
    # Structural: `prepare` runs once per campaign and a fresh recovery surface can
    # only be answered once a control-channel connection exists, so the two halves
    # of this fix live in different places and each is checked where it lives.
    $source = Get-Content -LiteralPath (Join-Path $scriptRoot 'release-verify.ps1') -Raw
    $prepareBlock = [regex]::Match($source, "(?ms)'prepare' \{.*?\n    \}\r?\n").Value
    Assert-True ($prepareBlock -match 'Backup-ReleaseRecoveryManifest') `
        'prepare must move a leftover recovery manifest aside before the campaign launches anything'
    $startSessionBlock = [regex]::Match($source, '(?ms)^function Start-ReleaseSession \{.*?^\}').Value
    Assert-True ($startSessionBlock -match 'Clear-ReleaseBlockingSurface') `
        'every scenario session start must dismiss a blocking surface before the scenario sends its first command'
}

Test-Case 'a verdict with omitted fields survives a strict-mode read' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')

    # What a Verify block is allowed to return: Ok and a reason, nothing else.
    # Reading .Evidence off that used to THROW under Set-StrictMode, and the
    # scenario then reported a PowerShell message instead of a verdict -- three UAC
    # prompts in one campaign were answered correctly and thrown away that way.
    $sparse = Resolve-ReleaseVerdict @{ Ok = $false; Detail = 'the updater refused' }
    Assert-Equal $false $sparse.Ok 'Ok survives'
    Assert-Equal 'the updater refused' $sparse.Detail 'Detail survives'
    Assert-Equal 0 @($sparse.Evidence).Count 'Evidence is an empty list rather than absent'

    # And the sparsest one a block can produce.
    $bare = Resolve-ReleaseVerdict @{ Ok = $true }
    Assert-Equal '' $bare.Detail 'Detail defaults to empty'
    Assert-Equal 0 @($bare.Evidence).Count 'Evidence defaults to empty'

    # A verdict that carries everything is handed back untouched.
    $full = Resolve-ReleaseVerdict @{ Ok = $true; Detail = 'measured'; Evidence = @('a', 'b') }
    Assert-Equal 2 @($full.Evidence).Count 'existing evidence is kept'

    # $null stays $null: "the block returned nothing" is a different finding from
    # "it returned a verdict with no detail", and callers report it as UNVERIFIED.
    Assert-True ($null -eq (Resolve-ReleaseVerdict $null)) 'a null verdict is not invented into one'
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

# A connection whose Request() answers whatever the test puts in $script:FakeReply,
# so a scenario's Run block can be exercised through the real Invoke-LiveVerifyCommand
# without a pipe or an application. Matches the fixture in
# live-verify-response-shape.tests.ps1.
function New-FakeLiveVerifyConnection {
    $connection = [pscustomobject]@{}
    Add-Member -InputObject $connection -MemberType ScriptMethod -Name Request -Value {
        param($command, $parameters, $timeoutMs)
        return $script:FakeReply
    }
    return $connection
}

Test-Case 'REL-CAP-FSE-001 reports UNAVAILABLE, not FAIL, when its own precondition is unmet' {
    # Observed in a dry run: with present diagnostics not opted in, this gate ran
    # its human-gate machinery anyway and reported FAIL with "present diagnostics
    # are unavailable (requiresOptIn); run REL-PRESENT-002 first" -- rule 3 names
    # this exactly: an unmet requirement is not a failure.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $entry = (Get-ReleaseScenarioCatalog) | Where-Object { $_.Id -eq 'REL-CAP-FSE-001' }
    Assert-True ($null -ne $entry) 'REL-CAP-FSE-001 must exist in the catalog'

    $script:FakeReply = [pscustomobject]@{
        id     = 1
        ok     = $true
        result = [pscustomobject]@{
            present = [pscustomobject]@{ available = $false; availability = 'requiresOptIn' }
        }
    }
    $fakeSession = [pscustomobject]@{ Connection = (New-FakeLiveVerifyConnection) }
    $ctx = [pscustomobject]@{ EnsureSession = { $fakeSession } }

    $outcome = & $entry.Run $ctx
    Assert-Equal 'UNAVAILABLE' $outcome.Result 'an unmet precondition must never reach FAIL'
    Assert-True ($outcome.Message -match 'REL-PRESENT-002') `
        'the message must point at the gate that establishes the precondition'
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

Test-Case 'a scenario reads snapshot fields through the safe accessor, never with $obj.field' {
    # Set-StrictMode is on for the runner, so `$state.phase` on an object without a
    # `phase` property THROWS. Inside a scenario that throw becomes "Scenario setup
    # failed: The property 'phase' cannot be found on this object" -- a PowerShell
    # message reported as a product FAIL. It cost a real campaign an UNAVAILABLE that
    # was reported as a failure, and left the scenario's own child process running.
    # Get-ReleaseSnapshotValue answers $null for an absent path instead.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    Assert-True ($code -notmatch '\$\(\$state\.phase\)') `
        'read updater/state fields with Get-ReleaseSnapshotValue: $($state.phase) throws under StrictMode'
    Assert-True ($code -notmatch '\$\(\$state\.installState\)') `
        'read updater/state fields with Get-ReleaseSnapshotValue: $($state.installState) throws under StrictMode'
}

Test-Case 'a sight-check judgement defers instead of asking when nobody can answer' {
    # Read-Host asked anyway under -NonInteractive, and under a redirected stdin it
    # got an empty string forever and re-asked -- an infinite loop, with the
    # scenario's Windows-appearance change still applied. The operator reader is
    # replaced here so a real console read can never be reached: if the rules let
    # the call through, the test fails loudly instead of hanging the suite.
    function Write-Step { param($Text) $script:LastStep = $Text }
    function Expand-ListArgument { param($Values) return @($Values) }
    # No simulated reader is installed on purpose: an installed one IS somebody to
    # ask (see Test-ReleaseOperatorPresent), and what this pins is the case where
    # there is nobody. A real console read is therefore unreachable only if the
    # rules hold, which is the assertion.
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Read-ReleaseOperatorJudgement')))
    try {
        $NonInteractive = $true
        $Attest = @()
        Assert-Equal 'skip' (Read-ReleaseOperatorJudgement -ScenarioId 'REL-VIS-OVERLAY-001' -Line 'looks right?').Answer `
            '-NonInteractive must defer the judgement, not ask it'

        $NonInteractive = $false
        $Attest = @('REL-VIS-OVERLAY-001')
        # `skip`, never an answer: -Attest says the CALLER performed an action and
        # leaves the verdict to a Verify block. Here the question IS the verdict --
        # whether something LOOKS right -- and attesting it would manufacture a
        # pass for a surface nobody saw.
        Assert-Equal 'skip' (Read-ReleaseOperatorJudgement -ScenarioId 'REL-VIS-OVERLAY-001' -Line 'looks right?').Answer `
            'an attested visual judgement is DEFERRED, never a pass'

        # This suite runs with a redirected stdin, so EVERY path defers here and the
        # return value alone cannot say which rule fired. The reason it reports can:
        # attesting some OTHER scenario must not consume this one's attest branch.
        $Attest = @('REL-SOMETHING-ELSE-001')
        Assert-Equal 'skip' (Read-ReleaseOperatorJudgement -ScenarioId 'REL-VIS-OVERLAY-001' -Line 'looks right?').Answer `
            'a redirected stdin defers rather than asking'
        Assert-True ($script:LastStep -match 'no interactive terminal') `
            "attesting a DIFFERENT scenario must not claim this one was attested: $script:LastStep"
    }
    finally { Set-ReleaseOperatorReader $null }
}


Test-Case 'an instruction step is one array element, not a continued string' {
    # Inside a multi-line @( ) literal, an element ending in `+` does NOT concatenate
    # with the next line: PowerShell ends the element at the newline and the
    # continuation becomes an element of its own. One instruction then prints as two
    # numbered steps, the first of them half a sentence. REL-PRESENT-002 shipped that
    # way -- its four instructions printed as five, with step 2 ending at "when the
    # shell " and step 3 starting at "is elevated".
    $lines = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $inDo = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if (-not $inDo) {
            # Only a Do array that spans lines can carry the trap; a single-line
            # one closes on the same line it opens.
            if ($line -match 'Do\s+=\s+@\($' ) { $inDo = $true }
            continue
        }
        if ($line.Trim() -eq ')') { $inDo = $false; continue }
        Assert-True (-not $line.TrimEnd().EndsWith('+')) `
            "line $($i + 1) continues a Do step with '+'; that splits it into two numbered instructions"
    }
}


Test-Case 'the judgement seam is reached with the scenario id' {
    # Read-ReleaseOperatorJudgement needs the id to tell whether -Attest names THIS
    # scenario; a call that passes only the question silently loses that check.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    $asks = [regex]::Matches($code, '\$ctx\.Judge\s+(\S+)')
    Assert-True ($asks.Count -gt 0) 'the catalog is expected to ask at least one sight-check judgement'
    foreach ($ask in $asks) {
        Assert-True ($ask.Groups[1].Value -match "^'REL-") `
            "`$ctx.Judge must be called with the scenario id first, got $($ask.Groups[1].Value)"
    }
}


# ---------------------------------------------------------------------------
# A declined elevation prompt (REL-UPD-MSI-DECLINE-001)
# ---------------------------------------------------------------------------

Test-Case 'a declined elevation prompt is judged on failureCase and installState, never on phase' {
    # The gate asserted `phase -notmatch 'fail|error'`, which cannot carry the
    # answer: measured against the real updater endpoint, a declined prompt reports
    # phase `failed` TOGETHER WITH failureCase uacDeclined, installState intact and
    # a retry entry step. The gate therefore failed the product for behaving
    # exactly as its model says it should.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')

    $declined = '{"failureCase":"uacDeclined","installState":"intact","phase":"failed","retryEntryStep":"install"}' |
        ConvertFrom-Json
    $verdict = Get-ReleaseDeclinedUpdateVerdict -State $declined
    Assert-True $verdict.Ok "a real decline must pass: $($verdict.Detail)"
    Assert-True ($verdict.Detail -match 'uacDeclined') 'the detail must name the classification it accepted'
    Assert-True ($verdict.Detail -match "phase='failed'") 'phase belongs in the record, reported not asserted'
}

Test-Case 'an install stranded in backup fails a declined-prompt gate' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $stranded = '{"installState":"strandedInBackup"}' | ConvertFrom-Json
    $verdict = Get-ReleaseDeclinedUpdateVerdict -State $stranded
    Assert-True (-not $verdict.Ok) 'a stranded install is never a pass'
    Assert-True ($verdict.Detail -match 'stranded') 'the detail must name what was found'
}

Test-Case 'a different failure case is not a declined prompt' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $other = '{"failureCase":"msiFailed","installState":"intact"}' | ConvertFrom-Json
    $verdict = Get-ReleaseDeclinedUpdateVerdict -State $other
    Assert-True (-not $verdict.Ok) 'an msiFailed run is a different event and must not pass this gate'
    Assert-True ($verdict.Detail -match 'msiFailed') 'the detail must name the classification it saw'
}

Test-Case 'a decline the updater classified as nothing at all is a failure that says what it saw' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $unclassified = '{"failureCase":null,"phase":"idle"}' | ConvertFrom-Json
    $verdict = Get-ReleaseDeclinedUpdateVerdict -State $unclassified
    Assert-True (-not $verdict.Ok) 'a missing failureCase proves nothing about the decline'
    Assert-True ($verdict.Detail -match "phase='idle'") "the message must name what was seen: $($verdict.Detail)"
}

Test-Case 'no gate decides a declined prompt from the updater phase' {
    # The rule that shipped was `$phase -match 'fail|error'`. `phase` describes how
    # far the updater got, not why it stopped, and the two are independent: the
    # declined case is phase `failed` with failureCase uacDeclined. Pinned as a
    # source rule as well as a verdict, because the tempting fix is to add
    # `-and $phase -ne 'failed'` somewhere rather than to stop reading it.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    Assert-True ($code -notmatch '\$phase -match') `
        'the update gates must not branch on phase; failureCase and installState carry the answer'
}

# ---------------------------------------------------------------------------
# A gate's connection comes from its context (REL-VIS-NOTIFY-001 and five more)
# ---------------------------------------------------------------------------

Test-Case 'no Verify block reaches into the runner script scope for its session' {
    # A Verify block runs after the operator answers, which can be minutes after
    # the scenario body prepared the state -- and the session is not the gate's to
    # own. `$script:Session` starts as $null and every scenario that calls
    # $ctx.EndSession puts it back there, so `$script:Session.Connection` throws
    # "The property 'Connection' cannot be found on this object" under StrictMode
    # and the gate reports a PowerShell message instead of a verdict.
    #
    # Comment lines are stripped: the file explains the trap in prose.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    Assert-True ($code -notmatch '\$script:Session') `
        'a scenario obtains its connection through $ctx/$context, never from the runner''s own $script:Session'
}

Test-Case 'a Verify block returns a verdict when the session is gone, instead of throwing' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')

    # The shape that used to be written, against the exact state the runner is in
    # after any scenario ended the shared session.
    $script:Session = $null
    $oldShape = { param($context, $gate) return @{ Ok = $true; Detail = "$($script:Session.Connection)" } }
    Assert-Throws { & $oldShape $null $null } `
        'the old shape must throw, or this test proves nothing about the new one'

    # The new shape: a session that cannot be re-established is a verdict.
    $dead = [pscustomobject]@{ EnsureSession = { throw 'the application could not be launched' } }
    $link = Get-ReleaseGateConnection -Context $dead
    Assert-True (-not $link.Ok) 'an unreachable session must be reported, not thrown'
    Assert-True ($link.Detail -match 'could not be re-established') `
        "the detail must say what happened: $($link.Detail)"

    # A live session is handed straight through.
    $session = [pscustomobject]@{ Connection = 'the-connection' }
    $live = Get-ReleaseGateConnection -Context ([pscustomobject]@{ EnsureSession = { $session } })
    Assert-True $live.Ok 'a live session must be usable'
    Assert-Equal 'the-connection' $live.Connection 'the connection must be the session''s own'

    # A context that carries no seam at all is a verdict too, not a property throw.
    $none = Get-ReleaseGateConnection -Context ([pscustomobject]@{ Artifact = 'x' })
    Assert-True (-not $none.Ok) 'a context with no EnsureSession must be reported'

    # And a session object that exists but carries no connection.
    $empty = Get-ReleaseGateConnection -Context ([pscustomobject]@{ EnsureSession = { [pscustomobject]@{ } } })
    Assert-True (-not $empty.Ok) 'a session with no connection is not a usable one'
}

Test-Case 'a session that was RELAUNCHED under a gate is UNVERIFIED, not a product failure' {
    # EnsureSession does not only reconnect, it LAUNCHES a fresh application when
    # the process is gone -- and a fresh one has an empty notification hub, no
    # overlays on screen and no recording running. A gate that asserted over that
    # would report a product FAIL for a loss on the runner's side.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $session = [pscustomobject]@{ Connection = 'the-connection'; RunId = 'run-two' }
    $ctx = [pscustomobject]@{ EnsureSession = { $session } }

    $same = Get-ReleaseGateConnection -Context $ctx -ExpectedSessionId 'run-two'
    Assert-True $same.Ok 'the same session is usable'
    Assert-True (-not $same.Relaunched) 'a reused session is not a relaunch'

    $other = Get-ReleaseGateConnection -Context $ctx -ExpectedSessionId 'run-one'
    Assert-True $other.Ok 'the connection is still usable; what changed is what it is connected to'
    Assert-True $other.Relaunched 'a different run id is a different application process'

    # A gate that never recorded which session it prepared cannot detect a
    # relaunch, and must not claim one.
    $unknown = Get-ReleaseGateConnection -Context $ctx
    Assert-True (-not $unknown.Relaunched) 'no expectation means no relaunch claim'

    $verdict = Get-ReleaseLostSessionVerdict -Subject 'the notification hub the operator judged'
    Assert-Equal 'UNVERIFIED' $verdict.Result 'a lost session is not a product defect'
    Assert-True (-not $verdict.Ok) 'it is not a pass either'
}

Test-Case 'every gate whose subject is the running process records the session it prepared' {
    # The relaunch check is only possible where the scenario body wrote down which
    # session it saw. A gate that reads the previous process's state without doing
    # so silently loses the guard.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    $uses = ([regex]::Matches($code, 'Get-ReleaseGateConnection -Context \$context')).Count
    Assert-True ($uses -gt 0) 'the catalog is expected to obtain connections through the context'
    Assert-Equal $uses ([regex]::Matches($code, "ExpectedSessionId `"\`$\(Get-ReleaseGateStateValue")).Count `
        'every context connection in a Verify block must declare the session it expects'
    # `(?<!\$)` so the helper's own local $sessionId is not counted as a gate.
    Assert-Equal $uses ([regex]::Matches($code,
            '(?<!\$)sessionId\s+= "\$\(Get-ReleaseSnapshotValue -Object \$session -Path ''RunId''\)"')).Count `
        'and every such gate must record that session id in its State'
}

Test-Case 'a Verify block can name UNVERIFIED and the human gate reports it as UNVERIFIED' {
    # Ok = $false alone collapses "nothing was measured" into "the product is
    # broken". Rules 1 and 3 of the runner exist to keep those apart, and a gate
    # whose elevated worker never started has found no defect.
    function Write-Step { param($Text) $script:LastStep = $Text }
    function Expand-ListArgument { param($Values) return @($Values) }
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    Set-ReleaseOperatorReader { param($Prompt) throw 'the terminal must not be asked here' }
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Get-ReleaseGateLine')))
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'New-ReleaseOperatorStep')))
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Invoke-ReleaseHumanGate')))

    $NonInteractive = $false
    $Attest = @('T-SEAM-001')
    $gate = @{
        Id                = 'T-SEAM-001'
        Title             = 'a gate whose worker never ran'
        Why               = 'test'
        Do                = @('nothing')
        Expected          = 'nothing'
        VerifyDescription = 'test'
        Verify            = { param($context, $gate)
            return @{ Ok = $false; Result = 'UNVERIFIED'; Detail = 'the elevated worker could not be started' }
        }
    }
    $outcome = Invoke-ReleaseHumanGate -Gate $gate -Context ([pscustomobject]@{ })
    Assert-Equal 'UNVERIFIED' $outcome.Result "a named UNVERIFIED must survive the seam: $($outcome.Message)"
    Assert-True ($outcome.Message -match 'worker could not be started') 'and carry its own reason'

    # PASS and FAIL still travel through Ok, so a block that names nothing is
    # unaffected by the new branch.
    $gate.Verify = { param($context, $gate) return @{ Ok = $false; Detail = 'the product did the wrong thing' } }
    Assert-Equal 'FAIL' (Invoke-ReleaseHumanGate -Gate $gate -Context ([pscustomobject]@{ })).Result `
        'an unnamed false verdict is still a FAIL'
    $gate.Verify = { param($context, $gate) return @{ Ok = $true; Detail = 'measured' } }
    Assert-Equal 'PASS' (Invoke-ReleaseHumanGate -Gate $gate -Context ([pscustomobject]@{ })).Result `
        'an unnamed true verdict is still a PASS'
}

# ---------------------------------------------------------------------------
# The 44.1 kHz endpoint gate (REL-AUD-FORMAT-001)
# ---------------------------------------------------------------------------

function New-FakeSessionReport {
    # The envelope session.latest actually answers, around the fields the recording
    # integrity criteria read.
    param(
        [bool] $Degraded = $false,
        [object[]] $UndrainedFrames = @(0),
        [bool] $SegmentsFinalized = $true
    )
    $tracks = @()
    for ($i = 0; $i -lt $UndrainedFrames.Count; $i++) {
        $tracks += [pscustomobject]@{ track = $i; drained_frames = 512; undrained_frames = $UndrainedFrames[$i] }
    }
    return [pscustomobject]@{ available = $true
        report                          = [pscustomobject]@{
            counters = [pscustomobject]@{ mux_failures = 0 }
            audio    = [pscustomobject]@{ degraded_occurred = $Degraded; resampler_drain = $tracks }
            segments = @([pscustomobject]@{ index = 0; finalized = $SegmentsFinalized })
        }
    }
}

Test-Case 'a recording whose audio stops short of the container fails the endpoint gate' {
    # The assertion this replaces was "the output file has an audio track", which
    # cannot fail on this gate's subject: every capture path opens the endpoint with
    # AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM and asks for 48 kHz, so nothing downstream
    # can tell a 44.1 kHz device from a 48 kHz one. What the gate CAN prove is that
    # recording through that conversion produced a whole file.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseRecordingIntegrityVerdict -Report (New-FakeSessionReport) -ContainerSeconds 8.0 `
        -AudioSpanSeconds @(5.0)
    Assert-Equal 'FAIL' $verdict.Result 'an audio track that covers 5s of an 8s container is a gap'
    Assert-True ($verdict.Message -match '5') "the message must name the span it measured: $($verdict.Message)"
}

Test-Case 'a degraded source or a dropped resampler tail fails the endpoint gate' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $degraded = Get-ReleaseRecordingIntegrityVerdict -Report (New-FakeSessionReport -Degraded $true) `
        -ContainerSeconds 8.0 -AudioSpanSeconds @(8.0)
    Assert-Equal 'FAIL' $degraded.Result 'a source that degraded to silence is not a complete recording'
    Assert-True ($degraded.Message -match 'degraded_occurred') "the message must name it: $($degraded.Message)"

    $undrained = Get-ReleaseRecordingIntegrityVerdict -Report (New-FakeSessionReport -UndrainedFrames @(17)) `
        -ContainerSeconds 8.0 -AudioSpanSeconds @(8.0)
    Assert-Equal 'FAIL' $undrained.Result 'captured audio that never reached the file is a defect'
    Assert-True ($undrained.Message -match '17') "the message must name the loss: $($undrained.Message)"

    $unfinalized = Get-ReleaseRecordingIntegrityVerdict -Report (New-FakeSessionReport -SegmentsFinalized $false) `
        -ContainerSeconds 8.0 -AudioSpanSeconds @(8.0)
    Assert-Equal 'FAIL' $unfinalized.Result 'an unfinalized segment is not a finished recording'
}

Test-Case 'a clean recording passes the endpoint gate' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseRecordingIntegrityVerdict -Report (New-FakeSessionReport) -ContainerSeconds 8.0 `
        -AudioSpanSeconds @(7.99)
    Assert-Equal 'PASS' $verdict.Result "a full, gapless recording must pass: $($verdict.Message)"
}

Test-Case 'a missing session report is unverified for the endpoint gate, never a pass' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    Assert-Equal 'UNVERIFIED' (Get-ReleaseRecordingIntegrityVerdict -Report $null -ContainerSeconds 8.0 `
            -AudioSpanSeconds @(8.0)).Result 'no report means nothing was checked'
    Assert-Equal 'UNVERIFIED' (Get-ReleaseRecordingIntegrityVerdict -Report ([pscustomobject]@{ available = $false }) `
            -ContainerSeconds 8.0 -AudioSpanSeconds @(8.0)).Result 'an empty envelope means nothing was checked'
    # No audio stream at all: nothing was measured about the endpoint, which is a
    # different statement from "the recording is broken".
    Assert-Equal 'UNVERIFIED' (Get-ReleaseRecordingIntegrityVerdict -Report (New-FakeSessionReport) `
            -ContainerSeconds 8.0 -AudioSpanSeconds @()).Result 'no audio track means nothing was measured'
}

Test-Case 'the soak gate and the endpoint gate share one set of integrity criteria' {
    # Duplicated criteria drift. The soak verdict adds its own duration skew, outage
    # budget and zero-tolerance counters on top, and both go through the same
    # helper for what a complete recording means.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $problems = @(Get-ReleaseRecordingIntegrityProblem -Report (New-FakeSessionReport -Degraded $true).report `
            -ContainerSeconds 10.0 -AudioSpanSeconds @(4.0))
    Assert-Equal 2 $problems.Count "the shared helper must find both the gap and the degradation: $($problems -join '; ')"
    $source = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') -Raw
    Assert-Equal 1 ([regex]::Matches($source, "-Path 'audio\.degraded_occurred'")).Count `
        'the degraded-source criterion must be read in exactly one place'
    Assert-Equal 1 ([regex]::Matches($source, "-Path 'undrained_frames'")).Count `
        'and so must the resampler-tail criterion'
}

Test-Case 'the gate no longer claims to read a sample rate the product cannot report' {
    # Windows performs the 44.1 -> 48 kHz conversion inside WASAPI
    # (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM), so a gate that looked for 44100 in the
    # product's own numbers would fail a correct product on every machine.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    Assert-True ($code -notmatch 'source_sample_rate') `
        'no gate may read a per-track capture source rate; no emitter can produce one'
}

# ---------------------------------------------------------------------------
# The Chocolatey package rehearsal (REL-PKG-CHOCO-001)
# ---------------------------------------------------------------------------

Test-Case 'the Chocolatey rehearsal is catalogued as an opt-in prompt gate' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $entry = (Get-ReleaseScenarioCatalog) | Where-Object { $_.Id -eq 'REL-PKG-CHOCO-001' }
    Assert-True ($null -ne $entry) 'REL-PKG-CHOCO-001 must exist in the catalog'
    Assert-Equal 'SECURE' $entry.Layer 'a gate that needs a UAC prompt is SECURE, like the other two'
    Assert-True $entry.OptIn 'a gate that installs and uninstalls software must never run on a default sweep'
}

function New-FakeChocolateyResult {
    param(
        [string[]] $Steps = @('prepare', 'pack', 'removeExisting', 'install', 'uninstall', 'restore'),
        [bool] $RestoreRan = $true,
        [string] $VcredistBefore = '14.44.35211',
        [string] $VcredistAfter = '14.44.35211',
        [string[]] $Observations = @()
    )
    $recorded = @($Steps | ForEach-Object {
            [pscustomobject]@{ name = $_; ok = $true; detail = ''; assertions = @('a', 'b'); failedAssertions = @() }
        })
    return [pscustomobject]@{ ok = $true; steps = $recorded; restoreRan = $RestoreRan
        vcredistBefore              = $VcredistBefore; vcredistAfter = $VcredistAfter; observations = $Observations
    }
}

Test-Case 'a clean Chocolatey rehearsal passes' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseChocolateyVerdict -Result (New-FakeChocolateyResult)
    Assert-Equal 'PASS' $verdict.Result "a clean run must pass: $($verdict.Message)"
    Assert-True ($verdict.Message -match '12 assertion') 'the message must say how much was actually checked'
    Assert-True ($verdict.Message -notmatch 'vcredist') 'an unchanged redistributable is not worth a line'
}

Test-Case 'a rehearsal that changed the Visual C++ redistributable says so' {
    # vcredist140 is a declared dependency of the package, so installing it can
    # install or upgrade one -- and nothing puts that back. A verdict that stayed
    # silent would claim the machine was left as it was found.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseChocolateyVerdict -Result (New-FakeChocolateyResult -VcredistBefore '' `
            -VcredistAfter '14.44.35211')
    Assert-Equal 'PASS' $verdict.Result 'installing a dependency is not a packaging defect'
    Assert-True ($verdict.Message -match 'NOT restored') "the change must be named: $($verdict.Message)"
    Assert-True ($verdict.Message -match 'not installed -> 14\.44\.35211') 'with what it was and what it is'
}

Test-Case 'a rehearsal that left the machine without ExoSnap says so loudly' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $result = New-FakeChocolateyResult -RestoreRan $false
    $result.steps[4].ok = $false
    $result.steps[4].failedAssertions = @('the ARP entry is gone')
    $verdict = Get-ReleaseChocolateyVerdict -Result $result
    Assert-Equal 'FAIL' $verdict.Result 'a failed assertion is a failed rehearsal'
    Assert-True ($verdict.Message -match 'WAS NOT REINSTALLED') `
        "a failure that left no ExoSnap installed must say so: $($verdict.Message)"
}

Test-Case 'observations are recorded in the verdict rather than asserted on' {
    # The empty parent directory and the parent registry key: the package declares
    # no owner for them, so their removal is not something it promises.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseChocolateyVerdict -Result (New-FakeChocolateyResult `
            -Observations @('C:\Program Files\Codexo after uninstall: still present (empty parent, not owned by the package)'))
    Assert-Equal 'PASS' $verdict.Result 'an unowned empty parent is not a packaging defect'
    Assert-True ($verdict.Message -match 'recorded: ') "and it still reaches the report: $($verdict.Message)"

    # The worker must not assert on either of them.
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    Assert-True ($worker -notmatch "Add-Assertion[^\n]*vendorDirectory") `
        'the manufacturer folder is observed, never required'
    Assert-True ($worker -notmatch "Add-Assertion[^\n]*vendorKey") `
        'the manufacturer registry key is observed, never required'
}

Test-Case 'the Chocolatey verdict names the assertion that failed' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $steps = @(
        [pscustomobject]@{ name = 'prepare'; ok = $true; detail = ''; assertions = @('a'); failedAssertions = @() }
        [pscustomobject]@{ name = 'pack'; ok = $true; detail = ''; assertions = @('a'); failedAssertions = @() }
        [pscustomobject]@{ name = 'removeExisting'; ok = $true; detail = ''; assertions = @(); failedAssertions = @() }
        [pscustomobject]@{ name = 'install'; ok = $true; detail = ''; assertions = @('a'); failedAssertions = @() }
        [pscustomobject]@{ name = 'uninstall'; ok = $false; detail = 'residue'
            assertions                                 = @('the ARP entry is gone', 'HKLM:\SOFTWARE\Codexo is gone')
            failedAssertions                           = @('HKLM:\SOFTWARE\Codexo is gone')
        }
        [pscustomobject]@{ name = 'restore'; ok = $true; detail = ''; assertions = @('a'); failedAssertions = @() }
    )
    $verdict = Get-ReleaseChocolateyVerdict -Result ([pscustomobject]@{ ok = $false; steps = $steps })
    Assert-Equal 'FAIL' $verdict.Result 'a failed assertion is a failed rehearsal'
    Assert-True ($verdict.Message -match 'uninstall') 'the message must name the step'
    Assert-True ($verdict.Message -match 'Codexo is gone') `
        "the message must name the assertion, not just the step: $($verdict.Message)"
}

Test-Case 'a rehearsal that never restored the machine is unverified, never a pass' {
    # Every later gate in the campaign expects the release still installed. A worker
    # that packed, installed and uninstalled cleanly and then stopped has passed
    # every step it ran -- which is exactly why the expected steps are listed rather
    # than derived from the result.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $verdict = Get-ReleaseChocolateyVerdict -Result (New-FakeChocolateyResult -RestoreRan $false `
            -Steps @('prepare', 'pack', 'removeExisting', 'install', 'uninstall'))
    Assert-Equal 'UNVERIFIED' $verdict.Result 'a run that stopped early is not a pass'
    Assert-True ($verdict.Message -match 'restore') "the message must name what was never reached: $($verdict.Message)"

    Assert-Equal 'UNVERIFIED' (Get-ReleaseChocolateyVerdict -Result $null).Result `
        'a worker that wrote nothing has proven nothing'
    # `@($null)` is a ONE-element array, so a result with no steps at all used to
    # look like a single anonymous step and this branch was unreachable.
    Assert-Equal 'UNVERIFIED' (Get-ReleaseChocolateyVerdict -Result ([pscustomobject]@{ ok = $true })).Result `
        'a result with no steps has measured nothing'
}

Test-Case 'the worker restores the machine from a finally block, exactly once' {
    # A restore that only runs on the happy path is a restore that never runs when
    # it is needed: an ambiguous ARP entry, a copy that could not be written or a
    # killed choco all leave the machine mid-rehearsal.
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    $finallyIndex = $worker.IndexOf("`nfinally {")
    Assert-True ($finallyIndex -gt 0) 'the worker must have a top-level finally block'
    foreach ($match in [regex]::Matches($worker, "New-Step -Name 'restore'")) {
        Assert-True ($match.Index -gt $finallyIndex) `
            'the restore must live inside finally, so a throw on any earlier step cannot skip it'
    }
    Assert-Equal 1 ([regex]::Matches($worker, "'/i', \`$MsiPath")).Count `
        'and it reinstalls exactly once, never twice'
    Assert-True ($worker.Contains("'/i', `$MsiPath")) 'and it reinstalls the bound MSI'
}

Test-Case 'the worker installs with the community feed so the vcredist dependency resolves' {
    # A directory source alone cannot resolve vcredist140, and choco then fails with
    # a dependency error that reads like a packaging defect.
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    Assert-True ($worker -match 'community\.chocolatey\.org/api/v2/') `
        'the install source must include the community feed'
    Assert-True ($worker.Contains('$workDirectory$chocoSourceSuffix')) `
        'and the local package directory must come first, so the local nupkg wins'
}

Test-Case 'the worker judges the CALLER user configuration directory, not its own' {
    # An elevated child started from another account resolves LOCALAPPDATA to that
    # account's profile, and the uninstall would then be judged against a directory
    # ExoSnap has never written to -- unchanged for the wrong reason.
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    Assert-True ($worker -match '\[Parameter\(Mandatory\)\] \[string\] \$UserConfigDirectory') `
        'the directory is a mandatory parameter'
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    Assert-True ($code -notmatch '\$env:LOCALAPPDATA') 'and never read out of this process environment'
    Assert-True ($worker -match 'exists and is not empty') `
        'an absent or empty directory proves nothing about an uninstall leaving it alone'
}

Test-Case 'the gate ends the shared application session before the worker touches the machine' {
    # Start-ReleaseSession redirects recordings (EXOSNAP_OUTPUT_DIR) but not the
    # configuration directory, so a live campaign session writes into the very
    # directory the uninstall is judged against -- and the running exe would block
    # the uninstall besides.
    $code = @(Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') |
            Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
    $gateBody = [regex]::Match($code, "(?s)REL-PKG-CHOCO-001'.*?HumanGate \`$gate").Value
    Assert-True ($gateBody -match '\$context\.EndSession') `
        'the prompt path must end the session before launching the worker'
    Assert-True ($gateBody -match '\$ctx\.EndSession') 'and so must the elevated path'
}

Test-Case 'the elevated worker is launched with every argument quoted' {
    # Start-Process joins ArgumentList with spaces and quotes nothing, so one
    # unquoted "C:\Program Files\..." arrives as two arguments and the parameter
    # after it silently receives the tail.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $code = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') -Raw
    $launcher = [regex]::Match($code, '(?s)function Invoke-ReleaseElevatedWorker \{.*?\n\}').Value
    $quoteIdiom = "'`"{0}`"' -f"
    Assert-True ($launcher.Contains($quoteIdiom)) 'the launcher must quote each element'
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    Assert-True ($worker.Contains($quoteIdiom)) 'and so must every msiexec/choco invocation inside the worker'
}

Test-Case 'an ExoSnap MSI is identified before anything is installed with it' {
    # "the single .msi that happened to be in that folder" is not an
    # identification, and this gate uninstalls whatever it finds and installs that
    # file.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    foreach ($name in @('ExoSnap-0.9.0-windows-x64.msi', 'ExoSnap-0.9.0-rc17-windows-x64.msi')) {
        Assert-True (Test-ReleaseMsiFileName -Name $name) "$name is a published release name"
    }
    foreach ($name in @('SomethingElse.msi', 'ExoSnap.msi', 'ExoSnap-0.9.0-windows-x86.msi',
            'ExoSnap-setup-windows-x64.msi', 'exosnap-0.9.0-windows-x64.msi.bak')) {
        Assert-True (-not (Test-ReleaseMsiFileName -Name $name)) "$name must not be taken for a release MSI"
    }
}

Test-Case 'an MSI that is not ExoSnap by Codexo is UNAVAILABLE, never rehearsed' {
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $root = New-TestDirectory
    $msi = Join-Path $root 'ExoSnap-0.9.0-windows-x64.msi'
    Set-Content -LiteralPath $msi -Value 'not really an msi' -Encoding utf8NoBOM
    $artifact = [pscustomobject]@{ exePath = $msi }

    # The Property-table read is its own function precisely so the identity rule
    # above it can be exercised without a real installer database.
    function Get-ReleaseMsiProperty { param($Path) return @{ ProductName = 'Notepad++'; Manufacturer = 'Somebody' } }
    $stranger = Resolve-ReleaseMsiArtifact -Artifact $artifact
    Assert-True (-not $stranger.Ok) "a foreign package must be refused: $($stranger.Detail)"
    Assert-True ($stranger.Detail -match 'Notepad\+\+') 'and the reason must name what it actually is'

    function Get-ReleaseMsiProperty { param($Path) return $null }
    $unreadable = Resolve-ReleaseMsiArtifact -Artifact $artifact
    Assert-True (-not $unreadable.Ok) 'a package whose Property table cannot be read is not identified'

    function Get-ReleaseMsiProperty {
        param($Path) return @{ ProductName = 'ExoSnap'; Manufacturer = 'Codexo'; ProductVersion = '0.9.0' }
    }
    $ours = Resolve-ReleaseMsiArtifact -Artifact $artifact
    Assert-True $ours.Ok "the real package is accepted: $($ours.Detail)"
    Assert-Equal '0.9.0' $ours.ProductVersion 'and its ProductVersion is recorded'
}

Test-Case 'the ARP lookup refuses to guess between two ExoSnap products' {
    # Picking one of two matches and uninstalling it is how an unrelated product
    # disappears from a developer machine.
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    Assert-True ($worker.Contains("publisher -ne 'Codexo'")) 'the publisher must be part of the identification'
    Assert-True ($worker.Contains('$key.PSChildName -notmatch')) 'and the key has to be a ProductCode GUID'
    Assert-True ($worker -match 'refusing to guess which one to remove') `
        'more than one match must abort before any msiexec runs'
}

Test-Case 'the runtime-DLL stage key is short enough to launch and cannot collide silently' {
    # Two defects in one line of build infrastructure, both of which stop the
    # release runner's own build gate rather than the product:
    #   * keyed on the ABSOLUTE binary directory, the generated batch file name
    #     reached 269 characters in a build tree under a git worktree, and ninja
    #     could not launch it at all ("The filename or extension is too long") --
    #     no target in the tree built.
    #   * MAKE_C_IDENTIFIER maps '/' and '_' to the same character, so two
    #     different directories can claim one key; the second then stages its DLLs
    #     into the first one's output folder and every test binary there fails to
    #     START with 0xC0000135, which reads as a broken build.
    $cmake = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $scriptRoot) 'cmake/exosnap_testing.cmake') -Raw
    Assert-True ($cmake -match 'file\(RELATIVE_PATH _exosnap_dir_relative') `
        'the stage key must be derived from the path relative to the build root'
    Assert-True ($cmake -notmatch 'MAKE_C_IDENTIFIER "\$\{CMAKE_CURRENT_BINARY_DIR\}"') `
        'never from the absolute binary directory'
    Assert-True ($cmake -match 'exosnap_stage_key_\$\{_exosnap_dir_key\}') `
        'and a claimed key must be recorded so a collision can be detected'
    Assert-True ($cmake -match '(?s)_exosnap_stage_owner.*?message\(FATAL_ERROR') `
        'a collision must be a configure error, not a silently shared target'
}

Test-Case 'the Chocolatey lib directory follows the machine ChocolateyInstall root' {
    $worker = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/choco-rehearsal-worker.ps1') -Raw
    Assert-True ($worker -match '\$env:ChocolateyInstall') `
        'a machine can install Chocolatey elsewhere, and a check against an unused path passes for the wrong reason'
}

Test-Case 'the package copy is pointed at the local MSI and the tracked files are untouched' {
    # The tracked checksum describes an MSI that does not exist on GitHub until the
    # release is built, so the rehearsal has to install from a local path. Doing
    # that by editing the tracked file would leave the repository dirty with a
    # machine-specific path in it.
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $root = New-TestDirectory
    $source = Join-Path $root 'chocolatey'
    New-Item -ItemType Directory -Path (Join-Path $source 'tools') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $source 'exosnap.nuspec') -Value '<package />' -Encoding utf8NoBOM
    $installScript = Join-Path $source 'tools/chocolateyinstall.ps1'
    Set-Content -LiteralPath $installScript -Encoding utf8NoBOM -Value @'
$packageArgs = @{
  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.9.0/ExoSnap-0.9.0-windows-x64.msi'
  checksum64     = '0000000000000000000000000000000000000000000000000000000000000000'
}
'@
    $before = (Get-FileHash -LiteralPath $installScript -Algorithm SHA256).Hash

    $msi = Join-Path $root 'ExoSnap-0.9.0-windows-x64.msi'
    Set-Content -LiteralPath $msi -Value 'not really an msi' -Encoding utf8NoBOM
    $sha = 'abc123' * 10 + 'abcd'
    $copy = New-ReleaseChocolateyPackageCopy -SourceDirectory $source `
        -DestinationDirectory (Join-Path $root 'work') -MsiPath $msi -Sha256 $sha
    Assert-True $copy.Ok "the copy must succeed: $($copy.Detail)"

    $rewritten = Get-Content -LiteralPath $copy.InstallScript -Raw
    Assert-True ($rewritten -match [regex]::Escape($msi)) 'url64bit must point at the local MSI'
    Assert-True ($rewritten -match [regex]::Escape($sha)) 'checksum64 must be the local MSI hash'
    Assert-True ($rewritten -notmatch 'github\.com') 'the published URL must be gone from the copy'
    Assert-Equal $before (Get-FileHash -LiteralPath $installScript -Algorithm SHA256).Hash `
        'the tracked chocolateyinstall.ps1 must not be modified'

    # A file whose shape changed must be refused rather than packed unrewritten:
    # the rehearsal would otherwise silently download the PREVIOUS release.
    $odd = Join-Path $root 'odd'
    New-Item -ItemType Directory -Path (Join-Path $odd 'tools') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $odd 'tools/chocolateyinstall.ps1') -Encoding utf8NoBOM `
        -Value '$packageArgs = @{ url64bit = "double quoted"; checksum64 = "double quoted" }'
    $refused = New-ReleaseChocolateyPackageCopy -SourceDirectory $odd `
        -DestinationDirectory (Join-Path $root 'work2') -MsiPath $msi -Sha256 $sha
    Assert-True (-not $refused.Ok) 'a chocolateyinstall.ps1 the rewrite cannot match must be refused'
}

Test-Case 'the tracked Chocolatey package still has the shape the rehearsal rewrites' {
    # The contract between packaging/chocolatey and REL-PKG-CHOCO-001: exactly one
    # single-quoted url64bit and one checksum64. Checked here rather than only at
    # rehearsal time, because the rehearsal runs elevated on a release machine.
    $installScript = Join-Path (Split-Path -Parent $scriptRoot) 'packaging/chocolatey/tools/chocolateyinstall.ps1'
    Assert-True (Test-Path -LiteralPath $installScript) 'packaging/chocolatey/tools/chocolateyinstall.ps1 must exist'
    $text = Get-Content -LiteralPath $installScript -Raw
    Assert-Equal 1 ([regex]::Matches($text, "(?m)^(\s*url64bit\s*=\s*)'[^']*'")).Count 'exactly one url64bit'
    Assert-Equal 1 ([regex]::Matches($text, "(?m)^(\s*checksum64\s*=\s*)'[^']*'")).Count 'exactly one checksum64'
}


# ---------------------------------------------------------------------------
# The dry run: a simulated operator and simulated tools
# ---------------------------------------------------------------------------
#
# ACCEPTANCE, BEFORE ANY DEVELOPER CLICK. A gate that has never run does not get
# shown to a person, so every human gate and both sight checks are exercised here
# RED once and GREEN once: the runner's whole human layer -- the operator
# protocol, the external-tool adapters, the sandbox transport and each gate's own
# Verify block -- with no machine action of any kind.
#
# Nothing below launches a process, opens a pipe, starts a virtual machine, reads
# a real audio endpoint or writes a registry value. The three seams the human
# layer was built around are what make that possible:
#
#   Set-ReleaseOperatorReader     answers the prompts
#   Set-ReleaseToolInvoker        answers PresentMon, SoundVolumeView, pnputil,
#                                 WindowsSandbox.exe and UI Automation
#   Set-ReleaseToolAvailability   decides which of those exist on this machine
#
# and a scripted control channel stands in for the product.

function Get-DryRunIdentity {
    <#
    .SYNOPSIS
        The handshake identity a simulated connection carries.
    .DESCRIPTION
        Matches the artifact under test by default, because a gate that refuses a
        DIFFERENT binary has to be given the same one before any of its other
        assertions can be reached.
    #>
    param([Parameter(Mandatory)] $Responder)
    if ($Responder.Responses.ContainsKey('identity')) { return $Responder.Responses['identity'] }
    return [pscustomobject]@{ executableSha256 = 'sha-under-test'; productVersion = '0.9.0'; pid = 4242 }
}

function New-DryRunResponder {
    <#
    .SYNOPSIS
        A scripted control channel: command name -> answer.
    .DESCRIPTION
        A value may be a plain object (every call answers it), an ARRAY (successive
        calls consume successive elements and the last repeats, which is how
        "degraded, then recovered" is expressed), or a script block taking the
        parameters. A refusal is @{ ok = $false; error = @{ message = '...' } }.

        An unscripted command answers ok with an empty result rather than throwing:
        a gate under test must fail on the assertion it is about, not on the first
        incidental command a fixture forgot.
    #>
    param([hashtable] $Responses = @{})
    return @{ Responses = $Responses; Calls = @{}; Log = [System.Collections.Generic.List[string]]::new() }
}

function Invoke-DryRunResponse {
    param([Parameter(Mandatory)] $Responder, [Parameter(Mandatory)] [string] $Command, $Parameters)
    $Responder.Log.Add($Command)
    if (-not $Responder.Responses.ContainsKey($Command)) { return @{ ok = $true; result = [pscustomobject]@{ } } }
    $value = $Responder.Responses[$Command]
    if ($value -is [scriptblock]) { $value = & $value $Parameters }
    if ($value -is [object[]]) {
        $index = if ($Responder.Calls.ContainsKey($Command)) { $Responder.Calls[$Command] } else { 0 }
        $Responder.Calls[$Command] = [Math]::Min($index + 1, $value.Count - 1)
        $value = $value[[Math]::Min($index, $value.Count - 1)]
    }
    if ($value -is [System.Collections.IDictionary] -and $value.Contains('ok')) { return $value }
    return @{ ok = $true; result = $value }
}

function Invoke-DryRunHumanGate {
    <#
    .SYNOPSIS
        The shipped human-gate contract, over the simulated operator.
    .DESCRIPTION
        Lifted out of release-verify.ps1 rather than reimplemented, for the same
        reason the scenarios are the shipped ones: a harness that carried its own
        idea of what a gate does could not catch the gate drifting away from it.
    #>
    param([Parameter(Mandatory)] $Gate, [Parameter(Mandatory)] $Context)
    $NonInteractive = $false
    $Attest = @()
    function Expand-ListArgument { param($Values) return @($Values) }
    function Write-Step { param([string] $Text) }
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Get-ReleaseGateLine')))
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'New-ReleaseOperatorStep')))
    . ([scriptblock]::Create((Get-ReleaseVerifyFunctionText -Name 'Invoke-ReleaseHumanGate')))
    return Invoke-ReleaseHumanGate -Gate $Gate -Context $Context
}

function Invoke-ReleaseDryRun {
    <#
    .SYNOPSIS
        Runs one real scenario body against a simulated machine and operator.
    .DESCRIPTION
        The scenario is the SHIPPED one out of Get-ReleaseScenarioCatalog, not a
        copy: a harness that tested its own transcription of a gate would prove
        nothing about the gate. Everything the body reaches outside itself is
        replaced here, in this function's own scope, so the substitutions cannot
        leak into another test.

        Returns the scenario's own result, plus the prompts the simulated operator
        was shown and the tool invocations it caused -- both of which are
        assertions in their own right: a gate that reached a verdict without ever
        asking, or that asked with the wrong prompt, is a defect the result alone
        cannot show.
    #>
    param(
        [Parameter(Mandatory)] [string] $ScenarioId,
        [hashtable] $Responses = @{},
        [hashtable] $Tools = @{},
        [scriptblock] $ToolInvoker,
        [scriptblock] $Operator,
        [object[]] $EnvctlProperties = @(),
        [hashtable] $Variables = @{},
        [switch] $Elevated,
        [switch] $FullscreenProbe
    )

    $root = New-TestDirectory
    $prompts = [System.Collections.Generic.List[string]]::new()
    $invocations = [System.Collections.Generic.List[object]]::new()
    $responder = New-DryRunResponder -Responses $Responses
    $envctlProperties = $EnvctlProperties

    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    . (Join-Path $scriptRoot 'lib/ReleaseExternalTools.ps1')
    . (Join-Path $scriptRoot 'lib/ReleaseSandbox.ps1')

    # --- the product, scripted -------------------------------------------------
    function Invoke-LiveVerifyCommand {
        param($Connection, [string] $Command, $Parameters)
        return Invoke-DryRunResponse -Responder $responder -Command $Command -Parameters $Parameters
    }
    function Connect-LiveVerify {
        param([string] $RunId, [int] $ConnectTimeoutMs, [string] $Role, [int] $Protocol)
        if ($responder.Responses.ContainsKey('connect.refuse')) { throw 'the endpoint refused the connection' }
        return [pscustomobject]@{ RunId = $RunId; Identity = (Get-DryRunIdentity -Responder $responder) }
    }
    function Get-LiveVerifyState { param($Connection) return [pscustomobject]@{ blockingSurface = 'none' } }
    function New-LiveVerifyRunId { return 'dry-run-id' }
    function Get-LiveVerifyFfprobe { return $responder.Responses['ffprobe'] }

    # --- the runner's own environment -----------------------------------------
    function Write-Step { param([string] $Text) }
    function Write-Heading { param([string] $Text) }
    function Start-Sleep { param([int] $Seconds, [int] $Milliseconds) }
    function Get-EnvironmentSnapshot { param($Orchestrator) return [pscustomobject]@{ properties = $envctlProperties } }
    function Get-Process { param([string] $Name, $ErrorAction) return @() }
    function Get-PnpDevice { param($Class, $Status, $ErrorAction) return @() }
    function Start-Process {
        param([string] $FilePath, $ArgumentList, [switch] $PassThru, [switch] $Wait)
        $invocations.Add(@{ Tool = 'Start-Process'; Path = $FilePath; Arguments = @($ArgumentList) })
        return [pscustomobject]@{ Id = 4242; HasExited = $true }
    }

    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')

    # Defined AFTER the dot-source so they replace the catalogue's own: a real
    # sleep-and-poll loop would make the dry run take as long as a campaign, and a
    # real appearance switch would change the developer's Windows.
    function Wait-ReleaseRecordingState { param($Connection, $States, $TimeoutMs) return $true }
    # Every bounded poll finishes in a few milliseconds here. The loops are the
    # shipped ones; only how long they are willing to wait for a machine that is
    # not there is shortened.
    function Get-ReleaseGateDeadline { param([double] $Seconds) return [DateTime]::UtcNow.AddMilliseconds(40) }
    function Test-RunnerElevated { return [bool]$Elevated }
    # The outage seam, stubbed rather than run: the real one starts a background job
    # that DISABLES an audio device on this machine. What the gate is judged on is
    # what the product reported while the device was gone, and the scripted
    # pipeline snapshots say that without anybody's sound card being touched.
    function Start-ReleaseEndpointOutage {
        param([string] $Executable, [string[]] $DisableArguments, [string[]] $EnableArguments,
            [int] $DelaySeconds, [int] $OutageSeconds)
        $invocations.Add(@{ Tool = 'outage'; Path = $Executable; Arguments = @($DisableArguments) })
        return $null
    }
    function Stop-ReleaseEndpointOutage {
        param($Job, [string] $Executable, [string[]] $EnableArguments)
        $invocations.Add(@{ Tool = 'outage-restore'; Path = $Executable; Arguments = @($EnableArguments) })
    }
    function Wait-ReleaseProbeGone { param($ProcessName, $TimeoutMs) return $true }
    function Get-WindowsAppearance { return 'Dark' }
    function Set-WindowsAppearance { param([string] $Appearance) $invocations.Add(@{ Tool = 'appearance'; Value = $Appearance }) }
    function Resolve-FullscreenProbe { if ($FullscreenProbe) { return 'C:\probe\probe_fullscreen.exe' } return $null }
    function Resolve-StallWindowProbe { return $null }
    function Get-ReleaseAudioPacketSpan { param($FfprobePath, $Path, $StreamIndexes) return @(8.0) }
    function Save-LiveVerifyEvidence {
        param($Context, [string] $CheckId, [string] $Name, $Value, [string] $Raw)
        return "checks/$CheckId/$Name"
    }
    function Get-ReleaseAutomationElements {
        param([int] $ProcessId, [int] $TimeoutSeconds)
        if ($responder.Responses.ContainsKey('uia')) { return $responder.Responses['uia'] }
        return @{ Ok = $true; Elements = @(); Detail = 'no simulated automation tree' }
    }
    function Close-ReleaseUpdaterProcess { param([int] $TimeoutMs) $invocations.Add(@{ Tool = 'updater-close' }) }
    function Resolve-ReleaseMsiArtifact {
        param($Artifact)
        if ($responder.Responses.ContainsKey('msi')) { return $responder.Responses['msi'] }
        return @{ Ok = $false; Detail = 'no MSI beside the artifact in this dry run' }
    }

    # --- the operator and the external tools ----------------------------------
    Clear-ReleaseToolAvailability
    foreach ($name in $Tools.Keys) { Set-ReleaseToolAvailability -Name $name -Path $Tools[$name] }
    Set-ReleaseToolInvoker {
        param($Tool, $Arguments)
        $invocations.Add(@{ Tool = $Tool.Name; Arguments = @($Arguments) })
        if ($null -ne $ToolInvoker) { return & $ToolInvoker $Tool $Arguments }
        return @{ ExitCode = 0; Output = '' }
    }.GetNewClosure()
    Set-ReleaseOperatorReader {
        param([string] $Prompt)
        $prompts.Add($Prompt)
        if ($null -ne $Operator) { return & $Operator $Prompt }
        return ''
    }.GetNewClosure()

    $previousVariables = @{}
    foreach ($name in $Variables.Keys) {
        $previousVariables[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, $Variables[$name])
    }

    try {
        $catalog = Get-ReleaseScenarioCatalog
        $entry = @($catalog | Where-Object { $_.Id -eq $ScenarioId }) | Select-Object -First 1
        if ($null -eq $entry) { throw "No such scenario in the catalog: $ScenarioId" }

        $session = [pscustomobject]@{ RunId = 'dry-run-session'
            # `Identity` is what a real connection carries after the handshake, and
            # REL-PRESENT-002 compares it against the artifact SHA-256 before it
            # believes anything the session says.
            Connection                      = [pscustomobject]@{ Name = 'fake'
                Identity                    = (Get-DryRunIdentity -Responder $responder)
            }
            Process                         = [pscustomobject]@{ HasExited = $false }
        }
        $context = $null
        $context = [pscustomobject]@{
            RunDirectory    = $root
            RepositoryRoot  = (Split-Path -Parent $scriptRoot)
            Artifact        = [pscustomobject]@{ exePath = 'C:\rc\exosnap.exe'; exeSha256 = 'sha-under-test'
                installTree                             = $true; productVersion = '0.9.0'
            }
            Environment     = @{}
            Orchestrator    = [pscustomobject]@{ Available = ($envctlProperties.Count -gt 0); Dirty = $false }
            State           = @{}
            EnsureSession   = { $session }.GetNewClosure()
            EndSession      = { }
            ElevatedSession = { if ($Elevated) { $session } else { $null } }.GetNewClosure()
            HumanGate       = { param($gate) Invoke-DryRunHumanGate -Gate $gate -Context $context }
            Judge           = { param($id, $line, $detail)
                $step = if ($null -eq $detail) { @{} } else { $detail.Clone() }
                $step['Id'] = $id
                $step['Line'] = $line
                return Invoke-ReleaseOperatorJudgement -Step $step
            }
        }
        $result = & $entry.Run $context
        return @{ Result = $result; Prompts = @($prompts); Invocations = @($invocations)
            Commands            = @($responder.Log); Entry = $entry
        }
    }
    finally {
        Set-ReleaseOperatorReader $null
        Set-ReleaseToolInvoker $null
        Clear-ReleaseToolAvailability
        foreach ($name in $previousVariables.Keys) {
            [Environment]::SetEnvironmentVariable($name, $previousVariables[$name])
        }
    }
}

function New-DryRunPresentSnapshot {
    param([string] $Mode = 'independentFlip', [int] $Count = 1200, [bool] $Available = $true, [bool] $Elevated = $true)
    return [pscustomobject]@{
        present = [pscustomobject]@{ optIn = $true; elevated = $Elevated; available = $Available
            availability                   = $(if ($Available) { 'available' } else { 'requiresElevation' })
            reason                         = $null; mode = $Mode; tearing = $false; presentCount = $Count
            discardedCount                 = 0; modeFlipCount = 0
        }
        audio   = [pscustomobject]@{ outputs = @(
                [pscustomobject]@{ name = 'Speakers'; default = $true },
                [pscustomobject]@{ name = 'CABLE Input (VB-Audio Virtual Cable)'; default = $false }
            )
        }
    }
}

function New-DryRunPipelineSnapshot {
    param([bool] $Degraded = $false, [string] $Lifecycle = 'recording', [bool] $Active = $true)
    return [pscustomobject]@{ lifecycle = $Lifecycle
        audio                           = [pscustomobject]@{ active = $Active; sourceDegraded = $Degraded; degradedSources = $(if ($Degraded) { 1 } else { 0 }) }
    }
}

function New-DryRunSandboxResult {
    param([hashtable] $Steps)
    $list = @()
    foreach ($name in $Steps.Keys) { $list += [pscustomobject]@{ name = $name; ok = $Steps[$name]; detail = 'dry run' } }
    return [pscustomobject]@{ finishedUtc = '2026-09-07T00:00:00Z'; fatal = ''; steps = $list }
}

function Write-DryRunSandboxDocument {
    <#
    .SYNOPSIS
        Places a sandbox result where the gate expects one, so no sandbox is started.
    .DESCRIPTION
        Both MSI gates reuse the document a previous run produced. Writing one is
        therefore the whole of "the sandbox ran" as far as the gate is concerned,
        which is exactly the seam a dry run needs -- and it is the same reuse path
        a real second gate takes, not a special case invented for the test.
    #>
    param([Parameter(Mandatory)] [string] $RunDirectory, [Parameter(Mandatory)] $Result)
    $directory = Join-Path $RunDirectory 'checks/update-sandbox'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $directory 'result.json') -Encoding utf8NoBOM `
        -Value ($Result | ConvertTo-Json -Depth 12)
}

# ---------------------------------------------------------------------------
# The operator protocol
# ---------------------------------------------------------------------------

Test-Case 'there are exactly two action prompts and they say who acts next' {
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    # The rc19 defect, pinned: one line meant "start it now" for the MSI and
    # Chocolatey gates and "I already did it" for the present, audio and visual
    # gates. Two forms now, and each names the actor.
    Assert-Equal '[Enter] startet jetzt' (Get-ReleaseOperatorPromptText -Kind 'Start') 'the runner acts on Enter'
    Assert-Equal '[Enter] wenn erledigt' (Get-ReleaseOperatorPromptText -Kind 'Done') 'the operator has acted'
    Assert-True ((Get-ReleaseOperatorPromptText -Kind 'Judgement') -notmatch 'Enter') `
        'a judgement must not be answerable by the reflex that answers an action prompt'
}

Test-Case 'Enter proceeds, ? shows the detail without deciding, s and x do not' {
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    $step = @{ Id = 'T-1'; Line = 'do the thing'; Kind = 'Done'; Why = 'because'; Expected = 'a thing'
        VerifyDescription                                                                  = 'by looking'
    }
    $answers = [System.Collections.Generic.Queue[string]]::new()
    @('?', '') | ForEach-Object { $answers.Enqueue($_) }
    Set-ReleaseOperatorReader { param($p) return $answers.Dequeue() }.GetNewClosure()
    try {
        Assert-Equal 'go' (Invoke-ReleaseOperatorStep -Step $step -Quiet) 'Enter after ? must still proceed'
        Assert-Equal 0 $answers.Count 'both answers must have been consumed'

        Set-ReleaseOperatorReader { param($p) return 's' }
        Assert-Equal 'skip' (Invoke-ReleaseOperatorStep -Step $step -Quiet) 's skips'
        Set-ReleaseOperatorReader { param($p) return 'x' }
        Assert-Equal 'abort' (Invoke-ReleaseOperatorStep -Step $step -Quiet) 'x stops'
    }
    finally { Set-ReleaseOperatorReader $null }
}

Test-Case 'an unrecognised answer decides nothing and is asked again' {
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    # A typo, a stray paste or a leftover keystroke must never end a gate: the old
    # prompt treated anything unrecognised as an abort, which is a destructive
    # default for a single character.
    $answers = [System.Collections.Generic.Queue[string]]::new()
    @('yes please', 'zzz', '') | ForEach-Object { $answers.Enqueue($_) }
    Set-ReleaseOperatorReader { param($p) return $answers.Dequeue() }.GetNewClosure()
    try {
        Assert-Equal 'go' (Invoke-ReleaseOperatorStep -Step @{ Id = 'T'; Line = 'l'; Kind = 'Done' } -Quiet) `
            'only Enter proceeds; the two typos before it must have been re-asked'
        Assert-Equal 0 $answers.Count 'every answer must have been consumed'
    }
    finally { Set-ReleaseOperatorReader $null }
}

Test-Case 'a sight check records the reason with the judgement, not afterwards' {
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    $answers = [System.Collections.Generic.Queue[string]]::new()
    @('n', 'the toast tint is grey') | ForEach-Object { $answers.Enqueue($_) }
    Set-ReleaseOperatorReader { param($p) return $answers.Dequeue() }.GetNewClosure()
    try {
        $judgement = Invoke-ReleaseOperatorJudgement -Step @{ Id = 'T'; Line = 'does it look right?' }
        Assert-Equal 'wrong' $judgement.Answer 'n is a defect report'
        Assert-Equal 'the toast tint is grey' $judgement.Reason 'and the reason travels with it'
    }
    finally { Set-ReleaseOperatorReader $null }
}

Test-Case 'the ordered sequence puts every dependency before the gate that needs it' {
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $catalog = Get-ReleaseScenarioCatalog
    $ordered = @(Get-ReleaseHumanPlanOrder -Entries ([object[]]$catalog))
    $position = @{}
    for ($i = 0; $i -lt $ordered.Count; $i++) { $position[$ordered[$i].Id] = $i }
    Assert-Equal $catalog.Count $ordered.Count 'ordering must not lose or duplicate a scenario'
    foreach ($entry in $ordered) {
        foreach ($dependency in @(Get-ReleaseScenarioDependency -Entry $entry)) {
            Assert-True ($position[$dependency] -lt $position[$entry.Id]) `
                "$dependency must run before $($entry.Id)"
        }
    }
    # The three that cost a campaign a FAIL, named rather than merely implied.
    Assert-True ($position['REL-PRESENT-002'] -lt $position['REL-CAP-FSE-001']) 'present before fullscreen'
    Assert-True ($position['REL-UPD-MSI-DECLINE-001'] -lt $position['REL-UPD-MSI-001']) 'decline before accept'
    Assert-True ($position['REL-UPD-MSI-001'] -lt $position['REL-PKG-CHOCO-001']) 'the MSI gates before Chocolatey'
}

Test-Case 'a dependency cycle is reported rather than silently broken' {
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    $a = [pscustomobject]@{ Id = 'A'; Title = 'a'; DependsOn = @('B') }
    $b = [pscustomobject]@{ Id = 'B'; Title = 'b'; DependsOn = @('A') }
    Assert-Throws { Get-ReleaseHumanPlanOrder -Entries @($a, $b) } 'an unrunnable order is a catalog defect'
}

Test-Case 'every gate that can ask a person declares one line and which form it is' {
    # The sequence view promises the operator how many steps may ask them
    # something. A gate that reaches a prompt without declaring AsksAPerson makes
    # that promise false, and one without a Line falls back to the first entry of
    # the old numbered block -- which is how the meaning got separated from the
    # question in the first place.
    . (Join-Path $scriptRoot 'lib/ReleaseOperator.ps1')
    . (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1')
    $expected = @('REL-PRESENT-002', 'REL-CAP-STALL-001', 'REL-CAP-FSE-001', 'REL-AUD-DEGRADE-001',
        'REL-AUD-SILENCE-001', 'REL-AUD-FORMAT-001', 'REL-VIS-OVERLAY-001', 'REL-VIS-NOTIFY-001',
        'REL-UPD-MSI-DECLINE-001', 'REL-UPD-MSI-001', 'REL-PKG-CHOCO-001')
    $catalog = Get-ReleaseScenarioCatalog
    $declared = @($catalog | Where-Object { Test-ReleaseScenarioAsksAPerson -Entry $_ } | ForEach-Object { $_.Id })
    foreach ($id in $expected) {
        Assert-True ($declared -contains $id) "$id can reach an operator prompt and must declare AsksAPerson"
    }
    $source = Get-Content -LiteralPath (Join-Path $scriptRoot 'lib/ReleaseScenarios.ps1') -Raw
    $lines = ([regex]::Matches($source, "(?m)^\s*Line\s*=")).Count
    Assert-True ($lines -ge 10) "every operator gate needs its own Line (found $lines)"
    # There are TWO action forms and no third. A gate that invented one would put
    # the campaign back where rc19 found it: a prompt whose meaning depends on
    # which screen of instructions scrolled past.
    $allKinds = ([regex]::Matches($source, "(?m)^\s*Kind\s*=")).Count
    $knownKinds = ([regex]::Matches($source, "(?m)^\s*Kind\s*=\s*'(Start|Done)'")).Count
    Assert-Equal $allKinds $knownKinds 'a gate may only declare Start or Done'
    # And exactly two scenarios end in a person's eyes. Every other gate is decided
    # by a measurement, which is what makes the sight checks the last human gates
    # rather than the usual ones.
    $judged = ([regex]::Matches($source, '\$ctx\.Judge')).Count
    Assert-Equal 2 $judged "only the two sight checks may ask for a judgement (found $judged)"
}

function New-DryRunSandboxLauncher {
    <#
    .SYNOPSIS
        A WindowsSandbox.exe that writes what the worker inside it would have.
    .DESCRIPTION
        Everything up to the virtual machine is exercised for real: the staging
        copy, the .wsb, the mapped-folder paths and the marker-plus-result
        transport. Only the machine is simulated -- which is the whole of what a
        dry run may not start.
    #>
    param([Parameter(Mandatory)] $Document, [switch] $WriteNothing)
    return {
        param($Tool, $Arguments)
        if ($Tool.Name -ne 'sandbox') { return @{ ExitCode = 0; Output = '' } }
        if ($WriteNothing) { return @{ ExitCode = 0; Output = '' } }
        $staging = Split-Path -Parent $Arguments[0]
        Set-Content -LiteralPath (Join-Path $staging 'result.json') -Encoding utf8NoBOM `
            -Value ($Document | ConvertTo-Json -Depth 12)
        Set-Content -LiteralPath (Join-Path $staging 'done.marker') -Value 'done' -Encoding utf8NoBOM
        return @{ ExitCode = 0; Output = '' }
    }.GetNewClosure()
}

function Invoke-DryRunSandboxUpdateGate {
    <#
    .SYNOPSIS
        One MSI gate, over a simulated sandbox run of the update rehearsal.
    #>
    param([Parameter(Mandatory)] [string] $ScenarioId, [Parameter(Mandatory)] [hashtable] $Steps)
    $root = New-TestDirectory
    $baseMsi = Join-Path $root 'ExoSnap-0.8.0-windows-x64.msi'
    Set-Content -LiteralPath $baseMsi -Value 'not really an msi' -Encoding utf8NoBOM
    return Invoke-ReleaseDryRun -ScenarioId $ScenarioId `
        -Tools @{ sandbox = 'C:\Windows\System32\WindowsSandbox.exe' } `
        -Variables @{ EXOSNAP_UPDATE_FROM_MSI = $baseMsi } `
        -ToolInvoker (New-DryRunSandboxLauncher -Document (New-DryRunSandboxResult -Steps $Steps))
}

function Invoke-DryRunChocolateyGate {
    <#
    .SYNOPSIS
        The Chocolatey gate, over a simulated sandbox run of the real rehearsal worker.
    #>
    param([Parameter(Mandatory)] [object[]] $Steps, [bool] $RestoreRan = $true)
    $root = New-TestDirectory
    $msi = Join-Path $root 'ExoSnap-0.9.0-windows-x64.msi'
    Set-Content -LiteralPath $msi -Value 'not really an msi' -Encoding utf8NoBOM
    $document = [pscustomobject]@{
        steps          = @($Steps | ForEach-Object { [pscustomobject]$_ })
        restoreRan     = $RestoreRan
        vcredistBefore = '14.40.0'
        vcredistAfter  = '14.40.0'
    }
    return Invoke-ReleaseDryRun -ScenarioId 'REL-PKG-CHOCO-001' `
        -Tools @{ sandbox = 'C:\Windows\System32\WindowsSandbox.exe' } `
        -Responses @{ msi = @{ Ok = $true; Path = $msi; Sha256 = ('ab' * 32); Detail = 'dry run' } } `
        -ToolInvoker (New-DryRunSandboxLauncher -Document $document)
}


# ---------------------------------------------------------------------------
# Every human gate, RED once and GREEN once
# ---------------------------------------------------------------------------

function New-DryRunPresentMonInvoker {
    <#
    .SYNOPSIS
        A PresentMon that writes the CSV it is asked for.
    .DESCRIPTION
        The rows are what the gate compares against our own classification, so the
        modes are the fixture's whole point. Writing a real file rather than
        returning rows keeps Get-ReleasePresentMonObservation's own CSV handling in
        the path -- an oracle that cannot be read is not an oracle.
    #>
    param([string[]] $Modes = @('Hardware: Legacy Flip'), [int] $Rows = 3, [int] $ExitCode = 0)
    return {
        param($Tool, $Arguments)
        if ($Tool.Name -ne 'presentmon') { return @{ ExitCode = 0; Output = '' } }
        if ($ExitCode -ne 0) { return @{ ExitCode = $ExitCode; Output = 'PresentMon refused' } }
        $index = [array]::IndexOf($Arguments, '--output_file')
        $csv = $Arguments[$index + 1]
        New-Item -ItemType Directory -Path (Split-Path -Parent $csv) -Force | Out-Null
        $lines = @('Application,ProcessID,PresentMode')
        for ($i = 0; $i -lt $Rows; $i++) { $lines += "probe.exe,4242,$($Modes[$i % $Modes.Count])" }
        Set-Content -LiteralPath $csv -Value $lines -Encoding utf8NoBOM
        return @{ ExitCode = 0; Output = '' }
    }.GetNewClosure()
}

function New-DryRunFfprobe {
    <#
    .SYNOPSIS
        A stand-in ffprobe that prints one canned probe document.
    #>
    param([Parameter(Mandatory)] [string] $Directory, [bool] $WithAudio = $true, [double] $Duration = 8.0)
    $streams = if ($WithAudio) {
        '{"index":0,"codec_type":"video","codec_name":"h264"},{"index":1,"codec_type":"audio","codec_name":"opus","sample_rate":"48000"}'
    }
    else { '{"index":0,"codec_type":"video","codec_name":"h264"}' }
    $json = '{"format":{"duration":"' + $Duration + '"},"streams":[' + $streams + ']}'
    $path = Join-Path $Directory 'fake-ffprobe.ps1'
    Set-Content -LiteralPath $path -Encoding utf8NoBOM -Value @"
param([Parameter(ValueFromRemainingArguments = `$true)] [string[]] `$Rest)
Write-Output '$json'
"@
    return $path
}

Test-Case 'REL-PRESENT-002 is red when the ETW session decodes nothing and green when it does' {
    # RED: elevated, opted in, and still no presents -- the case an operator used to
    # be asked about after launching an elevated instance by hand.
    $red = Invoke-ReleaseDryRun -ScenarioId 'REL-PRESENT-002' -Elevated -Responses @{
        'environment.snapshot' = (New-DryRunPresentSnapshot -Available $false)
    }
    Assert-Equal 'FAIL' $red.Result.Result "an unavailable present session must fail: $($red.Result.Message)"
    Assert-Equal 0 $red.Prompts.Count 'an elevated runner must not ask anybody about this gate'

    # GREEN, with the independent oracle agreeing.
    $green = Invoke-ReleaseDryRun -ScenarioId 'REL-PRESENT-002' -Elevated `
        -Tools @{ presentmon = 'C:\tools\PresentMon.exe' } `
        -ToolInvoker (New-DryRunPresentMonInvoker -Modes @('Hardware: Independent Flip')) `
        -Responses @{ 'environment.snapshot' = (New-DryRunPresentSnapshot) }
    Assert-Equal 'PASS' $green.Result.Result "a decoded present session must pass: $($green.Result.Message)"
    Assert-True ($green.Result.Message -match 'PresentMon corroborates') `
        "the verdict must name the independent oracle: $($green.Result.Message)"
}

Test-Case 'REL-PRESENT-002 fails when PresentMon decodes nothing we claim to have decoded' {
    # The reason the oracle is here at all: our numbers come from an ETW session we
    # opened, so a gate that only reads them back asks one decoder whether it agrees
    # with itself.
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-PRESENT-002' -Elevated `
        -Tools @{ presentmon = 'C:\tools\PresentMon.exe' } `
        -ToolInvoker (New-DryRunPresentMonInvoker -Rows 0) `
        -Responses @{ 'environment.snapshot' = (New-DryRunPresentSnapshot) }
    Assert-Equal 'FAIL' $result.Result.Result 'an uncorroborated present count is not evidence'
    Assert-True ($result.Result.Message -match 'not corroborated') $result.Result.Message
}

Test-Case 'REL-CAP-FSE-001 is red on a composed window and green on a real exclusive one' {
    $responses = @{ 'environment.snapshot' = (New-DryRunPresentSnapshot -Mode 'composed') }
    $red = Invoke-ReleaseDryRun -ScenarioId 'REL-CAP-FSE-001' -Elevated -FullscreenProbe -Responses $responses
    Assert-Equal 'FAIL' $red.Result.Result "a composed window is not exclusive fullscreen: $($red.Result.Message)"

    $green = Invoke-ReleaseDryRun -ScenarioId 'REL-CAP-FSE-001' -Elevated -FullscreenProbe `
        -Tools @{ presentmon = 'C:\tools\PresentMon.exe' } `
        -ToolInvoker (New-DryRunPresentMonInvoker -Modes @('Hardware: Legacy Flip')) `
        -Responses @{ 'environment.snapshot' = (New-DryRunPresentSnapshot -Mode 'exclusiveFullscreen') }
    Assert-Equal 'PASS' $green.Result.Result "a real exclusive flip must pass: $($green.Result.Message)"
    Assert-Equal 0 $green.Prompts.Count 'the probe answers this gate; nobody is asked'
    Assert-True ($green.Result.Message -match 'PresentMon agrees') $green.Result.Message
}

Test-Case 'REL-CAP-FSE-001 fails when the two present decoders disagree' {
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-CAP-FSE-001' -Elevated -FullscreenProbe `
        -Tools @{ presentmon = 'C:\tools\PresentMon.exe' } `
        -ToolInvoker (New-DryRunPresentMonInvoker -Modes @('Composed: Flip')) `
        -Responses @{ 'environment.snapshot' = (New-DryRunPresentSnapshot -Mode 'exclusiveFullscreen') }
    Assert-Equal 'FAIL' $result.Result.Result 'a disagreement about the most consequential capture path is a finding'
    Assert-True ($result.Result.Message -match 'Composed: Flip') $result.Result.Message
}

Test-Case 'REL-CAP-FSE-001 reuses the elevated session instead of launching a second instance' {
    # The rc19 defect: it launched its own unelevated instance while the elevated one
    # was up, the machine-wide single-instance guard swallowed it, and the gate
    # reported "could not connect to the Live Verify endpoint" for a healthy product.
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-CAP-FSE-001' -Elevated -FullscreenProbe `
        -Responses @{ 'environment.snapshot' = (New-DryRunPresentSnapshot -Mode 'exclusiveFullscreen') }
    $launches = @($result.Invocations | Where-Object { $_.Tool -eq 'Start-Process' -and $_.Path -match 'exosnap' })
    Assert-Equal 0 $launches.Count 'the gate must launch no ExoSnap of its own'
    $entry = $result.Entry
    Assert-True (@($entry.DependsOn) -contains 'REL-PRESENT-002') 'and it must declare where the session comes from'
    Assert-True ([bool]$entry.UsesElevatedSession) 'and that it wants the shared elevated instance'
}

Test-Case 'REL-AUD-SILENCE-001 is red when quiet is reported as degraded and green when it is not' {
    $endpoints = New-DryRunPresentSnapshot
    $red = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-SILENCE-001' `
        -Tools @{ soundvolumeview = 'C:\tools\SoundVolumeView.exe' } `
        -Responses @{
        'environment.snapshot' = $endpoints
        'record.snapshot'      = [pscustomobject]@{ systemAudioEnabled = $true }
        'pipeline.snapshot'    = (New-DryRunPipelineSnapshot -Degraded $true)
    }
    Assert-Equal 'FAIL' $red.Result.Result "silence must not be reported as device loss: $($red.Result.Message)"
    Assert-True ($red.Result.Message -match 'silent source was reported as degraded') $red.Result.Message

    $green = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-SILENCE-001' `
        -Tools @{ soundvolumeview = 'C:\tools\SoundVolumeView.exe' } `
        -Responses @{
        'environment.snapshot' = $endpoints
        'record.snapshot'      = [pscustomobject]@{ systemAudioEnabled = $true }
        'pipeline.snapshot'    = (New-DryRunPipelineSnapshot)
    }
    Assert-Equal 'PASS' $green.Result.Result "an active, quiet source must pass: $($green.Result.Message)"
    Assert-Equal 0 $green.Prompts.Count 'a virtual cable makes silence a fact, so nobody is asked'
    Assert-True ($green.Result.Message -match 'CABLE Input') "and the verdict says what it routed to: $($green.Result.Message)"
}

Test-Case 'REL-AUD-SILENCE-001 puts the default endpoint back, and asks when it cannot route' {
    $endpoints = New-DryRunPresentSnapshot
    $routed = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-SILENCE-001' `
        -Tools @{ soundvolumeview = 'C:\tools\SoundVolumeView.exe' } `
        -Responses @{
        'environment.snapshot' = $endpoints
        'record.snapshot'      = [pscustomobject]@{ systemAudioEnabled = $true }
        'pipeline.snapshot'    = (New-DryRunPipelineSnapshot)
    }
    $switches = @($routed.Invocations | Where-Object { $_.Tool -eq 'soundvolumeview' })
    Assert-Equal 2 $switches.Count 'the default endpoint is switched and switched back'
    Assert-True ($switches[-1].Arguments -contains 'Speakers') `
        'the operator default must be the LAST thing this gate sets'

    # No tool -> one line, and it says why it is being asked.
    $asked = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-SILENCE-001' -Tools @{ soundvolumeview = $null } `
        -Operator { param($p) return '' } -Responses @{
        'environment.snapshot' = $endpoints
        'record.snapshot'      = [pscustomobject]@{ systemAudioEnabled = $true }
        'pipeline.snapshot'    = (New-DryRunPipelineSnapshot)
    }
    Assert-Equal 'PASS' $asked.Result.Result $asked.Result.Message
    Assert-Equal 1 $asked.Prompts.Count 'exactly one line is shown, not a block of four'
    Assert-True ($asked.Prompts[0] -match 'wenn erledigt') "and it is the you-have-acted form: $($asked.Prompts[0])"
}

Test-Case 'REL-AUD-FORMAT-001 stops asking a person for a machine-state precondition' {
    # THE rc19 DEFECT. Step 3 of a four-part block was "make it the DEFAULT playback
    # device" -- a machine state, asked of a person, and then reported as a product
    # FAIL when they set the format and not the role.
    $root = New-TestDirectory
    $properties = @(
        [pscustomobject]@{ key = 'audio.render.44100-test:friendly-name'; value = 'Test Endpoint' }
        [pscustomobject]@{ key = 'audio.render.44100-test:device-format'; value = '44100/24/2' }
        [pscustomobject]@{ key = 'audio.render.44100-test:default-roles'; value = 'console,multimedia' }
        [pscustomobject]@{ key = 'audio.render.normal:friendly-name'; value = 'Speakers' }
    )
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-FORMAT-001' -EnvctlProperties $properties `
        -Tools @{ soundvolumeview = 'C:\tools\SoundVolumeView.exe' } -Responses @{
        ffprobe            = (New-DryRunFfprobe -Directory $root)
        'record.snapshot'  = [pscustomobject]@{ systemAudioEnabled = $true }
        'record.result'    = [pscustomobject]@{ succeeded = $true; outputPath = (Join-Path $root 'out.mkv') }
        'session.latest'   = (New-FakeSessionReport)
    }
    Assert-Equal 'PASS' $result.Result.Result "the tool path must reach a pass: $($result.Result.Message)"
    Assert-Equal 0 $result.Prompts.Count 'nobody is asked to set a default playback device any more'
    $calls = @($result.Invocations | Where-Object { $_.Tool -eq 'soundvolumeview' })
    Assert-True (@($calls | Where-Object { $_.Arguments -contains '/SetDefaultFormat' }).Count -ge 1) `
        'the shared-mode format is set by the tool'
    Assert-True (@($calls | Where-Object { $_.Arguments -contains '/SetDefault' }).Count -ge 1) `
        'and so is the default render role'
}

Test-Case 'REL-AUD-FORMAT-001 is red when the endpoint holds no default role' {
    # Without this assertion the gate cannot fail at all: system audio is captured
    # from the DEFAULT endpoint, so a 44.1 kHz device that is not default is never in
    # the recorded path. It read green in every campaign up to rc17 that way.
    $properties = @(
        [pscustomobject]@{ key = 'audio.render.44100-test:friendly-name'; value = 'Test Endpoint' }
        [pscustomobject]@{ key = 'audio.render.44100-test:device-format'; value = '44100/24/2' }
        [pscustomobject]@{ key = 'audio.render.44100-test:default-roles'; value = '' }
    )
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-FORMAT-001' -EnvctlProperties $properties `
        -Tools @{ soundvolumeview = 'C:\tools\SoundVolumeView.exe' }
    Assert-Equal 'FAIL' $result.Result.Result 'a format nothing records through is not a pass'
    Assert-True ($result.Result.Message -match 'no default render role') $result.Result.Message
}

Test-Case 'REL-AUD-DEGRADE-001 is red without a recovery and green with one' {
    $variables = @{ EXOSNAP_AUDIO_DEVICE_INSTANCE_ID = 'SWD\MMDEVAPI\{0.0.0}'; EXOSNAP_ENDPOINT_VISIBILITY_TOOL = '' }
    $properties = @([pscustomobject]@{ key = 'audio.render.normal:friendly-name'; value = 'Speakers' })
    # RED: the device goes and never comes back, which fails the same assertion as a
    # device that never went.
    $red = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-DEGRADE-001' -Elevated -Variables $variables `
        -EnvctlProperties $properties -Tools @{ pnputil = 'C:\Windows\System32\pnputil.exe' } -Responses @{
        'record.snapshot'   = [pscustomobject]@{ systemAudioEnabled = $true }
        'pipeline.snapshot' = @((New-DryRunPipelineSnapshot -Degraded $true))
    }
    Assert-Equal 'FAIL' $red.Result.Result "a degradation that never cleared is a defect: $($red.Result.Message)"
    Assert-True ($red.Result.Message -match 'never cleared') $red.Result.Message

    # GREEN: degraded, then recovered, still recording.
    $green = Invoke-ReleaseDryRun -ScenarioId 'REL-AUD-DEGRADE-001' -Elevated -Variables $variables `
        -EnvctlProperties $properties -Tools @{ pnputil = 'C:\Windows\System32\pnputil.exe' } -Responses @{
        'record.snapshot'   = [pscustomobject]@{ systemAudioEnabled = $true }
        'pipeline.snapshot' = @(
            (New-DryRunPipelineSnapshot -Degraded $true),
            (New-DryRunPipelineSnapshot -Degraded $false)
        )
    }
    Assert-Equal 'PASS' $green.Result.Result "degraded then recovered is the contract: $($green.Result.Message)"
    Assert-Equal 0 $green.Prompts.Count 'pnputil removes the device, so nobody unplugs anything'
    Assert-True ($green.Result.Message -match '\[pnputil\]') $green.Result.Message
}

Test-Case 'REL-UPD-MSI-DECLINE-001 runs in a sandbox and is red on a stranded install' {
    $steps = @{ 'install-base' = $true; 'select-channel' = $true; 'decline-offer' = $true
        'decline-apply'        = $true; 'decline-state' = $false; 'decline-updater-closed' = $true
    }
    $red = Invoke-DryRunSandboxUpdateGate -ScenarioId 'REL-UPD-MSI-DECLINE-001' -Steps $steps
    Assert-Equal 'FAIL' $red.Result.Result "a wrong failureCase is a product defect: $($red.Result.Message)"
    Assert-True ($red.Result.Message -match 'decline-state') $red.Result.Message

    $steps['decline-state'] = $true
    $green = Invoke-DryRunSandboxUpdateGate -ScenarioId 'REL-UPD-MSI-DECLINE-001' -Steps $steps
    Assert-Equal 'PASS' $green.Result.Result $green.Result.Message
    Assert-Equal 0 $green.Prompts.Count 'nobody clicks a Secure Desktop prompt in the automated campaign'
}

Test-Case 'REL-UPD-MSI-DECLINE-001 is unverified when the worker never reached a step' {
    # A step the worker never got to must not read as a pass over a shorter list.
    $result = Invoke-DryRunSandboxUpdateGate -ScenarioId 'REL-UPD-MSI-DECLINE-001' `
        -Steps @{ 'install-base' = $true; 'select-channel' = $true }
    Assert-Equal 'UNVERIFIED' $result.Result.Result $result.Result.Message
    Assert-True ($result.Result.Message -match 'never reached') $result.Result.Message
}

Test-Case 'REL-UPD-MSI-001 reads the same rehearsal and requires the updater to have been gone' {
    # The second rc19 defect: the declined update left the updater holding
    # exosnap-updater.exe, so the accept could not stage its own and failed with
    # "Failed to stage updater file" -- reported as an MSI failure.
    $steps = @{ 'install-base' = $true; 'updater-gone-before-accept' = $false; 'accept-offer' = $true
        'accept-apply'         = $true; 'accept-installed' = $true
    }
    $red = Invoke-DryRunSandboxUpdateGate -ScenarioId 'REL-UPD-MSI-001' -Steps $steps
    Assert-Equal 'FAIL' $red.Result.Result "a held updater file must fail here, not later: $($red.Result.Message)"
    Assert-True ($red.Result.Message -match 'updater-gone-before-accept') $red.Result.Message

    $steps['updater-gone-before-accept'] = $true
    $green = Invoke-DryRunSandboxUpdateGate -ScenarioId 'REL-UPD-MSI-001' -Steps $steps
    Assert-Equal 'PASS' $green.Result.Result $green.Result.Message
    Assert-Equal 0 $green.Prompts.Count 'an elevated sandbox raises no prompt'
}

Test-Case 'the update gates say what to install when no sandbox and no base MSI are there' {
    # Never green without evidence: a missing precondition is UNAVAILABLE with the
    # exact thing somebody has to install, not a pass and not a failure.
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-UPD-MSI-DECLINE-001' `
        -Tools @{ sandbox = $null } -Variables @{ EXOSNAP_UPDATE_FROM = ''; EXOSNAP_UPDATE_FROM_MSI = '' }
    Assert-Equal 'UNAVAILABLE' $result.Result.Result $result.Result.Message
    Assert-Equal 0 $result.Prompts.Count 'nobody is asked to perform a gate that cannot be verified'
}

Test-Case 'REL-PKG-CHOCO-001 stages a real sandbox run and judges its result document' {
    # The staging, the .wsb and the result transport are exercised for real; only
    # the virtual machine itself is simulated, by a launcher that writes what the
    # worker would have written.
    $red = Invoke-DryRunChocolateyGate -Steps @(
        @{ name = 'prepare'; ok = $true }, @{ name = 'pack'; ok = $true },
        @{ name = 'removeExisting'; ok = $true },
        @{ name = 'install'; ok = $false; detail = 'no start-menu shortcut' },
        @{ name = 'uninstall'; ok = $true }, @{ name = 'restore'; ok = $true }) -RestoreRan $true
    Assert-Equal 'FAIL' $red.Result.Result $red.Result.Message
    Assert-True ($red.Result.Message -match 'start-menu shortcut') $red.Result.Message

    $green = Invoke-DryRunChocolateyGate -Steps @(
        @{ name = 'prepare'; ok = $true }, @{ name = 'pack'; ok = $true },
        @{ name = 'removeExisting'; ok = $true }, @{ name = 'install'; ok = $true },
        @{ name = 'uninstall'; ok = $true }, @{ name = 'restore'; ok = $true }) -RestoreRan $true
    Assert-Equal 'PASS' $green.Result.Result $green.Result.Message
    Assert-Equal 0 $green.Prompts.Count 'the sandbox worker is already elevated; there is no prompt to accept'
    Assert-True ($green.Result.Message -match 'no prompt was raised') $green.Result.Message
}

Test-Case 'REL-VIS-OVERLAY-001 is red on a wrong appearance and green when both are judged right' {
    $overlays = [pscustomobject]@{ overlays = @([pscustomobject]@{ name = 'badge'; visible = $true }) }
    $responses = @{
        'overlay.snapshot' = $overlays
        'settings.get'     = [pscustomobject]@{ 'app.showQuickControls' = $false }
        'app.identity'     = [pscustomobject]@{ pid = 4242; productVersion = '0.9.0' }
        uia                = @{ Ok = $true; Detail = 'simulated'; Elements = @([pscustomobject]@{ Name = 'Recording'; ClassName = 'Qt'; ControlType = 'Window' }) }
    }
    # RED: the operator sees something wrong in Light, and the reason travels with
    # the judgement rather than being asked for again afterwards.
    # Both appearances are still judged after a wrong one: which of the two is
    # broken is part of the finding, and a gate that stopped at the first would
    # report half of it.
    $answers = [System.Collections.Generic.Queue[string]]::new()
    @('n', 'the badge went white in Light', 'j') | ForEach-Object { $answers.Enqueue($_) }
    $red = Invoke-ReleaseDryRun -ScenarioId 'REL-VIS-OVERLAY-001' -Responses $responses `
        -Operator { param($p) return $answers.Dequeue() }.GetNewClosure()
    Assert-Equal 'FAIL' $red.Result.Result $red.Result.Message
    Assert-True ($red.Result.Message -match 'went white in Light') "the reason must reach the report: $($red.Result.Message)"

    $green = Invoke-ReleaseDryRun -ScenarioId 'REL-VIS-OVERLAY-001' -Responses $responses `
        -Operator { param($p) return 'j' }
    Assert-Equal 'PASS' $green.Result.Result $green.Result.Message
    Assert-Equal 2 $green.Prompts.Count 'one judgement per appearance, asked while that appearance is on screen'
    foreach ($prompt in $green.Prompts) {
        Assert-True ($prompt -match '\[j\] richtig') "a sight check is never answered with Enter: $prompt"
    }
}

Test-Case 'REL-VIS-OVERLAY-001 fails when the overlays never reached the desktop' {
    # overlay.snapshot is OUR account of what we asked for. UI Automation is the one
    # reader that survives WDA_EXCLUDEFROMCAPTURE, so the two stop being the same
    # sentence.
    $result = Invoke-ReleaseDryRun -ScenarioId 'REL-VIS-OVERLAY-001' -Operator { param($p) return 'j' } -Responses @{
        'overlay.snapshot' = [pscustomobject]@{ overlays = @([pscustomobject]@{ name = 'badge'; visible = $true }) }
        'settings.get'     = [pscustomobject]@{ 'app.showQuickControls' = $false }
        'app.identity'     = [pscustomobject]@{ pid = 4242 }
        uia                = @{ Ok = $true; Detail = 'simulated'; Elements = @() }
    }
    Assert-Equal 'FAIL' $result.Result.Result $result.Result.Message
    Assert-True ($result.Result.Message -match 'did not reach the desktop') $result.Result.Message
}

Test-Case 'REL-VIS-NOTIFY-001 is red when the toast never reached the desktop and green when it did' {
    $entries = [pscustomobject]@{ entries = @([pscustomobject]@{ sequence = 7; title = 'Window capture appears to have stalled' }) }
    $base = @{
        'notifications.snapshot' = @([pscustomobject]@{ entries = @() }, $entries)
        'app.identity'           = [pscustomobject]@{ pid = 4242 }
    }
    $red = Invoke-ReleaseDryRun -ScenarioId 'REL-VIS-NOTIFY-001' -Operator { param($p) return 'j' } `
        -Responses ($base + @{ uia = @{ Ok = $true; Detail = 'simulated'; Elements = @([pscustomobject]@{ Name = 'ExoSnap'; ClassName = 'Qt'; ControlType = 'Window' }) } })
    Assert-Equal 'FAIL' $red.Result.Result $red.Result.Message
    Assert-True ($red.Result.Message -match 'never reached the desktop') $red.Result.Message

    $green = Invoke-ReleaseDryRun -ScenarioId 'REL-VIS-NOTIFY-001' -Operator { param($p) return 'j' } `
        -Responses ($base + @{ uia = @{ Ok = $true; Detail = 'simulated'; Elements = @(
                    [pscustomobject]@{ Name = 'Window capture appears to have stalled'; ClassName = 'Qt'; ControlType = 'Text' }) } })
    Assert-Equal 'PASS' $green.Result.Result $green.Result.Message
    Assert-Equal 1 $green.Prompts.Count 'one judgement, and only the tint is being judged'
    Assert-True ($green.Prompts[0] -match '\[j\] richtig') $green.Prompts[0]
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
