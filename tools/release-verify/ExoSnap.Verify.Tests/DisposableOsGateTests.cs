using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

public sealed class DisposableOsHarnessPlumbingTests
{
    [Fact]
    public async Task TheHarnessDisposableOsFakeIsReachableFromAGateContext()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes => fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([])),
            TestContext.Current.CancellationToken);

        var services = harness.Context.RequireServices();
        var run = await services.DisposableOs.RunAsync(
            new DisposableOsWorkerRequest("noop.ps1", [], []), TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.Single(harness.Fakes.DisposableOs.Requests);
    }
}

/// <summary>REL-UPD-MSI-DECLINE-001: UpdateDeclineGate.</summary>
public sealed class UpdateDeclineGateTests : IDisposable
{
    private readonly string baseMsi = Path.Combine(
        Path.GetTempPath(),
        "exosnap-base-" + Guid.NewGuid().ToString("N") + ".msi");

    public UpdateDeclineGateTests() => File.WriteAllText(this.baseMsi, "msi");

    public void Dispose() => File.Delete(this.baseMsi);

    [Fact]
    public async Task IsUnavailableWhenNoBaseMsiIsConfigured()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001", DisposableOsGateFixture.StageWorker, TestContext.Current.CancellationToken);

        var result = await new UpdateDeclineGate(() => null).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("EXOSNAP_UPDATE_FROM_MSI", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenTheWorkerScriptIsNotInTheRepository()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("sandbox-update-worker.ps1", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenNoTransportCanRun()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Unavailable("no transport"));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task AFaultedRunIsAnInfrastructureErrorNeverAFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Faulted("the sandbox wrote no marker"));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
    }

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("decline-offer", true, "offered"),
            new("decline-apply", true, "applied"),
            new("decline-state", true, "failureCase uacDeclined, installState intact"),
        ])));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        Assert.Equal("sandbox-update-worker.ps1", request.WorkerFileName);
        Assert.Contains(this.baseMsi, request.SourceFiles);
        Assert.Contains(Path.GetFileName(this.baseMsi), request.WorkerArguments);
    }

    [Fact]
    public async Task AFailedStepIsFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("decline-offer", false, "update.check timed out"),
        ])));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
    }

    [Fact]
    public async Task AStepTheWorkerNeverReachedIsAnInfrastructureErrorNotAFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
        ])));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("decline-offer", result.Message, StringComparison.Ordinal);
    }

    private static Task<GateHarness> HarnessAsync(DisposableOsRun run) => GateHarness.CreateAsync(
        "REL-UPD-MSI-DECLINE-001",
        fakes =>
        {
            DisposableOsGateFixture.StageWorker(fakes);
            fakes.DisposableOs.Run = run;
        },
        TestContext.Current.CancellationToken);
}

/// <summary>REL-UPD-MSI-001: UpdateAcceptGate.</summary>
public sealed class UpdateAcceptGateTests : IDisposable
{
    private readonly string baseMsi = Path.Combine(
        Path.GetTempPath(),
        "exosnap-base-" + Guid.NewGuid().ToString("N") + ".msi");

    public UpdateAcceptGateTests() => File.WriteAllText(this.baseMsi, "msi");

    public void Dispose() => File.Delete(this.baseMsi);

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("updater-gone-before-accept", true, "no stale updater"),
            new("accept-offer", true, "offered"),
            new("accept-apply", true, "applied"),
            new("accept-installed", true, "product version advanced"),
        ])));

        var result = await new UpdateAcceptGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }

    [Fact]
    public async Task TheDeclineStepsAreNotRequiredOfTheAcceptGate()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("decline-offer", false, "the decline half failed"),
            new("updater-gone-before-accept", true, "no stale updater"),
            new("accept-offer", true, "offered"),
            new("accept-apply", true, "applied"),
            new("accept-installed", true, "product version advanced"),
        ])));

        var result = await new UpdateAcceptGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }

    private static Task<GateHarness> HarnessAsync(DisposableOsRun run) => GateHarness.CreateAsync(
        "REL-UPD-MSI-001",
        fakes =>
        {
            DisposableOsGateFixture.StageWorker(fakes);
            fakes.DisposableOs.Run = run;
        },
        TestContext.Current.CancellationToken);
}

/// <summary>REL-PKG-CHOCO-001: ChocolateyRehearsalGate.</summary>
public sealed class ChocolateyRehearsalGateTests
{
    private const string FakeDigest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    private static readonly ReleaseMsiLookup NoInstaller =
        new(null, string.Empty, "no ExoSnap-<version>-windows-x64.msi was found beside the bound artifact");

    private static readonly DisposableOsRunResult RehearsalPassed = new(
    [
        new("prepare", true, "nuspec rewritten"),
        new("pack", true, "packed"),
        new("removeExisting", true, "no prior install"),
        new("install", true, "installed"),
        new("uninstall", true, "uninstalled"),
        new("restore", true, "release MSI reinstalled"),
    ]);

    [Fact]
    public async Task IsUnavailableWhenThePackageSourceIsMissing()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PKG-CHOCO-001", DisposableOsGateFixture.StageChocoWorker, TestContext.Current.CancellationToken);

        var result = await new ChocolateyRehearsalGate(_ => NoInstaller).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("packaging/chocolatey", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenNoReleaseMsiCanBeIdentified()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(RehearsalPassed));

        var result = await new ChocolateyRehearsalGate(_ => NoInstaller).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("ExoSnap-", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(RehearsalPassed));
        var msi = DisposableOsGateFixture.StageReleaseMsi(harness);

        var result = await new ChocolateyRehearsalGate(_ => new ReleaseMsiLookup(msi, FakeDigest, string.Empty)).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        Assert.Equal("sandbox-choco-worker.ps1", request.WorkerFileName);

        // The package source goes in as the directory itself: the worker reads
        // tools/chocolateyinstall.ps1 underneath it.
        Assert.Contains(
            request.SourceFiles,
            source => source.EndsWith(Path.Combine("packaging", "chocolatey"), StringComparison.Ordinal));
        Assert.Contains("chocolatey", request.WorkerArguments);
        Assert.Contains("-MsiSha256", request.WorkerArguments);
        Assert.NotNull(request.EvidenceDirectory);
    }

    [Fact]
    public async Task AFailedRehearsalStepIsFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("prepare", true, "nuspec rewritten"),
            new("pack", true, "packed"),
            new("removeExisting", true, "no prior install"),
            new("install", false, "choco install exited 1"),
            new("uninstall", true, "uninstalled"),
            new("restore", true, "release MSI reinstalled"),
        ])));
        var msi = DisposableOsGateFixture.StageReleaseMsi(harness);

        var result = await new ChocolateyRehearsalGate(_ => new ReleaseMsiLookup(msi, FakeDigest, string.Empty)).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
    }

    private static Task<GateHarness> HarnessAsync(DisposableOsRun run) => GateHarness.CreateAsync(
        "REL-PKG-CHOCO-001",
        fakes =>
        {
            DisposableOsGateFixture.StageChocoWorker(fakes);
            DisposableOsGateFixture.StagePackageSource(fakes);
            fakes.DisposableOs.Run = run;
        },
        TestContext.Current.CancellationToken);
}

public sealed class ReleaseMsiArtifactTests : IDisposable
{
    private readonly string directory = Path.Combine(
        Path.GetTempPath(),
        "exosnap-release-msi-" + Guid.NewGuid().ToString("N"));

    public ReleaseMsiArtifactTests()
    {
        Directory.CreateDirectory(this.directory);
        File.WriteAllText(this.ExePath(), "exe");
    }

    public void Dispose() => Directory.Delete(this.directory, recursive: true);

    [Fact]
    public void AnOverrideThatNamesNothingIsReportedAsSuch()
    {
        var lookup = ReleaseMsiArtifact.Locate(this.ExePath(), _ => Path.Combine(this.directory, "absent.msi"));

        Assert.Null(lookup.Path);
        Assert.Contains("EXOSNAP_RELEASE_MSI", lookup.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void AFileNotNamedLikeAPublishedReleaseIsNotIdentified()
    {
        File.WriteAllText(Path.Combine(this.directory, "installer.msi"), "msi");

        var lookup = ReleaseMsiArtifact.Locate(this.ExePath(), _ => null);

        Assert.Null(lookup.Path);
    }

    [Fact]
    public void TwoCandidatesAreAmbiguousRatherThanAGuess()
    {
        File.WriteAllText(Path.Combine(this.directory, "ExoSnap-0.9.0-windows-x64.msi"), "a");
        File.WriteAllText(Path.Combine(this.directory, "ExoSnap-0.9.1-windows-x64.msi"), "b");

        var lookup = ReleaseMsiArtifact.Locate(this.ExePath(), _ => null);

        Assert.Null(lookup.Path);
        Assert.Contains("EXOSNAP_RELEASE_MSI", lookup.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void APublishedNameOverAnythingButAnInstallerIsNotIdentified()
    {
        // The name pattern is the cheap half of identification; the package's own
        // Property table is the half a renamed file cannot pass.
        var msi = Path.Combine(this.directory, "ExoSnap-0.9.0-windows-x64.msi");
        File.WriteAllText(msi, "not an installer");

        var lookup = ReleaseMsiArtifact.Locate(this.ExePath(), _ => null);

        Assert.Null(lookup.Path);
        Assert.Contains("Property table", lookup.Detail, StringComparison.Ordinal);
    }

    private string ExePath() => Path.Combine(this.directory, "exosnap.exe");
}

internal static class DisposableOsGateFixture
{
    /// <summary>Puts the update worker script where a gate looks for it in the repository.</summary>
    internal static void StageWorker(GateFakes fakes) => StageScript(fakes, UpdateDeclineGate.WorkerFileName);

    /// <summary>Puts the Chocolatey worker script and a plausible executable in place.</summary>
    internal static void StageChocoWorker(GateFakes fakes)
    {
        StageScript(fakes, ChocolateyRehearsalGate.WorkerFileName);

        // The default fake path is a bare file name; the MSI lookup needs a real
        // directory to look beside.
        fakes.ExecutablePath = Path.Combine(fakes.RepositoryRoot, "dist", "exosnap.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(fakes.ExecutablePath)!);
        File.WriteAllText(fakes.ExecutablePath, "exe");
    }

    /// <summary>Creates the tracked package directory the rehearsal stages.</summary>
    internal static void StagePackageSource(GateFakes fakes)
    {
        var tools = Path.Combine(fakes.RepositoryRoot, "packaging", "chocolatey", "tools");
        Directory.CreateDirectory(tools);
        File.WriteAllText(Path.Combine(tools, "chocolateyinstall.ps1"), "# install");
        File.WriteAllText(
            Path.Combine(fakes.RepositoryRoot, "packaging", "chocolatey", "exosnap.nuspec"), "<package />");
    }

    /// <summary>Publishes an MSI beside the bound artifact, the way a release does.</summary>
    internal static string StageReleaseMsi(GateHarness harness)
    {
        var path = Path.Combine(
            Path.GetDirectoryName(harness.Fakes.ExecutablePath)!, "ExoSnap-0.9.0-windows-x64.msi");
        File.WriteAllText(path, "msi");
        return path;
    }

    private static void StageScript(GateFakes fakes, string fileName)
    {
        var path = Path.Combine(fakes.RepositoryRoot, "scripts", "lib", fileName);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "# worker");
    }

    /// <summary>
    /// Writes a worker that pulls in another script, and that script, so a payload
    /// test has a dependency graph to be right or wrong about.
    /// </summary>
    internal static void StageScriptWithDependency(GateFakes fakes, string workerFileName, string dependencyFileName)
    {
        var lib = Path.Combine(fakes.RepositoryRoot, "scripts", "lib");
        Directory.CreateDirectory(lib);
        File.WriteAllText(
            Path.Combine(lib, workerFileName),
            $"Import-Module (Join-Path $StagingDirectory '{dependencyFileName}') -Force");
        File.WriteAllText(Path.Combine(lib, dependencyFileName), "# module");
    }
}

/// <summary>
/// A gate stages every script its worker pulls in.
/// </summary>
/// <remarks>
/// The worker runs inside a machine with nothing but its staging directory, so a
/// dependency the gate did not stage is a run that dies at the first line needing
/// it -- reported as whatever PowerShell says about a missing module, which reads
/// nothing like an incomplete payload. Two were missing, one of them only
/// transitively, and both were missing because the list was written by hand.
/// </remarks>
public sealed class DisposableOsPayloadContractTests : IDisposable
{
    private readonly string baseMsi = Path.Combine(
        Path.GetTempPath(), "payload-contract-" + Guid.NewGuid().ToString("N") + ".msi");

    public DisposableOsPayloadContractTests() => File.WriteAllText(this.baseMsi, "msi");

    public void Dispose() => File.Delete(this.baseMsi);

    [Fact]
    public async Task TheUpdateGateStagesWhatItsWorkerImports()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes =>
            {
                DisposableOsGateFixture.StageScriptWithDependency(
                    fakes, UpdateDeclineGate.WorkerFileName, "LiveVerifyClient.psm1");
                fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([]));
            },
            TestContext.Current.CancellationToken);

        await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        var worker = Path.Combine(
            harness.Fakes.RepositoryRoot, "scripts", "lib", UpdateDeclineGate.WorkerFileName);

        Assert.Empty(WorkerPayload.MissingFrom(worker, request.SourceFiles, WorkerPayload.ReadFileOrNull));
        Assert.Contains(request.SourceFiles, path => Path.GetFileName(path) == "LiveVerifyClient.psm1");
    }

    [Fact]
    public async Task TheUpdateGateAsksForItsEvidence()
    {
        // It did not, so a failed update run left its MSI logs and updater state
        // inside a machine that was then discarded -- the one run whose logs were
        // worth having.
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes =>
            {
                DisposableOsGateFixture.StageWorker(fakes);
                fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([]));
            },
            TestContext.Current.CancellationToken);

        await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        Assert.False(string.IsNullOrWhiteSpace(request.EvidenceDirectory));
    }

    [Fact]
    public async Task TheChocolateyGateStagesItsRehearsalWorkerAndKeepsThePackageDirectory()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PKG-CHOCO-001",
            fakes =>
            {
                DisposableOsGateFixture.StageChocoWorker(fakes);
                DisposableOsGateFixture.StagePackageSource(fakes);
                DisposableOsGateFixture.StageScriptWithDependency(
                    fakes, ChocolateyRehearsalGate.WorkerFileName, "choco-rehearsal-worker.ps1");
                fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([]));
            },
            TestContext.Current.CancellationToken);
        var msi = DisposableOsGateFixture.StageReleaseMsi(harness);

        // The lookup is injected: identifying a real MSI means reading its Property
        // table, and this test is about the payload, not about MSI identity (which
        // ReleaseMsiArtifact's own tests cover).
        var result = await new ChocolateyRehearsalGate(_ => new ReleaseMsiLookup(msi, new string('a', 64), "staged"))
            .RunAsync(harness.Context, TestContext.Current.CancellationToken);
        Assert.True(
            harness.Fakes.DisposableOs.Requests.Count == 1,
            $"the gate never reached the transport: {result.Outcome} -- {result.Message}");

        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        var worker = Path.Combine(
            harness.Fakes.RepositoryRoot, "scripts", "lib", ChocolateyRehearsalGate.WorkerFileName);

        Assert.Empty(WorkerPayload.MissingFrom(worker, request.SourceFiles, WorkerPayload.ReadFileOrNull));

        // The package source stays a directory: the worker reads
        // tools/chocolateyinstall.ps1 underneath it, so a flattened payload would
        // lose the only structure it depends on.
        Assert.Contains(
            request.SourceFiles,
            path => Directory.Exists(path) && Path.GetFileName(path) == "chocolatey");
    }

    [Fact]
    public async Task AWorkerWhoseDependencyIsNotStagedIsCaughtHere()
    {
        // The guard's own premise: if the derivation stopped finding anything, every
        // test above would pass on an empty answer. This stages a worker that needs
        // a module and then asks about a payload that omits it.
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes => DisposableOsGateFixture.StageScriptWithDependency(
                fakes, UpdateDeclineGate.WorkerFileName, "Needed.psm1"),
            TestContext.Current.CancellationToken);

        var worker = Path.Combine(
            harness.Fakes.RepositoryRoot, "scripts", "lib", UpdateDeclineGate.WorkerFileName);

        Assert.Equal(
            ["Needed.psm1"],
            WorkerPayload.MissingFrom(worker, [worker], WorkerPayload.ReadFileOrNull));
    }
}
