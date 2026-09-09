using System.Text.Json;

namespace ExoSnap.Verify.Adapters.LiveVerify;

/// <summary>The recording lifecycle states a gate waits for.</summary>
public static class RecordingStates
{
    /// <summary>A recording is running.</summary>
    public const string Recording = "Recording";

    /// <summary>A recording finished and produced a result.</summary>
    public const string Completed = "Completed";

    /// <summary>A recording ended without producing a usable result.</summary>
    public const string Failed = "Failed";

    /// <summary>The states a stop settles into, whichever way it went.</summary>
    public static readonly string[] Terminal = [Completed, Failed];
}

/// <summary>What asking a session to end produced.</summary>
/// <remarks>
/// Three outcomes, not two. "The application was asked to close and did not" is a
/// product defect; "there was no window to ask" is not, and a contract that returned
/// false for both would report the second as the first. Offscreen is exactly where
/// that happens: the platform plugin creates no native window, so there is nothing
/// for a close request to reach.
/// </remarks>
public enum SessionShutdown
{
    /// <summary>The application was asked to close and it exited.</summary>
    Exited,

    /// <summary>The application was asked to close and was still running at the deadline.</summary>
    StillRunning,

    /// <summary>There was no window to ask, so no close was ever requested.</summary>
    NotRequestable,
}

/// <summary>One answer from the control channel, with its refusal made readable.</summary>
/// <param name="Ok">Whether the command was accepted.</param>
/// <param name="Result">The result payload; the default element on a refusal.</param>
/// <param name="ErrorCode">Structured refusal cause, or an empty string.</param>
/// <param name="ErrorMessage">The refusal in words, or an empty string.</param>
public sealed record ControlAnswer(bool Ok, JsonElement Result, string ErrorCode, string ErrorMessage)
{
    /// <summary>The refusal as one sentence, for a verdict message.</summary>
    public string Refusal => this.ErrorMessage.Length > 0 ? this.ErrorMessage : this.ErrorCode;
}

/// <summary>
/// One running ExoSnap process, driven through its own semantic automation.
/// </summary>
/// <remarks>
/// Every product assertion goes through this surface rather than through a screen
/// reading: a gate that parses a label asserts about a string, and a gate that
/// screenshots a page to decide whether a recording worked asserts about pixels.
///
/// Waits are state-based and bounded, never fixed sleeps. A timeout returns what was
/// last observed rather than throwing, so a gate reports what it actually saw instead
/// of "timed out".
/// </remarks>
public interface ILiveVerifySession : IAsyncDisposable
{
    /// <summary>The run id this session was launched with; also the connection credential.</summary>
    string RunId { get; }

    /// <summary>Sends a command and returns the answer, refusal included.</summary>
    /// <exception cref="ExoSnap.Verify.LiveVerify.LiveVerifyException">
    /// No answer arrived within the deadline, or the connection is gone.
    /// </exception>
    Task<ControlAnswer> InvokeAsync(
        string command,
        IReadOnlyDictionary<string, object?>? parameters,
        CancellationToken cancellationToken);

    /// <summary>The newest state revision seen on any answer or event.</summary>
    long StateRevision { get; }

    /// <summary>
    /// How many events the client is holding that nobody has read.
    /// </summary>
    /// <remarks>
    /// Part of the surface because a shutdown gate asserts that unread events do not
    /// hold the application open, and a gate that could not say how many were unread
    /// would be asserting about a condition it never established.
    /// </remarks>
    int BufferedEventCount { get; }

    /// <summary>
    /// Waits until the recording lifecycle reaches one of <paramref name="states"/>,
    /// or the deadline passes, and returns the state last observed.
    /// </summary>
    Task<string> WaitForRecordingStateAsync(
        IReadOnlyCollection<string> states,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    /// <summary>
    /// Waits until the server's state revision advances past
    /// <paramref name="after"/>, and reports whether it did.
    /// </summary>
    Task<bool> WaitForRevisionAsync(long after, TimeSpan timeout, CancellationToken cancellationToken);

    /// <summary>
    /// Closes the control channel with whatever events are still buffered, asks the
    /// application to close, and reports what happened.
    /// </summary>
    /// <remarks>
    /// The exit is the assertion, not a courtesy: an application held open by events
    /// nobody read is the defect this contract exists to pin. The channel is closed
    /// first on purpose, because that is what a runner that stopped reading looks like
    /// from the server side.
    /// </remarks>
    Task<SessionShutdown> ShutdownAsync(TimeSpan timeout, CancellationToken cancellationToken);
}

/// <summary>Starts an ExoSnap process with a control channel of its own.</summary>
public interface ILiveVerifySessionFactory
{
    /// <summary>
    /// Launches the artifact, connects, and completes the handshake.
    /// </summary>
    /// <param name="executablePath">The exosnap.exe under test.</param>
    /// <param name="cancellationToken">Cancels the launch; a partially started process is still killed.</param>
    /// <exception cref="ExoSnap.Verify.LiveVerify.LiveVerifyException">
    /// The process did not open its endpoint, refused the handshake, or answered a
    /// different protocol. Always an infrastructure failure: nothing was measured.
    /// </exception>
    Task<ILiveVerifySession> LaunchAsync(string executablePath, CancellationToken cancellationToken);
}

/// <summary>Convenience wrappers for the commands the migrated gates use.</summary>
/// <remarks>
/// Extension methods rather than interface members so a fake implements one method
/// and inherits every typed command, and so a new gate can add a command without
/// every fake in the suite having to grow one.
/// </remarks>
public static class LiveVerifySessionCommands
{
    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(30);

    /// <summary>The product version and executable digest the running process reports about itself.</summary>
    public static Task<ControlAnswer> AppIdentityAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "app.identity", null, cancellationToken);

    /// <summary>The application's own view of itself: window, page, lifecycle.</summary>
    public static Task<ControlAnswer> AppSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "app.snapshot", null, cancellationToken);

    /// <summary>The machine as the product sees it: displays, present diagnostics, scaling.</summary>
    public static Task<ControlAnswer> EnvironmentSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "environment.snapshot", null, cancellationToken);

    /// <summary>Selects a capture target by kind, optionally filtered by window title.</summary>
    public static Task<ControlAnswer> SelectTargetAsync(
        this ILiveVerifySession session,
        string kind,
        string? titleFilter,
        CancellationToken cancellationToken)
    {
        var parameters = new Dictionary<string, object?>(StringComparer.Ordinal) { ["kind"] = kind };
        if (!string.IsNullOrWhiteSpace(titleFilter))
        {
            parameters["titleFilter"] = titleFilter;
        }

        return Invoke(session, "record.selectTarget", parameters, cancellationToken);
    }

    /// <summary>Starts a recording.</summary>
    public static Task<ControlAnswer> StartRecordingAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "record.start", null, cancellationToken);

    /// <summary>Stops a recording. The finalize continues after the answer arrives.</summary>
    public static Task<ControlAnswer> StopRecordingAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "record.stop", null, cancellationToken);

    /// <summary>Places a marker in the running recording.</summary>
    public static Task<ControlAnswer> AddMarkerAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "record.addMarker", null, cancellationToken);

    /// <summary>What the finished recording produced: whether it succeeded, and where it went.</summary>
    public static Task<ControlAnswer> RecordResultAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "record.result", null, cancellationToken);

    /// <summary>The record surface's own state: selected source, whether it is busy.</summary>
    public static Task<ControlAnswer> RecordSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "record.snapshot", null, cancellationToken);

    /// <summary>The last session report, as an envelope carrying availability and the report.</summary>
    public static Task<ControlAnswer> SessionLatestAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "session.latest", null, cancellationToken);

    /// <summary>The live pipeline measurements. The groups are absent while <c>valid</c> is false.</summary>
    public static Task<ControlAnswer> PipelineSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "pipeline.snapshot", null, cancellationToken);

    /// <summary>The preview's own bookkeeping, including the repaint debt.</summary>
    public static Task<ControlAnswer> PreviewSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "preview.snapshot", null, cancellationToken);

    /// <summary>Every window the application owns, with its role and native geometry.</summary>
    public static Task<ControlAnswer> WindowsSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "windows.snapshot", null, cancellationToken);

    /// <summary>
    /// Moves the main window to a display, named by <c>QScreen::name()</c>.
    /// </summary>
    /// <remarks>
    /// There is no index form on purpose: an ordinal would silently mean a different
    /// monitor after a topology change, which is the one thing a mixed-monitor gate
    /// must not do.
    /// </remarks>
    public static Task<ControlAnswer> MoveToScreenAsync(
        this ILiveVerifySession session,
        string screenName,
        CancellationToken cancellationToken) =>
        Invoke(
            session,
            "window.moveToScreen",
            new Dictionary<string, object?>(StringComparer.Ordinal) { ["screen"] = screenName },
            cancellationToken);

    /// <summary>The application's update state machine, plus the endpoint of any updater it launched.</summary>
    public static Task<ControlAnswer> UpdateStateAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "update.getState", null, cancellationToken);

    /// <summary>Asks the product to check for an update.</summary>
    public static Task<ControlAnswer> UpdateCheckAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "update.check", null, cancellationToken);

    /// <summary>The notification hub's permanent record.</summary>
    public static Task<ControlAnswer> NotificationsSnapshotAsync(this ILiveVerifySession session, CancellationToken cancellationToken) =>
        Invoke(session, "notifications.snapshot", null, cancellationToken);

    /// <summary>Sets one product setting through the product's own settings surface.</summary>
    public static Task<ControlAnswer> SetSettingAsync(
        this ILiveVerifySession session,
        string key,
        object? value,
        CancellationToken cancellationToken) =>
        Invoke(
            session,
            "settings.set",
            new Dictionary<string, object?>(StringComparer.Ordinal) { ["key"] = key, ["value"] = value },
            cancellationToken);

    private static Task<ControlAnswer> Invoke(
        ILiveVerifySession session,
        string command,
        IReadOnlyDictionary<string, object?>? parameters,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(session);
        return session.InvokeAsync(command, parameters, cancellationToken);
    }

    /// <summary>The default deadline a command is given when a gate names none.</summary>
    public static TimeSpan CommandTimeout => DefaultTimeout;
}
