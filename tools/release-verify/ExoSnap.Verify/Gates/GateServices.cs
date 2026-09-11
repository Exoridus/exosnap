using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Adapters.Elevation;
using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Adapters.PresentMon;
using ExoSnap.Verify.Processes;
using ExoSnap.Verify.Windows;
using ExoSnap.Verify.Windows.Uia;

namespace ExoSnap.Verify.Gates;

/// <summary>The artifact a campaign is bound to.</summary>
/// <param name="ExecutablePath">The exosnap.exe under test.</param>
/// <param name="ProductVersion">The version those bytes report about themselves.</param>
/// <param name="RepositoryRoot">Where the helper scripts a few gates invoke live.</param>
public sealed record ArtifactUnderTest(string ExecutablePath, string ProductVersion, string RepositoryRoot);

/// <summary>
/// One ExoSnap process shared by every gate that needs one.
/// </summary>
/// <remarks>
/// Shared rather than one per gate, because the application enforces a
/// single-instance guard that a throwaway configuration directory does not lift: a
/// second launch hands focus to the running instance and exits without ever
/// constructing its control channel, so the runner then waits for an endpoint nobody
/// opened. A gate that needs an instance of its own therefore ends this one first.
/// </remarks>
public interface IGateSessionHost : IAsyncDisposable
{
    /// <summary>The running session, launching one if there is none.</summary>
    Task<ILiveVerifySession> EnsureAsync(CancellationToken cancellationToken);

    /// <summary>Ends the shared session so a gate can launch its own instance.</summary>
    Task EndAsync();
}

/// <summary>Everything a migrated gate is given beyond its own declaration.</summary>
/// <param name="Artifact">The bytes the verdicts describe.</param>
/// <param name="Sessions">The shared application session.</param>
/// <param name="Ffprobe">The independent oracle for a produced recording.</param>
/// <param name="Environment">The transactional environment orchestrator.</param>
/// <param name="Envctl">The environment tool, for the gates that only read it.</param>
/// <param name="PresentMon">The independent oracle for what a window did on screen.</param>
/// <param name="Processes">The runner every child process goes through.</param>
/// <param name="SessionFactory">
/// The launcher, for the gates that need an instance of their own rather than the
/// shared one.
/// </param>
/// <param name="Uia">The UI Automation reader the visual gates use past WDA_EXCLUDEFROMCAPTURE.</param>
/// <param name="SystemAppearance">The Windows apps-colour appearance, driven by the overlay gate.</param>
/// <param name="ElevatedWorker">
/// The boundary to elevated work: the parent never inspects elevated UI, only the
/// worker's result file.
/// </param>
/// <param name="DisposableOs">
/// Runs a worker script on a disposable machine. Which machine is a transport
/// decision, not a gate's: a gate states what it needs run and reads back a verdict.
/// </param>
/// <param name="LastPresentConfirmation">
/// What the previous present cross-check confirmed, or null when there has never been
/// one. Null makes that gate required, which is the correct default: nothing has been
/// confirmed yet.
/// </param>
/// <param name="PresentCapturePath">The PresentMon capture taken for this run, or null.</param>
public sealed record GateServices(
    ArtifactUnderTest Artifact,
    IGateSessionHost Sessions,
    IFfprobe Ffprobe,
    EnvironmentOrchestrator Environment,
    IEnvctl Envctl,
    IPresentMon PresentMon,
    ProcessRunner Processes,
    ILiveVerifySessionFactory SessionFactory,
    IUiAutomation Uia,
    ISystemAppearance SystemAppearance,
    IElevatedWorkerHost ElevatedWorker,
    IDisposableOsRunner DisposableOs,
    PresentConfirmation? LastPresentConfirmation = null,
    string? PresentCapturePath = null);

/// <summary>The shared session, launched on demand and ended when a gate asks.</summary>
public sealed class GateSessionHost : IGateSessionHost
{
    private readonly ILiveVerifySessionFactory factory;
    private readonly string executablePath;
    private ILiveVerifySession? session;

    /// <summary>Creates a host over a launcher and the artifact under test.</summary>
    public GateSessionHost(ILiveVerifySessionFactory factory, string executablePath)
    {
        ArgumentNullException.ThrowIfNull(factory);
        ArgumentException.ThrowIfNullOrWhiteSpace(executablePath);
        this.factory = factory;
        this.executablePath = executablePath;
    }

    /// <inheritdoc/>
    public async Task<ILiveVerifySession> EnsureAsync(CancellationToken cancellationToken) =>
        this.session ??= await this.factory.LaunchAsync(this.executablePath, cancellationToken).ConfigureAwait(false);

    /// <inheritdoc/>
    public async Task EndAsync()
    {
        if (this.session is null)
        {
            return;
        }

        var ending = this.session;
        this.session = null;
        await ending.DisposeAsync().ConfigureAwait(false);
    }

    /// <inheritdoc/>
    public ValueTask DisposeAsync() => new(this.EndAsync());
}
