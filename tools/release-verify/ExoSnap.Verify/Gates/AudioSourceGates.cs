using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// The one render-endpoint control Windows exposes no documented setter for.
/// </summary>
/// <remarks>
/// <c>default-roles</c> and <c>device-format</c> are both ENV_HUMAN in the envctl
/// catalogue, and envctl refuses to write them by policy: the only route is the
/// undocumented IPolicyConfig interface, which is out of scope. So the mechanism
/// stays outside the release path in a named third-party tool -- the runner knows
/// what it wants, not how -- and a machine without the tool reports the gate
/// unavailable rather than passing it.
/// </remarks>
public sealed class AudioEndpointControl
{
    /// <summary>The variable pinning SoundVolumeView.exe.</summary>
    public const string PathVariable = "EXOSNAP_SOUNDVOLUMEVIEW";

    private readonly ProcessRunner processes;
    private readonly ResolvedTool tool;

    /// <summary>Creates the control resolving SoundVolumeView on this machine.</summary>
    public AudioEndpointControl(ProcessRunner processes, ToolResolver tools)
    {
        ArgumentNullException.ThrowIfNull(processes);
        ArgumentNullException.ThrowIfNull(tools);
        this.processes = processes;
        this.tool = tools.Resolve("SoundVolumeView", PathVariable);
    }

    /// <summary>Whether the endpoint control is usable here.</summary>
    public bool Available => this.tool.Available;

    /// <summary>Why it is not, or an empty string when it is.</summary>
    public string UnavailableReason => this.Available
        ? string.Empty
        : $"SoundVolumeView.exe was not found (checked {PathVariable} and PATH); Windows exposes no documented "
            + "setter for the default render endpoint, so this gate cannot route audio without it";

    /// <summary>Makes one render endpoint the default for every role.</summary>
    public async Task<(bool Ok, string Detail)> SetDefaultAsync(string endpointName, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(endpointName);
        var run = await this.processes
            .RunAsync(new ProcessRunRequest(this.tool.Path!, "/SetDefault", endpointName, "all"), cancellationToken)
            .ConfigureAwait(false);

        return run.Succeeded
            ? (true, $"'{endpointName}' is the default render endpoint for every role")
            : (false, $"SoundVolumeView /SetDefault '{endpointName}' exited {run.ExitCode}: {run.StandardError}");
    }
}

/// <summary>
/// REL-AUD-SILENCE-001: a connected source that is playing nothing is not degraded.
/// </summary>
/// <remarks>
/// The distinction is the whole gate. Degradation means the device is gone; quiet is
/// not a fault. Only a real endpoint can be the second while remaining the first.
///
/// Silence is produced by routing playback to a virtual cable rather than by asking a
/// person not to make a sound: nobody is routed to a cable, so silence is a fact about
/// the machine instead of a promise, and the gate stops being wrong the moment a
/// notification chimes. The previous default is restored whatever happens -- a
/// campaign that left the developer's sound on a virtual cable would have broken the
/// machine it was verifying.
/// </remarks>
public sealed class AudioSilenceGate : IScenarioBody
{
    /// <summary>The variable naming an endpoint nothing plays to.</summary>
    public const string EndpointVariable = "EXOSNAP_SILENT_AUDIO_ENDPOINT";

    /// <summary>The endpoint pattern used when the variable is unset.</summary>
    public const string DefaultEndpointPattern = "CABLE Input*";

    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan SampleEvery = TimeSpan.FromMilliseconds(500);

    private readonly TimeSpan observeFor;
    private readonly Func<string, string?> readEnvironment;

    /// <summary>Creates the gate with the release observation window.</summary>
    public AudioSilenceGate()
        : this(TimeSpan.FromSeconds(15), Environment.GetEnvironmentVariable)
    {
    }

    /// <summary>Creates the gate with an explicit window, so its logic needs no wait.</summary>
    public AudioSilenceGate(TimeSpan observeFor, Func<string, string?> readEnvironment)
    {
        ArgumentNullException.ThrowIfNull(readEnvironment);
        this.observeFor = observeFor;
        this.readEnvironment = readEnvironment;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.AudioEndpoints.Available)
        {
            return ScenarioResult.Unavailable(services.AudioEndpoints.UnavailableReason);
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        // Every audio scenario needs the system source on and none of them may assume
        // it: a campaign runs against whatever settings the machine already had, and a
        // scenario that asserts over audio it never enabled asserts over nothing.
        var enabled = await EnableSystemAudioAsync(session, cancellationToken).ConfigureAwait(false);
        if (enabled is not null)
        {
            return ScenarioResult.InfrastructureError(enabled);
        }

        var pattern = this.readEnvironment(EndpointVariable) is { Length: > 0 } configured
            ? configured
            : DefaultEndpointPattern;

        var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var endpoints = AudioEndpoints.From(environment.Result);
        var silent = AudioEndpoints.MatchingName(endpoints, pattern);
        var previous = AudioEndpoints.DefaultName(endpoints);

        if (silent is null)
        {
            return ScenarioResult.Unavailable(
                $"no render endpoint matches '{pattern}'. Install VB-CABLE from https://vb-audio.com/Cable/, or "
                + $"set {EndpointVariable} to a device nothing plays to");
        }

        if (previous is null)
        {
            return ScenarioResult.InfrastructureError(
                "the product reports no default render endpoint, so there is nothing to restore afterwards");
        }

        var routed = false;
        try
        {
            if (!string.Equals(silent, previous, StringComparison.OrdinalIgnoreCase))
            {
                var switched = await services.AudioEndpoints.SetDefaultAsync(silent, cancellationToken)
                    .ConfigureAwait(false);
                if (!switched.Ok)
                {
                    return ScenarioResult.InfrastructureError(switched.Detail);
                }

                routed = true;
            }

            return await this.ObserveAsync(context, session, silent, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            if (routed)
            {
                await services.AudioEndpoints.SetDefaultAsync(previous, CancellationToken.None).ConfigureAwait(false);
            }
        }
    }

    private static async Task<string?> EnableSystemAudioAsync(
        ILiveVerifySession session, CancellationToken cancellationToken)
    {
        var set = await session.SetSettingAsync("audio.systemEnabled", true, cancellationToken).ConfigureAwait(false);
        if (!set.Ok)
        {
            return $"settings.set audio.systemEnabled refused: {set.Refusal}";
        }

        var record = await session.RecordSnapshotAsync(cancellationToken).ConfigureAwait(false);
        return Snapshots.IsTrue(record.Result, "systemAudioEnabled")
            ? null
            : "the system-audio source did not become enabled after settings.set";
    }

    private async Task<ScenarioResult> ObserveAsync(
        ScenarioContext context,
        ILiveVerifySession session,
        string endpointName,
        CancellationToken cancellationToken)
    {
        await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
        var started = await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        if (!started.Ok)
        {
            return ScenarioResult.Fail($"record.start refused: {started.Refusal}");
        }

        await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);

        var samples = new List<AudioSample>();
        var raw = new List<JsonElement>();
        var deadline = DateTime.UtcNow + this.observeFor;
        while (DateTime.UtcNow < deadline)
        {
            var pipeline = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
            raw.Add(pipeline.Result);
            samples.Add(new AudioSample(
                Snapshots.IsTrue(pipeline.Result, "audio.active"),
                Snapshots.IsTrue(pipeline.Result, "audio.sourceDegraded")));
            await Task.Delay(SampleEvery, cancellationToken).ConfigureAwait(false);
        }

        await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);

        var evidence = new[]
        {
            GateEvidence.SaveText(
                context,
                "pipeline-samples.json",
                "[" + string.Join(",", raw.Select(element => element.GetRawText())) + "]"),
        };

        var verdict = AudioEndpoints.SilenceVerdict(samples, evidence);
        return verdict with { Message = $"[routed to '{endpointName}'] {verdict.Message}" };
    }
}

/// <summary>
/// REL-AUD-FORMAT-001: recording on a 44.1 kHz endpoint produces correct audio.
/// </summary>
/// <remarks>
/// The rate is the point: the engine mixes at 48 kHz and a 44.1 kHz endpoint is the
/// case where a resampler is in the path. What the gate asserts is the recorded file,
/// through ffprobe, against the endpoint the product reported recording from.
///
/// The endpoint format is not set here. <c>device-format</c> is ENV_HUMAN in the
/// envctl catalogue -- the Settings "Default Format" drop-down has no public API --
/// so a machine with no 44.1 kHz endpoint reports the gate unavailable and names what
/// to prepare, rather than passing on a 48 kHz device and calling it the 44.1 case.
/// </remarks>
public sealed class AudioFormatGate : IScenarioBody
{
    /// <summary>The rate this gate is about.</summary>
    public const int Rate = 44100;

    /// <summary>The variable naming the prepared endpoint.</summary>
    public const string EndpointVariable = "EXOSNAP_44100_AUDIO_ENDPOINT";

    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromMinutes(2);

    private readonly TimeSpan recordFor;
    private readonly Func<string, string?> readEnvironment;

    /// <summary>Creates the gate with the release recording length.</summary>
    public AudioFormatGate()
        : this(TimeSpan.FromSeconds(10), Environment.GetEnvironmentVariable)
    {
    }

    /// <summary>Creates the gate with an explicit length, so its logic needs no wait.</summary>
    public AudioFormatGate(TimeSpan recordFor, Func<string, string?> readEnvironment)
    {
        ArgumentNullException.ThrowIfNull(readEnvironment);
        this.recordFor = recordFor;
        this.readEnvironment = readEnvironment;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Ffprobe.Available)
        {
            return ScenarioResult.Unavailable("ffprobe is not resolvable");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var configured = this.readEnvironment(EndpointVariable);
        if (string.IsNullOrWhiteSpace(configured))
        {
            return ScenarioResult.Unavailable(
                $"set {EndpointVariable} to a render endpoint whose shared-mode format is {Rate} Hz; Windows "
                + "exposes no documented setter for it, so the device is prepared once in Sound settings");
        }

        var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var endpoints = AudioEndpoints.From(environment.Result);
        var target = AudioEndpoints.MatchingName(endpoints, configured);
        if (target is null)
        {
            return ScenarioResult.Unavailable(
                $"ExoSnap enumerates no render endpoint matching '{configured}'");
        }

        var enabled = await EnableSystemAudioAsync(session, cancellationToken).ConfigureAwait(false);
        if (enabled is not null)
        {
            return ScenarioResult.InfrastructureError(enabled);
        }

        await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
        var started = await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        if (!started.Ok)
        {
            return ScenarioResult.Fail($"record.start refused: {started.Refusal}");
        }

        await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);
        await Task.Delay(this.recordFor, cancellationToken).ConfigureAwait(false);
        await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
            .ConfigureAwait(false);

        var result = await session.RecordResultAsync(cancellationToken).ConfigureAwait(false);
        var evidence = new List<Evidence> { GateEvidence.SaveJson(context, "record-result.json", result.Result) };

        if (!Snapshots.IsTrue(result.Result, "succeeded"))
        {
            return ScenarioResult.Fail($"the recording on '{target}' did not succeed", [.. evidence]);
        }

        var outputPath = Snapshots.Text(result.Result, "outputPath");
        var probe = await services.Ffprobe.InspectAsync(outputPath, cancellationToken).ConfigureAwait(false);
        evidence.Add(GateEvidence.SaveText(context, "ffprobe.json", probe.RawJson));

        return AudioFormatVerdict(probe.AudioStreams.Select(stream => stream.SampleRateHz).ToList(), target, evidence);
    }

    /// <summary>
    /// What the recorded audio streams say about a recording made on a 44.1 kHz endpoint.
    /// </summary>
    /// <remarks>
    /// The recorded rate is not required to be the endpoint's. The engine mixes at
    /// 48 kHz and resampling to it is the correct behaviour -- what would be wrong is
    /// no audio track at all, or a track whose rate is neither the endpoint's nor the
    /// engine's, which means something in the path invented a third one.
    /// </remarks>
    public static ScenarioResult AudioFormatVerdict(
        IReadOnlyList<int?> recordedRates, string endpointName, IReadOnlyList<Evidence> evidence)
    {
        ArgumentNullException.ThrowIfNull(recordedRates);
        ArgumentNullException.ThrowIfNull(evidence);

        if (recordedRates.Count == 0)
        {
            return ScenarioResult.Fail(
                $"the recording on '{endpointName}' carries no audio track", [.. evidence]);
        }

        var unexpected = recordedRates
            .Where(rate => rate is not (Rate or 48000))
            .Select(rate => rate?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "unknown")
            .ToList();

        return unexpected.Count > 0
            ? ScenarioResult.Fail(
                $"a recording from the {Rate} Hz endpoint '{endpointName}' carries audio at "
                + $"{string.Join(", ", unexpected)} Hz, which is neither the endpoint rate nor the engine mix rate",
                [.. evidence])
            : ScenarioResult.Pass(
                $"the recording from the {Rate} Hz endpoint '{endpointName}' carries "
                + $"{recordedRates.Count} audio track(s) at an expected rate",
                [.. evidence]);
    }

    private static async Task<string?> EnableSystemAudioAsync(
        ILiveVerifySession session, CancellationToken cancellationToken)
    {
        var set = await session.SetSettingAsync("audio.systemEnabled", true, cancellationToken).ConfigureAwait(false);
        if (!set.Ok)
        {
            return $"settings.set audio.systemEnabled refused: {set.Refusal}";
        }

        var record = await session.RecordSnapshotAsync(cancellationToken).ConfigureAwait(false);
        return Snapshots.IsTrue(record.Result, "systemAudioEnabled")
            ? null
            : "the system-audio source did not become enabled after settings.set";
    }
}
