# ADR 0011: Encoder Capability and Settings Schema

## Status

Accepted — implementation scheduled for 0.5.0 (see roadmap).

## Context

As ExoSnap gains multiple encoder backends (NVENC, AMF, QSV, x264, SVT-AV1), UI pages and
reconciliation logic accumulate per-encoder `if`-chains: "if NVENC, show CQ; if x264, show CRF;
if AMF, hide lossless; …". These chains scatter capability knowledge across the UI layer, making
it impossible to add an encoder without auditing every settings page.

The same problem appears in preset reconciliation: when a preset is loaded and the configured
encoder is unavailable, the fallback logic must know which codecs, rate-control modes, and quality
ranges the substitute encoder supports.

## Decision

Each encoder publishes an `EncoderCapabilitySchema` — a structured, queryable declaration of
everything the encoder supports. The UI and reconciliation logic read from this schema; they do
not contain per-encoder knowledge themselves.

### Schema fields

| Field | Type | Description |
|---|---|---|
| `encoder_id` | string | Stable identifier (e.g., `nvenc`, `amf`, `qsv`, `x264`, `svt_av1`) |
| `codecs` | set | Supported video codecs (AVC, HEVC, AV1, …) |
| `profiles` | map codec → set | Supported codec profiles (Baseline, Main, High, Main10, …) |
| `levels` | map codec → set | Supported codec levels |
| `bit_depths` | set | Supported bit depths (8, 10, 12) |
| `chroma_formats` | set | Supported chroma formats (4:2:0, 4:2:2, 4:4:4) |
| `rate_control_modes` | set | Supported canonical rate control modes (see ADR 0009) |
| `presets` | list | Encoder-specific speed/quality presets (e.g., P1–P7 for NVENC) |
| `max_resolution` | struct | Maximum encoded width × height |
| `max_framerate` | rational | Maximum encoded frame rate |
| `hdr_support` | bool | Whether HDR metadata injection is supported |
| `lossless_support` | bool | Whether lossless mode is supported |
| `b_frames_support` | bool | Whether B-frame encoding is supported |
| `lookahead_support` | bool | Whether lookahead/multi-pass rate control is available |
| `forced_keyframe` | bool | Whether frame-accurate forced keyframe insertion is supported |

### Runtime vs. compile-time schema

The schema has two layers:

1. **Static schema** — declared at compile time per encoder class; contains the theoretical
   maximum capability of the encoder.
2. **Runtime probe result** — returned by `CapabilityProbe` after querying the actual hardware
   or driver. The intersection of static schema and runtime probe is what the UI presents.

`CapabilityProbe` runs at application startup and after device topology changes (see ADR 0005).

### UI generation rule

UI controls for encoder settings are generated or validated from the runtime schema. Specifically:

- Rate control mode pickers show only modes in `rate_control_modes`.
- Bit depth pickers show only depths in `bit_depths` for the active codec.
- Lossless options appear only if `lossless_support = true`.
- B-frame controls appear only if `b_frames_support = true`.
- Preset pickers use the encoder's `presets` list, not a hardcoded UI list.

No encoder-specific `if`-chain is permitted in settings page code.

### Settings schema and validation

Encoder settings values (quality, bitrate, preset index, b-frame count, …) are validated against
the schema at apply time. Out-of-range values are clamped and a warning is logged; they are never
silently accepted and then rejected by the encoder at encode time.

## Consequences

- Adding a new encoder requires implementing `IVideoEncoder` and declaring its
  `EncoderCapabilitySchema`; no settings page code changes are required.
- Preset reconciliation can safely fall back to an available encoder by comparing schemas, not by
  containing per-encoder knowledge.
- The schema is a pure value struct with no Qt or platform dependencies; it is testable without
  a GPU or running encoder.
- Schema design for future encoders (Intel ARC, Apple VideoToolbox on ARM64) follows the same
  interface without UI changes.
