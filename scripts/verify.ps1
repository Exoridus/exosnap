#Requires -Version 7.0
<#
.SYNOPSIS
    The single local entry point for verifying a change.

.DESCRIPTION
    Two contracts:

      -Fast   The cheap relevant checks, scoped conservatively to what changed.
              Made for pre-commit. It may check more than it strictly had to; it
              may never be falsely safe, and it never claims to be -Full.

      -Full   Every gate that blocks locally today, at full scope. Made for
              pre-push. By construction a superset of -Fast: same plan builder,
              scope forced to "everything".

    What this script owns is ORDER and DEPENDENCY, not the checks themselves.
    Every step calls the script that already owned it -- check-format.ps1,
    check-quality.ps1, run-tests.ps1, run-clang-tidy-blocking.ps1, check-drift.ps1
    -- so there is exactly one definition of each gate.

    Two properties are the reason it exists at all:

    1. Build and test are one evidence chain. A test run that is invoked
       independently of the build it depends on can pass against binaries that
       predate the source. That happened three times during the Qt 6.11 uplift:
       once a false green, twice a false red. Here `tests` depends on `build`, and
       a dependency that did not pass yields SKIPPED_DEPENDENCY, never PASS.

    2. Fail fast, cheapest first. A whitespace error does not wait behind a
       compile, and nothing downstream of a failure is started.

    Packaging (ZIP/MSI/updater) is deliberately NOT part of -Full: the local
    blocking contract never contained it, and inventing an MSI build on every push
    would be a new cost, not a preserved gate. CI and release-candidate.yml own it.

.PARAMETER Fast
    Run the fast, scoped contract.

.PARAMETER Full
    Run the complete local blocking contract.

.PARAMETER Staged
    Scope the change set to what is staged, and let clang-format fix and re-stage.
    This is what the pre-commit hook passes.

.PARAMETER Base
    Commit to diff against for the change set. Defaults to the merge base with
    origin/main, then origin/HEAD, then HEAD.

.PARAMETER DryRun
    Do not execute anything: report the plan with every executed check forced to
    the outcome given by -SimulatePass/-SimulateFail. This is how the pipeline
    itself is tested without a compiler.

.PARAMETER SimulateFail
    In -DryRun, the check names that should report FAIL.

.EXAMPLE
    .\scripts\verify.ps1 -Fast

.EXAMPLE
    .\scripts\verify.ps1 -Full
#>

[CmdletBinding(DefaultParameterSetName = 'Fast')]
param(
    [Parameter(ParameterSetName = 'Fast')] [switch] $Fast,
    [Parameter(ParameterSetName = 'Full')] [switch] $Full,
    [switch] $Staged,
    [string] $Base,
    [string] $Preset = 'windows-x64-debug',
    [string] $Config = 'Debug',
    [switch] $DryRun,
    [string[]] $SimulateFail = @(),
    [string] $ResultPath,
    [int] $FailureTailLines = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'lib/VerifyPipeline.psm1') -Force -DisableNameChecking
Import-Module (Join-Path $PSScriptRoot 'lib/MsvcEnvironment.psm1') -Force -DisableNameChecking

# Before anything else, and before any child process is started: see the function
# for what a leaked hook git environment does to a repository.
Clear-InheritedGitEnvironment

$status = Get-VerifyStatusName
$mode = if ($Full) { 'Full' } else { 'Fast' }
$buildDir = "build/$Preset"
$logRoot = Join-Path $repoRoot '.workspace/verify'
if (-not $ResultPath) { $ResultPath = Join-Path $logRoot 'latest.json' }

# ---------------------------------------------------------------------------
# Repository facts
# ---------------------------------------------------------------------------

function Invoke-Git {
    param([string[]] $GitArgs)
    $output = & git -C $repoRoot @GitArgs 2>$null
    return @($output)
}

$head = (Invoke-Git @('rev-parse', 'HEAD') | Select-Object -First 1)
$dirty = @(Invoke-Git @('status', '--porcelain')).Count -gt 0

if (-not $Base) {
    foreach ($candidate in @('origin/main', 'origin/HEAD', 'main')) {
        $merge = (Invoke-Git @('merge-base', $candidate, 'HEAD') | Select-Object -First 1)
        if ($merge) { $Base = $merge; break }
    }
}

$changed = Get-VerifyChangedFile -RepoRoot $repoRoot -Base $Base -Staged:$Staged
$scope = if ($mode -eq 'Full') { Get-VerifyScope -ChangedFiles @() } else { Get-VerifyScope -ChangedFiles $changed }

$plan = New-VerifyPlan -Mode $mode -Scope $scope -BuildDir $buildDir -Preset $Preset -Config $Config

# ---------------------------------------------------------------------------
# Running a step
# ---------------------------------------------------------------------------

$script:LastFailedTests = @()
$script:MsvcEnvironmentReady = $false

function Test-PresetUsesNinja {
    <#
    .SYNOPSIS
        Whether a configure preset builds with Ninja, following inherits.
    #>
    param([Parameter(Mandatory)] [string] $Name)

    $presets = (Get-Content -LiteralPath (Join-Path $repoRoot 'CMakePresets.json') -Raw |
        ConvertFrom-Json).configurePresets
    # Bounded rather than while($true): a cycle in inherits is a broken presets
    # file, and hanging the whole pipeline is a worse way to report it.
    for ($hop = 0; $hop -lt 16 -and $Name; $hop++) {
        $preset = $presets | Where-Object { $_.name -eq $Name } | Select-Object -First 1
        if (-not $preset) { return $false }
        if ($preset.PSObject.Properties.Name -contains 'generator' -and $preset.generator) {
            return $preset.generator -eq 'Ninja'
        }
        $Name = if ($preset.PSObject.Properties.Name -contains 'inherits') { @($preset.inherits)[0] } else { $null }
    }
    return $false
}

function Initialize-CompilerEnvironment {
    <#
    .SYNOPSIS
        Makes cl.exe reachable before the first step that needs a compiler.
    .DESCRIPTION
        The Visual Studio generator locates its own toolchain; Ninja does not, and
        a plain PowerShell has no cl.exe on PATH. Doing this here rather than
        asking the developer for a Developer PowerShell is what makes the fast
        preset usable from the shell they already have open. The import applies to
        this process only and is a no-op inside a Developer PowerShell.
    #>
    if ($script:MsvcEnvironmentReady) { return }
    $script:MsvcEnvironmentReady = $true
    if (-not (Test-PresetUsesNinja -Name $Preset)) { return }
    Enter-MsvcEnvironment | Out-Null
}

function Invoke-Step {
    <#
    .SYNOPSIS
        Runs one external command and reports PASS/FAIL plus its captured output.
    #>
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $FilePath,
        [string[]] $Arguments = @(),
        [string] $WorkingDirectory = $repoRoot
    )

    if (-not (Test-Path -LiteralPath $logRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
    }
    $logPath = Join-Path $logRoot "$Name.log"

    # Captured, not streamed. A pre-commit hook that prints every line of every
    # passing step buries the summary it exists to produce; the full output is on
    # disk either way, and a failure prints its tail immediately below.
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments *>&1 | Set-Content -LiteralPath $logPath -Encoding utf8
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if ($null -eq $code) { $code = 0 }
    if ($code -eq 0) {
        return @{ Status = $status.Pass; Evidence = @{ log = $logPath } }
    }

    Write-Host ""
    Write-Host "---- $Name output (last $FailureTailLines lines) ----"
    Get-Content -LiteralPath $logPath -Tail $FailureTailLines -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host $_ }
    Write-Host "Full log: $logPath"
    return @{ Status = $status.Fail; Detail = "exit $code"; Evidence = @{ log = $logPath } }
}

function Get-FailedCTestName {
    param([string] $LogPath)
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return @() }
    # ctest's failure summary lines look like:  "  12 - quick.qml.record_controls (Failed)"
    return @(Get-Content -LiteralPath $LogPath |
            ForEach-Object {
                if ($_ -match '^\s*\d+\s+-\s+(\S+)\s+\((Failed|Timeout|Subprocess aborted)') { $Matches[1] }
            } | Sort-Object -Unique)
}

$realExecutor = {
    param($check, $context)

    switch ($check.Kind) {
        'sanity' {
            $problems = @()
            if (-not $head) { $problems += 'HEAD could not be resolved (not a git repository?)' }
            if (-not (Test-Path -LiteralPath (Join-Path $repoRoot '.qt-version') -PathType Leaf)) {
                $problems += '.qt-version is missing'
            }
            if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'CMakePresets.json') -PathType Leaf)) {
                $problems += 'CMakePresets.json is missing'
            }
            if ($problems.Count -gt 0) {
                return @{ Status = $status.Fail; Detail = ($problems -join '; ') }
            }
            $where = if ($dirty) { 'working tree dirty' } else { 'working tree clean' }
            return @{ Status = $status.Pass; Detail = "$($head.Substring(0, 8)), $where" }
        }

        'diff' {
            $ranges = @(, @('diff', '--check'))
            $ranges += , @('diff', '--cached', '--check')
            if ($mode -eq 'Full' -and $Base) { $ranges += , @('diff', '--check', "$Base...HEAD") }
            foreach ($range in $ranges) {
                $null = Invoke-Git $range
                if ($LASTEXITCODE -ne 0) {
                    return @{ Status = $status.Fail; Detail = "git $($range -join ' ') reported whitespace or conflict-marker damage" }
                }
            }
            return @{ Status = $status.Pass }
        }

        'drift' {
            return Invoke-Step -Name 'drift' -FilePath 'pwsh' -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'check-drift.ps1'))
        }

        'source-hygiene' {
            # -Base HEAD scopes this to the work in front of the developer: the
            # staged and working-tree changes, not the whole branch. That is what
            # makes the rules adoptable at all. The branch-wide sweep
            # (check-source-hygiene.ps1, or -All for every tracked file) is a
            # known backlog on this tree; widening the scope here is a one-word
            # change once it is cleared.
            return Invoke-Step -Name 'source-hygiene' -FilePath 'pwsh' -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'check-source-hygiene.ps1'),
                '-Base', 'HEAD')
        }

        'format' {
            $formatArgs = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'check-format.ps1'))
            if ($Staged) { $formatArgs += @('-Staged', '-Fix') }
            return Invoke-Step -Name 'format' -FilePath 'pwsh' -Arguments $formatArgs
        }

        'script-tests' {
            $tests = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'tests') -Filter '*.tests.ps1' -File |
                    Sort-Object Name)
            foreach ($test in $tests) {
                $outcome = Invoke-Step -Name "script-tests.$($test.BaseName)" -FilePath 'pwsh' `
                    -Arguments @('-NoProfile', '-NonInteractive', '-File', $test.FullName)
                if ($outcome.Status -ne $status.Pass) {
                    return @{ Status = $status.Fail; Detail = "$($test.Name) failed"; Evidence = $outcome.Evidence }
                }
            }
            return @{ Status = $status.Pass; Detail = "$($tests.Count) script test file(s)" }
        }

        'configure' {
            Initialize-CompilerEnvironment
            return Invoke-Step -Name 'configure' -FilePath 'cmake' -Arguments @('--preset', $Preset)
        }

        'qmllint' {
            # The CMake target, never a bare qmllint call: a hand-rolled invocation
            # on this repository reports resolution failures the target does not.
            Initialize-CompilerEnvironment
            return Invoke-Step -Name 'qmllint' -FilePath 'cmake' `
                -Arguments @('--build', $buildDir, '--target', 'all_qmllint')
        }

        'build' {
            # Also here, not only in 'configure': a plan that reuses an existing
            # build directory does not reconfigure, and the compiler is needed
            # either way.
            Initialize-CompilerEnvironment
            return Invoke-Step -Name 'build' -FilePath 'cmake' -Arguments @('--build', '--preset', $Preset)
        }

        'tests' {
            $testArgs = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'run-tests.ps1'),
                '-BuildDir', $buildDir, '-Config', $Config)
            if ($check.Evidence.filter) { $testArgs += @('-Filter', $check.Evidence.filter) }
            $outcome = Invoke-Step -Name 'tests' -FilePath 'pwsh' -Arguments $testArgs
            if ($outcome.Status -ne $status.Pass) {
                $script:LastFailedTests = Get-FailedCTestName -LogPath $outcome.Evidence.log
                $outcome.Detail = "$($outcome.Detail); failed: $($script:LastFailedTests -join ', ')"
            }
            return $outcome
        }

        'cppcheck' {
            return Invoke-Step -Name 'cppcheck' -FilePath 'pwsh' -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'check-quality.ps1'),
                '-Only', 'cppcheck')
        }

        'clang-tidy' {
            return Invoke-Step -Name 'clang-tidy' -FilePath 'pwsh' -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'check-quality.ps1'),
                '-Only', 'clang-tidy')
        }

        'clang-tidy-blocking' {
            return Invoke-Step -Name 'clang-tidy-blocking' -FilePath 'pwsh' -Arguments @(
                '-NoProfile', '-NonInteractive', '-File', (Join-Path $PSScriptRoot 'run-clang-tidy-blocking.ps1'),
                '-BuildDir', $buildDir)
        }

        default { throw "verify.ps1 has no executor for check kind '$($check.Kind)'." }
    }
}

$dryRunExecutor = {
    param($check, $context)
    if ($SimulateFail -contains $check.Name) {
        # A simulated test failure has to carry simulated failing tests, otherwise
        # the QML diagnostic contract would look satisfied for the wrong reason.
        if ($check.Kind -eq 'tests') { $script:LastFailedTests = @('quick.qml.record_controls') }
        return @{ Status = $status.Fail; Detail = 'simulated failure' }
    }
    return @{ Status = $status.Pass; Detail = 'simulated' }
}

# A failing QML test that reports only an exit code is not a test report. Re-run
# the QuickTest binaries behind the failing ctest names with -o <file>,txt so the
# per-function diagnosis exists as evidence rather than as something to rediscover.
$diagnosticProvider = {
    param($check, $outcome)

    $commands = Resolve-QmlDiagnosticCommand -BuildDir (Join-Path $repoRoot $buildDir) `
        -LogDirectory $logRoot -Config $Config -FailedTestNames $script:LastFailedTests
    $produced = @()
    foreach ($command in $commands) {
        if ($DryRun) {
            $produced += $command.OutputPath
            continue
        }
        if (-not (Test-Path -LiteralPath $command.FilePath -PathType Leaf)) { continue }
        & $command.FilePath @($command.Arguments) *> $null
        if (Test-Path -LiteralPath $command.OutputPath -PathType Leaf) {
            $produced += $command.OutputPath
            Write-Host ""
            Write-Host "---- $($command.TestName) (QuickTest report) ----"
            Get-Content -LiteralPath $command.OutputPath -Tail 60 | ForEach-Object { Write-Host $_ }
        }
    }
    return @($produced)
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "verify ($mode) — HEAD $(if ($head) { $head.Substring(0, 8) } else { '?' })$(if ($dirty) { ' +dirty' })"
if ($mode -eq 'Fast') {
    Write-Host "  scope: $(if ($scope.Categories) { $scope.Categories -join ', ' } else { 'nothing' }) ($($scope.ChangedFiles.Count) file(s))"
    foreach ($reason in $scope.EscalationReasons) { Write-Host "  escalated: $reason" }
}
else {
    Write-Host '  scope: everything (this mode is the full local blocking contract)'
}
Write-Host ""

$run = Invoke-VerifyPlan -Plan $plan -Executor $(if ($DryRun) { $dryRunExecutor } else { $realExecutor }) `
    -DiagnosticProvider $diagnosticProvider

Write-Host ""
foreach ($line in (New-VerifySummary -Run $run)) { Write-Host $line }

$saved = Save-VerifyResult -Run $run -Path $ResultPath -Head $head -Base $Base -Dirty $dirty -Scope $scope
Write-Host ""
Write-Host "summary: $saved"

if ($run.result -ne 'passed') {
    Write-Host "verify ($mode): FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "verify ($mode): passed" -ForegroundColor Green
exit 0
