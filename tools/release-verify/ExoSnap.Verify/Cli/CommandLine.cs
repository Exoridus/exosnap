using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Cli;

/// <summary>
/// A parsed command line: one verb, then options and positional values.
/// </summary>
/// <remarks>
/// Hand-written rather than taken from a package. The harness has three commands
/// and a handful of options, and a parser this small is one fewer dependency
/// between a release decision and the code that produced it.
/// </remarks>
public sealed class CommandLine
{
    private readonly Dictionary<string, List<string>> options;

    private CommandLine(string verb, Dictionary<string, List<string>> options, IReadOnlyList<string> positional)
    {
        this.Verb = verb;
        this.options = options;
        this.Positional = new ReadOnlyCollection<string>([.. positional]);
    }

    /// <summary>The verb, or an empty string when none was given.</summary>
    public string Verb { get; }

    /// <summary>Values that were not attached to an option.</summary>
    public ReadOnlyCollection<string> Positional { get; }

    /// <summary>
    /// Parses arguments in the form <c>verb --option value --flag positional</c>.
    /// An option repeated more than once keeps every value.
    /// </summary>
    public static CommandLine Parse(IReadOnlyList<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(arguments);

        var verb = arguments.Count > 0 && !arguments[0].StartsWith('-') ? arguments[0] : string.Empty;
        var options = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
        var positional = new List<string>();

        for (var index = verb.Length > 0 ? 1 : 0; index < arguments.Count; index++)
        {
            var argument = arguments[index];
            if (!argument.StartsWith("--", StringComparison.Ordinal))
            {
                positional.Add(argument);
                continue;
            }

            var name = argument[2..];
            var value = string.Empty;
            var equals = name.IndexOf('=', StringComparison.Ordinal);
            if (equals >= 0)
            {
                value = name[(equals + 1)..];
                name = name[..equals];
            }
            else if (index + 1 < arguments.Count && !arguments[index + 1].StartsWith("--", StringComparison.Ordinal))
            {
                value = arguments[index + 1];
                index++;
            }

            if (!options.TryGetValue(name, out var values))
            {
                values = [];
                options[name] = values;
            }

            values.Add(value);
        }

        return new CommandLine(verb, options, positional);
    }

    /// <summary>Whether an option was given at all, with or without a value.</summary>
    public bool HasFlag(string name) => this.options.ContainsKey(name);

    /// <summary>The last value given for an option, or a fallback.</summary>
    public string Value(string name, string fallback) =>
        this.options.TryGetValue(name, out var values) && values.Count > 0 && values[^1].Length > 0
            ? values[^1]
            : fallback;

    /// <summary>Every value given for an option, in order.</summary>
    public ReadOnlyCollection<string> Values(string name) =>
        new([.. this.options.TryGetValue(name, out var values) ? values.Where(value => value.Length > 0) : []]);
}
