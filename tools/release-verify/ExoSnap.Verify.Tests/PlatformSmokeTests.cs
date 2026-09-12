using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Adapters.Elevation;
using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Processes;
using ExoSnap.Verify.Windows;
using ExoSnap.Verify.Windows.Uia;

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

    // A real virtual machine boots for this one; the worker itself does nothing.
    private const string SandboxSmokeVariable = "EXOSNAP_SANDBOX_SMOKE";

    private static readonly TimeSpan SandboxSmokeTimeout = TimeSpan.FromMinutes(5);

    [Fact]
    public void AHardwareAdapterReportsAUserModeDriverVersion()
    {
        // The one production read in the tooling fingerprint that could silently
        // return nothing: an empty driver version makes the whole fingerprint empty,
        // which reads as "cannot be reused" and would quietly switch the binding off
        // on every machine.
        var adapters = ExoSnap.Verify.Windows.GraphicsProbe.TryEnumerateAdapters();
        Assert.SkipWhen(adapters is null, "DXGI is not reachable on this machine");

        var hardware = adapters!.FirstOrDefault(adapter => !adapter.IsSoftware);
        Assert.SkipWhen(hardware is null, "this machine has no hardware graphics adapter");

        Assert.NotEmpty(hardware!.UserModeDriverVersion);
        Assert.Contains(".", hardware.UserModeDriverVersion, StringComparison.Ordinal);
    }

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

    [Fact]
    public void FlaUiReadsARealElementTreeForAnOffscreenProbeWindow()
    {
        if (InteractiveDesktop.IsReachable() != true)
        {
            Assert.Skip(
                "no interactive desktop (locked workstation or Secure Desktop); UI Automation has no tree to read");
            return;
        }

        var uia = new FlaUiAutomation();
        var probe = new Thread(() => ProbeWindow.ShowOffscreen(TimeSpan.FromSeconds(5))) { IsBackground = true };
        probe.Start();
        try
        {
            var tree = UiTreeSnapshot.Unreadable("the probe was never read");
            var deadline = DateTime.UtcNow.AddSeconds(8);
            while (DateTime.UtcNow < deadline)
            {
                tree = uia.SnapshotProcess(Environment.ProcessId, TimeSpan.FromSeconds(2));
                if (tree.Ok && tree.Elements.Count > 0)
                {
                    break;
                }

                Thread.Sleep(200);
            }

            Assert.True(tree.Ok, tree.Detail);
            Assert.Empty(tree.MissingText([ProbeWindow.Title]));
        }
        finally
        {
            probe.Join(TimeSpan.FromSeconds(10));
        }
    }

    [Fact]
    public void FlaUiReportsANonRunningProcessAsUnreadableRatherThanEmpty()
    {
        var tree = new FlaUiAutomation().SnapshotProcess(int.MaxValue, TimeSpan.FromMilliseconds(200));

        // An unreadable tree and an empty one are opposite verdicts: a gate turns the
        // first into an infrastructure error and the second into a product finding.
        Assert.False(tree.Ok);
        Assert.Contains("not running", tree.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task TheElevatedWorkerSelfTestRoundTripsThroughTheResultFile()
    {
        var worker = ElevatedWorkerHost.Resolve(FindRepositoryRoot());
        if (worker is null)
        {
            Assert.Skip("ExoSnap.Verify.Worker.exe has not been built into its bin directory");
            return;
        }

        using var directory = FixtureTool.NewTemporaryDirectory("-worker-selftest");
        var resultPath = Path.Combine(directory.Path, ElevatedWorkerResult.FileName);

        // selfTest: the worker skips its elevation assertion and writes a canned
        // result, so this exercises the real process launch and the file boundary
        // with no UAC prompt. The elevated relaunch itself is a Phase F check.
        var run = await new ElevatedWorkerHost(worker).RunAsync(
            "REL-PRESENT-002",
            resultPath,
            targetExe: string.Empty,
            TimeSpan.FromSeconds(30),
            selfTest: true,
            TestContext.Current.CancellationToken);

        Assert.True(run.Kind == ElevatedWorkerRunKind.Completed, $"{run.Kind}: {run.Detail} (worker: {worker})");
        Assert.NotNull(run.Result);
        Assert.Equal(ElevatedWorkerOutcome.Pass, run.Result!.Outcome);
        Assert.Equal("selfTest", run.Result.PresentMode);
        Assert.Equal("REL-PRESENT-002", run.Result.TaskId);
    }

    [Fact]
    public async Task SandboxTransportRunsARealWorkerAndReadsItsMarker()
    {
        // Opt-in, unlike every other smoke in this file: a sandbox is a real virtual
        // machine whose window opens on, and takes focus from, whoever is at the
        // desktop, and only one may run at a time on a machine. It is therefore
        // started deliberately, never as a side effect of running the suite.
        if (Environment.GetEnvironmentVariable(SandboxSmokeVariable) is not "1")
        {
            Assert.Skip($"the sandbox smoke opens a real virtual machine window; set {SandboxSmokeVariable}=1 to run it");
            return;
        }

        using var processes = new ProcessRunner();
        using var directory = FixtureTool.NewTemporaryDirectory("-sandbox-smoke");
        var transport = new SandboxTransport(processes, new ToolResolver(), Path.Combine(directory.Path, "staging"));
        if (!transport.Available)
        {
            Assert.Skip(transport.UnavailableReason);
            return;
        }

        // No product under test: this proves the staging, launch, marker-poll and
        // result-read path works end to end against a real virtual machine.
        var worker = Path.Combine(directory.Path, "smoke-worker.ps1");
        await File.WriteAllTextAsync(
            worker,
            """
            param([string] $StagingDirectory, [string] $ResultPath, [string] $MarkerPath)
            Set-Content -LiteralPath $ResultPath -Value '{"steps":[{"name":"ran","ok":true,"detail":"smoke","kind":"product"}]}'
            New-Item -ItemType File -Path $MarkerPath -Force | Out-Null
            """,
            TestContext.Current.CancellationToken);

        var run = await transport.RunWorkerAsync(
            new DisposableOsWorkerRequest("smoke-worker.ps1", [worker], []) { Timeout = SandboxSmokeTimeout },
            TestContext.Current.CancellationToken);

        Assert.True(run.Kind == DisposableOsRunKind.Completed, $"{run.Kind}: {run.Detail}");
        var step = Assert.Single(run.Result!.Steps);
        Assert.Equal("ran", step.Name);
        Assert.True(step.Ok);
    }

    [Fact]
    public void ARealInstallerDeclaresTheProductUnderTest()
    {
        var pinned = Environment.GetEnvironmentVariable(ReleaseMsiArtifact.PathVariable);
        if (string.IsNullOrWhiteSpace(pinned) || !File.Exists(pinned))
        {
            Assert.Skip(
                $"no installer to read: set {ReleaseMsiArtifact.PathVariable} to a published ExoSnap MSI");
            return;
        }

        var properties = MsiPackage.ReadProperties(pinned);

        Assert.NotNull(properties);
        Assert.Equal("ExoSnap", properties!["ProductName"]);
        Assert.Equal("Codexo", properties["Manufacturer"]);
        Assert.Matches(@"^\d+\.\d+\.\d+", properties["ProductVersion"]);
    }

    [Fact]
    public void TheAppsColourAppearanceReadsAsLightOrDarkOnThisMachine()
    {
        // Read-only: the write path is exercised only by the overlay gate's own run,
        // which pairs every set with a restore. A real Windows desktop always has this
        // value, so Unknown here means the read itself is broken.
        var appearance = new WindowsSystemAppearance().Current;

        Assert.Contains(appearance, new[] { AppsAppearance.Light, AppsAppearance.Dark });
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
