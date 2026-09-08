using System.Text.Json;

namespace ExoSnap.Verify.Processes;

/// <summary>
/// An external tool did not answer in the shape its contract promises.
/// </summary>
/// <remarks>
/// Always an infrastructure error, never a product verdict. Unparseable output
/// means the observation was never made, and reporting it as a failure would
/// claim a defect nobody measured.
/// </remarks>
public sealed class ToolContractException : Exception
{
    /// <summary>Creates the exception with a default message.</summary>
    public ToolContractException()
        : base("An external tool did not answer in the shape its contract promises.")
    {
        this.Tool = string.Empty;
    }

    /// <summary>Creates the exception with a message.</summary>
    public ToolContractException(string message)
        : base(message)
    {
        this.Tool = string.Empty;
    }

    /// <summary>Creates the exception with a message and a cause.</summary>
    public ToolContractException(string message, Exception? innerException)
        : base(message, innerException)
    {
        this.Tool = string.Empty;
    }

    private ToolContractException(string tool, string message, Exception? innerException)
        : base(message, innerException)
    {
        this.Tool = tool;
    }

    /// <summary>The tool whose output could not be read.</summary>
    public string Tool { get; }

    /// <summary>Names the tool whose output could not be read.</summary>
    public static ToolContractException ForTool(string tool, string detail, Exception? innerException = null) =>
        new(tool, $"{tool}: {detail}", innerException);
}

/// <summary>Reads the structured output of an external tool.</summary>
public static class ToolContract
{
    /// <summary>
    /// Parses a tool's standard output as a JSON document.
    /// </summary>
    /// <exception cref="ToolContractException">
    /// The tool wrote nothing, or wrote something that is not JSON.
    /// </exception>
    public static JsonDocument ParseJson(ProcessRunResult result, string tool)
    {
        ArgumentNullException.ThrowIfNull(result);
        ArgumentException.ThrowIfNullOrWhiteSpace(tool);

        if (result.TimedOut)
        {
            throw ToolContractException.ForTool(tool, "the tool did not finish within its deadline");
        }

        if (string.IsNullOrWhiteSpace(result.StandardOutput))
        {
            throw ToolContractException.ForTool(tool, "the tool exited without writing any output");
        }

        try
        {
            return JsonDocument.Parse(result.StandardOutput);
        }
        catch (JsonException exception)
        {
            throw ToolContractException.ForTool(tool, "the tool wrote output that is not valid JSON", exception);
        }
    }
}
