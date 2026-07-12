# ADR 0050: The diagnostics tier is part of the diagnosis

## Status

Accepted.

## Context

Diagnostics is the signature "diagnostics-first" surface. The design canon defines a four-tier
honesty model — **Blocker · Measured problem · Optimisation · Fact** — where each tier owns both a
colour and a default visibility, and the rail is: *hiding is only ever for noise (Tier 3 + 4); a real
problem (Tier 1 + 2) is always visible.*

Before this change the engine only produced a coarse `Pass / Notice / Blocker` severity, and the UI
split Tier-2 (measured problem) from Tier-3 (optimisation tip) with a hard-coded id allowlist
(`isOptimisationTip`). That allowlist was a downstream re-derivation of information the check already
knew, and it silently drifted from the checks (a new Notice defaulted to Tier-2 unless someone
remembered to add its id). Environment facts (elevation, audio format) were hard-coded UI rows,
outside the model entirely.

## Decision

- Add a real `DiagnosticTier { Blocker, MeasuredProblem, Optimisation, Fact }` to `DiagnosticResult`.
  **Every check declares its own tier at the diagnosis site** (`MakeResult` requires it); the
  `isOptimisationTip` allowlist is deleted. The UI buckets purely on the declared tier.
- Encode the honesty rule in the model as pure predicates: `IsAlwaysVisible` (Tier 1 + 2) and
  `BundlesIntoTipChip` (Tier 3 only). The verdict counts only Tier 1 + 2; Tier 3 never turns the
  verdict amber; Tier 4 is never counted.
- **Tier-4 facts flow through the same `DiagnosticResult` model** but on a *separate producer*
  (`RecommendationEngine::GenerateEnvironmentFacts`) so they never pollute the recommendation
  checklist or the verdict — they render only in the Expert Environment panel.
- Severity (`Pass / Notice / Blocker`) is retained as the orthogonal record of "does this gate the
  start" and drives card colour; tier drives bucketing and visibility. A Tier-3 optimisation is
  always a `Notice`, never a `Blocker`.

## Consequences

- A new check cannot land without stating its tier — the compiler enforces it, and the tier can no
  longer drift from the diagnosis.
- Audio device loss (ADR 0046) surfaces as a calm **Tier-2 measured problem** card, never a blocker,
  built from the live snapshot's `AudioDiagnostics::degraded_sources`.
- The Expert Environment panel is data-driven from Fact-tier results instead of hard-coded rows.
- `GetAllRecommendationCodes` grows `rec.audio.degraded`.
