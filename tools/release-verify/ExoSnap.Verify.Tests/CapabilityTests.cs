using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// How a machine's measured capabilities decide whether a scenario may run.
/// </summary>
public sealed class CapabilityTests
{
    private static CapabilitySet SetOf(params (string Key, string Value)[] values) =>
        new(
            values.ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.OrdinalIgnoreCase),
            values.ToDictionary(pair => pair.Key, _ => "test", StringComparer.OrdinalIgnoreCase));

    [Fact]
    public void AnExactMatchIsSatisfied()
    {
        var set = SetOf((CapabilityKeys.DisplayHdr, "true"));
        Assert.True(set.Evaluate(CapabilityRequirement.Is(CapabilityKeys.DisplayHdr, "true")).Satisfied);
    }

    [Fact]
    public void AnUnknownValueNeverSatisfiesAnything()
    {
        var set = SetOf((CapabilityKeys.DisplayHdr, CapabilityKeys.Unknown));

        Assert.False(set.Evaluate(CapabilityRequirement.Is(CapabilityKeys.DisplayHdr, "true")).Satisfied);
        Assert.False(set.Evaluate(
            new CapabilityRequirement(CapabilityKeys.DisplayHdr, CapabilityOperator.NotEquals, "true")).Satisfied);
        Assert.False(set.Evaluate(CapabilityRequirement.AtLeast(CapabilityKeys.DisplayHdr, "1")).Satisfied);
    }

    [Fact]
    public void AnAbsentKeyReadsAsUnknown()
    {
        var set = SetOf();
        Assert.Equal(CapabilityKeys.Unknown, set[CapabilityKeys.GpuVendor]);
        Assert.False(set.Evaluate(CapabilityRequirement.Is(CapabilityKeys.GpuVendor, "NVIDIA")).Satisfied);
    }

    [Fact]
    public void NumericLowerBoundsCompareNumericallyNotTextually()
    {
        var set = SetOf((CapabilityKeys.OsWindows, "11"));
        Assert.True(set.Evaluate(CapabilityRequirement.AtLeast(CapabilityKeys.OsWindows, "10")).Satisfied);
        Assert.False(set.Evaluate(CapabilityRequirement.AtLeast(CapabilityKeys.OsWindows, "12")).Satisfied);
    }

    [Fact]
    public void ANonNumericValueNeverSatisfiesALowerBound()
    {
        var set = SetOf((CapabilityKeys.GpuVendor, "NVIDIA"));
        Assert.False(set.Evaluate(CapabilityRequirement.AtLeast(CapabilityKeys.GpuVendor, "10")).Satisfied);
    }

    [Fact]
    public void TheUnsatisfiedMessageNamesTheKeyAndTheRequiredValue()
    {
        var requirement = CapabilityRequirement.Is(CapabilityKeys.DisplayHdr, "true");
        Assert.Equal("capability display.hdr=true not satisfied", requirement.UnsatisfiedMessage);
    }

    [Fact]
    public void FirstUnsatisfiedReportsTheEarliestFailingRequirement()
    {
        var set = SetOf((CapabilityKeys.GpuD3D11, "true"), (CapabilityKeys.DisplayHdr, "false"));
        var unsatisfied = set.FirstUnsatisfied(
        [
            CapabilityRequirement.Is(CapabilityKeys.GpuD3D11, "true"),
            CapabilityRequirement.Is(CapabilityKeys.DisplayHdr, "true"),
        ]);

        Assert.NotNull(unsatisfied);
        Assert.Equal(CapabilityKeys.DisplayHdr, unsatisfied.Requirement.Key);
        Assert.Equal("false", unsatisfied.Observed);
    }

    [Fact]
    public void ToolResolutionPrefersTheEnvironmentOverrideOverPath()
    {
        var resolver = new ToolResolver(
            name => name == "EXOSNAP_PRESENTMON" ? @"C:\pinned\PresentMon.exe" : null,
            path => path.Contains("pinned", StringComparison.OrdinalIgnoreCase),
            () => @"C:\other");

        var resolved = resolver.Resolve("PresentMon", "EXOSNAP_PRESENTMON");
        Assert.True(resolved.Available);
        Assert.Equal("EXOSNAP_PRESENTMON", resolved.Source);
    }

    [Fact]
    public void ToolResolutionFallsBackToPath()
    {
        var resolver = new ToolResolver(
            _ => null,
            path => path.EndsWith(@"tools\ffprobe.exe", StringComparison.OrdinalIgnoreCase),
            () => @"C:\none;C:\tools");

        var resolved = resolver.Resolve("ffprobe", "EXOSNAP_FFPROBE");
        Assert.True(resolved.Available);
        Assert.Equal("PATH", resolved.Source);
    }

    [Fact]
    public void AToolThatIsNowhereIsReportedAsAbsentRatherThanGuessed()
    {
        var resolver = new ToolResolver(_ => null, _ => false, () => @"C:\none");
        var resolved = resolver.Resolve("SoundVolumeView", "EXOSNAP_SOUNDVOLUMEVIEW");

        Assert.False(resolved.Available);
        Assert.Null(resolved.Path);
    }

    [Fact]
    public void ProbingTheRealMachineAnswersEveryKeyItClaims()
    {
        var document = new MachineCapabilityProbe().Probe();

        Assert.Equal(MachineCapabilityDocument.CurrentSchemaVersion, document.SchemaVersion);
        Assert.NotEmpty(document.Machine.Fingerprint);

        // Every key carries a source, and a value that is not "unknown" must have
        // come from somewhere: a probe that reported a value without naming its
        // mechanism is a value nobody can audit.
        foreach (var (key, value) in document.Capabilities)
        {
            Assert.True(document.Sources.ContainsKey(key), $"{key} has no recorded source");
            Assert.False(string.IsNullOrWhiteSpace(value), $"{key} has an empty value");
        }

        Assert.Contains(CapabilityKeys.Elevated, document.Capabilities.Keys);
        Assert.Contains(CapabilityKeys.OsWindows, document.Capabilities.Keys);
        Assert.Contains(CapabilityKeys.GpuD3D11, document.Capabilities.Keys);
    }
}
