#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for scripts/find-qualified-rc.ps1, the rule that decides which release
    candidate a commit may be promoted from.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Two gates read this answer -- the publish lock on a final tag, and the guard on
    sign-manifest's standalone attach -- so a wrong answer here is a release published
    against evidence that describes something else. The cases are the ways it can be
    wrong: no candidate at this commit, a candidate at a different commit, an older
    candidate winning over a newer one, a candidate of a different base version, and a
    candidate that carries no record or no sidecars.

    `gh` is a stub script driven by environment variables, so nothing here reaches the
    network or a real release.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$script:Finder = Join-Path $scriptRoot 'find-qualified-rc.ps1'
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
function Assert-Match {
    param([string] $Pattern, [string] $Text, [string] $Message)
    if ($Text -notmatch $Pattern) { throw "$Message (no match for '$Pattern' in: $Text)" }
}

$script:Commit = '1111111111111111111111111111111111111111'
$script:OtherCommit = '2222222222222222222222222222222222222222'

function New-TestDirectory {
    $path = Join-Path ([IO.Path]::GetTempPath()) "find-qualified-rc-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

function New-FakeGh {
    <#
    .SYNOPSIS
        A `gh` that answers from environment variables instead of GitHub.
    #>
    param([Parameter(Mandatory)] [string] $Directory)

    $path = Join-Path $Directory 'fake-gh.ps1'
    $body = @'
$ErrorActionPreference = 'Stop'
if ($args[0] -eq 'api' -and $args[1] -match '/releases$') {
    foreach ($tag in ($env:FAKE_GH_TAGS -split ';' | Where-Object { $_ })) { Write-Output $tag }
    exit 0
}
if ($args[0] -eq 'api' -and $args[1] -match '/commits/(.+)$') {
    $tag = $Matches[1]
    foreach ($pair in ($env:FAKE_GH_COMMITS -split ';' | Where-Object { $_ })) {
        $parts = $pair -split '='
        if ($parts[0] -eq $tag) { Write-Output $parts[1]; exit 0 }
    }
    exit 1
}
if ($args[0] -eq 'release' -and $args[1] -eq 'download') {
    $dir = $args[$args.IndexOf('--dir') + 1]
    $pattern = $args[$args.IndexOf('--pattern') + 1]
    $available = @($env:FAKE_GH_ASSETS -split ';' | Where-Object { $_ })
    $matched = @($available | Where-Object { $_ -like $pattern })
    if ($matched.Count -eq 0) { exit 1 }
    foreach ($name in $matched) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $dir $name) -Value '{}' -Encoding utf8NoBOM
    }
    exit 0
}
exit 1
'@
    Set-Content -LiteralPath $path -Value $body -Encoding utf8NoBOM
    return $path
}

function Invoke-Finder {
    param(
        [string] $Tags,
        [string] $Commits,
        [string] $Assets = 'release-verification.json;release-verification.json.sig;ExoSnap-0.9.1-rc2-windows-x64-portable.sha256;artifact-manifest.json;toolchain-manifest.json',
        [string] $BaseVersion = '0.9.1',
        [string] $Commit = $script:Commit
    )

    $directory = New-TestDirectory
    $gh = New-FakeGh -Directory $directory
    $previous = @{
        Tags    = $env:FAKE_GH_TAGS
        Commits = $env:FAKE_GH_COMMITS
        Assets  = $env:FAKE_GH_ASSETS
    }
    $env:FAKE_GH_TAGS = $Tags
    $env:FAKE_GH_COMMITS = $Commits
    $env:FAKE_GH_ASSETS = $Assets
    try {
        $tag = & $script:Finder -Repository 'Exoridus/exosnap' -BaseVersion $BaseVersion `
            -Commit $Commit -OutputDirectory (Join-Path $directory 'evidence') -GhCommand $gh
        return @{ Tag = "$tag"; Error = $null; Directory = (Join-Path $directory 'evidence') }
    }
    catch {
        return @{ Tag = $null; Error = $_.Exception.Message; Directory = (Join-Path $directory 'evidence') }
    }
    finally {
        $env:FAKE_GH_TAGS = $previous.Tags
        $env:FAKE_GH_COMMITS = $previous.Commits
        $env:FAKE_GH_ASSETS = $previous.Assets
    }
}

Write-Host 'find-qualified-rc.ps1'

Test-Case 'the newest candidate at this commit wins' {
    $result = Invoke-Finder -Tags 'v0.9.1-rc1;v0.9.1-rc2;v0.9.0' `
        -Commits "v0.9.1-rc1=$($script:Commit);v0.9.1-rc2=$($script:Commit);v0.9.0=$($script:OtherCommit)"
    Assert-Equal 'v0.9.1-rc2' $result.Tag "the newest candidate must win: $($result.Error)"
}

Test-Case 'a newer candidate at another commit does not displace the matching one' {
    # The trap this rule exists for is the reverse -- an older record promoting a
    # newer candidate -- but a candidate cut from different source must not win
    # either, however recent it is.
    $result = Invoke-Finder -Tags 'v0.9.1-rc1;v0.9.1-rc7' `
        -Commits "v0.9.1-rc1=$($script:Commit);v0.9.1-rc7=$($script:OtherCommit)"
    Assert-Equal 'v0.9.1-rc1' $result.Tag "only candidates at this commit count: $($result.Error)"
}

Test-Case 'a candidate of another base version is not a candidate for this one' {
    $result = Invoke-Finder -Tags 'v0.9.0-rc4' -Commits "v0.9.0-rc4=$($script:Commit)"
    Assert-True ($null -eq $result.Tag) 'a candidate of another base must not be accepted'
    Assert-Match 'No published release candidate' $result.Error 'the reason must say none was found'
}

Test-Case 'no candidate at this commit is a refusal that names the checklist' {
    $result = Invoke-Finder -Tags 'v0.9.1-rc1' -Commits "v0.9.1-rc1=$($script:OtherCommit)"
    Assert-True ($null -eq $result.Tag) 'a commit with no candidate must be refused'
    Assert-Match 'release-checklist' $result.Error 'the reason must point at the process'
}

Test-Case 'a candidate carrying no qualification record is refused' {
    $result = Invoke-Finder -Tags 'v0.9.1-rc1' -Commits "v0.9.1-rc1=$($script:Commit)" `
        -Assets 'ExoSnap-0.9.1-rc1-windows-x64-portable.sha256'
    Assert-True ($null -eq $result.Tag) 'a recordless candidate must be refused'
    Assert-Match 'no release-verification.json' $result.Error 'the reason must name the missing record'
}

Test-Case 'a candidate publishing no sidecars is refused' {
    $result = Invoke-Finder -Tags 'v0.9.1-rc1' -Commits "v0.9.1-rc1=$($script:Commit)" `
        -Assets 'release-verification.json;release-verification.json.sig'
    Assert-True ($null -eq $result.Tag) 'a sidecar-less candidate must be refused'
    Assert-Match 'no .sha256 sidecars' $result.Error 'the reason must name the missing sidecars'
}

Test-Case 'a candidate with no signature or inventory still resolves, so the checks can say why' {
    # These four downloads are deliberately not blocking here: the record checks give
    # the operator a fix, and a download failure would replace it with a stack trace.
    $result = Invoke-Finder -Tags 'v0.9.1-rc1' -Commits "v0.9.1-rc1=$($script:Commit)" `
        -Assets 'release-verification.json;ExoSnap-0.9.1-rc1-windows-x64-portable.sha256'
    Assert-Equal 'v0.9.1-rc1' $result.Tag "discovery must succeed: $($result.Error)"
    Assert-True (Test-Path -LiteralPath (Join-Path $result.Directory 'release-verification.json')) `
        'the record must have been downloaded'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $result.Directory 'release-verification.json.sig'))) `
        'the absent signature must simply be absent'
}

Write-Host ''
Write-Host "  $($script:Passed) passed, $($script:Failed) failed."
if ($script:Failed -gt 0) { exit 1 }
exit 0
