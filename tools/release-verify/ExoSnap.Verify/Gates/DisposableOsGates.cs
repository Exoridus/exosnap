using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-UPD-MSI-DECLINE-001: declining the elevation prompt yields a truthful
/// cancel state.
/// </summary>
/// <remarks>
/// Runs on a disposable machine. The guest's logon token is already
/// administrative, so no UAC prompt is ever raised there; the decline is produced
/// by the updater's own fault-injection seam inside the worker script, which
/// returns ERROR_CANCELLED from the elevation call the way a declined prompt
/// would. Per ADR 0067 ("cancel is not failure") the product assertion is
/// <c>failureCase == uacDeclined</c> with the installation intact, and this gate
/// deliberately never asserts on <c>phase</c>: a decline legitimately reports
/// <c>phase: failed</c>.
/// </remarks>
public sealed class UpdateDeclineGate : IScenarioBody
{
    /// <summary>The guest worker script, staged from the repository root.</summary>
    public const string WorkerFileName = "sandbox-update-worker.ps1";

    /// <summary>Where the worker lives, relative to the repository root.</summary>
    public const string WorkerScriptPath = "scripts/lib/" + WorkerFileName;

    /// <summary>The variable naming the older build both update gates start from.</summary>
    public const string BaseMsiVariable = "EXOSNAP_UPDATE_FROM_MSI";

    private static readonly string[] RequiredSteps =
        ["install-base", "decline-offer", "decline-apply", "decline-state"];

    private readonly Func<string?> readBaseMsi;

    /// <summary>Creates the gate reading the real process environment.</summary>
    public UpdateDeclineGate()
        : this(() => Environment.GetEnvironmentVariable(BaseMsiVariable))
    {
    }

    /// <summary>Creates the gate with an injected environment reader.</summary>
    public UpdateDeclineGate(Func<string?> readBaseMsi)
    {
        ArgumentNullException.ThrowIfNull(readBaseMsi);
        this.readBaseMsi = readBaseMsi;
    }

    /// <inheritdoc/>
    public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken) =>
        DisposableOsUpdateRun.ExecuteAsync(context, this.readBaseMsi(), RequiredSteps, cancellationToken);
}

/// <summary>REL-UPD-MSI-001: the MSI update elevates, installs and relaunches.</summary>
/// <remarks>
/// Runs the same worker script as <see cref="UpdateDeclineGate"/>, which performs
/// the decline and accept halves back to back because the accept half needs the
/// exact older-build starting point the decline half installed. This gate reads
/// only the accept-specific steps out of the result, so a failure in the decline
/// half is that gate's verdict and not this one's.
/// </remarks>
public sealed class UpdateAcceptGate : IScenarioBody
{
    private static readonly string[] RequiredSteps =
        ["install-base", "updater-gone-before-accept", "accept-offer", "accept-apply", "accept-installed"];

    private readonly Func<string?> readBaseMsi;

    /// <summary>Creates the gate reading the real process environment.</summary>
    public UpdateAcceptGate()
        : this(() => Environment.GetEnvironmentVariable(UpdateDeclineGate.BaseMsiVariable))
    {
    }

    /// <summary>Creates the gate with an injected environment reader.</summary>
    public UpdateAcceptGate(Func<string?> readBaseMsi)
    {
        ArgumentNullException.ThrowIfNull(readBaseMsi);
        this.readBaseMsi = readBaseMsi;
    }

    /// <inheritdoc/>
    public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken) =>
        DisposableOsUpdateRun.ExecuteAsync(context, this.readBaseMsi(), RequiredSteps, cancellationToken);
}

/// <summary>
/// REL-PKG-CHOCO-001: the Chocolatey package installs, uninstalls and leaves the
/// machine as it was.
/// </summary>
/// <remarks>
/// The only gate that installs software from a package manager, so it runs on a
/// disposable machine rather than the real one. The release MSI is installed first
/// and reinstalled by the worker's own finally block whatever happens, because the
/// rehearsal asserts that the package's uninstall leaves the user's configuration
/// directory alone and that directory has to have content before "unchanged" means
/// anything. The Visual C++ redistributable a Chocolatey dependency may upgrade is
/// deliberately not rolled back - downgrading a runtime is worse than the change -
/// and its version before and after is recorded in the worker's evidence.
/// </remarks>
public sealed class ChocolateyRehearsalGate : IScenarioBody
{
    /// <summary>The guest worker script, staged from the repository root.</summary>
    public const string WorkerFileName = "sandbox-choco-worker.ps1";

    /// <summary>Where the worker lives, relative to the repository root.</summary>
    public const string WorkerScriptPath = "scripts/lib/" + WorkerFileName;

    /// <summary>Where the tracked package sources live, relative to the repository root.</summary>
    public const string PackageSourcePath = "packaging/chocolatey";

    private static readonly string[] RequiredSteps =
        ["prepare", "pack", "removeExisting", "install", "uninstall", "restore"];

    private readonly Func<string, ReleaseMsiLookup> locateMsi;

    /// <summary>Creates the gate identifying the installer on the real machine.</summary>
    public ChocolateyRehearsalGate()
        : this(executablePath => ReleaseMsiArtifact.Locate(executablePath, Environment.GetEnvironmentVariable))
    {
    }

    /// <summary>Creates the gate with an injected installer lookup.</summary>
    public ChocolateyRehearsalGate(Func<string, ReleaseMsiLookup> locateMsi)
    {
        ArgumentNullException.ThrowIfNull(locateMsi);
        this.locateMsi = locateMsi;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var packageSource = Path.Combine(services.Artifact.RepositoryRoot, "packaging", "chocolatey");
        if (!Directory.Exists(packageSource))
        {
            return ScenarioResult.Unavailable($"{PackageSourcePath} is missing");
        }

        var worker = Path.Combine(services.Artifact.RepositoryRoot, WorkerScriptPath);
        if (!File.Exists(worker))
        {
            return ScenarioResult.Unavailable($"{WorkerScriptPath} is missing");
        }

        var msi = this.locateMsi(services.Artifact.ExecutablePath);
        if (msi.Path is null)
        {
            return ScenarioResult.Unavailable(msi.Detail);
        }

        // The package source stays a directory: the worker reads
        // tools/chocolateyinstall.ps1 underneath it, so flattening it into the
        // staging directory would lose the only structure it depends on.
        //
        // The scripts are derived, not listed: this worker invokes
        // choco-rehearsal-worker.ps1, which dot-sources ReleaseScenarios.ps1, and a
        // hand-written list is how both went missing. WorkerPayload reads them out
        // of the scripts, so the next relative import someone adds is staged
        // without anyone remembering to.
        var staged = new List<string>(WorkerPayload.StagingPathsFor(worker, WorkerPayload.ReadFileOrNull))
        {
            msi.Path,
            packageSource,
        };
        var request = new DisposableOsWorkerRequest(
            WorkerFileName,
            staged,
            [
                "-PackageSource", Path.GetFileName(packageSource),
                "-MsiPath", Path.GetFileName(msi.Path),
                "-MsiSha256", msi.Sha256,
            ])
        {
            EvidenceDirectory = context.EvidenceDirectory,
            // The rehearsal installs from the local package but Chocolatey itself is
            // bootstrapped from chocolatey.org, so this run cannot be offline.
            RequiresNetwork = true,
        };
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => DisposableOsUpdateRun.ToScenarioResult(run, RequiredSteps),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }
}

/// <summary>REL-INSTALL-CLEAN-001: a first start on a machine with nothing of ExoSnap on it.</summary>
/// <remarks>
/// Clean is a state, not a screen. A machine carrying an earlier install, an earlier
/// configuration, a recovery manifest, update state or a per-user registry key does
/// something different on a first start -- so the worker establishes the state before
/// it measures anything, and a machine that was not clean is reported as the harness
/// failing to set the test up.
///
/// The upgrade direction is not repeated here: REL-UPD-MSI-001 already installs an
/// older release and updates it to the bound candidate. And the visual first-run
/// surfaces belong to the visual scenarios; pressing them in would make a state test
/// fail for a pixel.
/// </remarks>
public sealed class CleanFirstStartGate : IScenarioBody
{
    /// <summary>The guest worker script, staged from the repository root.</summary>
    public const string WorkerFileName = "clean-first-start-worker.ps1";

    /// <summary>Where the worker lives, relative to the repository root.</summary>
    public const string WorkerScriptPath = "scripts/lib/" + WorkerFileName;

    /// <summary>The steps that assert something about the started application.</summary>
    public static readonly string[] ProductSteps =
    [
        "first-start-identity",
        "first-start-no-recovery",
        "first-start-defaults",
        "first-start-shutdown",
        "second-start-healthy",
    ];

    private readonly Func<string, ReleaseMsiLookup> locateMsi;

    /// <summary>Creates the gate resolving the candidate MSI beside the bound executable.</summary>
    public CleanFirstStartGate()
        : this(executablePath => ReleaseMsiArtifact.Locate(executablePath, Environment.GetEnvironmentVariable))
    {
    }

    /// <summary>Creates the gate with an injected MSI lookup.</summary>
    public CleanFirstStartGate(Func<string, ReleaseMsiLookup> locateMsi)
    {
        ArgumentNullException.ThrowIfNull(locateMsi);
        this.locateMsi = locateMsi;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var worker = Path.Combine(services.Artifact.RepositoryRoot, WorkerScriptPath);
        if (!File.Exists(worker))
        {
            return ScenarioResult.Unavailable($"{WorkerScriptPath} is missing");
        }

        // Before the guest is started: without a candidate identity there is nothing
        // to compare the started application against.
        if (services.Artifact.DescribeIncompleteCandidateIdentity() is { Length: > 0 } incomplete)
        {
            return ScenarioResult.InfrastructureError(incomplete);
        }

        var msi = this.locateMsi(services.Artifact.ExecutablePath);
        if (msi.Path is null)
        {
            return ScenarioResult.Unavailable(msi.Detail);
        }

        var staged = new List<string>(WorkerPayload.StagingPathsFor(worker, WorkerPayload.ReadFileOrNull))
        {
            msi.Path,
        };
        var request = new DisposableOsWorkerRequest(
            WorkerFileName,
            staged,
            [
                "-MsiPath", Path.GetFileName(msi.Path),
                "-ExpectedVersion", services.Artifact.ProductVersion,
            ])
        {
            EvidenceDirectory = context.EvidenceDirectory,
            // Nothing here downloads anything. An offline machine is also the honest
            // starting point: a first start that quietly reached an update feed is a
            // different measurement.
            RequiresNetwork = false,
        };

        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => DisposableOsUpdateRun.ToScenarioResult(run, ProductSteps),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }
}

/// <summary>What the two update gates do identically apart from which steps they require.</summary>
internal static class DisposableOsUpdateRun
{
    internal static async Task<ScenarioResult> ExecuteAsync(
        ScenarioContext context,
        string? baseMsi,
        IReadOnlyList<string> requiredSteps,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (string.IsNullOrWhiteSpace(baseMsi) || !File.Exists(baseMsi))
        {
            return ScenarioResult.Unavailable(
                $"set {UpdateDeclineGate.BaseMsiVariable} to an older official ExoSnap MSI; the update gates need a starting point older than the bound artifact");
        }

        var worker = Path.Combine(services.Artifact.RepositoryRoot, UpdateDeclineGate.WorkerScriptPath);
        if (!File.Exists(worker))
        {
            return ScenarioResult.Unavailable($"{UpdateDeclineGate.WorkerScriptPath} is missing");
        }

        // Before the guest is started: a binding that cannot identify a candidate,
        // or whose parts came from different ones, has nothing to compare the
        // installed build against. Discovered here it is an infrastructure error;
        // discovered at the end of the run it would read as a product failure.
        if (services.Artifact.DescribeIncompleteCandidateIdentity() is { Length: > 0 } incomplete)
        {
            return ScenarioResult.InfrastructureError(incomplete);
        }

        // The MSI goes in by its staged leaf name: the transport owns the translation
        // into whatever path its guest sees.
        //
        // The scripts are derived (WorkerPayload): this worker imports
        // LiveVerifyClient.psm1, which was not staged -- the run reached the import
        // and failed with PowerShell's module error, which reads nothing like an
        // incomplete payload.
        var staged = new List<string>(WorkerPayload.StagingPathsFor(worker, WorkerPayload.ReadFileOrNull))
        {
            baseMsi,
        };

        // Evidence was only requested by the Chocolatey gate, so an update run that
        // failed left its MSI logs and updater state inside a machine that was then
        // discarded -- the one run whose logs were worth having.
        // The candidate's identity, so the worker can assert that the update
        // installed THIS build. "the version changed" used to be the whole proof,
        // which any newer release satisfies -- including another RC of the same
        // base version that nobody verified.
        var request = new DisposableOsWorkerRequest(
            UpdateDeclineGate.WorkerFileName,
            staged,
            [
                "-BaseMsiPath", Path.GetFileName(baseMsi),
                "-ExpectedVersion", services.Artifact.ProductVersion,
                "-ExpectedCommit", services.Artifact.SourceCommit,
                "-ExpectedExeSha256", services.Artifact.ExecutableSha256,
            ])
        {
            EvidenceDirectory = context.EvidenceDirectory,
            // The update path is the thing under test: the guest has to reach the
            // release feed to be offered anything at all.
            RequiresNetwork = true,
        };
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => ToScenarioResult(run, requiredSteps),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }

    /// <summary>
    /// A worker that never reached a required step measured nothing, so it is an
    /// infrastructure error rather than a product verdict. Only a step that ran and
    /// reported not ok says ExoSnap is wrong.
    /// </summary>
    /// <summary>
    /// The scenario result for one disposable-OS run, carrying what the transport
    /// said about the evidence as well as what the worker said about the product.
    /// </summary>
    /// <remarks>
    /// The transport has been reporting an evidence outcome since the collection
    /// stopped swallowing failures, and nothing read it: a run whose logs were
    /// destroyed reached the record as an ordinary pass. The two facts stay separate
    /// here and are joined once, in the promotion contract.
    /// </remarks>
    internal static ScenarioResult ToScenarioResult(DisposableOsRun run, IReadOnlyList<string> requiredSteps)
    {
        ArgumentNullException.ThrowIfNull(run);
        var result = ToScenarioResult(DisposableOsVerdict.From(run.Result, requiredSteps));
        return run.Evidence.IsComplete ? result : result with { EvidenceGap = run.Evidence.Detail };
    }

    internal static ScenarioResult ToScenarioResult(DisposableOsVerdict verdict) => verdict.Kind switch
    {
        DisposableOsVerdictKind.Pass => ScenarioResult.Pass(verdict.Message),
        DisposableOsVerdictKind.Fail => ScenarioResult.Fail(verdict.Message),
        DisposableOsVerdictKind.Unverified => ScenarioResult.InfrastructureError(verdict.Message),
        _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS verdict kind {verdict.Kind}"),
    };
}
