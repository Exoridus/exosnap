#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the Qt/build drift guard.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case builds a throwaway git repository in the temp directory and points
    check-drift.ps1 at it. Nothing touches this repository, and nothing is built.

    Every rule is tested BOTH ways. A guard that has only ever been seen green
    proves nothing -- the first version of a guard in this shape passed on
    everything, including the violations it was written for. So each rule gets a
    fixture that must be rejected and a fixture that must be accepted, and the
    accepted ones deliberately include the shapes that previously produced false
    positives: rule words inside comments, and a two-part find_package() minimum
    that is not an SDK version.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$guard = Join-Path $scriptRoot 'check-drift.ps1'

# Every fixture below runs `git init` and `git add -A` in a temp directory. Under a
# git hook those inherit GIT_DIR/GIT_INDEX_FILE from the repository being
# committed, GIT_DIR beats -C, and the fixture's index and `git init` land on the
# REAL repository -- which is exactly what happened the first time this file ran
# from a pre-commit hook: the repository's index was replaced by the fixture's and
# core.bare was set on the shared config, breaking git in every worktree.
$script:LeakedGitVariables = @(
    'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY',
    'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_COMMON_DIR', 'GIT_PREFIX',
    'GIT_CEILING_DIRECTORIES', 'GIT_NAMESPACE', 'GIT_QUARANTINE_PATH')

function Invoke-IsolatedGit {
    param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $GitArgs)
    $saved = @{}
    foreach ($name in $script:LeakedGitVariables) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name)
        if ($null -ne $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
    }
    try { & git @GitArgs 2>&1 | Out-Null }
    finally {
        foreach ($name in $script:LeakedGitVariables) {
            if ($null -ne $saved[$name]) { Set-Item "Env:$name" -Value $saved[$name] }
        }
    }
}

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

function New-FixtureRepo {
    <#
    .SYNOPSIS
        A minimal repository that the guard accepts, as the base for each case.
    .PARAMETER Files
        Relative path -> content. Overrides or adds to the clean base.
    #>
    param([hashtable] $Files = @{})

    $root = Join-Path ([IO.Path]::GetTempPath()) "check-drift-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    $base = @{
        '.qt-version'                          = "6.11.1`n"
        '.github/actions/setup-qt/action.yml'  = @'
name: Set up Qt 6
description: Owns Qt provisioning.
runs:
  using: composite
  steps:
    - name: Install Qt
      uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
      with:
        version: ${{ steps.resolve.outputs.version }}
'@
        '.github/workflows/ci.yml'             = @'
jobs:
  build:
    steps:
      - name: Install Qt 6
        uses: ./.github/actions/setup-qt
        with:
          profile: quick
'@
        'CMakeLists.txt'                       = @'
cmake_minimum_required(VERSION 3.25)
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64")
find_package(Qt6 6.11 REQUIRED COMPONENTS Core Quick)
'@
    }

    foreach ($key in $Files.Keys) { $base[$key] = $Files[$key] }

    foreach ($relative in $base.Keys) {
        $path = Join-Path $root $relative
        $directory = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        Set-Content -LiteralPath $path -Value $base[$relative] -Encoding utf8 -NoNewline
    }

    Invoke-IsolatedGit -C $root init --quiet
    Invoke-IsolatedGit -C $root add -A
    return $root
}

function Invoke-Guard {
    param([string] $Root)
    $output = & pwsh -NoProfile -NonInteractive -File $guard -RepoRoot $Root 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Test-Fixture {
    <#
    .SYNOPSIS
        Builds a fixture, runs the guard, and cleans up.
    #>
    param([hashtable] $Files = @{}, [scriptblock] $Assert)
    $root = New-FixtureRepo -Files $Files
    try { & $Assert (Invoke-Guard -Root $root) }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host 'The clean shape is accepted'

Test-Case 'the migrated repository shape passes' {
    Test-Fixture -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "a clean fixture must pass, but the guard said:`n$($result.Output)"
    }
}

Test-Case 'the real repository passes' {
    # The guard is only worth running if the tree it guards is currently clean.
    $result = Invoke-Guard -Root (Resolve-Path (Join-Path $scriptRoot '..')).Path
    Assert-True ($result.ExitCode -eq 0) "this repository must satisfy its own guard:`n$($result.Output)"
}

Write-Host ''
Write-Host 'setup-qt-centralized'

Test-Case 'a workflow using install-qt-action directly is rejected' {
    # This is the pre-migration shape, verbatim in structure. It has to be red,
    # otherwise the migration proved nothing.
    Test-Fixture -Files @{
        '.github/workflows/asan.yml' = @'
jobs:
  asan:
    steps:
      - name: Install Qt 6
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: '6.11.1'
          arch: 'win64_msvc2022_64'
          archives: 'qtbase qtsvg'
          cache: true
'@
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'install-qt-action outside the composite action must be rejected'
        Assert-True ($result.Output -match 'setup-qt-centralized') 'the violation must name its rule'
    }
}

Test-Case 'the composite action itself may use install-qt-action' {
    Test-Fixture -Assert {
        param($result)
        Assert-True ($result.Output -notmatch 'setup-qt-centralized') `
            'the action that owns Qt provisioning must not be reported for owning it'
    }
}

Test-Case 'prose naming install-qt-action is not a violation' {
    # The earlier attempt at a guard in this shape flagged its own explanation.
    Test-Fixture -Files @{
        '.github/workflows/notes.yml' = @'
# Historical note: this job used jurplel/install-qt-action directly until the
# uses: jurplel/install-qt-action line moved into the composite action.
jobs:
  noop:
    steps:
      - run: echo ok
'@
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "a comment must not be a violation:`n$($result.Output)"
    }
}

Write-Host ''
Write-Host 'qt-version-consistency'

Test-Case 'a workflow pinning a different Qt version is rejected' {
    Test-Fixture -Files @{
        '.github/workflows/old.yml' = @'
jobs:
  build:
    steps:
      - uses: ./.github/actions/setup-qt
        with:
          version: '6.9.2'
'@
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'a divergent Qt version must be rejected'
        Assert-True ($result.Output -match 'qt-version-consistency') 'the violation must name its rule'
        Assert-True ($result.Output -match '6\.9\.2') 'the violation must name the offending version'
    }
}

Test-Case 'a stale Qt SDK path is rejected' {
    Test-Fixture -Files @{
        'CMakeLists.txt' = @'
cmake_minimum_required(VERSION 3.25)
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.9.0/msvc2022_64")
'@
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'an SDK path pinned to another version must be rejected'
        Assert-True ($result.Output -match 'qt-version-consistency') 'the violation must name its rule'
    }
}

Test-Case 'a two-part find_package minimum is not an SDK version' {
    # find_package(Qt6 6.11 REQUIRED ...) states a MINIMUM. Treating it as the
    # installed version would fail every repository that has one.
    Test-Fixture -Files @{
        'app/CMakeLists.txt' = "find_package(Qt6 6.11 REQUIRED COMPONENTS Quick QuickTest)`n"
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "a version minimum must not be read as an SDK pin:`n$($result.Output)"
    }
}

Test-Case 'documentation naming an older Qt is not a violation' {
    Test-Fixture -Files @{
        'docs/history.md'    = "Until August 2026 the project built against Qt 6.9.2 in C:/Qt/6.9.2/msvc2022_64.`n"
        'CHANGELOG.md'       = "- Moved from Qt 6.9.2 to Qt 6.11.1.`n"
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "history must be allowed to say what it says:`n$($result.Output)"
    }
}

Test-Case 'bumping .qt-version alone is rejected' {
    # The point of a canonical version: changing it without migrating the places
    # that still hard-code the old one is exactly the drift being guarded.
    Test-Fixture -Files @{ '.qt-version' = "6.12.0`n" } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'a canonical bump that leaves stale literals behind must be rejected'
        Assert-True ($result.Output -match '6\.11\.1 does not match the canonical 6\.12\.0') `
            'the message must say which value is stale and what it should be'
    }
}

Test-Case 'a missing canonical version file is rejected' {
    $root = New-FixtureRepo
    try {
        Remove-Item -LiteralPath (Join-Path $root '.qt-version') -Force
        Invoke-IsolatedGit -C $root add -A
        $result = Invoke-Guard -Root $root
        Assert-True ($result.ExitCode -ne 0) 'without a canonical version there is nothing to be consistent with'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host 'qt-sdk-path-allowlist'

Test-Case 'a new absolute Qt SDK path is rejected' {
    Test-Fixture -Files @{
        'scripts/new-helper.ps1' = "`$qtBin = 'C:\Qt\6.11.1\msvc2022_64\bin'`n"
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'a new hard-coded SDK path must be rejected even at the right version'
        Assert-True ($result.Output -match 'qt-sdk-path-allowlist') 'the violation must name its rule'
    }
}

Test-Case 'the already-accepted SDK paths stay accepted' {
    Test-Fixture -Files @{
        'scripts/check-quality.ps1' = "`$qtBin = 'C:\Qt\6.11.1\msvc2022_64\bin'`n"
        'scripts/run-tests.ps1'     = "`$qtBin = 'C:\Qt\6.11.1\msvc2022_64\bin'`n"
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "the known locations must not be reported:`n$($result.Output)"
    }
}

Write-Host ''
Write-Host 'no-qmake-project'

Test-Case 'a qmake project file is rejected' {
    Test-Fixture -Files @{ 'app/app.pro' = "TEMPLATE = app`n" } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'a .pro file must be rejected'
        Assert-True ($result.Output -match 'no-qmake-project') 'the violation must name its rule'
    }
}

Test-Case 'a file merely mentioning qmake is not a violation' {
    Test-Fixture -Files @{
        'docs/build.md' = "This project never used qmake or .pro files; it is CMake-only.`n"
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "prose about qmake is not a qmake project:`n$($result.Output)"
    }
}

Write-Host ''
Write-Host 'Discovery hygiene'

Test-Case 'test fixtures are not scanned' {
    # A guard has to have fixtures that contain exactly what it rejects. Scanning
    # them reports the guard's own evidence as a violation -- which is how this
    # very file first failed its own pre-commit hook.
    Test-Fixture -Files @{
        'scripts/tests/some-guard.tests.ps1' = @'
$fixture = @"
  uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730
  version: '6.9.2'
"@
$qtBin = 'C:\Qt.9.2\msvc2022_64in'
'@
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "a test fixture must not be read as production configuration:`n$($result.Output)"
    }
}

Test-Case 'an untracked second checkout is not scanned' {
    # Agent sessions keep full copies of this repository under .claude/worktrees/.
    # A recursive directory walk would read them as source and report violations
    # that exist only in somebody else's branch.
    $root = New-FixtureRepo
    try {
        $nested = Join-Path $root '.claude/worktrees/agent-x'
        New-Item -ItemType Directory -Path (Join-Path $nested '.github/workflows') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $nested '.github/workflows/ci.yml') -Encoding utf8 -Value @'
jobs:
  build:
    steps:
      - uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: '6.9.2'
'@
        $result = Invoke-Guard -Root $root
        Assert-True ($result.ExitCode -eq 0) `
            "an untracked nested checkout must not be scanned, but the guard said:`n$($result.Output)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a fixture does not write into the repository a hook points at' {
    # The regression: with GIT_DIR and GIT_INDEX_FILE set, building a fixture used
    # to reinitialise and re-index whatever they pointed at.
    $outer = Join-Path ([IO.Path]::GetTempPath()) "check-drift-outer/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $outer -Force | Out-Null
    try {
        Set-Content -LiteralPath (Join-Path $outer 'kept.txt') -Value 'kept' -Encoding utf8
        Invoke-IsolatedGit -C $outer init --quiet
        Invoke-IsolatedGit -C $outer add -A

        $outerGit = Join-Path $outer '.git'
        $before = (Get-FileHash -LiteralPath (Join-Path $outerGit 'index')).Hash

        $env:GIT_DIR = $outerGit
        $env:GIT_INDEX_FILE = Join-Path $outerGit 'index'
        try {
            $fixture = New-FixtureRepo
            Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
        }
        finally {
            Remove-Item Env:GIT_DIR -ErrorAction SilentlyContinue
            Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
        }

        $after = (Get-FileHash -LiteralPath (Join-Path $outerGit 'index')).Hash
        Assert-True ($before -eq $after) 'building a fixture rewrote the index of the repository GIT_DIR pointed at'

        $config = Get-Content -LiteralPath (Join-Path $outerGit 'config') -Raw
        Assert-True ($config -notmatch '(?m)^\s*bare\s*=\s*true') `
            'building a fixture set core.bare on the repository GIT_DIR pointed at'
    }
    finally { Remove-Item -LiteralPath $outer -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
