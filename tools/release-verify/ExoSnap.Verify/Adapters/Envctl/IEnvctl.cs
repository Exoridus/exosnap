using System.Text.Json;

namespace ExoSnap.Verify.Adapters.Envctl;

/// <summary>
/// One answer from <c>exosnap-envctl</c>.
/// </summary>
/// <param name="Ok">The tool's own <c>ok</c> flag; false when the field was absent.</param>
/// <param name="State">The transaction state the tool reported, or an empty string.</param>
/// <param name="ErrorCode">Structured refusal cause, or an empty string.</param>
/// <param name="Error">Human-readable refusal cause, or an empty string.</param>
/// <param name="ExitCode">The tool's exit code, which for several subcommands is a verdict rather than a fault.</param>
/// <param name="Body">The whole document, for the fields a caller reads directly.</param>
/// <param name="RawJson">Exactly what the tool wrote, so a verdict can cite it.</param>
public sealed record EnvctlResponse(
    bool Ok,
    string State,
    string ErrorCode,
    string Error,
    int ExitCode,
    JsonElement Body,
    string RawJson)
{
    /// <summary>A property of the document, or the default element when it is absent.</summary>
    public JsonElement Field(string name) =>
        this.Body.ValueKind == JsonValueKind.Object && this.Body.TryGetProperty(name, out var value)
            ? value
            : default;

    /// <summary>The elements of an array property, or an empty sequence.</summary>
    public IEnumerable<JsonElement> Array(string name)
    {
        var field = this.Field(name);
        return field.ValueKind == JsonValueKind.Array ? field.EnumerateArray() : [];
    }
}

/// <summary>
/// The Windows environment, as the only thing allowed to change it sees it.
/// </summary>
/// <remarks>
/// ExoSnap itself never grows a setter for HDR, refresh rate or the default audio
/// endpoint: a recording application that can reconfigure the machine is a different
/// product with a different threat model. Every read and every mutation goes through
/// <c>exosnap-envctl</c>, which is a test-only executable, never installed and never
/// linked into the product.
///
/// The subcommands split into two groups, and the split is why a non-zero exit is not
/// uniformly an error. <c>describe</c> and <c>snapshot</c> answer questions that
/// always have an answer, so a non-zero exit there is a fault. <c>resolve-aliases</c>,
/// <c>list-modes</c>, <c>begin</c> and <c>restore</c> exit non-zero to state a verdict
/// - an unbound alias, a display that offers no such mode, an outstanding restore -
/// and the body carries it.
/// </remarks>
public interface IEnvctl
{
    /// <summary>Whether the tool is built on this machine at all.</summary>
    bool Available { get; }

    /// <summary>Absolute path of the resolved tool, or null.</summary>
    string? ExecutablePath { get; }

    /// <summary>The capability classification table: what is readable, mutable, or a person's own act.</summary>
    Task<EnvctlResponse> DescribeAsync(CancellationToken cancellationToken);

    /// <summary>The full read-only environment snapshot, keyed by stable Windows identifiers.</summary>
    Task<EnvctlResponse> SnapshotAsync(CancellationToken cancellationToken);

    /// <summary>Binds machine-local device aliases to stable Windows identifiers.</summary>
    Task<EnvctlResponse> ResolveAliasesAsync(CancellationToken cancellationToken);

    /// <summary>The display modes a refresh-rate transaction may target.</summary>
    Task<EnvctlResponse> ListModesAsync(string deviceAlias, CancellationToken cancellationToken);

    /// <summary>Restores whatever a killed run left in the journal, and reports whether mutation is allowed again.</summary>
    Task<EnvctlResponse> RecoverAsync(string journalPath, CancellationToken cancellationToken);

    /// <summary>
    /// Opens a transaction: snapshot, journal, validate, apply the minimal delta, read
    /// back, and verify. Reaching a successful answer means the applied state was
    /// already confirmed by a read-back.
    /// </summary>
    Task<EnvctlResponse> BeginAsync(
        string scenario,
        string runId,
        string journalPath,
        string desiredFilePath,
        CancellationToken cancellationToken);

    /// <summary>Puts back exactly what the machine had, and reports whether the read-back agreed.</summary>
    Task<EnvctlResponse> RestoreAsync(string journalPath, CancellationToken cancellationToken);
}
