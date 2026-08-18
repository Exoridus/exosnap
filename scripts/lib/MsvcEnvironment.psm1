#Requires -Version 7.0
<#
.SYNOPSIS
    Imports the MSVC toolchain environment into the current process.

.DESCRIPTION
    The Ninja presets need cl.exe, INCLUDE and LIB in the environment; the Visual
    Studio generator finds its own toolchain and does not. Rather than requiring a
    Developer PowerShell, this discovers the installation with vswhere and imports
    what vcvars64.bat produces.

    Two things about that batch file are not obvious, and both cost a working
    Ninja path when ignored.

    It inherits VSINSTALLDIR from the caller. A machine that has that variable set
    to an installation directory WITHOUT its trailing separator makes vcvars derive
    the toolchain directory as "<install>VC\" -- a path that does not exist -- and
    the VC initialization is then skipped in silence. The environment still comes
    back with INCLUDE, LIB and the Windows SDK set, so the failure only surfaces
    later, as CMake reporting no CMAKE_CXX_COMPILER. Every VS-derived variable is
    therefore cleared before the call rather than corrected: a value we do not
    supply cannot be the wrong one.

    Its exit code is not usable. Optional extension scripts that have nothing to do
    with the compiler (the bundled CMake, the connection manager) can fail and make
    the whole script report failure while the toolchain came up correctly. The
    result is validated by looking for cl.exe on the resulting PATH instead.

    Nothing here writes outside the current process: no user or machine variable is
    modified, and a second call is a no-op once cl.exe resolves.
#>

Set-StrictMode -Version Latest

# Cleared before vcvars runs. VSCMD_* is what a partially initialized Developer
# Prompt leaves behind and what makes vcvars think it has already run.
$script:InheritedVisualStudioVariables = @(
    'VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir', 'VCToolsRedistDir',
    'VCIDEInstallDir', 'VS170COMNTOOLS', 'VS180COMNTOOLS',
    'VSCMD_VER', 'VSCMD_ARG_HOST_ARCH', 'VSCMD_ARG_TGT_ARCH', 'VSCMD_VCVARSALL_INIT')

function Find-VisualStudioInstallation {
    <#
    .SYNOPSIS
        The installation path of the newest Visual Studio with the C++ toolset.
    .DESCRIPTION
        Returns $null when vswhere or a qualifying installation is absent. The
        caller decides whether that is fatal.
    #>
    [CmdletBinding()]
    param()

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { return $null }

    $found = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
    if (-not $found) { return $null }
    return $found.Trim()
}

function Get-MsvcEnvironmentBlock {
    <#
    .SYNOPSIS
        The environment vcvars64.bat produces, as an ordered name/value map.
    .PARAMETER InstallationPath
        A Visual Studio installation directory.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $InstallationPath)

    $vcvars = Join-Path $InstallationPath 'VC/Auxiliary/Build/vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
        throw "vcvars64.bat not found under '$InstallationPath'. The installation lacks the C++ build tools."
    }

    $clear = ($script:InheritedVisualStudioVariables | ForEach-Object { "set `"$_=`"" }) -join ' & '
    # `&` and not `&&`: the exit code is unusable, see the module description.
    $command = "$clear & call `"$vcvars`" >nul 2>&1 & set"

    $captured = [ordered]@{}
    foreach ($line in (& cmd /c $command)) {
        $split = $line.IndexOf('=')
        if ($split -lt 1) { continue }
        $captured[$line.Substring(0, $split)] = $line.Substring($split + 1)
    }
    return $captured
}

function Find-CompilerOnPath {
    <#
    .SYNOPSIS
        cl.exe as reached from a PATH value, or $null.
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)] [AllowEmptyString()] [string] $PathValue)

    foreach ($directory in ($PathValue -split ';')) {
        if (-not $directory) { continue }
        $candidate = Join-Path $directory 'cl.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

function Enter-MsvcEnvironment {
    <#
    .SYNOPSIS
        Makes cl.exe and the MSVC headers and libraries reachable from this process.
    .DESCRIPTION
        A no-op when cl.exe already resolves, so calling it from a Developer
        PowerShell costs nothing and cannot downgrade an environment the caller
        set up deliberately.

        Throws when no installation with the C++ toolset is present, or when the
        imported environment still has no compiler in it. It never returns having
        silently done nothing.
    .PARAMETER Quiet
        Suppress the informational line naming the compiler that was imported.
    .OUTPUTS
        The full path of the compiler now in use.
    #>
    [CmdletBinding()]
    param([switch] $Quiet)

    $existing = Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($existing) { return $existing.Source }

    $installation = Find-VisualStudioInstallation
    if (-not $installation) {
        throw 'No Visual Studio installation with the x64 C++ toolset was found. Install the "Desktop development with C++" workload.'
    }

    $environment = Get-MsvcEnvironmentBlock -InstallationPath $installation

    $pathKey = @($environment.Keys | Where-Object { $_ -ieq 'Path' }) | Select-Object -First 1
    $compiler = if ($pathKey) { Find-CompilerOnPath -PathValue $environment[$pathKey] } else { $null }
    if (-not $compiler) {
        throw ("vcvars64.bat ran for '$installation' but produced no cl.exe. " +
               'Run it by hand with VSCMD_DEBUG=2 to see which initialization step failed.')
    }

    foreach ($name in $environment.Keys) {
        Set-Item -LiteralPath "Env:$name" -Value $environment[$name]
    }

    if (-not $Quiet) { Write-Host "msvc: $compiler" -ForegroundColor DarkGray }
    return $compiler
}

Export-ModuleMember -Function Enter-MsvcEnvironment, Find-VisualStudioInstallation, Get-MsvcEnvironmentBlock, Find-CompilerOnPath
