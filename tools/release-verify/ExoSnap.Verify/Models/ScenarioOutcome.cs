namespace ExoSnap.Verify.Models;

/// <summary>
/// What a scenario concluded.
/// </summary>
/// <remarks>
/// <see cref="Fail"/> has exactly one meaning: ExoSnap is wrong. Everything that
/// went wrong on the way to an observation, from an unparseable tool output to a
/// lost named pipe to a harness timeout, is <see cref="InfrastructureError"/>. A
/// campaign carrying either is not releasable, but only one of them claims a
/// product defect, and a claim nobody measured is worse than no claim.
/// </remarks>
public enum ScenarioOutcome
{
    /// <summary>The product behaved as the scenario requires.</summary>
    Pass,

    /// <summary>The product did not behave as the scenario requires.</summary>
    Fail,

    /// <summary>The scenario could not be carried out; nothing was learned about the product.</summary>
    InfrastructureError,

    /// <summary>A precondition inside the harness was not met, so the scenario never started.</summary>
    Blocked,

    /// <summary>This machine does not satisfy a capability the scenario requires.</summary>
    Unavailable,

    /// <summary>Deliberately postponed to a later run, by decision rather than by machine state.</summary>
    Deferred,

    /// <summary>Not selected for this run.</summary>
    Skipped,

    /// <summary>A recorded verdict that no longer describes the artifact or catalog in front of it.</summary>
    Stale,
}
