namespace ExoSnap.Verify.Gates;

/// <summary>
/// Who acts next. The whole point of the distinction.
/// </summary>
/// <remarks>
/// One campaign lost three gates to a prompt that read <c>[y] yes [n] no</c> and
/// meant "start it now" for some gates and "I already did it" for others. Each
/// form here says which, and the judgement form is deliberately answered with
/// letters so the reflex that answers the other two cannot answer it.
/// </remarks>
public enum OperatorAction
{
    /// <summary>The runner acts on confirmation; nothing has happened yet.</summary>
    RunnerActsOnConfirm,

    /// <summary>The person acts first and confirms afterwards.</summary>
    OperatorActsThenConfirms,

    /// <summary>The person's verdict IS the measurement; there is nothing else to ask.</summary>
    OperatorJudges,
}

/// <summary>
/// How a request for a person ended.
/// </summary>
public enum OperatorOutcome
{
    /// <summary>The step was carried out, or the judgement was positive.</summary>
    Confirmed,

    /// <summary>The person refused the step, or judged it negative.</summary>
    Declined,

    /// <summary>The caller attested the step beforehand. The consequence still gets verified.</summary>
    Attested,

    /// <summary>Nobody is at the machine. Never a pass, and never a failure either.</summary>
    NoOperator,
}

/// <summary>
/// One request for a person, as the protocol wants it.
/// </summary>
/// <param name="Id">The scenario id, which is also what <c>--attest</c> names.</param>
/// <param name="Line">The single line the person is shown. Not a paragraph.</param>
/// <param name="Action">Who acts next.</param>
/// <param name="Why">Why no API can do this. Shown only on request.</param>
/// <param name="Expected">What the person should see. Shown only on request.</param>
/// <param name="VerifyDescription">How the runner checks the consequence. Shown only on request.</param>
public sealed record OperatorStep(
    string Id,
    string Line,
    OperatorAction Action,
    string Why = "",
    string Expected = "",
    string VerifyDescription = "");

/// <summary>
/// The console seam. A dry run replaces it, which is what lets every gate be
/// seen answered both ways before a person is shown one.
/// </summary>
public interface IOperatorConsole
{
    /// <summary>Shows one line. No trailing newline is implied.</summary>
    void Write(string text);

    /// <summary>The person's answer, or null when the input has ended.</summary>
    string? ReadLine();
}

/// <summary>
/// Asks a person for what no documented API reaches, and records what they said.
/// </summary>
/// <remarks>
/// Three rules this type exists to keep.
/// <para>
/// An unattended run never passes an operator step. It reports
/// <see cref="OperatorOutcome.NoOperator"/>, which a scenario turns into
/// Unavailable -- a verdict about the machine, not about the product.
/// </para>
/// <para>
/// Attestation is checked BEFORE interactivity, so <c>--attest ID</c> on an
/// unattended run means the caller performed the action, and the runner still
/// verifies the consequence. It is a statement about who acted, never a verdict.
/// </para>
/// <para>
/// Everything beyond the one line -- why this is manual, what to expect, how the
/// consequence is checked -- is behind <c>?</c> and is not printed by default.
/// Paragraphs before a prompt are how the meaning of the prompt gets lost.
/// </para>
/// </remarks>
public sealed class OperatorGate
{
    private readonly IOperatorConsole? console;
    private readonly HashSet<string> attested;

    /// <summary>
    /// Creates a gate. A null console is an unattended run.
    /// </summary>
    /// <param name="console">The console seam, or null when nobody is there.</param>
    /// <param name="attested">Scenario ids the caller says it performed itself.</param>
    public OperatorGate(IOperatorConsole? console, IEnumerable<string>? attested = null)
    {
        this.console = console;
        this.attested = new HashSet<string>(attested ?? [], StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>The prompt a step is shown with, without the trailing space.</summary>
    /// <param name="action">Who acts next.</param>
    public static string PromptFor(OperatorAction action) => action switch
    {
        OperatorAction.RunnerActsOnConfirm => "[Enter] startet jetzt  [n] überspringen  [?] Details",
        OperatorAction.OperatorActsThenConfirms => "[Enter] wenn erledigt  [n] nicht gemacht  [?] Details",
        OperatorAction.OperatorJudges => "[ok] wie beschrieben  [bad] nicht wie beschrieben  [?] Details",
        _ => throw new ArgumentOutOfRangeException(nameof(action)),
    };

    /// <summary>
    /// Asks for one step and returns what the person said.
    /// </summary>
    /// <param name="step">The step to ask for.</param>
    /// <returns>
    /// <see cref="OperatorOutcome.Attested"/> when the caller attested this id,
    /// <see cref="OperatorOutcome.NoOperator"/> when nobody is at the machine,
    /// otherwise the answer.
    /// </returns>
    public OperatorOutcome Ask(OperatorStep step)
    {
        ArgumentNullException.ThrowIfNull(step);

        if (this.attested.Contains(step.Id))
        {
            return OperatorOutcome.Attested;
        }

        if (this.console is null)
        {
            return OperatorOutcome.NoOperator;
        }

        while (true)
        {
            this.console.Write($"{step.Line}\n{PromptFor(step.Action)} ");
            var answer = this.console.ReadLine();
            if (answer is null)
            {
                // The input ended without an answer. An unanswered question is
                // not a yes, and a run that hit this had no operator after all.
                return OperatorOutcome.NoOperator;
            }

            switch (answer.Trim().ToLowerInvariant())
            {
                case "?":
                    this.WriteDetail(step);
                    continue;
                case "" when step.Action != OperatorAction.OperatorJudges:
                case "ok" when step.Action == OperatorAction.OperatorJudges:
                    return OperatorOutcome.Confirmed;
                case "n" when step.Action != OperatorAction.OperatorJudges:
                case "bad" when step.Action == OperatorAction.OperatorJudges:
                    return OperatorOutcome.Declined;
                default:
                    // A judgement answered with Enter, or a step answered with a
                    // word it does not take. Asking again is the only safe reading:
                    // guessing which the person meant is the defect this protocol
                    // was written against.
                    continue;
            }
        }
    }

    private void WriteDetail(OperatorStep step)
    {
        foreach (var (label, value) in new[]
                 {
                     ("why", step.Why), ("expected", step.Expected), ("verified by", step.VerifyDescription),
                 })
        {
            if (!string.IsNullOrWhiteSpace(value))
            {
                this.console!.Write($"  {label}: {value}\n");
            }
        }
    }
}
