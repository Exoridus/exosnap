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
    private static readonly TimeSpan DefaultPollInterval = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan DefaultMachineFreeTimeout = TimeSpan.FromMinutes(3);


    // Every process Windows keeps alive for a running sandbox. Asked about rather
    // than killed: one of these may belong to a sandbox a person opened themselves.
    private static readonly string[] SandboxProcessNames =
        ["WindowsSandbox", "WindowsSandboxClient", "WindowsSandboxServer", "WindowsSandboxRemoteSession"];

    private readonly ResolvedTool sandbox;
    private readonly string stagingRoot;
    private readonly string? powerShellHome;
    private readonly Func<bool> machineIsBusy;
    private readonly Func<string, CancellationToken, Task<ProcessRunResult>> launch;
    private readonly TimeSpan pollInterval;
    private readonly TimeSpan machineFreeTimeout;

    /// <summary>Creates a transport resolving WindowsSandbox.exe and PowerShell 7 on this machine.</summary>
    public SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot)
        : this(processes, ResolveSandbox(tools), stagingRoot, ResolvePowerShellHome(tools), null, null, null, null)
    {
    }

    /// <summary>Creates a transport with an injected PowerShell 7 home, for tests.</summary>
    public SandboxTransport(ProcessRunner processes, ToolResolver tools, string stagingRoot, string powerShellHome)
        : this(processes, ResolveSandbox(tools), stagingRoot, RequireHome(powerShellHome), null, null, null, null)
    {
    }

    /// <summary>
    /// Creates a transport whose machine is stood in for: <paramref name="machineIsBusy"/>
    /// replaces asking Windows which sandbox processes exist, and <paramref name="launch"/>
    /// replaces starting one over a generated configuration.
    /// </summary>
    /// <remarks>
    /// The lifecycle rules here decide when the staging directory -- the only copy of
    /// what a run produced -- may be deleted, and there is no way to exercise them
    /// against a real Windows Sandbox without opening one on the developer's machine
    /// and leaving it running to reach the interesting case.
    /// </remarks>
    internal SandboxTransport(
        ProcessRunner processes,
        ToolResolver tools,
        string stagingRoot,
        string powerShellHome,
        Func<bool> machineIsBusy,
        Func<string, CancellationToken, Task<ProcessRunResult>> launch,
        TimeSpan? pollInterval = null,
        TimeSpan? machineFreeTimeout = null)
        : this(
            processes,
            ResolveSandbox(tools),
            stagingRoot,
            RequireHome(powerShellHome),
            machineIsBusy,
            launch,
            pollInterval,
            machineFreeTimeout)
    {
    }

    private SandboxTransport(
        ProcessRunner processes,
        ResolvedTool sandbox,
        string stagingRoot,
        string? powerShellHome,
        Func<bool>? machineIsBusy,
        Func<string, CancellationToken, Task<ProcessRunResult>>? launch,
        TimeSpan? pollInterval,
        TimeSpan? machineFreeTimeout)
    {
        ArgumentNullException.ThrowIfNull(processes);
        ArgumentException.ThrowIfNullOrWhiteSpace(stagingRoot);
        this.sandbox = sandbox;
        this.stagingRoot = stagingRoot;
        this.powerShellHome = powerShellHome;
        this.machineIsBusy = machineIsBusy ?? MachineIsBusy;
        this.pollInterval = pollInterval ?? DefaultPollInterval;
        this.machineFreeTimeout = machineFreeTimeout ?? DefaultMachineFreeTimeout;
        this.launch = launch ?? ((configurationPath, cancellationToken) => processes.RunAsync(
            new ProcessRunRequest(this.sandbox.Path!, configurationPath) { Timeout = LaunchTimeout },
            cancellationToken));
    }

    /// <summary>
    /// How long the lifecycle is still asked about after the caller has cancelled.
    /// </summary>
    /// <remarks>
    /// The question cannot be skipped -- deleting a live machine's mapped folder is
    /// the thing being prevented -- but a person who pressed Ctrl-C is waiting, so it
    /// is asked briefly and answered honestly either way.
    /// </remarks>
    private TimeSpan CancelledMachineGrace =>
        this.machineFreeTimeout < TimeSpan.FromSeconds(15) ? this.machineFreeTimeout : TimeSpan.FromSeconds(15);

    /// <inheritdoc/>
    public string Name => "sandbox";

    /// <inheritdoc/>
    /// <remarks>
    /// Not a statement about what Windows Sandbox can do. Nothing here measures the
    /// session a worker lands in, so this transport cannot back the claim, and a
    /// transport that made it anyway would be the harness asserting something it never
    /// checked -- on exactly the question that separates a guest which can show a
    /// picture from one that only answers.
    /// </remarks>
    public bool ProvesInteractiveGuest => false;

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
        if (!await this.WaitForFreeMachineAsync(this.machineFreeTimeout).ConfigureAwait(false))
        {
            return DisposableOsRun.Unavailable(
                "a Windows Sandbox is already running on this machine and only one may run at a time");
        }

        var staging = Path.Combine(this.stagingRoot, "sandbox-" + Guid.NewGuid().ToString("N"));

        // Assigned in the finally so the cancellation path reaches the same rules,
        // and read after it: the value a return statement hands back is fixed before
        // the finally runs, so collecting there and returning from inside the try
        // handed every caller the outcome the run STARTED with.
        var evidence = EvidenceOutcome.NotRequested;
        var machineEnded = false;
        var outcome = DisposableOsRun.Faulted("the sandbox run produced no outcome");
        try
        {
            outcome = await this.RunStagedAsync(staging, request, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            // The order is the contract. A worker deadline and a caller cancellation
            // both say the WORKER stopped answering; neither says the machine is
            // gone, and the staging directory is still mapped into it.
            machineEnded = await this.WaitForFreeMachineAsync(
                cancellationToken.IsCancellationRequested ? this.CancelledMachineGrace : this.machineFreeTimeout)
                .ConfigureAwait(false);

            evidence = CollectEvidence(staging, request.EvidenceDirectory);

            // Deleting is the one irreversible step here: a directory left behind is
            // recoverable, evidence is not. So it happens only when the machine that
            // could still be writing into it is known to be gone AND nothing in it
            // failed to come out.
            if (machineEnded && (evidence.IsComplete || evidence.State == EvidenceOutcomeState.Missing))
            {
                TryDelete(staging);
            }
        }

        return (machineEnded ? outcome : MachineStillRunning(outcome, staging)) with { Evidence = evidence };
    }

    /// <summary>
    /// Everything between a prepared staging directory and the worker result. Split
    /// out so the cleanup rules are one block that every path passes through.
    /// </summary>
    private async Task<DisposableOsRun> RunStagedAsync(
        string staging, DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        var preparation = this.Prepare(staging, request);
        if (preparation.Failure is not null)
        {
            return DisposableOsRun.Faulted(preparation.Failure);
        }

        var started = await this.launch(preparation.ConfigurationPath, cancellationToken).ConfigureAwait(false);
        if (!started.Succeeded)
        {
            return DisposableOsRun.Faulted(
                $"WindowsSandbox.exe exited {started.ExitCode} without starting the worker: {started.StandardError}");
        }

        return await AwaitResultAsync(preparation, request.Timeout, this.pollInterval, cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>
    /// The outcome restated for a machine that outlived its worker: nothing was
    /// cleaned up, and the run cannot be read as finished.
    /// </summary>
    /// <remarks>
    /// A result document a worker really wrote is still true, but a machine holding
    /// the staging directory open is not something to stay quiet about: the next gate
    /// finds the one sandbox this machine allows already taken, and the kept staging
    /// directory is where this run output still is.
    /// </remarks>
    private static DisposableOsRun MachineStillRunning(DisposableOsRun outcome, string staging) =>
        DisposableOsRun.Faulted(
            $"{outcome.Detail}; the Windows Sandbox is still running, so the staging directory was kept "
            + $"rather than deleted out from under it: {staging}");

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
        SandboxPreparation preparation, TimeSpan timeout, TimeSpan pollInterval, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.Add(timeout < TimeSpan.Zero ? TimeSpan.Zero : timeout);
        while (!File.Exists(preparation.MarkerPath))
        {
            if (DateTime.UtcNow >= deadline)
            {
                return DisposableOsRun.Faulted(
                    $"the sandbox worker did not finish within {timeout.TotalMinutes:0} minute(s); no marker was written");
            }

            await Task.Delay(pollInterval, cancellationToken).ConfigureAwait(false);
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
    /// <remarks>
    /// Deliberately not cancellable: it is called from the cleanup path, where a
    /// cancellation is precisely the reason the answer is needed. A caller in a hurry
    /// passes a shorter timeout instead.
    /// </remarks>
    private async Task<bool> WaitForFreeMachineAsync(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow.Add(timeout);
        while (this.machineIsBusy())
        {
            if (DateTime.UtcNow >= deadline)
            {
                return false;
            }

            await Task.Delay(this.pollInterval, CancellationToken.None).ConfigureAwait(false);
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

        // Every value that reaches the document is escaped, not only the command: a
        // mapped folder is element content too, and a host directory may legitimately
        // contain & or a bracket. An unescaped one makes the .wsb malformed, which
        // Windows Sandbox reports as a modal dialog standing on someone's desktop.
        var networking = request.RequiresNetwork ? "Default" : "Disable";
        return $"""
            <Configuration>
              <VGpu>Disable</VGpu>
              <Networking>{networking}</Networking>
              <MappedFolders>
                <MappedFolder>
                  <HostFolder>{SecurityElement.Escape(staging)}</HostFolder>
                  <ReadOnly>false</ReadOnly>
                </MappedFolder>
                <MappedFolder>
                  <HostFolder>{SecurityElement.Escape(this.powerShellHome!)}</HostFolder>
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
