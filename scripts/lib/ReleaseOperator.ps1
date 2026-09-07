#Requires -Version 7.0
<#
.SYNOPSIS
    The operator protocol: one line per step, one window, one ordered sequence.

.DESCRIPTION
    The rc19 campaign found the same defect four times over, and it was never in
    the product: the line `[y] yes [n] no` meant "start it now" for the MSI and
    Chocolatey gates and "I already did it" for the present, audio and visual
    gates. One keystroke, two opposite meanings, decided by which screen of
    instructions had scrolled past.

    So there are exactly two ACTION forms here, and each says who acts next:

        [Enter] startet jetzt     the RUNNER acts when you press Enter
        [Enter] wenn erledigt     YOU act, and press Enter when you are done

    A third form exists only for the two sight checks, where the answer is not
    "I did it" but the verdict itself -- a person judging colour on a
    capture-excluded overlay is the measurement, and there is nothing else to
    ask. It is deliberately spelled with letters rather than Enter so it can
    never be answered by the reflex that answers the other two.

    Everything else the old gate printed -- why this is manual, what to expect,
    how the runner will verify it -- is available behind `?` and is not printed
    by default. Five paragraphs before every prompt is how the meaning of the
    prompt got lost in the first place.
#>

Set-StrictMode -Version Latest

# The reader seam. A dry run replaces it with a simulated operator, which is what
# lets every gate be observed RED once and GREEN once before a person is ever
# shown one. Nothing else in this file reads the console.
$script:ReleaseOperatorReader = $null

function Set-ReleaseOperatorReader {
    <#
    .SYNOPSIS
        Installs a simulated operator. Passing $null restores the real console.
    .DESCRIPTION
        The block receives the prompt text and returns the typed answer.
    #>
    param([scriptblock] $Reader)
    $script:ReleaseOperatorReader = $Reader
}

function Get-ReleaseOperatorPromptText {
    <#
    .SYNOPSIS
        The exact prompt line for one step kind.
    .DESCRIPTION
        One table, read by the prompt and by the tests that pin the vocabulary, so
        a third meaning cannot be introduced by editing a string literal in place.
    #>
    param([Parameter(Mandatory)] [ValidateSet('Start', 'Done', 'Judgement')] [string] $Kind)
    switch ($Kind) {
        'Start' { return '[Enter] startet jetzt' }
        'Done' { return '[Enter] wenn erledigt' }
        'Judgement' { return '[j] richtig  [n] falsch' }
    }
}

function Test-ReleaseOperatorPresent {
    <#
    .SYNOPSIS
        Whether anybody can answer a prompt at all.
    .DESCRIPTION
        A redirected stdin means no operator, and a gate that cannot be answered is
        DEFERRED rather than asked -- checked before the instructions are shown, so
        nobody performs a two-minute physical action that cannot be confirmed
        afterwards.

        An installed reader answers first, and that is the point: a dry run has a
        simulated operator, so there IS somebody to ask, and every gate can be
        observed reaching a verdict before a person is ever shown one. In a real
        run no reader is installed and the console is the only answer.
    #>
    if ($null -ne $script:ReleaseOperatorReader) { return $true }
    return -not [Console]::IsInputRedirected
}

function Read-ReleaseOperatorInput {
    param([Parameter(Mandatory)] [string] $Prompt)
    if ($null -ne $script:ReleaseOperatorReader) {
        $answer = & $script:ReleaseOperatorReader $Prompt
        if ($null -eq $answer) { return '' }
        return "$answer"
    }
    Write-Host "  $Prompt" -NoNewline
    $typed = Read-Host
    if ($null -eq $typed) { return '' }
    return $typed
}

function Write-ReleaseOperatorDetail {
    <#
    .SYNOPSIS
        The material the old gate printed unconditionally, printed only on `?`.
    #>
    param([Parameter(Mandatory)] $Step)
    $sections = [ordered]@{
        'why this needs a person' = 'Why'
        'expected'                = 'Expected'
        'verified by'             = 'VerifyDescription'
    }
    Write-Host ''
    foreach ($label in $sections.Keys) {
        $name = $sections[$label]
        if (-not $Step.ContainsKey($name)) { continue }
        $value = $Step[$name]
        if ([string]::IsNullOrWhiteSpace("$value")) { continue }
        Write-Host "    $label : $value" -ForegroundColor DarkGray
    }
    Write-Host ''
}

function Invoke-ReleaseOperatorStep {
    <#
    .SYNOPSIS
        Shows one line, waits for one keystroke, and answers who acts next.
    .DESCRIPTION
        Returns 'go', 'skip' or 'abort'. An unrecognised answer is asked again:
        a stray Enter or a pasted line must never decide anything, least of all
        something destructive.

        `Kind` is the whole contract. 'Start' promises the runner will act on
        Enter and nothing has happened yet; 'Done' promises the operator has
        already acted and the runner is about to verify. A step may not be shown
        with the wrong one -- see the sequence tests.
    #>
    param(
        [Parameter(Mandatory)] $Step,
        [switch] $Quiet
    )
    $kind = if ($Step.ContainsKey('Kind')) { "$($Step.Kind)" } else { 'Done' }
    $prompt = Get-ReleaseOperatorPromptText -Kind $kind
    if (-not $Quiet) {
        Write-Host ''
        Write-Host "$($Step.Id)  $($Step.Line)" -ForegroundColor Yellow
    }
    while ($true) {
        $typed = (Read-ReleaseOperatorInput -Prompt "$prompt   (? = details, s = skip, x = abort)").Trim().ToLowerInvariant()
        switch -Regex ($typed) {
            '^$' { return 'go' }
            '^\?+$' { Write-ReleaseOperatorDetail -Step $Step }
            '^(s|skip)$' { return 'skip' }
            '^(x|abort|q|quit)$' { return 'abort' }
            default {
                Write-Host '  Enter, or ? for details. Nothing recorded yet.' -ForegroundColor DarkGray
            }
        }
    }
}

function Invoke-ReleaseOperatorJudgement {
    <#
    .SYNOPSIS
        The sight check: the answer IS the verdict.
    .DESCRIPTION
        Returns a hashtable with `Answer` ('right', 'wrong', 'skip', 'abort') and,
        for 'wrong', the one-line `Reason` that reaches the report. A judgement is
        never spelled with Enter, so the reflex that answers an action prompt
        cannot pass a surface nobody looked at.
    #>
    param([Parameter(Mandatory)] $Step)
    Write-Host ''
    Write-Host "$($Step.Id)  $($Step.Line)" -ForegroundColor Yellow
    $prompt = Get-ReleaseOperatorPromptText -Kind 'Judgement'
    while ($true) {
        $typed = (Read-ReleaseOperatorInput -Prompt "$prompt   (? = details, s = skip, x = abort)").Trim().ToLowerInvariant()
        switch -Regex ($typed) {
            '^(j|y|ja|yes|ok)$' { return @{ Answer = 'right' } }
            '^(n|no|nein|f|fail)$' {
                $why = Read-ReleaseOperatorInput -Prompt 'What was wrong? (one line, recorded in the report)'
                if ([string]::IsNullOrWhiteSpace($why)) { $why = 'no detail given' }
                return @{ Answer = 'wrong'; Reason = $why.Trim() }
            }
            '^\?+$' { Write-ReleaseOperatorDetail -Step $Step }
            '^(s|skip)$' { return @{ Answer = 'skip' } }
            '^(x|abort|q|quit)$' { return @{ Answer = 'abort' } }
            default { Write-Host '  j or n, please. Nothing recorded yet.' -ForegroundColor DarkGray }
        }
    }
}

# ---------------------------------------------------------------------------
# The ordered sequence
# ---------------------------------------------------------------------------

function Get-ReleaseHumanPlanOrder {
    <#
    .SYNOPSIS
        Orders the selected scenarios so every declared dependency runs first.
    .DESCRIPTION
        The dependencies are real and each one cost a campaign a FAIL:

          REL-CAP-FSE-001       needs the elevated present session REL-PRESENT-002
                                establishes; a second, unelevated instance is
                                swallowed by the single-instance guard and reaches
                                no control channel at all.
          REL-UPD-MSI-001       needs the older installed build that
                                REL-UPD-MSI-DECLINE-001 still has; the accept gate
                                removes that starting point for good.
          REL-PKG-CHOCO-001     reinstalls the release MSI at the end, so it runs
                                after both update gates rather than under them.

        A stable sort, not an arbitrary topological one: within the constraints
        the catalog order is preserved, so `list` and a run read the same way. A
        cycle is reported rather than silently broken -- an unrunnable order is a
        catalog defect, and quietly picking one is how it would stay hidden.
    #>
    param([Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Entries)

    $byId = @{}
    foreach ($entry in $Entries) { $byId[$entry.Id] = $entry }

    $ordered = [System.Collections.Generic.List[object]]::new()
    $placed = @{}
    $visiting = @{}

    $place = $null
    $place = {
        param($entry, $chain)
        if ($placed.ContainsKey($entry.Id)) { return }
        if ($visiting.ContainsKey($entry.Id)) {
            throw "The scenario dependencies form a cycle: $(($chain + $entry.Id) -join ' -> ')"
        }
        $visiting[$entry.Id] = $true
        foreach ($dependency in @(Get-ReleaseScenarioDependency -Entry $entry)) {
            # A dependency that is not part of THIS selection is not an error: a
            # single `-Only REL-CAP-FSE-001` is a legitimate way to re-attempt one
            # gate, and the gate itself reports what it could not find.
            if ($byId.ContainsKey($dependency)) { & $place $byId[$dependency] ($chain + $entry.Id) }
        }
        $visiting.Remove($entry.Id)
        $placed[$entry.Id] = $true
        $ordered.Add($entry)
    }

    foreach ($entry in $Entries) { & $place $entry @() }
    return [object[]]$ordered.ToArray()
}

function Get-ReleaseScenarioDependency {
    param([Parameter(Mandatory)] $Entry)
    if ($Entry.PSObject.Properties.Name -notcontains 'DependsOn' -or $null -eq $Entry.DependsOn) { return @() }
    return @($Entry.DependsOn)
}

function Show-ReleaseHumanPlan {
    <#
    .SYNOPSIS
        Prints the whole human sequence once, before the first gate runs.
    .DESCRIPTION
        One window, one list, in the order it will happen, so an operator can see
        how long they are committing to and which steps depend on which. A gate
        that needs nobody is listed as `automated` rather than left out: knowing
        that fourteen of the sixteen steps ask nothing is the point.
    #>
    param([Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Entries)
    $human = @($Entries | Where-Object { Test-ReleaseScenarioAsksAPerson -Entry $_ })
    Write-Host ''
    Write-Host '== Sequence' -ForegroundColor Cyan
    $index = 1
    foreach ($entry in $Entries) {
        $asks = Test-ReleaseScenarioAsksAPerson -Entry $entry
        $marker = if ($asks) { 'you' } else { 'automated' }
        $colour = if ($asks) { 'Yellow' } else { 'DarkGray' }
        $after = @(Get-ReleaseScenarioDependency -Entry $entry)
        $depends = if ($after.Count -gt 0) { "  (after $($after -join ', '))" } else { '' }
        Write-Host ("  {0,2}. {1,-11} {2,-26} {3}{4}" -f $index, $marker, $entry.Id, $entry.Title, $depends) -ForegroundColor $colour
        $index++
    }
    Write-Host ''
    Write-Host "  $($human.Count) of $($Entries.Count) step(s) may ask you something." -ForegroundColor DarkGray
}

function Test-ReleaseScenarioAsksAPerson {
    <#
    .SYNOPSIS
        Whether a scenario can reach an operator prompt at all.
    .DESCRIPTION
        Declared per scenario rather than inferred from the layer: SECURE and
        MANUAL_* say where the truth comes from, and several of those gates now
        drive themselves with a tool and ask nobody. `AsksAPerson` is what the
        sequence view promises the operator, so it has to be the thing the gate
        actually does.
    #>
    param([Parameter(Mandatory)] $Entry)
    if ($Entry.PSObject.Properties.Name -notcontains 'AsksAPerson') { return $false }
    return [bool]$Entry.AsksAPerson
}
