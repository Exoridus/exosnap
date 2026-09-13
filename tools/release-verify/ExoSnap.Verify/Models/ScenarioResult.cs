namespace ExoSnap.Verify.Models;

/// <summary>
/// What one scenario concluded, why, and what it can show for it.
/// </summary>
/// <param name="Outcome">The verdict.</param>
/// <param name="Message">One sentence in the terms the scenario is written in.</param>
/// <param name="Evidence">Files backing the verdict; empty when the verdict needs none.</param>
public sealed record ScenarioResult(ScenarioOutcome Outcome, string Message, Evidence[] Evidence)
{
    private static readonly Evidence[] NoEvidence = [];

    /// <summary>
    /// Why the evidence for this scenario did not reach the host, or an empty string
    /// when it did or none was asked for.
    /// </summary>
    /// <remarks>
    /// A separate fact from the outcome, because they are separate questions and only
    /// the promotion contract joins them. A disposable-OS run can assert everything it
    /// was asked to and still lose the logs that show it: the product verdict is real,
    /// and a record that cannot be looked into has not produced what promotion asks
    /// for. Folding the two would either hide the loss or turn it into a product
    /// failure, and it is neither.
    /// </remarks>
    public string EvidenceGap { get; init; } = string.Empty;

    /// <summary>Whether the evidence this scenario produced reached the host.</summary>
    public bool EvidenceComplete => this.EvidenceGap.Length == 0;

    /// <summary>The product behaved as required.</summary>
    public static ScenarioResult Pass(string message, params Evidence[] evidence) =>
        new(ScenarioOutcome.Pass, message, evidence.Length == 0 ? NoEvidence : evidence);

    /// <summary>The product did not behave as required. Reserved for exactly that.</summary>
    public static ScenarioResult Fail(string message, params Evidence[] evidence) =>
        new(ScenarioOutcome.Fail, message, evidence.Length == 0 ? NoEvidence : evidence);

    /// <summary>The scenario could not be carried out, so nothing was learned about the product.</summary>
    public static ScenarioResult InfrastructureError(string message, params Evidence[] evidence) =>
        new(ScenarioOutcome.InfrastructureError, message, evidence.Length == 0 ? NoEvidence : evidence);

    /// <summary>A harness precondition was not met and the scenario never started.</summary>
    public static ScenarioResult Blocked(string message) =>
        new(ScenarioOutcome.Blocked, message, NoEvidence);

    /// <summary>This machine does not satisfy a capability the scenario requires.</summary>
    public static ScenarioResult Unavailable(string message) =>
        new(ScenarioOutcome.Unavailable, message, NoEvidence);

    /// <summary>Postponed by decision rather than by machine state.</summary>
    public static ScenarioResult Deferred(string message) =>
        new(ScenarioOutcome.Deferred, message, NoEvidence);

    /// <summary>Not selected for this run.</summary>
    public static ScenarioResult Skipped(string message) =>
        new(ScenarioOutcome.Skipped, message, NoEvidence);

    /// <summary>A recorded verdict that no longer describes what is in front of it.</summary>
    public static ScenarioResult Stale(string message) =>
        new(ScenarioOutcome.Stale, message, NoEvidence);

    /// <summary>
    /// Whether this verdict prevents a release from being qualified. Only
    /// <see cref="ScenarioOutcome.Pass"/> does not; the caller decides which
    /// scenarios are required before consulting this, so a scenario nobody asked
    /// for never reaches it.
    /// </summary>
    public bool BlocksQualification => this.Outcome != ScenarioOutcome.Pass;
}
