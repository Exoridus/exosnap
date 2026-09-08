using System.Diagnostics;
using Microsoft.Win32.SafeHandles;
using Windows.Win32;
using Windows.Win32.Security;

namespace ExoSnap.Verify.Windows;

/// <summary>
/// Reads the privilege facts about the current process that scenario selection
/// depends on.
/// </summary>
public static class ProcessPrivilege
{
    /// <summary>
    /// Whether the current process runs with an elevated token, or null when the
    /// token could not be read.
    /// </summary>
    public static bool? IsElevated()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        using var process = Process.GetCurrentProcess();
        SafeFileHandle? token = null;
        try
        {
            if (!PInvoke.OpenProcessToken(process.SafeHandle, TOKEN_ACCESS_MASK.TOKEN_QUERY, out token))
            {
                return null;
            }

            Span<byte> buffer = stackalloc byte[sizeof(uint)];
            if (!PInvoke.GetTokenInformation(
                    token,
                    TOKEN_INFORMATION_CLASS.TokenElevation,
                    buffer,
                    out uint written) || written < sizeof(uint))
            {
                return null;
            }

            return BitConverter.ToUInt32(buffer) != 0;
        }
        finally
        {
            token?.Dispose();
        }
    }

    /// <summary>
    /// The Windows session id of the current process, or null when it could not
    /// be read. Session 0 is the non-interactive service session.
    /// </summary>
    public static uint? SessionId()
    {
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        using var process = Process.GetCurrentProcess();
        return PInvoke.ProcessIdToSessionId((uint)process.Id, out uint session) ? session : null;
    }
}
