using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.LiveVerify;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Worker;

/// <summary>
/// REL-PRESENT-002's elevated half: an elevated ExoSnap, in-depth present
/// diagnostics on, decoding real presents while the monitor records.
/// </summary>
/// <remarks>
/// PresentMon needs a real-time ETW session, which Windows grants only to an
/// elevated process. This runs inside that process. The independent PresentMon
/// cross-check is a different gate (REL-PRESENT-XCHECK-001) and not repeated here.
/// </remarks>
internal static class PresentDiagnosticsTask
{
    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan PresentDeadline = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan SamplePause = TimeSpan.FromMilliseconds(500);

    private const string OracleNote =
        "an independent PresentMon read is REL-PRESENT-XCHECK-001, deliberately not repeated here";

    public static async Task<ElevatedWorkerResult> RunAsync(
        ElevatedWorkerRequest request,
        CancellationToken cancellationToken)
    {
        if (ProcessPrivilege.IsElevated() != true)
        {
            return ElevatedWorkerResult.For(
                request.TaskId,
                ElevatedWorkerOutcome.InfrastructureError,
                "the worker is not running with an elevated token, so it cannot open a real-time ETW session");
        }

        if (string.IsNullOrWhiteSpace(request.TargetExe) || !File.Exists(request.TargetExe))
        {
            return ElevatedWorkerResult.For(
                request.TaskId,
                ElevatedWorkerOutcome.InfrastructureError,
                $"the application under test was not found at '{request.TargetExe}'",
                elevated: true);
        }

        ILiveVerifySession session;
        try
        {
            session = await new LiveVerifySessionFactory()
                .LaunchAsync(request.TargetExe, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (LiveVerifyException exception)
        {
            return ElevatedWorkerResult.For(
                request.TaskId,
                ElevatedWorkerOutcome.InfrastructureError,
                $"the elevated application did not come up on its control channel: {exception.Message}",
                elevated: true);
        }

        await using (session.ConfigureAwait(false))
        {
            var enabled = await session.InvokeAsync(
                    "diagnostics.setInDepth",
                    new Dictionary<string, object?>(StringComparer.Ordinal) { ["enabled"] = true },
                    cancellationToken)
                .ConfigureAwait(false);
            if (!enabled.Ok)
            {
                return ElevatedWorkerResult.For(
                    request.TaskId,
                    ElevatedWorkerOutcome.InfrastructureError,
                    $"diagnostics.setInDepth was refused on the elevated session: {enabled.Refusal}",
                    elevated: true);
            }

            await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
            await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
            await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
                .ConfigureAwait(false);

            // Bounded, state-based wait: the ETW session needs a present to decode, and
            // presents arrive as the desktop draws. Polling the snapshot is the
            // measurement, not a guess that enough time has passed.
            var present = default(JsonElement);
            var deadline = DateTime.UtcNow.Add(PresentDeadline);
            while (DateTime.UtcNow < deadline)
            {
                var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
                present = Snapshots.Value(environment.Result, "present").Clone();
                if (Snapshots.IsTrue(present, "available") && (Snapshots.Number(present, "presentCount") ?? 0) > 0)
                {
                    break;
                }

                await Task.Delay(SamplePause, cancellationToken).ConfigureAwait(false);
            }

            await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
            await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
                .ConfigureAwait(false);

            return Judge(request.TaskId, present);
        }
    }

    private static ElevatedWorkerResult Judge(string taskId, JsonElement present)
    {
        var count = (long)(Snapshots.Number(present, "presentCount") ?? 0);
        var mode = Snapshots.Text(present, "mode");
        bool? tearing = Snapshots.IsTrue(present, "tearingAllowed")
            ? true
            : Snapshots.IsFalse(present, "tearingAllowed") ? false : null;

        if (!Snapshots.IsTrue(present, "elevated"))
        {
            return ElevatedWorkerResult.For(
                taskId,
                ElevatedWorkerOutcome.Fail,
                "the elevated application still reports present.elevated false",
                elevated: true,
                presentCount: count,
                presentMode: mode,
                tearingAllowed: tearing,
                oracleNote: OracleNote);
        }

        if (!Snapshots.IsTrue(present, "available"))
        {
            return ElevatedWorkerResult.For(
                taskId,
                ElevatedWorkerOutcome.Fail,
                "present diagnostics stayed unavailable under elevation: " +
                $"{Snapshots.Text(present, "availability")} / {Snapshots.Text(present, "reason")}",
                elevated: true,
                oracleNote: OracleNote);
        }

        if (count <= 0)
        {
            return ElevatedWorkerResult.For(
                taskId,
                ElevatedWorkerOutcome.Fail,
                "the elevated session opened but decoded no presents while the monitor was recording",
                elevated: true,
                oracleNote: OracleNote);
        }

        if (string.IsNullOrWhiteSpace(mode) || string.Equals(mode, "unavailable", StringComparison.Ordinal))
        {
            return ElevatedWorkerResult.For(
                taskId,
                ElevatedWorkerOutcome.Fail,
                $"{count} present(s) were decoded but no presentation mode was classified",
                elevated: true,
                presentCount: count,
                oracleNote: OracleNote);
        }

        return ElevatedWorkerResult.For(
            taskId,
            ElevatedWorkerOutcome.Pass,
            $"elevated present diagnostics decoded {count} present(s), mode {mode}",
            elevated: true,
            presentCount: count,
            presentMode: mode,
            tearingAllowed: tearing,
            oracleNote: OracleNote);
    }
}
