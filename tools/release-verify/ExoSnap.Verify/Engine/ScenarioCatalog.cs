using System.Collections.ObjectModel;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Engine;

/// <summary>
/// A scenario catalog is broken: two scenarios share an id, or a dependency
/// cannot be resolved.
/// </summary>
public sealed class CatalogException : Exception
{
    /// <summary>Creates the exception with a default message.</summary>
    public CatalogException()
        : base("The scenario catalog is not well formed.")
    {
    }

    /// <summary>Creates the exception with a message.</summary>
    public CatalogException(string message)
        : base(message)
    {
    }

    /// <summary>Creates the exception with a message and a cause.</summary>
    public CatalogException(string message, Exception? innerException)
        : base(message, innerException)
    {
    }
}

/// <summary>
/// Every scenario the harness knows, in a dependency-respecting order.
/// </summary>
/// <remarks>
/// The catalog carries a version derived from the descriptors themselves rather
/// than from a number somebody remembers to increment. That is what makes
/// staleness detectable: a recorded verdict whose catalog version no longer
/// matches describes a scenario that may since have changed what it asserts.
/// </remarks>
public sealed class ScenarioCatalog
{
    private readonly Dictionary<string, Scenario> byId;

    /// <summary>Builds a catalog, validating ids and dependencies.</summary>
    /// <exception cref="CatalogException">
    /// An id is duplicated, a dependency does not exist, or the dependencies form
    /// a cycle.
    /// </exception>
    public ScenarioCatalog(IEnumerable<Scenario> scenarios)
    {
        ArgumentNullException.ThrowIfNull(scenarios);

        var declared = scenarios.ToList();
        this.byId = new Dictionary<string, Scenario>(StringComparer.OrdinalIgnoreCase);
        foreach (var scenario in declared)
        {
            if (!this.byId.TryAdd(scenario.Id, scenario))
            {
                throw new CatalogException($"Scenario id '{scenario.Id}' appears more than once.");
            }
        }

        this.Scenarios = Order(declared, this.byId);
        this.Version = ComputeVersion(this.Scenarios);
    }

    /// <summary>Every scenario, ordered so a dependency always precedes its dependent.</summary>
    public ReadOnlyCollection<Scenario> Scenarios { get; }

    /// <summary>Digest over the descriptors, used to detect a catalog that has moved.</summary>
    public string Version { get; }

    /// <summary>The descriptors, in the same order as <see cref="Scenarios"/>.</summary>
    public ReadOnlyCollection<ScenarioDescriptor> Descriptors =>
        new([.. this.Scenarios.Select(scenario => scenario.Descriptor)]);

    /// <summary>Looks a scenario up by id.</summary>
    public Scenario? Find(string id) =>
        this.byId.TryGetValue(id, out var scenario) ? scenario : null;

    /// <summary>The catalog as the document written next to a run.</summary>
    public ScenarioDescriptorDocument ToDocument() =>
        new(ScenarioDescriptorDocument.CurrentSchemaVersion, this.Version, this.Descriptors);

    // A stable topological sort: the catalog's own order is preserved except
    // where a dependency forces a scenario later. A level-order sort would be
    // just as correct and would scatter the catalog's reading order, which is
    // what a person compares a run against.
    private static ReadOnlyCollection<Scenario> Order(List<Scenario> declared, Dictionary<string, Scenario> byId)
    {
        var remaining = declared;
        var emitted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var ordered = new List<Scenario>(remaining.Count);

        foreach (var scenario in remaining)
        {
            foreach (var dependency in scenario.Descriptor.DependsOn)
            {
                if (!byId.ContainsKey(dependency))
                {
                    throw new CatalogException(
                        $"Scenario '{scenario.Id}' depends on '{dependency}', which is not in the catalog.");
                }
            }
        }

        while (remaining.Count > 0)
        {
            var next = remaining.FindIndex(scenario => scenario.Descriptor.DependsOn.All(emitted.Contains));
            if (next < 0)
            {
                var stuck = string.Join(", ", remaining.Select(scenario => scenario.Id).Order(StringComparer.Ordinal));
                throw new CatalogException($"The scenario dependencies form a cycle among: {stuck}.");
            }

            ordered.Add(remaining[next]);
            emitted.Add(remaining[next].Id);
            remaining.RemoveAt(next);
        }

        return new ReadOnlyCollection<Scenario>(ordered);
    }

    private static string ComputeVersion(IEnumerable<Scenario> scenarios)
    {
        var builder = new StringBuilder();
        foreach (var scenario in scenarios)
        {
            builder.Append(
                JsonSerializer.Serialize(
                    scenario.Descriptor,
                    VerifyJsonContext.Default.ScenarioDescriptor));
            builder.Append('\n');
        }

        var digest = SHA256.HashData(Encoding.UTF8.GetBytes(builder.ToString()));
        return Convert.ToHexString(digest)[..16].ToLower(CultureInfo.InvariantCulture);
    }
}
