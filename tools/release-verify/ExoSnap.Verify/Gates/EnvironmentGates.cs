using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-ENV-001: every environment property the orchestrator knows is classified, and
/// every claim it makes names the mechanism behind it.
/// </summary>
/// <remarks>
/// Asserted over the catalogue, not over the properties this machine happens to have
/// bound. The projection onto bound aliases is empty on a machine with no alias
/// profile, and an empty list satisfies "every entry is classified" vacuously - which
/// once reported a pass for a classification nobody had made.
/// </remarks>
public sealed class EnvironmentClassificationGate : IScenarioBody
{
    /// <summary>The classes a catalogue entry is allowed to carry.</summary>
    public static readonly string[] KnownClasses =
    [
        "ENV_READ", "ENV_MUTATE_SAFE", "ENV_MUTATE_TESTONLY", "ENV_HUMAN", "PHYSICAL", "SECURE", "UNAVAILABLE",
    ];

    private static readonly string[] MutableClasses = ["ENV_MUTATE_SAFE", "ENV_MUTATE_TESTONLY"];

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Envctl.Available)
        {
            return ScenarioResult.Unavailable("exosnap-envctl is not built");
        }

        var described = await services.Envctl.DescribeAsync(cancellationToken).ConfigureAwait(false);
        var evidence = GateEvidence.SaveJson(context, "capabilities.json", described.Body);

        var catalogue = described.Array("catalogue").ToList();
        if (catalogue.Count == 0)
        {
            return ScenarioResult.Fail(
                "the capability catalogue is empty; no environment property is classified at all", evidence);
        }

        var unclassified = catalogue
            .Where(entry => !KnownClasses.Contains(entry.OptionalString("capability"), StringComparer.Ordinal))
            .ToList();
        if (unclassified.Count > 0)
        {
            return ScenarioResult.Fail(
                $"{Count(unclassified.Count)} catalogue entries carry no recognised capability class", evidence);
        }

        // Every entry must also name how it is read, and a mutable one how it is
        // mutated. ENV_MUTATE_SAFE with no named mechanism is a claim with nothing
        // behind it, which is precisely what the class is supposed to rule out.
        var mechanismless = catalogue.Where(entry =>
        {
            var capability = entry.OptionalString("capability");
            return string.IsNullOrWhiteSpace(entry.OptionalString("readMechanism")) ||
                   (MutableClasses.Contains(capability, StringComparer.Ordinal) &&
                    string.IsNullOrWhiteSpace(entry.OptionalString("mutateMechanism")));
        }).ToList();

        if (mechanismless.Count > 0)
        {
            return ScenarioResult.Fail(
                $"{Count(mechanismless.Count)} catalogue entries name no mechanism for what they claim", evidence);
        }

        var mutable = catalogue.Count(entry =>
            string.Equals(entry.OptionalString("capability"), "ENV_MUTATE_SAFE", StringComparison.Ordinal));

        return ScenarioResult.Pass(
            $"{Count(catalogue.Count)} properties classified, {Count(mutable)} safely mutable", evidence);
    }

    private static string Count(int value) => value.ToString(CultureInfo.InvariantCulture);
}

/// <summary>
/// REL-ENV-002: device aliases resolve to stable identifiers, and no alias resolves
/// to more than one device.
/// </summary>
/// <remarks>
/// An ambiguous alias is a hard failure of the profile, not a warning: silently
/// picking one of two matching devices is exactly the behaviour that makes a suite lie
/// about which hardware it exercised.
/// </remarks>
public sealed class DeviceAliasGate : IScenarioBody
{
    private static readonly string[] BoundStatuses = ["ok", "friendly_name_changed"];

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Envctl.Available)
        {
            return ScenarioResult.Unavailable("exosnap-envctl is not built");
        }

        var aliases = await services.Envctl.ResolveAliasesAsync(cancellationToken).ConfigureAwait(false);
        var evidence = GateEvidence.SaveJson(context, "aliases.json", aliases.Body);

        var bindings = aliases.Array("bindings").ToList();
        var errors = aliases.Array("errors").ToList();

        var ambiguous = errors
            .Where(error => string.Equals(error.OptionalString("code"), "ambiguous_device", StringComparison.Ordinal))
            .Concat(bindings.Where(binding =>
                string.Equals(binding.OptionalString("status"), "ambiguous_device", StringComparison.Ordinal)))
            .Select(entry => entry.OptionalString("alias"))
            .ToList();

        if (ambiguous.Count > 0)
        {
            return ScenarioResult.Fail($"ambiguous_device for: {string.Join(", ", ambiguous)}", evidence);
        }

        var bound = bindings
            .Count(binding => BoundStatuses.Contains(binding.OptionalString("status"), StringComparer.Ordinal));
        var unbound = bindings.Count(binding =>
            string.Equals(binding.OptionalString("status"), "device_not_present", StringComparison.Ordinal)) +
            errors.Count;

        // No bindings and no errors means there is no alias profile on this machine
        // yet: nothing was resolved, so nothing was proven. A pass over an empty list
        // would claim the alias model works on a machine that has never used it.
        if (bound == 0 && unbound == 0)
        {
            var candidates = aliases.Array("candidates").Count();
            return ScenarioResult.Unavailable(
                "no alias profile on this machine; bind one with exosnap-envctl bind-alias " +
                $"({candidates.ToString(CultureInfo.InvariantCulture)} device(s) enumerated as candidates)");
        }

        return ScenarioResult.Pass(
            $"{bound.ToString(CultureInfo.InvariantCulture)} alias(es) bound and present; " +
            $"{unbound.ToString(CultureInfo.InvariantCulture)} not on this machine",
            evidence);
    }
}

/// <summary>
/// REL-ENV-003: a real mutation is applied, verified by a read-back, and restored
/// exactly - and it touches nothing else on the way.
/// </summary>
/// <remarks>
/// Reaching the body at all means the applied state was already read back and
/// compared by the tool, so what is asserted here is the other half: that the
/// mutation was minimal. A refresh-rate change that moved a second property changed
/// something nobody asked it to.
///
/// The rate is chosen against the machine rather than hardcoded, because Windows
/// enumerates the nominal and the actual rate of one physical mode separately and
/// collapses them on apply.
/// </remarks>
public sealed class EnvironmentMutationGate : IScenarioBody
{
    /// <summary>The alias whose refresh rate the gate moves.</summary>
    public const string DisplayAlias = "display.main-hdr";

    /// <summary>The property the transaction is written against.</summary>
    public const string RefreshProperty = DisplayAlias + ":refresh-hz";

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Envctl.Available)
        {
            return ScenarioResult.Unavailable("exosnap-envctl is not built");
        }

        var listed = await services.Envctl.ListModesAsync(DisplayAlias, cancellationToken).ConfigureAwait(false);
        if (!listed.Ok)
        {
            return ScenarioResult.Unavailable("the display modes could not be enumerated");
        }

        var display = listed.Array("displays").FirstOrDefault();
        if (display.ValueKind != JsonValueKind.Object)
        {
            return ScenarioResult.Unavailable($"no display resolved for {DisplayAlias}");
        }

        var offered = Snapshots.Items(display, "modes")
            .Select(mode => Snapshots.Number(mode, "refreshHz"))
            .Where(rate => rate is not null)
            .Select(rate => (int)rate!.Value)
            .ToList();
        var current = (int)(Snapshots.Number(display, "current.refreshHz") ?? 0);

        var target = EnvironmentOrchestrator.SelectUntwinnedRefreshRate(offered, current);
        if (target is null)
        {
            return ScenarioResult.Unavailable(
                "this display enumerates no alternative refresh rate a read-back could confirm");
        }

        var desired = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            [RefreshProperty] = target.Value.ToString(CultureInfo.InvariantCulture),
        };

        var outcome = await services.Environment
            .RunAsync(context.Descriptor.Id, desired, (begun, _) => Task.FromResult(Judge(context, begun)), cancellationToken)
            .ConfigureAwait(false);

        return GateOutcome.Of(outcome);
    }

    private static ScenarioResult Judge(ScenarioContext context, JsonElement begun)
    {
        var evidence = GateEvidence.SaveJson(context, "transaction.json", begun);
        var applied = Snapshots.Items(begun, "applied");

        if (applied.Count != 1)
        {
            return ScenarioResult.Fail(
                $"A refresh-rate change touched {applied.Count.ToString(CultureInfo.InvariantCulture)} properties; it must touch exactly one",
                evidence);
        }

        return ScenarioResult.Pass(
            $"refresh {Snapshots.Text(applied[0], "from")} -> {Snapshots.Text(applied[0], "to")}, verified by read-back",
            evidence);
    }
}

/// <summary>Turns an environment transaction into the scenario's verdict.</summary>
public static class GateOutcome
{
    /// <summary>
    /// The verdict a finished transaction produces.
    /// </summary>
    /// <remarks>
    /// The split this exists for: Fail means ExoSnap is wrong, and nothing else may
    /// claim it. A body that threw, a setup that never reached the desired state, and
    /// a scenario that returned no verdict at all measured nothing about the product,
    /// so none of them may be recorded as a defect.
    ///
    /// A restore that did not put the machine back is reported alongside the product
    /// verdict rather than replacing it: the campaign carries the restore verdicts
    /// separately, and a passing gate that left a display in the wrong mode still
    /// blocks a promotion.
    /// </remarks>
    public static ScenarioResult Of(EnvironmentTransactionOutcome outcome)
    {
        ArgumentNullException.ThrowIfNull(outcome);

        if (outcome.SetupErrorCode.Length > 0)
        {
            return outcome.SetupOutcome();
        }

        if (outcome.BodyError.Length > 0)
        {
            return ScenarioResult.InfrastructureError($"the scenario threw: {outcome.BodyError}");
        }

        return outcome.Product ?? ScenarioResult.InfrastructureError("the scenario returned no verdict");
    }
}
