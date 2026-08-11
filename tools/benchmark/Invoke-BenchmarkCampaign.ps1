<#
.SYNOPSIS
    Runs the alternating Widgets-vs-Quick campaign for one scenario.

.DESCRIPTION
    The order is alternated (W Q Q W W Q by default) rather than blocked, so a
    thermal drift or a background task that builds up over the session lands on
    both frontends instead of on whichever went second.

    Every run goes through Invoke-BenchmarkRun.ps1, which is the only place that
    knows how to launch anything. A run that fails its acceptance check stops the
    campaign: continuing would leave an unbalanced set, and an unbalanced set is
    worse than a short one.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Scenario,
    [string[]]$Order = @('widgets', 'quick', 'quick', 'widgets', 'widgets', 'quick'),
    [int]$CooldownSeconds = 20,
    # Forwarded verbatim. Not forwarding them was how a campaign silently ran the
    # SHIPPING build: the run script's own defaults applied instead, and a binary
    # without the harness just starts as the normal application.
    [string]$WidgetsExe,
    [string]$QuickExe,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\.workspace\benchmark-results'),
    [switch]$Calibration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$runScript = Join-Path $PSScriptRoot 'Invoke-BenchmarkRun.ps1'
$counters = @{ widgets = 0; quick = 0 }

for ($i = 0; $i -lt $Order.Count; $i++) {
    $frontend = $Order[$i]
    $counters[$frontend]++
    Write-Host ''
    Write-Host ("=== [{0}/{1}] {2} run {3} ===" -f ($i + 1), $Order.Count, $frontend, $counters[$frontend])

    $arguments = @{
        Frontend   = $frontend
        Scenario   = $Scenario
        RunIndex   = $counters[$frontend]
        OutputRoot = $OutputRoot
    }
    if ($Calibration) { $arguments.Calibration = $true }
    if ($WidgetsExe) { $arguments.WidgetsExe = $WidgetsExe }
    if ($QuickExe) { $arguments.QuickExe = $QuickExe }

    # The run script throws when a run is not acceptable; $ErrorActionPreference =
    # 'Stop' lets that abort the campaign. An unbalanced set is worse than a short
    # one, so there is no continue-on-error path here.
    & $runScript @arguments

    if ($i -lt $Order.Count - 1) {
        # Let the GPU and the encoder settle. Without it the second run of a pair
        # starts warmer than the first and the order bias the alternation is meant
        # to cancel comes back in through the back door.
        Write-Host "Cooldown ${CooldownSeconds}s..."
        Start-Sleep -Seconds $CooldownSeconds
    }
}

Write-Host ''
Write-Host 'Campaign complete. Build the comparison with:'
Write-Host "  .\Compare-BenchmarkRuns.ps1 -Scenario $Scenario"
