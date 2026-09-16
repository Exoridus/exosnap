using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Binding a verdict to what it was measured with, not only to what it measured.
/// </summary>
/// <remarks>
/// The artifact bytes were already bound and the rest was not: the same executable
/// measured by a different ffprobe, on a different Windows build or through a
/// different display driver is a different measurement, and reusing the older verdict
/// is how a local pass from before a driver update qualifies a release nobody re-ran.
/// </remarks>
public sealed class ToolingFingerprintTests
{
    private static ToolingFingerprint Measured => new("26100.4061", "32.0.15.8098", "9.0.1", "2.5.1");

    [Fact]
    public void TheSameToolsOnTheSameMachineProduceTheSameDigest()
    {
        Assert.Equal(Measured.Digest, Measured.Digest);
        Assert.NotEmpty(Measured.Digest);
    }

    [Theory]
    [InlineData("OsBuild")]
    [InlineData("GpuDriver")]
    [InlineData("Ffprobe")]
    [InlineData("PresentMon")]
    public void ChangingAnyOneOfThemChangesTheDigest(string changed)
    {
        var other = changed switch
        {
            "OsBuild" => Measured with { OsBuild = "27000.1" },
            "GpuDriver" => Measured with { GpuDriver = "33.0.0.1" },
            "Ffprobe" => Measured with { Ffprobe = "9.1.0" },
            _ => Measured with { PresentMon = "2.6.0" },
        };

        Assert.NotEqual(Measured.Digest, other.Digest);
        Assert.False(other.Accepts(Measured.Digest));
    }

    [Fact]
    public void AFingerprintWithAnUnknownFieldHasNoDigestAtAll()
    {
        // Not a digest over the word "unknown": that would compare equal to the next
        // equally ignorant run and let exactly the reuse this prevents happen.
        Assert.Empty((Measured with { Ffprobe = ToolingFingerprint.Unknown }).Digest);
        Assert.Empty(ToolingFingerprint.Nothing.Digest);
    }

    [Fact]
    public void ARunThatCannotSayWhatItMeasuresWithAcceptsNothing()
    {
        Assert.False(ToolingFingerprint.Nothing.Accepts(Measured.Digest));
        Assert.Contains("cannot say", ToolingFingerprint.Nothing.DescribeMismatch(Measured.Digest),
            StringComparison.Ordinal);
    }

    [Fact]
    public void TheUnknownFieldsAreNamedSoAMachineCanBeFixed()
    {
        var reason = (Measured with { Ffprobe = ToolingFingerprint.Unknown }).DescribeMismatch(Measured.Digest);

        Assert.Contains("ffprobe", reason, StringComparison.Ordinal);
        Assert.DoesNotContain("PresentMon", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void AVerdictThatRecordedNoFingerprintIsNeverAccepted()
    {
        // The state an older run wrote. Absence of a record is not evidence that the
        // two agree, and reading it as unchanged is the whole defect.
        Assert.False(Measured.Accepts(string.Empty));
        Assert.Contains("do not say what they were measured with", Measured.DescribeMismatch(string.Empty),
            StringComparison.Ordinal);
    }

    [Fact]
    public void AMatchingFingerprintIsAccepted()
    {
        Assert.True(Measured.Accepts(Measured.Digest));
    }
}

/// <summary>What a run state hands back to a run measuring under different tools.</summary>
public sealed class StaleByToolingTests
{
    private static ToolingFingerprint Measured => new("26100.4061", "32.0.15.8098", "9.0.1", "2.5.1");

    private static RunState StateWith(string tooling) => new(
        RunState.CurrentSchemaVersion,
        "run",
        "v0.9.1-rc1",
        "commit",
        "artifact",
        "1.0.0",
        DateTimeOffset.UtcNow,
        [new ScenarioVerdict("A", ScenarioOutcome.Pass, "measured", 1, [])])
    {
        ToolingFingerprint = tooling,
    };

    [Fact]
    public void VerdictsMeasuredWithTheSameToolsComeBackAsThemselves()
    {
        var verdicts = StateWith(Measured.Digest).VerdictsFor("artifact", "1.0.0", Measured);

        Assert.Equal(ScenarioOutcome.Pass, verdicts[0].Outcome);
    }

    [Fact]
    public void VerdictsMeasuredWithDifferentToolsComeBackStale()
    {
        var verdicts = StateWith(Measured.Digest)
            .VerdictsFor("artifact", "1.0.0", Measured with { Ffprobe = "9.1.0" });

        Assert.Equal(ScenarioOutcome.Stale, verdicts[0].Outcome);
        Assert.Contains("changed since this verdict was recorded", verdicts[0].Message, StringComparison.Ordinal);
    }

    [Fact]
    public void VerdictsThatRecordedNoToolingComeBackStale()
    {
        var verdicts = StateWith(string.Empty).VerdictsFor("artifact", "1.0.0", Measured);

        Assert.Equal(ScenarioOutcome.Stale, verdicts[0].Outcome);
        Assert.Contains("do not say what they were measured with", verdicts[0].Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ACallerThatDoesNotSayWhatItMeasuresWithGetsTheOlderAnswer()
    {
        // The report command reads a run without probing the machine, and making it
        // probe would mean a printout could not be produced on a different machine at
        // all. The rule is opt-in at the call site for that reason.
        var verdicts = StateWith(string.Empty).VerdictsFor("artifact", "1.0.0");

        Assert.Equal(ScenarioOutcome.Pass, verdicts[0].Outcome);
    }

    [Fact]
    public void AChangedArtifactStillReportsTheArtifactRatherThanTheTooling()
    {
        // Both moved. The artifact is the more consequential one and the message says
        // so, rather than blaming a tool nobody touched.
        var verdicts = StateWith(string.Empty).VerdictsFor("different", "1.0.0", Measured);

        Assert.Contains("the artifacts changed", verdicts[0].Message, StringComparison.Ordinal);
    }
}

/// <summary>
/// When a changed tool set blocks a campaign rather than only invalidating reuse.
/// </summary>
/// <remarks>
/// The two are different severities on purpose. A verdict measured with something
/// else may not be reused; a run whose own tool set cannot be fully read is not
/// thereby disqualified, because refusing to qualify on a machine without an optional
/// tool would stop every campaign there -- a worse failure than the one prevented.
/// </remarks>
public sealed class ToolingBlocksReuseButNotEveryRunTests
{
    private static ToolingFingerprint Measured => new("26100.4061", "32.0.15.8098", "9.0.1", "2.5.1");

    [Fact]
    public void AnUnreadableToolRefusesReuse()
    {
        Assert.False(ToolingFingerprint.Nothing.Accepts(Measured.Digest));
    }

    [Fact]
    public void AnUnreadableToolIsNotItselfADisagreement()
    {
        // Nothing is claimed to have changed: the run simply cannot say. The
        // reconciliation guard reads this as "no reuse", never as "this machine may
        // not produce a qualified run".
        Assert.Empty(ToolingFingerprint.Nothing.Digest);
    }

    [Fact]
    public void TwoFullyMeasuredToolSetsThatDifferDisagree()
    {
        var other = Measured with { GpuDriver = "33.0.0.1" };

        Assert.NotEmpty(other.Digest);
        Assert.False(other.Accepts(Measured.Digest));
        Assert.Contains("changed since", other.DescribeMismatch(Measured.Digest), StringComparison.Ordinal);
    }
}
