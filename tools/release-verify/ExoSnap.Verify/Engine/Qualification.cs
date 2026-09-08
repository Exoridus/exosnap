using System.Collections.ObjectModel;
using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Engine;

/// <summary>
/// Decides whether a set of verdicts permits promoting a release candidate.
/// </summary>
/// <remarks>
/// The rule is deliberately one-sided: promotion needs every required scenario to
/// have passed, and anything else at all refuses. A verdict that was never
/// measured and a verdict that failed are equally disqualifying, because a
/// release process whose default is "publish unless something objected" is the
/// process this harness exists to replace.
/// </remarks>
public static class Qualification
{
    /// <summary>Builds the record a promotion may rest on.</summary>
    /// <param name="rcTag">The release candidate tag.</param>
    /// <param name="sourceCommitSha">The commit the artifacts were built from.</param>
    /// <param name="artifacts">The exact bytes the verdicts describe.</param>
    /// <param name="machine">The machine the verdicts were produced on.</param>
    /// <param name="capabilities">The capabilities measured at the start of the run.</param>
    /// <param name="catalogVersion">Digest of the catalog that was run.</param>
    /// <param name="verdicts">Every scenario verdict.</param>
    /// <param name="required">The ids that must have passed.</param>
    /// <param name="environmentRestores">Whether every mutated property came back.</param>
    public static QualificationRecord Build(
        string rcTag,
        string sourceCommitSha,
        IReadOnlyCollection<ArtifactDigest> artifacts,
        MachineFingerprint machine,
        IReadOnlyDictionary<string, string> capabilities,
        string catalogVersion,
        IReadOnlyCollection<ScenarioVerdict> verdicts,
        IReadOnlyCollection<string> required,
        IReadOnlyCollection<EnvironmentRestoreVerdict> environmentRestores)
    {
        ArgumentNullException.ThrowIfNull(artifacts);
        ArgumentNullException.ThrowIfNull(machine);
        ArgumentNullException.ThrowIfNull(capabilities);
        ArgumentNullException.ThrowIfNull(verdicts);
        ArgumentNullException.ThrowIfNull(required);
        ArgumentNullException.ThrowIfNull(environmentRestores);

        var evidence = verdicts.SelectMany(verdict => verdict.Evidence).ToList();

        return new QualificationRecord(
            QualificationRecord.CurrentSchemaVersion,
            rcTag,
            sourceCommitSha,
            new ReadOnlyCollection<ArtifactDigest>([.. artifacts]),
            machine,
            HarnessIdentity(),
            catalogVersion,
            new ReadOnlyDictionary<string, string>(
                capabilities.ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal)),
            new ReadOnlyCollection<ScenarioVerdict>([.. verdicts]),
            new ReadOnlyCollection<EnvironmentRestoreVerdict>([.. environmentRestores]),
            new ReadOnlyCollection<Evidence>(evidence),
            DateTimeOffset.UtcNow,
            Decide(verdicts, required, environmentRestores));
    }

    /// <summary>
    /// The reasons a set of verdicts refuses promotion, in reporting order. An
    /// empty result means promotion is permitted.
    /// </summary>
    public static ReadOnlyCollection<string> Objections(
        IReadOnlyCollection<ScenarioVerdict> verdicts,
        IReadOnlyCollection<string> required,
        IReadOnlyCollection<EnvironmentRestoreVerdict> environmentRestores)
    {
        ArgumentNullException.ThrowIfNull(verdicts);
        ArgumentNullException.ThrowIfNull(required);
        ArgumentNullException.ThrowIfNull(environmentRestores);

        var objections = new List<string>();
        var byId = verdicts.ToDictionary(verdict => verdict.Id, StringComparer.OrdinalIgnoreCase);

        foreach (var id in required)
        {
            if (!byId.TryGetValue(id, out var verdict))
            {
                objections.Add($"{id}: required, but the run recorded no verdict for it");
                continue;
            }

            if (verdict.Outcome != ScenarioOutcome.Pass)
            {
                objections.Add(string.Format(
                    CultureInfo.InvariantCulture,
                    "{0}: required, but reported {1} ({2})",
                    id,
                    verdict.Outcome,
                    verdict.Message));
            }
        }

        // An infrastructure error anywhere disqualifies, required or not: it means
        // the run itself did not work, and the scenarios around it were measured
        // on a machine in an unknown condition.
        foreach (var verdict in verdicts.Where(v => v.Outcome == ScenarioOutcome.InfrastructureError))
        {
            if (!required.Contains(verdict.Id, StringComparer.OrdinalIgnoreCase))
            {
                objections.Add($"{verdict.Id}: infrastructure error ({verdict.Message})");
            }
        }

        foreach (var restore in environmentRestores.Where(restore => !restore.Restored))
        {
            objections.Add(
                $"{restore.Property}: left at '{restore.Observed}', expected '{restore.Expected}'");
        }

        return new ReadOnlyCollection<string>(objections);
    }

    /// <summary>
    /// A digest binding a run to the exact artifact bytes it measured, used to
    /// detect a resumed run whose artifacts have changed underneath it.
    /// </summary>
    public static string ArtifactFingerprint(IEnumerable<ArtifactDigest> artifacts)
    {
        ArgumentNullException.ThrowIfNull(artifacts);
        var material = string.Join(
            '\n',
            artifacts
                .Select(artifact => artifact.Name + ':' + artifact.Sha256)
                .Order(StringComparer.Ordinal));
        var digest = SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(material));
        return Convert.ToHexString(digest)[..16].ToLower(CultureInfo.InvariantCulture);
    }

    /// <summary>Which harness build is producing verdicts.</summary>
    public static HarnessIdentity HarnessIdentity()
    {
        var assembly = Assembly.GetExecutingAssembly();
        var informational = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        var version = informational ?? assembly.GetName().Version?.ToString() ?? "unknown";

        // The informational version carries the source revision as a "+sha"
        // suffix when the build stamped one. Nothing invents it when it did not.
        var plus = version.IndexOf('+', StringComparison.Ordinal);
        return plus >= 0
            ? new HarnessIdentity(version[..plus], version[(plus + 1)..])
            : new HarnessIdentity(version, "unknown");
    }

    private static QualificationOutcome Decide(
        IReadOnlyCollection<ScenarioVerdict> verdicts,
        IReadOnlyCollection<string> required,
        IReadOnlyCollection<EnvironmentRestoreVerdict> environmentRestores) =>
        Objections(verdicts, required, environmentRestores).Count == 0
            ? QualificationOutcome.Qualified
            : QualificationOutcome.NotQualified;
}
