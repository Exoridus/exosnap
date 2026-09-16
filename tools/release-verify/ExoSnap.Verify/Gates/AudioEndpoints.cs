using ExoSnap.Verify.Models;
using System.Text.Json;

namespace ExoSnap.Verify.Gates;

/// <summary>One render endpoint as the product enumerates it.</summary>
/// <param name="Name">The friendly name, which is what the external switcher addresses.</param>
/// <param name="IsDefault">Whether it is currently the default for playback.</param>
public sealed record AudioEndpoint(string Name, bool IsDefault);

/// <summary>One poll of the audio source during a recording.</summary>
/// <param name="Active">Whether a source was being captured at that moment.</param>
/// <param name="Degraded">Whether the product reported the source degraded.</param>
public sealed record AudioSample(bool Active, bool Degraded);

/// <summary>
/// Reading render endpoints, and deciding what a run of audio samples means.
/// </summary>
/// <remarks>
/// The endpoints come from the product's own environment snapshot rather than from
/// WASAPI directly: a gate about what the product captures has to be told which
/// endpoints the product enumerates, and one Windows offers that ExoSnap does not see
/// is itself the finding.
/// </remarks>
public static class AudioEndpoints
{
    /// <summary>The render endpoints in an <c>environment.snapshot</c> answer.</summary>
    public static IReadOnlyList<AudioEndpoint> From(JsonElement snapshot)
    {
        var endpoints = new List<AudioEndpoint>();
        foreach (var output in Snapshots.Items(snapshot, "audio.outputs"))
        {
            var name = Snapshots.Text(output, "name");
            if (!string.IsNullOrWhiteSpace(name))
            {
                endpoints.Add(new AudioEndpoint(name, Snapshots.IsTrue(output, "default")));
            }
        }

        return endpoints;
    }

    /// <summary>The endpoint whose name matches <paramref name="pattern"/>, or null.</summary>
    /// <remarks>
    /// Deliberately no fallback to "the first output". A gate that silently
    /// reconfigured somebody's speakers because the device it wanted was absent would
    /// be worse than one that reports the precondition missing.
    /// </remarks>
    public static string? MatchingName(IEnumerable<AudioEndpoint> endpoints, string pattern)
    {
        ArgumentNullException.ThrowIfNull(endpoints);
        ArgumentException.ThrowIfNullOrWhiteSpace(pattern);

        return endpoints.FirstOrDefault(endpoint => Matches(endpoint.Name, pattern))?.Name;
    }

    /// <summary>The endpoint currently default for playback, or null.</summary>
    public static string? DefaultName(IEnumerable<AudioEndpoint> endpoints)
    {
        ArgumentNullException.ThrowIfNull(endpoints);
        return endpoints.FirstOrDefault(endpoint => endpoint.IsDefault)?.Name;
    }

    /// <summary>
    /// What a window of samples says about a connected but silent source.
    /// </summary>
    /// <remarks>
    /// Two different answers, and the difference is the point of the gate. A source
    /// the product reported degraded while nothing was playing is a defect:
    /// degradation means the device is gone, not that it is quiet. A window in which
    /// no source was ever active measured nothing about how a silent one is treated
    /// -- an assertion over sources that did not exist is satisfied the way an empty
    /// list satisfies anything -- so it is an infrastructure error and not a verdict.
    /// </remarks>
    public static ScenarioResult SilenceVerdict(IReadOnlyList<AudioSample> samples, IReadOnlyList<Evidence> evidence)
    {
        ArgumentNullException.ThrowIfNull(samples);
        ArgumentNullException.ThrowIfNull(evidence);

        if (samples.Count == 0)
        {
            return ScenarioResult.InfrastructureError("the audio source was never polled", [.. evidence]);
        }

        if (!samples.Any(sample => sample.Active))
        {
            return ScenarioResult.InfrastructureError(
                $"no audio source was active across {samples.Count} sample(s), so nothing was observed about how "
                + "a silent one is treated",
                [.. evidence]);
        }

        var degraded = samples.Count(sample => sample.Degraded);
        return degraded > 0
            ? ScenarioResult.Fail(
                $"a connected but silent source was reported as degraded in {degraded} of {samples.Count} sample(s)",
                [.. evidence])
            : ScenarioResult.Pass(
                $"an active source, silent across {samples.Count} sample(s), produced no degradation",
                [.. evidence]);
    }

    /// <summary>
    /// The sample rate an envctl <c>wave-format</c> string declares, or null.
    /// </summary>
    /// <remarks>
    /// The format is rendered as <c>rate/bits/channels</c>. Parsed rather than
    /// compared as a whole string because a gate asks about the rate alone: an
    /// endpoint at 44100/24/2 and one at 44100/16/2 are both the 44.1 kHz case.
    /// </remarks>
    public static int? SampleRateOf(string waveFormat)
    {
        if (string.IsNullOrWhiteSpace(waveFormat))
        {
            return null;
        }

        var head = waveFormat.Split('/')[0].Trim();
        return int.TryParse(head, out var rate) && rate > 0 ? rate : null;
    }

    private static bool Matches(string name, string pattern)
    {
        // The pattern comes from a gate or from a variable an operator sets once for
        // their machine, and the only form either uses is a trailing wildcard.
        if (pattern.EndsWith('*'))
        {
            return name.StartsWith(pattern[..^1], StringComparison.OrdinalIgnoreCase);
        }

        return string.Equals(name, pattern, StringComparison.OrdinalIgnoreCase);
    }
}
