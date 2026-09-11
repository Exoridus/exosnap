using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

public sealed class DisposableOsVerdictTests
{
    private static readonly string[] DeclineSteps =
        ["install-base", "decline-offer", "decline-apply", "decline-state"];

    [Fact]
    public void EveryRequiredStepPresentAndOkIsPass()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-pass.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Pass, verdict.Kind);
        Assert.Contains("4", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStepThatRanAndFailedIsFail()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-fail.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("decline-apply", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ARequiredStepTheWorkerNeverReachedIsUnverified()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-incomplete.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void NoResultDocumentAtAllIsUnverified()
    {
        var verdict = DisposableOsVerdict.From(null, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("no result document", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void UnparseableJsonParsesToNull()
    {
        Assert.Null(DisposableOsRunResult.Parse("{ not json"));
    }

    [Fact]
    public void AStepWithoutANameIsUnverifiedRatherThanACrash()
    {
        var result = DisposableOsRunResult.Parse("""{"steps":[{"ok":true,"detail":"ran"}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("install-base", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStepWithoutADetailStillReportsItsFailure()
    {
        var result = DisposableOsRunResult.Parse(
            """{"steps":[{"name":"install-base","ok":true,"detail":"ran"},{"name":"decline-offer","ok":false}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ANullStepEntryIsIgnored()
    {
        var result = DisposableOsRunResult.Parse("""{"steps":[null,{"name":"install-base","ok":true,"detail":"ran"}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ADuplicateStepCannotMaskAFailure()
    {
        var result = DisposableOsRunResult.Parse(
            """{"steps":[{"name":"decline-apply","ok":false,"detail":"applied anyway"},{"name":"decline-apply","ok":true,"detail":"retried"}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("applied anyway", verdict.Message, StringComparison.Ordinal);
    }
}

public sealed class DisposableOsRunnerTests
{
    private sealed class FixedTransport : IDisposableOsTransport
    {
        private readonly DisposableOsRun run;

        public FixedTransport(string name, bool available, DisposableOsRun run)
        {
            this.Name = name;
            this.Available = available;
            this.run = run;
        }

        public string Name { get; }

        public bool Available { get; }

        public string UnavailableReason => this.Available ? string.Empty : $"{this.Name} is not available";

        public List<DisposableOsWorkerRequest> Requests { get; } = [];

        public Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
        {
            this.Requests.Add(request);
            return Task.FromResult(this.run);
        }
    }

    [Fact]
    public async Task SkipsAnUnavailableTransportAndUsesTheNextOne()
    {
        var unavailable = new FixedTransport("primary", available: false, DisposableOsRun.Faulted("unreachable"));
        var completed = DisposableOsRun.Completed(new DisposableOsRunResult([]));
        var fallback = new FixedTransport("fallback", available: true, completed);
        var runner = new DisposableOsRunner([unavailable, fallback]);
        var request = new DisposableOsWorkerRequest("worker.ps1", [], []);

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Same(completed, run);
        Assert.Empty(unavailable.Requests);
        Assert.Single(fallback.Requests);
    }

    [Fact]
    public async Task NoAvailableTransportIsUnavailableNotFaulted()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("primary", available: false, DisposableOsRun.Faulted("n/a")),
        ]);
        var request = new DisposableOsWorkerRequest("worker.ps1", [], []);

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Unavailable, run.Kind);
        Assert.Contains("no disposable-OS transport", run.Detail, StringComparison.Ordinal);
        Assert.Contains("primary is not available", run.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void TransportNamesReportsEveryTransportInOrder()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("sandbox", available: false, DisposableOsRun.Faulted("n/a")),
            new FixedTransport("vm", available: true, DisposableOsRun.Faulted("n/a")),
        ]);

        Assert.Equal(["sandbox", "vm"], runner.TransportNames);
    }
}

public sealed class SandboxTransportTests : IDisposable
{
    private readonly string root = Path.Combine(
        Path.GetTempPath(),
        "exosnap-sandbox-transport-tests-" + Guid.NewGuid().ToString("N"));

    private readonly ProcessRunner processes = new();

    public SandboxTransportTests()
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

    [Fact]
    public void IsUnavailableWhenWindowsSandboxIsNotResolvable()
    {
        var transport = new SandboxTransport(this.processes, UnresolvableTools(), this.StagingRoot, this.PowerShellHome);

        Assert.False(transport.Available);
        Assert.Contains("WindowsSandbox.exe", transport.UnavailableReason, StringComparison.Ordinal);
    }

    [Fact]
    public void IsUnavailableWhenPowerShellSevenIsNotResolvable()
    {
        var transport = new SandboxTransport(this.processes, SandboxOnlyTools(), this.StagingRoot);

        Assert.False(transport.Available);
        Assert.Contains("PowerShell 7", transport.UnavailableReason, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AMissingSourceFileIsFaultedNotUnavailable()
    {
        var transport = new SandboxTransport(this.processes, SandboxOnlyTools(), this.StagingRoot, this.PowerShellHome);
        var request = new DisposableOsWorkerRequest(
            "missing-worker.ps1",
            [Path.Combine(this.StagingRoot, "does-not-exist", "missing-worker.ps1")],
            []);

        var run = await transport.RunWorkerAsync(request, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Faulted, run.Kind);
        Assert.Contains("does not exist", run.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void ADirectorySourceIsStagedWithItsOwnStructure()
    {
        var package = Path.Combine(this.root, "chocolatey");
        Directory.CreateDirectory(Path.Combine(package, "tools"));
        File.WriteAllText(Path.Combine(package, "exosnap.nuspec"), "<package />");
        File.WriteAllText(Path.Combine(package, "tools", "chocolateyinstall.ps1"), "# install");
        var worker = this.WriteWorker("choco-worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest("choco-worker.ps1", [worker, package], ["-PackageSource", "chocolatey"]));

        Assert.Null(preparation.Failure);
        Assert.True(File.Exists(Path.Combine(staging, "chocolatey", "tools", "chocolateyinstall.ps1")));
    }

    [Fact]
    public void AnArgumentNamingAStagedEntryBecomesItsGuestPath()
    {
        var msi = Path.Combine(this.root, "ExoSnap-0.9.0.msi");
        File.WriteAllText(msi, "msi");
        var worker = this.WriteWorker("update-worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest(
                "update-worker.ps1",
                [worker, msi],
                ["-BaseMsiPath", "ExoSnap-0.9.0.msi", "-UpdateChannel", "preview"]));

        Assert.Null(preparation.Failure);
        var configuration = File.ReadAllText(preparation.ConfigurationPath);
        // The command is XML-escaped into the .wsb, so the quoting around each
        // argument reaches the guest as &quot; and is decoded there.
        Assert.Contains(
            @"&quot;C:\Users\WDAGUtilityAccount\Desktop\run\ExoSnap-0.9.0.msi&quot;",
            configuration,
            StringComparison.Ordinal);
        Assert.Contains("&quot;preview&quot;", configuration, StringComparison.Ordinal);
    }

    [Fact]
    public void TheConfigurationMapsStagingWritableAndPowerShellReadOnly()
    {
        var worker = this.WriteWorker("worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest("worker.ps1", [worker], []));

        var configuration = File.ReadAllText(preparation.ConfigurationPath).ReplaceLineEndings("\n");
        Assert.Contains($"<HostFolder>{staging}</HostFolder>\n      <ReadOnly>false</ReadOnly>", configuration, StringComparison.Ordinal);
        Assert.Contains($"<HostFolder>{this.PowerShellHome}</HostFolder>\n      <ReadOnly>true</ReadOnly>", configuration, StringComparison.Ordinal);
        Assert.Contains(@"C:\Users\WDAGUtilityAccount\Desktop\PowerShell-7\pwsh.exe", configuration, StringComparison.Ordinal);
        Assert.DoesNotContain(this.root + "<", configuration, StringComparison.Ordinal);
    }

    [Fact]
    public void AnEvidenceDirectoryIsCreatedInTheGuestAndPassedToTheWorker()
    {
        var worker = this.WriteWorker("choco-worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest("choco-worker.ps1", [worker], [])
            {
                EvidenceDirectory = Path.Combine(this.root, "collected"),
            });

        Assert.Null(preparation.Failure);
        Assert.True(Directory.Exists(Path.Combine(staging, "evidence")));
        Assert.Contains(
            @"-EvidenceDirectory&quot; &quot;C:\Users\WDAGUtilityAccount\Desktop\run\evidence&quot;",
            File.ReadAllText(preparation.ConfigurationPath),
            StringComparison.Ordinal);
    }

    [Fact]
    public void AWorkerWithNoEvidenceDirectoryIsNotPassedOne()
    {
        var worker = this.WriteWorker("update-worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest("update-worker.ps1", [worker], []));

        Assert.False(Directory.Exists(Path.Combine(staging, "evidence")));
        Assert.DoesNotContain(
            "-EvidenceDirectory",
            File.ReadAllText(preparation.ConfigurationPath),
            StringComparison.Ordinal);
    }

    [Fact]
    public void TheGuestEndsItselfSoTheNextRunCanHaveTheMachine()
    {
        var worker = this.WriteWorker("worker.ps1");
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest("worker.ps1", [worker], ["-UpdateChannel", "preview"]));

        var launcher = Path.Combine(staging, "sandbox-run.ps1");
        Assert.True(File.Exists(launcher));
        Assert.Contains("shutdown.exe /s /f /t 0", File.ReadAllText(launcher), StringComparison.Ordinal);

        // The worker is launched through the wrapper and arrives as its first
        // argument, so the shutdown runs whatever the worker did.
        var configuration = File.ReadAllText(preparation.ConfigurationPath);
        Assert.Contains(@"-File &quot;C:\Users\WDAGUtilityAccount\Desktop\run\sandbox-run.ps1&quot;", configuration, StringComparison.Ordinal);
        Assert.Contains(@"&quot;C:\Users\WDAGUtilityAccount\Desktop\run\worker.ps1&quot; &quot;-UpdateChannel&quot;", configuration, StringComparison.Ordinal);
    }

    [Fact]
    public void AWorkerThatIsNotStagedIsAFailureRatherThanAnUnrunnableConfiguration()
    {
        var staging = Path.Combine(this.StagingRoot, "run");

        var preparation = this.Transport().Prepare(
            staging,
            new DisposableOsWorkerRequest("worker.ps1", [], []));

        Assert.NotNull(preparation.Failure);
        Assert.Contains("worker.ps1", preparation.Failure, StringComparison.Ordinal);
    }

    private static ToolResolver UnresolvableTools() => new(
        readEnvironment: _ => null,
        fileExists: _ => false,
        readPath: () => null);

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
