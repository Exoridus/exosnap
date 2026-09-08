using System.Collections.ObjectModel;
using System.Text.Json;
using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Adapters.PresentMon;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>Reads the committed adapter-contract and gate-test fixtures.</summary>
internal static class Fixtures
{
    /// <summary>Absolute path of a fixture file in the test output.</summary>
    public static string Path(string name) => System.IO.Path.Combine(AppContext.BaseDirectory, "Fixtures", name);

    /// <summary>The content of a fixture file.</summary>
    public static string Read(string name) => File.ReadAllText(Path(name));
}

/// <summary>A configurable <see cref="IFfprobe"/> that never starts a process.</summary>
internal sealed class FakeFfprobe : IFfprobe
{
    private FfprobeResult? inspectResult;
    private Exception? inspectThrows;

    /// <inheritdoc/>
    public bool Available { get; set; } = true;

    /// <summary>The result the next <see cref="InspectAsync"/> call returns.</summary>
    public FfprobeResult InspectResult
    {
        set => this.inspectResult = value;
    }

    /// <summary>The exception the next <see cref="InspectAsync"/> call throws instead of returning.</summary>
    public Exception InspectThrows
    {
        set => this.inspectThrows = value;
    }

    /// <summary>The spans the next <see cref="PacketSpanSecondsAsync"/> call returns.</summary>
    public IReadOnlyList<double> PacketSpans { get; set; } = [];

    /// <summary>Every path <see cref="InspectAsync"/> was asked about, in call order.</summary>
    public List<string> InspectedPaths { get; } = [];

    /// <summary>Every call <see cref="PacketSpanSecondsAsync"/> received, in call order.</summary>
    public List<(string Path, IReadOnlyList<int> StreamIndexes)> PacketSpanCalls { get; } = [];

    /// <inheritdoc/>
    public Task<FfprobeResult> InspectAsync(string path, CancellationToken cancellationToken)
    {
        this.InspectedPaths.Add(path);
        if (this.inspectThrows is not null)
        {
            throw this.inspectThrows;
        }

        return Task.FromResult(
            this.inspectResult ?? throw new InvalidOperationException("FakeFfprobe.InspectResult was not configured."));
    }

    /// <inheritdoc/>
    public Task<ReadOnlyCollection<double>> PacketSpanSecondsAsync(
        string path,
        IReadOnlyList<int> streamIndexes,
        CancellationToken cancellationToken)
    {
        this.PacketSpanCalls.Add((path, streamIndexes));
        return Task.FromResult(new ReadOnlyCollection<double>([.. this.PacketSpans]));
    }
}

/// <summary>
/// A configurable <see cref="IEnvctl"/> that answers through the same JSON parser
/// the real adapter uses, so a fixture document exercises the real parsing code.
/// </summary>
internal sealed class FakeEnvctl : IEnvctl
{
    /// <summary>A recovery answer that leaves mutation allowed and nothing to restore.</summary>
    public const string CleanRecoverJson =
        """{"ok":true,"command":"recover","state":"Clean","mutationAllowed":true,"evidence":{"properties":[]}}""";

    /// <inheritdoc/>
    public bool Available { get; set; } = true;

    /// <inheritdoc/>
    public string? ExecutablePath { get; set; } = @"C:\fake\exosnap-envctl.exe";

    /// <summary>The document the next <c>describe</c> call answers with.</summary>
    public string DescribeJson { get; set; } = """{"ok":true,"command":"describe","catalogue":[]}""";

    /// <summary>The document the next <c>snapshot</c> call answers with.</summary>
    public string SnapshotJson { get; set; } = """{"ok":true,"command":"snapshot"}""";

    /// <summary>The document the next <c>resolve-aliases</c> call answers with.</summary>
    public string ResolveAliasesJson { get; set; } =
        """{"ok":true,"command":"resolve-aliases","bindings":[],"errors":[],"candidates":[]}""";

    /// <summary>The document the next <c>list-modes</c> call answers with.</summary>
    public string ListModesJson { get; set; } = """{"ok":true,"command":"list-modes","displays":[]}""";

    /// <summary>The document the startup <c>recover</c> call answers with.</summary>
    public string RecoverJson { get; set; } = CleanRecoverJson;

    /// <summary>The document the next <c>begin</c> call answers with.</summary>
    public string BeginJson { get; set; } = """{"ok":true,"command":"begin","state":"Active","applied":[]}""";

    /// <summary>The document the next <c>restore</c> call answers with.</summary>
    public string RestoreJson { get; set; } = """{"ok":true,"command":"restore","state":"Restored"}""";

    /// <summary>Every command invoked, in call order (<c>list-modes</c> and <c>begin</c> carry their argument).</summary>
    public List<string> Calls { get; } = [];

    /// <inheritdoc/>
    public Task<EnvctlResponse> DescribeAsync(CancellationToken cancellationToken)
    {
        this.Calls.Add("describe");
        return Task.FromResult(Envctl.ParseDocument(this.DescribeJson));
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> SnapshotAsync(CancellationToken cancellationToken)
    {
        this.Calls.Add("snapshot");
        return Task.FromResult(Envctl.ParseDocument(this.SnapshotJson));
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> ResolveAliasesAsync(CancellationToken cancellationToken)
    {
        this.Calls.Add("resolve-aliases");
        return Task.FromResult(Envctl.ParseDocument(this.ResolveAliasesJson));
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> ListModesAsync(string deviceAlias, CancellationToken cancellationToken)
    {
        this.Calls.Add($"list-modes:{deviceAlias}");
        return Task.FromResult(Envctl.ParseDocument(this.ListModesJson));
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> RecoverAsync(string journalPath, CancellationToken cancellationToken)
    {
        this.Calls.Add("recover");
        return Task.FromResult(Envctl.ParseDocument(this.RecoverJson));
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> BeginAsync(
        string scenario,
        string runId,
        string journalPath,
        string desiredFilePath,
        CancellationToken cancellationToken)
    {
        this.Calls.Add($"begin:{scenario}");
        return Task.FromResult(Envctl.ParseDocument(this.BeginJson));
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> RestoreAsync(string journalPath, CancellationToken cancellationToken)
    {
        this.Calls.Add("restore");
        return Task.FromResult(Envctl.ParseDocument(this.RestoreJson));
    }
}

/// <summary>A configurable <see cref="IPresentMon"/> that never starts a process.</summary>
internal sealed class FakePresentMon : IPresentMon
{
    private PresentMonCapture? capture;

    /// <inheritdoc/>
    public bool Available { get; set; } = true;

    /// <inheritdoc/>
    public string UnavailableReason { get; set; } = string.Empty;

    /// <summary>The capture the next <see cref="ReadCaptureAsync"/> call returns.</summary>
    public PresentMonCapture Capture
    {
        set => this.capture = value;
    }

    /// <summary>Every path <see cref="ReadCaptureAsync"/> was asked to read, in call order.</summary>
    public List<string> ReadPaths { get; } = [];

    /// <summary>Configures the capture by parsing PresentMon CSV text.</summary>
    public void SetCaptureFromCsv(string csv) => this.capture = PresentMonCsv.Parse(csv);

    /// <inheritdoc/>
    public Task<PresentMonCapture> ReadCaptureAsync(string csvPath, CancellationToken cancellationToken)
    {
        this.ReadPaths.Add(csvPath);
        return Task.FromResult(
            this.capture ?? throw new InvalidOperationException("FakePresentMon.Capture was not configured."));
    }
}

/// <summary>
/// A configurable <see cref="ILiveVerifySession"/> that answers from a command name
/// to JSON result map, without opening a control channel.
/// </summary>
internal sealed class FakeLiveVerifySession : ILiveVerifySession
{
    private readonly Dictionary<string, string> answers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (string ErrorCode, string ErrorMessage)> refusals =
        new(StringComparer.Ordinal);
    private readonly Dictionary<string, List<string>> sequencedAnswers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> sequenceIndex = new(StringComparer.Ordinal);
    private readonly List<string> recordingStates = [];
    private int recordingStateIndex;

    /// <inheritdoc/>
    public string RunId { get; init; } = "fake-run";

    /// <inheritdoc/>
    public long StateRevision { get; set; }

    /// <inheritdoc/>
    public int BufferedEventCount { get; set; }

    /// <summary>What <see cref="ShutdownAsync"/> reports.</summary>
    public SessionShutdown ShutdownResult { get; set; } = SessionShutdown.Exited;

    /// <summary>Every command <see cref="InvokeAsync"/> received, in call order.</summary>
    public List<string> InvokedCommands { get; } = [];

    /// <summary>Whether <see cref="DisposeAsync"/> was called.</summary>
    public bool Disposed { get; private set; }

    /// <summary>Sets the JSON document a command answers with.</summary>
    public void SetResult(string command, string json)
    {
        this.answers[command] = json;
        this.refusals.Remove(command);
    }

    /// <summary>Makes a command refuse instead of answering.</summary>
    public void Refuse(string command, string errorCode = "refused", string errorMessage = "refused by the fake session")
    {
        this.refusals[command] = (errorCode, errorMessage);
        this.answers.Remove(command);
    }

    /// <summary>
    /// Sets a sequence of JSON documents a command answers with, one per call. Once
    /// exhausted, further calls keep returning the last document in the sequence.
    /// </summary>
    /// <remarks>
    /// For a gate that samples the same command more than once and needs to see it
    /// change between calls - a preview that starts idle and then advances, for
    /// example - where a single static answer cannot express that.
    /// </remarks>
    public void SetResultSequence(string command, params string[] jsonDocuments)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(command);
        ArgumentNullException.ThrowIfNull(jsonDocuments);
        this.sequencedAnswers[command] = [.. jsonDocuments];
        this.sequenceIndex[command] = 0;
        this.answers.Remove(command);
        this.refusals.Remove(command);
    }

    /// <summary>The sequence <see cref="WaitForRecordingStateAsync"/> returns, one entry per call.</summary>
    /// <remarks>Once exhausted, further calls keep returning the last scripted state.</remarks>
    public void ScriptRecordingStates(params string[] states)
    {
        this.recordingStates.Clear();
        this.recordingStates.AddRange(states);
        this.recordingStateIndex = 0;
    }

    /// <inheritdoc/>
    public Task<ControlAnswer> InvokeAsync(
        string command,
        IReadOnlyDictionary<string, object?>? parameters,
        CancellationToken cancellationToken)
    {
        this.InvokedCommands.Add(command);

        if (this.refusals.TryGetValue(command, out var refusal))
        {
            return Task.FromResult(new ControlAnswer(false, default, refusal.ErrorCode, refusal.ErrorMessage));
        }

        string json;
        if (this.sequencedAnswers.TryGetValue(command, out var sequence) && sequence.Count > 0)
        {
            var index = Math.Min(this.sequenceIndex[command], sequence.Count - 1);
            json = sequence[index];
            this.sequenceIndex[command] = index + 1;
        }
        else
        {
            json = this.answers.TryGetValue(command, out var configured) ? configured : "{}";
        }

        using var document = JsonDocument.Parse(json);
        return Task.FromResult(new ControlAnswer(true, document.RootElement.Clone(), string.Empty, string.Empty));
    }

    /// <inheritdoc/>
    public Task<string> WaitForRecordingStateAsync(
        IReadOnlyCollection<string> states,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        if (this.recordingStates.Count == 0)
        {
            return Task.FromResult(string.Empty);
        }

        var index = Math.Min(this.recordingStateIndex, this.recordingStates.Count - 1);
        this.recordingStateIndex++;
        return Task.FromResult(this.recordingStates[index]);
    }

    /// <inheritdoc/>
    public Task<bool> WaitForRevisionAsync(long after, TimeSpan timeout, CancellationToken cancellationToken) =>
        Task.FromResult(true);

    /// <inheritdoc/>
    public Task<SessionShutdown> ShutdownAsync(TimeSpan timeout, CancellationToken cancellationToken) =>
        Task.FromResult(this.ShutdownResult);

    /// <inheritdoc/>
    public ValueTask DisposeAsync()
    {
        this.Disposed = true;
        return ValueTask.CompletedTask;
    }
}

/// <summary>
/// Hands out one <see cref="FakeLiveVerifySession"/> as the shared session, and
/// records how many times it was ended.
/// </summary>
internal sealed class FakeSessionHost(FakeLiveVerifySession session, List<string>? log = null) : IGateSessionHost
{
    private readonly List<string> log = log ?? [];

    /// <summary>How many times <see cref="EndAsync"/> was called.</summary>
    public int EndCallCount { get; private set; }

    /// <summary>The order in which this host and the session factory below were used.</summary>
    public IReadOnlyList<string> Log => this.log;

    /// <inheritdoc/>
    public Task<ILiveVerifySession> EnsureAsync(CancellationToken cancellationToken)
    {
        this.log.Add("sessions.ensure");
        return Task.FromResult<ILiveVerifySession>(session);
    }

    /// <inheritdoc/>
    public Task EndAsync()
    {
        this.EndCallCount++;
        this.log.Add("sessions.end");
        return Task.CompletedTask;
    }

    /// <inheritdoc/>
    public ValueTask DisposeAsync() => new(this.EndAsync());
}

/// <summary>Hands out one <see cref="FakeLiveVerifySession"/> as a freshly launched instance.</summary>
internal sealed class FakeSessionFactory(FakeLiveVerifySession session, List<string>? log = null)
    : ILiveVerifySessionFactory
{
    private readonly List<string> log = log ?? [];

    /// <summary>Every executable path <see cref="LaunchAsync"/> was asked to start, in call order.</summary>
    public List<string> LaunchedExecutables { get; } = [];

    /// <inheritdoc/>
    public Task<ILiveVerifySession> LaunchAsync(string executablePath, CancellationToken cancellationToken)
    {
        this.log.Add("factory.launch");
        this.LaunchedExecutables.Add(executablePath);
        return Task.FromResult<ILiveVerifySession>(session);
    }
}

/// <summary>Every fake a gate's <see cref="GateServices"/> can be wired to.</summary>
internal sealed class GateFakes
{
    private readonly List<string> sessionLog = [];

    /// <summary>The independent ffprobe oracle.</summary>
    public FakeFfprobe Ffprobe { get; } = new();

    /// <summary>The environment tool, driving both the read-only gates and the orchestrator.</summary>
    public FakeEnvctl Envctl { get; } = new();

    /// <summary>The independent PresentMon oracle.</summary>
    public FakePresentMon PresentMon { get; } = new();

    /// <summary>The one session a gate under test is handed, whichever way it reaches it.</summary>
    public FakeLiveVerifySession Session { get; } = new();

    /// <summary>The shared session host.</summary>
    public FakeSessionHost SessionHost { get; }

    /// <summary>The launcher for a gate that needs an instance of its own.</summary>
    public FakeSessionFactory SessionFactory { get; }

    /// <summary>Whether <see cref="SessionHost"/> ended its session before <see cref="SessionFactory"/> launched one.</summary>
    public IReadOnlyList<string> SessionLog => this.sessionLog;

    /// <summary>The capability values a scenario's requirements and body may read.</summary>
    public Dictionary<string, string> CapabilityValues { get; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Where the artifact under test is said to live; also where a gate's helper scripts are looked for.</summary>
    public string RepositoryRoot { get; set; } = string.Empty;

    /// <summary>The executable path a gate's <c>ArtifactUnderTest</c> carries.</summary>
    public string ExecutablePath { get; set; } = "exosnap.exe";

    /// <summary>The product version a gate's <c>ArtifactUnderTest</c> carries.</summary>
    public string ProductVersion { get; set; } = "0.9.1";

    /// <summary>What a previous present cross-check confirmed, or null.</summary>
    public PresentConfirmation? LastPresentConfirmation { get; set; }

    /// <summary>The PresentMon capture path a run is bound to, or null.</summary>
    public string? PresentCapturePath { get; set; }

    /// <summary>Builds the bundle, wiring the session host and factory to one shared session and log.</summary>
    public GateFakes()
    {
        this.SessionHost = new FakeSessionHost(this.Session, this.sessionLog);
        this.SessionFactory = new FakeSessionFactory(this.Session, this.sessionLog);
    }
}

/// <summary>
/// Builds a <see cref="ScenarioContext"/> for one scenario id, wired to a fresh
/// <see cref="GateFakes"/> bundle, so a gate test is two lines: create the harness,
/// run the body.
/// </summary>
internal sealed class GateHarness : IDisposable
{
    private readonly TemporaryDirectory directory;
    private readonly ProcessRunner processes;

    private GateHarness(TemporaryDirectory directory, ProcessRunner processes, ScenarioContext context, GateFakes fakes)
    {
        this.directory = directory;
        this.processes = processes;
        this.Context = context;
        this.Fakes = fakes;
    }

    /// <summary>The context a gate's <c>RunAsync</c> is called with.</summary>
    public ScenarioContext Context { get; }

    /// <summary>The fakes <see cref="Context"/> was wired to.</summary>
    public GateFakes Fakes { get; }

    /// <summary>
    /// Builds a harness for one catalog scenario. <paramref name="configure"/> runs
    /// before the environment orchestrator opens, so it can shape the recovery
    /// answer a mutating gate's transaction depends on.
    /// </summary>
    public static async Task<GateHarness> CreateAsync(
        string scenarioId,
        Action<GateFakes>? configure,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scenarioId);

        var directory = FixtureTool.NewTemporaryDirectory("-" + scenarioId);
        var fakes = new GateFakes { RepositoryRoot = directory.Path };
        configure?.Invoke(fakes);

        var processes = new ProcessRunner();
        var journalDirectory = Path.Combine(directory.Path, "journal");
        var orchestrator = await EnvironmentOrchestrator.OpenAsync(
                fakes.Envctl,
                "test-run",
                journalDirectory,
                Path.Combine(journalDirectory, "env-journal.json"),
                cancellationToken)
            .ConfigureAwait(false);

        var services = new GateServices(
            new ArtifactUnderTest(fakes.ExecutablePath, fakes.ProductVersion, fakes.RepositoryRoot),
            fakes.SessionHost,
            fakes.Ffprobe,
            orchestrator,
            fakes.Envctl,
            fakes.PresentMon,
            processes,
            fakes.SessionFactory,
            fakes.LastPresentConfirmation,
            fakes.PresentCapturePath);

        var descriptor = ReleaseCatalog.Descriptors()
            .First(candidate => string.Equals(candidate.Id, scenarioId, StringComparison.Ordinal));

        var capabilities = new CapabilitySet(
            fakes.CapabilityValues,
            fakes.CapabilityValues.ToDictionary(pair => pair.Key, _ => "test", StringComparer.OrdinalIgnoreCase));

        var evidenceDirectory = Path.Combine(directory.Path, "evidence", scenarioId);
        Directory.CreateDirectory(evidenceDirectory);

        var context = new ScenarioContext(descriptor, capabilities, processes, evidenceDirectory, services);
        return new GateHarness(directory, processes, context, fakes);
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        this.processes.Dispose();
        this.directory.Dispose();
    }
}
