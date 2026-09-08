using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Real-platform smokes: the adapters driven against the actual tools on this
/// machine, when those tools are here to drive. Hermetic in the sense that
/// matters - never a GUI, never synthesized input, never a machine mutation -
/// and each one skips with a truthful reason rather than failing when its
/// mechanism is simply not present.
/// </summary>
public sealed class PlatformSmokeTests
{
    private static readonly TimeSpan FfmpegTimeout = TimeSpan.FromSeconds(60);

    [Fact]
    public async Task FfprobeReadsARealEncodeItJustMade()
    {
        var tools = new ToolResolver();
        var ffmpeg = tools.Resolve("ffmpeg", "EXOSNAP_FFMPEG");
        var ffprobe = tools.Resolve("ffprobe", "EXOSNAP_FFPROBE");

        if (!ffmpeg.Available)
        {
            Assert.Skip("ffmpeg is not resolvable on PATH (or EXOSNAP_FFMPEG); nothing to encode with");
            return;
        }

        if (!ffprobe.Available)
        {
            Assert.Skip("ffprobe is not resolvable on PATH (or EXOSNAP_FFPROBE); nothing to inspect with");
            return;
        }

        using var directory = FixtureTool.NewTemporaryDirectory("-ffprobe-smoke");
        var outputPath = Path.Combine(directory.Path, "smoke.mkv");

        using var processes = new ProcessRunner();
        var encode = await processes.RunAsync(
            new ProcessRunRequest(
                ffmpeg.Path!,
                "-y",
                "-f", "lavfi", "-i", "testsrc=size=320x240:rate=30:duration=2",
                "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                "-c:v", "libx264", "-preset", "ultrafast",
                "-c:a", "aac",
                "-shortest",
                outputPath)
            {
                Timeout = FfmpegTimeout,
            },
            TestContext.Current.CancellationToken);

        Assert.True(encode.Succeeded, $"ffmpeg did not produce the smoke encode: {encode.StandardError}");
        Assert.True(File.Exists(outputPath), "ffmpeg reported success but wrote no file");

        var adapter = new Ffprobe(processes, ffprobe.Path);
        var inspected = await adapter.InspectAsync(outputPath, TestContext.Current.CancellationToken);

        Assert.Single(inspected.VideoStreams);
        Assert.Single(inspected.AudioStreams);
        Assert.NotNull(inspected.Format.DurationSeconds);
        Assert.InRange(inspected.Format.DurationSeconds!.Value, 1.0, 4.0);

        var spans = await adapter.PacketSpanSecondsAsync(
            outputPath, [inspected.AudioStreams[0].Index], TestContext.Current.CancellationToken);

        Assert.True(spans[0] > 1.0, $"the audio packet span was only {spans[0]}s for a ~2s encode");
    }

    [Fact]
    public async Task EnvctlSnapshotIsReadOnlyAndReportsOk()
    {
        var tools = new ToolResolver();
        var repositoryRoot = FindRepositoryRoot();
        var envctlPath = CampaignServices.ResolveEnvctl(tools, repositoryRoot ?? string.Empty);

        if (envctlPath is null)
        {
            Assert.Skip(
                "exosnap-envctl is not resolvable (checked EXOSNAP_ENVCTL, PATH, and the " +
                "tools/envctl build targets under " +
                $"{(repositoryRoot is null ? "(no repository root found)" : repositoryRoot)}); build it with " +
                "-DEXOSNAP_BUILD_PROBES=ON is NOT required, envctl is its own target");
            return;
        }

        using var processes = new ProcessRunner();
        var envctl = new Envctl(processes, envctlPath);

        // snapshot is read-only by contract: never begin, never restore, never mutate.
        var response = await envctl.SnapshotAsync(TestContext.Current.CancellationToken);

        Assert.True(response.Ok, $"exosnap-envctl snapshot did not report ok: {response.RawJson}");
    }

    [Fact]
    public void PresentMonSmokeIsDeferredToTheVmSlice() =>
        Assert.Skip(
            "starting PresentMon needs an elevated ETW session; the real-platform smoke for it is deferred " +
            "to the disposable VM slice rather than run against the developer's own desktop");

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
