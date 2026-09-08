using System.Collections.ObjectModel;
using System.Globalization;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Capabilities;

/// <summary>The result of comparing one requirement against this machine.</summary>
/// <param name="Requirement">The requirement that was evaluated.</param>
/// <param name="Observed">What the machine reported, or "unknown".</param>
/// <param name="Satisfied">Whether the requirement holds.</param>
public sealed record CapabilityEvaluation(CapabilityRequirement Requirement, string Observed, bool Satisfied)
{
    /// <summary>The message an unsatisfied requirement is reported with.</summary>
    public string Message => this.Requirement.UnsatisfiedMessage;
}

/// <summary>
/// What this machine can do, measured once at the start of a run.
/// </summary>
/// <remarks>
/// A key that is absent and a key whose value is <c>unknown</c> mean the same
/// thing and never satisfy a requirement. That asymmetry is deliberate: a
/// requirement that cannot be checked has not been met, and the scenario is
/// reported Unavailable rather than run against a guess.
/// </remarks>
public sealed class CapabilitySet
{
    private readonly Dictionary<string, string> values;
    private readonly Dictionary<string, string> sources;

    /// <summary>Builds a set from measured values and the mechanism behind each.</summary>
    public CapabilitySet(IReadOnlyDictionary<string, string> values, IReadOnlyDictionary<string, string> sources)
    {
        ArgumentNullException.ThrowIfNull(values);
        ArgumentNullException.ThrowIfNull(sources);
        this.values = new Dictionary<string, string>(values, StringComparer.OrdinalIgnoreCase);
        this.sources = new Dictionary<string, string>(sources, StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>Every measured value, ordered by key.</summary>
    public ReadOnlyDictionary<string, string> Values =>
        new(this.values.OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal));

    /// <summary>The mechanism behind each measured value, ordered by key.</summary>
    public ReadOnlyDictionary<string, string> Sources =>
        new(this.sources.OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal));

    /// <summary>
    /// The measured value for a key, or <see cref="CapabilityKeys.Unknown"/> when
    /// no probe answered it.
    /// </summary>
    public string this[string key] =>
        this.values.TryGetValue(key, out var value) ? value : CapabilityKeys.Unknown;

    /// <summary>Compares one requirement against this machine.</summary>
    public CapabilityEvaluation Evaluate(CapabilityRequirement requirement)
    {
        ArgumentNullException.ThrowIfNull(requirement);
        var observed = this[requirement.Key];

        if (string.Equals(observed, CapabilityKeys.Unknown, StringComparison.OrdinalIgnoreCase))
        {
            return new CapabilityEvaluation(requirement, observed, false);
        }

        var satisfied = requirement.Operator switch
        {
            CapabilityOperator.Equals =>
                string.Equals(observed, requirement.Value, StringComparison.OrdinalIgnoreCase),
            CapabilityOperator.NotEquals =>
                !string.Equals(observed, requirement.Value, StringComparison.OrdinalIgnoreCase),
            CapabilityOperator.AtLeast => AtLeast(observed, requirement.Value),
            _ => false,
        };

        return new CapabilityEvaluation(requirement, observed, satisfied);
    }

    /// <summary>
    /// The first requirement this machine does not satisfy, or null when it
    /// satisfies all of them.
    /// </summary>
    public CapabilityEvaluation? FirstUnsatisfied(IEnumerable<CapabilityRequirement> requirements)
    {
        ArgumentNullException.ThrowIfNull(requirements);
        foreach (var requirement in requirements)
        {
            var evaluation = this.Evaluate(requirement);
            if (!evaluation.Satisfied)
            {
                return evaluation;
            }
        }

        return null;
    }

    private static bool AtLeast(string observed, string required) =>
        double.TryParse(observed, NumberStyles.Float, CultureInfo.InvariantCulture, out var left) &&
        double.TryParse(required, NumberStyles.Float, CultureInfo.InvariantCulture, out var right) &&
        left >= right;
}
