#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the source-hygiene scanner.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case is a throwaway git repository in the temp directory. The two
    properties worth pinning are opposites of each other:

      a comment carrying provenance is reported;
      the same token in code, in a string literal, or as legitimate technical
      vocabulary is not.

    The second half is what decides whether a rule survives contact with a real
    tree. This repository ships diagnostic identifiers shaped exactly like ticket
    numbers (ART-001, ENV-001) and writes SHA-256, UTF-8 and \\.\pipe\ in
    comments on purpose, so those all appear here as cases that must stay silent.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$scanner = Join-Path $scriptRoot 'check-source-hygiene.ps1'

# A git hook exports GIT_DIR/GIT_INDEX_FILE to everything it starts, and GIT_DIR
# beats `git -C`, so a fixture built under a hook would initialise and re-index
# the real repository instead. See check-drift.tests.ps1 for the incident.
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

function Invoke-Scanner {
    <#
    .SYNOPSIS
        Runs the scanner over a one-file fixture repository.
    .DESCRIPTION
        The file is committed and then scanned with -All, so the result depends
        only on the content, never on what the diff machinery considers changed.
    #>
    param(
        [Parameter(Mandatory)] [string] $RelativePath,
        [Parameter(Mandatory)] [string] $Content
    )

    $root = Join-Path ([IO.Path]::GetTempPath()) "source-hygiene-tests/$([guid]::NewGuid().ToString('n'))"
    $path = Join-Path $root $RelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    try {
        Set-Content -LiteralPath $path -Value $Content -Encoding utf8
        Invoke-IsolatedGit -C $root init --quiet
        Invoke-IsolatedGit -C $root add -A

        $output = & pwsh -NoProfile -NonInteractive -File $scanner -RepoRoot $root -All 2>&1 | Out-String
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

function Assert-Reported {
    param([string] $RelativePath, [string] $Content, [string] $Rule)
    $result = Invoke-Scanner -RelativePath $RelativePath -Content $Content
    Assert-True ($result.Output -match [regex]::Escape($Rule)) `
        "'$Rule' was not reported. Scanner said:`n$($result.Output)"
}

function Assert-Silent {
    param([string] $RelativePath, [string] $Content)
    $result = Invoke-Scanner -RelativePath $RelativePath -Content $Content
    Assert-True ($result.Output -notmatch 'in source comment') `
        "nothing should have been reported, but the scanner said:`n$($result.Output)"
}

Write-Host ''
Write-Host 'Provenance in comments is reported'

Test-Case 'a tracker reference in a C++ comment' {
    Assert-Reported 'app/x.cpp' "// Retry once, see QCR-804 for the trace.`nint x = 1;`n" 'task-id'
}

Test-Case 'a tracker reference in a doc comment' {
    Assert-Reported 'app/x.h' "/** Loads the asset. Added for BUG-42. */`nvoid load();`n" 'task-id'
}

Test-Case 'an issue number' {
    Assert-Reported 'app/x.cpp' "// Works around the crash from #321.`nint x = 1;`n" 'issue-reference'
}

Test-Case 'a private workspace reference' {
    Assert-Reported 'app/x.cpp' "// Rationale in .workspace/plans/audio.md`nint x = 1;`n" 'private-workspace'
}

Test-Case 'a machine-specific path' {
    Assert-Reported 'app/x.cpp' "// Qt lives in C:\Qt on the build box.`nint x = 1;`n" 'absolute-path'
}

Test-Case 'a network path' {
    Assert-Reported 'app/x.cpp' "// Fixtures are on \\buildshare\media.`nint x = 1;`n" 'unc-path'
}

Test-Case 'conversation provenance' {
    Assert-Reported 'app/x.cpp' "// The user asked for the tray icon to stay put.`nint x = 1;`n" 'conversation-provenance'
}

Test-Case 'agent provenance' {
    Assert-Reported 'app/x.cpp' "// Claude suggested hoisting this out of the loop.`nint x = 1;`n" 'agent-provenance'
}

Test-Case 'a non-ASCII dash' {
    Assert-Reported 'app/x.cpp' "// Clamp the timestamp `u{2014} the source clock can go backwards.`nint x = 1;`n" 'non-ascii-punctuation'
}

Test-Case 'provenance in a PowerShell comment' {
    Assert-Reported 'scripts/x.ps1' "# See QCR-208 for why this retries.`n`$x = 1`n" 'task-id'
}

Test-Case 'provenance in a CMake comment' {
    Assert-Reported 'app/CMakeLists.txt' "# Kept for BUG-7.`nadd_library(x)`n" 'task-id'
}

Test-Case 'provenance in a QML comment' {
    Assert-Reported 'app/x.qml' "// Widened for QCR-101.`nItem { }`n" 'task-id'
}

Write-Host ''
Write-Host 'Code, data and legitimate vocabulary are left alone'

Test-Case 'a ticket-shaped identifier in a string literal is data' {
    # ExoSnap ships diagnostic identifiers of exactly this shape. Reporting them
    # would make the rule unusable in the tree it was written for.
    Assert-Silent 'app/x.cpp' "const char* id = `"QCR-804`";`nconst char* other = `"BUG-42`";`n"
}

Test-Case 'technical vocabulary that looks like a ticket' {
    Assert-Silent 'app/x.cpp' "// Hash with SHA-256, decode as UTF-8, verify the CRC-32.`nint x = 1;`n"
}

Test-Case 'a product diagnostic identifier in a comment' {
    Assert-Silent 'app/x.cpp' "// Emits ART-001 when the artifact is missing.`nint x = 1;`n"
}

Test-Case 'the Win32 device namespace is not a network path' {
    Assert-Silent 'app/x.cpp' "// The endpoint is \\.\pipe\exosnap and \\?\ removes the MAX_PATH limit.`nint x = 1;`n"
}

Test-Case 'a hex-looking word that is not a commit' {
    # "defaced" is seven characters of [a-f]. Confirming candidates against the
    # repository is what keeps the commit-hash rule from firing on English.
    Assert-Silent 'app/x.cpp' "// The surface was defaced by the previous blit.`nint x = 1;`n"
}

Test-Case 'a preprocessor directive is not an issue number' {
    Assert-Silent 'app/x.cpp' "#include <cstdio>`n#define X 1`nint x = 1;`n"
}

Test-Case 'a URL in code is not a comment' {
    Assert-Silent 'app/x.cpp' "const char* u = `"https://example.com/a#321`";`n"
}

Test-Case 'clean code with a justified comment' {
    Assert-Silent 'app/x.cpp' @'
// Clamp against the source timestamp rather than wall time, so presentation
// stays monotonic when the capture stalls.
timestamp = std::max(timestamp, previous);
'@
}

Write-Host ''
Write-Host 'Staged adoption'

Test-Case 'the bulk rules report without blocking' {
    # A gate that is red on arrival gets switched off rather than obeyed. The two
    # rules with a large existing backlog report and return success; the precise
    # ones fail the run.
    $result = Invoke-Scanner -RelativePath 'app/x.cpp' -Content "// Fixed under QCR-804.`nint x = 1;`n"
    Assert-True ($result.Output -match 'ADVISORY') 'the backlog rules must be reported as advisory'
    Assert-True ($result.ExitCode -eq 0) 'an advisory-only finding must not fail the run'
}

Test-Case 'a precise rule fails the run' {
    $result = Invoke-Scanner -RelativePath 'app/x.cpp' -Content "// See .workspace/plans/x.md`nint x = 1;`n"
    Assert-True ($result.ExitCode -ne 0) 'a blocking finding must fail the run'
    Assert-True ($result.Output -match 'blocking finding') 'the count of blocking findings must be stated'
}

Test-Case 'a clean file exits 0' {
    $result = Invoke-Scanner -RelativePath 'app/x.cpp' -Content "int x = 1;`n"
    Assert-True ($result.ExitCode -eq 0) 'a clean tree must pass'
}

Test-Case 'test fixtures are not scanned' {
    # This file is full of the shapes the rules reject; that is what a rejected
    # fixture is. It blocked this scanner's own first commit before the
    # exclusion existed.
    $result = Invoke-Scanner -RelativePath 'scripts/tests/some.tests.ps1' `
        -Content "# See QCR-804 and .workspace/plans/x.md and https://example.com/a#321`n"
    Assert-True ($result.ExitCode -eq 0) "a fixture must not be read as production source:`n$($result.Output)"
    Assert-True ($result.Output -notmatch 'in source comment') 'nothing in a fixture should be reported'
}

Write-Host ''
Write-Host 'Output shape'

Test-Case 'a finding is three lines: where, what, what to do' {
    $result = Invoke-Scanner -RelativePath 'app/x.cpp' -Content "// See .workspace/plans/x.md`nint x = 1;`n"
    # \s*$ rather than a bare $: PowerShell captures CRLF, and in .NET
    # multiline mode $ anchors ahead of the newline with the carriage
    # return still on the line.
    Assert-True ($result.Output -match '(?m)^source-hygiene: app/x\.cpp:1\s*$') 'the location must be a clickable path:line'
    Assert-True ($result.Output -match '(?m)^private-workspace in source comment:') 'the rule and the offending text must be named'
    Assert-True ($result.Output -notmatch '(?i)policy|section|according to') `
        'the message must carry the fix, not a restatement of the policy -- it is re-read on every hook retry'
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
