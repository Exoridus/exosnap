using System.Globalization;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Cli;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify;

/// <summary>
/// The release verification harness.
/// </summary>
/// <remarks>
/// Nothing here promotes, tags, or publishes anything. The most a run produces is a
/// qualification record saying the candidate may be promoted; who acts on that is a
/// decision outside this program.
/// </remarks>
public static class Program
{
    private const int ExitOk = 0;
    private const int ExitNotQualified = 1;
    private const int ExitUsage = 2;
    private const int ExitInfrastructure = 3;

    /// <summary>Entry point.</summary>
    public static int Main(string[] args)
    {
        // Every number and date this program prints or writes is invariant, so an
        // operator's locale can never change what a report says.
        CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
        CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

        var command = CommandLine.Parse(args);
        try
        {
            return command.Verb switch
            {
                "capabilities" => Capabilities(command),
                "list" => List(command),
                "prepare" => PrepareAsync(command).GetAwaiter().GetResult(),
                "run" => RunAsync(command).GetAwaiter().GetResult(),
                "report" => Report(command),
                "qualify" => Qualify(command),
                "" or "help" or "--help" => Usage(ExitOk),
                _ => Usage(ExitUsage, $"Unknown command '{command.Verb}'."),
            };
        }
        catch (CatalogException exception)
        {
            Console.Error.WriteLine($"INFRA_ERROR the catalog is not usable: {exception.Message}");
            return ExitInfrastructure;
        }
        catch (ToolContractException exception)
        {
            Console.Error.WriteLine($"INFRA_ERROR {exception.Message}");
            return ExitInfrastructure;
        }
        catch (IOException exception)
        {
            Console.Error.WriteLine($"INFRA_ERROR {exception.Message}");
            return ExitInfrastructure;
        }
        catch (UnauthorizedAccessException exception)
        {
            Console.Error.WriteLine($"INFRA_ERROR {exception.Message}");
            return ExitInfrastructure;
        }
    }

    private static int Capabilities(CommandLine command)
    {
        var document = new MachineCapabilityProbe().Probe();
        var output = command.Value("out", Campaign.CapabilitiesFileName);
        VerifyJson.WriteFile(output, document, VerifyJsonContext.Default.MachineCapabilityDocument);

        foreach (var (key, value) in document.Capabilities)
        {
            Console.WriteLine($"{key,-32} {value,-12} {document.Sources[key]}");
        }

        Console.WriteLine();
        Console.WriteLine($"machine {document.Machine.Fingerprint}");
        Console.WriteLine($"written {Path.GetFullPath(output)}");
        return ExitOk;
    }

    private static int List(CommandLine command)
    {
        var catalog = ReleaseCatalog.Create();

        if (command.HasFlag("json"))
        {
            Console.WriteLine(VerifyJson.Serialize(
                catalog.ToDocument(),
                VerifyJsonContext.Default.ScenarioDescriptorDocument));
            return ExitOk;
        }

        var migrated = ReleaseCatalog.MigratedIds();
        Console.WriteLine(
            $"catalog {catalog.Version}, {catalog.Scenarios.Count} scenarios, {migrated.Count} with a migrated body");
        Console.WriteLine();
        foreach (var descriptor in catalog.Descriptors)
        {
            var requires = descriptor.Requires.Count == 0
                ? "-"
                : string.Join(", ", descriptor.Requires.Select(Format));
            var body = migrated.Contains(descriptor.Id, StringComparer.OrdinalIgnoreCase) ? "migrated" : "declared";
            Console.WriteLine($"{descriptor.Id,-26} {descriptor.Class,-15} {descriptor.Isolation,-13} " +
                              $"{descriptor.Interaction,-17} {(descriptor.OptIn ? "opt-in" : "default"),-8} {body,-9} {requires}");
            Console.WriteLine($"{string.Empty,-26} {descriptor.Title}");
        }

        return ExitOk;
    }

    private static Task<int> PrepareAsync(CommandLine command)
    {
        var exe = command.Value("exe", string.Empty);
        if (exe.Length == 0)
        {
            // A release gate binds its verdict to a specific set of bytes, so there is
            // deliberately no default resolution here.
            return Task.FromResult(Usage(ExitUsage, "prepare needs --exe: a campaign is bound to explicit bytes."));
        }

        var runId = command.Value("run-id", Campaign.NewRunId());
        var run = RunDirectory.Open(command.Value("run-dir", Path.Combine(RunsRoot(command), runId)));

        var binding = Campaign.Bind(
            runId,
            command.Value("rc", string.Empty),
            command.Value("commit", string.Empty),
            exe);

        var packages = command.Values("package").Select(Campaign.DescribePackage).ToList();
        var document = new CampaignDocument(
            CampaignDocument.CurrentSchemaVersion, binding, packages, RepositoryRoot(command));
        Campaign.WriteDocument(run, document);

        var capabilities = new MachineCapabilityProbe().Probe();
        VerifyJson.WriteFile(
            Path.Combine(run.Root, Campaign.CapabilitiesFileName),
            capabilities,
            VerifyJsonContext.Default.MachineCapabilityDocument);

        var catalog = ReleaseCatalog.Create();
        VerifyJson.WriteFile(
            Path.Combine(run.Root, Campaign.CatalogFileName),
            catalog.ToDocument(),
            VerifyJsonContext.Default.ScenarioDescriptorDocument);

        run.WriteState(new RunState(
            RunState.CurrentSchemaVersion,
            runId,
            binding.RcTag,
            binding.SourceCommit,
            Qualification.ArtifactFingerprint([new ArtifactDigest(
                Path.GetFileName(binding.ExecutablePath), binding.ExecutableSha256)]),
            catalog.Version,
            DateTimeOffset.UtcNow,
            []));

        Console.WriteLine($"prepared {runId}");
        Console.WriteLine($"artifact {binding.ExecutablePath}");
        Console.WriteLine($"version  {binding.ProductVersion} ({binding.ExecutableSha256[..16]})");
        Console.WriteLine($"rc       {binding.RcTag} at {binding.SourceCommit}");
        Console.WriteLine($"run dir  {run.Root}");
        return Task.FromResult(ExitOk);
    }

    private static async Task<int> RunAsync(CommandLine command)
    {
        var run = OpenRun(command);
        if (run is null)
        {
            return Usage(ExitUsage, "run needs a prepared campaign; use --run-dir or prepare one first.");
        }

        var campaign = Campaign.ReadDocument(run);
        if (campaign is null)
        {
            return Usage(ExitUsage, $"'{run.Root}' holds no {CampaignDocument.FileName}; prepare the campaign first.");
        }

        var catalog = ReleaseCatalog.Create();
        var capabilities = new MachineCapabilityProbe().ProbeSet();
        var selection = new ScenarioSelection(
            IncludeOptIn: command.HasFlag("include-opt-in"),
            Classes: command.Values("class"),
            Ids: command.Values("id"));

        var engine = new VerifyEngine(catalog);
        var plan = engine.Plan(selection, capabilities);

        await using var services = await CampaignServices.OpenAsync(
            campaign,
            campaign.Binding.RunId,
            Path.Combine(run.Root, "environment"),
            command.Value("journal", Path.Combine(campaign.RepositoryRoot, ".workspace", "env-journal.json")),
            command.Value("alias-profile", string.Empty) is { Length: > 0 } profile ? profile : null,
            CancellationToken.None).ConfigureAwait(false);

        var verdicts = await engine
            .RunAsync(plan, capabilities, run, services.Gates.Processes, CancellationToken.None, services.Gates)
            .ConfigureAwait(false);

        var state = run.ReadState();
        run.WriteState((state ?? new RunState(
                RunState.CurrentSchemaVersion,
                campaign.Binding.RunId,
                campaign.Binding.RcTag,
                campaign.Binding.SourceCommit,
                string.Empty,
                catalog.Version,
                DateTimeOffset.UtcNow,
                [])) with
        {
            Verdicts = verdicts,
        });

        Campaign.WriteRestores(run, services.Gates.Environment.Restores);

        PrintVerdicts(verdicts);
        return verdicts.Any(verdict => verdict.Outcome is ScenarioOutcome.Fail or ScenarioOutcome.InfrastructureError)
            ? ExitNotQualified
            : ExitOk;
    }

    private static int Report(CommandLine command)
    {
        var run = OpenRun(command);
        var state = run?.ReadState();
        if (state is null)
        {
            return Usage(ExitUsage, "report needs a run directory holding a state document.");
        }

        Console.WriteLine($"campaign {state.RunId}, rc {state.RcTag}, commit {state.SourceCommitSha}");
        Console.WriteLine();
        PrintVerdicts(state.Verdicts);
        return ExitOk;
    }

    private static int Qualify(CommandLine command)
    {
        var catalog = ReleaseCatalog.Create();

        if (command.HasFlag("dry-run"))
        {
            var capabilities = new MachineCapabilityProbe().ProbeSet();
            var selection = new ScenarioSelection(
                IncludeOptIn: command.HasFlag("include-opt-in"),
                Classes: command.Values("class"),
                Ids: command.Values("id"));

            var plan = new VerifyEngine(catalog).Plan(selection, capabilities);
            Console.WriteLine($"qualify --dry-run {command.Value("rc", "(no rc)")}");
            Console.WriteLine($"catalog {plan.CatalogVersion}, harness {Qualification.HarnessIdentity().Version}");
            Console.WriteLine();

            foreach (var planned in plan.Scenarios)
            {
                Console.WriteLine(
                    $"{planned.Descriptor.Id,-26} {planned.Outcome?.ToString() ?? "WOULD_RUN",-20} {planned.Message}");
            }

            var wouldRun = plan.Scenarios.Count(planned => planned.WouldRun);
            Console.WriteLine();
            Console.WriteLine($"{wouldRun} of {plan.Scenarios.Count} scenarios would run on this machine.");

            // A dry run never qualifies anything. Saying so out loud costs one line and
            // removes the only way this output could be mistaken for a verdict.
            Console.WriteLine("NOT QUALIFIED (dry run produces no qualification record)");
            return ExitNotQualified;
        }

        var run = OpenRun(command);
        var state = run?.ReadState();
        var campaign = run is null ? null : Campaign.ReadDocument(run);
        if (run is null || state is null || campaign is null)
        {
            return Usage(ExitUsage, "qualify needs a completed campaign; use --run-dir, or --dry-run to plan one.");
        }

        var capabilityDocument = VerifyJson.ReadFile(
            Path.Combine(run.Root, Campaign.CapabilitiesFileName),
            VerifyJsonContext.Default.MachineCapabilityDocument);
        if (capabilityDocument is null)
        {
            return Usage(ExitUsage, $"'{run.Root}' holds no {Campaign.CapabilitiesFileName}.");
        }

        // Deliberately not through CampaignServices: opening the orchestrator performs
        // the mandatory recovery pass, which may restore a machine. Writing a record
        // about verdicts already taken must not change what those verdicts described.
        var envctlAvailable =
            CampaignServices.ResolveEnvctl(new ToolResolver(), campaign.RepositoryRoot) is not null;

        var environment = Campaign.EnvironmentFacts(capabilityDocument, envctlAvailable);
        var required = Campaign.RequiredIds(catalog.Descriptors, command.Values("required"));
        var rows = Campaign.Rows(state.Verdicts, catalog.Descriptors, required, Campaign.ReadRestores(run));
        var harness = Campaign.HarnessIdentity(campaign.RepositoryRoot);

        var path = Path.Combine(run.Root, ReleaseVerificationRecord.FileName);
        var blockers = ReleaseVerificationRecord.Write(
            path,
            campaign.Binding,
            ReleaseVerificationRecord.FingerprintOf(environment),
            harness.Version,
            harness.Commit,
            harness.Dirty,
            catalog.Version,
            ReleaseVerificationRecord.CatalogDigest(catalog.Descriptors),
            catalog.Scenarios.Count,
            capabilityDocument.Capabilities,
            environment,
            campaign.Packages,
            required,
            command.Values("required"),
            rows,
            run.Root);

        Console.WriteLine($"written {path}");
        if (blockers.Count == 0)
        {
            Console.WriteLine($"QUALIFIED FOR PROMOTION commit: {campaign.Binding.SourceCommit} rc: {campaign.Binding.RcTag}");
            return ExitOk;
        }

        Console.WriteLine("NOT QUALIFIED");
        foreach (var blocker in blockers)
        {
            Console.WriteLine($"  - {blocker}");
        }

        return ExitNotQualified;
    }

    private static void PrintVerdicts(IReadOnlyList<ScenarioVerdict> verdicts)
    {
        foreach (var verdict in verdicts)
        {
            Console.WriteLine(
                $"{verdict.Id,-26} {ReleaseVerificationRecord.StateOf(verdict.Outcome),-14} {verdict.Message}");
        }

        Console.WriteLine();
        foreach (var group in verdicts
                     .GroupBy(verdict => verdict.Outcome)
                     .OrderBy(group => group.Key.ToString(), StringComparer.Ordinal))
        {
            Console.WriteLine($"{ReleaseVerificationRecord.StateOf(group.Key),-14} {group.Count()}");
        }
    }

    private static RunDirectory? OpenRun(CommandLine command)
    {
        var explicitDirectory = command.Value("run-dir", string.Empty);
        if (explicitDirectory.Length > 0)
        {
            return RunDirectory.Open(explicitDirectory);
        }

        var root = RunsRoot(command);
        if (!Directory.Exists(root))
        {
            return null;
        }

        // The newest campaign, so an operator who prepared one a moment ago does not
        // have to repeat its name. Never a campaign from another artifact: the run
        // state carries its own fingerprint and reports Stale when it moved.
        var newest = new DirectoryInfo(root).GetDirectories()
            .OrderByDescending(directory => directory.Name, StringComparer.Ordinal)
            .FirstOrDefault();
        return newest is null ? null : RunDirectory.Open(newest.FullName);
    }

    private static string RunsRoot(CommandLine command) =>
        command.Value("runs-root", Path.Combine(RepositoryRoot(command), ".workspace", "release-verify"));

    private static string RepositoryRoot(CommandLine command) =>
        Path.GetFullPath(command.Value("repo", Environment.CurrentDirectory));

    private static string Format(CapabilityRequirement requirement) => requirement.Operator switch
    {
        CapabilityOperator.AtLeast => $"{requirement.Key} >= {requirement.Value}",
        CapabilityOperator.NotEquals => $"{requirement.Key} != {requirement.Value}",
        _ => $"{requirement.Key} = {requirement.Value}",
    };

    private static int Usage(int exitCode, string? problem = null)
    {
        var writer = exitCode == ExitOk ? Console.Out : Console.Error;
        if (problem is not null)
        {
            writer.WriteLine(problem);
            writer.WriteLine();
        }

        writer.WriteLine("ExoSnap.Verify - release qualification harness");
        writer.WriteLine();
        writer.WriteLine("  capabilities [--out <path>]");
        writer.WriteLine("      Measure this machine and write machine-capabilities.json.");
        writer.WriteLine();
        writer.WriteLine("  list [--json]");
        writer.WriteLine("      Print the scenario catalog.");
        writer.WriteLine();
        writer.WriteLine("  prepare --exe <path> [--rc <tag>] [--commit <sha>] [--package <path>]...");
        writer.WriteLine("      Bind a campaign to explicit bytes and measure the machine.");
        writer.WriteLine();
        writer.WriteLine("  run [--id <id>] [--class <c>] [--include-opt-in]");
        writer.WriteLine("      Run the selected scenarios against the prepared campaign.");
        writer.WriteLine();
        writer.WriteLine("  report");
        writer.WriteLine("      Print the verdicts recorded so far.");
        writer.WriteLine();
        writer.WriteLine("  qualify [--required <id>]... | qualify --dry-run");
        writer.WriteLine("      Write release-verification.json, or plan a run without touching anything.");
        return exitCode;
    }
}
