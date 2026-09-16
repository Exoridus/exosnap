using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// What the ported release catalog must keep true.
/// </summary>
public sealed class CatalogTests
{
    [Fact]
    public void TheCatalogIsWellFormedAndComplete()
    {
        var catalog = ReleaseCatalog.Create();

        // No fixed count. A number here is a line to bump whenever the catalog grows,
        // which says nothing about whether it grew correctly -- the invariants that
        // matter are below and in UnmigratedScenariosAreNamedNotCounted.
        Assert.NotEmpty(catalog.Scenarios);
        Assert.Equal(
            catalog.Scenarios.Count,
            catalog.Scenarios.Select(scenario => scenario.Id).Distinct(StringComparer.OrdinalIgnoreCase).Count());

        foreach (var descriptor in catalog.Descriptors)
        {
            Assert.False(string.IsNullOrWhiteSpace(descriptor.Title), $"{descriptor.Id} has no title");
            Assert.False(string.IsNullOrWhiteSpace(descriptor.Class), $"{descriptor.Id} has no class");
            Assert.False(string.IsNullOrWhiteSpace(descriptor.Source), $"{descriptor.Id} cites no source");
            Assert.NotEmpty(descriptor.Oracle);
        }
    }

    /// <summary>
    /// Every scenario has an executable body except the ones named here, with why.
    /// </summary>
    /// <remarks>
    /// A list rather than a count. "27 of 28 are migrated" is satisfied by migrating
    /// one and losing another, and it gives a reader nothing to act on; naming the
    /// exception forces the reason to be written down and makes finishing it visible
    /// as this list shrinking.
    /// </remarks>
    [Fact]
    public void UnmigratedScenariosAreNamedNotCounted()
    {
        // REL-AUD-DEGRADE-001 needs an audio endpoint to physically disappear
        // mid-recording. The envctl catalogue classifies endpoint-state as PHYSICAL
        // because no API causes it, and the typed harness has no operator gate to ask
        // a person through -- so faking it would be the harness verifying its own
        // fake. It is finished when that operator gate exists.
        string[] expected = ["REL-AUD-DEGRADE-001"];

        var migrated = ReleaseCatalog.MigratedIds();
        var declared = ReleaseCatalog.Descriptors()
            .Select(descriptor => descriptor.Id)
            .Where(id => !migrated.Contains(id, StringComparer.OrdinalIgnoreCase))
            .Order(StringComparer.Ordinal)
            .ToList();

        Assert.Equal(expected, declared);
    }

    [Fact]
    public void EveryScenarioTheChecklistCitesIsPresent()
    {
        var ids = ReleaseCatalog.Descriptors().Select(descriptor => descriptor.Id).ToHashSet(StringComparer.Ordinal);

        foreach (var id in new[]
                 {
                     "REL-ENV-001", "REL-ENV-002", "REL-ENV-003", "REL-SCHEMA-001",
                     "REL-PRESENT-001", "REL-PRESENT-002", "REL-PRESENT-XCHECK-001",
                     "REL-CAP-001", "REL-CAP-STALL-001", "REL-CAP-QUIET-001", "REL-CAP-FSE-001",
                     "REL-AUD-DEGRADE-001", "REL-AUD-SILENCE-001", "REL-AUD-FORMAT-001", "REL-AUD-CLOCK-001",
                     "REL-DISP-REFRESH-001", "REL-DISP-HDR-001", "REL-DISP-MIXED-001", "REL-DISP-DPI-001",
                     "REL-VIS-OVERLAY-001", "REL-VIS-NOTIFY-001",
                     "REL-UPD-PORTABLE-001", "REL-UPD-MSI-DECLINE-001", "REL-UPD-MSI-001",
                     "REL-PKG-CHOCO-001", "REL-JOURNEY-001", "REL-SHUTDOWN-001",
                 })
        {
            Assert.Contains(id, ids);
        }
    }

    /// <summary>
    /// A scenario is either migrated or declared, and the catalog agrees with itself
    /// about which. The point is the second half: an unmigrated body must report
    /// Skipped, never Pass, so a gate nobody has written can never look like a gate
    /// that ran.
    /// </summary>
    [Fact]
    public async Task AnUnmigratedScenarioReportsSkippedAndNeverPass()
    {
        var migrated = ReleaseCatalog.MigratedIds().ToHashSet(StringComparer.OrdinalIgnoreCase);
        Assert.NotEmpty(migrated);

        foreach (var scenario in ReleaseCatalog.Create().Scenarios)
        {
            if (migrated.Contains(scenario.Id))
            {
                Assert.IsNotType<NotMigratedBody>(scenario.Body);
                continue;
            }

            var body = Assert.IsType<NotMigratedBody>(scenario.Body);
            var result = await body.RunAsync(null!, TestContext.Current.CancellationToken);
            Assert.Equal(ScenarioOutcome.Skipped, result.Outcome);
            Assert.Equal(NotMigratedBody.Reason, result.Message);
        }
    }

    /// <summary>
    /// Every id the migration table names has a body, and every body has a
    /// declaration. A name in one list and not the other is a gate that either cannot
    /// run or cannot be selected.
    /// </summary>
    [Fact]
    public void EveryMigratedIdIsDeclaredInTheCatalog()
    {
        var declared = ReleaseCatalog.Descriptors().Select(descriptor => descriptor.Id).ToHashSet(StringComparer.Ordinal);

        foreach (var id in ReleaseCatalog.MigratedIds())
        {
            Assert.Contains(id, declared);
        }
    }

    [Fact]
    public void DependenciesAlwaysPrecedeTheirDependents()
    {
        var catalog = ReleaseCatalog.Create();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var scenario in catalog.Scenarios)
        {
            foreach (var dependency in scenario.Descriptor.DependsOn)
            {
                Assert.Contains(dependency, seen);
            }

            seen.Add(scenario.Id);
        }
    }

    [Fact]
    public void TheCatalogVersionChangesWhenADescriptorChanges()
    {
        var original = ReleaseCatalog.Create().Version;
        var modified = new ScenarioCatalog(
            ReleaseCatalog.Descriptors()
                .Select((descriptor, index) => new Scenario(
                    index == 0 ? descriptor with { Title = descriptor.Title + " (edited)" } : descriptor,
                    new NotMigratedBody())));

        Assert.NotEqual(original, modified.Version);
    }

    [Fact]
    public void RequiredIdsExcludeTheOptInScenarios()
    {
        var required = ReleaseCatalog.RequiredIds();
        var optIn = ReleaseCatalog.Descriptors()
            .Where(descriptor => descriptor.OptIn)
            .Select(descriptor => descriptor.Id);

        Assert.NotEmpty(required);
        foreach (var id in optIn)
        {
            Assert.DoesNotContain(id, required);
        }
    }

    [Fact]
    public void ADeviceRequirementIsWrittenAgainstAnAliasNotAProductName()
    {
        foreach (var requirement in ReleaseCatalog.Descriptors()
                     .SelectMany(descriptor => descriptor.Requires)
                     .Where(requirement => requirement.Key.StartsWith(
                         CapabilityKeys.DevicePrefix, StringComparison.Ordinal)))
        {
            Assert.Equal(CapabilityKeys.Bound, requirement.Value);
            Assert.DoesNotContain(' ', requirement.Key);
        }
    }

    [Fact]
    public void ADuplicateIdIsRejected()
    {
        var descriptor = ReleaseCatalog.Descriptors()[0];
        Assert.Throws<CatalogException>(() => new ScenarioCatalog(
        [
            new Scenario(descriptor, new NotMigratedBody()),
            new Scenario(descriptor, new NotMigratedBody()),
        ]));
    }

    [Fact]
    public void AnUnknownDependencyIsRejected()
    {
        var descriptor = ReleaseCatalog.Descriptors()[0] with
        {
            DependsOn = new System.Collections.ObjectModel.ReadOnlyCollection<string>(["REL-DOES-NOT-EXIST"]),
        };

        Assert.Throws<CatalogException>(() =>
            new ScenarioCatalog([new Scenario(descriptor, new NotMigratedBody())]));
    }

    [Fact]
    public void ADependencyCycleIsRejected()
    {
        var first = ReleaseCatalog.Descriptors()[0] with
        {
            DependsOn = new System.Collections.ObjectModel.ReadOnlyCollection<string>(["REL-ENV-002"]),
        };
        var second = ReleaseCatalog.Descriptors()[1] with
        {
            DependsOn = new System.Collections.ObjectModel.ReadOnlyCollection<string>([first.Id]),
        };

        var thrown = Assert.Throws<CatalogException>(() => new ScenarioCatalog(
        [
            new Scenario(first, new NotMigratedBody()),
            new Scenario(second, new NotMigratedBody()),
        ]));

        Assert.Contains("cycle", thrown.Message, StringComparison.Ordinal);
    }
}
