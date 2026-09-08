namespace ExoSnap.Verify.Models;

/// <summary>Which machine a scenario needs, and what it is allowed to disturb there.</summary>
public enum ScenarioIsolation
{
    /// <summary>No device, no real UI, no registry. Runs anywhere, including CI.</summary>
    Hermetic,

    /// <summary>A real Windows desktop with the real binary, but no special hardware.</summary>
    Desktop,

    /// <summary>A disposable operating system: Windows Sandbox or a throwaway VM.</summary>
    DisposableOs,

    /// <summary>A machine with declared hardware: a specific GPU, panel, or audio endpoint.</summary>
    HardwareLab,
}

/// <summary>What privilege the scenario needs to reach its subject.</summary>
public enum ScenarioPrivilege
{
    /// <summary>Runs entirely at standard integrity.</summary>
    Standard,

    /// <summary>Needs an elevated worker process of its own; the parent never inspects elevated UI.</summary>
    Elevated,

    /// <summary>Crosses the Secure Desktop, which is never automated and always confirmed by a person.</summary>
    SecureDesktop,
}

/// <summary>How much of the scenario a person has to carry out.</summary>
public enum ScenarioInteraction
{
    /// <summary>Start to finish without a person.</summary>
    Automated,

    /// <summary>A person performs a step the machine has no documented API for.</summary>
    OperatorAssisted,

    /// <summary>A person is the oracle, because nothing the harness can capture shows the subject.</summary>
    OperatorJudged,
}

/// <summary>The weakest mechanism a scenario actually relies on.</summary>
/// <remarks>
/// Named for the mechanism rather than for the amount of automation: a scenario
/// that drives everything itself but reads one value off a screen is only as
/// trustworthy as that reading.
/// </remarks>
public enum ScenarioLayer
{
    /// <summary>No person, and every assertion goes through a typed surface.</summary>
    FullAuto,

    /// <summary>Automated, and every product assertion goes through the control channel.</summary>
    ControlChannel,

    /// <summary>Automated except for one step a person performs.</summary>
    SemiAuto,

    /// <summary>Crosses a Secure Desktop prompt a person must answer.</summary>
    Secure,

    /// <summary>A person changes physical hardware.</summary>
    ManualPhysical,

    /// <summary>A person judges what is on the real desktop.</summary>
    ManualVisual,
}

/// <summary>How a capability requirement compares against the measured value.</summary>
public enum CapabilityOperator
{
    /// <summary>Exact match, case-insensitive.</summary>
    Equals,

    /// <summary>Anything but this value; an unknown capability never satisfies it.</summary>
    NotEquals,

    /// <summary>Numeric comparison; a non-numeric measured value never satisfies it.</summary>
    AtLeast,
}

/// <summary>One condition a machine must satisfy before a scenario may run.</summary>
/// <param name="Key">Capability key, as written into the machine capability document.</param>
/// <param name="Operator">How the measured value is compared.</param>
/// <param name="Value">The value the requirement is written against.</param>
public sealed record CapabilityRequirement(string Key, CapabilityOperator Operator, string Value)
{
    /// <summary>An exact-match requirement.</summary>
    public static CapabilityRequirement Is(string key, string value) =>
        new(key, CapabilityOperator.Equals, value);

    /// <summary>A numeric lower bound.</summary>
    public static CapabilityRequirement AtLeast(string key, string value) =>
        new(key, CapabilityOperator.AtLeast, value);

    /// <summary>
    /// The message a scenario is reported with when this requirement is not met.
    /// </summary>
    public string UnsatisfiedMessage => $"capability {this.Key}={this.Value} not satisfied";
}

/// <summary>
/// Everything about a scenario that is true before it runs.
/// </summary>
/// <param name="Id">Stable identifier, cited by the report and the checklist.</param>
/// <param name="Title">One line, in the terms the gate is written in.</param>
/// <param name="Class">Grouping, and the unit a selection opts into.</param>
/// <param name="Layer">The weakest mechanism the scenario actually uses.</param>
/// <param name="Requires">Capability conditions this machine must satisfy.</param>
/// <param name="Isolation">Which machine the scenario needs.</param>
/// <param name="Privilege">What privilege it needs to reach its subject.</param>
/// <param name="Interaction">How much of it a person carries out.</param>
/// <param name="Mutates">Environment properties it changes and must restore.</param>
/// <param name="Oracle">What decides the verdict, beyond ExoSnap's own reports.</param>
/// <param name="OptIn">Excluded from a default sweep because it is long or disruptive.</param>
/// <param name="Source">Where the requirement is written down, never restated here.</param>
/// <param name="DependsOn">Scenario ids that must have run first.</param>
public sealed record ScenarioDescriptor(
    string Id,
    string Title,
    string Class,
    ScenarioLayer Layer,
    IReadOnlyList<CapabilityRequirement> Requires,
    ScenarioIsolation Isolation,
    ScenarioPrivilege Privilege,
    ScenarioInteraction Interaction,
    IReadOnlyList<string> Mutates,
    IReadOnlyList<string> Oracle,
    bool OptIn,
    string Source,
    IReadOnlyList<string> DependsOn);
