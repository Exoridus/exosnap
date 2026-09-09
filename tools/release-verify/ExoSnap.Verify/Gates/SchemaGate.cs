using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-SCHEMA-001: every field path the catalog reads actually exists.
/// </summary>
/// <remarks>
/// Gates once shipped reading field paths no emitter emits, and several of them threw
/// inside a human gate - after the operator had already unplugged an audio interface
/// or answered a UAC prompt. The errors survived for one reason: the opt-in gates were
/// never executed, so nothing ever evaluated the paths. A catalog whose assertions are
/// only checked when a person is standing at the machine has no early failure mode at
/// all, and this gate is that failure mode.
///
/// It asserts existence, never values. What a field says is the other gates' business;
/// that it is there at all is a contract between this catalog and the product's
/// emitters, and contracts are checked cheaply.
/// </remarks>
public sealed class FieldContractGate : IScenarioBody
{
    private static readonly TimeSpan RecordingSettle = TimeSpan.FromSeconds(4);
    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan NotificationWindow = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan NotificationPoll = TimeSpan.FromMilliseconds(250);

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var contract = FieldContract.Entries();
        if (contract.Count == 0)
        {
            return ScenarioResult.Fail("the field contract is empty; nothing was checked");
        }

        // The hub keeps a permanent record but starts empty, so an entry has to be
        // published before the entry-shape paths can be walked at all. Waited for as a
        // state, not slept over: whether a diagnostics run publishes anything on this
        // machine depends on what it finds, and an empty hub is reported as unchecked
        // rather than passed.
        await session.InvokeAsync("diagnostics.run", null, cancellationToken).ConfigureAwait(false);
        await WaitForNotificationAsync(session, NotificationWindow, cancellationToken).ConfigureAwait(false);

        var missing = new List<string>();
        var skipped = new List<string>();
        var checkedPaths = 0;
        var snapshots = new Dictionary<string, JsonElement>(StringComparer.Ordinal);

        foreach (var stage in new[] { ContractStage.Idle, ContractStage.Recording, ContractStage.Result })
        {
            var stageContract = contract.Where(entry => entry.Stage == stage).ToList();
            if (stageContract.Count == 0)
            {
                continue;
            }

            if (stage == ContractStage.Recording)
            {
                var refusal = await StartRecordingAsync(session, skipped, cancellationToken).ConfigureAwait(false);
                if (refusal is not null)
                {
                    return refusal;
                }
            }
            else if (stage == ContractStage.Result)
            {
                await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
                await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
                    .ConfigureAwait(false);
            }

            foreach (var command in stageContract.Select(entry => entry.Command).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal))
            {
                var answer = await session.InvokeAsync(command, null, cancellationToken).ConfigureAwait(false);
                if (!answer.Ok)
                {
                    // A refused command is a different fact from a missing field, and
                    // it is reported as its own line rather than folded into either a
                    // pass or a failure.
                    skipped.Add($"{command} refused: {answer.Refusal}");
                    continue;
                }

                snapshots[$"{Name(stage)}/{command}"] = answer.Result.Clone();

                foreach (var entry in stageContract.Where(candidate =>
                             string.Equals(candidate.Command, command, StringComparison.Ordinal)))
                {
                    checkedPaths++;
                    var resolved = Snapshots.Resolve(answer.Result, entry.Path);
                    switch (resolved.Presence)
                    {
                        case FieldPresence.Present:
                            break;
                        case FieldPresence.Empty:
                            // The collection is there and the name is therefore right;
                            // its element shape simply could not be walked right now.
                            // Named out loud so an empty check never reads as a pass.
                            skipped.Add($"{command} {entry.Path}: '{resolved.At}' is empty, element shape unchecked");
                            break;
                        default:
                            missing.Add(
                                $"{command}.{entry.Path} (read by {entry.UsedBy}) -- '{resolved.At}' is not emitted");
                            break;
                    }
                }
            }
        }

        var evidence = new[]
        {
            SaveSnapshots(context, snapshots),
            GateEvidence.SaveText(
                context,
                "contract.json",
                JsonSerializer.Serialize(
                    new ContractSummary(checkedPaths, missing, skipped),
                    GateJson.Default.ContractSummary)),
        };

        if (missing.Count > 0)
        {
            return ScenarioResult.Fail(
                $"{Count(missing.Count)} of {Count(checkedPaths)} field path(s) do not exist: {string.Join(" | ", missing)}",
                evidence);
        }

        var note = skipped.Count > 0
            ? $"; {Count(skipped.Count)} unchecked: {string.Join(" | ", skipped)}"
            : string.Empty;

        return ScenarioResult.Pass(
            $"{Count(checkedPaths)} field path(s) exist across {Count(snapshots.Count)} snapshot(s){note}",
            evidence);
    }

    private static async Task WaitForNotificationAsync(
        ILiveVerifySession session,
        TimeSpan window,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + window;
        while (DateTime.UtcNow < deadline)
        {
            var hub = await session.NotificationsSnapshotAsync(cancellationToken).ConfigureAwait(false);
            if (hub.Ok && Snapshots.NotificationEntries(hub.Result).Count > 0)
            {
                return;
            }

            await Task.Delay(NotificationPoll, cancellationToken).ConfigureAwait(false);
        }
    }

    private static async Task<ScenarioResult?> StartRecordingAsync(
        ILiveVerifySession session,
        List<string> skipped,
        CancellationToken cancellationToken)
    {
        await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);

        // Every audio path in the contract asserts over a source that has to exist.
        // The shipped default has system audio on, but a campaign runs against
        // whatever settings the machine already had, and a check that asserts over
        // audio it never enabled asserts over nothing.
        var audio = await SystemAudio.EnableAsync(session, cancellationToken).ConfigureAwait(false);
        if (!audio.Enabled)
        {
            skipped.Add($"audio paths: {audio.Detail}");
        }

        var started = await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        if (!started.Ok)
        {
            return ScenarioResult.Fail($"record.start refused: {started.Refusal}");
        }

        await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);

        // Long enough for the audio and A/V groups to have measured something. A group
        // that exists but has seen nothing still has its keys, which is all this
        // asserts.
        await Task.Delay(RecordingSettle, cancellationToken).ConfigureAwait(false);
        return null;
    }

    private static Evidence SaveSnapshots(ScenarioContext context, IReadOnlyDictionary<string, JsonElement> snapshots)
    {
        var path = Path.Combine(context.EvidenceDirectory, "snapshots.json");
        Directory.CreateDirectory(context.EvidenceDirectory);

        using (var stream = File.Create(path))
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            foreach (var (key, value) in snapshots.OrderBy(pair => pair.Key, StringComparer.Ordinal))
            {
                writer.WritePropertyName(key);
                value.WriteTo(writer);
            }

            writer.WriteEndObject();
        }

        return Evidence.ForFile("snapshots", path);
    }

    private static string Name(ContractStage stage) => stage switch
    {
        ContractStage.Recording => "recording",
        ContractStage.Result => "result",
        _ => "idle",
    };

    private static string Count(int value) => value.ToString(CultureInfo.InvariantCulture);
}

/// <summary>What one field-contract pass checked, and what it could not.</summary>
/// <param name="Checked">How many paths were walked.</param>
/// <param name="Missing">Paths no emitter emits.</param>
/// <param name="Skipped">Paths that could not be walked, each with its reason.</param>
public sealed record ContractSummary(int Checked, IReadOnlyList<string> Missing, IReadOnlyList<string> Skipped);

/// <summary>Whether the product's system-audio source could be turned on.</summary>
/// <param name="Enabled">Whether it is on now.</param>
/// <param name="Detail">What happened, in one sentence.</param>
public sealed record SystemAudioState(bool Enabled, string Detail);

/// <summary>
/// Turns the system-audio source on through the product's own settings surface.
/// </summary>
/// <remarks>
/// Every audio assertion needs this and none of them may assume it. The shipped
/// default has system audio on, but a campaign runs against whatever settings the
/// machine already had - and a gate that asserts over audio it never enabled asserts
/// over nothing. One gate passed that way, looking for a degraded source among sources
/// that did not exist.
/// </remarks>
public static class SystemAudio
{
    /// <summary>The setting key the source is enabled through.</summary>
    public const string SettingKey = "audio.systemEnabled";

    /// <summary>Turns it on, and confirms it through the product's own record surface.</summary>
    public static async Task<SystemAudioState> EnableAsync(
        ILiveVerifySession session,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(session);

        var set = await session.SetSettingAsync(SettingKey, true, cancellationToken).ConfigureAwait(false);
        if (!set.Ok)
        {
            return new SystemAudioState(false, $"settings.set {SettingKey} refused: {set.Refusal}");
        }

        var record = await session.RecordSnapshotAsync(cancellationToken).ConfigureAwait(false);
        return Snapshots.IsTrue(record.Result, "systemAudioEnabled")
            ? new SystemAudioState(true, "system audio enabled")
            : new SystemAudioState(false, "the system-audio source did not become enabled after settings.set");
    }
}
