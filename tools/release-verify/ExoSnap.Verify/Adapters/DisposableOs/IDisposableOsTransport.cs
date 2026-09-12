namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>
/// One worker script to run on a disposable machine: the script itself, the files
/// it needs staged alongside it, and the arguments specific to what it is
/// asserting. A transport appends its own trailing arguments (a staging directory,
/// a result path, a marker path, translated into whatever that transport's guest
/// sees). A gate body never computes a guest path itself.
/// </summary>
/// <param name="WorkerFileName">The script's file name; must be one of <paramref name="SourceFiles"/>.</param>
/// <param name="SourceFiles">
/// Host paths to copy into the staging directory: the worker script and every file
/// it reads (a base MSI, a nuspec, the release package). Never mapped from the
/// working tree directly: a worker that could write into the source it copied
/// from is a worker that can change the thing it is verifying.
/// </param>
/// <param name="WorkerArguments">
/// The worker's own named parameters this gate is responsible for (for example
/// <c>-BaseMsiPath</c>, <c>-UpdateChannel</c>), as raw tokens. The transport
/// appends its own staging/result/marker parameters after these.
/// </param>
public sealed record DisposableOsWorkerRequest(
    string WorkerFileName,
    IReadOnlyList<string> SourceFiles,
    IReadOnlyList<string> WorkerArguments)
{
    /// <summary>How long a run may take before it is reported faulted.</summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(30);

    /// <summary>
    /// Where the worker's evidence is copied back to on the host, or null when the
    /// worker produces none. Set it only for a worker that declares an
    /// <c>-EvidenceDirectory</c> parameter: the transport creates that directory
    /// inside the guest, passes it, and copies what the worker wrote back here
    /// before the disposable machine and its staging are discarded.
    /// </summary>
    public string? EvidenceDirectory { get; init; }
}

/// <summary>How one disposable-OS worker run ended.</summary>
public enum DisposableOsRunKind
{
    /// <summary>The worker ran and wrote a result document.</summary>
    Completed,

    /// <summary>The worker could not be run or wrote no result. Never a product verdict.</summary>
    Faulted,

    /// <summary>No transport could carry this run out on this machine.</summary>
    Unavailable,
}

/// <summary>What one disposable-OS worker run produced.</summary>
/// <param name="Kind">How it ended.</param>
/// <param name="Detail">One sentence about how it ended.</param>
/// <param name="Result">The worker's result document, present only when <see cref="Kind"/> is Completed.</param>
public sealed record DisposableOsRun(DisposableOsRunKind Kind, string Detail, DisposableOsRunResult? Result)
{
    /// <summary>
    /// How completely the worker's evidence reached the host, so a record can tell
    /// a product verdict apart from the question of whether it can be looked into.
    /// </summary>
    /// <remarks>
    /// A gate that is required for promotion and whose evidence could not be
    /// collected has not produced the evidence the promotion contract asks for, even
    /// when the product assertions passed. Carried separately for that reason rather
    /// than folded into the verdict: the two are different facts and only the caller
    /// knows whether its evidence is contractually required.
    /// </remarks>
    public EvidenceOutcome Evidence { get; init; } = EvidenceOutcome.NotRequested;

    /// <summary>The worker wrote this result.</summary>
    public static DisposableOsRun Completed(DisposableOsRunResult result) =>
        new(DisposableOsRunKind.Completed, "the worker finished and wrote a result document", result);

    /// <summary>The worker could not be run, or ran and wrote nothing.</summary>
    public static DisposableOsRun Faulted(string detail) => new(DisposableOsRunKind.Faulted, detail, null);

    /// <summary>No transport could carry this run out.</summary>
    public static DisposableOsRun Unavailable(string detail) => new(DisposableOsRunKind.Unavailable, detail, null);
}

/// <summary>One way to run a worker script on a disposable machine.</summary>
public interface IDisposableOsTransport
{
    /// <summary>The transport's name, for evidence and log messages ("sandbox", "vm").</summary>
    string Name { get; }

    /// <summary>Whether this transport is usable on this machine right now.</summary>
    bool Available { get; }

    /// <summary>Why this transport is not usable here, or an empty string when it is.</summary>
    string UnavailableReason { get; }

    /// <summary>Stages the request and runs it, returning what the worker produced.</summary>
    Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken);
}

/// <summary>Runs a worker through the first available transport.</summary>
public interface IDisposableOsRunner
{
    /// <summary>The transports this runner tries, in order.</summary>
    IReadOnlyList<string> TransportNames { get; }

    /// <summary>Runs through the first available transport, or reports Unavailable.</summary>
    Task<DisposableOsRun> RunAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken);
}

/// <summary>
/// Tries each transport in order and uses the first one that is available. Mirrors
/// the PowerShell fallback rule this replaces: a transport that cannot run reports
/// so through <see cref="IDisposableOsTransport.Available"/>, never by throwing, so
/// trying the next one is always safe.
/// </summary>
public sealed class DisposableOsRunner : IDisposableOsRunner
{
    private readonly IReadOnlyList<IDisposableOsTransport> transports;

    /// <summary>Creates a runner over transports, tried in the given order.</summary>
    public DisposableOsRunner(IReadOnlyList<IDisposableOsTransport> transports)
    {
        ArgumentNullException.ThrowIfNull(transports);
        this.transports = transports;
    }

    /// <inheritdoc/>
    public IReadOnlyList<string> TransportNames => [.. this.transports.Select(transport => transport.Name)];

    /// <inheritdoc/>
    public async Task<DisposableOsRun> RunAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        foreach (var transport in this.transports)
        {
            if (!transport.Available)
            {
                continue;
            }

            return await transport.RunWorkerAsync(request, cancellationToken).ConfigureAwait(false);
        }

        var reasons = this.transports.Count == 0
            ? "no transport is configured"
            : string.Join("; ", this.transports.Select(transport => $"{transport.Name}: {transport.UnavailableReason}"));
        return DisposableOsRun.Unavailable($"no disposable-OS transport is available on this machine ({reasons})");
    }
}

/// <summary>How completely a worker's evidence reached the host.</summary>
public enum EvidenceOutcomeState
{
    /// <summary>The request asked for none.</summary>
    NotRequested,

    /// <summary>Everything the worker wrote was copied back.</summary>
    Complete,

    /// <summary>Some of it was copied; the rest could not be.</summary>
    Partial,

    /// <summary>None of it could be copied, though the worker had written it.</summary>
    Failed,

    /// <summary>The worker declared an evidence directory and wrote none.</summary>
    Missing,
}

/// <summary>What collecting a worker's evidence produced.</summary>
/// <param name="State">How completely it reached the host.</param>
/// <param name="FilesCollected">How many files were copied back.</param>
/// <param name="FilesFailed">How many could not be.</param>
/// <param name="Detail">One sentence naming what was lost, when anything was.</param>
public sealed record EvidenceOutcome(
    EvidenceOutcomeState State,
    int FilesCollected,
    int FilesFailed,
    string Detail)
{
    /// <summary>The request asked for no evidence.</summary>
    public static EvidenceOutcome NotRequested { get; } =
        new(EvidenceOutcomeState.NotRequested, 0, 0, "no evidence was requested");

    /// <summary>
    /// True when the evidence is as complete as it was going to be. False means a
    /// caller that needs its evidence cannot treat this run as fully answered.
    /// </summary>
    public bool IsComplete =>
        this.State is EvidenceOutcomeState.NotRequested or EvidenceOutcomeState.Complete;
}