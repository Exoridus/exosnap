<#
.SYNOPSIS
    Runs the curated, BLOCKING clang-tidy check set over the project's own sources.

.DESCRIPTION
    The repository-wide .clang-tidy configuration enables a broad check set that is
    still advisory (see the advisory-unused-checks job in .github/workflows/advisory-checks.yml).
    This script runs only the small curated subset listed in $BlockingChecks below.
    Each of those was measured at zero findings in repository-owned files across
    all 509 project translation units, which is what makes it safe to fail a pull
    request on. .clang-tidy carries the same list plus the candidates that did not
    qualify and why.

    Scope. Analysed are the translation units in the Ninja build's
    compile_commands.json that live inside the repository but outside the build
    tree and outside third_party/ -- app/, apps/, libs/, tools/, tests/.

    -Base restricts that set to what a change actually affects: the changed
    translation units themselves, plus every translation unit that includes a
    changed header (resolved from Ninja's recorded dependency graph, so a header
    edit still reaches its consumers), plus the canary units below when the
    analysis configuration itself is what changed. A full-tree pass measures
    ~5.6 CPU-hours, nearly all of it clang re-parsing Qt headers, which is why CI
    runs the change-scoped form and the full form is a local / on-demand
    operation.

    What -Base does NOT cover: a change that alters how the code is compiled
    without touching a source file, a header, or one of $CanaryTriggers -- new
    compiler flags in a CMakeLists, a different toolchain, a preset edit. Those
    can move findings in units this run never looks at. Re-run the full pass by
    hand after such a change.

    Analyser note. clang-tidy registers the entire clang-analyzer core package as
    soon as any clang-analyzer-* check is requested; -clang-analyzer-core.X cannot
    turn the extras back off. Only the checks named in $BlockingChecks count as a
    violation here, so the extras stay out of the verdict.

.PARAMETER BuildDir
    Directory containing compile_commands.json. Defaults to the Ninja debug preset.

.PARAMETER Base
    Git revision to diff against. When given, only the affected translation units
    are analysed. Omit for a full-tree pass. A missing Ninja dependency graph is a
    hard error in this mode, not a reason to analyse less.

.PARAMETER ClangTidy
    Explicit path to clang-tidy.exe. Autodetected from the Visual Studio LLVM
    toolset and then from PATH when omitted.

.PARAMETER Jobs
    Parallel clang-tidy processes. Defaults to the processor count.

.PARAMETER ListChecks
    Print the blocking check list and exit.

.EXAMPLE
    pwsh -NonInteractive -File scripts/run-clang-tidy-blocking.ps1 -Base HEAD^

.EXAMPLE
    pwsh -NonInteractive -File scripts/run-clang-tidy-blocking.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/windows-x64-ninja-debug',
    [string]$Base,
    [string]$ClangTidy,
    [int]$Jobs = 0,
    [switch]$ListChecks
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# The blocking set. Every entry was verified to produce zero findings over all
# project translation units before it was added. Do not extend this list without
# re-running the full pass (no -Base) and confirming a clean result.
# ---------------------------------------------------------------------------
$BlockingChecks = @(
    'bugprone-use-after-move'
    'bugprone-dangling-handle'
    'clang-analyzer-core.CallAndMessage'
    'clang-analyzer-core.uninitialized.*'
    'clang-analyzer-cplusplus.NewDelete*'
)

if ($ListChecks) {
    $BlockingChecks | ForEach-Object { Write-Host $_ }
    exit 0
}

# ---------------------------------------------------------------------------
# Canary translation units, analysed whenever the analysis configuration itself
# changes rather than the code it inspects. A change to .clang-tidy or to this
# script alters what every future run means, but touches no .cpp and no header,
# so the change-scoped selection below would pick nothing and report green. One
# representative unit per compilation flavour keeps that class of change honest
# at a cost of a couple of minutes:
#   * engine, no Qt, heavy Win32/D3D interop
#   * Qt widget code, the expensive parse
#   * a gtest unit, where the test-only idioms live
# ---------------------------------------------------------------------------
$CanaryTriggers = @('.clang-tidy', 'scripts/run-clang-tidy-blocking.ps1')
$CanarySources = @(
    'libs/recorder_core/src/audio_thread.cpp'
    'app/ui/widgets/AudioSourceToggle.cpp'
    'libs/recorder_core/tests/test_split_sentinel_policy.cpp'
)

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

$compileDb = Join-Path $BuildDir 'compile_commands.json'
if (-not (Test-Path -LiteralPath $compileDb)) {
    Write-Host "::error::compile_commands.json not found at $compileDb"
    Write-Host "Configure the Ninja preset first: cmake --preset windows-x64-ninja-debug"
    exit 1
}

# clang-tidy ships with the Visual Studio LLVM toolset on the CI runners and on a
# standard developer install. This gate blocks the PR, so a missing tool is a hard
# failure -- skipping would make the gate silently meaningless.
if (-not $ClangTidy) {
    $vsRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    if (Test-Path -LiteralPath $vsRoot) {
        $ClangTidy = Get-ChildItem -LiteralPath $vsRoot -Recurse -Filter 'clang-tidy.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\Llvm\\x64\\bin\\' } |
            Select-Object -ExpandProperty FullName -First 1
    }
}
if (-not $ClangTidy) {
    $ClangTidy = (Get-Command clang-tidy -ErrorAction SilentlyContinue)?.Source
}
if (-not $ClangTidy) {
    Write-Host "::error::clang-tidy.exe not found (Visual Studio LLVM toolset or PATH)."
    exit 1
}

$normalizedRoot = $repoRoot.Replace('\', '/').TrimEnd('/')
$normalizedBuild = $BuildDir.Replace('\', '/').TrimEnd('/')

# Paths reach this from three sources (compile_commands.json, git, clang-tidy
# diagnostics) that do not agree on case, so every prefix test is ordinal
# case-insensitive.
$ic = [System.StringComparison]::OrdinalIgnoreCase

# Both Ninja's dependency lists and clang-tidy's diagnostics can name a file
# relative to the build directory (`../../libs/x.h`). Those have to be resolved
# before any prefix test, or a real finding in a repository header would be
# mistaken for a third-party one and dropped.
function Resolve-AgainstBuildDir {
    param([string]$Path)
    $p = $Path.Replace('\', '/')
    if (-not [System.IO.Path]::IsPathRooted($p)) {
        $p = [System.IO.Path]::GetFullPath((Join-Path $BuildDir $p)).Replace('\', '/')
    }
    return $p
}

function Test-InRepo {
    param([string]$Path)
    return $Path.StartsWith("$normalizedRoot/", $ic) -and
           -not $Path.StartsWith("$normalizedBuild/", $ic) -and
           $Path -notmatch '(?i)/third_party/'
}

function Test-ProjectSource {
    param([string]$Path)
    return $Path -match '(?i)\.(cpp|cxx|cc|c)$' -and (Test-InRepo $Path)
}

$db = Get-Content -LiteralPath $compileDb -Raw | ConvertFrom-Json
$allSources = $db.file | ForEach-Object { $_.Replace('\', '/') } | Where-Object { Test-ProjectSource $_ } | Sort-Object -Unique

if (-not $allSources) {
    Write-Host "::error::No project translation units found in $compileDb"
    exit 1
}

$sources = $allSources
$scope = 'full tree'

if ($Base) {
    $changedAll = @(git -C $repoRoot diff --name-only --diff-filter=ACMR "$Base" -- . 2>$null)
    if ($LASTEXITCODE -ne 0) {
        Write-Host "::error::git diff against '$Base' failed - is the base revision fetched?"
        exit 1
    }
    $changedAll = @($changedAll | ForEach-Object { $_.Replace('\', '/') })
    $changed = @($changedAll | Where-Object { $_ -match '(?i)\.(cpp|cxx|cc|c|h|hpp|hxx|inl)$' })

    $canaryHit = @($changedAll | Where-Object { $CanaryTriggers -contains $_ })

    if (-not $changed -and -not $canaryHit) {
        Write-Host "No C/C++ sources or headers changed since $Base - nothing to analyse."
        exit 0
    }

    # Built element-wise: when only a canary changed, $changed is empty, and an
    # empty pipeline casts to $null — the HashSet collection constructor throws
    # on that ("Value cannot be null"), killing exactly the runs the canary
    # list exists to protect.
    $changedAbs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($c in $changed) { [void]$changedAbs.Add("$normalizedRoot/$c") }

    $affected = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($s in $allSources) { if ($changedAbs.Contains($s)) { [void]$affected.Add($s) } }

    # The analysis configuration changed, so pull in the canaries (see the list
    # near the top for why). A canary that is missing from the compile database
    # is a stale list, not a reason to quietly analyse less.
    if ($canaryHit) {
        Write-Host "Analysis configuration changed ($($canaryHit -join ', ')) - adding canary translation units."
        foreach ($c in $CanarySources) {
            $abs = "$normalizedRoot/$c"
            if ($allSources -contains $abs) {
                [void]$affected.Add($abs)
            }
            else {
                Write-Host "::error::Canary translation unit '$c' is not in the compile database."
                Write-Host "Update `$CanarySources in this script to name files that still exist."
                exit 1
            }
        }
    }

    # A changed header must reach the translation units that include it. Ninja
    # recorded exactly that during the build (/showIncludes -> .ninja_deps), so
    # the fan-out comes from the real dependency graph rather than a guess.
    $changedHeaders = @($changed | Where-Object { $_ -match '\.(h|hpp|hxx|inl)$' })
    if ($changedHeaders) {
        $objToSource = @{}
        foreach ($e in $db) {
            if ($e.output) {
                $out = $e.output.Replace('\', '/')
                if ($out.StartsWith("$normalizedBuild/")) { $out = $out.Substring($normalizedBuild.Length + 1) }
                $objToSource[$out] = $e.file.Replace('\', '/')
            }
        }

        $depLines = @()
        Push-Location $BuildDir
        try { $depLines = @(& ninja -t deps 2>$null) } catch { $depLines = @() } finally { Pop-Location }

        # Fail closed. Without the dependency graph the consumers of a changed
        # header are unknown, and a header-only change would then analyse nothing
        # and report green - the gate would be silently absent exactly where it
        # matters most. An unavailable graph is an infrastructure fault, so it
        # goes red rather than quietly narrowing the scope.
        if (-not $depLines) {
            Write-Host "::error::'ninja -t deps' returned nothing for $BuildDir."
            Write-Host "The changed headers ($($changedHeaders.Count)) cannot be mapped to the translation"
            Write-Host "units that include them, so this run cannot prove anything. Rebuild the Ninja"
            Write-Host "preset so .ninja_deps exists, or run the full pass without -Base."
            exit 1
        }

        $currentSource = $null
        foreach ($line in $depLines) {
            if ($line -match '^(\S.*?):\s+#deps ') {
                $obj = $Matches[1].Replace('\', '/')
                $currentSource = $objToSource[$obj]
                continue
            }
            if (-not $currentSource) { continue }
            if ($line -match '^\s+(\S.*)$') {
                $dep = Resolve-AgainstBuildDir $Matches[1]
                if ($changedAbs.Contains($dep) -and (Test-ProjectSource $currentSource)) {
                    [void]$affected.Add($currentSource)
                }
            }
        }
    }

    $sources = @($affected) | Sort-Object
    $scope = "changed since $Base"
    if (-not $sources) {
        Write-Host "No analysable translation unit is affected by the changes since $Base."
        exit 0
    }
}

if ($Jobs -le 0) {
    $Jobs = [int]$env:NUMBER_OF_PROCESSORS
    if ($Jobs -le 0) { $Jobs = 4 }
}

$checksArg = '-*,' + ($BlockingChecks -join ',')
# Positive header allowlist: the project's own headers only, never the build tree
# or third_party/. clang-tidy's regex engine has no lookahead, so an exclusion
# pattern is not expressible here.
$headerFilter = '[/\\](app|apps|libs|tools|tests)[/\\]'

Write-Host "clang-tidy      : $ClangTidy"
Write-Host "compile database: $compileDb"
Write-Host "blocking checks : $($BlockingChecks -join ', ')"
Write-Host "scope           : $scope - $($sources.Count) translation unit(s), $Jobs parallel job(s)"
Write-Host ''

$results = $sources | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $output = & $using:ClangTidy `
        -p $using:BuildDir `
        --quiet `
        --checks=$using:checksArg `
        --header-filter=$using:headerFilter `
        $_ 2>&1 | Out-String
    [pscustomobject]@{ File = $_; Output = $output }
}

# clang-tidy's exit code is NOT the verdict here. Two pre-existing conditions make
# it non-zero for reasons unrelated to the blocking set, both of them long-standing
# and orthogonal to this gate:
#   * clang parses in MSVC-compat mode and rejects a handful of constructs cl.exe
#     accepts (MSVC's non-constexpr offsetof macro, the vendored PresentMon ETW
#     headers) -- clang-diagnostic-error, not a check finding;
#   * findings inside Qt and Windows SDK headers, which are not ours to fix.
# The verdict is therefore: a diagnostic from a blocking check, located in a file
# this repository owns.
$diagPattern = '^(.*?):(\d+):(\d+):\s+(?:warning|error):\s+(.*?)\s+\[([^\]]+)\]\s*$'
$blockingRegex = '^(?:' + (($BlockingChecks | ForEach-Object { [regex]::Escape($_).Replace('\*', '[^,\]]*') }) -join '|') + ')$'

$violations = [ordered]@{}
foreach ($r in $results) {
    foreach ($line in ($r.Output -split "`r?`n")) {
        if ($line -notmatch $diagPattern) { continue }
        # Copy every capture out before anything else runs a regex: the next
        # -match/-notmatch replaces $Matches wholesale.
        $where = Resolve-AgainstBuildDir $Matches[1]
        $row = $Matches[2]
        $col = $Matches[3]
        $message = $Matches[4]
        $checks = $Matches[5] -replace ',-warnings-as-errors', ''
        if (-not (Test-InRepo $where)) { continue }
        $rel = $where.Substring($normalizedRoot.Length + 1)
        foreach ($check in ($checks -split ',')) {
            if ($check -notmatch $blockingRegex) { continue }
            $violations["${rel}:${row}:${col} [$check] $message"] = $true
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "::error::clang-tidy blocking check violations: $($violations.Count)"
    $violations.Keys | Sort-Object | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host 'These checks are required to stay at zero findings. Fix the code, or take'
    Write-Host 'the check out of $BlockingChecks in this script and out of .clang-tidy.'
    exit 1
}

Write-Host "clang-tidy blocking check set: clean ($($sources.Count) translation unit(s))."
exit 0
