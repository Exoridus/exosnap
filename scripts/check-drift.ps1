#Requires -Version 7.0
<#
.SYNOPSIS
    Guards the few build/Qt invariants that used to be enforced by everyone
    remembering them.

.DESCRIPTION
    Not a Qt linter and not a style checker. Four rules, each one a shape that has
    already gone wrong here or is one copy-paste away from doing so, and each one
    written to have a very low false-positive rate:

      qt-version-consistency  Every three-part Qt version literal in build
                              machinery equals the canonical .qt-version.
      setup-qt-centralized    install-qt-action is used only by the composite
                              action that owns Qt provisioning.
      no-qmake-project        No qmake-era .pro/.pri project file appears.
      qt-sdk-path-allowlist   An absolute Qt SDK path appears only where it is
                              already known and accepted.

    Two rules that were considered and deliberately NOT implemented, because both
    fire on legitimate code far too often to be blocking: "new shipping QML import
    without a deployment entry" (every test-only import trips it) and "new shipping
    executable without a packaging entry" (the repository has many deliberately
    unpackaged test executables).

.NOTES
    File discovery is `git ls-files`, never a recursive directory walk. The
    repository contains gitignored full second checkouts under .claude/worktrees/
    while parallel agent sessions run, plus build trees under build/; a recursive
    scanner would read those as source and report the same violation five times,
    or report one that only exists in someone else's branch.

    Comment lines are stripped before matching, and scripts/tests/ is skipped
    entirely. An earlier attempt at a guard in this shape reported violations
    against its own explanatory prose, because the rule words appear in the
    comments that describe the rule; this one reported its own rejected fixtures,
    which by construction contain every shape it rejects.

.PARAMETER RepoRoot
    Repository to check. Defaults to the repository this script lives in. Tests
    point it at a fixture instead.

.PARAMETER Quiet
    Print only violations.
#>

param(
    [string] $RepoRoot,
    [switch] $Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

# Absolute Qt SDK paths that already exist and are accepted. This list is the
# point of the rule: it is not meant to grow silently. A new entry is a decision.
$script:QtSdkPathAllowlist = @(
    'CMakeLists.txt',
    'scripts/check-quality.ps1',
    'scripts/run-tests.ps1'
)

# Test fixtures are the one place that is SUPPOSED to contain every shape these
# rules reject: a guard with no rejected fixture has never been shown to reject
# anything. Scanning them reports the guard's own evidence as a violation.
$script:ExcludedPattern = '(?i)^scripts/tests/'

# Where a Qt version literal is load-bearing. Documentation and changelogs
# legitimately name older versions and are not checked.
$script:VersionScannedPattern = '(?i)(^CMakeLists\.txt$|(^|/)CMakeLists\.txt$|\.cmake$|^CMakePresets\.json$|^scripts/.*\.ps1$|^\.github/.*\.ya?ml$)'

function Get-TrackedFile {
    param([Parameter(Mandatory)] [string] $Root)
    $files = & git -C $Root ls-files 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed in '$Root'. check-drift.ps1 needs a git repository."
    }
    return , @($files | Where-Object { $_ })
}

function Remove-CommentLine {
    <#
    .SYNOPSIS
        Blanks whole-line comments, keeping line numbering intact.
    .DESCRIPTION
        Only whole-line comments. A trailing comment on a real line is left alone
        on purpose: stripping it needs to know about string literals, and getting
        that wrong silently changes what the rules see.
    #>
    param([string[]] $Lines)
    # Comma operator: a one-line file would otherwise return a bare string, and
    # under Set-StrictMode the caller's .Count then fails on a scalar.
    return , @($Lines | ForEach-Object {
            if ($_ -match '^\s*(#|//|<!--)') { '' } else { $_ }
        })
}

function New-Violation {
    param(
        [Parameter(Mandatory)] [string] $Rule,
        [Parameter(Mandatory)] [string] $File,
        [int] $Line = 0,
        [Parameter(Mandatory)] [string] $Message
    )
    return [pscustomobject]@{ Rule = $Rule; File = $File; Line = $Line; Message = $Message }
}

function Test-DriftRule {
    <#
    .SYNOPSIS
        Runs every rule against a repository and returns the violations.
    #>
    [OutputType([object[]])]
    param([Parameter(Mandatory)] [string] $Root)

    $violations = [System.Collections.Generic.List[object]]::new()
    $tracked = Get-TrackedFile -Root $Root

    # -- canonical version -----------------------------------------------------
    $canonicalPath = Join-Path $Root '.qt-version'
    if (-not (Test-Path -LiteralPath $canonicalPath -PathType Leaf)) {
        $violations.Add((New-Violation -Rule 'qt-version-consistency' -File '.qt-version' `
                    -Message 'the canonical Qt version file is missing'))
        return , @($violations)
    }
    $canonical = (Get-Content -LiteralPath $canonicalPath -Raw).Trim()
    if ($canonical -notmatch '^\d+\.\d+\.\d+$') {
        $violations.Add((New-Violation -Rule 'qt-version-consistency' -File '.qt-version' `
                    -Message "'$canonical' is not a three-part version"))
        return , @($violations)
    }

    foreach ($relative in $tracked) {
        $path = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        if ($relative -match $script:ExcludedPattern) { continue }

        # -- no-qmake-project --------------------------------------------------
        if ($relative -match '(?i)\.(pro|pri)$') {
            $violations.Add((New-Violation -Rule 'no-qmake-project' -File $relative `
                        -Message 'qmake-era project file; this project is CMake-only'))
            continue
        }

        $isVersionScanned = $relative -match $script:VersionScannedPattern
        $isYaml = $relative -match '(?i)\.ya?ml$'
        if (-not $isVersionScanned -and -not $isYaml) { continue }

        $lines = Remove-CommentLine -Lines @(Get-Content -LiteralPath $path)

        for ($i = 0; $i -lt $lines.Count; $i++) {
            $line = $lines[$i]
            if (-not $line) { continue }
            $number = $i + 1

            # -- setup-qt-centralized -----------------------------------------
            # Structured match on a `uses:` step, not on the bare name: the
            # composite action's own prose mentions install-qt-action, and so
            # does this file.
            if ($line -match '(?i)^\s*(-\s*)?uses:\s*\S*install-qt-action') {
                if ($relative -ne '.github/actions/setup-qt/action.yml') {
                    $violations.Add((New-Violation -Rule 'setup-qt-centralized' -File $relative -Line $number `
                                -Message 'install-qt-action is used directly; use ./.github/actions/setup-qt instead'))
                }
            }

            # -- qt-sdk-path-allowlist ----------------------------------------
            if ($line -match '(?i)[A-Z]:[\\/]{1,2}Qt[\\/]{1,2}\d+\.\d+\.\d+') {
                if ($script:QtSdkPathAllowlist -notcontains $relative) {
                    $violations.Add((New-Violation -Rule 'qt-sdk-path-allowlist' -File $relative -Line $number `
                                -Message 'absolute Qt SDK path outside the accepted locations'))
                }
            }

            # -- qt-version-consistency ---------------------------------------
            # Two load-bearing shapes only:
            #   an absolute Qt SDK path, drive letter through three-part version
            #   a workflow version input, `version:` followed by a three-part version
            # A two-part find_package(Qt6 6.11 REQUIRED ...) is a MINIMUM, not the
            # installed SDK, and is deliberately not matched.
            $found = @()
            $sdk = [regex]::Matches($line, '(?i)[A-Z]:[\\/]{1,2}Qt[\\/]{1,2}(\d+\.\d+\.\d+)')
            foreach ($match in $sdk) { $found += $match.Groups[1].Value }
            if ($isYaml) {
                $versionInput = [regex]::Match($line, "(?i)^\s*version:\s*'?(\d+\.\d+\.\d+)'?\s*$")
                if ($versionInput.Success) { $found += $versionInput.Groups[1].Value }
            }

            foreach ($version in ($found | Sort-Object -Unique)) {
                if ($version -ne $canonical) {
                    $violations.Add((New-Violation -Rule 'qt-version-consistency' -File $relative -Line $number `
                                -Message "Qt $version does not match the canonical $canonical from .qt-version"))
                }
            }
        }
    }

    return , @($violations)
}

$violations = Test-DriftRule -Root $RepoRoot

if ($violations.Count -eq 0) {
    if (-not $Quiet) {
        Write-Host 'check-drift: OK (Qt version, Qt setup, Qt SDK paths, no qmake project files)'
    }
    exit 0
}

Write-Host 'check-drift: FAILED' -ForegroundColor Red
foreach ($violation in $violations) {
    $where = if ($violation.Line -gt 0) { "$($violation.File):$($violation.Line)" } else { $violation.File }
    Write-Host "  [$($violation.Rule)] ${where}: $($violation.Message)" -ForegroundColor Red
}
exit 1
