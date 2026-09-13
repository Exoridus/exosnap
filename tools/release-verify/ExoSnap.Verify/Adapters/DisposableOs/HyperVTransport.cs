using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Adapters.DisposableOs;

/// <summary>
/// Runs a worker in a throwaway Hyper-V clone of the release-verification guest.
/// </summary>
/// <remarks>
/// <para>
/// The control plane is the recipe under <c>tools/vm</c>. It already owns everything
/// that needs a Hyper-V host and could not be reimplemented here without saying the
/// same things twice: the differencing disk taken from a golden image that is never
/// written to, the GPU partition and its read-back, the image fingerprint, the
/// interactive agent and its handshake, and the order that rescues a run's evidence
/// before anything is allowed to discard it.
/// </para>
/// <para>
/// What this adds is the typed edge. A gate body hands over a worker request and
/// receives the same result it would from any other transport, so it does not know
/// which machine carried it -- and a run that produced no result document is Faulted
/// rather than read as a verdict, because a machine the harness could not build says
/// nothing about the product.
/// </para>
/// <para>
/// Preferred over the sandbox for a run that declares it needs a proven interactive
/// guest, which is the one thing the sandbox transport cannot answer for.
/// </para>
/// </remarks>
public sealed class HyperVTransport : IDisposableOsTransport
{
    private const string RecipeRelativePath = @"tools\vm\Invoke-ReleaseVmRun.ps1";

    private static readonly TimeSpan DefaultRunTimeout = TimeSpan.FromMinutes(150);

    private readonly string repositoryRoot;
    private readonly HyperVAccess access;
    private readonly string stagingRoot;
    private readonly Func<ProcessRunRequest, CancellationToken, Task<ProcessRunResult>> invoke;

    /// <summary>Creates a transport over the recipe in <paramref name="repositoryRoot"/>.</summary>
    public HyperVTransport(ProcessRunner processes, string repositoryRoot, HyperVAccess access, string stagingRoot)
        : this(processes, repositoryRoot, access, stagingRoot, null)
    {
    }

    /// <summary>Creates a transport whose recipe invocation is stood in for, for tests.</summary>
    /// <remarks>
    /// There is no way to exercise the composition and the reading against a real
    /// Hyper-V host without building a virtual machine on the developer's own
    /// machine, which is minutes per case and a GPU partition taken away from them
    /// while it runs.
    /// </remarks>
    internal HyperVTransport(
        ProcessRunner processes,
        string repositoryRoot,
        HyperVAccess access,
        string stagingRoot,
        Func<ProcessRunRequest, CancellationToken, Task<ProcessRunResult>>? invoke)
    {
        ArgumentNullException.ThrowIfNull(processes);
        ArgumentException.ThrowIfNullOrWhiteSpace(repositoryRoot);
        ArgumentNullException.ThrowIfNull(access);
        ArgumentException.ThrowIfNullOrWhiteSpace(stagingRoot);

        this.repositoryRoot = repositoryRoot;
        this.access = access;
        this.stagingRoot = stagingRoot;
        this.invoke = invoke ?? ((request, cancellationToken) => processes.RunAsync(request, cancellationToken));
    }

    /// <inheritdoc/>
    public string Name => "vm";

    /// <inheritdoc/>
    public bool Available => this.access.Usable && File.Exists(this.RecipePath);

    /// <inheritdoc/>
    /// <remarks>
    /// Backed by the recipe: its readiness step holds the agent's own receipt against
    /// the run requirement and refuses a guest whose session is not one a campaign can
    /// run in.
    /// </remarks>
    public bool ProvesInteractiveGuest => true;

    /// <inheritdoc/>
    public string UnavailableReason
    {
        get
        {
            if (this.Available)
            {
                return string.Empty;
            }

            var accessReason = this.access.DescribeUnavailable();
            if (!string.IsNullOrEmpty(accessReason))
            {
                return accessReason;
            }

            return $"the virtual machine recipe {RecipeRelativePath} is not in {this.repositoryRoot}";
        }
    }

    /// <inheritdoc/>
    public async Task<DisposableOsRun> RunWorkerAsync(
        DisposableOsWorkerRequest request, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (!this.Available)
        {
            return DisposableOsRun.Unavailable(this.UnavailableReason);
        }

        var runId = "verify-" + Guid.NewGuid().ToString("N")[..12];
        var staging = Path.Combine(this.stagingRoot, runId);
        var harness = Path.Combine(staging, "harness");
        var results = Path.Combine(staging, "results");

        var staged = StagePayload(request, harness);
        if (staged is not null)
        {
            // Before a machine exists: a payload that cannot be assembled is not
            // something a virtual machine would explain.
            return DisposableOsRun.Faulted(staged);
        }

        var evidence = EvidenceOutcome.NotRequested;
        DisposableOsRun outcome;
        try
        {
            var completed = await this.invoke(this.Compose(runId, request, harness, staging), cancellationToken)
                .ConfigureAwait(false);
            outcome = ReadOutcome(completed, Path.Combine(results, runId));
        }
        finally
        {
            evidence = SandboxTransport.CollectEvidence(Path.Combine(results, runId), request.EvidenceDirectory);
        }

        return outcome with { Evidence = evidence };
    }

    private string RecipePath => Path.Combine(this.repositoryRoot, RecipeRelativePath);

    /// <summary>Copies the request's payload into the directory the recipe hands the guest.</summary>
    /// <returns>Why it could not be staged, or null when it was.</returns>
    private static string? StagePayload(DisposableOsWorkerRequest request, string harness)
    {
        Directory.CreateDirectory(harness);
        foreach (var source in request.SourceFiles)
        {
            var leaf = Path.GetFileName(Path.TrimEndingDirectorySeparator(source));
            var destination = Path.Combine(harness, leaf);
            if (Directory.Exists(source))
            {
                CopyDirectory(source, destination);
            }
            else if (File.Exists(source))
            {
                File.Copy(source, destination, overwrite: true);
            }
            else
            {
                return $"the virtual machine run needs '{source}', which does not exist";
            }
        }

        return File.Exists(Path.Combine(harness, request.WorkerFileName))
            ? null
            : $"the worker {request.WorkerFileName} is not among the staged source files";
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.EnumerateFiles(source))
        {
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), overwrite: true);
        }

        foreach (var directory in Directory.EnumerateDirectories(source))
        {
            CopyDirectory(directory, Path.Combine(destination, Path.GetFileName(directory)));
        }
    }

    /// <summary>
    /// Turns what the recipe returned into a typed result.
    /// </summary>
    /// <remarks>
    /// The result document decides, not the exit code. A campaign that found defects
    /// exits non-zero and that is a verdict; a run with no document measured nothing,
    /// whatever it exited with, and reporting a verdict for it would be the harness
    /// blaming the product for a machine it could not build.
    /// </remarks>
    private static DisposableOsRun ReadOutcome(ProcessRunResult completed, string resultDirectory)
    {
        var document = Path.Combine(resultDirectory, "result.json");
        if (!File.Exists(document))
        {
            var detail = string.IsNullOrWhiteSpace(completed.StandardError)
                ? $"the virtual machine run exited {completed.ExitCode} and wrote no result document"
                : completed.StandardError.Trim();
            return DisposableOsRun.Faulted(detail);
        }

        var result = DisposableOsRunResult.Parse(File.ReadAllText(document));
        return result is null
            ? DisposableOsRun.Faulted("the virtual machine result document is not valid JSON")
            : DisposableOsRun.Completed(result);
    }

    private ProcessRunRequest Compose(
        string runId, DisposableOsWorkerRequest request, string harness, string staging)
    {
        // The recipe owns every guest path. What it is told is what to run and where
        // the pieces are on the host; a command line assembled here would be a second
        // place that has to know the guest layout.
        var arguments = new List<string>
        {
            "-NoProfile", "-File", this.RecipePath,
            "-RunId", runId,
            "-HarnessDirectory", harness,
            "-ResultRoot", Path.Combine(staging, "results"),
            "-GuestCommand", GuestCommandFor(request),
            "-Network", request.RequiresNetwork ? "Connected" : "Disconnected",
        };

        if (request.RequiresInteractiveGuest)
        {
            arguments.Add("-RequireInteractiveGuest");
        }

        return new ProcessRunRequest("pwsh", arguments)
        {
            WorkingDirectory = this.repositoryRoot,
            Timeout = request.Timeout > TimeSpan.Zero ? request.Timeout + TimeSpan.FromMinutes(30) : DefaultRunTimeout,
        };
    }

    private static string GuestCommandFor(DisposableOsWorkerRequest request)
    {
        var parts = new List<string>
        {
            "pwsh -NoProfile -ExecutionPolicy Bypass -File",
            Quote(Path.Combine("C:\\ExoSnapRun\\harness", request.WorkerFileName)),
        };
        parts.AddRange(request.WorkerArguments.Select(Quote));
        parts.AddRange(["-ResultPath", Quote("C:\\ExoSnapRun\\results\\result.json")]);
        parts.AddRange(["-MarkerPath", Quote("C:\\ExoSnapRun\\results\\done.marker")]);
        parts.AddRange(["-StagingDirectory", Quote("C:\\ExoSnapRun\\harness")]);

        if (request.EvidenceDirectory is not null)
        {
            parts.AddRange(["-EvidenceDirectory", Quote("C:\\ExoSnapRun\\results\\evidence")]);
        }

        return string.Join(' ', parts);
    }

    private static string Quote(string argument) =>
        argument.Contains(' ', StringComparison.Ordinal) ? "\"" + argument + "\"" : argument;
}
