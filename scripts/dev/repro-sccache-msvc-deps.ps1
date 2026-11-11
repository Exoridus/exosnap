# Reproduce sccache/MSVC /showIncludes replay against Ninja in two isolated checkouts.
# Run in an MSVC Developer PowerShell where cl, cmake, ninja and sccache are on PATH.
# No project worktrees, existing caches or source files are modified.
[CmdletBinding()]
param(
    [switch]$KeepEvidence
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

foreach ($tool in @('cl', 'cmake', 'ninja', 'sccache')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing $tool. Run from an MSVC Developer PowerShell with CMake, Ninja and sccache on PATH."
    }
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap-sccache-replay-" + [guid]::NewGuid().ToString('N'))
$srcA = Join-Path $root 'source-a'
$srcB = Join-Path $root 'source-b'
$buildA = Join-Path $root 'build-a'
$buildB = Join-Path $root 'build-b'
$oldCacheDir = [Environment]::GetEnvironmentVariable('SCCACHE_DIR')
$oldServerPort = [Environment]::GetEnvironmentVariable('SCCACHE_SERVER_PORT')
$oldGhaEnabled = [Environment]::GetEnvironmentVariable('SCCACHE_GHA_ENABLED')
$port = Get-Random -Minimum 45000 -Maximum 61000

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed (exit $LASTEXITCODE): $($Arguments -join ' ')"
    }
}

function Get-CacheHits {
    $json = & sccache --show-stats --stats-format=json
    if ($LASTEXITCODE -ne 0) { throw 'Cannot query sccache statistics' }
    $stats = $json | ConvertFrom-Json
    if ($null -eq $stats.stats.cache_hits.counts) { throw 'Missing sccache cache_hits.counts' }
    $sum = ($stats.stats.cache_hits.counts.PSObject.Properties | Measure-Object -Property Value -Sum).Sum
    if ($null -eq $sum) { return [int64]0 }
    return [int64]$sum
}

try {
    foreach ($dir in @($root, $srcA, $srcB)) {
        [void](New-Item -ItemType Directory -Path $dir -Force)
    }
    $env:SCCACHE_DIR = Join-Path $root 'cache'
    $env:SCCACHE_SERVER_PORT = [string]$port
    # Do not use the existing GitHub backend or production cache. Keep any
    # inherited SCCACHE_BASEDIRS unchanged and report it for provenance.
    Remove-Item Env:SCCACHE_GHA_ENABLED -ErrorAction SilentlyContinue

    $cmake = @'
cmake_minimum_required(VERSION 3.27)
project(exosnap_sccache_replay LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(exosnap_probe main.cpp)
'@
    $cpp = @'
#include "marker.h"
#include <cstdio>
int main() { std::printf("%d\n", marker()); return 0; }
'@
    $header = 'inline int marker() { return 41; }'
    foreach ($source in @($srcA, $srcB)) {
        Set-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Value $cmake -Encoding utf8
        Set-Content -LiteralPath (Join-Path $source 'main.cpp') -Value $cpp -Encoding utf8
        Set-Content -LiteralPath (Join-Path $source 'marker.h') -Value $header -Encoding utf8
    }

    Write-Host "Scratch: $root"
    Write-Host "sccache: $( (& sccache --version) -join ' ' )"
    Write-Host "SCCACHE_BASEDIRS: $([Environment]::GetEnvironmentVariable('SCCACHE_BASEDIRS'))"
    Write-Host 'Build A (populate isolated cache)'
    Invoke-Checked cmake @('-S', $srcA, '-B', $buildA, '-G', 'Ninja', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache')
    Invoke-Checked cmake @('--build', $buildA)
    $before = Get-CacheHits

    Write-Host 'Build B (a second source root with identical inputs)'
    Invoke-Checked cmake @('-S', $srcB, '-B', $buildB, '-G', 'Ninja', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache')
    Invoke-Checked cmake @('--build', $buildB)
    $after = Get-CacheHits
    $hits = $after - $before

    # Inspect the actual Ninja database, not sccache's summary.
    # Foreign includes can also be wrong while the previous checkout exists.
    $deps = & ninja -C $buildB -t deps
    if ($LASTEXITCODE -ne 0) { throw 'ninja -t deps failed' }
    $previousRoot = $srcA.Replace('\', '/')
    $foreign = @($deps | Where-Object { $_.Replace('\', '/').IndexOf($previousRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0 })

    Write-Host "Cross-checkout cache hits: $hits"
    Write-Host "Dependency lines referencing checkout A: $($foreign.Count)"
    foreach ($line in ($foreign | Select-Object -First 8)) {
        Write-Host "  $line"
    }

    # A healthy dependency graph must recompile when the current header changes.
    # This is a correctness gate independent of cache hit rate.
    Start-Sleep -Milliseconds 1100
    Set-Content -LiteralPath (Join-Path $srcB 'marker.h') -Value 'inline int marker() { return 42; }' -Encoding utf8
    Invoke-Checked cmake @('--build', $buildB)
    $binary = Join-Path $buildB 'exosnap_probe.exe'
    if (-not (Test-Path -LiteralPath $binary)) {
        $binary = Join-Path $buildB 'exosnap_probe'
    }
    $value = & $binary
    if ($LASTEXITCODE -ne 0 -or "$value".Trim() -ne '42') {
        throw "Header-change correctness failure: expected 42, got '$value'"
    }

    if ($hits -eq 0) {
        Write-Warning 'INCONCLUSIVE: the isolated builds did not produce a cross-checkout sccache hit. Do not infer that dependency replay is safe.'
    } elseif ($foreign.Count -gt 0) {
        Write-Warning 'REPRODUCED: cached /showIncludes data references another checkout. Check no-op rebuilds and investigate a safe sccache-side fix.'
    } else {
        Write-Host 'PASS: cross-checkout hit without a foreign checkout reference in Ninja deps.'
    }
    Write-Host 'PASS: modifying marker.h in B produced the expected executable output.'
}
finally {
    # Only terminate the isolated sccache server launched with this random port.
    try { & sccache --stop-server 2>$null | Out-Null } catch {}
    if ($null -eq $oldCacheDir) { Remove-Item Env:SCCACHE_DIR -ErrorAction SilentlyContinue }
    else { $env:SCCACHE_DIR = $oldCacheDir }
    if ($null -eq $oldServerPort) { Remove-Item Env:SCCACHE_SERVER_PORT -ErrorAction SilentlyContinue }
    else { $env:SCCACHE_SERVER_PORT = $oldServerPort }
    if ($null -eq $oldGhaEnabled) { Remove-Item Env:SCCACHE_GHA_ENABLED -ErrorAction SilentlyContinue }
    else { $env:SCCACHE_GHA_ENABLED = $oldGhaEnabled }
    if ($KeepEvidence) { Write-Host "Evidence retained: $root" }
    elseif (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
