using System.Collections.ObjectModel;
using System.Text.Json;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;
using Qualify = ExoSnap.Verify.Engine.Qualification;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// What a qualification record may and may not say.
/// </summary>
public sealed class QualificationTests
{
    private static ScenarioVerdict Verdict(string id, ScenarioOutcome outcome) =>
        new(id, outcome, outcome.ToString(), 1, new ReadOnlyCollection<Evidence>([]));

    private static readonly MachineFingerprint Machine =
        new("fingerprint", "Windows", new ReadOnlyCollection<string>(["adapter"]));

    private static QualificationRecord Build(
        IReadOnlyCollection<ScenarioVerdict> verdicts,
        IReadOnlyCollection<string> required,
        IReadOnlyCollection<EnvironmentRestoreVerdict>? restores = null) =>
        Qualify.Build(
            "v0.9.1-rc1",
            "abc123",
            [new ArtifactDigest("ExoSnap.msi", "deadbeef")],
            Machine,
            new Dictionary<string, string> { ["elevated"] = "false" },
            "catalog-1",
            verdicts,
            required,
            restores ?? []);

    [Fact]
    public void EveryRequiredScenarioPassingQualifies()
    {
        var record = Build([Verdict("A", ScenarioOutcome.Pass)], ["A"]);
        Assert.Equal(QualificationOutcome.Qualified, record.Overall);
    }

    [Theory]
    [InlineData(ScenarioOutcome.Fail)]
    [InlineData(ScenarioOutcome.InfrastructureError)]
    [InlineData(ScenarioOutcome.Blocked)]
    [InlineData(ScenarioOutcome.Unavailable)]
    [InlineData(ScenarioOutcome.Deferred)]
    [InlineData(ScenarioOutcome.Skipped)]
    [InlineData(ScenarioOutcome.Stale)]
    public void AnythingButAPassOnARequiredScenarioRefuses(ScenarioOutcome outcome)
    {
        var record = Build([Verdict("A", outcome)], ["A"]);
        Assert.Equal(QualificationOutcome.NotQualified, record.Overall);
    }

    [Fact]
    public void ARequiredScenarioWithNoVerdictAtAllRefuses()
    {
        var record = Build([Verdict("A", ScenarioOutcome.Pass)], ["A", "B"]);

        Assert.Equal(QualificationOutcome.NotQualified, record.Overall);
        Assert.Contains(
            Qualify.Objections(record.Verdicts, ["A", "B"], []),
            objection => objection.Contains("no verdict", StringComparison.Ordinal));
    }

    [Fact]
    public void AnInfrastructureErrorAnywhereRefusesEvenWhenNotRequired()
    {
        var record = Build(
            [Verdict("A", ScenarioOutcome.Pass), Verdict("B", ScenarioOutcome.InfrastructureError)],
            ["A"]);

        Assert.Equal(QualificationOutcome.NotQualified, record.Overall);
    }

    [Fact]
    public void AnOptInScenarioThatWasNotRunDoesNotRefuse()
    {
        var record = Build(
            [Verdict("A", ScenarioOutcome.Pass), Verdict("B", ScenarioOutcome.Skipped)],
            ["A"]);

        Assert.Equal(QualificationOutcome.Qualified, record.Overall);
    }

    [Fact]
    public void AnEnvironmentPropertyLeftChangedRefuses()
    {
        var record = Build(
            [Verdict("A", ScenarioOutcome.Pass)],
            ["A"],
            [new EnvironmentRestoreVerdict("display.main-hdr:hdr", "off", "on", false)]);

        Assert.Equal(QualificationOutcome.NotQualified, record.Overall);
        Assert.Contains(
            Qualify.Objections(record.Verdicts, ["A"], record.EnvironmentRestores),
            objection => objection.Contains("display.main-hdr:hdr", StringComparison.Ordinal));
    }

    [Fact]
    public void TheArtifactFingerprintIsOrderIndependentAndContentBound()
    {
        var one = Qualify.ArtifactFingerprint(
            [new ArtifactDigest("a.msi", "1111"), new ArtifactDigest("b.zip", "2222")]);
        var reversed = Qualify.ArtifactFingerprint(
            [new ArtifactDigest("b.zip", "2222"), new ArtifactDigest("a.msi", "1111")]);
        var changed = Qualify.ArtifactFingerprint(
            [new ArtifactDigest("a.msi", "1111"), new ArtifactDigest("b.zip", "3333")]);

        Assert.Equal(one, reversed);
        Assert.NotEqual(one, changed);
    }

    [Fact]
    public void TheRecordSerializesAsCamelCaseWithNamedOutcomes()
    {
        var record = Build([Verdict("REL-CAP-001", ScenarioOutcome.Pass)], ["REL-CAP-001"]);
        var json = VerifyJson.Serialize(record, VerifyJsonContext.Default.QualificationRecord);

        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;

        Assert.Equal("v0.9.1-rc1", root.GetProperty("rcTag").GetString());
        Assert.Equal("abc123", root.GetProperty("sourceCommitSha").GetString());
        Assert.Equal("Qualified", root.GetProperty("overall").GetString());
        Assert.Equal("Pass", root.GetProperty("verdicts")[0].GetProperty("outcome").GetString());
        Assert.Equal("deadbeef", root.GetProperty("artifacts")[0].GetProperty("sha256").GetString());
        Assert.True(root.TryGetProperty("machineFingerprintPlaceholder", out _) is false);
        Assert.Equal("fingerprint", root.GetProperty("machine").GetProperty("fingerprint").GetString());
        Assert.Equal("catalog-1", root.GetProperty("catalogVersion").GetString());
        Assert.True(root.TryGetProperty("capabilities", out _));
        Assert.True(root.TryGetProperty("environmentRestores", out _));
        Assert.True(root.TryGetProperty("evidenceHashes", out _));
        Assert.True(root.TryGetProperty("timestampUtc", out _));
        Assert.True(root.TryGetProperty("harness", out _));
    }

    [Fact]
    public void EvidenceIsBoundToTheContentOfTheFileItNames()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-evidence");
        var path = Path.Combine(directory.Path, "log.txt");
        File.WriteAllText(path, "first");

        var first = Evidence.ForFile("log", path);
        File.WriteAllText(path, "second");
        var second = Evidence.ForFile("log", path);

        Assert.NotEqual(first.Sha256, second.Sha256);
        Assert.Equal(64, first.Sha256.Length);
        Assert.Equal(first.Sha256, first.Sha256.ToLowerInvariant());
    }

    [Fact]
    public void EvidenceForAMissingFileIsRefusedRatherThanRecordedEmpty()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-evidence-missing");
        Assert.Throws<FileNotFoundException>(() =>
            Evidence.ForFile("log", Path.Combine(directory.Path, "absent.txt")));
    }
}
