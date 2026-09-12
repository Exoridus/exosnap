using System.Diagnostics;
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

    private const string LauncherFileName = "sandbox-run.ps1";

    // Windows runs at most one sandbox per machine, so a machine left running after
    // its worker finished blocks every later gate with "only one instance of Windows
    // Sandbox is allowed". Nothing on the host can end a specific sandbox without
    // guessing at processes that may belong to a person's own session, so the guest
    // ends itself. $args rather than a param block: the worker's own named
    // parameters must not bind to this wrapper.
    private const string LauncherScript = """
        $worker = $args[0]
        $rest = if ($args.Count -gt 1) { @($args[1..($args.Count - 1)]) } else { @() }
        try {
            & $worker @rest
        }
        finally {
            shutdown.exe /s /f /t 0
        }
        """;

    private static readonly TimeSpan LaunchTimeout = TimeSpan.FromSeconds(120);
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan MachineFreeTimeout = TimeSpan.FromMinutes(3);

    // Every process Windows keeps alive for a running sandbox. Asked about rather
    // than killed: one of these may belong to a sandbox a person opened themselves.
    private static readonly string[] SandboxProcessNames =
        ["WindowsSandbox", "WindowsSandboxClient", "WindowsSandboxServer", "WindowsSandboxRemoteSession"];

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

        // Asked before the launcher is started, never after: WindowsSandbox.exe
        // reports a machine that is already running as a modal dialog rather than an
        // exit code, which would stand on someone's desktop until they clicked it.
        if (!await WaitForFreeMachineAsync(cancellationToken).ConfigureAwait(false))
        {
            return DisposableOsRun.Unavailable(
                "a Windows Sandbox is already running on this machine and only one may run at a time");
        }

        var staging = Path.Combine(this.stagingRoot, "sandbox-" + Guid.NewGuid().ToString("N"));
        // Collected in the finally and attached to whatever is returned, so the
        // caller learns about a failed collection on every path -- including the
        // paths that faulted, which are the ones whose evidence matters most.
        var evidence = EvidenceOutcome.NotRequested;
        DisposableOsRun outcome;
        try
        {
            var preparation = this.Prepare(staging, request);
            if (preparation.Failure is not null)
            {
                outcome = DisposableOsRun.Faulted(preparation.Failure);
                return outcome with { Evidence = evidence };
            }

            var launch = await this.processes.RunAsync(
                new ProcessRunRequest(this.sandbox.Path!, preparation.ConfigurationPath) { Timeout = LaunchTimeout },
                cancellationToken).ConfigureAwait(false);
            if (!launch.Succeeded)
            {
                outcome = DisposableOsRun.Faulted(
                    $"WindowsSandbox.exe exited {launch.ExitCode} without starting the worker: {launch.StandardError}");
                return outcome with { Evidence = evidence };
            }

            outcome = await AwaitResultAsync(preparation, request.Timeout, cancellationToken).ConfigureAwait(false);

            // The marker means the worker finished, not that the machine is gone.
            // Returning while it still shuts down would hand the next gate a
            // machine it cannot have.
            await WaitForFreeMachineAsync(cancellationToken).ConfigureAwait(false);
            return outcome with { Evidence = evidence };
        }
        finally
        {
            // Before the staging goes, and whatever the verdict was: a run that
            // faulted is the one whose evidence is worth the most.
            evidence = CollectEvidence(staging, request.EvidenceDirectory);
            if (evidence.IsComplete || evidence.State == EvidenceOutcomeState.Missing)
            {
                TryDelete(staging);
            }

            // Otherwise the staging directory stays. It is the only other copy of
            // what could not be collected, and deleting it is the one irreversible
            // step here: a directory left behind is recoverable, evidence is not.
            // The path is named in the Evidence detail the caller receives.
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

        File.WriteAllText(Path.Combine(staging, LauncherFileName), LauncherScript);
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

    /// <summary>True once no sandbox is running, false when the wait ran out.</summary>
    private static async Task<bool> WaitForFreeMachineAsync(CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.Add(MachineFreeTimeout);
        while (MachineIsBusy())
        {
            if (DateTime.UtcNow >= deadline)
            {
                return false;
            }

            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
        }

        return true;
    }

    private static bool MachineIsBusy()
    {
        foreach (var name in SandboxProcessNames)
        {
            var processes = Process.GetProcessesByName(name);
            try
            {
                if (processes.Length > 0)
                {
                    return true;
                }
            }
            finally
            {
                foreach (var process in processes)
                {
                    process.Dispose();
                }
            }
        }

        return false;
    }

    /// <summary>
    /// Copies what the worker wrote back to the host, and says how completely.
    /// </summary>
    /// <remarks>
    /// It swallowed IOException and UnauthorizedAccessException and returned, and
    /// the caller then deleted the staging directory -- so a failed copy destroyed
    /// the only source of the evidence and nothing anywhere said so. It also copied
    /// through a CopyDirectory that threw on the first unreadable file, abandoning
    /// every file after it.
    ///
    /// Now every file is attempted, the failures are counted and named, and the
    /// caller keeps the staging directory when anything was lost: a run whose
    /// evidence could not be collected is one whose evidence still exists
    /// somewhere, and deleting it is the one irreversible thing here.
    /// </remarks>
    internal static EvidenceOutcome CollectEvidence(string staging, string? destination)
    {
        if (destination is null)
        {
            return EvidenceOutcome.NotRequested;
        }

        var produced = Path.Combine(staging, EvidenceLeaf);
        if (!Directory.Exists(produced))
        {
            // The worker declares an evidence directory and did not write one. That
            // is not a copy failure, and it is not nothing either: a gate that
            // expected evidence has none.
            return new EvidenceOutcome(EvidenceOutcomeState.Missing, 0, 0,
                $"the worker wrote no {EvidenceLeaf} directory");
        }

        var failures = new List<string>();
        var copied = CopyDirectoryBestEffort(produced, destination, failures);

        if (failures.Count == 0)
        {
            return new EvidenceOutcome(EvidenceOutcomeState.Complete, copied, 0,
                $"{copied} evidence file(s) collected");
        }

        var state = copied > 0 ? EvidenceOutcomeState.Partial : EvidenceOutcomeState.Failed;
        // Capped: a directory whose whole content is unreadable would otherwise
        // produce a message nobody can read either.
        var named = string.Join(" | ", failures.Take(5));
        var more = failures.Count > 5 ? $" (+{failures.Count - 5} more)" : string.Empty;
        return new EvidenceOutcome(state, copied, failures.Count,
            $"{copied} evidence file(s) collected, {failures.Count} could not be: {named}{more}; "
            + $"the staging directory was kept so nothing is lost: {staging}");
    }

    /// <summary>
    /// Copies as much as it can and records what it could not, rather than stopping
    /// at the first failure. Returns the number of files copied.
    /// </summary>
    private static int CopyDirectoryBestEffort(string source, string destination, List<string> failures)
    {
        try
        {
            Directory.CreateDirectory(destination);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            failures.Add($"{destination}: {ex.Message}");
            return 0;
        }

        var copied = 0;
        IEnumerable<string> files;
        IEnumerable<string> directories;
        try
        {
            files = Directory.EnumerateFiles(source).ToList();
            directories = Directory.EnumerateDirectories(source).ToList();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            failures.Add($"{source}: {ex.Message}");
            return 0;
        }

        foreach (var file in files)
        {
            try
            {
                File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), overwrite: true);
                copied++;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                failures.Add($"{Path.GetFileName(file)}: {ex.Message}");
            }
        }

        foreach (var directory in directories)
        {
            copied += CopyDirectoryBestEffort(
                directory, Path.Combine(destination, Path.GetFileName(directory)), failures);
        }

        return copied;
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
        var quoted = string.Join(' ', arguments.Prepend(guestWorker).Select(Quote));
        var guestLauncher = Path.Combine(guestStaging, LauncherFileName);
        var command = SecurityElement.Escape(
            $"{Quote(guestShell)} -ExecutionPolicy Bypass -NoProfile -File {Quote(guestLauncher)} {quoted}");

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
