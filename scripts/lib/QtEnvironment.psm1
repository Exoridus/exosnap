#Requires -Version 7.0
<#
.SYNOPSIS
    Resolves the Qt installation the local tooling must use.

.DESCRIPTION
    `.qt-version` is the canonical Qt version for this repository -- the CI
    setup action reads it, check-drift.ps1 enforces that no build machinery
    contradicts it, and the local scripts resolve their Qt paths through here.

    The point is that a Qt uplift is one edit. A version spelled out in a script
    is a second version source that keeps working after the uplift -- against the
    previous Qt -- which is the failure this module removes.

    The install root defaults to the standard online-installer location and is
    overridable with EXOSNAP_QT_ROOT; only the version underneath it is canonical.

    A missing install is NOT an error: Qt may already be on PATH, or installed
    somewhere else entirely, and that has always been tolerated. Falling back to
    a DIFFERENT version's directory never is.
#>

Set-StrictMode -Version Latest

# The canonical three-part Qt version. Throws with a readable message rather
# than returning something a caller could quietly build a wrong path from.
function Get-CanonicalQtVersion {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepoRoot
    )

    $path = Join-Path $RepoRoot '.qt-version'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw ".qt-version is missing at '$path'. It is the canonical Qt version for this repository."
    }

    $version = (Get-Content -LiteralPath $path -Raw).Trim()
    if ($version -notmatch '^\d+\.\d+\.\d+$') {
        throw ".qt-version contains '$version', which is not a three-part version (e.g. 6.11.1)."
    }
    return $version
}

# The msvc2022_64 install root for the canonical version, or $null when it is not
# present on this machine. Never another version's root.
function Resolve-QtInstallRoot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepoRoot
    )

    $version = Get-CanonicalQtVersion -RepoRoot $RepoRoot
    $installRoot = if ($env:EXOSNAP_QT_ROOT) { $env:EXOSNAP_QT_ROOT } else { Join-Path $env:SystemDrive '\Qt' }
    $root = Join-Path (Join-Path $installRoot $version) 'msvc2022_64'
    if (Test-Path -LiteralPath $root -PathType Container) {
        return $root
    }
    return $null
}

# Puts the canonical Qt's bin directory on PATH and its plugins on
# QT_PLUGIN_PATH. Returns $true when it did, $false when the install is absent
# (the caller is then relying on whatever Qt is already on PATH).
function Add-QtToPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$IncludePlugins
    )

    $root = Resolve-QtInstallRoot -RepoRoot $RepoRoot
    if ($null -eq $root) {
        $version = Get-CanonicalQtVersion -RepoRoot $RepoRoot
        Write-Host "Qt $version was not found under the expected install root; relying on Qt already on PATH." `
            -ForegroundColor Yellow
        return $false
    }

    $env:PATH = "$(Join-Path $root 'bin');$env:PATH"
    if ($IncludePlugins) {
        $plugins = Join-Path $root 'plugins'
        if (Test-Path -LiteralPath $plugins -PathType Container) {
            $env:QT_PLUGIN_PATH = $plugins
        }
    }
    return $true
}

Export-ModuleMember -Function Get-CanonicalQtVersion, Resolve-QtInstallRoot, Add-QtToPath
