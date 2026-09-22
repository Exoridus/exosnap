#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the third-party identity the release manifest records.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Two properties are under test. Against the real repository, every component
    that reaches a shipped binary has a readable pin, so a release manifest can
    never quietly ship a dependency it cannot name. Against fixtures, a pin that
    has gone missing or become unreadable produces a refusal rather than a
    shorter list, because a shorter list is the failure that would not be
    noticed.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptRoot
Import-Module (Join-Path $scriptRoot 'lib/DependencyIdentity.psm1') -Force

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

function Assert-Throws {
    param([Parameter(Mandatory)] [scriptblock] $Body, [Parameter(Mandatory)] [string] $Message)
    try { & $Body }
    catch { return }
    throw $Message
}

function New-FixtureRoot {
    <#
    .SYNOPSIS
        A minimal tree carrying the four inputs the reader consults, owned by the
        calling case.
    #>
    $root = Join-Path ([IO.Path]::GetTempPath()) "dependency-identity/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    foreach ($relative in @('third_party', 'cmake', 'libs/update/third_party/miniz',
            'libs/update/third_party/monocypher', 'third_party/nvidia', 'app/assets/fonts')) {
        New-Item -ItemType Directory -Path (Join-Path $root $relative) -Force | Out-Null
    }

    Set-Content -LiteralPath (Join-Path $root '.qt-version') -Value '6.11.2' -Encoding utf8
    Set-Content -LiteralPath (Join-Path $root 'libs/update/third_party/miniz/miniz.c') -Value 'int miniz;' -Encoding utf8
    Set-Content -LiteralPath (Join-Path $root 'libs/update/third_party/monocypher/monocypher.c') -Value 'int mono;' -Encoding utf8
    Set-Content -LiteralPath (Join-Path $root 'third_party/nvidia/nvEncodeAPI.h') -Value '#pragma once' -Encoding utf8
    Set-Content -LiteralPath (Join-Path $root 'app/assets/fonts/font.txt') -Value 'font' -Encoding utf8

    Set-Content -LiteralPath (Join-Path $root 'cmake/VendorFFmpeg.cmake') -Encoding utf8 -Value @'
set(EXOSNAP_FFMPEG_VERSION "r7-n8.1.1" CACHE STRING "pinned")
FetchContent_Declare(
    ffmpeg_prebuilt
    URL      "https://example.invalid/r7/ffmpeg-win64-lgpl-shared.zip"
    URL_HASH "SHA256=E895E66FC9CE1871ABC09A09FD9B99B663971ADFDD2BC1F10B119942E656AFCB"
)
'@

    $declarations = foreach ($name in @('spdlog', 'nlohmann_json', 'tomlplusplus', 'libopus',
            'flac', 'rnnoise', 'EBML', 'libmatroska', 'presentmon')) {
        @"
FetchContent_Declare(
  $name
  GIT_REPOSITORY https://example.invalid/$name.git
  GIT_TAG        0123456789abcdef0123456789abcdef01234567 # tag v1.2.3
)
"@
    }
    Set-Content -LiteralPath (Join-Path $root 'third_party/CMakeLists.txt') -Value ($declarations -join "`n") -Encoding utf8

    return $root
}

Write-Host 'Dependency identity'

Test-Case 'every expected component in the real repository has a readable pin' {
    $entries = Get-DependencyIdentity -RepoRoot $repoRoot
    foreach ($expected in @('Qt', 'FFmpeg', 'spdlog', 'nlohmann_json', 'tomlplusplus', 'libopus',
            'flac', 'rnnoise', 'EBML', 'libmatroska', 'presentmon', 'miniz', 'monocypher')) {
        $entry = $entries | Where-Object { $_.name -eq $expected }
        Assert-True ($entry) "'$expected' is missing from the dependency identity"
        Assert-True ($entry.source) "'$expected' has no source"
    }
}

Test-Case 'every FetchContent component names the exact revision it was built from' {
    $entries = Get-DependencyIdentity -RepoRoot $repoRoot
    foreach ($name in @('spdlog', 'libopus', 'flac', 'rnnoise', 'EBML', 'libmatroska', 'presentmon')) {
        $entry = $entries | Where-Object { $_.name -eq $name }
        Assert-True ($entry.revision -match '^[0-9a-f]{40}$') `
            "'$name' does not carry a full commit revision; '$($entry.revision)' cannot identify a source tree"
    }
}

Test-Case 'the FFmpeg entry carries the archive hash the build verifies against' {
    $ffmpeg = (Get-DependencyIdentity -RepoRoot $repoRoot) | Where-Object { $_.name -eq 'FFmpeg' }
    Assert-True ($ffmpeg.archiveSha256 -match '^[0-9a-f]{64}$') 'the FFmpeg archive hash is not a SHA-256'
    $vendorText = Get-Content -LiteralPath (Join-Path $repoRoot 'cmake/VendorFFmpeg.cmake') -Raw
    Assert-True ($vendorText -match "(?i)$($ffmpeg.archiveSha256)") `
        'the recorded FFmpeg hash is not the one cmake/VendorFFmpeg.cmake pins'
}

Test-Case 'a component that disappears from its pin file is refused, not dropped' {
    $root = New-FixtureRoot
    try {
        $path = Join-Path $root 'third_party/CMakeLists.txt'
        $text = Get-Content -LiteralPath $path -Raw
        $stripped = [regex]::Replace($text, '(?is)FetchContent_Declare\(\s*libopus\s.*?\)', '')
        Assert-True ($stripped -notmatch 'libopus') 'the fixture still declares libopus; the case proves nothing'
        Set-Content -LiteralPath $path -Value $stripped -Encoding utf8
        Assert-Throws { Get-DependencyIdentity -RepoRoot $root } `
            'a missing libopus pin produced a shorter list instead of a refusal'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'an FFmpeg pin without its hash is refused' {
    $root = New-FixtureRoot
    try {
        $path = Join-Path $root 'cmake/VendorFFmpeg.cmake'
        $text = (Get-Content -LiteralPath $path -Raw) -replace '(?i)\s*URL_HASH[^\r\n]*', ''
        Set-Content -LiteralPath $path -Value $text -Encoding utf8
        Assert-Throws { Get-DependencyIdentity -RepoRoot $root } `
            'an unpinned FFmpeg archive was recorded as an identity'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a Qt version that is not three parts is refused' {
    $root = New-FixtureRoot
    try {
        Set-Content -LiteralPath (Join-Path $root '.qt-version') -Value '6.11' -Encoding utf8
        Assert-Throws { Get-DependencyIdentity -RepoRoot $root } `
            'a two-part Qt version was accepted as an SDK identity'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the vendored hash follows the content, not just the file names' {
    $root = New-FixtureRoot
    try {
        $before = ((Get-DependencyIdentity -RepoRoot $root) | Where-Object { $_.name -eq 'miniz' }).sourceSha256
        Add-Content -LiteralPath (Join-Path $root 'libs/update/third_party/miniz/miniz.c') -Value 'int added;'
        $after = ((Get-DependencyIdentity -RepoRoot $root) | Where-Object { $_.name -eq 'miniz' }).sourceSha256
        Assert-True ($after -ne $before) 'an edited vendored source produced the same digest'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the vendored hash follows the file names, not just the content' {
    $root = New-FixtureRoot
    try {
        $before = ((Get-DependencyIdentity -RepoRoot $root) | Where-Object { $_.name -eq 'miniz' }).sourceSha256
        Rename-Item -LiteralPath (Join-Path $root 'libs/update/third_party/miniz/miniz.c') -NewName 'renamed.c'
        $after = ((Get-DependencyIdentity -RepoRoot $root) | Where-Object { $_.name -eq 'miniz' }).sourceSha256
        Assert-True ($after -ne $before) 'a renamed vendored source produced the same digest'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a missing vendored tree is refused' {
    $root = New-FixtureRoot
    try {
        Remove-Item -LiteralPath (Join-Path $root 'libs/update/third_party/monocypher') -Recurse -Force
        Assert-Throws { Get-DependencyIdentity -RepoRoot $root } `
            'a vendored component that is no longer in the tree was silently omitted'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a pin block never borrows the revision of the declaration after it' {
    $text = @'
FetchContent_Declare(
  first
  GIT_REPOSITORY https://example.invalid/first.git
)
FetchContent_Declare(
  second
  GIT_REPOSITORY https://example.invalid/second.git
  GIT_TAG        aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa # tag v9.9.9
)
'@
    Assert-True ($null -eq (Get-FetchContentPin -Text $text -Name 'first')) `
        'a declaration without a GIT_TAG was given the next declaration''s revision'
    Assert-True ((Get-FetchContentPin -Text $text -Name 'second').revision -eq 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa') `
        'the second declaration lost its own revision'
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
