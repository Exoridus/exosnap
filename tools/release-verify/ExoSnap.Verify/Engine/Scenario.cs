using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Engine;

/// <summary>What a scenario body is given when it runs.</summary>
/// <param name="Descriptor">The scenario's own declaration.</param>
/// <param name="Capabilities">What this machine reported at the start of the run.</param>
/// <param name="Processes">The runner every child process must go through.</param>
/// <param name="EvidenceDirectory">Directory the scenario writes its evidence files into.</param>
/// <param name="Services">
/// The adapters a migrated gate drives. Null for a run that measures nothing - a plan,
/// a catalog listing, or a scenario-logic test whose body needs none - so a gate that
/// reaches for one it was not given fails loudly rather than against a stub.
/// </param>
public sealed record ScenarioContext(
    ScenarioDescriptor Descriptor,
    CapabilitySet Capabilities,
    ProcessRunner Processes,
    string EvidenceDirectory,
    GateServices? Services = null)
{
    /// <summary>The adapters, or a refusal naming what the gate cannot reach.</summary>
    /// <exception cref="InvalidOperationException">This run supplied no adapters.</exception>
    public GateServices RequireServices() =>
        this.Services ??
        throw new InvalidOperationException(
            $"Scenario '{this.Descriptor.Id}' needs the verify adapters, and this run was created without them.");
}

/// <summary>The work a scenario does once its requirements are satisfied.</summary>
public interface IScenarioBody
{
    /// <summary>
    /// Carries the scenario out and reports what it concluded.
    /// </summary>
    /// <remarks>
    /// A body may return any outcome, but it should not catch its own
    /// infrastructure failures: an exception that escapes is turned into
    /// <see cref="ScenarioOutcome.InfrastructureError"/> by the engine, which is
    /// exactly the right verdict and one place fewer for a body to get it wrong.
    /// </remarks>
    Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken);
}

/// <summary>A declaration paired with the work it stands for.</summary>
/// <param name="Descriptor">Everything true about the scenario before it runs.</param>
/// <param name="Body">The work.</param>
public sealed record Scenario(ScenarioDescriptor Descriptor, IScenarioBody Body)
{
    /// <summary>The scenario's stable identifier.</summary>
    public string Id => this.Descriptor.Id;
}

/// <summary>
/// A body that has not been written yet.
/// </summary>
/// <remarks>
/// Exists so the catalog can be complete before the gates are migrated. It
/// reports <see cref="ScenarioOutcome.Skipped"/> with a reason, never a Pass:
/// a gate that has not been written must never look like a gate that ran.
/// </remarks>
public sealed class NotMigratedBody : IScenarioBody
{
    /// <summary>The reason every unmigrated scenario is reported with.</summary>
    public const string Reason = "not migrated";

    /// <inheritdoc/>
    public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken) =>
        Task.FromResult(ScenarioResult.Skipped(Reason));
}
