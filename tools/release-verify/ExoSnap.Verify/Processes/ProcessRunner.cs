using System.Diagnostics;
using System.Text;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Processes;

/// <summary>
/// Starts external tools and reports exactly what they produced.
/// </summary>
/// <remarks>
/// Three properties this type exists for.
///
/// No shell. <c>UseShellExecute</c> is false and arguments are passed as a list,
/// so nothing the caller supplies is ever re-interpreted as syntax.
///
/// Both streams are drained concurrently. A child that fills the standard error
/// pipe while the parent is still reading standard output blocks forever, and
/// that deadlock is indistinguishable from a hanging tool.
///
/// Every child joins a job object with kill-on-close. A gate that times out or is
/// cancelled must not leave an updater or a probe behind, because the next
/// scenario would then measure a machine the previous one still owns.
/// </remarks>
public sealed class ProcessRunner : IDisposable
{
    // How long a result waits on a pipe that a leaked grandchild still holds
    // open. Long enough that a slow flush is not truncated, short enough that a
    // leak does not stall the campaign behind it.
    private static readonly TimeSpan PipeDrainGrace = TimeSpan.FromSeconds(5);

    private readonly WindowsJobObject? job;
    private bool disposed;

    /// <summary>Creates a runner that owns a job object for its children.</summary>
    public ProcessRunner()
    {
        this.job = WindowsJobObject.TryCreate();
    }

    /// <summary>
    /// Whether child containment is active. False means the platform refused to
    /// create a job object; children still run, but a killed run may leave
    /// grandchildren behind.
    /// </summary>
    public bool ChildContainmentActive => this.job is not null;

    /// <summary>
    /// Runs a child to completion, its deadline, or cancellation.
    /// </summary>
    /// <exception cref="ProcessStartFailedException">The executable could not be started at all.</exception>
    public async Task<ProcessRunResult> RunAsync(ProcessRunRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        ObjectDisposedException.ThrowIf(this.disposed, this);

        var startInfo = new ProcessStartInfo
        {
            FileName = request.FileName,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            CreateNoWindow = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,
            StandardErrorEncoding = System.Text.Encoding.UTF8,
        };

        foreach (var argument in request.Arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        if (!string.IsNullOrEmpty(request.WorkingDirectory))
        {
            startInfo.WorkingDirectory = request.WorkingDirectory;
        }

        foreach (var (name, value) in request.Environment)
        {
            if (value is null)
            {
                startInfo.Environment.Remove(name);
            }
            else
            {
                startInfo.Environment[name] = value;
            }
        }

        using var process = new Process { StartInfo = startInfo };
        var stopwatch = Stopwatch.StartNew();

        try
        {
            if (!process.Start())
            {
                throw ProcessStartFailedException.ForExecutable(request.FileName, null);
            }
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or
                                              InvalidOperationException or
                                              PlatformNotSupportedException)
        {
            throw ProcessStartFailedException.ForExecutable(request.FileName, exception);
        }

        // Assignment can only happen after the child exists, so a grandchild
        // spawned in the first instants of its life can still escape. Nothing in
        // the supported surface for a managed Process closes that window.
        this.job?.TryAssign(process.SafeHandle);

        // Pumped into a buffer rather than read with ReadToEndAsync, because a
        // grandchild that inherited the child's pipe handle keeps the stream open
        // after the child is gone. ReadToEndAsync would then produce nothing at
        // all; this keeps whatever the child actually wrote.
        var output = new StringBuilder();
        var error = new StringBuilder();
        var stdout = PumpAsync(process.StandardOutput, output);
        var stderr = PumpAsync(process.StandardError, error);

        if (request.StandardInput is not null)
        {
            await process.StandardInput.WriteAsync(request.StandardInput).ConfigureAwait(false);
        }

        process.StandardInput.Close();

        var timedOut = false;
        using (var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
        {
            deadline.CancelAfter(request.Timeout);
            try
            {
                await process.WaitForExitAsync(deadline.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                timedOut = !cancellationToken.IsCancellationRequested;
                Kill(process);
                if (!timedOut)
                {
                    await DrainAsync(stdout, stderr).ConfigureAwait(false);
                    throw;
                }
            }
        }

        await DrainAsync(stdout, stderr).ConfigureAwait(false);
        stopwatch.Stop();

        return new ProcessRunResult(
            request.FileName,
            request.Arguments,
            timedOut ? -1 : process.ExitCode,
            Read(output),
            Read(error),
            timedOut,
            stopwatch.Elapsed);
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        if (this.disposed)
        {
            return;
        }

        this.disposed = true;
        this.job?.Dispose();
    }

    private static void Kill(Process process)
    {
        try
        {
            process.Kill(entireProcessTree: true);
        }
        catch (InvalidOperationException)
        {
            // Already gone between the deadline and the kill.
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // The process is exiting and the handle no longer accepts a kill.
        }
    }

    private static async Task PumpAsync(StreamReader reader, StringBuilder sink)
    {
        var buffer = new char[4096];
        while (true)
        {
            int read;
            try
            {
                read = await reader.ReadAsync(buffer, CancellationToken.None).ConfigureAwait(false);
            }
            catch (IOException)
            {
                // The pipe went away with the process that owned it.
                return;
            }

            if (read <= 0)
            {
                return;
            }

            lock (sink)
            {
                sink.Append(buffer, 0, read);
            }
        }
    }

    // A pump ends when its pipe closes, which happens only once every process
    // holding a handle to it is gone. A grandchild that inherited the handle would
    // otherwise hold the result forever, so the wait is bounded and whatever the
    // child wrote is reported rather than nothing.
    private static async Task DrainAsync(Task stdout, Task stderr)
    {
        var both = Task.WhenAll(stdout, stderr);
        await Task.WhenAny(both, Task.Delay(PipeDrainGrace)).ConfigureAwait(false);
    }

    private static string Read(StringBuilder sink)
    {
        lock (sink)
        {
            return sink.ToString();
        }
    }
}

/// <summary>
/// The executable could not be started at all.
/// </summary>
/// <remarks>
/// Separate from a non-zero exit code on purpose: a tool that did not start
/// measured nothing, so a scenario turns this into an infrastructure error rather
/// than a product verdict.
/// </remarks>
public sealed class ProcessStartFailedException : Exception
{
    /// <summary>Creates the exception with a default message.</summary>
    public ProcessStartFailedException()
        : base("The process could not be started.")
    {
        this.FileName = string.Empty;
    }

    /// <summary>Creates the exception with a message.</summary>
    public ProcessStartFailedException(string message)
        : base(message)
    {
        this.FileName = string.Empty;
    }

    /// <summary>Creates the exception with a message and a cause.</summary>
    public ProcessStartFailedException(string message, Exception? innerException)
        : base(message, innerException)
    {
        this.FileName = string.Empty;
    }

    private ProcessStartFailedException(string fileName, string message, Exception? innerException)
        : base(message, innerException)
    {
        this.FileName = fileName;
    }

    /// <summary>The executable that would not start.</summary>
    public string FileName { get; }

    /// <summary>Names the executable that would not start.</summary>
    public static ProcessStartFailedException ForExecutable(string fileName, Exception? innerException) =>
        new(fileName, $"The process '{fileName}' could not be started.", innerException);
}
