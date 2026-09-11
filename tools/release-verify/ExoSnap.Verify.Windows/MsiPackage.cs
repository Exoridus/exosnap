using System.Runtime.InteropServices;

namespace ExoSnap.Verify.Windows;

/// <summary>
/// Reads what an installer package says about itself, without installing it.
/// </summary>
/// <remarks>
/// An MSI is a small database, and its Property table is the package's own claim
/// about what it is. Reading it answers "is this the product under test" before
/// anything hands the file to msiexec, which matters for a gate that uninstalls
/// whatever it finds before installing this file. The installed-product registry
/// under Uninstall carries the same values, but only after an install, which is
/// too late to decide whether to run the installer at all.
///
/// Declared here rather than through the project's generated Win32 bindings
/// because those marshal an MSIHANDLE as a SafeHandle; the handles are plain
/// 32-bit values whose only operation is MsiCloseHandle, and the generated shape
/// obscures more than it protects.
/// </remarks>
public static partial class MsiPackage
{
    // msiOpenDatabaseModeReadOnly: the open mode travels in the pointer argument
    // itself, and zero means read-only. Nothing is written back to the package.
    private const string OpenReadOnly = null!;

    private const uint ErrorMoreData = 234;

    /// <summary>
    /// Reads the package's Property table, or null when the file is not a readable
    /// installer package.
    /// </summary>
    public static IReadOnlyDictionary<string, string>? ReadProperties(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (!NativeGate.IsWindows)
        {
            return null;
        }

        uint database = 0;
        uint view = 0;
        try
        {
            if (MsiOpenDatabaseW(path, OpenReadOnly, out database) != 0)
            {
                return null;
            }

            if (MsiDatabaseOpenViewW(database, "SELECT `Property`, `Value` FROM `Property`", out view) != 0 ||
                MsiViewExecute(view, 0) != 0)
            {
                return null;
            }

            var properties = new Dictionary<string, string>(StringComparer.Ordinal);
            while (MsiViewFetch(view, out var record) == 0)
            {
                try
                {
                    var name = ReadField(record, 1);
                    if (name.Length > 0)
                    {
                        properties[name] = ReadField(record, 2);
                    }
                }
                finally
                {
                    MsiCloseHandle(record);
                }
            }

            return properties;
        }
        finally
        {
            if (view != 0)
            {
                MsiCloseHandle(view);
            }

            if (database != 0)
            {
                MsiCloseHandle(database);
            }
        }
    }

    [LibraryImport("msi.dll", EntryPoint = "MsiOpenDatabaseW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint MsiOpenDatabaseW(string databasePath, string? persist, out uint database);

    [LibraryImport("msi.dll", EntryPoint = "MsiDatabaseOpenViewW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint MsiDatabaseOpenViewW(uint database, string query, out uint view);

    [LibraryImport("msi.dll")]
    private static partial uint MsiViewExecute(uint view, uint record);

    [LibraryImport("msi.dll")]
    private static partial uint MsiViewFetch(uint view, out uint record);

    [LibraryImport("msi.dll", EntryPoint = "MsiRecordGetStringW", StringMarshalling = StringMarshalling.Utf16)]
    private static partial uint MsiRecordGetStringW(uint record, uint field, Span<char> value, ref uint length);

    // Declared as void: closing a handle that is already gone is the only failure
    // it reports, and there is nothing a caller could do about it.
    [LibraryImport("msi.dll")]
    private static partial void MsiCloseHandle(uint handle);

    private static string ReadField(uint record, uint field)
    {
        // Two calls by design: the first reports the length the value needs, and a
        // buffer one character short truncates silently rather than failing.
        uint length = 0;
        var status = MsiRecordGetStringW(record, field, [], ref length);
        if (status != 0 && status != ErrorMoreData)
        {
            return string.Empty;
        }

        length += 1;
        var value = new char[length];
        return MsiRecordGetStringW(record, field, value, ref length) == 0
            ? new string(value, 0, (int)length)
            : string.Empty;
    }
}
