using System.Text;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Engine;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The generated catalog status page: the renderer's shape, and the committed
/// copy staying in step with the catalog.
/// </summary>
public sealed class CatalogStatusPageTests
{
    private static readonly ScenarioCatalog Catalog = ReleaseCatalog.Create();

    [Fact]
    public void TheRenderedPageCoversEveryScenarioExactlyOnce()
    {
        var page = Render();

        Assert.StartsWith("# Release verification catalog\n", page, StringComparison.Ordinal);
        Assert.Contains(Catalog.Version, page, StringComparison.Ordinal);
        Assert.EndsWith("\n", page, StringComparison.Ordinal);

        // An LF file compared byte for byte: a stray carriage return would make the
        // drift check fail on one platform and pass on another.
        Assert.DoesNotContain('\r', page);

        foreach (var descriptor in Catalog.Descriptors)
        {
            Assert.Equal(1, Occurrences(page, $"| {descriptor.Id} |"));
        }
    }

    [Fact]
    public void TheMigratedAndRequiredColumnsAgreeWithTheCatalog()
    {
        var migrated = ReleaseCatalog.MigratedIds().ToHashSet(StringComparer.OrdinalIgnoreCase);
        var required = ReleaseCatalog.RequiredIds().ToHashSet(StringComparer.OrdinalIgnoreCase);

        var rows = Render()
            .Split('\n')
            .Where(line => line.StartsWith("| REL-", StringComparison.Ordinal))
            .ToList();

        Assert.Equal(Catalog.Descriptors.Count, rows.Count);

        foreach (var row in rows)
        {
            // "| ID | Class | Tier | Layer | Isolation | Privilege | Interaction | Requires | Oracle | Migrated | Required | Source |"
            var cells = row.Split('|').Select(cell => cell.Trim()).ToArray();
            var id = cells[1];

            Assert.Equal(migrated.Contains(id) ? "yes" : "no", cells[10]);
            Assert.Equal(required.Contains(id) ? "yes" : "no", cells[11]);
        }
    }

    [Fact]
    public void EveryOptInScenarioIsRenderedAsNotRequired()
    {
        var page = Render();

        foreach (var descriptor in Catalog.Descriptors.Where(descriptor => descriptor.OptIn))
        {
            var row = page.Split('\n').Single(line => line.StartsWith($"| {descriptor.Id} |", StringComparison.Ordinal));
            var cells = row.Split('|').Select(cell => cell.Trim()).ToArray();

            Assert.Equal("no", cells[11]);
        }
    }

    /// <summary>
    /// The committed page is a generated artifact. This regenerates it when
    /// <c>EXOSNAP_WRITE_VERIFY_FIXTURES=1</c> and otherwise fails when it has
    /// drifted from the catalog, the same contract the sample qualification record
    /// is held to.
    /// </summary>
    [Fact]
    public void TheCommittedCatalogPageMatchesTheCatalog()
    {
        var repositoryRoot = FindRepositoryRoot();
        if (repositoryRoot is null)
        {
            Assert.Skip("could not locate the repository root (no AGENTS.md above the test output directory)");
            return;
        }

        var pagePath = Path.Combine(
            repositoryRoot,
            CatalogStatusPage.RelativePath.Replace('/', Path.DirectorySeparatorChar));
        var rendered = Render();

        if (string.Equals(
            Environment.GetEnvironmentVariable("EXOSNAP_WRITE_VERIFY_FIXTURES"), "1", StringComparison.Ordinal))
        {
            File.WriteAllText(pagePath, rendered, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            return;
        }

        if (!File.Exists(pagePath))
        {
            Assert.Skip(
                $"the committed catalog page is not present at '{pagePath}' and " +
                "EXOSNAP_WRITE_VERIFY_FIXTURES=1 was not set to generate it");
            return;
        }

        var committed = File.ReadAllText(pagePath).Replace("\r\n", "\n", StringComparison.Ordinal);
        Assert.True(
            committed == rendered,
            $"'{pagePath}' is stale. Regenerate it with 'cd tools/release-verify && " +
            $"dotnet run --project ExoSnap.Verify -- catalog --out ../../{CatalogStatusPage.RelativePath}'.");
    }

    private static string Render() =>
        CatalogStatusPage.Render(Catalog, ReleaseCatalog.MigratedIds(), ReleaseCatalog.RequiredIds());

    private static int Occurrences(string haystack, string needle)
    {
        var count = 0;
        for (var index = haystack.IndexOf(needle, StringComparison.Ordinal);
             index >= 0;
             index = haystack.IndexOf(needle, index + needle.Length, StringComparison.Ordinal))
        {
            count++;
        }

        return count;
    }

    private static string? FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "AGENTS.md")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        return null;
    }
}
