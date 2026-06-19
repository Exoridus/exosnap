# ADR 0010: Media Compatibility Registry

## Status

Partially implemented — 0.2.0 container registry shipped (CONTAINER-COMPAT-REGISTRY-R1).
Full capability registry (encoder capabilities, HDR, rate-control) deferred to 0.5.0.
0.5.0 Settings redesign (SETTINGS-REDESIGN-D6) changes the codec-picker UI paradigm
from hide-invalid to show-all-plus-callout (see "0.5.0 UI behaviour change" below);
the registry and engine constraints are unchanged.

### 0.2.0 implementation notes

`ContainerCompatRegistry` (`libs/capability/include/capability/container_compat_registry.h`,
`libs/capability/src/container_compat_registry.cpp`) is the single source of truth for
container × video-codec × audio-codec compatibility.

- All 27 combinations of the 3 × 3 × 3 enum dimensions are explicitly classified.
- `ContainerCompatLevel` maps to the ADR classification model (`Recommended` / `Allowed` /
  `Experimental` / `Fallback` / `Prohibited`).
- The registry drives `CapabilitySet::QueryCombo()` (via `RegistryEntryToSupportAnnotation`):
  `Recommended → Available`, `Allowed → ValidUnvalidated`,
  `Experimental + Fallback → NotImplemented`, `Prohibited → Invalid`.
- `RecordingPreset::ReconcileContainerCodecs()` now delegates entirely to
  `ContainerCompatRegistry::ReconcileCodecs()`, removing the previous ad-hoc switch/if chain.
- `SettingsResolver` (resolver.cpp) delegates preferred-codec queries to the registry.
- The registry blocks recording start for Prohibited combinations via the existing
  `CapabilitySet → SettingsResolver::ValidateConfig → RecordingCoordinator` path; no new
  UI paradigm was introduced.
- `Allowed → ValidUnvalidated` in the CapabilitySet: user-selectable, recording is not
  blocked; a warning is surfaced in the UI. `Prohibited → Invalid`: hard block, must not
  appear in any UI picker and recording cannot start.

**Pre-v1 behaviour change (HEVC):** MKV + HEVC + (any audio) previously reconciled to MKV + H264 + AAC
(ad-hoc HEVC→H264 downgrade). Under the registry the reconciler falls through to the primary
working path (AV1 + Opus) because no Recommended or Allowed entry exists for HEVC in any
combination today. The old H264 downgrade was implicit; the new path is explicit and reversible
once HEVC is implemented.

**Policy correction (MKV + H.264 + Opus):** This combination was initially classified
`Prohibited` in the first registry implementation. That was incorrect: Matroska carries Opus
natively and the Opus-in-MKV write path is production-validated via AV1+Opus. The combination
is reclassified to `Allowed` (player-matrix pass for H.264+Opus specifically is not yet on
file — that is the Allowed caveat). `ReconcileCodecs()` now leaves MKV+H264+Opus presets
unchanged instead of rewriting the audio codec to AAC.

### 0.5.0 UI behaviour change (Settings Redesign D6)

The 0.5.0 Settings surface redesign (SETTINGS-REDESIGN-D6) changes how the Settings codec
pickers present compatibility, **without changing the registry or the engine constraints**:

- **Codec pickers no longer filter by container.** The Format & encoding card offers every
  real video codec (AV1, H.264 today) and audio codec (Opus, AAC today) at all times. The
  earlier behaviour — hiding the combinations a container cannot hold — is removed.
- **Incompatible combinations surface a compatibility callout instead of disappearing.** When
  the active container/codec pair is registry-`Prohibited`/`Invalid` (e.g. WebM + H.264,
  MP4 + Opus, MP4 + AV1), the card shows an amber callout naming the conflict plus a
  **"Fix codecs"** action that calls `RecordingPreset::ReconcileContainerCodecs()` (the same
  registry reconciliation). When compatible, a green "Current format: …" status line is shown.
- **This supersedes the UI-only clauses of this ADR** that said prohibited combinations
  "must not appear in any UI picker" and that "UI pickers are generated from registry queries;
  per-page hardcoded lists are removed". For the Settings codec pickers those clauses no longer
  hold: the picker shows all real codecs and the registry verdict is surfaced as an explainable
  callout rather than by hiding options. Rationale: the D6 "compare, don't hide" principle —
  hiding an option never told the user *why* it was unavailable.
- **Engine constraints are unchanged.** The registry remains the single authority;
  `ReconcileContainerCodecs()` still enforces valid combinations on load and on "Fix codecs";
  recording start is still blocked for prohibited combinations via the existing
  diagnostic-blocker path. The callout is the proactive in-place warning; the blocker remains
  the backstop.
- Multi-option settings additionally expose a **CompareHint** popover that lists each option
  with its own qualitative effect line (the explainer doubles as the picker). Future codecs
  (HEVC, PCM, FLAC) appear there with a version tag ("0.6"/"0.7") and stay non-selectable until
  their controls exist.

## Context

ExoSnap exposes container, video codec, and audio codec choices across multiple UI pages (Output,
Video, Audio, presets). Without a single authority governing which combinations are valid,
implementations diverge:

- Container/codec pairings get validated inconsistently (e.g., UI allows WebM + H.264 even though
  no player supports it).
- Compatibility warnings are duplicated or absent depending on which code path runs.
- Adding a new codec or container requires touching every validation site.

The UI must only surface vetted combinations. "Theoretically muxable" is not the bar — the bar is
a tested player/editor matrix.

## Decision

A single **media compatibility registry** is the authoritative source for:

- Whether a container/codec combination is allowed, recommended, experimental, or prohibited.
- Associated compatibility warnings (Apple, browser, NLE).
- Fallback behavior when a selected combination becomes unavailable.

The registry is queried by the UI and by `RecordingPreset` reconciliation. No combination is
offered to the user unless the registry explicitly allows it.

### Container / codec / audio matrix

| Container | Video codecs | Audio codecs | Notes |
|---|---|---|---|
| MKV | AV1, HEVC, AVC | Opus, AAC, PCM, FLAC | Primary target; VFR supported |
| MP4 | AV1, HEVC, AVC | AAC | Primary for Apple/NLE compatibility; delivered as progressive MP4 via remux-on-stop (ADR 0014) |
| MP4 | — | PCM | Allowed; sample-entry variant and player matrix must be specified |
| MP4 | — | FLAC | Not a 1.0 target; fragile MP4 compatibility |
| WebM | AV1 | Opus | AV1 + Opus only |
| WebM (optional) | VP9 (later) | Opus | — |

### Explicit prohibitions

- **WebM + H.264** is prohibited. No major player or browser supports H.264 in a WebM container.
  The registry must return `Prohibited` for this pairing; it must not appear in any UI picker.
- **WebM + HEVC** is prohibited for the same reason.

### Known caveats

**PCM in MP4:** ISO-BMFF supports PCM (`lpcm`, `in24`, `in32`, `sowt`, etc.) but compatibility
varies by sample-entry type, tool, and platform. The registry entry for PCM-in-MP4 must specify
the concrete sample-entry variant and the tested player matrix before the combination is marked
`Allowed`. A bare "PCM" entry without this specification is not acceptable.

**FLAC in MP4:** Not a 1.0 target. FLAC fits MKV cleanly; FLAC-in-MP4 compatibility is fragile
and player support is inconsistent. The registry returns `Experimental` or `Prohibited` until
a real player matrix is validated.

**`hvc1` vs `hev1` for HEVC in MP4:** These two four-character codes differ in where parameter
sets are stored (`hvc1` = in `hvcC` box; `hev1` = in-band). Apple/QuickTime requires `hvc1` for
hardware-accelerated playback. The correct variant must be verified on real files via `ffprobe`
or Bento4/MP4Box in the 0.7.0 HEVC/HDR slice (via the libavformat remux path; see ADR 0014).
The registry must not mark HEVC-in-MP4 as
`Recommended` until this is confirmed.

### Compatibility classification

| Classification | Meaning |
|---|---|
| `Recommended` | Vetted combination; tested player/editor matrix on file |
| `Allowed` | Works but with caveats; warning shown in UI |
| `Experimental` | Technically possible; not yet tested at scale |
| `Fallback` | Used when a preferred combination fails; not user-selectable |
| `Prohibited` | Must not appear in the UI under any circumstance |

**Classification policy (authoritative):** `Prohibited` is reserved for genuinely
incompatible container/codec pairings only — the container physically cannot carry
the codec, or no major player supports the combination (e.g. AAC in WebM, H.264 in
WebM). A combination that is technically compatible but lacks a full player-matrix
validation pass must never be `Prohibited`; it belongs in `Experimental` (not yet
tested at scale) or `Allowed` (works with known caveats, warning shown in UI).

## Consequences

- Adding a new codec or container requires a registry entry with a tested matrix — not just a
  muxer capability flag.
- The UI pickers are generated from registry queries; per-page hardcoded lists are removed.
- `RecordingPreset` reconciliation calls the registry to validate loaded presets; invalid
  combinations are corrected to the nearest allowed combination on load.
- The `hvc1`/`hev1` and PCM-in-MP4 questions are blocking items before those combinations can
  be marked `Recommended` or `Allowed`.
- The registry is a pure value-model component with no Qt or UI dependencies; it is testable
  without a running application.
