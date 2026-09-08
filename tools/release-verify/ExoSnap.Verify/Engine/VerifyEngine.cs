using System.Collections.ObjectModel;
using System.Diagnostics;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Engine;

/// <summary>Which scenarios a run is about.</summary>
/// <param name="IncludeOptIn">Include scenarios marked opt-in, which are long or disruptive.</param>
/// <param name="Classes">Only these classes, or every class when empty.</param>
/// <param name="Ids">Only these scenario ids, or every id when empty.</param>
public sealed record ScenarioSelection(
    bool IncludeOptIn = false,
    IReadOnlyCollection<string>? Classes = null,
    IReadOnlyCollection<string>? Ids = null)
{
    /// <summary>Whether a descriptor is part of this selection.</summary>
    public bool Includes(ScenarioDescriptor descriptor)
    {
        ArgumentNullException.ThrowIfNull(descriptor);

        if (this.Ids is { Count: > 0 })
        {
            return this.Ids.Contains(descriptor.Id, StringComparer.OrdinalIgnoreCase);
        }

        if (descriptor.OptIn && !this.IncludeOptIn)
        {
            return false;
        }

        return this.Classes is not { Count: > 0 } ||
               this.Classes.Contains(descriptor.Class, StringComparer.OrdinalIgnoreCase);
    }
}

/// <summary>What a scenario would do, decided before anything runs.</summary>
/// <param name="Descriptor">The scenario.</param>
/// <param name="Outcome">The outcome the scenario already has, or null when it would run.</param>
/// <param name="Message">Why it already has one.</param>
public sealed record PlannedScenario(ScenarioDescriptor Descriptor, ScenarioOutcome? Outcome, string Message)
{
    /// <summary>Whether the scenario body would actually be invoked.</summary>
    public bool WouldRun => this.Outcome is null;
}

/// <summary>The whole plan for a run.</summary>
/// <param name="CatalogVersion">Digest of the catalog the plan was built from.</param>
/// <param name="Scenarios">Every scenario, in dependency order, with its predicted outcome.</param>
public sealed record RunPlan(string CatalogVersion, ReadOnlyCollection<PlannedScenario> Scenarios);

/// <summary>
/// Turns a catalog and a machine into a plan, and a plan into verdicts.
/// </summary>
/// <remarks>
/// The engine never lets a harness fault become a product verdict. An exception
/// escaping a scenario body, a dependency that did not pass, a machine that does
/// not satisfy a requirement: each has its own outcome, and none of them is
/// <see cref="ScenarioOutcome.Fail"/>.
/// </remarks>
public sealed class VerifyEngine
{
    private readonly ScenarioCatalog catalog;

    /// <summary>Creates an engine over a catalog.</summary>
    public VerifyEngine(ScenarioCatalog catalog)
    {
        ArgumentNullException.ThrowIfNull(catalog);
        this.catalog = catalog;
    }

    /// <summary>
    /// Decides what would happen, without running anything or touching the
    /// machine beyond the capabilities already measured.
    /// </summary>
    public RunPlan Plan(ScenarioSelection selection, CapabilitySet capabilities)
    {
        ArgumentNullException.ThrowIfNull(selection);
        ArgumentNullException.ThrowIfNull(capabilities);

        var planned = new List<PlannedScenario>(this.catalog.Scenarios.Count);
        var settled = new Dictionary<string, ScenarioOutcome?>(StringComparer.OrdinalIgnoreCase);

        foreach (var scenario in this.catalog.Scenarios)
        {
            var descriptor = scenario.Descriptor;
            PlannedScenario entry;

            if (!selection.Includes(descriptor))
            {
                entry = new PlannedScenario(
                    descriptor,
                    ScenarioOutcome.Skipped,
                    descriptor.OptIn ? "opt-in, not selected for this run" : "not selected for this run");
            }
            else if (BlockingDependency(descriptor, settled) is { } blocking)
            {
                entry = new PlannedScenario(
                    descriptor,
                    ScenarioOutcome.Blocked,
                    $"depends on {blocking}, which will not pass in this run");
            }
            else if (capabilities.FirstUnsatisfied(descriptor.Requires) is { } unsatisfied)
            {
                entry = new PlannedScenario(descriptor, ScenarioOutcome.Unavailable, unsatisfied.Message);
            }
            else
            {
                entry = new PlannedScenario(descriptor, null, "would run");
            }

            settled[descriptor.Id] = entry.Outcome;
            planned.Add(entry);
        }

        return new RunPlan(this.catalog.Version, new ReadOnlyCollection<PlannedScenario>(planned));
    }

    /// <summary>
    /// Runs the plan and returns one verdict per scenario, in plan order.
    /// </summary>
    public async Task<ReadOnlyCollection<ScenarioVerdict>> RunAsync(
        RunPlan plan,
        CapabilitySet capabilities,
        RunDirectory runDirectory,
        ProcessRunner processes,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(plan);
        ArgumentNullException.ThrowIfNull(capabilities);
        ArgumentNullException.ThrowIfNull(runDirectory);
        ArgumentNullException.ThrowIfNull(processes);

        var verdicts = new List<ScenarioVerdict>(plan.Scenarios.Count);
        var settled = new Dictionary<string, ScenarioOutcome>(StringComparer.OrdinalIgnoreCase);

        foreach (var planned in plan.Scenarios)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var descriptor = planned.Descriptor;

            ScenarioResult result;
            var stopwatch = Stopwatch.StartNew();

            if (planned.Outcome is { } predetermined)
            {
                result = new ScenarioResult(predetermined, planned.Message, []);
            }
            else if (FailedDependency(descriptor, settled) is { } failed)
            {
                result = ScenarioResult.Blocked($"depends on {failed}, which did not pass");
            }
            else
            {
                result = await this.RunOneAsync(descriptor, capabilities, runDirectory, processes, cancellationToken)
                    .ConfigureAwait(false);
            }

            stopwatch.Stop();
            settled[descriptor.Id] = result.Outcome;
            verdicts.Add(new ScenarioVerdict(
                descriptor.Id,
                result.Outcome,
                result.Message,
                (long)stopwatch.Elapsed.TotalMilliseconds,
                new ReadOnlyCollection<Evidence>(result.Evidence)));
        }

        return new ReadOnlyCollection<ScenarioVerdict>(verdicts);
    }

    private async Task<ScenarioResult> RunOneAsync(
        ScenarioDescriptor descriptor,
        CapabilitySet capabilities,
        RunDirectory runDirectory,
        ProcessRunner processes,
        CancellationToken cancellationToken)
    {
        var scenario = this.catalog.Find(descriptor.Id);
        if (scenario is null)
        {
            return ScenarioResult.InfrastructureError(
                $"the plan names '{descriptor.Id}', which the catalog does not contain");
        }

        try
        {
            var context = new ScenarioContext(
                descriptor,
                capabilities,
                processes,
                runDirectory.EvidenceDirectoryFor(descriptor.Id));
            return await scenario.Body.RunAsync(context, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
#pragma warning disable CA1031 // A harness fault must never escape as an unhandled crash and must never become a Fail.
        catch (Exception exception)
#pragma warning restore CA1031
        {
            return ScenarioResult.InfrastructureError(
                $"the scenario raised {exception.GetType().Name}: {exception.Message}");
        }
    }

    private static string? BlockingDependency(
        ScenarioDescriptor descriptor,
        Dictionary<string, ScenarioOutcome?> settled) =>
        descriptor.DependsOn.FirstOrDefault(
            dependency => settled.TryGetValue(dependency, out var outcome) && outcome is not null);

    private static string? FailedDependency(
        ScenarioDescriptor descriptor,
        Dictionary<string, ScenarioOutcome> settled) =>
        descriptor.DependsOn.FirstOrDefault(
            dependency => settled.TryGetValue(dependency, out var outcome) && outcome != ScenarioOutcome.Pass);
}
