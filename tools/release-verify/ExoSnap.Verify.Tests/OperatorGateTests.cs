using ExoSnap.Verify.Gates;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// A console that answers from a script and remembers what it was shown.
/// </summary>
internal sealed class ScriptedOperator(params string?[] answers) : IOperatorConsole
{
    private readonly Queue<string?> answers = new(answers);

    public List<string> Shown { get; } = [];

    public void Write(string text) => this.Shown.Add(text);

    public string? ReadLine() => this.answers.Count > 0 ? this.answers.Dequeue() : null;
}

/// <summary>
/// The protocol for asking a person, and the three ways it must not pass.
/// </summary>
public sealed class OperatorGateTests
{
    private static OperatorStep Step(OperatorAction action) => new(
        "REL-AUD-DEGRADE-001",
        "Ziehe jetzt das USB-Headset ab.",
        action,
        Why: "no API removes a physical endpoint",
        Expected: "the recording keeps running and reports silence",
        VerifyDescription: "pipeline.snapshot plus ffprobe on the finished file");

    [Theory]
    [InlineData(OperatorAction.RunnerActsOnConfirm)]
    [InlineData(OperatorAction.OperatorActsThenConfirms)]
    public void EnterConfirmsTheTwoActionForms(OperatorAction action)
    {
        var gate = new OperatorGate(new ScriptedOperator(""));

        Assert.Equal(OperatorOutcome.Confirmed, gate.Ask(Step(action)));
    }

    [Theory]
    [InlineData(OperatorAction.RunnerActsOnConfirm)]
    [InlineData(OperatorAction.OperatorActsThenConfirms)]
    public void TheTwoActionFormsCanBeRefused(OperatorAction action)
    {
        var gate = new OperatorGate(new ScriptedOperator("n"));

        Assert.Equal(OperatorOutcome.Declined, gate.Ask(Step(action)));
    }

    [Fact]
    public void AJudgementCannotBeAnsweredByTheReflexThatAnswersTheOthers()
    {
        // Enter is what a person presses without reading. For a gate whose whole
        // content is the person's verdict, accepting it would record a verdict
        // nobody gave -- so the question is asked again instead.
        var console = new ScriptedOperator("", "", "ok");
        var gate = new OperatorGate(console);

        Assert.Equal(OperatorOutcome.Confirmed, gate.Ask(Step(OperatorAction.OperatorJudges)));
        Assert.Equal(3, console.Shown.Count(line => line.Contains("[ok]", StringComparison.Ordinal)));
    }

    [Fact]
    public void AJudgementCanBeNegative()
    {
        var gate = new OperatorGate(new ScriptedOperator("bad"));

        Assert.Equal(OperatorOutcome.Declined, gate.Ask(Step(OperatorAction.OperatorJudges)));
    }

    [Fact]
    public void AnUnattendedRunNeverPassesAnOperatorStep()
    {
        var gate = new OperatorGate(console: null);

        Assert.Equal(OperatorOutcome.NoOperator, gate.Ask(Step(OperatorAction.OperatorActsThenConfirms)));
    }

    [Fact]
    public void InputThatEndsWithoutAnAnswerIsNotAYes()
    {
        var gate = new OperatorGate(new ScriptedOperator([null]));

        Assert.Equal(OperatorOutcome.NoOperator, gate.Ask(Step(OperatorAction.OperatorActsThenConfirms)));
    }

    [Fact]
    public void AttestationIsCheckedBeforeInteractivity()
    {
        // --attest on an unattended run is the whole point: it says the CALLER
        // performed the action. The runner still verifies the consequence, which
        // is why this is not a verdict.
        var gate = new OperatorGate(console: null, attested: ["rel-aud-degrade-001"]);

        Assert.Equal(OperatorOutcome.Attested, gate.Ask(Step(OperatorAction.OperatorActsThenConfirms)));
    }

    [Fact]
    public void TheDetailIsBehindTheQuestionMarkAndNotInThePrompt()
    {
        var console = new ScriptedOperator("?", "");
        var gate = new OperatorGate(console);

        Assert.Equal(OperatorOutcome.Confirmed, gate.Ask(Step(OperatorAction.OperatorActsThenConfirms)));
        Assert.DoesNotContain(console.Shown[0], "no API removes a physical endpoint", StringComparison.Ordinal);
        Assert.Contains(console.Shown, line => line.Contains("no API removes a physical endpoint",
            StringComparison.Ordinal));
    }

    [Fact]
    public void EachFormSaysWhoActsNext()
    {
        Assert.Contains("startet jetzt", OperatorGate.PromptFor(OperatorAction.RunnerActsOnConfirm),
            StringComparison.Ordinal);
        Assert.Contains("wenn erledigt", OperatorGate.PromptFor(OperatorAction.OperatorActsThenConfirms),
            StringComparison.Ordinal);
        Assert.DoesNotContain("Enter", OperatorGate.PromptFor(OperatorAction.OperatorJudges),
            StringComparison.Ordinal);
    }
}
