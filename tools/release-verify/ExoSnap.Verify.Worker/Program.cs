using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Worker;

/// <summary>
/// The elevated half of the release harness.
/// </summary>
/// <remarks>
/// The parent starts this without a prompt. When there is real elevated work to do
/// and this process is not elevated, it relaunches itself with <c>runas</c> - that
/// relaunch is where the one UAC prompt a person answers appears. It talks to its
/// parent through exactly one channel: the result document it writes to the path it
/// was given. It reads nothing back, and it always leaves a result behind - even on
/// a declined prompt or an unhandled fault - so the parent is never left waiting on
/// a file that will not arrive.
/// </remarks>
public static class Program
{
    private const int ExitOk = 0;
    private const int ExitUsage = 2;
    private const int ExitInfrastructure = 3;

    // ShellExecute reports a declined UAC prompt as this Win32 error.
    private const int ErrorCancelled = 1223;

    /// <summary>Entry point.</summary>
    public static int Main(string[] args)
    {
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;

        var request = ElevatedWorker.TryParse(args, out var error);
        if (request is null)
        {
            Console.Error.WriteLine(error);
            return ExitUsage;
        }

        if (!request.SelfTest && ProcessPrivilege.IsElevated() != true)
        {
            return RelaunchElevated(request, args);
        }

        ElevatedWorkerResult result;
        try
        {
            result = request.SelfTest
                ? SelfTest(request.TaskId)
                : PresentDiagnosticsTask.RunAsync(request, CancellationToken.None).GetAwaiter().GetResult();
        }
#pragma warning disable CA1031 // The worker must always leave a result behind; an escaped fault becomes an infrastructure verdict, never a crash.
        catch (Exception exception)
#pragma warning restore CA1031
        {
            result = ElevatedWorkerResult.For(
                request.TaskId,
                ElevatedWorkerOutcome.InfrastructureError,
                $"the worker raised {exception.GetType().Name}: {exception.Message}");
        }

        result.Write(request.ResultPath);
        return result.Outcome == ElevatedWorkerOutcome.InfrastructureError ? ExitInfrastructure : ExitOk;
    }

    // The one place a UAC prompt is raised. A declined prompt is the operator's
    // choice, not a fault: it is recorded as a Deferred result so the parent can
    // tell it apart from a worker that never ran.
    private static int RelaunchElevated(ElevatedWorkerRequest request, string[] args)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = Environment.ProcessPath ?? throw new InvalidOperationException("no process path to relaunch"),
            UseShellExecute = true,
            Verb = "runas",
        };
        foreach (var argument in args)
        {
            startInfo.ArgumentList.Add(argument);
        }

        try
        {
            using var elevated = Process.Start(startInfo) ??
                throw new InvalidOperationException("Process.Start returned no elevated worker");
            elevated.WaitForExit();
            return elevated.ExitCode;
        }
        catch (Win32Exception exception) when (exception.NativeErrorCode == ErrorCancelled)
        {
            ElevatedWorkerResult.For(
                request.TaskId,
                ElevatedWorkerOutcome.Deferred,
                "the operator declined the elevation prompt; the elevated observation was not taken")
                .Write(request.ResultPath);
            return ExitOk;
        }
        catch (Exception exception) when (exception is Win32Exception or InvalidOperationException)
        {
            ElevatedWorkerResult.For(
                request.TaskId,
                ElevatedWorkerOutcome.InfrastructureError,
                $"the elevated relaunch could not be started: {exception.Message}")
                .Write(request.ResultPath);
            return ExitInfrastructure;
        }
    }

    // Proves the parent-to-worker file boundary without a UAC prompt: the worker
    // reached its entry point, and its result round-trips through the same reader
    // the real path uses. It reports the real elevation state so a caller can see
    // whether it happened to be elevated anyway.
    private static ElevatedWorkerResult SelfTest(string taskId) =>
        ElevatedWorkerResult.For(
            taskId,
            ElevatedWorkerOutcome.Pass,
            "self-test: the worker started and the result-file boundary works",
            elevated: ProcessPrivilege.IsElevated() == true,
            presentMode: "selfTest");
}
