using System.Diagnostics;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Adapters.Elevation;

/// <summary>
/// Runs <c>ExoSnap.Verify.Worker.exe</c> and waits for the result file it writes.
/// </summary>
public sealed class ElevatedWorkerHost : IElevatedWorkerHost
{
    /// <summary>The environment variable that pins the worker executable.</summary>
    public const string PathVariable = "EXOSNAP_VERIFY_WORKER";

    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(250);

    private readonly string? workerPath;

    /// <summary>Resolves the worker executable beside the harness, or from the pin.</summary>
    public ElevatedWorkerHost()
        : this(ResolveWorkerPath())
    {
    }

    /// <summary>Creates a host bound to a specific worker executable path.</summary>
    public ElevatedWorkerHost(string? workerPath) => this.workerPath = workerPath;

    /// <inheritdoc/>
    public bool Available => this.workerPath is not null && File.Exists(this.workerPath);

    /// <inheritdoc/>
    public string UnavailableReason => this.Available
        ? string.Empty
        : $"ExoSnap.Verify.Worker.exe was not found (checked {PathVariable} and the harness directory); " +
          "the elevated present scenario cannot run without it";

    /// <inheritdoc/>
    public async Task<ElevatedWorkerRun> RunAsync(
        string taskId,
        string resultPath,
        string targetExe,
        TimeSpan timeout,
        bool selfTest,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(taskId);
        ArgumentException.ThrowIfNullOrWhiteSpace(resultPath);
        ArgumentNullException.ThrowIfNull(targetExe);

        if (!this.Available)
        {
            return ElevatedWorkerRun.Unavailable(this.UnavailableReason);
        }

        var full = Path.GetFullPath(resultPath);
        // A result from a previous attempt would be read as this one's.
        DeleteIfPresent(full);

        // The worker is started at the caller's own integrity level, with no window
        // and nothing redirected. It elevates itself when it has real work to do -
        // that relaunch is where the one UAC prompt appears - and a declined prompt
        // comes back as a Deferred result in the file, not as a launch failure here.
        var startInfo = new ProcessStartInfo
        {
            FileName = this.workerPath!,
            WorkingDirectory = Path.GetDirectoryName(this.workerPath!) ?? Environment.CurrentDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        foreach (var argument in ElevatedWorker.BuildArguments(taskId, full, targetExe, selfTest))
        {
            startInfo.ArgumentList.Add(argument);
        }

        Process process;
        try
        {
            process = Process.Start(startInfo) ??
                      throw new InvalidOperationException("Process.Start returned no process for the worker.");
        }
        catch (Exception exception)
            when (exception is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            return ElevatedWorkerRun.Faulted($"the worker could not be started: {exception.Message}");
        }

        try
        {
            return await AwaitResultAsync(full, () => SafeHasExited(process), timeout, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            KillIfRunning(process);
            process.Dispose();
        }
    }

    /// <summary>
    /// Waits for a worker result at <paramref name="resultPath"/>, giving up when
    /// the worker has exited without writing one or the deadline passes.
    /// </summary>
    internal static async Task<ElevatedWorkerRun> AwaitResultAsync(
        string resultPath,
        Func<bool> workerExited,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.Add(timeout < TimeSpan.Zero ? TimeSpan.Zero : timeout);

        while (true)
        {
            var result = ElevatedWorkerResult.Read(resultPath);
            if (result is not null)
            {
                return ElevatedWorkerRun.Completed(result);
            }

            if (workerExited())
            {
                // One more read: the worker may have written the file and exited
                // between the read above and this check.
                result = ElevatedWorkerResult.Read(resultPath);
                return result is not null
                    ? ElevatedWorkerRun.Completed(result)
                    : ElevatedWorkerRun.Faulted("the worker exited without writing a result document");
            }

            if (DateTime.UtcNow >= deadline)
            {
                return ElevatedWorkerRun.Faulted(
                    $"the worker did not write a result within {timeout.TotalSeconds:0}s");
            }

            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// Where <c>ExoSnap.Verify.Worker.exe</c> is: the pin, then beside the harness,
    /// then the build tree under a repository root when one is given.
    /// </summary>
    /// <remarks>
    /// A self-contained harness publish carries the worker beside it, so the middle
    /// step is what a shipped run uses. The build-tree step is for a run from a
    /// developer checkout, where nothing has been published yet.
    /// </remarks>
    public static string? Resolve(string? repositoryRoot = null)
    {
        var pinned = Environment.GetEnvironmentVariable(PathVariable);
        if (!string.IsNullOrWhiteSpace(pinned) && File.Exists(pinned))
        {
            return Path.GetFullPath(pinned);
        }

        foreach (var directory in new[]
                 {
                     AppContext.BaseDirectory,
                     Path.GetDirectoryName(typeof(ElevatedWorkerHost).Assembly.Location),
                 })
        {
            if (string.IsNullOrEmpty(directory))
            {
                continue;
            }

            var candidate = Path.Combine(directory, "ExoSnap.Verify.Worker.exe");
            if (Runnable(candidate))
            {
                return candidate;
            }
        }

        if (!string.IsNullOrWhiteSpace(repositoryRoot))
        {
            foreach (var configuration in new[] { "Release", "Debug" })
            {
                var candidate = Path.Combine(
                    repositoryRoot,
                    "tools", "release-verify", "ExoSnap.Verify.Worker", "bin",
                    configuration, "net10.0-windows", "ExoSnap.Verify.Worker.exe");
                if (Runnable(candidate))
                {
                    return Path.GetFullPath(candidate);
                }
            }
        }

        return null;
    }

    // A framework-dependent apphost without its managed assembly beside it exits
    // before Main runs. That is exactly the shape a build reference leaves in a
    // test output directory, and it must not shadow a complete build elsewhere.
    private static bool Runnable(string apphostPath) =>
        File.Exists(apphostPath) && File.Exists(Path.ChangeExtension(apphostPath, ".dll"));

    private static string? ResolveWorkerPath() => Resolve();

    private static void DeleteIfPresent(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static bool SafeHasExited(Process process)
    {
        try
        {
            return process.HasExited;
        }
        catch (InvalidOperationException)
        {
            return true;
        }
    }

    private static void KillIfRunning(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (InvalidOperationException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }
    }
}
