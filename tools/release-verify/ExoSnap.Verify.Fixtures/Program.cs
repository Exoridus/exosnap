using System.Diagnostics;
using System.Globalization;

namespace ExoSnap.Verify.Fixtures;

/// <summary>
/// A child process that misbehaves on request.
/// </summary>
/// <remarks>
/// Every hostile case the process runner has to survive needs a real executable
/// that can be copied to a path containing spaces, an ampersand or non-ASCII
/// characters. A shell would not do: its own quoting rules would be what the test
/// measured. Nothing here is shipped, and nothing here touches the machine.
/// </remarks>
public static class Program
{
    /// <summary>Entry point.</summary>
    public static int Main(string[] args)
    {
        var exitCode = 0;
        for (var index = 0; index < args.Length; index++)
        {
            switch (args[index])
            {
                case "--stdout":
                    Console.Out.WriteLine(Next(args, ref index));
                    break;

                case "--stderr":
                    Console.Error.WriteLine(Next(args, ref index));
                    break;

                case "--exit":
                    exitCode = int.Parse(Next(args, ref index), CultureInfo.InvariantCulture);
                    break;

                case "--sleep":
                    Thread.Sleep(int.Parse(Next(args, ref index), CultureInfo.InvariantCulture));
                    break;

                case "--flood-stderr":
                    Flood(Console.Error, int.Parse(Next(args, ref index), CultureInfo.InvariantCulture));
                    break;

                case "--json":
                    Console.Out.WriteLine("{\"ok\":true,\"value\":42}");
                    break;

                case "--bad-json":
                    Console.Out.WriteLine("{\"ok\":true,");
                    break;

                case "--decimal":
                    // Formatted in the named culture on purpose: a tool that
                    // reports 3,5 where the parser expects 3.5 is the failure this
                    // exists to reproduce.
                    Console.Out.WriteLine((3.5).ToString("F1", CultureInfo.GetCultureInfo(Next(args, ref index))));
                    break;

                case "--decimal-json":
                    Console.Out.WriteLine(
                        "{\"drift\":" + (3.5).ToString("F1", CultureInfo.GetCultureInfo(Next(args, ref index))) + "}");
                    break;

                case "--print-args":
                    foreach (var argument in args)
                    {
                        Console.Out.WriteLine(argument);
                    }

                    break;

                case "--print-env":
                    Console.Out.WriteLine(Environment.GetEnvironmentVariable(Next(args, ref index)) ?? "(unset)");
                    break;

                case "--print-cwd":
                    Console.Out.WriteLine(Environment.CurrentDirectory);
                    break;

                case "--echo-stdin":
                    Console.Out.WriteLine(Console.In.ReadToEnd().Trim());
                    break;

                case "--spawn-child":
                    Console.Out.WriteLine(SpawnChild(Next(args, ref index)));
                    break;

                default:
                    break;
            }
        }

        Console.Out.Flush();
        Console.Error.Flush();
        return exitCode;
    }

    private static string Next(string[] args, ref int index)
    {
        index++;
        return index < args.Length ? args[index] : string.Empty;
    }

    private static void Flood(TextWriter writer, int lines)
    {
        for (var line = 0; line < lines; line++)
        {
            writer.WriteLine(new string('x', 512));
        }
    }

    // The grandchild gets its own redirected pipes so it does not inherit this
    // process's standard output. Without that it would hold the parent's reader
    // open long after this process exited, and the test would be measuring pipe
    // inheritance rather than the job object.
    private static int SpawnChild(string milliseconds)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = Environment.ProcessPath ?? "dotnet",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        startInfo.ArgumentList.Add("--sleep");
        startInfo.ArgumentList.Add(milliseconds);

        using var child = Process.Start(startInfo);
        return child?.Id ?? -1;
    }
}
