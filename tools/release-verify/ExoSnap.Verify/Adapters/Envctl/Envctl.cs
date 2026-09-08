using System.Text.Json;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Adapters.Envctl;

/// <summary>
/// The real <c>exosnap-envctl</c>, driven through the containing process runner.
/// </summary>
/// <remarks>
/// The <c>--profile</c> pair goes on every invocation that resolves an alias,
/// including the restore. Omitting it there once meant no aliases loaded, every
/// device answered "not present", and the restore gave up on hardware that was
/// plainly attached - on the one path where giving up is most expensive.
/// </remarks>
public sealed class Envctl : IEnvctl
{
    /// <summary>The name this adapter reports itself under in a contract violation.</summary>
    public const string ToolName = "exosnap-envctl";

    /// <summary>The environment variable that pins an explicit build of the tool.</summary>
    public const string PathVariable = "EXOSNAP_ENVCTL";

    private static readonly TimeSpan CommandTimeout = TimeSpan.FromMinutes(2);

    private readonly ProcessRunner runner;
    private readonly string? aliasProfile;

    /// <summary>Creates an adapter for a resolved tool, or for none at all.</summary>
    /// <param name="runner">The runner every child process goes through.</param>
    /// <param name="executablePath">Absolute path of the tool, or null when it is not built.</param>
    /// <param name="aliasProfile">The machine-local alias profile, or null to use the tool's default.</param>
    public Envctl(ProcessRunner runner, string? executablePath, string? aliasProfile = null)
    {
        ArgumentNullException.ThrowIfNull(runner);
        this.runner = runner;
        this.ExecutablePath = executablePath;
        this.aliasProfile = aliasProfile;
    }

    /// <inheritdoc/>
    public bool Available => this.ExecutablePath is not null;

    /// <inheritdoc/>
    public string? ExecutablePath { get; }

    /// <inheritdoc/>
    public Task<EnvctlResponse> DescribeAsync(CancellationToken cancellationToken) =>
        this.InvokeAsync(strict: true, ["describe"], withProfile: false, cancellationToken);

    /// <inheritdoc/>
    public Task<EnvctlResponse> SnapshotAsync(CancellationToken cancellationToken) =>
        this.InvokeAsync(strict: true, ["snapshot"], withProfile: true, cancellationToken);

    /// <inheritdoc/>
    public Task<EnvctlResponse> ResolveAliasesAsync(CancellationToken cancellationToken) =>
        this.InvokeAsync(strict: false, ["resolve-aliases"], withProfile: true, cancellationToken);

    /// <inheritdoc/>
    public Task<EnvctlResponse> ListModesAsync(string deviceAlias, CancellationToken cancellationToken)
    {
        List<string> arguments = ["list-modes"];
        if (!string.IsNullOrWhiteSpace(deviceAlias))
        {
            arguments.Add("--alias");
            arguments.Add(deviceAlias);
        }

        return this.InvokeAsync(strict: false, arguments, withProfile: true, cancellationToken);
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> RecoverAsync(string journalPath, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(journalPath);
        return this.InvokeAsync(strict: true, ["recover", "--journal", journalPath], withProfile: true, cancellationToken);
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> BeginAsync(
        string scenario,
        string runId,
        string journalPath,
        string desiredFilePath,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scenario);
        ArgumentException.ThrowIfNullOrWhiteSpace(runId);
        ArgumentException.ThrowIfNullOrWhiteSpace(journalPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(desiredFilePath);

        return this.InvokeAsync(
            strict: false,
            [
                "begin",
                "--scenario", scenario,
                "--run-id", runId,
                "--journal", journalPath,
                "--desired", desiredFilePath,
            ],
            withProfile: true,
            cancellationToken);
    }

    /// <inheritdoc/>
    public Task<EnvctlResponse> RestoreAsync(string journalPath, CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(journalPath);
        return this.InvokeAsync(strict: false, ["restore", "--journal", journalPath], withProfile: true, cancellationToken);
    }

    /// <summary>
    /// Reads a document the tool wrote earlier, so a contract test and the real
    /// adapter share one parser.
    /// </summary>
    /// <exception cref="ToolContractException">The text is not the document the tool promises.</exception>
    public static EnvctlResponse ParseDocument(string json, int exitCode = 0)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(json);
        try
        {
            using var document = JsonDocument.Parse(json);
            return Describe(document.RootElement.Clone(), json, exitCode);
        }
        catch (JsonException exception)
        {
            throw ToolContractException.ForTool(ToolName, "the document is not valid JSON", exception);
        }
    }

    private static EnvctlResponse Describe(JsonElement root, string raw, int exitCode)
    {
        var ok = root.ValueKind == JsonValueKind.Object &&
                 root.TryGetProperty("ok", out var okValue) &&
                 okValue.ValueKind == JsonValueKind.True;

        return new EnvctlResponse(ok, Text(root, "state"), Text(root, "errorCode"), Text(root, "error"), exitCode, root, raw);
    }

    private static string Text(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private async Task<EnvctlResponse> InvokeAsync(
        bool strict,
        IReadOnlyList<string> arguments,
        bool withProfile,
        CancellationToken cancellationToken)
    {
        var tool = this.ExecutablePath ??
                   throw ToolContractException.ForTool(ToolName, "the tool is not built on this machine");

        List<string> full = [.. arguments];
        if (withProfile && !string.IsNullOrWhiteSpace(this.aliasProfile))
        {
            full.Add("--profile");
            full.Add(this.aliasProfile);
        }

        var result = await this.runner
            .RunAsync(new ProcessRunRequest(tool, full) { Timeout = CommandTimeout }, cancellationToken)
            .ConfigureAwait(false);

        var invocation = string.Join(' ', full);
        if (result.TimedOut)
        {
            throw ToolContractException.ForTool(ToolName, $"'{invocation}' did not finish within its deadline");
        }

        if (string.IsNullOrWhiteSpace(result.StandardOutput))
        {
            throw ToolContractException.ForTool(
                ToolName,
                $"'{invocation}' exited {result.ExitCode} without writing any output");
        }

        EnvctlResponse response;
        try
        {
            using var document = JsonDocument.Parse(result.StandardOutput);
            response = Describe(document.RootElement.Clone(), result.StandardOutput, result.ExitCode);
        }
        catch (JsonException exception)
        {
            throw ToolContractException.ForTool(
                ToolName, $"'{invocation}' produced unparseable output", exception);
        }

        if (strict && result.ExitCode != 0)
        {
            var detail = response.Error.Length > 0 ? response.Error : result.StandardOutput.Trim();
            throw ToolContractException.ForTool(ToolName, $"'{invocation}' failed (exit {result.ExitCode}): {detail}");
        }

        return response;
    }
}
