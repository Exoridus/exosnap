#Requires -Version 7.0
<#
.SYNOPSIS
    The offline half of scripts/check-feed-drift.ps1: what the publication policy is
    allowed to say.
.DESCRIPTION
    Only the deterministic half is under test. The feed half reads four public
    services, and a test that needed them would fail on a train rather than on a
    defect; what it does is reported and never blocks, so the thing worth pinning down
    is the statement the policy makes.

    Every case starts from a tree the checker accepts and breaks exactly one thing.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptRoot
$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([string] $Name, [scriptblock] $Body)
    try {
        & $Body
        $script:Passed++
        Write-Host "  PASS  $Name"
    }
    catch {
        $script:Failed++
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }

$cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not parse the project version from the root CMakeLists.txt.'
}
$script:Version = $Matches[1]

function New-PolicyFixture {
    <#
    .SYNOPSIS
        A tree the checker accepts: the three packaging surfaces, a CMake version, and
        the policy that describes them.
    .PARAMETER Version
        The version the fixture tree declares. Defaults to this repository's.
    .PARAMETER Channels
        Replaces the channel set entirely.
    #>
    param([string] $Version, [System.Collections.IDictionary] $Channels)

    if (-not $Version) { $Version = $script:Version }
    $root = Join-Path ([IO.Path]::GetTempPath()) "publication-policy-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path (Join-Path $root 'packaging/chocolatey') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'packaging/winget/manifests') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'packaging/scoop') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $root 'packaging/chocolatey/exosnap.nuspec') -Value '<package />'
    Set-Content -LiteralPath (Join-Path $root 'packaging/scoop/exosnap.json') -Value '{}'
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') `
        -Value "project(exosnap VERSION $Version LANGUAGES C CXX)`n" -NoNewline

    if ($null -eq $Channels) {
        $Channels = [ordered]@{
            github     = [ordered]@{ intent = 'publish'; expectedVersion = $Version; feed = 'https://example.invalid/github' }
            chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = '0.6.0'; resumeAt = '99.0.0'
                feed = 'https://example.invalid/choco'; reason = 'held on purpose' }
            winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
                feed = 'https://example.invalid/winget'; reason = 'held on purpose' }
            scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
                feed = 'https://example.invalid/scoop'; reason = 'held on purpose' }
        }
    }
    ([ordered]@{ policyVersion = 1; product = 'ExoSnap'; channels = $Channels } | ConvertTo-Json -Depth 6) |
        Set-Content -LiteralPath (Join-Path $root 'policy.json') -Encoding utf8
    return $root
}

function Invoke-Checker {
    param([string] $Root)
    $output = & pwsh -NoProfile -NonInteractive -File (Join-Path $scriptRoot 'check-feed-drift.ps1') `
        -Offline -RepoRoot $Root -PolicyPath (Join-Path $Root 'policy.json') 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

Write-Host 'check-feed-drift.ps1 (policy half)'

Test-Case 'the policy in this repository is consistent with its packaging tree' {
    $output = & pwsh -NoProfile -NonInteractive -File (Join-Path $scriptRoot 'check-feed-drift.ps1') -Offline 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -eq 0) "the tracked policy must pass: $output"
}

Test-Case 'a fixture built to pass does pass' {
    $result = Invoke-Checker -Root (New-PolicyFixture)
    Assert-True ($result.ExitCode -eq 0) "the base fixture must pass, or every negative below proves nothing: $($result.Output)"
}

Test-Case 'a packaging surface the policy says nothing about fails' {
    $channels = [ordered]@{
        github = [ordered]@{ intent = 'publish'; expectedVersion = $script:Version; feed = 'https://example.invalid/github' }
        winget = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop  = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'a channel in the tree and not in the policy must fail'
    Assert-True ($result.Output -match 'packaging/chocolatey is in the tree') `
        "the failure must name the uncovered channel: $($result.Output)"
}

Test-Case 'a publishing channel pinned to another version fails' {
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = '0.0.1'; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = '0.6.0'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/choco'; reason = 'held' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'a publishing channel must name the version the tree declares'
    Assert-True ($result.Output -match 'github is meant to publish') "the failure must name the channel: $($result.Output)"
}

Test-Case 'a hold that resumes where it already is fails' {
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = $script:Version; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = '0.6.0'; resumeAt = '0.6.0'
            feed = 'https://example.invalid/choco'; reason = 'held' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'a hold must resume ahead of where it is held'
    Assert-True ($result.Output -match 'which is not ahead of it') "the failure must say why: $($result.Output)"
}

Test-Case 'a hold the tree has already overtaken fails' {
    # The case the policy exists for: a hold nobody lifted, which by now names a
    # version that shipped. Left alone it reads as an intact decision.
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = '9.9.9'; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = '0.6.0'; resumeAt = '0.7.0'
            feed = 'https://example.invalid/choco'; reason = 'held' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Version '9.9.9' -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'a hold the tree has passed must fail'
    Assert-True ($result.Output -match 'overtaken rather than lifted') "the failure must say what happened: $($result.Output)"
}

Test-Case 'a hold with no reason fails' {
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = $script:Version; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = '0.6.0'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/choco'; reason = '' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'a hold without a reason must fail'
    Assert-True ($result.Output -match 'states no reason') "the failure must say what is missing: $($result.Output)"
}

Test-Case 'a hold that comes due is reported and does not fail' {
    # Publishing is the developer's act. A due hold is a thing to say out loud, never
    # a reason to turn a build red, and never a submission.
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = '9.9.9'; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = '0.6.0'; resumeAt = '9.9.9'
            feed = 'https://example.invalid/choco'; reason = 'held' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Version '9.9.9' -Channels $channels)
    Assert-True ($result.ExitCode -eq 0) "a due hold must not fail the run: $($result.Output)"
    Assert-True ($result.Output -match 'chocolatey resumes at 9\.9\.9') "the due hold must be reported: $($result.Output)"
}

Test-Case 'an intent the checker does not implement fails' {
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = $script:Version; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'maybe'; expectedVersion = '0.6.0'; feed = 'https://example.invalid/choco' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'an unknown intent must fail rather than be ignored'
    Assert-True ($result.Output -match "declares intent 'maybe'") "the failure must name it: $($result.Output)"
}

Test-Case 'a version that is not x.y.z fails' {
    $channels = [ordered]@{
        github     = [ordered]@{ intent = 'publish'; expectedVersion = $script:Version; feed = 'https://example.invalid/github' }
        chocolatey = [ordered]@{ intent = 'hold'; expectedVersion = 'latest'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/choco'; reason = 'held' }
        winget     = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/winget'; reason = 'held' }
        scoop      = [ordered]@{ intent = 'hold'; expectedVersion = '0.8.1'; resumeAt = '99.0.0'
            feed = 'https://example.invalid/scoop'; reason = 'held' }
    }
    $result = Invoke-Checker -Root (New-PolicyFixture -Channels $channels)
    Assert-True ($result.ExitCode -ne 0) 'a version that cannot be ordered must fail'
    Assert-True ($result.Output -match "expectedVersion 'latest'") "the failure must quote it: $($result.Output)"
}

Write-Host ''
Write-Host "  $($script:Passed) passed, $($script:Failed) failed."
if ($script:Failed -gt 0) { exit 1 }
exit 0
