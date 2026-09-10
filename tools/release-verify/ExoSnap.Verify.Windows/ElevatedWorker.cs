namespace ExoSnap.Verify.Windows;

/// <summary>
/// The command-line contract between an unelevated verifier and the elevated work a
/// few scenarios need.
/// </summary>
/// <remarks>
/// User Interface Privilege Isolation forbids a standard-integrity process from
/// inspecting or driving elevated UI, and a harness that tried would either fail
/// silently or report a verdict it never observed. So the arrangement is inverted:
/// the elevated half runs as its own process (<c>ExoSnap.Verify.Worker.exe</c>, which
/// carries a <c>requireAdministrator</c> manifest) and the only thing that crosses
/// back is a result document at a path the parent chose. The parent never reads the
/// worker's console or its windows, and the Secure Desktop elevation prompt is
/// answered by a person, never scripted.
///
/// Everything the worker needs travels on the argument list, not the environment: a
/// process started with <c>runas</c> cannot be handed a customised environment
/// block, so that channel does not exist for this launch.
/// </remarks>
public static class ElevatedWorker
{
    /// <summary>The command-line verb the worker executable answers to.</summary>
    public const string Verb = "elevated-worker";

    /// <summary>
    /// The option carrying the path the worker must write its result document to.
    /// Nothing is read back from the worker's standard output: a process the parent
    /// may not inspect is a process whose console it may not depend on either.
    /// </summary>
    public const string ResultPathOption = "--result";

    /// <summary>The option carrying the application under test the worker drives.</summary>
    public const string TargetOption = "--target";

    /// <summary>
    /// Runs the worker without its elevation assertion and writes a canned result,
    /// so the parent-to-worker file boundary can be exercised without a UAC prompt.
    /// </summary>
    public const string SelfTestOption = "--self-test";

    /// <summary>Builds the argument list the parent starts the worker with.</summary>
    /// <param name="taskId">The scenario id the worker is to carry out.</param>
    /// <param name="resultPath">Absolute path the worker writes its result to.</param>
    /// <param name="targetExe">The application under test the worker drives, or an empty string.</param>
    /// <param name="selfTest">Whether to ask for the no-elevation self-test path.</param>
    public static IReadOnlyList<string> BuildArguments(
        string taskId,
        string resultPath,
        string targetExe,
        bool selfTest = false)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(taskId);
        ArgumentException.ThrowIfNullOrWhiteSpace(resultPath);

        var arguments = new List<string> { Verb, taskId, ResultPathOption, resultPath };
        if (!string.IsNullOrWhiteSpace(targetExe))
        {
            arguments.Add(TargetOption);
            arguments.Add(targetExe);
        }

        if (selfTest)
        {
            arguments.Add(SelfTestOption);
        }

        return arguments;
    }

    /// <summary>
    /// Parses the worker's own argument list.
    /// </summary>
    /// <returns>The parsed request, or null with <paramref name="error"/> set.</returns>
    public static ElevatedWorkerRequest? TryParse(IReadOnlyList<string> arguments, out string error)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        error = string.Empty;

        if (arguments.Count < 4 || !string.Equals(arguments[0], Verb, StringComparison.Ordinal))
        {
            error = $"expected '{Verb} <task-id> {ResultPathOption} <path> [{TargetOption} <exe>] [{SelfTestOption}]'";
            return null;
        }

        var taskId = arguments[1];
        string? resultPath = null;
        var targetExe = string.Empty;
        var selfTest = false;

        for (var index = 2; index < arguments.Count; index++)
        {
            switch (arguments[index])
            {
                case ResultPathOption when index + 1 < arguments.Count:
                    resultPath = arguments[++index];
                    break;
                case TargetOption when index + 1 < arguments.Count:
                    targetExe = arguments[++index];
                    break;
                case SelfTestOption:
                    selfTest = true;
                    break;
                default:
                    error = $"unexpected argument '{arguments[index]}'";
                    return null;
            }
        }

        if (string.IsNullOrWhiteSpace(taskId) || string.IsNullOrWhiteSpace(resultPath))
        {
            error = $"both a task id and {ResultPathOption} <path> are required";
            return null;
        }

        return new ElevatedWorkerRequest(taskId, resultPath, targetExe, selfTest);
    }
}

/// <summary>What the parent asked the elevated worker to do.</summary>
/// <param name="TaskId">The scenario id.</param>
/// <param name="ResultPath">Where the worker writes its result document.</param>
/// <param name="TargetExe">The application under test to drive, or an empty string.</param>
/// <param name="SelfTest">Whether the elevation assertion is skipped and a canned result written.</param>
public sealed record ElevatedWorkerRequest(string TaskId, string ResultPath, string TargetExe, bool SelfTest);
