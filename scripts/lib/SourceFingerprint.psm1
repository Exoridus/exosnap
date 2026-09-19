#Requires -Version 7.0
<#
.SYNOPSIS
    The identity of the source a build or a test result is about.

.DESCRIPTION
    A test result is only evidence if it can name what it was produced from. This
    is that name: a content-derived digest of HEAD, of every tracked modification
    and of the CONTENT of every untracked file in the working tree.

    Timestamps are not usable for the question. A restored or copied tree keeps the
    mtimes it was created with, so a stale tree can look newer than the source it
    is missing.
#>

Set-StrictMode -Version Latest

function Get-SourceFingerprint {
    <#
    .SYNOPSIS
        A content digest of the working tree, or a stated failure to produce one.
    .DESCRIPTION
        The digest covers HEAD, `git diff HEAD` and the content of every file
        `git ls-files --others --exclude-standard` reports. Untracked files are
        hashed, not merely listed: a new source file the build compiles is
        untracked until it is added, so a listing by name would report the same
        digest before and after every edit to it.

        WHAT A PAIR OF EQUAL DIGESTS DOES NOT PROVE. Taken at two moments, they do
        not prove the tree was untouched in between -- an edit that was made and
        reverted leaves no trace here. Combined with a lock that keeps other
        cooperating entry points out, they do prove that the source a build was
        made from is the source its result is attributed to.

        Every failure is reported rather than absorbed: ok = $false with a reason,
        never an empty digest that would compare equal to another failure.

    .PARAMETER RepoRoot
        The working tree to identify.

    .PARAMETER MaxUntrackedFiles
        Above this many untracked files the tree is not treated as a source tree
        and no digest is produced. Generated output somewhere under the root is
        the usual cause, and hashing it would make the digest a statement about
        build products.

    .PARAMETER MaxUntrackedBytes
        The same limit by volume.

    .OUTPUTS
        ok, reason, head, dirty, fingerprint, untracked_files, untracked_bytes.
    #>
    [OutputType([pscustomobject])]
    param(
        [Parameter(Mandatory)] [string] $RepoRoot,
        [int] $MaxUntrackedFiles = 4000,
        [long] $MaxUntrackedBytes = 256MB
    )

    function New-Failure {
        param([string] $Reason)
        return [pscustomobject]@{
            ok = $false; reason = $Reason; head = $null; dirty = $null
            fingerprint = $null; untracked_files = 0; untracked_bytes = 0L
        }
    }

    $head = (& git -C $RepoRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $head) { return New-Failure 'git rev-parse HEAD failed' }

    $diff = (& git -C $RepoRoot diff HEAD 2>$null | Out-String)
    if ($LASTEXITCODE -ne 0) { return New-Failure 'git diff HEAD failed' }

    $untracked = @(& git -C $RepoRoot ls-files --others --exclude-standard 2>$null |
        Where-Object { $_.Trim() -ne '' } | Sort-Object)
    if ($LASTEXITCODE -ne 0) { return New-Failure 'git ls-files --others failed' }

    if ($untracked.Count -gt $MaxUntrackedFiles) {
        return New-Failure "$($untracked.Count) untracked files exceed the $MaxUntrackedFiles this run will hash"
    }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $parts = [System.Collections.Generic.List[string]]::new()
        $parts.Add($head.Trim())
        $parts.Add($diff)

        $untrackedBytes = 0L
        foreach ($relative in $untracked) {
            $full = Join-Path $RepoRoot $relative
            try {
                $info = Get-Item -LiteralPath $full -Force -ErrorAction Stop
                if ($info.PSIsContainer) { continue }
                $untrackedBytes += $info.Length
                if ($untrackedBytes -gt $MaxUntrackedBytes) {
                    return New-Failure "untracked files exceed $([int]($MaxUntrackedBytes / 1MB)) MB; not hashed"
                }
                $content = [System.IO.File]::ReadAllBytes($full)
            }
            catch {
                # A file git listed and this run cannot read is an unknown input,
                # not an absent one.
                return New-Failure "untracked file '$relative' could not be read: $($_.Exception.Message)"
            }
            $parts.Add($relative)
            $parts.Add([BitConverter]::ToString($sha.ComputeHash($content)).Replace('-', ''))
        }

        $bytes = [Text.Encoding]::UTF8.GetBytes(($parts -join "`n"))
        $digest = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }

    return [pscustomobject]@{
        ok              = $true
        reason          = $null
        head            = $head.Trim()
        dirty           = [bool]($diff.Trim() -ne '' -or $untracked.Count -gt 0)
        fingerprint     = $digest
        untracked_files = $untracked.Count
        untracked_bytes = $untrackedBytes
    }
}

Export-ModuleMember -Function Get-SourceFingerprint
