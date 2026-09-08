using System.Text.Json;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The release-verification record: the digest that binds it to a catalog, the
/// rules that refuse to let it qualify a release, and its exact written shape.
/// </summary>
public sealed class ReleaseVerificationRecordTests
{
    private static CampaignBinding Binding() =>
        new("run-1", "v0.9.1-rc1", "abc123", @"C:\exosnap.exe", "0.9.1", "deadbeef", InstallTree: true);

    private static ReleasePackage[] Packages() => [new ReleasePackage("ExoSnap.zip", "deadbeef", 100)];

    private static RecordedCheck Check(
        string id,
        string state = "PASS",
        bool required = true,
        bool optIn = false,
        string restoreResult = "NOT_APPLICABLE",
        IReadOnlyList<Evidence>? evidence = null) =>
        new(id, $"{id} title", "FullAuto", state, required, optIn, "message", 1, restoreResult, evidence ?? [], Attempted: true);

    [Fact]
    public void CatalogDigestIsOrderIndependent()
    {
        var descriptors = ReleaseCatalog.Descriptors();
        var forward = ReleaseVerificationRecord.CatalogDigest(descriptors);
        var reversed = ReleaseVerificationRecord.CatalogDigest(descriptors.Reverse());

        Assert.Equal(forward, reversed);
    }

    [Fact]
    public void CatalogDigestIsStableAcrossIdenticalCalls()
    {
        var descriptors = ReleaseCatalog.Descriptors();
        Assert.Equal(
            ReleaseVerificationRecord.CatalogDigest(descriptors),
            ReleaseVerificationRecord.CatalogDigest(descriptors));
    }

    [Fact]
    public void CatalogDigestChangesWhenAScenariosOptInFlagFlips()
    {
        var descriptors = ReleaseCatalog.Descriptors();
        var before = ReleaseVerificationRecord.CatalogDigest(descriptors);

        var flipped = descriptors.Select((descriptor, index) =>
            index == 0 ? descriptor with { OptIn = !descriptor.OptIn } : descriptor);
        var after = ReleaseVerificationRecord.CatalogDigest(flipped);

        Assert.NotEqual(before, after);
    }

    [Theory]
    [InlineData("runId")]
    [InlineData("rcTag")]
    [InlineData("sourceCommit")]
    public void BlockersFlagsAnEmptyBindingField(string field)
    {
        var binding = field switch
        {
            "runId" => Binding() with { RunId = string.Empty },
            "rcTag" => Binding() with { RcTag = string.Empty },
            _ => Binding() with { SourceCommit = string.Empty },
        };

        var reasons = ReleaseVerificationRecord.Blockers(
            binding, "fingerprint", "harness-commit", "catalog-digest", Packages(), [Check("A")]);

        Assert.Contains(reasons, reason => reason.Contains($"'{field}' is empty", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsAnEmptyMachineFingerprint()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), string.Empty, "harness-commit", "catalog-digest", Packages(), [Check("A")]);

        Assert.Contains(reasons, r => r.Contains("'machineFingerprint' is empty", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsAnEmptyHarnessCommit()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", string.Empty, "catalog-digest", Packages(), [Check("A")]);

        Assert.Contains(reasons, r => r.Contains("'harness.commit' is empty", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsAnEmptyCatalogDigest()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", string.Empty, Packages(), [Check("A")]);

        Assert.Contains(reasons, r => r.Contains("'catalog.digest' is empty", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsNoPackageDigestAtAll()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest",
            [new ReleasePackage("a.zip", string.Empty, 1)], [Check("A")]);

        Assert.Contains(reasons, r => r.Contains("no release package SHA-256", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsNoChecksAtAll()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(), []);

        Assert.Contains(reasons, r => r.Contains("no scenario verdicts", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsAFail()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(), [Check("A", state: "FAIL")]);

        Assert.Contains(reasons, r => r.Contains("product defect (FAIL): A", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsAnOptionalFail()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(),
            [Check("A"), Check("OPT", state: "FAIL", required: false, optIn: true)]);

        Assert.Contains(reasons, r => r.Contains("product defect (FAIL): OPT", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsAnInfrastructureError()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(),
            [Check("A", state: "INFRA_ERROR")]);

        Assert.Contains(reasons, r => r.Contains("INFRA_ERROR", StringComparison.Ordinal) && r.Contains('A'));
    }

    [Fact]
    public void BlockersFlagsARequiredGateThatIsUnavailable()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(),
            [Check("A", state: "UNAVAILABLE")]);

        Assert.Contains(reasons, r => r.Contains("required gate A is UNAVAILABLE", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsARestoreThatIsNeitherRestoredNorNotApplicable()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(),
            [Check("A", restoreResult: "RESTORE_FAILED")]);

        Assert.Contains(
            reasons,
            r => r.Contains("A left the machine misconfigured: environment restore RESTORE_FAILED", StringComparison.Ordinal));
    }

    [Fact]
    public void BlockersFlagsCitedEvidenceThatIsNotOnDisk()
    {
        var missing = new Evidence(
            "log",
            Path.Combine(Path.GetTempPath(), "exosnap-verify-tests", "no-such-file-" + Guid.NewGuid().ToString("N") + ".txt"),
            "deadbeef");

        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(),
            [Check("A", evidence: [missing])]);

        Assert.Contains(reasons, r => r.Contains("A cites evidence that is not there: log", StringComparison.Ordinal));
    }

    [Fact]
    public void ACleanRecordHasNoBlockers()
    {
        var reasons = ReleaseVerificationRecord.Blockers(
            Binding(), "fingerprint", "harness-commit", "catalog-digest", Packages(), [Check("A")]);

        Assert.Empty(reasons);
    }

    [Fact]
    public void WriteProducesAQualifiedRecordWhenNothingBlocks()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-clean-record");
        var path = Path.Combine(directory.Path, "release-verification.json");

        var blockers = ReleaseVerificationRecord.Write(
            path,
            Binding(),
            machineFingerprint: "fingerprint",
            harnessVersion: "1.0.0",
            harnessCommit: "harness-commit",
            harnessDirty: false,
            catalogVersion: "catalog-1",
            catalogDigest: "catalog-digest",
            catalogSize: 1,
            capabilities: new Dictionary<string, string>(),
            environment: new Dictionary<string, string>(),
            packages: Packages(),
            requiredIds: ["A"],
            namedOptIn: [],
            checks: [Check("A")],
            runDirectory: directory.Path);

        Assert.Empty(blockers);

        using var document = JsonDocument.Parse(File.ReadAllText(path));
        Assert.Equal(
            "QUALIFIED", document.RootElement.GetProperty("qualification").GetProperty("overall").GetString());
    }

    /// <summary>
    /// Writes (only when explicitly asked) or reads back the committed sample
    /// record, and either way confirms the C# producer's shape matches it: the
    /// top-level property names, the check-row property names, and the schema.
    /// </summary>
    /// <remarks>
    /// The write path is what regenerates <c>scripts/tests/fixtures/verify-harness/release-verification.sample.json</c>
    /// when the record's shape deliberately changes; it never runs on its own.
    /// </remarks>
    [Fact]
    public void WritesTheSampleRecordFixture()
    {
        var repositoryRoot = FindRepositoryRoot();
        if (repositoryRoot is null)
        {
            Assert.Skip("could not locate the repository root (no AGENTS.md above the test output directory)");
            return;
        }

        var fixturePath = Path.Combine(
            repositoryRoot, "scripts", "tests", "fixtures", "verify-harness", "release-verification.sample.json");
        var writeFixture = string.Equals(
            Environment.GetEnvironmentVariable("EXOSNAP_WRITE_VERIFY_FIXTURES"), "1", StringComparison.Ordinal);

        if (!writeFixture && !File.Exists(fixturePath))
        {
            Assert.Skip(
                $"the committed sample fixture is not present at '{fixturePath}' and " +
                "EXOSNAP_WRITE_VERIFY_FIXTURES=1 was not set to generate it");
            return;
        }

        using var directory = FixtureTool.NewTemporaryDirectory("-sample-record");
        var evidencePath = Path.Combine(directory.Path, "ffprobe.json");
        File.WriteAllText(evidencePath, """{"streams":[],"format":{}}""");

        RecordedCheck[] checks =
        [
            new(
                "REL-CAP-001",
                "A recording is produced and validated by an independent tool",
                "FullAuto",
                "PASS",
                Required: true,
                OptIn: false,
                "6.02s recorded, av1 codec, ffprobe agrees",
                6200,
                "NOT_APPLICABLE",
                [Evidence.ForFile("ffprobe", evidencePath)],
                Attempted: true),
        ];

        var outputPath = writeFixture ? fixturePath : Path.Combine(directory.Path, "release-verification.json");

        var blockers = ReleaseVerificationRecord.Write(
            outputPath,
            new CampaignBinding(
                "rel-20260908-000000",
                "v0.9.1-rc1",
                "0000000000000000000000000000000000000000",
                @"C:\fixture\exosnap.exe",
                "0.9.1",
                new string('a', 64),
                InstallTree: true),
            machineFingerprint: "fingerprint-" + new string('a', 16),
            harnessVersion: "1.0.0",
            harnessCommit: "0000000000000000000000000000000000000000",
            harnessDirty: false,
            catalogVersion: "catalog-1",
            catalogDigest: "catalog-digest-" + new string('a', 16),
            catalogSize: 1,
            capabilities: new Dictionary<string, string> { ["elevated"] = "false" },
            environment: new Dictionary<string, string> { ["osVersion"] = "10.0.26200" },
            packages: [new ReleasePackage("ExoSnap-0.9.1-rc1-windows-x64-portable.zip", new string('b', 64), 12345)],
            requiredIds: ["REL-CAP-001"],
            namedOptIn: [],
            checks: checks,
            runDirectory: directory.Path);

        Assert.Empty(blockers);

        using var document = JsonDocument.Parse(File.ReadAllText(outputPath));
        Assert.Equal(
            "QUALIFIED", document.RootElement.GetProperty("qualification").GetProperty("overall").GetString());

        if (writeFixture)
        {
            return;
        }

        using var fixtureDocument = JsonDocument.Parse(File.ReadAllText(fixturePath));

        Assert.Equal(PropertyNames(fixtureDocument.RootElement), PropertyNames(document.RootElement));
        Assert.Equal(
            fixtureDocument.RootElement.GetProperty("schema").GetString(),
            document.RootElement.GetProperty("schema").GetString());

        var fixtureCheck = fixtureDocument.RootElement.GetProperty("checks").EnumerateArray().First();
        var producedCheck = document.RootElement.GetProperty("checks").EnumerateArray().First();
        Assert.Equal(PropertyNames(fixtureCheck), PropertyNames(producedCheck));
    }

    private static List<string> PropertyNames(JsonElement element) =>
        [.. element.EnumerateObject().Select(property => property.Name).OrderBy(name => name, StringComparer.Ordinal)];

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
