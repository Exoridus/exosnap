using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Adapters.PresentMon;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The adapter parsers, exercised against committed documents so a contract change
/// is caught without a real ffprobe, envctl, or PresentMon on hand.
/// </summary>
public sealed class AdapterContractTests
{
    [Fact]
    public void FfprobeReadsARealMkvDocument()
    {
        var result = Ffprobe.ParseDocument(Fixtures.Read("ffprobe-mkv.json"));

        Assert.Equal(3, result.Streams.Count);

        var video = Assert.Single(result.VideoStreams);
        Assert.Equal(0, video.Index);
        Assert.Equal("av1", video.CodecName);

        Assert.Equal(2, result.AudioStreams.Count);
        var firstAudio = result.AudioStreams[0];
        Assert.Equal(1, firstAudio.Index);
        Assert.Equal(48000, firstAudio.SampleRateHz);
        Assert.Equal(2, firstAudio.Channels);

        // ffprobe writes duration and sample_rate as JSON strings, not numbers.
        Assert.NotNull(result.Format.DurationSeconds);
        Assert.Equal(5.977, result.Format.DurationSeconds!.Value, 3);
    }

    [Fact]
    public void FfprobePacketSpanIsLastMinusFirstIgnoringNA()
    {
        var csv = Fixtures.Read("ffprobe-packets.csv");
        var span = Ffprobe.SpanOf(csv, streamIndex: 1);

        // 0.000000 .. 3.003000, with an "N/A" line and a trailing blank line ignored.
        Assert.Equal(3.003, span, 3);
    }

    [Fact]
    public void FfprobePacketSpanRefusesANonNumericTimestamp()
    {
        var thrown = Assert.Throws<ToolContractException>(() => Ffprobe.SpanOf("0.000000\nnot-a-number\n", 4));
        Assert.Contains("stream 4", thrown.Message, StringComparison.Ordinal);
        Assert.Contains("not-a-number", thrown.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void EnvctlDescribeReadsTheRealCapabilityCatalogue()
    {
        var response = Envctl.ParseDocument(Fixtures.Read("envctl-describe.json"));

        Assert.True(response.Ok);
        var catalogue = response.Array("catalogue").ToList();
        Assert.Equal(21, catalogue.Count);
        Assert.Contains(catalogue, entry => entry.OptionalString("capability") == "ENV_MUTATE_SAFE");
        Assert.Contains(catalogue, entry => entry.OptionalString("capability") == "ENV_READ");
        Assert.Contains(catalogue, entry => entry.OptionalString("capability") == "ENV_HUMAN");
        Assert.All(catalogue, entry => Assert.NotEqual(string.Empty, entry.OptionalString("readMechanism")));
    }

    [Fact]
    public void EnvctlResolveAliasesReadsTheRealBindingDocument()
    {
        var response = Envctl.ParseDocument(Fixtures.Read("envctl-resolve-aliases.json"));

        Assert.True(response.Ok);
        var bindings = response.Array("bindings").ToList();
        Assert.Equal(4, bindings.Count);
        Assert.All(bindings, binding => Assert.Equal("ok", binding.OptionalString("status")));
        Assert.Empty(response.Array("errors"));
        Assert.Contains(bindings, binding => binding.OptionalString("alias") == "display.main-hdr");
    }

    [Fact]
    public void EnvctlBeginReadsTheRealTransactionDocument()
    {
        var response = Envctl.ParseDocument(Fixtures.Read("envctl-begin.json"));

        Assert.True(response.Ok);
        Assert.Equal("Active", response.State);
        var applied = response.Array("applied").ToList();
        var property = Assert.Single(applied);
        Assert.Equal("display.main-hdr:refresh-hz", property.OptionalString("property"));
        Assert.Equal("144", property.OptionalString("from"));
        Assert.Equal("120", property.OptionalString("to"));
    }

    // The 2.5.1 capture below is synthetic: this repository has no way to run
    // PresentMon (an elevated ETW session) inside this test process, so it is
    // written by hand against the documented PresentMon 2.x column contract in
    // PresentMonCsv. The real-platform smoke that starting PresentMon actually
    // works is deferred to the VM slice (see PlatformSmokeTests).
    [Fact]
    public void PresentMonReadsTypedRecordsAndClassifiesModes()
    {
        var capture = PresentMonCsv.Parse(Fixtures.Read("presentmon-2.5.1.csv"));

        var single = capture.For(4242);
        var record = Assert.Single(single);
        Assert.Equal("exosnap.exe", record.Application);
        Assert.Equal(4242, record.ProcessId);
        Assert.Equal("0x000001", record.SwapChainAddress);
        Assert.Equal(PresentMode.IndependentFlip, record.Mode);
        Assert.Equal("Hardware: Independent Flip", record.RawMode);
        Assert.Equal(1, record.SyncInterval);
        Assert.False(record.AllowsTearing);
        Assert.NotNull(record.CpuStartSeconds);
        Assert.Equal(10.0, record.CpuStartSeconds!.Value, 3);
        Assert.NotNull(record.FrameTimeMs);
        Assert.Equal(16.667, record.FrameTimeMs!.Value, 3);

        var composedOnly = capture.For(6666);
        Assert.All(composedOnly, entry => Assert.Equal(PresentMode.Composed, entry.Mode));
    }

    [Fact]
    public void PresentMonHandlesAQuotedCommaInsideAnApplicationName()
    {
        var capture = PresentMonCsv.Parse(Fixtures.Read("presentmon-2.5.1.csv"));

        var record = Assert.Single(capture.For(6666));
        Assert.Equal("Test, App.exe", record.Application);
        Assert.True(record.AllowsTearing);
    }

    [Fact]
    public void PresentMonSkipsATruncatedFinalRow()
    {
        var capture = PresentMonCsv.Parse(Fixtures.Read("presentmon-2.5.1.csv"));

        Assert.DoesNotContain(capture.Records, record => record.ProcessId == 7777);
    }

    [Fact]
    public void PresentMonDominantModeIsUnknownForAProcessThatPresentedInTwoModes()
    {
        var capture = PresentMonCsv.Parse(Fixtures.Read("presentmon-2.5.1.csv"));

        Assert.Equal(2, capture.For(5555).Count);
        Assert.Equal(PresentMode.Unknown, capture.DominantModeFor(5555));
    }

    [Fact]
    public void PresentMonRefusesACaptureWithNoPresentModeColumn()
    {
        const string csv = "Application,ProcessID,SwapChainAddress\nexosnap.exe,4242,0x1\n";
        var thrown = Assert.Throws<ToolContractException>(() => PresentMonCsv.Parse(csv));
        Assert.Contains("PresentMode", thrown.Message, StringComparison.Ordinal);
    }
}
