using System.Globalization;

namespace ExoSnap.Verify.Windows;

/// <summary>
/// The boundary between an unelevated verifier and the elevated work a few
/// scenarios need.
/// </summary>
/// <remarks>
/// User Interface Privilege Isolation forbids a standard-integrity process from
/// inspecting or driving elevated UI, and a harness that tried would either fail
/// silently or report a verdict it never observed. So the arrangement is
/// inverted: the elevated half runs as its own process and the only thing that
/// crosses back is a result document at a path the parent chose. The parent never
/// reads the elevated process's windows, and the Secure Desktop is never
/// automated at all.
///
/// This is the contract, not the implementation. The elevated worker executable
/// arrives with the scenarios that need it; until then every entry point here
/// refuses rather than pretending to have done the work.
/// </remarks>
public static class ElevatedWorker
{
    /// <summary>The command-line verb the worker executable answers to.</summary>
    public const string Verb = "elevated-worker";

    /// <summary>
    /// The single argument the parent passes: the path the worker must write its
    /// result document to. Nothing is read back from the worker's standard
    /// output, because a process the parent may not inspect is a process whose
    /// console it may not depend on either.
    /// </summary>
    public const string ResultPathOption = "--result";

    /// <summary>
    /// Builds the argument list the parent starts the worker with.
    /// </summary>
    /// <param name="taskId">The scenario id the worker is to carry out.</param>
    /// <param name="resultPath">Absolute path the worker writes its result to.</param>
    public static IReadOnlyList<string> BuildArguments(string taskId, string resultPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(taskId);
        ArgumentException.ThrowIfNullOrWhiteSpace(resultPath);
        return [Verb, taskId, ResultPathOption, resultPath];
    }

    /// <summary>
    /// Entry point of the elevated half. Not implemented in this revision: the
    /// scenarios that need it have not been migrated, and an entry point that
    /// returned success without doing anything would be indistinguishable from
    /// one that had.
    /// </summary>
    /// <exception cref="NotSupportedException">Always.</exception>
    public static int Run(IReadOnlyList<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        throw new NotSupportedException(string.Format(
            CultureInfo.InvariantCulture,
            "The elevated worker is not implemented yet; '{0}' has no elevated half to run.",
            arguments.Count > 1 ? arguments[1] : "(no task)"));
    }
}
