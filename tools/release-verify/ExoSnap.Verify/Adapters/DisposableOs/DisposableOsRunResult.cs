using System.Text.Json;
using System.Text.Json.Serialization;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>One step a disposable-OS worker script recorded, in the order it ran them.</summary>
/// <param name="Name">The step's stable name, matched against a gate's required-step list.</param>
/// <param name="Ok">Whether the step succeeded.</param>
/// <param name="Detail">One sentence about what the step observed.</param>
public sealed record DisposableOsStepResult(string Name, bool Ok, string Detail);

/// <summary>
/// The result document a disposable-OS worker script writes: every step it
/// reached, in the shape <c>scripts/lib/sandbox-*-worker.ps1</c> already write.
/// </summary>
/// <param name="Steps">Every step the worker reached, in run order.</param>
public sealed record DisposableOsRunResult(IReadOnlyList<DisposableOsStepResult> Steps)
{
    /// <summary>Parses a worker's result document, or null when it is not valid JSON.</summary>
    public static DisposableOsRunResult? Parse(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        try
        {
            var document = JsonSerializer.Deserialize(json, DisposableOsJson.Default.DisposableOsRunResult);
            return document is null ? null : document with { Steps = document.Steps ?? [] };
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

/// <summary>What a disposable-OS run's step list adds up to.</summary>
public enum DisposableOsVerdictKind
{
    /// <summary>Every required step was reached and reported ok.</summary>
    Pass,

    /// <summary>A required step was reached and reported not ok. ExoSnap is wrong.</summary>
    Fail,

    /// <summary>The worker never reached a required step, or wrote no result at all.</summary>
    Unverified,
}

/// <summary>The verdict a step list adds up to, and why.</summary>
/// <param name="Kind">The verdict.</param>
/// <param name="Message">One sentence naming what decided it.</param>
public sealed record DisposableOsVerdict(DisposableOsVerdictKind Kind, string Message)
{
    /// <summary>
    /// Applies the shared disposable-OS verdict rule: a step that ran and failed is
    /// a FAIL; a required step the worker never reached is UNVERIFIED; only a run
    /// where every required step is present and ok is a PASS. A worker that stopped
    /// early otherwise leaves nothing but passes, so completeness is checked before
    /// any step's own flag is trusted.
    /// </summary>
    public static DisposableOsVerdict From(DisposableOsRunResult? result, IReadOnlyList<string> requiredSteps)
    {
        ArgumentNullException.ThrowIfNull(requiredSteps);
        if (result is null)
        {
            return new DisposableOsVerdict(DisposableOsVerdictKind.Unverified, "no result document was produced");
        }

        var byName = result.Steps.ToDictionary(step => step.Name, StringComparer.Ordinal);
        var failed = new List<string>();
        var missing = new List<string>();
        foreach (var name in requiredSteps)
        {
            if (!byName.TryGetValue(name, out var step))
            {
                missing.Add(name);
                continue;
            }

            if (!step.Ok)
            {
                failed.Add($"{name}: {step.Detail}");
            }
        }

        if (failed.Count > 0)
        {
            return new DisposableOsVerdict(
                DisposableOsVerdictKind.Fail,
                $"{failed.Count} step(s) failed: {string.Join(" | ", failed)}");
        }

        if (missing.Count > 0)
        {
            return new DisposableOsVerdict(
                DisposableOsVerdictKind.Unverified,
                $"the worker never reached {missing.Count} required step(s): {string.Join(", ", missing)}");
        }

        return new DisposableOsVerdict(
            DisposableOsVerdictKind.Pass,
            $"all {requiredSteps.Count} required step(s) passed");
    }
}

/// <summary>Source-generated contract for the disposable-OS worker result document.</summary>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    DefaultIgnoreCondition = JsonIgnoreCondition.Never)]
[JsonSerializable(typeof(DisposableOsRunResult))]
public sealed partial class DisposableOsJson : JsonSerializerContext;
