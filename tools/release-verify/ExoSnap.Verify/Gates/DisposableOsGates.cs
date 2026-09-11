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
        var request = new DisposableOsWorkerRequest(
            WorkerFileName,
            [worker, msi.Path, packageSource],
            [
                "-PackageSource", Path.GetFileName(packageSource),
                "-MsiPath", Path.GetFileName(msi.Path),
                "-MsiSha256", msi.Sha256,
            ])
        {
            EvidenceDirectory = context.EvidenceDirectory,
        };
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => DisposableOsUpdateRun.ToScenarioResult(
                DisposableOsVerdict.From(run.Result, RequiredSteps)),
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

        // The MSI goes in by its staged leaf name: the transport owns the translation
        // into whatever path its guest sees.
        var request = new DisposableOsWorkerRequest(
            UpdateDeclineGate.WorkerFileName,
            [worker, baseMsi],
            ["-BaseMsiPath", Path.GetFileName(baseMsi)]);
        var run = await services.DisposableOs.RunAsync(request, cancellationToken).ConfigureAwait(false);

        return run.Kind switch
        {
            DisposableOsRunKind.Unavailable => ScenarioResult.Unavailable(run.Detail),
            DisposableOsRunKind.Faulted => ScenarioResult.InfrastructureError(run.Detail),
            DisposableOsRunKind.Completed => ToScenarioResult(DisposableOsVerdict.From(run.Result, requiredSteps)),
            _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS run kind {run.Kind}"),
        };
    }

    /// <summary>
    /// A worker that never reached a required step measured nothing, so it is an
    /// infrastructure error rather than a product verdict. Only a step that ran and
    /// reported not ok says ExoSnap is wrong.
    /// </summary>
    internal static ScenarioResult ToScenarioResult(DisposableOsVerdict verdict) => verdict.Kind switch
    {
        DisposableOsVerdictKind.Pass => ScenarioResult.Pass(verdict.Message),
        DisposableOsVerdictKind.Fail => ScenarioResult.Fail(verdict.Message),
        DisposableOsVerdictKind.Unverified => ScenarioResult.InfrastructureError(verdict.Message),
        _ => ScenarioResult.InfrastructureError($"unrecognized disposable-OS verdict kind {verdict.Kind}"),
    };
}
