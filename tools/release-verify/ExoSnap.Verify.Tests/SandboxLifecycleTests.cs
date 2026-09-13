using System.Collections.ObjectModel;
using System.Xml.Linq;
using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// When a sandbox run may destroy the only copy of what it produced.
/// </summary>
/// <remarks>
/// A worker that stopped answering says nothing about the machine it was running
/// in: Windows Sandbox keeps running, with the staging directory still mapped into
/// it, until something inside ends it. Cleaning up on the worker's deadline
/// therefore deletes a live machine's mapped folder -- and with it the evidence
/// and the result document that machine may still be writing.
///
/// These cases pin the order: collect first, delete only once the machine is known
/// to be gone, and say which resource was kept when it is not.
/// </remarks>
public sealed class SandboxLifecycleTests : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(), "exosnap-sandbox-lifecycle-" + Guid.NewGuid().ToString("N"));

    private readonly ProcessRunner processes = new();

    public SandboxLifecycleTests()
    {
        this.StagingRoot = Path.Combine(this.root, "staging");
        this.PowerShellHome = Path.Combine(this.root, "PowerShell-7");
        Directory.CreateDirectory(this.StagingRoot);
        Directory.CreateDirectory(this.PowerShellHome);
    }

    private string StagingRoot { get; }

    private string PowerShellHome { get; }

    public void Dispose()
    {
        this.processes.Dispose();
        if (Directory.Exists(this.root))
        {
            Directory.Delete(this.root, recursive: true);
        }
    }

    // ---- What the run reports ------------------------------------------------

    [Fact]
    public async Task ARunCarriesTheEvidenceItActuallyCollected()
    {
        // The evidence outcome is computed while the run is being cleaned up, which
        // is after the value a return statement hands back has been fixed -- so the
        // caller used to receive NotRequested from every run, however the collection
        // had gone. A caller cannot tell a complete collection from a destroyed one.
        var collected = Path.Combine(this.root, "collected");
        var run = await this.RunAsync(
            guest: staging =>
            {
                File.WriteAllText(Path.Combine(staging, "evidence", "msi.log"), "installed");
                WriteResult(staging);
            },
            evidenceDirectory: collected,
            cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.Equal(EvidenceOutcomeState.Complete, run.Evidence.State);
        Assert.Equal(1, run.Evidence.FilesCollected);
        Assert.True(File.Exists(Path.Combine(collected, "msi.log")));
    }

    [Fact]
    public async Task ARunThatAskedForNoEvidenceStillSaysSo()
    {
        var run = await this.RunAsync(guest: WriteResult, cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.Equal(EvidenceOutcomeState.NotRequested, run.Evidence.State);
    }

    // ---- The lifecycle -------------------------------------------------------

    [Fact]
    public async Task ANormalRunWhoseMachineExitsCleansUp()
    {
        var run = await this.RunAsync(guest: WriteResult, cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.Empty(Directory.EnumerateDirectories(this.StagingRoot));
    }

    [Fact]
    public async Task AWorkerTimeoutWhileTheMachineStaysLiveKeepsTheStagingDirectory()
    {
        // The defect. The worker's deadline passed, so the run is faulted -- but the
        // sandbox is still running with this directory mapped into it, and deleting
        // it now takes the result document out from under a machine that may still
        // be writing one.
        var run = await this.RunAsync(
            guest: _ => { },
            machineEndsAfterRun: false,
            cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Faulted, run.Kind);
        Assert.Contains("still running", run.Detail, StringComparison.Ordinal);
        var kept = Assert.Single(Directory.EnumerateDirectories(this.StagingRoot));
        Assert.Contains(kept, run.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AWorkerTimeoutWhoseMachineDidEndStillCollectsItsEvidence()
    {
        // A worker that ran out of time is the one whose logs are worth the most, so
        // the deadline must not skip the collection.
        var collected = Path.Combine(this.root, "collected");
        var run = await this.RunAsync(
            guest: staging => File.WriteAllText(Path.Combine(staging, "evidence", "msi.log"), "partial"),
            evidenceDirectory: collected,
            cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Faulted, run.Kind);
        Assert.Equal(EvidenceOutcomeState.Complete, run.Evidence.State);
        Assert.True(File.Exists(Path.Combine(collected, "msi.log")));
    }

    [Fact]
    public async Task CancellationDoesNotDeleteTheStagingDirectoryBeforeTheMachineIsKnownToBeGone()
    {
        // Ctrl-C reaches the poll loop as a cancellation, which used to unwind
        // straight past the collection into the delete -- while the machine it was
        // waiting for was still running.
        using var cancellation = new CancellationTokenSource();
        var run = this.RunAsync(
            guest: _ => cancellation.Cancel(),
            machineEndsAfterRun: false,
            cancellationToken: cancellation.Token);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await run);

        Assert.Single(Directory.EnumerateDirectories(this.StagingRoot));
    }

    [Fact]
    public async Task ACancelledRunWhoseMachineEndedIsStillCollectedAndCleanedUp()
    {
        using var cancellation = new CancellationTokenSource();
        var collected = Path.Combine(this.root, "collected");
        var run = this.RunAsync(
            guest: staging =>
            {
                File.WriteAllText(Path.Combine(staging, "evidence", "msi.log"), "partial");
                cancellation.Cancel();
            },
            evidenceDirectory: collected,
            cancellationToken: cancellation.Token);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await run);

        Assert.True(File.Exists(Path.Combine(collected, "msi.log")));
        Assert.Empty(Directory.EnumerateDirectories(this.StagingRoot));
    }

    [Fact]
    public async Task AMachineThatWasAlreadyBusyIsNeverEndedByThisTransport()
    {
        // One of those processes may be a sandbox the developer opened. The
        // transport waits for it and gives up; it does not kill anything.
        var run = await this.RunAsync(
            guest: WriteResult,
            machineBusyBeforeRun: true,
            cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Unavailable, run.Kind);
        Assert.Contains("already running", run.Detail, StringComparison.Ordinal);
    }

    // ---- The configuration document -----------------------------------------

    [Fact]
    public void AHostPathWithMarkupCharactersStillProducesParseableXml()
    {
        // A mapped folder is written into the .wsb as element content. Of the five
        // characters XML reserves, Windows rejects < > and " in a path outright and
        // permits & and the apostrophe -- so those two are the whole exposure, and a
        // user folder named "Ann & Bob" is what made the document malformed. Windows
        // Sandbox reports that as a modal dialog about an invalid configuration,
        // standing on someone's desktop until they click it.
        var worker = this.WriteWorker("worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "Ann & Bob's run");

        var preparation = this.Transport().Prepare(
            staging, new DisposableOsWorkerRequest("worker.ps1", [worker], []));

        Assert.Null(preparation.Failure);
        var document = XDocument.Parse(File.ReadAllText(preparation.ConfigurationPath));
        var mapped = document.Root!.Element("MappedFolders")!.Elements("MappedFolder").First();
        Assert.Equal(staging, mapped.Element("HostFolder")!.Value);
    }

    [Fact]
    public void NetworkingIsDisabledUnlessTheRunDeclaresItNeedsIt()
    {
        // Not a default: a worker that reaches the internet without saying it would
        // is a run nobody can reason about, and an offline gate that silently had
        // networking proves less than it looks like it does.
        var worker = this.WriteWorker("worker.ps1");

        var offline = this.Transport().Prepare(
            Path.Combine(this.StagingRoot, "offline"),
            new DisposableOsWorkerRequest("worker.ps1", [worker], []));
        Assert.Equal("Disable", Networking(offline));

        var online = this.Transport().Prepare(
            Path.Combine(this.StagingRoot, "online"),
            new DisposableOsWorkerRequest("worker.ps1", [worker], []) { RequiresNetwork = true });
        Assert.Equal("Default", Networking(online));
    }

    private static string Networking(SandboxPreparation preparation) =>
        XDocument.Parse(File.ReadAllText(preparation.ConfigurationPath)).Root!.Element("Networking")!.Value;

    private static void WriteResult(string staging)
    {
        File.WriteAllText(
            Path.Combine(staging, "result.json"),
            """{"steps":[{"name":"install-base","ok":true,"detail":"ok","kind":"bootstrap"}]}""");
        File.WriteAllText(Path.Combine(staging, "done.marker"), "done");
    }

    /// <summary>
    /// Runs the transport with the machine faked out: <paramref name="guest"/> stands
    /// in for what the worker does inside the sandbox, and is called with the staging
    /// directory at the moment the launcher would have started it.
    /// </summary>
    private Task<DisposableOsRun> RunAsync(
        Action<string> guest,
        string? evidenceDirectory = null,
        bool machineEndsAfterRun = true,
        bool machineBusyBeforeRun = false,
        CancellationToken cancellationToken = default)
    {
        var busy = machineBusyBeforeRun;
        var transport = new SandboxTransport(
            this.processes,
            SandboxOnlyTools(),
            this.StagingRoot,
            this.PowerShellHome,
            machineIsBusy: () => busy,
            launch: (configurationPath, _) =>
            {
                busy = !machineEndsAfterRun;
                guest(Path.GetDirectoryName(configurationPath)!);
                return Task.FromResult(Exited(configurationPath));
            },
            // Real timings would spend minutes per case waiting for a machine that
            // is a lambda here.
            pollInterval: TimeSpan.FromMilliseconds(20),
            machineFreeTimeout: TimeSpan.FromMilliseconds(300));

        var request = new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker("worker.ps1")], [])
        {
            EvidenceDirectory = evidenceDirectory,
            // Short enough that a run with no marker reaches its deadline while the
            // test is still running.
            Timeout = TimeSpan.FromSeconds(1),
        };

        return transport.RunWorkerAsync(request, cancellationToken);
    }

    private static ProcessRunResult Exited(string configurationPath) => new(
        "WindowsSandbox.exe",
        new ReadOnlyCollection<string>([configurationPath]),
        0,
        string.Empty,
        string.Empty,
        false,
        TimeSpan.Zero);

    private static ToolResolver SandboxOnlyTools() => new(
        readEnvironment: _ => null,
        fileExists: path => path.EndsWith("WindowsSandbox.exe", StringComparison.OrdinalIgnoreCase),
        readPath: () => @"C:\Windows\System32");

    private SandboxTransport Transport() =>
        new(this.processes, SandboxOnlyTools(), this.StagingRoot, this.PowerShellHome);

    private string WriteWorker(string fileName)
    {
        var path = Path.Combine(this.root, fileName);
        File.WriteAllText(path, "# worker");
        return path;
    }
}
