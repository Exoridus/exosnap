using System.Text.Json.Serialization;

namespace ExoSnap.Verify.Gates;

/// <summary>One A/V drift sample taken during a long recording.</summary>
/// <param name="AtUtc">When the sample was taken.</param>
/// <param name="Lifecycle">The pipeline lifecycle at that moment.</param>
/// <param name="AvDriftMs">The drift the engine reported, or null when the group was absent.</param>
/// <param name="DriftAvailability">Why the drift is or is not measurable right now.</param>
public sealed record DriftSample(
    DateTimeOffset AtUtc,
    string Lifecycle,
    double? AvDriftMs,
    string DriftAvailability);

/// <summary>What a shutdown gate observed.</summary>
/// <param name="Outcome">Whether the process exited, stayed, or owned no window to ask.</param>
/// <param name="DeadlineSeconds">How long it was given.</param>
/// <param name="BufferedEvents">How many events the client was holding when the channel closed.</param>
public sealed record ShutdownObservation(string Outcome, double DeadlineSeconds, int BufferedEvents);

/// <summary>What a present cross-check compared, and what it concluded.</summary>
/// <param name="ProcessId">The process both observers were asked about.</param>
/// <param name="ProductMode">What ExoSnap reported.</param>
/// <param name="ObservedMode">What PresentMon reported.</param>
/// <param name="PresentCount">How many presents PresentMon attributed to the process.</param>
/// <param name="Agreed">Whether the two named the same presentation path.</param>
/// <param name="PresentCodeHash">Digest of the present-diagnostics sources the record was taken against.</param>
/// <param name="OsBuild">The Windows build the record was taken on.</param>
public sealed record PresentCrossCheckSummary(
    int ProcessId,
    string ProductMode,
    string ObservedMode,
    int PresentCount,
    bool Agreed,
    string PresentCodeHash,
    string OsBuild);

/// <summary>Source-generated contracts for the documents gates write as evidence.</summary>
/// <remarks>
/// Source generation rather than reflection, for the same reason the harness documents
/// use it: a document shape that stopped serializing is a build error rather than a
/// surprise on the machine that decides whether a release ships.
/// </remarks>
[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    UseStringEnumConverter = true,
    WriteIndented = true,
    DefaultIgnoreCondition = JsonIgnoreCondition.Never)]
[JsonSerializable(typeof(ContractSummary))]
[JsonSerializable(typeof(IReadOnlyList<DriftSample>))]
[JsonSerializable(typeof(ShutdownObservation))]
[JsonSerializable(typeof(PresentCrossCheckSummary))]
public sealed partial class GateJson : JsonSerializerContext;
