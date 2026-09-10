using System.Diagnostics;
using Windows.Win32;
using Windows.Win32.Foundation;
using Windows.Win32.UI.WindowsAndMessaging;

namespace ExoSnap.Verify.Windows.Uia;

/// <summary>
/// A throwaway top-level window used only to give the UI Automation smoke a real
/// element tree to read.
/// </summary>
/// <remarks>
/// It is a predefined <c>STATIC</c> control shown far off-screen with
/// <c>WS_EX_NOACTIVATE</c> and <c>WS_EX_TOOLWINDOW</c>: never visible, never on the
/// taskbar, and it never touches focus or the cursor, so it does not fall under the
/// rule against driving the running application. UI Automation still enumerates it,
/// which is the whole point.
/// </remarks>
public static class ProbeWindow
{
    /// <summary>The window text, also the accessible name UI Automation reports.</summary>
    public const string Title = "ExoSnap.Verify UIA probe window";

    /// <summary>
    /// Shows the probe window for <paramref name="lifetime"/>, pumping its messages,
    /// then destroys it. Returns the process id the window belonged to.
    /// </summary>
    public static int ShowOffscreen(TimeSpan lifetime)
    {
        if (!NativeGate.IsWindows)
        {
            throw new PlatformNotSupportedException("The probe window is a Win32 facility.");
        }

        var window = CreateOffscreenStatic();
        if (window.IsNull)
        {
            throw new InvalidOperationException("CreateWindowEx returned no window for the STATIC probe.");
        }

        try
        {
            PInvoke.ShowWindow(window, SHOW_WINDOW_CMD.SW_SHOWNOACTIVATE);
            Pump(lifetime);
            return Environment.ProcessId;
        }
        finally
        {
            PInvoke.DestroyWindow(window);
        }
    }

    private static unsafe HWND CreateOffscreenStatic() =>
        PInvoke.CreateWindowEx(
            WINDOW_EX_STYLE.WS_EX_NOACTIVATE | WINDOW_EX_STYLE.WS_EX_TOOLWINDOW,
            "STATIC",
            Title,
            WINDOW_STYLE.WS_POPUP,
            -32000,
            -32000,
            120,
            40,
            HWND.Null,
            null,
            null,
            null);

    private static void Pump(TimeSpan lifetime)
    {
        var stopwatch = Stopwatch.StartNew();
        while (stopwatch.Elapsed < lifetime)
        {
            while (PInvoke.PeekMessage(out var message, HWND.Null, 0, 0, PEEK_MESSAGE_REMOVE_TYPE.PM_REMOVE))
            {
                PInvoke.DispatchMessage(in message);
            }

            Thread.Sleep(25);
        }
    }
}
