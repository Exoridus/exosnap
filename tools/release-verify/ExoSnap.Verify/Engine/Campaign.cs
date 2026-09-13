using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Adapters.Elevation;
using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Adapters.PresentMon;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;
using ExoSnap.Verify.Windows.Uia;

namespace ExoSnap.Verify.Engine;

/// <summary>
/// What a campaign is bound to, kept beside its verdicts.
/// </summary>
/// <param name="SchemaVersion">Version of this document's shape.</param>
/// <param name="Binding">The artifact and the release candidate.</param>
/// <param name="Packages">The published downloadables the verdicts describe.</param>
/// <param name="RepositoryRoot">Where the helper scripts a few gates invoke live.</param>
/// <remarks>
/// Separate from the run state because the two answer different questions: the state
/// says what has been measured so far, and this says what it was measured against. A
/// campaign that resumed against a different artifact is detected by the state's own
/// fingerprint, not by re-reading this.
/// </remarks>
public sealed record CampaignDocument(
    string SchemaVersion,
    CampaignBinding Binding,
    IReadOnlyList<ReleasePackage> Packages,
    string RepositoryRoot)
{
    /// <summary>The schema version this build of the harness writes.</summary>
    public const string CurrentSchemaVersion = "1";

    /// <summary>The file name a campaign's binding is stored under.</summary>
    public const string FileName = "campaign.json";
}

/// <summary>
/// Wires the adapters a campaign drives, and takes them down again.
/// </summary>
/// <remarks>
/// One place where every external mechanism is resolved, so a run can say what it
/// found and what it did not before any gate asks. A tool resolved here is pinned by
/// its environment variable when one is set: a campaign that parses a tool's output
/// has to be able to name the exact build whose output it parsed.
/// </remarks>
public sealed class CampaignServices : IAsyncDisposable
{
    private readonly ProcessRunner processes;
    private readonly GateSessionHost sessions;

    private CampaignServices(ProcessRunner processes, GateSessionHost sessions, GateServices gates)
    {
        this.processes = processes;
        this.sessions = sessions;
        this.Gates = gates;
    }

    /// <summary>What the gates are given.</summary>
    public GateServices Gates { get; }

    /// <summary>Resolves every adapter and opens the environment orchestrator.</summary>
    public static async Task<CampaignServices> OpenAsync(
        CampaignDocument campaign,
        string runId,
        string journalDirectory,
        string journalPath,
        string? aliasProfile,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(campaign);

        var processes = new ProcessRunner();
        var tools = new ToolResolver();

        var ffprobe = new Ffprobe(processes, tools.Resolve("ffprobe", "EXOSNAP_FFPROBE").Path);
        var envctl = new Envctl(processes, ResolveEnvctl(tools, campaign.RepositoryRoot), aliasProfile);
        var presentMon = new PresentMonReader(tools.Resolve("PresentMon", "EXOSNAP_PRESENTMON").Path);

        // Transport order is the fallback order: the first one that reports itself
        // available and can satisfy what the run declared carries it. The virtual
        // machine comes first because it is the only one that measures the session a
        // worker lands in, which a capture run needs and the sandbox cannot answer for.
        var disposableOs = new DisposableOsRunner(
        [
            new HyperVTransport(
                processes,
                campaign.RepositoryRoot,
                HyperVAccess.Measure(
                    moduleExists: name => Directory.Exists(Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.System),
                        "WindowsPowerShell", "v1.0", "Modules", name)),
                    // The management service, by the process it runs as: asking for it
                    // by name needs a package reference for one boolean.
                    serviceRunning: name => Process.GetProcessesByName(name).Length > 0),
                Path.Combine(Path.GetTempPath(), "exosnap-verify-vm")),
            new SandboxTransport(processes, tools, Path.Combine(Path.GetTempPath(), "exosnap-verify-sandbox")),
        ]);

        var environment = await EnvironmentOrchestrator
            .OpenAsync(envctl, runId, journalDirectory, journalPath, cancellationToken)
            .ConfigureAwait(false);

        // Offscreen unless the caller says otherwise. A campaign must never take focus
        // from whoever is using the machine, and the platform plugin is the only
        // control that holds for every window the process opens rather than only the
        // ones the harness knows about.
        var factory = new LiveVerifySessionFactory(
            new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase)
            {
                ["QT_QPA_PLATFORM"] = Environment.GetEnvironmentVariable("QT_QPA_PLATFORM") ?? "offscreen",
            },
            null);

        var sessions = new GateSessionHost(factory, campaign.Binding.ExecutablePath);

        var gates = new GateServices(
            new ArtifactUnderTest(
                campaign.Binding.ExecutablePath,
                campaign.Binding.ProductVersion,
                campaign.RepositoryRoot,
                campaign.Binding.RcTag,
                campaign.Binding.SourceCommit,
                campaign.Binding.ExecutableSha256),
            sessions,
            ffprobe,
            environment,
            envctl,
            presentMon,
            processes,
            factory,
            new FlaUiAutomation(),
            new Windows.WindowsSystemAppearance(),
            new ElevatedWorkerHost(ElevatedWorkerHost.Resolve(campaign.RepositoryRoot)),
            disposableOs,
            new AudioEndpointControl(processes, tools));

        return new CampaignServices(processes, sessions, gates);
    }

    /// <inheritdoc/>
    public async ValueTask DisposeAsync()
    {
        await this.sessions.DisposeAsync().ConfigureAwait(false);
        this.processes.Dispose();
    }

    /// <summary>
    /// Where <c>exosnap-envctl</c> is, or null.
    /// </summary>
    /// <remarks>
    /// A developer build artifact rather than a machine tool, so the pinning
    /// environment variable wins, PATH is the fallback, and the build tree is where it
    /// normally lives. Never guessed at beyond the trees this repository actually
    /// configures: a tool found somewhere else is a tool nobody can attribute a
    /// verdict to.
    /// </remarks>
    public static string? ResolveEnvctl(ToolResolver tools, string repositoryRoot)
    {
        ArgumentNullException.ThrowIfNull(tools);
        var resolved = tools.Resolve("exosnap-envctl", Envctl.PathVariable);
        if (resolved.Available)
        {
            return resolved.Path;
        }

        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            return null;
        }

        foreach (var candidate in new[]
                 {
                     "build/windows-x64-ninja-release/tools/envctl/exosnap-envctl.exe",
                     "build/windows-x64-ninja-debug/tools/envctl/exosnap-envctl.exe",
                     "build/windows-x64-release/tools/envctl/Release/exosnap-envctl.exe",
                     "build/windows-x64-debug/tools/envctl/Debug/exosnap-envctl.exe",
                 })
        {
            var path = Path.Combine(repositoryRoot, candidate.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(path))
            {
                return Path.GetFullPath(path);
            }
        }

        return null;
    }
}

/// <summary>Whether every property a campaign mutated came back.</summary>
/// <param name="SchemaVersion">Version of this document's shape.</param>
/// <param name="Restores">Scenario id to restore verdict.</param>
public sealed record EnvironmentRestoreDocument(
    string SchemaVersion,
    IReadOnlyDictionary<string, string> Restores)
{
    /// <summary>The schema version this build of the harness writes.</summary>
    public const string CurrentSchemaVersion = "1";
}

/// <summary>Reads and writes the documents a campaign directory holds.</summary>
public static class Campaign
{
    /// <summary>The file the machine capability document is written to.</summary>
    public const string CapabilitiesFileName = "machine-capabilities.json";

    /// <summary>The file the scenario catalog is written to.</summary>
    public const string CatalogFileName = "scenario-catalog.json";

    /// <summary>
    /// The file the environment restore verdicts are written to.
    /// </summary>
    /// <remarks>
    /// Its own document because `run` and `qualify` are separate invocations, and a
    /// restore verdict that lived only in the running orchestrator would be gone by
    /// the time the record is written. A campaign that left a display in the wrong
    /// mode and could not say so afterwards is the failure this file prevents.
    /// </remarks>
    public const string RestoresFileName = "environment-restores.json";

    /// <summary>Writes a campaign binding into a run directory.</summary>
    public static void WriteDocument(RunDirectory run, CampaignDocument document)
    {
        ArgumentNullException.ThrowIfNull(run);
        VerifyJson.WriteFile(
            Path.Combine(run.Root, CampaignDocument.FileName), document, VerifyJsonContext.Default.CampaignDocument);
    }

    /// <summary>Reads the campaign binding, or null when the directory holds none.</summary>
    public static CampaignDocument? ReadDocument(RunDirectory run)
    {
        ArgumentNullException.ThrowIfNull(run);
        return VerifyJson.ReadFile(
            Path.Combine(run.Root, CampaignDocument.FileName), VerifyJsonContext.Default.CampaignDocument);
    }

    /// <summary>Reasons a prepared campaign no longer describes the bytes and catalog it will run.</summary>
    public static ReadOnlyCollection<string> ReconciliationBlockers(
        RunDirectory run,
        CampaignDocument campaign,
        ScenarioCatalog catalog,
        ToolingFingerprint? tooling = null)
    {
        ArgumentNullException.ThrowIfNull(run);
        ArgumentNullException.ThrowIfNull(campaign);
        ArgumentNullException.ThrowIfNull(catalog);

        var reasons = new List<string>();
        CampaignBinding current;
        try
        {
            current = Bind(
                campaign.Binding.RunId,
                campaign.Binding.RcTag,
                campaign.Binding.SourceCommit,
                campaign.Binding.ExecutablePath);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            reasons.Add($"the bound executable cannot be read: {exception.Message}");
            return new ReadOnlyCollection<string>(reasons);
        }

        if (!string.Equals(current.ExecutableSha256, campaign.Binding.ExecutableSha256, StringComparison.OrdinalIgnoreCase))
        {
            reasons.Add("the bound executable bytes changed after prepare");
        }

        var storedCatalog = VerifyJson.ReadFile(
            Path.Combine(run.Root, CatalogFileName), VerifyJsonContext.Default.ScenarioDescriptorDocument);
        if (storedCatalog is null ||
            !string.Equals(storedCatalog.CatalogVersion, catalog.Version, StringComparison.Ordinal) ||
            !string.Equals(
                ReleaseVerificationRecord.CatalogDigest(storedCatalog.Scenarios),
                ReleaseVerificationRecord.CatalogDigest(catalog.Descriptors),
                StringComparison.OrdinalIgnoreCase))
        {
            reasons.Add("the prepared scenario catalog does not match this harness");
        }

        var state = run.ReadState();
        var artifactFingerprint = Qualification.ArtifactFingerprint(
            [new ArtifactDigest(Path.GetFileName(current.ExecutablePath), current.ExecutableSha256)]);
        if (state is null ||
            !string.Equals(state.RunId, campaign.Binding.RunId, StringComparison.Ordinal) ||
            !string.Equals(state.RcTag, campaign.Binding.RcTag, StringComparison.Ordinal) ||
            !string.Equals(state.SourceCommitSha, campaign.Binding.SourceCommit, StringComparison.Ordinal) ||
            !string.Equals(state.ArtifactFingerprint, artifactFingerprint, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(state.CatalogVersion, catalog.Version, StringComparison.Ordinal))
        {
            reasons.Add("the run state does not match the prepared campaign, artifact, and catalog");
        }

        // A verdict is a statement about bytes measured BY something ON something.
        // The bytes are bound above; this binds the rest, so a local pass produced
        // with a different ffprobe, on a different Windows build or through a
        // different display driver cannot qualify a release nobody re-ran.
        //
        // Only a real disagreement blocks. A field this machine cannot read makes the
        // digest empty, and that has to mean "these verdicts may not be REUSED" rather
        // than "this run may not qualify": refusing to qualify because a tool nobody
        // needs is unreadable would stop every campaign on a machine without it, which
        // is a different and much worse failure than the one being prevented.
        if (tooling is { Digest.Length: > 0 } &&
            state is { ToolingFingerprint.Length: > 0 } &&
            !tooling.Accepts(state.ToolingFingerprint))
        {
            reasons.Add(tooling.DescribeMismatch(state.ToolingFingerprint));
        }

        return new ReadOnlyCollection<string>(reasons);
    }

    /// <summary>A fresh campaign identifier, ordered so directories sort by start time.</summary>
    public static string NewRunId() =>
        "rel-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);

    /// <summary>Describes one published downloadable by its bytes.</summary>
    public static ReleasePackage DescribePackage(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        var full = Path.GetFullPath(path);
        using var stream = File.OpenRead(full);
        var digest = Convert.ToHexString(SHA256.HashData(stream)).ToLower(CultureInfo.InvariantCulture);
        return new ReleasePackage(Path.GetFileName(full), digest, new FileInfo(full).Length);
    }

    /// <summary>Binds a campaign to an artifact, hashing the executable it will drive.</summary>
    public static CampaignBinding Bind(
        string runId,
        string rcTag,
        string sourceCommit,
        string executablePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(executablePath);
        var full = Path.GetFullPath(executablePath);
        if (!File.Exists(full))
        {
            throw new FileNotFoundException("The artifact under test does not exist.", full);
        }

        using var stream = File.OpenRead(full);
        var digest = Convert.ToHexString(SHA256.HashData(stream)).ToLower(CultureInfo.InvariantCulture);
        var version = FileVersionInfo.GetVersionInfo(full).ProductVersion ?? string.Empty;

        // An install tree is what the updater and handoff gates need, and it is a fact
        // about the layout beside the executable rather than a switch a caller sets.
        var installTree = Directory.Exists(Path.Combine(Path.GetDirectoryName(full) ?? ".", "platforms"));

        return new CampaignBinding(runId, rcTag, sourceCommit, full, version, digest, installTree);
    }

    /// <summary>Which harness build is producing verdicts, and whether its tree was clean.</summary>
    public static (string Version, string Commit, bool Dirty) HarnessIdentity(string repositoryRoot)
    {
        var identity = Qualification.HarnessIdentity();
        if (string.IsNullOrWhiteSpace(repositoryRoot) || !Directory.Exists(repositoryRoot))
        {
            return (identity.Version, identity.Commit, false);
        }

        var commit = Git(repositoryRoot, "rev-parse", "HEAD");
        if (string.IsNullOrWhiteSpace(commit))
        {
            return (identity.Version, identity.Commit, false);
        }

        var status = Git(repositoryRoot, "status", "--porcelain");
        return (identity.Version, commit.Trim(), status.Trim().Length > 0);
    }

    /// <summary>The environment facts a record's fingerprint is taken over.</summary>
    public static ReadOnlyDictionary<string, string> EnvironmentFacts(
        MachineCapabilityDocument capabilities,
        bool envctlAvailable)
    {
        ArgumentNullException.ThrowIfNull(capabilities);

        var facts = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["osVersion"] = capabilities.Machine.OsVersion,
            ["gpus"] = string.Join('|', capabilities.Machine.Gpus),
            ["processorCount"] = Environment.ProcessorCount.ToString(CultureInfo.InvariantCulture),
            ["architecture"] = System.Runtime.InteropServices.RuntimeInformation.OSArchitecture.ToString(),
            ["envctl"] = envctlAvailable ? "available" : "missing",
        };

        foreach (var (key, value) in capabilities.Capabilities)
        {
            facts["capability." + key] = value;
        }

        return new ReadOnlyDictionary<string, string>(facts);
    }

    /// <summary>The gates promotion depends on: everything not opt-in, plus what this release named.</summary>
    /// <exception cref="CatalogException">A named id is not in the catalog.</exception>
    public static ReadOnlyCollection<string> RequiredIds(
        IReadOnlyList<ScenarioDescriptor> descriptors,
        IReadOnlyCollection<string> namedOptIn)
    {
        ArgumentNullException.ThrowIfNull(descriptors);
        ArgumentNullException.ThrowIfNull(namedOptIn);

        var known = descriptors.Select(descriptor => descriptor.Id).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var duplicates = namedOptIn
            .GroupBy(id => id, StringComparer.OrdinalIgnoreCase)
            .Where(group => group.Count() > 1)
            .Select(group => group.Key)
            .ToList();
        if (duplicates.Count > 0)
        {
            throw new CatalogException($"Duplicate scenario id(s) named as required: {string.Join(", ", duplicates)}");
        }

        var unknown = namedOptIn.Where(id => !known.Contains(id)).ToList();
        if (unknown.Count > 0)
        {
            // An unknown id is an error rather than an empty set: a typo that quietly
            // required nothing is the exact failure this lock exists to prevent.
            throw new CatalogException($"Unknown scenario id(s) named as required: {string.Join(", ", unknown)}");
        }

        return new ReadOnlyCollection<string>(
        [
            .. descriptors
                .Where(descriptor => !descriptor.OptIn || namedOptIn.Contains(descriptor.Id, StringComparer.OrdinalIgnoreCase))
                .Select(descriptor => descriptor.Id),
        ]);
    }

    /// <summary>Writes the environment restore verdicts beside a run's state.</summary>
    public static void WriteRestores(RunDirectory run, IReadOnlyDictionary<string, string> restores)
    {
        ArgumentNullException.ThrowIfNull(run);
        ArgumentNullException.ThrowIfNull(restores);
        VerifyJson.WriteFile(
            Path.Combine(run.Root, RestoresFileName),
            new EnvironmentRestoreDocument(EnvironmentRestoreDocument.CurrentSchemaVersion, restores),
            VerifyJsonContext.Default.EnvironmentRestoreDocument);
    }

    /// <summary>The recorded restore verdicts, or an empty map when the run has none.</summary>
    public static IReadOnlyDictionary<string, string> ReadRestores(RunDirectory run)
    {
        ArgumentNullException.ThrowIfNull(run);
        var document = VerifyJson.ReadFile(
            Path.Combine(run.Root, RestoresFileName), VerifyJsonContext.Default.EnvironmentRestoreDocument);
        return document?.Restores ?? new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>Turns the run's verdicts into the rows a record carries.</summary>
    public static ReadOnlyCollection<RecordedCheck> Rows(
        IReadOnlyList<ScenarioVerdict> verdicts,
        IReadOnlyList<ScenarioDescriptor> descriptors,
        IReadOnlyCollection<string> requiredIds,
        IReadOnlyDictionary<string, string>? restores = null)
    {
        ArgumentNullException.ThrowIfNull(verdicts);
        ArgumentNullException.ThrowIfNull(descriptors);
        ArgumentNullException.ThrowIfNull(requiredIds);

        var byId = descriptors.ToDictionary(descriptor => descriptor.Id, StringComparer.OrdinalIgnoreCase);
        return new ReadOnlyCollection<RecordedCheck>(
        [
            .. verdicts.Select(verdict =>
            {
                var descriptor = byId.TryGetValue(verdict.Id, out var found) ? found : null;
                return new RecordedCheck(
                    verdict.Id,
                    descriptor?.Title ?? string.Empty,
                    descriptor?.Layer.ToString() ?? string.Empty,
                    ReleaseVerificationRecord.StateOf(verdict.Outcome),
                    requiredIds.Contains(verdict.Id, StringComparer.OrdinalIgnoreCase),
                    descriptor?.OptIn ?? false,
                    verdict.Message,
                    verdict.DurationMs,
                    restores is not null && restores.TryGetValue(verdict.Id, out var restore)
                        ? restore
                        : RestoreResultNames.For(RestoreResult.NotApplicable),
                    verdict.Evidence,
                    Attempted(verdict.Outcome));
            }),
        ]);
    }

    // Skipped, Unavailable and Blocked are settled by the plan before anything runs,
    // so no body was invoked and there is nothing to have attempted. Every other
    // outcome, including an infrastructure error, describes a body that started.
    private static bool Attempted(ScenarioOutcome outcome) =>
        outcome is not (ScenarioOutcome.Skipped or ScenarioOutcome.Unavailable or ScenarioOutcome.Blocked);

    private static string Git(string repositoryRoot, params string[] arguments)
    {
        try
        {
            var startInfo = new ProcessStartInfo
            {
                FileName = "git",
                WorkingDirectory = repositoryRoot,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            foreach (var argument in arguments)
            {
                startInfo.ArgumentList.Add(argument);
            }

            using var process = Process.Start(startInfo);
            if (process is null)
            {
                return string.Empty;
            }

            var output = process.StandardOutput.ReadToEnd();
            process.WaitForExit(10000);
            return process.ExitCode == 0 ? output : string.Empty;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            // No git on this machine. The blocker for an empty harness commit says so;
            // guessing would be worse.
            return string.Empty;
        }
    }
}
