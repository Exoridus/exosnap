using System.Collections.ObjectModel;
using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Carrying one worker run out in a throwaway Hyper-V clone.
/// </summary>
/// <remarks>
/// The control plane is the recipe under tools/vm, which already owns the parts that
/// need a Hyper-V host: the differencing disk, the GPU partition and its read-back,
/// the image fingerprint, the interactive agent, and the rescue-before-cleanup order.
/// This transport composes a run of it and turns what comes back into the same typed
/// result every other transport produces, so a gate body does not know which machine
/// it ran on.
///
/// The recipe itself is exercised by scripts/tests/vm-recipe.tests.ps1. What is
/// checked here is the composition and the reading -- the parts that decide whether a
/// run is reported as a product verdict or as the harness failing to set one up.
/// </remarks>
public sealed class HyperVRunTests : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(), "exosnap-hyperv-run-" + Guid.NewGuid().ToString("N"));

    private readonly ProcessRunner processes = new();

    private readonly List<ProcessRunRequest> invocations = [];

    public HyperVRunTests()
    {
        Directory.CreateDirectory(Path.Combine(this.root, "tools", "vm"));
        File.WriteAllText(Path.Combine(this.root, "tools", "vm", "Invoke-ReleaseVmRun.ps1"), "# recipe");
        Directory.CreateDirectory(this.StagingRoot);
    }

    private string StagingRoot => Path.Combine(this.root, "staging");

    public void Dispose()
    {
        this.processes.Dispose();
        if (Directory.Exists(this.root))
        {
            Directory.Delete(this.root, recursive: true);
        }
    }

    [Fact]
    public void ItProvesAnInteractiveGuestBecauseTheRecipeMeasuresOne()
    {
        // The claim the runner's capability filter reads. It is backed: the recipe's
        // readiness step holds the agent's own receipt against the run requirement and
        // refuses a guest whose session is not where a campaign belongs.
        Assert.True(this.Transport().ProvesInteractiveGuest);
    }

    [Fact]
    public void ItIsUnavailableWithoutHyperVAndSaysWhy()
    {
        var transport = new HyperVTransport(
            this.processes,
            this.root,
            new HyperVAccess(ModulePresent: false, InAdministratorsGroup: false, Elevated: false, HostServiceRunning: false),
            this.StagingRoot);

        Assert.False(transport.Available);
        Assert.Contains("Hyper-V", transport.UnavailableReason, StringComparison.Ordinal);
    }

    [Fact]
    public void ItIsUnavailableWhenTheRecipeIsNotInTheRepository()
    {
        var transport = new HyperVTransport(
            this.processes, Path.Combine(this.root, "elsewhere"), Granted, this.StagingRoot);

        Assert.False(transport.Available);
        Assert.Contains("Invoke-ReleaseVmRun.ps1", transport.UnavailableReason, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ARunAsksTheRecipeForWhatTheRequestDeclared()
    {
        var run = await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker()], ["-Channel", "preview"])
            {
                RequiresInteractiveGuest = true,
                RequiresNetwork = true,
            },
            guest: WriteResult);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        var arguments = Assert.Single(this.invocations).Arguments;
        Assert.Contains("-RequireInteractiveGuest", arguments);
        Assert.Contains("-Network", arguments);
        Assert.Contains("Connected", arguments);
    }

    [Fact]
    public async Task ARunThatNeedsNoNetworkAsksForNone()
    {
        await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker()], []), guest: WriteResult);

        var arguments = Assert.Single(this.invocations).Arguments;
        Assert.Contains("Disconnected", arguments);
        Assert.DoesNotContain("-RequireInteractiveGuest", arguments);
    }

    [Fact]
    public async Task AWorkerThatIsNotStagedIsFaultedBeforeAMachineIsStarted()
    {
        var run = await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [], []), guest: WriteResult);

        Assert.Equal(DisposableOsRunKind.Faulted, run.Kind);
        Assert.Empty(this.invocations);
    }

    [Fact]
    public async Task ARecipeThatFailedIsFaultedRatherThanReadAsAVerdict()
    {
        // The run never produced a result document, so there is nothing to draw a
        // product conclusion from. Reporting one anyway would be the harness blaming
        // the product for a machine it could not build.
        var run = await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker()], []),
            guest: _ => { },
            exitCode: 3,
            standardError: "'ExoSnap-Run-x' is not ready for this run: the agent runs in session 0");

        Assert.Equal(DisposableOsRunKind.Faulted, run.Kind);
        Assert.Contains("session 0", run.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ANonZeroCampaignExitWithAResultIsStillAVerdict()
    {
        // A campaign that found defects exits non-zero, and that is a result. Only the
        // absence of a result document means nothing was measured.
        var run = await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker()], []),
            guest: staging => File.WriteAllText(
                Path.Combine(staging, "result.json"),
                """{"steps":[{"name":"install-base","ok":false,"detail":"assertion did not hold","kind":"product"}]}"""),
            exitCode: 1);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.False(Assert.Single(run.Result!.Steps).Ok);
    }

    [Fact]
    public async Task EvidenceTheRecipeCopiedOutReachesTheCaller()
    {
        var collected = Path.Combine(this.root, "collected");
        var run = await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker()], []) { EvidenceDirectory = collected },
            guest: staging =>
            {
                Directory.CreateDirectory(Path.Combine(staging, "evidence"));
                File.WriteAllText(Path.Combine(staging, "evidence", "msi.log"), "installed");
                WriteResult(staging);
            });

        Assert.Equal(EvidenceOutcomeState.Complete, run.Evidence.State);
        Assert.True(File.Exists(Path.Combine(collected, "msi.log")));
    }

    [Fact]
    public async Task EvidenceThatCouldNotBeCollectedKeepsTheResultDirectory()
    {
        // The same rule the sandbox transport follows, for the same reason: a
        // directory left behind is recoverable and evidence is not.
        var run = await this.RunAsync(
            new DisposableOsWorkerRequest("worker.ps1", [this.WriteWorker()], [])
            {
                EvidenceDirectory = Path.Combine(this.root, "collected"),
            },
            guest: WriteResult);

        Assert.Equal(EvidenceOutcomeState.Missing, run.Evidence.State);
        Assert.Contains("wrote no", run.Evidence.Detail, StringComparison.Ordinal);
    }

    private static HyperVAccess Granted => new(
        ModulePresent: true, InAdministratorsGroup: true, Elevated: false, HostServiceRunning: true);

    private static void WriteResult(string resultDirectory) =>
        File.WriteAllText(
            Path.Combine(resultDirectory, "result.json"),
            """{"steps":[{"name":"install-base","ok":true,"detail":"ok","kind":"bootstrap"}]}""");

    private HyperVTransport Transport() =>
        new(this.processes, this.root, Granted, this.StagingRoot);

    private Task<DisposableOsRun> RunAsync(
        DisposableOsWorkerRequest request,
        Action<string> guest,
        int exitCode = 0,
        string standardError = "")
    {
        var transport = new HyperVTransport(
            this.processes,
            this.root,
            Granted,
            this.StagingRoot,
            invoke: (invocation, _) =>
            {
                this.invocations.Add(invocation);
                var resultDirectory = ResultDirectoryOf(invocation);
                Directory.CreateDirectory(resultDirectory);
                guest(resultDirectory);
                return Task.FromResult(new ProcessRunResult(
                    invocation.FileName,
                    new ReadOnlyCollection<string>([.. invocation.Arguments]),
                    exitCode,
                    string.Empty,
                    standardError,
                    false,
                    TimeSpan.Zero));
            });

        return transport.RunWorkerAsync(request, TestContext.Current.CancellationToken);
    }

    /// <summary>
    /// Where the recipe puts one run's evidence: a directory named by the run id
    /// under the result root it was given.
    /// </summary>
    private static string ResultDirectoryOf(ProcessRunRequest invocation) =>
        Path.Combine(ValueOf(invocation, "-ResultRoot"), ValueOf(invocation, "-RunId"));

    private static string ValueOf(ProcessRunRequest invocation, string name)
    {
        var arguments = invocation.Arguments.ToList();
        var index = arguments.IndexOf(name);
        return index >= 0 && index + 1 < arguments.Count ? arguments[index + 1] : string.Empty;
    }

    private string WriteWorker()
    {
        var path = Path.Combine(this.root, "worker.ps1");
        File.WriteAllText(path, "# worker");
        return path;
    }
}
