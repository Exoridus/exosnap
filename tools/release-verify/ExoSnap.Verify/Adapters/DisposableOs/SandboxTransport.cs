using System.Security;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>What one staged sandbox run needs to be launched and read back.</summary>
/// <param name="Failure">Why the run could not be staged, or null when it was.</param>
/// <param name="ConfigurationPath">The generated <c>.wsb</c> to hand to WindowsSandbox.exe.</param>
/// <param name="ResultPath">Where the worker's result document will appear on the host.</param>
/// <param name="MarkerPath">The file whose appearance means the worker finished.</param>
internal sealed record SandboxPreparation(
    string? Failure,
    string ConfigurationPath,
    string ResultPath,
    string MarkerPath);

/// <summary>
/// Runs a worker inside a fresh Windows Sandbox and waits for its result document.
/// </summary>
/// <remarks>
/// <c>WindowsSandbox.exe</c> returns as soon as the virtual machine is asked for,
/// not when the worker inside it finishes, so completion is read from the
/// worker's own marker file: a sandbox that crashed or was closed by hand leaves
/// no marker, and that is reported as Faulted rather than read as a verdict.
///
/// The staging directory is the only channel in both directions. Mapped
/// read-write, it carries the worker and its payload in, and the result document
/// and marker back. Nothing else is mapped from the repository, so a worker
/// cannot reach, and so cannot change, the thing it is verifying. The sandbox
/// logon token is already administrative, so no UAC prompt is ever raised inside
/// it; this transport itself needs no host elevation.
///
/// PowerShell 7 is mapped in read-only beside the staging directory because
/// Windows Sandbox ships Windows PowerShell 5.1 only and the worker scripts
/// declare <c>#Requires -Version 7.0</c>. A machine without a resolvable pwsh is
/// reported unavailable up front rather than failing inside a virtual machine
/// with no console attached to it.
/// </remarks>
public sealed class SandboxTransport : IDisposableOsTransport
{
    /// <summary>The environment variable pinning WindowsSandbox.exe, matching the PowerShell layer's.</summary>
    public const string PathVariable = "EXOSNAP_SANDBOX_EXE";

    /// <summary>The environment variable pinning the PowerShell 7 the guest runs the worker with.</summary>
    public const string PowerShellPathVariable = "EXOSNAP_PWSH";

    // Where a mapped folder lands inside the sandbox: the sandbox user's desktop,
    // under the host folder's own leaf name. Fixed by Windows, not configurable,
    // which is why guest paths can be computed here rather than discovered from
    // inside the virtual machine.
    private const string GuestDesktop = @"C:\Users\WDAGUtilityAccount\Desktop";

    private const string EvidenceLeaf = "evidence";

    private static readonly TimeSpan LaunchTimeout = TimeSpan.FromSeconds(120);
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);

    private readonly ProcessRunner processes;
    private readonly ResolvedTool sandbox;
    private readonly string stagingRoot;
    private readonly string? powerShellHome;

    /// <summary>Creates a transport resolving WindowsSandbox.exe and PowerShell 7 on this machine.</summary>
    public SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot)
        : this(processes, ResolveSandbox(tools), stagingRoot, ResolvePowerShellHome(tools))
    {
    }

    /// <summary>Creates a transport with an injected PowerShell 7 home, for tests.</summary>
    public SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot, string powerShellHome)
        : this(processes, ResolveSandbox(tools), stagingRoot, RequireHome(powerShellHome))
    {
    }

    private SandboxTransport(ProcessRunner processes, ResolvedTool sandbox, string stagingRoot, string? powerShellHome)
    {
        ArgumentNullException.ThrowIfNull(processes);
        ArgumentException.ThrowIfNullOrWhiteSpace(stagingRoot);
        this.processes = processes;
        this.sandbox = sandbox;
        this.stagingRoot = stagingRoot;
        this.powerShellHome = powerShellHome;
    }

    /// <inheritdoc/>
    public string Name => "sandbox";

    /// <inheritdoc/>
    public bool Available => this.sandbox.Available && this.powerShellHome is not null && Directory.Exists(this.powerShellHome);

    /// <inheritdoc/>
    public string UnavailableReason
    {
        get
        {
            if (this.Available)
            {
                return string.Empty;
            }

            if (!this.sandbox.Available)
            {
                return $"WindowsSandbox.exe was not found (checked {PathVariable} and PATH)";
            }

            return this.powerShellHome is null
                ? $"PowerShell 7 was not found (checked {PowerShellPathVariable} and PATH), so the sandbox has no shell to run the worker with"
                : $"the PowerShell 7 home '{this.powerShellHome}' does not exist, so the sandbox has no shell to run the worker with";
        }
    }

    /// <inheritdoc/>
    public async Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!this.Available)
        {
            return DisposableOsRun.Unavailable(this.UnavailableReason);
        }

        var staging = Path.Combine(this.stagingRoot, "sandbox-" + Guid.NewGuid().ToString("N"));
        try
        {
            var preparation = this.Prepare(staging, request);
            if (preparation.Failure is not null)
            {
                return DisposableOsRun.Faulted(preparation.Failure);
            }

            var launch = await this.processes.RunAsync(
                new ProcessRunRequest(this.sandbox.Path!, preparation.ConfigurationPath) { Timeout = LaunchTimeout },
                cancellationToken).ConfigureAwait(false);
            if (!launch.Succeeded)
            {
                return DisposableOsRun.Faulted(
                    $"WindowsSandbox.exe exited {launch.ExitCode} without starting the worker: {launch.StandardError}");
            }

            return await AwaitResultAsync(preparation, request.Timeout, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            // Before the staging goes, and whatever the verdict was: a run that
            // faulted is the one whose evidence is worth the most.
            CollectEvidence(staging, request.EvidenceDirectory);
            TryDelete(staging);
        }
    }

    /// <summary>
    /// Copies the request's payload into a fresh staging directory and writes the
    /// <c>.wsb</c> that runs the worker over it. Split out from the run so the part
    /// that has no virtual machine in it can be checked without one.
    /// </summary>
    internal SandboxPreparation Prepare(string staging, DisposableOsWorkerRequest request)
    {
        var resultPath = Path.Combine(staging, "result.json");
        var markerPath = Path.Combine(staging, "done.marker");
        var configurationPath = Path.Combine(staging, "release-verify.wsb");

        Directory.CreateDirectory(staging);
        var staged = new List<string>();
        foreach (var source in request.SourceFiles)
        {
            var leaf = Path.GetFileName(Path.TrimEndingDirectorySeparator(source));
            var destination = Path.Combine(staging, leaf);
            if (Directory.Exists(source))
            {
                CopyDirectory(source, destination);
            }
            else if (File.Exists(source))
            {
                File.Copy(source, destination, overwrite: true);
            }
            else
            {
                return new SandboxPreparation(
                    $"the sandbox run needs '{source}', which does not exist", configurationPath, resultPath, markerPath);
            }

            staged.Add(leaf);
        }

        if (request.EvidenceDirectory is not null)
        {
            Directory.CreateDirectory(Path.Combine(staging, EvidenceLeaf));
        }

        if (!File.Exists(Path.Combine(staging, request.WorkerFileName)))
        {
            return new SandboxPreparation(
                $"the sandbox worker {request.WorkerFileName} is not among the staged source files",
                configurationPath,
                resultPath,
                markerPath);
        }

        File.WriteAllText(
            configurationPath,
            this.BuildConfiguration(staging, request, staged));
        return new SandboxPreparation(null, configurationPath, resultPath, markerPath);
    }

    private static ResolvedTool ResolveSandbox(ToolResolver tools)
    {
        ArgumentNullException.ThrowIfNull(tools);
        return tools.Resolve("WindowsSandbox", PathVariable);
    }

    private static string? ResolvePowerShellHome(ToolResolver tools)
    {
        ArgumentNullException.ThrowIfNull(tools);
        var shell = tools.Resolve("pwsh", PowerShellPathVariable);
        return shell.Available ? Path.GetDirectoryName(shell.Path) : null;
    }

    private static string RequireHome(string powerShellHome)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(powerShellHome);
        return powerShellHome;
    }

    private static async Task<DisposableOsRun> AwaitResultAsync(
        SandboxPreparation preparation, TimeSpan timeout, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.Add(timeout < TimeSpan.Zero ? TimeSpan.Zero : timeout);
        while (!File.Exists(preparation.MarkerPath))
        {
            if (DateTime.UtcNow >= deadline)
            {
                return DisposableOsRun.Faulted(
                    $"the sandbox worker did not finish within {timeout.TotalMinutes:0} minute(s); no marker was written");
            }

            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
        }

        if (!File.Exists(preparation.ResultPath))
        {
            return DisposableOsRun.Faulted("the sandbox worker signalled completion but wrote no result document");
        }

        var json = await File.ReadAllTextAsync(preparation.ResultPath, cancellationToken).ConfigureAwait(false);
        var result = DisposableOsRunResult.Parse(json);
        return result is null
            ? DisposableOsRun.Faulted("the sandbox result document is not valid JSON")
            : DisposableOsRun.Completed(result);
    }

    private static void CollectEvidence(string staging, string? destination)
    {
        if (destination is null)
        {
            return;
        }

        var produced = Path.Combine(staging, EvidenceLeaf);
        if (!Directory.Exists(produced))
        {
            return;
        }

        try
        {
            CopyDirectory(produced, destination);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.EnumerateFiles(source))
        {
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), overwrite: true);
        }

        foreach (var directory in Directory.EnumerateDirectories(source))
        {
            CopyDirectory(directory, Path.Combine(destination, Path.GetFileName(directory)));
        }
    }

    private static string Quote(string argument) =>
        "\"" + argument.Replace("\"", "\\\"", StringComparison.Ordinal) + "\"";

    private static string GuestPath(string hostDirectory) =>
        Path.Combine(GuestDesktop, Path.GetFileName(Path.TrimEndingDirectorySeparator(hostDirectory)));

    private static void TryDelete(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private string BuildConfiguration(string staging, DisposableOsWorkerRequest request, IReadOnlyList<string> staged)
    {
        var guestStaging = GuestPath(staging);
        var guestWorker = Path.Combine(guestStaging, request.WorkerFileName);
        var guestShell = Path.Combine(GuestPath(this.powerShellHome!), "pwsh.exe");

        // The worker scripts dereference their path parameters directly, never
        // joined against the staging directory themselves, so an argument naming a
        // staged entry has to arrive as the path that entry has inside the guest.
        var arguments = request.WorkerArguments
            .Select(argument => staged.Contains(argument, StringComparer.OrdinalIgnoreCase)
                ? Path.Combine(guestStaging, argument)
                : argument)
            .Append("-StagingDirectory").Append(guestStaging)
            .Append("-ResultPath").Append(Path.Combine(guestStaging, "result.json"))
            .Append("-MarkerPath").Append(Path.Combine(guestStaging, "done.marker"));

        if (request.EvidenceDirectory is not null)
        {
            arguments = arguments
                .Append("-EvidenceDirectory").Append(Path.Combine(guestStaging, EvidenceLeaf));
        }

        // Quoted argument by argument rather than joined once: a staged path carries
        // the campaign id and, on a machine whose user name has a space, a space.
        // Double quotes, not the shell-style single quotes: the logon command is a
        // Windows command line, and the guest's argument parser passes a single
        // quote through as part of the value rather than as quoting.
        var quoted = string.Join(' ', arguments.Select(Quote));
        var command = SecurityElement.Escape(
            $"{Quote(guestShell)} -ExecutionPolicy Bypass -NoProfile -File {Quote(guestWorker)} {quoted}");

        return $"""
            <Configuration>
              <VGpu>Disable</VGpu>
              <Networking>Default</Networking>
              <MappedFolders>
                <MappedFolder>
                  <HostFolder>{staging}</HostFolder>
                  <ReadOnly>false</ReadOnly>
                </MappedFolder>
                <MappedFolder>
                  <HostFolder>{this.powerShellHome}</HostFolder>
                  <ReadOnly>true</ReadOnly>
                </MappedFolder>
              </MappedFolders>
              <LogonCommand>
                <Command>{command}</Command>
              </LogonCommand>
            </Configuration>
            """;
    }
}
