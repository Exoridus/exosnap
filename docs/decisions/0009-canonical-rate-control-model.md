# ADR 0009: Canonical Rate Control Model

## Status

Accepted — UI exposure and per-encoder mapping implementation scheduled for 0.5.0 (see roadmap).

## Context

Each hardware and software encoder exposes rate control through its own terminology and parameter
set. NVENC uses CQ/CQP/VBR/CBR. AMF uses CQP/VBR/CBR. QSV uses ICQ/VBR/CBR. x264/x265 use CRF
(Constant Rate Factor). SVT-AV1 uses a CRF-like parameter with different internal semantics.

Presenting all of these directly in the UI would require users to understand per-vendor terminology
and would produce inconsistent behavior when switching encoders. Conversely, flattening everything
to "CRF" is incorrect: CRF is defined by x264/x265 semantics (quality-bounded VBR with a
psychovisual model) and does not describe NVENC CQ or AMF CQP in any precise sense.

## Decision

The UI exposes a single canonical rate control model. Encoders map from this model to their native
parameters internally. The canonical model is never bypassed.

### Canonical rate control modes

```
Rate control
├── Constant quality   — quality-target, encoder chooses bitrate
├── Variable bitrate   — encoder targets a bitrate, quality varies
├── Constant bitrate   — strict bitrate, quality and buffer managed by encoder
└── Lossless           — only where the active encoder and codec support it
```

### Per-encoder mapping

| Canonical mode   | NVENC         | AMF   | QSV    | x264/x265 | SVT-AV1    |
|---|---|---|---|---|---|
| Constant quality | CQ (VBR-HQ) or CQP | CQP   | ICQ    | CRF       | CRF-like   |
| Variable bitrate | VBR           | VBR   | VBR    | VBR       | VBR        |
| Constant bitrate | CBR           | CBR   | CBR    | CBR       | CBR        |
| Lossless         | Lossless (where supported) | — | — | lossless preset | lossless |

### "CRF" is never presented in the UI

The label "CRF" is not used in the UI, tooltip, or API surface. It appears only in internal
encoder-specific mapping code and in this ADR for disambiguation. Presenting "CRF" to users
implies x264/x265 semantics that do not hold for hardware encoders.

### Lossless availability

Lossless is only offered when `CapabilityProbe` confirms support for the active encoder + codec
combination. It is hidden, not greyed out, when unsupported, to avoid implying that lossless is
a universal feature.

## Consequences

- Switching encoders (e.g., from NVENC to x264) preserves the canonical rate control selection;
  only the internal mapping changes.
- The UI quality slider maps to the appropriate native quality parameter per encoder; the numeric
  range may differ (e.g., NVENC CQ 0–51 vs. x264 CRF 0–51 with different perceptual curves).
- `EncoderCapabilitySchema` (see ADR 0011) declares which canonical modes each encoder supports;
  the UI is driven from that schema, not from hardcoded if-chains.
- Advanced users who need raw per-vendor parameters are out of scope for the canonical model.
  Expert-mode raw parameter pass-through is a post-1.0 consideration.
