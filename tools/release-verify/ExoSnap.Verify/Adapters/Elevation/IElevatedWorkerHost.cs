using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Adapters.Elevation;

/// <summary>How a request to the elevated worker ended.</summary>
/// <remarks>
/// A declined UAC prompt is not one of these: the worker records that itself, as a
/// <see cref="ExoSnap.Verify.Windows.ElevatedWorkerOutcome.Deferred"/> result, so it
/// arrives as a <see cref="Completed"/> run the caller reads like any other.
/// </remarks>
public enum ElevatedWorkerRunKind
{
    /// <summary>The worker ran and wrote a result document.</summary>
    Completed,

    /// <summary>The worker could not be run or did not write a result. Never a product verdict.</summary>
    Faulted,

    /// <summary>The worker executable could not be resolved on this machine.</summary>
    Unavailable,
}

/// <summary>What one run of the elevated worker produced.</summary>
/// <param name="Kind">How it ended.</param>
/// <param name="Detail">One sentence about how it ended.</param>
/// <param name="Result">The worker's result document, present only when <see cref="Kind"/> is Completed.</param>
public sealed record ElevatedWorkerRun(ElevatedWorkerRunKind Kind, string Detail, ElevatedWorkerResult? Result)
{
    /// <summary>The worker wrote this result.</summary>
    public static ElevatedWorkerRun Completed(ElevatedWorkerResult result) =>
        new(ElevatedWorkerRunKind.Completed, result.Message, result);

    /// <summary>The worker could not be run, or ran and wrote nothing.</summary>
    public static ElevatedWorkerRun Faulted(string detail) =>
        new(ElevatedWorkerRunKind.Faulted, detail, null);

    /// <summary>There is no worker executable to run.</summary>
    public static ElevatedWorkerRun Unavailable(string detail) =>
        new(ElevatedWorkerRunKind.Unavailable, detail, null);
}

/// <summary>
/// Runs work that needs an elevated token, across the process boundary UIPI forces.
/// </summary>
/// <remarks>
/// The implementation launches <c>ExoSnap.Verify.Worker.exe</c> - which elevates
/// itself through its manifest - and then does exactly one thing with it: wait for
/// the result file to appear at the path it was told to write. It never reads the
/// worker's console and never touches its windows or the elevated application's,
/// because a standard-integrity process that inspected elevated UI is the mistake
/// this boundary exists to make impossible.
/// </remarks>
public interface IElevatedWorkerHost
{
    /// <summary>Whether the worker executable is resolvable on this machine.</summary>
    bool Available { get; }

    /// <summary>Why the worker is not usable here, or an empty string when it is.</summary>
    string UnavailableReason { get; }

    /// <summary>
    /// Runs one elevated task and waits for its result document.
    /// </summary>
    /// <param name="taskId">The scenario id the worker carries out.</param>
    /// <param name="resultPath">Where the worker is told to write its result.</param>
    /// <param name="targetExe">The application under test the worker drives, or an empty string.</param>
    /// <param name="timeout">How long to wait for the result document after the worker starts.</param>
    /// <param name="selfTest">
    /// When true the worker is started without <c>runas</c> and skips its elevation
    /// assertion, so the file boundary can be exercised without a UAC prompt.
    /// </param>
    Task<ElevatedWorkerRun> RunAsync(
        string taskId,
        string resultPath,
        string targetExe,
        TimeSpan timeout,
        bool selfTest,
        CancellationToken cancellationToken);
}
