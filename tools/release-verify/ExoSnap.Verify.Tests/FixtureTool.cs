using System.Globalization;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Locates the fixture child process, and copies it to hostile paths on demand.
/// </summary>
internal static class FixtureTool
{
    private const string FolderName = "fixture-tool";
    private const string ExecutableName = "ExoSnap.Verify.Fixtures.exe";

    /// <summary>Absolute path of the fixture executable in the test output.</summary>
    public static string Path { get; } =
        System.IO.Path.Combine(AppContext.BaseDirectory, FolderName, ExecutableName);

    /// <summary>
    /// Copies the fixture into a new directory whose name contains the given
    /// characters, and returns the path of the executable there.
    /// </summary>
    /// <remarks>
    /// The whole directory is copied because a framework-dependent executable
    /// needs its assembly and runtime configuration beside it. Copying only the
    /// apphost would produce a launch failure that looked like the runner's fault.
    /// </remarks>
    public static string CopyTo(string directory)
    {
        var source = System.IO.Path.Combine(AppContext.BaseDirectory, FolderName);
        Directory.CreateDirectory(directory);
        foreach (var file in Directory.EnumerateFiles(source))
        {
            File.Copy(file, System.IO.Path.Combine(directory, System.IO.Path.GetFileName(file)), overwrite: true);
        }

        return System.IO.Path.Combine(directory, ExecutableName);
    }

    /// <summary>A fresh temporary directory that is removed when disposed.</summary>
    public static TemporaryDirectory NewTemporaryDirectory(string suffix) => new(suffix);
}

/// <summary>A directory under the system temporary path, removed on dispose.</summary>
internal sealed class TemporaryDirectory : IDisposable
{
    public TemporaryDirectory(string suffix)
    {
        this.Path = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(),
            "exosnap-verify-tests",
            Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture) + suffix);
        Directory.CreateDirectory(this.Path);
    }

    /// <summary>Absolute path of the directory.</summary>
    public string Path { get; }

    public void Dispose()
    {
        try
        {
            Directory.Delete(this.Path, recursive: true);
        }
        catch (IOException)
        {
            // A file still held by a process that is exiting is not a test
            // failure; the system temporary directory is cleaned up eventually.
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
