using Windows.Win32;
using Windows.Win32.System.StationsAndDesktops;

namespace ExoSnap.Verify.Windows;

/// <summary>
/// Whether the current process can reach the desktop a person is looking at.
/// </summary>
public static class InteractiveDesktop
{
    /// <summary>
    /// True when the input desktop can be opened. It cannot be while the
    /// workstation is locked or while the Secure Desktop is up, which is exactly
    /// when a UI scenario would otherwise fail for a reason that has nothing to
    /// do with the product.
    /// </summary>
    public static bool? IsReachable()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        using var desktop = PInvoke.OpenInputDesktop_SafeHandle(
            0,
            false,
            DESKTOP_ACCESS_FLAGS.DESKTOP_READOBJECTS);
        return !desktop.IsInvalid;
    }
}
