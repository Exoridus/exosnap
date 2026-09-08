using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using Windows.Win32;
using Windows.Win32.Graphics.Gdi;

namespace ExoSnap.Verify.Windows;

/// <summary>
/// The refresh rates the attached displays enumerate, read through GDI.
/// </summary>
/// <remarks>
/// GDI rather than DXGI on purpose: Windows enumerates the nominal and the actual
/// rate of one physical mode separately (59 and 60, 119 and 120) and collapses
/// them on apply. A scenario that wants a rate a read-back can confirm has to see
/// that duplication rather than a list somebody already deduplicated.
/// </remarks>
public static class DisplayModeProbe
{
    /// <summary>
    /// The distinct refresh rates in Hz offered by any display attached to the
    /// desktop, or null when no display could be enumerated.
    /// </summary>
    public static ReadOnlyCollection<uint>? TryEnumerateRefreshRates()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        var rates = new SortedSet<uint>();
        var device = new DISPLAY_DEVICEW { cb = (uint)Marshal.SizeOf<DISPLAY_DEVICEW>() };

        for (uint deviceIndex = 0; PInvoke.EnumDisplayDevices(null, deviceIndex, ref device, 0); deviceIndex++)
        {
            if ((device.StateFlags & DISPLAY_DEVICE_STATE_FLAGS.DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
            {
                continue;
            }

            var name = device.DeviceName.ToString();
            var mode = new DEVMODEW { dmSize = (ushort)Marshal.SizeOf<DEVMODEW>() };
            for (uint modeIndex = 0;
                 PInvoke.EnumDisplaySettingsEx(name, (ENUM_DISPLAY_SETTINGS_MODE)modeIndex, ref mode, 0);
                 modeIndex++)
            {
                // 0 and 1 are the documented encodings for "the hardware default
                // rate", not a rate a scenario could ever ask for by number.
                if (mode.dmDisplayFrequency > 1)
                {
                    rates.Add(mode.dmDisplayFrequency);
                }
            }
        }

        return rates.Count == 0 ? null : new ReadOnlyCollection<uint>([.. rates]);
    }
}
