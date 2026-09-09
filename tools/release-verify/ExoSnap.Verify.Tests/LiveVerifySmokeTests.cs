using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The Live Verify session adapter against the real application.
/// </summary>
/// <remarks>
/// The only test in the suite that starts ExoSnap, and it starts it offscreen with a
/// throwaway configuration directory inside a job object. Never on the visible
/// desktop: the developer works on the same machine, and a harness that took focus
/// while a controller was in use would break the thing it was measuring.
///
/// It skips rather than fails when the Debug build or the Qt install is not there. A
/// missing build is a statement about the tree, not about the product, and a suite
/// that went red for it would be red on every fresh clone.
/// </remarks>
public sealed class LiveVerifySmokeTests
{
    private static readonly TimeSpan ShutdownDeadline = TimeSpan.FromSeconds(20);

    [Fact]
    public async Task RealApplicationCompletesTheHandshakeAndShutsDownCleanly()
    {
        var repositoryRoot = RepositoryRoot();
        if (repositoryRoot is null)
        {
            Assert.Skip("the repository root could not be located from the test output directory");
            return;
        }

        var application = Path.Combine(
            repositoryRoot, "build", "windows-x64-ninja-debug", "app", "exosnap.exe");
        if (!File.Exists(application))
        {
            Assert.Skip(
                "the Debug application is not built; configure the windows-x64-ninja-debug preset and " +
                "build the 'exosnap' target");
            return;
        }

        var qtRoot = QtRoot(repositoryRoot);
        if (qtRoot is null)
        {
            Assert.Skip($"Qt {QtVersion(repositoryRoot)} is not installed under the expected root");
            return;
        }

        // The same environment scripts/run-tests.ps1 establishes: the offscreen
        // platform so nothing paints, the plugin path so the platform plugin resolves,
        // and Qt's bin on PATH so its DLLs and FFmpeg do. EXOSNAP_CONFIG_DIR is set by
        // the launcher itself, which is part of what this test is checking.
        var environment = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase)
        {
            ["QT_QPA_PLATFORM"] = "offscreen",
            ["QT_PLUGIN_PATH"] = Path.Combine(qtRoot, "plugins"),
            ["PATH"] = Path.Combine(qtRoot, "bin") + Path.PathSeparator +
                       (Environment.GetEnvironmentVariable("PATH") ?? string.Empty),
        };

        var factory = new LiveVerifySessionFactory(environment, Path.GetTempPath());
        var session = await factory.LaunchAsync(application, TestContext.Current.CancellationToken);

        var processId = ((LiveVerifySession)session).ProcessId;
        string configDirectory;
        try
        {
            Assert.False(string.IsNullOrWhiteSpace(session.RunId));
            configDirectory = ((LiveVerifySession)session).ConfigDirectory;
            Assert.True(Directory.Exists(configDirectory), "the throwaway configuration directory was not created");

            var identity = await session.AppIdentityAsync(TestContext.Current.CancellationToken);
            Assert.True(identity.Ok, $"app.identity refused: {identity.Refusal}");

            var version = identity.Result.ValueKind == JsonValueKind.Object &&
                          identity.Result.TryGetProperty("productVersion", out var reported)
                ? reported.GetString()
                : null;
            Assert.False(string.IsNullOrWhiteSpace(version), "app.identity reported no product version");

            // The channel is closed with whatever events are buffered, and the
            // application is asked to close. Offscreen it owns no window a close
            // request can reach, so the honest answer here is NotRequestable rather
            // than a timeout: REL-SHUTDOWN-001 asserts the exit itself, and it declares
            // Desktop isolation for exactly this reason. What this smoke pins is that
            // the adapter says which of the two happened instead of reporting a
            // deadline nobody was asked to meet.
            var shutdown = await session.ShutdownAsync(ShutdownDeadline, TestContext.Current.CancellationToken);
            Assert.True(
                shutdown is SessionShutdown.Exited or SessionShutdown.NotRequestable,
                $"the application was asked to close and was still running after " +
                $"{ShutdownDeadline.TotalSeconds.ToString("0", CultureInfo.InvariantCulture)} s");
        }
        finally
        {
            await session.DisposeAsync();
        }

        Assert.False(
            Directory.Exists(configDirectory),
            "the throwaway configuration directory outlived the session it belonged to");

        // A leaked instance holds the machine-wide single-instance mutex, so the next
        // launch hands focus to it and exits without ever opening a control channel.
        // The gate after this one then waits for an endpoint nobody opened and reports
        // an infrastructure error that belongs to this teardown, not to it.
        Assert.False(
            IsRunning(processId),
            "the application outlived the session that owned it and will block the next launch");
    }

    private static bool IsRunning(int processId)
    {
        try
        {
            using var process = System.Diagnostics.Process.GetProcessById(processId);
            return !process.HasExited;
        }
        catch (ArgumentException)
        {
            // No process with that id, which is the answer this asks for.
            return false;
        }
    }

    private static string? RepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "AGENTS.md")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        return null;
    }

    private static string QtVersion(string repositoryRoot)
    {
        var pin = Path.Combine(repositoryRoot, ".qt-version");
        return File.Exists(pin) ? File.ReadAllText(pin).Trim() : string.Empty;
    }

    // The same resolution scripts/lib/QtEnvironment.psm1 performs, so a machine that
    // can run the C++ suite can run this one without a second thing to configure.
    private static string? QtRoot(string repositoryRoot)
    {
        var version = QtVersion(repositoryRoot);
        if (version.Length == 0)
        {
            return null;
        }

        var installRoot = Environment.GetEnvironmentVariable("EXOSNAP_QT_ROOT");
        if (string.IsNullOrWhiteSpace(installRoot))
        {
            var systemDrive = Environment.GetEnvironmentVariable("SystemDrive") ?? "C:";
            installRoot = systemDrive + Path.DirectorySeparatorChar + "Qt";
        }

        var root = Path.Combine(installRoot, version, "msvc2022_64");
        return Directory.Exists(root) ? root : null;
    }
}
