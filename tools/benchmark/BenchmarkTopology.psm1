# Physical display topology checks for the frontend A/B benchmark.
#
# The campaign is only meaningful if the workload renders on the capture display
# and ExoSnap is visible on the other one. Neither Superposition nor Windows will
# tell us that went right, so the orchestrator asserts it — and aborts instead of
# quietly benchmarking the wrong monitor.
#
# Identity is established from three independent facts (primary flag, pixel mode,
# EDID model string), because a numeric enumeration index is not stable across a
# driver reset or a cable swap.

Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Windows.Forms | Out-Null

if (-not ('ExoSnap.Benchmark.DisplayMode' -as [type])) {
    Add-Type -Namespace 'ExoSnap.Benchmark' -Name 'DisplayMode' -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion;
    public short dmDriverVersion;
    public short dmSize;
    public short dmDriverExtra;
    public int   dmFields;
    public int   dmPositionX;
    public int   dmPositionY;
    public int   dmDisplayOrientation;
    public int   dmDisplayFixedOutput;
    public short dmColor;
    public short dmDuplex;
    public short dmYResolution;
    public short dmTTOption;
    public short dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels;
    public int   dmBitsPerPel;
    public int   dmPelsWidth;
    public int   dmPelsHeight;
    public int   dmDisplayFlags;
    public int   dmDisplayFrequency;
    public int   dmICMMethod;
    public int   dmICMIntent;
    public int   dmMediaType;
    public int   dmDitherType;
    public int   dmReserved1;
    public int   dmReserved2;
    public int   dmPanningWidth;
    public int   dmPanningHeight;
}

[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

public static DEVMODE CurrentMode(string deviceName) {
    DEVMODE mode = new DEVMODE();
    mode.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
    EnumDisplaySettings(deviceName, -1, ref mode);
    return mode;
}
'@
}

function Get-EdidMonitorModel {
    <#
    .SYNOPSIS
        EDID model names, in the order WMI enumerates monitors.
    .DESCRIPTION
        WmiMonitorID exposes the panel's own name (e.g. "27GL850"), which is the
        only display fact that survives a display-index reshuffle. Returns an
        empty array when the namespace is unavailable rather than throwing: an
        unreadable EDID must degrade the check, not abort the campaign before it
        has said why.
    #>
    try {
        $ids = Get-CimInstance -Namespace 'root\wmi' -ClassName 'WmiMonitorID' -ErrorAction Stop
    } catch {
        Write-Warning "EDID model names unavailable ($($_.Exception.Message)); model matching will be skipped."
        return @()
    }
    $models = @()
    foreach ($id in $ids) {
        $name = ''
        if ($id.UserFriendlyName) {
            $name = -join ($id.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
        }
        $models += [pscustomobject]@{
            InstanceName = $id.InstanceName
            Model        = $name.Trim()
        }
    }
    return $models
}

function Get-BenchmarkDisplayTopology {
    <#
    .SYNOPSIS
        Every attached display with the facts the campaign asserts on.
    #>
    # Deliberately NOT paired with the EDID list.
    #
    # WMI enumerates monitors in its own order, and on this machine that order is
    # the REVERSE of the Screen order: Screen 0 is the primary 2560x1440 panel
    # while WmiMonitorID entry 0 is the 1080p 27GL650F. Pairing by position
    # therefore attaches the wrong model name to the right screen — a correlation
    # that is not merely unverified but actively misleading, and one that would
    # have silently mislabelled every archived run manifest.
    #
    # The model set is asserted separately (Test-BenchmarkTopology), which needs no
    # per-screen correlation to be conclusive.
    $result = @()
    $index = 0
    foreach ($screen in [System.Windows.Forms.Screen]::AllScreens) {
        $mode = [ExoSnap.Benchmark.DisplayMode]::CurrentMode($screen.DeviceName)
        $result += [pscustomobject]@{
            Index       = $index
            DeviceName  = $screen.DeviceName
            Primary     = $screen.Primary
            X           = $screen.Bounds.X
            Y           = $screen.Bounds.Y
            Width       = $screen.Bounds.Width
            Height      = $screen.Bounds.Height
            RefreshHz   = $mode.dmDisplayFrequency
            BitsPerPel  = $mode.dmBitsPerPel
        }
        $index++
    }
    return $result
}

function Test-BenchmarkTopology {
    <#
    .SYNOPSIS
        Asserts the scenario's expected capture/UI displays against reality.
    .OUTPUTS
        A result object with Ok, Problems, CaptureDisplay and UiDisplay.
    #>
    param(
        [Parameter(Mandatory)] $Expected,   # scenario.topology
        $Topology = (Get-BenchmarkDisplayTopology)
    )

    $problems = @()
    $capture = $Topology | Where-Object { $_.Primary } | Select-Object -First 1
    $ui = $Topology | Where-Object { -not $_.Primary } | Select-Object -First 1

    # Identity is established WITHOUT correlating a monitor to a screen: assert that
    # the expected panels are attached at all, and separately that the primary and
    # secondary screens run the expected modes. On this machine exactly one panel is
    # 1440p and one is 1080p, so the two assertions together pin the topology down
    # unambiguously — and neither of them depends on an enumeration order that is
    # not stable across a driver reset or a cable swap.
    $attachedModels = @((Get-EdidMonitorModel).Model | Where-Object { $_ })
    foreach ($want in @($Expected.capture_display.model_contains, $Expected.ui_display.model_contains)) {
        if (-not $want) { continue }
        if ($attachedModels.Count -eq 0) {
            $problems += "EDID model names unavailable; cannot confirm '$want' is attached."
        } elseif (-not ($attachedModels | Where-Object { $_ -like "*$want*" })) {
            $problems += ("Expected panel '*{0}*' is not attached (found: {1})." -f $want, ($attachedModels -join ', '))
        }
    }

    if (-not $capture) {
        $problems += 'No primary display was reported.'
    } else {
        $want = $Expected.capture_display
        if ($capture.Width -ne $want.width -or $capture.Height -ne $want.height) {
            $problems += ("Capture display is {0}x{1}, scenario requires {2}x{3}." -f `
                $capture.Width, $capture.Height, $want.width, $want.height)
        }
        if ($want.min_refresh_hz -and $capture.RefreshHz -lt $want.min_refresh_hz) {
            $problems += ("Capture display runs at {0} Hz, scenario requires at least {1} Hz." -f `
                $capture.RefreshHz, $want.min_refresh_hz)
        }
    }

    if (-not $ui) {
        $problems += 'No secondary display was reported; ExoSnap would have to live inside the captured image.'
    } else {
        $want = $Expected.ui_display
        if ($want.width -and ($ui.Width -ne $want.width -or $ui.Height -ne $want.height)) {
            $problems += ("UI display is {0}x{1}, scenario expects {2}x{3}." -f `
                $ui.Width, $ui.Height, $want.width, $want.height)
        }
    }

    return [pscustomobject]@{
        Ok             = ($problems.Count -eq 0)
        Problems       = $problems
        CaptureDisplay = $capture
        UiDisplay      = $ui
        AttachedPanels = $attachedModels
        All            = $Topology
    }
}

Export-ModuleMember -Function Get-BenchmarkDisplayTopology, Get-EdidMonitorModel, Test-BenchmarkTopology
