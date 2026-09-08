using System.Globalization;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Cli;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Json;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify;

/// <summary>
/// The release verification harness.
/// </summary>
/// <remarks>
/// Nothing here promotes, tags, or publishes anything. The most a run produces is
/// a qualification record saying the candidate may be promoted; who acts on that
/// is a decision outside this program.
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
        var output = command.Value("out", "machine-capabilities.json");
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

        Console.WriteLine($"catalog {catalog.Version}, {catalog.Scenarios.Count} scenarios");
        Console.WriteLine();
        foreach (var descriptor in catalog.Descriptors)
        {
            var requires = descriptor.Requires.Count == 0
                ? "-"
                : string.Join(", ", descriptor.Requires.Select(Format));
            Console.WriteLine($"{descriptor.Id,-26} {descriptor.Class,-15} {descriptor.Isolation,-13} " +
                              $"{descriptor.Interaction,-17} {(descriptor.OptIn ? "opt-in" : "default"),-8} {requires}");
            Console.WriteLine($"{string.Empty,-26} {descriptor.Title}");
        }

        return ExitOk;
    }

    private static int Qualify(CommandLine command)
    {
        if (!command.HasFlag("dry-run"))
        {
            Console.Error.WriteLine(
                "Only 'qualify --dry-run' is available in this revision: no gate has been migrated yet, " +
                "so a run would produce a record backed by nothing.");
            return ExitUsage;
        }

        var catalog = ReleaseCatalog.Create();
        var capabilities = new MachineCapabilityProbe().ProbeSet();
        var selection = new ScenarioSelection(
            IncludeOptIn: command.HasFlag("include-opt-in"),
            Classes: command.Values("class"),
            Ids: command.Values("id"));

        var plan = new VerifyEngine(catalog).Plan(selection, capabilities);
        var rcTag = command.Value("rc", command.Positional.Count > 0 ? command.Positional[0] : "(no rc)");

        Console.WriteLine($"qualify --dry-run {rcTag}");
        Console.WriteLine($"catalog {plan.CatalogVersion}, harness {Qualification.HarnessIdentity().Version}");
        Console.WriteLine();

        foreach (var planned in plan.Scenarios)
        {
            var outcome = planned.Outcome?.ToString() ?? "WOULD_RUN";
            Console.WriteLine($"{planned.Descriptor.Id,-26} {outcome,-20} {planned.Message}");
        }

        var wouldRun = plan.Scenarios.Count(planned => planned.WouldRun);
        Console.WriteLine();
        Console.WriteLine($"{wouldRun} of {plan.Scenarios.Count} scenarios would run on this machine.");
        Console.WriteLine(
            "Nothing ran: every migrated body would report Skipped ('not migrated') in this revision.");

        // A dry run never qualifies anything. Saying so out loud costs one line
        // and removes the only way this output could be mistaken for a verdict.
        Console.WriteLine("NOT QUALIFIED (dry run produces no qualification record)");
        return ExitNotQualified;
    }

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
        writer.WriteLine("  qualify --dry-run [--rc <tag>] [--include-opt-in] [--class <c>] [--id <id>]");
        writer.WriteLine("      Evaluate the catalog against this machine without running anything.");
        return exitCode;
    }
}
