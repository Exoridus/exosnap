<#
.SYNOPSIS
    One-time Superposition scene calibration for the canonical A/B workload.

.DESCRIPTION
    Superposition exposes scenes 0..17 and the default benchmark sequence. Which
    of them is a good ExoSnap workload is an empirical question — continuous
    motion, high spatial detail, no long static interval, repeatable — so this
    surveys them instead of picking a number out of the manual.

    ExoSnap is deliberately NOT running: this pass characterises the workload
    alone, which is also the headroom measurement (how much GPU is left for the
    capture path to use). Recording clips for visual judgement is opt-in via
    -RecordClips, and those clips are never part of the accepted dataset.

    -mode_duration is expressed in whole minutes by the CLI, so one minute is the
    shortest a scene can be surveyed for.

.EXAMPLE
    .\Invoke-SceneSurvey.ps1 -Scenes 4,5,8,12 -Quality high
#>
[CmdletBinding()]
param(
    [int[]]$Scenes = @(0..17),
    [ValidateSet('low', 'medium', 'high', 'extreme')][string]$Quality = 'high',
    [string]$Textures = 'high',
    [string]$Resolution = '2560x1440',
    [ValidateSet(0, 1, 2)][int]$Fullscreen = 2,
    [int]$DurationMinutes = 1,
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\..\.workspace\benchmark-results\calibration\scene-survey'),
    [string]$SuperpositionCli = 'C:\Program Files\Unigine\Superposition Benchmark\bin\superposition_cli.exe',
    [switch]$IncludeDefaultSequence
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $SuperpositionCli)) { throw "Superposition CLI not found at $SuperpositionCli." }
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$targets = @()
foreach ($scene in $Scenes) { $targets += [pscustomobject]@{ Label = "scene-$scene"; Mode = @('-mode', 'scene', $scene) } }
if ($IncludeDefaultSequence) { $targets += [pscustomobject]@{ Label = 'default-sequence'; Mode = @('-mode', 'default') } }

$summary = @()
foreach ($target in $targets) {
    $csv = Join-Path $OutputRoot "$($target.Label).csv"
    $txt = Join-Path $OutputRoot "$($target.Label).txt"
    Write-Host "Surveying $($target.Label) at $Quality/$Resolution..."

    $arguments = @(
        '-api', 'directx',
        '-resolution', $Resolution,
        '-fullscreen', $Fullscreen,
        '-quality', $Quality,
        '-textures', $Textures,
        '-dof', 0, '-motion_blur', 0, '-sound', 0,
        '-iterations', 1
    ) + $target.Mode + @(
        '-mode_duration', $DurationMinutes,
        '-log_csv', $csv, '-log_csv_step', 0, '-log_txt', $txt
    )

    $process = Start-Process -FilePath $SuperpositionCli -ArgumentList $arguments -PassThru -Wait
    if ($process.ExitCode -ne 0) {
        Write-Warning "$($target.Label): exit code $($process.ExitCode)."
    }
    if (-not (Test-Path $csv)) {
        Write-Warning "$($target.Label): no CSV produced; skipped."
        continue
    }

    # Superposition writes TAB-separated values despite the .csv extension. A
    # comma parse yields one giant column and every downstream number silently
    # becomes empty, so the delimiter is explicit rather than inferred.
    $rows = Import-Csv -Path $csv -Delimiter "`t"
    if ($rows.Count -eq 0) { Write-Warning "$($target.Label): empty CSV."; continue }
    $columns = $rows[0].PSObject.Properties.Name
    $fpsColumn = ($columns | Where-Object { $_ -match '^FPS$' } | Select-Object -First 1)
    if (-not $fpsColumn) { Write-Warning "$($target.Label): no FPS column in CSV."; continue }
    $utilColumn = ($columns | Where-Object { $_ -match 'UTILIZATION' } | Select-Object -First 1)
    $tempColumn = ($columns | Where-Object { $_ -match 'TEMPERATURE' } | Select-Object -First 1)

    $fps = @($rows | ForEach-Object { [double]$_.$fpsColumn } | Where-Object { $_ -gt 0 })
    if ($fps.Count -eq 0) { continue }
    $sorted = $fps | Sort-Object
    $mean = ($fps | Measure-Object -Average).Average
    $median = $sorted[[int][math]::Floor(0.5 * ($sorted.Count - 1))]
    $p1 = $sorted[[int][math]::Floor(0.01 * ($sorted.Count - 1))]

    # Frame-to-frame FPS variability. A scene that sits at a flat rate for most of
    # its runtime has a static stretch in it, which is the one thing the canonical
    # workload must not have — a capture path is not stressed by a still image.
    $deltas = @()
    for ($i = 1; $i -lt $fps.Count; $i++) { $deltas += [math]::Abs($fps[$i] - $fps[$i - 1]) }
    $variability = if ($deltas.Count -gt 0) { ($deltas | Measure-Object -Average).Average } else { 0 }

    # GPU utilisation is the real headroom signal. FPS alone cannot distinguish
    # "easily hits 144" from "vsync-locked at 144 while the GPU is pinned" — and
    # only the first of those leaves room for ExoSnap's cost to be visible.
    $utilMean = $null; $utilP95 = $null; $tempMax = $null
    if ($utilColumn) {
        $util = @($rows | ForEach-Object { [double]$_.$utilColumn } | Where-Object { $_ -ge 0 })
        if ($util.Count -gt 0) {
            $utilSorted = $util | Sort-Object
            $utilMean = [math]::Round(($util | Measure-Object -Average).Average, 1)
            $utilP95 = [math]::Round($utilSorted[[int][math]::Floor(0.95 * ($utilSorted.Count - 1))], 1)
        }
    }
    if ($tempColumn) {
        $temp = @($rows | ForEach-Object { [double]$_.$tempColumn } | Where-Object { $_ -gt 0 })
        if ($temp.Count -gt 0) { $tempMax = [math]::Round(($temp | Measure-Object -Maximum).Maximum, 1) }
    }

    $summary += [pscustomobject]@{
        Target        = $target.Label
        Frames        = $fps.Count
        MeanFps       = [math]::Round($mean, 1)
        MedianFps     = [math]::Round($median, 1)
        P1Fps         = [math]::Round($p1, 1)
        FpsVariability = [math]::Round($variability, 2)
        GpuUtilMean   = $utilMean
        GpuUtilP95    = $utilP95
        GpuTempMax    = $tempMax
        HeadroomVs144 = [math]::Round($mean / 144.0, 2)
    }
}

$summary | Sort-Object -Property FpsVariability -Descending | Format-Table -AutoSize
$summary | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $OutputRoot 'survey.json') -Encoding UTF8
Write-Host ''
Write-Host "Survey written to $(Join-Path $OutputRoot 'survey.json')."
Write-Host 'Pick a scene on motion and detail, not on score. Then freeze it in the scenario definition.'
