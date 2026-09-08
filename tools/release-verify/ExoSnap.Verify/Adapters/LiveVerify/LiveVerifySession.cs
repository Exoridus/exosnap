using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.LiveVerify;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Adapters.LiveVerify;

/// <summary>
/// A real ExoSnap process and the control channel it opened.
/// </summary>
/// <remarks>
/// The process is started with a run id that is both the endpoint name and the
/// connection credential, under a throwaway configuration directory, and inside a job
/// object with kill-on-close. All three matter: a campaign must not read or write the
/// developer's own settings, and a gate that is cancelled must not leave a recording
/// application behind for the next one to measure.
///
/// The command surface is the product's own semantic automation. Nothing here reads a
/// label, and nothing screenshots a page to decide whether a recording worked.
/// </remarks>
public sealed class LiveVerifySession : ILiveVerifySession
{
    /// <summary>The command that reports the observable product state.</summary>
    public const string StateCommand = "ui.getState";

    /// <summary>The event that carries a new state revision on the application endpoint.</summary>
    public const string StateChangedEvent = "ui.stateChanged";

    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(250);

    private readonly LiveVerifyClient client;
    private readonly Process process;
    private readonly WindowsJobObject? job;
    private readonly string configDirectory;
    private readonly bool ownsConfigDirectory;
    private bool disposed;

    internal LiveVerifySession(
        LiveVerifyClient client,
        Process process,
        WindowsJobObject? job,
        string configDirectory,
        bool ownsConfigDirectory)
    {
        this.client = client;
        this.process = process;
        this.job = job;
        this.configDirectory = configDirectory;
        this.ownsConfigDirectory = ownsConfigDirectory;
    }

    /// <inheritdoc/>
    public string RunId => this.client.RunId;

    /// <inheritdoc/>
    public long StateRevision => this.client.StateRevision;

    /// <inheritdoc/>
    public int BufferedEventCount => this.client.Events.Count;

    /// <summary>The process id of the running application.</summary>
    public int ProcessId => this.process.Id;

    /// <summary>The throwaway configuration directory this session runs against.</summary>
    public string ConfigDirectory => this.configDirectory;

    /// <inheritdoc/>
    public async Task<ControlAnswer> InvokeAsync(
        string command,
        IReadOnlyDictionary<string, object?>? parameters,
        CancellationToken cancellationToken)
    {
        var response = await this.client
            .RequestAsync(command, parameters, LiveVerifySessionCommands.CommandTimeout, cancellationToken)
            .ConfigureAwait(false);
        return new ControlAnswer(response.Ok, response.Result, response.ErrorCode, response.ErrorMessage);
    }

    /// <inheritdoc/>
    public async Task<string> WaitForRecordingStateAsync(
        IReadOnlyCollection<string> states,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(states);
        var deadline = DateTime.UtcNow + timeout;
        var observed = await this.ReadRecordingStateAsync(cancellationToken).ConfigureAwait(false);

        while (!states.Contains(observed, StringComparer.OrdinalIgnoreCase))
        {
            var remaining = deadline - DateTime.UtcNow;
            if (remaining <= TimeSpan.Zero)
            {
                break;
            }

            if (!await this.WaitForRevisionAsync(this.client.StateRevision, remaining, cancellationToken)
                    .ConfigureAwait(false))
            {
                break;
            }

            observed = await this.ReadRecordingStateAsync(cancellationToken).ConfigureAwait(false);
        }

        return observed;
    }

    /// <inheritdoc/>
    public async Task<bool> WaitForRevisionAsync(long after, TimeSpan timeout, CancellationToken cancellationToken)
    {
        if (this.client.StateRevision > after)
        {
            return true;
        }

        // A state query is what advances the client's own revision when no event is
        // in flight, so the poll is a query rather than a sleep: it can only be
        // satisfied by the server reporting a different observable state.
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
            await this.InvokeAsync(StateCommand, null, cancellationToken).ConfigureAwait(false);
            if (this.client.StateRevision > after)
            {
                return true;
            }
        }

        return false;
    }

    /// <inheritdoc/>
    public async Task<SessionShutdown> ShutdownAsync(TimeSpan timeout, CancellationToken cancellationToken)
    {
        // The channel goes first, deliberately. Closing it with events still buffered
        // is what a runner that stopped reading looks like from the server side, and
        // an application that cannot exit in that condition is the defect
        // REL-SHUTDOWN-001 pins.
        await this.client.DisposeAsync().ConfigureAwait(false);

        try
        {
            if (this.process.HasExited)
            {
                return SessionShutdown.Exited;
            }

            // False means the process owns no window a close request can reach, which
            // is the normal condition offscreen. Reported as its own outcome rather
            // than waited out: a deadline nobody was asked to meet is not a defect.
            if (!this.process.CloseMainWindow())
            {
                return SessionShutdown.NotRequestable;
            }

            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            deadline.CancelAfter(timeout);
            await this.process.WaitForExitAsync(deadline.Token).ConfigureAwait(false);
            return SessionShutdown.Exited;
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return SessionShutdown.StillRunning;
        }
        catch (InvalidOperationException)
        {
            // Gone between the check and the close.
            return SessionShutdown.Exited;
        }
    }

    /// <inheritdoc/>
    public async ValueTask DisposeAsync()
    {
        if (this.disposed)
        {
            return;
        }

        this.disposed = true;
        await this.client.DisposeAsync().ConfigureAwait(false);

        // The job object first, and the kill only as a fallback. Closing a
        // kill-on-close job terminates everything assigned to it in one operation the
        // process cannot refuse, whereas Kill can fail quietly on a process that is
        // mid-teardown - and a recording application left running is a machine the
        // next gate would measure without knowing it.
        this.job?.Dispose();

        try
        {
            if (!this.process.HasExited)
            {
                this.process.Kill(entireProcessTree: true);
            }

            this.process.WaitForExit(5000);
        }
        catch (InvalidOperationException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }

        this.process.Dispose();

        if (this.ownsConfigDirectory)
        {
            try
            {
                Directory.Delete(this.configDirectory, recursive: true);
            }
            catch (IOException)
            {
                // A throwaway directory the operating system still holds open is not
                // worth a verdict; it lives under the temporary root either way.
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }

    private async Task<string> ReadRecordingStateAsync(CancellationToken cancellationToken)
    {
        var answer = await this.InvokeAsync(StateCommand, null, cancellationToken).ConfigureAwait(false);
        if (!answer.Ok)
        {
            // There is no product state in which a state query is a legitimate
            // refusal, so this is a broken connection rather than a finding.
            throw new LiveVerifyException($"{StateCommand} refused: {answer.ErrorCode} - {answer.ErrorMessage}");
        }

        return answer.Result.ValueKind == JsonValueKind.Object &&
               answer.Result.TryGetProperty("recordingState", out var state) &&
               state.ValueKind == JsonValueKind.String
            ? state.GetString() ?? string.Empty
            : string.Empty;
    }
}

/// <summary>
/// Starts ExoSnap with a control channel and hands back a connected session.
/// </summary>
/// <remarks>
/// Never on the visible desktop by default: the launcher sets the offscreen Qt
/// platform unless a caller explicitly asks otherwise, so a campaign cannot take
/// focus from whoever is using the machine.
/// </remarks>
public sealed class LiveVerifySessionFactory : ILiveVerifySessionFactory
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan HandshakeTimeout = TimeSpan.FromSeconds(15);

    private readonly IReadOnlyDictionary<string, string?> environment;
    private readonly string? configRoot;

    /// <summary>Creates a factory with the default offscreen environment.</summary>
    public LiveVerifySessionFactory()
        : this(new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase), null)
    {
    }

    /// <summary>Creates a factory with extra environment for the launched process.</summary>
    /// <param name="environment">Variables added to or removed from the child's environment.</param>
    /// <param name="configRoot">
    /// Where the throwaway configuration directories are created, or null for the
    /// system temporary directory.
    /// </param>
    public LiveVerifySessionFactory(IReadOnlyDictionary<string, string?> environment, string? configRoot)
    {
        ArgumentNullException.ThrowIfNull(environment);
        this.environment = environment;
        this.configRoot = configRoot;
    }

    /// <inheritdoc/>
    public async Task<ILiveVerifySession> LaunchAsync(string executablePath, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executablePath);

        var runId = LiveVerifyClient.NewRunId();
        var configDirectory = Path.Combine(
            this.configRoot ?? Path.GetTempPath(),
            "exosnap-verify-" + Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture));
        Directory.CreateDirectory(configDirectory);

        var startInfo = new ProcessStartInfo
        {
            FileName = executablePath,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        startInfo.ArgumentList.Add("--live-verify-control");
        startInfo.ArgumentList.Add(runId);
        startInfo.Environment["EXOSNAP_CONFIG_DIR"] = configDirectory;
        foreach (var (name, value) in this.environment)
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

        var job = WindowsJobObject.TryCreate();
        Process process;
        try
        {
            process = Process.Start(startInfo) ??
                      throw new LiveVerifyException($"'{executablePath}' could not be started.");
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or
                                              InvalidOperationException or
                                              PlatformNotSupportedException)
        {
            job?.Dispose();
            throw new LiveVerifyException($"'{executablePath}' could not be started.", exception);
        }

        job?.TryAssign(process.SafeHandle);

        var client = new LiveVerifyClient(runId);
        try
        {
            await client.ConnectAsync(ConnectTimeout, HandshakeTimeout, cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            await client.DisposeAsync().ConfigureAwait(false);
            Kill(process);
            job?.Dispose();
            throw;
        }

        return new LiveVerifySession(client, process, job, configDirectory, ownsConfigDirectory: true);
    }

    private static void Kill(Process process)
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
        finally
        {
            process.Dispose();
        }
    }
}
