using System.Text.Json;
using System.Text.Json.Serialization;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>What a step was doing, which decides what its failure means.</summary>
public static class DisposableOsStepKind
{
    /// <summary>
    /// The step asserted something about ExoSnap. Only these may fail a gate.
    /// </summary>
    public const string Product = "product";

    /// <summary>
    /// The step was building the environment the assertions need -- installing the
    /// older release, selecting a channel, launching a helper. A failure here
    /// measured nothing about the product.
    /// </summary>
    public const string Bootstrap = "bootstrap";

    /// <summary>Whether this is a kind the verdict rule knows how to read.</summary>
    public static bool IsRecognised(string? kind) =>
        string.Equals(kind, Product, StringComparison.Ordinal)
        || string.Equals(kind, Bootstrap, StringComparison.Ordinal);
}

/// <summary>One step a disposable-OS worker script recorded, in the order it ran them.</summary>
/// <param name="Name">The step's stable name, matched against a gate's required-step list.</param>
/// <param name="Ok">Whether the step succeeded.</param>
/// <param name="Detail">One sentence about what the step observed.</param>
/// <param name="Kind">
/// <see cref="DisposableOsStepKind.Product"/> or <see cref="DisposableOsStepKind.Bootstrap"/>.
/// There is deliberately no default that reads as either.
/// </param>
/// <remarks>
/// An unclassified step is its own outcome, not a product assertion by default.
/// Defaulting to product looks conservative and is not: "a new product check
/// nobody classified" and "a new bootstrap step nobody classified" are
/// indistinguishable from the outside, and reading both as product turns the
/// second into a false accusation against ExoSnap on a gate that is required for
/// promotion. The rule here is the same one the whole harness follows -- what was
/// not measured is never a defect -- so an unclassified step leaves the run
/// unverified and the schema gets fixed instead of a verdict being guessed.
/// </remarks>
public sealed record DisposableOsStepResult(
    string Name,
    bool Ok,
    string Detail,
    string Kind = "")
{
    /// <summary>A failure of this step is a statement about ExoSnap.</summary>
    public bool IsProductAssertion =>
        string.Equals(this.Kind, DisposableOsStepKind.Product, StringComparison.Ordinal);

    /// <summary>A failure of this step says the test environment was not built.</summary>
    public bool IsBootstrap =>
        string.Equals(this.Kind, DisposableOsStepKind.Bootstrap, StringComparison.Ordinal);

    /// <summary>
    /// The step carries no kind this rule knows, so nothing can be concluded from
    /// it either way.
    /// </summary>
    public bool IsUnclassified => !DisposableOsStepKind.IsRecognised(this.Kind);
}

/// <summary>
/// The result document a disposable-OS worker script writes: every step it
/// reached, in the shape <c>scripts/lib/sandbox-*-worker.ps1</c> already write.
/// </summary>
/// <param name="Steps">Every step the worker reached, in run order.</param>
public sealed record DisposableOsRunResult(IReadOnlyList<DisposableOsStepResult> Steps)
{
    /// <summary>
    /// Parses a worker's result document, or null when it is not valid JSON. A document
    /// that is valid JSON but omits parts of a step is repaired rather than rejected:
    /// a step without a name matches no required step and so reads as never reached.
    /// </summary>
    public static DisposableOsRunResult? Parse(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        try
        {
            var document = JsonSerializer.Deserialize(json, DisposableOsJson.Default.DisposableOsRunResult);
            return document is null ? null : document with { Steps = Repair(document.Steps) };
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // The document is written by a script running inside a disposable OS that may be
    // torn down mid-write, so JSON that parses is no guarantee that every step object
    // carries every member. Deserialization fills those with null despite the
    // non-nullable record signature; the verdict rule must see data, not a crash.
    private static List<DisposableOsStepResult> Repair(IReadOnlyList<DisposableOsStepResult>? steps)
    {
        if (steps is null)
        {
            return [];
        }

        var repaired = new List<DisposableOsStepResult>(steps.Count);
        foreach (var step in steps)
        {
            if (step is null)
            {
                continue;
            }

            // A null kind stays empty rather than being filled in: it is exactly
            // the "nobody classified this" case, and inventing a classification
            // here would be the guess the verdict rule refuses to make.
            repaired.Add(step.Name is null || step.Detail is null || step.Kind is null
                ? step with
                {
                    Name = step.Name ?? string.Empty,
                    Detail = step.Detail ?? string.Empty,
                    Kind = step.Kind ?? string.Empty,
                }
                : step);
        }

        return repaired;
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

        // A worker that recorded the same step twice is malformed, so the not-ok
        // observation is kept: a later retry must never overwrite a recorded failure.
        var byName = new Dictionary<string, DisposableOsStepResult>(StringComparer.Ordinal);
        foreach (var step in result.Steps)
        {
            if (byName.TryGetValue(step.Name, out var seen) && !seen.Ok)
            {
                continue;
            }

            byName[step.Name] = step;
        }

        // An unclassified step is checked before anything else is read out of the
        // document: the schema it was written against is not the one this rule
        // reads, so no verdict drawn from it would mean what it says.
        var unclassified = new List<string>();
        foreach (var step in byName.Values)
        {
            if (step.IsUnclassified)
            {
                unclassified.Add(string.IsNullOrEmpty(step.Kind)
                    ? $"{step.Name}: no kind"
                    : $"{step.Name}: unrecognised kind '{step.Kind}'");
            }
        }

        if (unclassified.Count > 0)
        {
            unclassified.Sort(StringComparer.Ordinal);
            return new DisposableOsVerdict(
                DisposableOsVerdictKind.Unverified,
                $"{unclassified.Count} step(s) do not say whether they assert the product or build the "
                + $"environment, so a failure could not be attributed: {string.Join(", ", unclassified)}");
        }

        var failed = new List<string>();
        var bootstrapFailed = new List<string>();
        var missing = new List<string>();
        foreach (var name in requiredSteps)
        {
            if (!byName.TryGetValue(name, out var step))
            {
                missing.Add(name);
                continue;
            }

            if (step.Ok)
            {
                continue;
            }

            if (step.IsBootstrap)
            {
                bootstrapFailed.Add($"{name}: {step.Detail}");
            }
            else
            {
                failed.Add($"{name}: {step.Detail}");
            }
        }

        // A bootstrap step the gate does not require can still have stopped the run:
        // every later step depends on the environment it was building.
        foreach (var step in byName.Values)
        {
            if (!step.Ok && step.IsBootstrap && !bootstrapFailed.Exists(
                    entry => entry.StartsWith(step.Name + ":", StringComparison.Ordinal)))
            {
                bootstrapFailed.Add($"{step.Name}: {step.Detail}");
            }
        }

        // A bootstrap step that failed built no environment, so nothing downstream
        // of it measured the product -- reporting that as a product failure is the
        // harness accusing ExoSnap of the harness's own setup problem. Checked
        // before the product failures so a run that never got off the ground cannot
        // be read as a defect.
        if (bootstrapFailed.Count > 0)
        {
            return new DisposableOsVerdict(
                DisposableOsVerdictKind.Unverified,
                $"the test environment could not be built, so nothing was measured: {string.Join(" | ", bootstrapFailed)}");
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
