namespace ExoSnap.Verify.Windows;

/// <summary>
/// Marks the boundary between the harness engine and the operating system.
/// </summary>
/// <remarks>
/// Every type in this assembly reads or manipulates real Windows state. Nothing
/// here decides a verdict; a probe that cannot answer returns null or throws, and
/// the caller turns that into <c>unknown</c> or an infrastructure error. That
/// split is what keeps "the machine could not be asked" from ever being reported
/// as "the product is wrong".
/// </remarks>
public static class NativeGate
{
    /// <summary>
    /// True when the current process runs on Windows, which every probe in this
    /// assembly requires.
    /// </summary>
    public static bool IsWindows => OperatingSystem.IsWindows();
}
