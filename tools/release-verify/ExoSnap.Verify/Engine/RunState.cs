using System.Collections.ObjectModel;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Engine;

/// <summary>
/// What a run directory knows about itself between invocations.
/// </summary>
/// <param name="SchemaVersion">Version of this document's shape.</param>
/// <param name="RunId">Identifier of the run, also the directory name.</param>
/// <param name="RcTag">The release candidate the run is about.</param>
/// <param name="SourceCommitSha">The commit the artifacts were built from.</param>
/// <param name="ArtifactFingerprint">Digest binding the run to the exact bytes it measured.</param>
/// <param name="CatalogVersion">Digest of the catalog the verdicts were produced against.</param>
/// <param name="StartedUtc">When the run started.</param>
/// <param name="Verdicts">Every verdict recorded so far.</param>
/// <remarks>
/// The two digests are the whole point of persisting state. A run that is resumed
/// against different artifacts, or against a catalog whose scenarios have moved,
/// still holds verdicts, and those verdicts no longer describe what is in front of
/// it. Detecting that is what <see cref="ScenarioOutcome.Stale"/> is for.
/// </remarks>
public sealed record RunState(
    string SchemaVersion,
    string RunId,
    string RcTag,
    string SourceCommitSha,
    string ArtifactFingerprint,
    string CatalogVersion,
    DateTimeOffset StartedUtc,
    IReadOnlyList<ScenarioVerdict> Verdicts)
{
    /// <summary>The schema version this build of the harness writes.</summary>
    public const string CurrentSchemaVersion = "1";

    /// <summary>
    /// The digest of what these verdicts were measured with, or an empty string.
    /// </summary>
    /// <remarks>
    /// Empty is what a state written before this field existed carries, and it is
    /// treated as "not recorded" rather than "unchanged": a verdict that cannot say
    /// which oracles, which Windows build and which display driver produced it has not
    /// shown it was produced under the conditions in front of the harness now.
    /// </remarks>
    public string ToolingFingerprint { get; init; } = string.Empty;

    /// <summary>The file name a run's state is stored under.</summary>
    public const string FileName = "state.json";

    /// <summary>
    /// The verdicts as they apply to the artifacts and catalog in front of the
    /// harness now. A verdict recorded against different bytes or a different
    /// catalog comes back as <see cref="ScenarioOutcome.Stale"/>.
    /// </summary>
    public IReadOnlyList<ScenarioVerdict> VerdictsFor(
        string artifactFingerprint, string catalogVersion, ToolingFingerprint? tooling = null)
    {
        var artifactMoved = !string.Equals(this.ArtifactFingerprint, artifactFingerprint, StringComparison.Ordinal);
        var catalogMoved = !string.Equals(this.CatalogVersion, catalogVersion, StringComparison.Ordinal);

        // A caller that does not say what it is measuring with gets the old answer:
        // the tooling rule is opt-in at the call site so a report that only reads a
        // run does not have to probe the machine to print it.
        var toolingMoved = tooling is not null && !tooling.Accepts(this.ToolingFingerprint);

        if (!artifactMoved && !catalogMoved && !toolingMoved)
        {
            return this.Verdicts;
        }

        var reason = (artifactMoved, catalogMoved) switch
        {
            (true, true) => "the artifacts and the scenario catalog both changed since this verdict was recorded",
            (true, false) => "the artifacts changed since this verdict was recorded",
            (false, true) => "the scenario catalog changed since this verdict was recorded",
            _ => tooling!.DescribeMismatch(this.ToolingFingerprint),
        };

        return new ReadOnlyCollection<ScenarioVerdict>(
        [
            .. this.Verdicts.Select(verdict => verdict with
            {
                Outcome = ScenarioOutcome.Stale,
                Message = reason,
            }),
        ]);
    }
}
