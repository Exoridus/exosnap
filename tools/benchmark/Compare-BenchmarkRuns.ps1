<#
.SYNOPSIS
    Builds the Widgets-vs-Quick comparison dataset for one scenario.

.DESCRIPTION
    Reads every run directory under a scenario, rejects the ones that are not
    comparable, and emits a table plus a machine-readable dataset.

    Two rules do the real work here:

      * A pair is only compared when every accepted run shares the same
        effective_recording_config.fingerprint. Matching command lines are not
        evidence; the fingerprint is read back from what the engine was handed.

      * A delta is only computed for metrics whose comparability is "identical".
        "approximate" metrics are printed side by side with their probe text, and
        "frontend_only" metrics are never subtracted at all. A Widgets preview
        "frame" is a swap-chain Present of one quad; a Quick preview "frame" is a
        scene-graph render of the whole window. Subtracting those two produces a
        number that looks like an answer and is not one.

.NOTES
    Development tooling. Read-only over the artifact tree.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Scenario,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\.workspace\benchmark-results'),
    [string]$ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scenarioDir = Join-Path $OutputRoot $Scenario
if (-not (Test-Path $scenarioDir)) { throw "No results at $scenarioDir." }
if (-not $ReportPath) { $ReportPath = Join-Path $scenarioDir 'comparison.json' }
# Belt and braces after the shadowing bug below: the output must never land inside
# a run directory, whatever the caller passes.
# Both sides normalised: the caller's path routinely carries ..\.. segments, and
# comparing those against a resolved path rejects perfectly valid input.
$scenarioDirFull = (Resolve-Path $scenarioDir).Path.TrimEnd('\')
$reportParentFull = (Split-Path -Parent ([System.IO.Path]::GetFullPath($ReportPath))).TrimEnd('\')
if ($reportParentFull -ne $scenarioDirFull) {
    throw "Refusing to write the comparison to $ReportPath; it must live directly under $scenarioDirFull."
}

# ---------------------------------------------------------------------------
# Load runs
# ---------------------------------------------------------------------------
$runs = @()
foreach ($dir in Get-ChildItem -Path $scenarioDir -Directory) {
    $manifestPath = Join-Path $dir.FullName 'run.json'
    if (-not (Test-Path $manifestPath)) { continue }
    $manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
    if (-not $manifest.exosnap_report) {
        Write-Warning "$($dir.Name): no ExoSnap report; excluded."
        continue
    }
    # NOT $reportPath: PowerShell variable names are case-insensitive, so that name
    # aliases the $ReportPath parameter — and this script then wrote its comparison
    # dataset over the last run's raw benchmark JSON, destroying an archived
    # artifact the campaign exists to preserve.
    $runReportPath = Join-Path $dir.FullName $manifest.exosnap_report
    $report = Get-Content -Raw $runReportPath | ConvertFrom-Json

    $reasons = @()
    if ($manifest.calibration) { $reasons += 'calibration run' }
    if (-not $manifest.topology_verified) { $reasons += 'topology unverified' }
    if ($manifest.exosnap_exit_code -ne 0) { $reasons += "exit code $($manifest.exosnap_exit_code)" }
    if (-not $report.outcome.succeeded) { $reasons += 'recording did not succeed' }
    if ($report.environment.build_config -ne 'Release') {
        $reasons += "build config $($report.environment.build_config) (accepted runs must be Release)"
    }
    if (-not $report.effective_recording_config.available) { $reasons += 'no effective config fingerprint' }

    $runs += [pscustomobject]@{
        RunId       = $manifest.run_id
        Frontend    = $manifest.frontend
        Directory   = $dir.FullName
        Manifest    = $manifest
        Report      = $report
        Fingerprint = if ($report.effective_recording_config.available) { $report.effective_recording_config.fingerprint } else { $null }
        Accepted    = ($reasons.Count -eq 0)
        Rejections  = $reasons
    }
}

foreach ($run in $runs | Where-Object { -not $_.Accepted }) {
    Write-Warning ("{0}: excluded ({1})" -f $run.RunId, ($run.Rejections -join '; '))
}
$accepted = @($runs | Where-Object { $_.Accepted })

# ---------------------------------------------------------------------------
# Comparability gate
# ---------------------------------------------------------------------------
$fingerprints = @($accepted.Fingerprint | Sort-Object -Unique)
if ($fingerprints.Count -gt 1) {
    Write-Host ''
    Write-Host 'Effective recording configurations DIVERGE across accepted runs:' -ForegroundColor Red
    foreach ($fp in $fingerprints) {
        $members = ($accepted | Where-Object { $_.Fingerprint -eq $fp }).RunId -join ', '
        Write-Host "  $fp : $members"
    }
    # Show the first divergent field so the operator does not have to diff by hand.
    $reference = ($accepted | Where-Object { $_.Fingerprint -eq $fingerprints[0] } | Select-Object -First 1)
    foreach ($fp in $fingerprints | Select-Object -Skip 1) {
        $other = ($accepted | Where-Object { $_.Fingerprint -eq $fp } | Select-Object -First 1)
        $diff = Compare-Object $reference.Report.effective_recording_config.fields `
                               $other.Report.effective_recording_config.fields
        foreach ($entry in $diff) {
            Write-Host ("  {0} {1}" -f $entry.SideIndicator, $entry.InputObject)
        }
    }
    throw 'Refusing to produce a comparison: the two sides did not record the same thing.'
}

$widgets = @($accepted | Where-Object { $_.Frontend -eq 'widgets' })
$quick = @($accepted | Where-Object { $_.Frontend -eq 'quick' })
Write-Host ("Accepted: {0} widgets, {1} quick. Effective config fingerprint: {2}" -f `
    $widgets.Count, $quick.Count, ($fingerprints | Select-Object -First 1))

# ---------------------------------------------------------------------------
# Metric extraction
# ---------------------------------------------------------------------------
function Get-MetricSet {
    param($Report)
    $set = [ordered]@{}
    foreach ($group in 'preview', 'recording', 'process') {
        if (-not $Report.PSObject.Properties.Name.Contains($group)) { continue }
        foreach ($property in $Report.$group.PSObject.Properties) {
            $set["$group.$($property.Name)"] = $property.Value
        }
    }
    return $set
}

function Get-Stats {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return $null }
    $mean = ($Values | Measure-Object -Average).Average
    $min = ($Values | Measure-Object -Minimum).Minimum
    $max = ($Values | Measure-Object -Maximum).Maximum
    # Spread relative to the mean. A delta smaller than the run-to-run spread is
    # not a finding, and the report has to make that visible instead of implying
    # precision the sample size does not support.
    $spread = if ($mean -ne 0) { ($max - $min) / [math]::Abs($mean) * 100.0 } else { 0.0 }
    return [pscustomobject]@{
        Mean = $mean; Min = $min; Max = $max; SpreadPercent = $spread; N = $Values.Count
    }
}

$metricNames = @()
foreach ($run in $accepted) { $metricNames += (Get-MetricSet $run.Report).Keys }
$metricNames = $metricNames | Sort-Object -Unique

$rows = @()
foreach ($name in $metricNames) {
    $comparability = $null
    $probeWidgets = $null
    $probeQuick = $null
    $widgetsValues = @()
    $quickValues = @()

    foreach ($run in $accepted) {
        $metric = (Get-MetricSet $run.Report)[$name]
        if ($null -eq $metric) { continue }
        if (-not $comparability) { $comparability = $metric.comparability }
        if ($null -ne $metric.value) {
            if ($run.Frontend -eq 'widgets') {
                $widgetsValues += [double]$metric.value
                if (-not $probeWidgets) { $probeWidgets = $metric.probe }
            } else {
                $quickValues += [double]$metric.value
                if (-not $probeQuick) { $probeQuick = $metric.probe }
            }
        }
    }

    $w = Get-Stats $widgetsValues
    $q = Get-Stats $quickValues

    $delta = $null
    $deltaPercent = $null
    if ($comparability -eq 'identical' -and $w -and $q) {
        $delta = $q.Mean - $w.Mean
        if ($w.Mean -ne 0) { $deltaPercent = $delta / [math]::Abs($w.Mean) * 100.0 }
    }

    $rows += [pscustomobject]@{
        Metric        = $name
        Comparability = $comparability
        WidgetsMean   = if ($w) { $w.Mean } else { $null }
        WidgetsSpread = if ($w) { $w.SpreadPercent } else { $null }
        WidgetsN      = if ($w) { $w.N } else { 0 }
        QuickMean     = if ($q) { $q.Mean } else { $null }
        QuickSpread   = if ($q) { $q.SpreadPercent } else { $null }
        QuickN        = if ($q) { $q.N } else { 0 }
        Delta         = $delta
        DeltaPercent  = $deltaPercent
        ProbeWidgets  = $probeWidgets
        ProbeQuick    = $probeQuick
        Note          = switch ($comparability) {
            'identical'     { 'Same producer and probe point; the delta is a real behavioural difference.' }
            'approximate'   { 'Structurally different probe points; side-by-side only, no delta computed.' }
            'frontend_only' { 'Only one frontend can produce this; never subtracted.' }
            default         { '' }
        }
    }
}

# ---------------------------------------------------------------------------
# Superposition workload side
# ---------------------------------------------------------------------------
function Get-SuperpositionStats {
    param([string]$CsvPath)
    if (-not (Test-Path $CsvPath)) { return $null }
    # TAB-separated despite the .csv extension.
    $rows = Import-Csv -Path $CsvPath -Delimiter "`t"
    if ($rows.Count -eq 0) { return $null }
    $columns = $rows[0].PSObject.Properties.Name
    $fpsColumn = ($columns | Where-Object { $_ -match '^FPS$' } | Select-Object -First 1)
    if (-not $fpsColumn) { return $null }
    $utilColumn = ($columns | Where-Object { $_ -match 'UTILIZATION' } | Select-Object -First 1)
    $tempColumn = ($columns | Where-Object { $_ -match 'TEMPERATURE' } | Select-Object -First 1)
    $fps = @($rows | ForEach-Object { [double]$_.$fpsColumn } | Where-Object { $_ -gt 0 })
    if ($fps.Count -eq 0) { return $null }
    $utilMean = $null; $tempMax = $null
    if ($utilColumn) {
        $util = @($rows | ForEach-Object { [double]$_.$utilColumn } | Where-Object { $_ -ge 0 })
        if ($util.Count -gt 0) { $utilMean = ($util | Measure-Object -Average).Average }
    }
    if ($tempColumn) {
        $temp = @($rows | ForEach-Object { [double]$_.$tempColumn } | Where-Object { $_ -gt 0 })
        if ($temp.Count -gt 0) { $tempMax = ($temp | Measure-Object -Maximum).Maximum }
    }
    $sorted = $fps | Sort-Object
    $percentile = {
        param($p)
        $index = [math]::Max(0, [math]::Min($sorted.Count - 1, [int][math]::Floor($p * ($sorted.Count - 1))))
        $sorted[$index]
    }
    return [pscustomobject]@{
        Frames   = $fps.Count
        MeanFps  = ($fps | Measure-Object -Average).Average
        MedianFps = & $percentile 0.50
        # Low-tail percentiles, not the single worst frame: one 2 ms hitch during
        # a driver allocation is not a workload characteristic.
        P1Fps    = & $percentile 0.01
        P5Fps    = & $percentile 0.05
        MinFps   = ($fps | Measure-Object -Minimum).Minimum
        GpuUtilMean = $utilMean
        GpuTempMax  = $tempMax
    }
}

$workload = @()
foreach ($run in $accepted) {
    $stats = Get-SuperpositionStats (Join-Path $run.Directory 'superposition.csv')
    if ($stats) {
        $workload += [pscustomobject]@{
            RunId = $run.RunId; Frontend = $run.Frontend
            Frames = $stats.Frames; MeanFps = $stats.MeanFps; MedianFps = $stats.MedianFps
            P1Fps = $stats.P1Fps; P5Fps = $stats.P5Fps; MinFps = $stats.MinFps
            GpuUtilMean = $stats.GpuUtilMean; GpuTempMax = $stats.GpuTempMax
        }
    }
}

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
$rows | Where-Object { $_.WidgetsN -gt 0 -or $_.QuickN -gt 0 } |
    Format-Table Metric, Comparability, WidgetsMean, QuickMean, Delta, DeltaPercent -AutoSize

if ($workload.Count -gt 0) {
    Write-Host ''
    Write-Host 'Superposition workload (external, NOT ExoSnap captured frames):'
    $workload | Format-Table -AutoSize
}

$dataset = [ordered]@{
    scenario                = $Scenario
    effective_config_hash   = ($fingerprints | Select-Object -First 1)
    accepted_runs           = @($accepted | ForEach-Object {
        [ordered]@{ run_id = $_.RunId; frontend = $_.Frontend; report = $_.Manifest.exosnap_report }
    })
    excluded_runs           = @($runs | Where-Object { -not $_.Accepted } | ForEach-Object {
        [ordered]@{ run_id = $_.RunId; reasons = $_.Rejections }
    })
    metrics                 = $rows
    superposition_workload  = $workload
}
$dataset | ConvertTo-Json -Depth 8 | Set-Content -Path $ReportPath -Encoding UTF8
Write-Host ''
Write-Host "Comparison dataset: $ReportPath"
