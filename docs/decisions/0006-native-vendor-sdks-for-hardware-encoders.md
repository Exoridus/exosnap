# ADR 0006: Native Vendor SDKs for Hardware Encoders

## Status

Accepted — implementation scheduled for 0.5.0 (encoder factory), 0.9.0 (AMF), 0.10.0 (QSV/oneVPL) (see roadmap).

## Context

The `0.1.0` MVP encodes through NVENC only, accessed via a direct CUDA/NVENC API path. As ExoSnap
adds AMD and Intel hardware support, two implementation paths are available:

1. Route all encoders through FFmpeg's `libavcodec` wrappers (`h264_nvenc`, `hevc_amf`, `h264_qsv`, …).
2. Speak each vendor SDK directly (NVENC API, AMF SDK, oneVPL/QSV).

FFmpeg wrappers are convenient but impose constraints that become painful at this product tier:
capability negotiation is coarse, forced-keyframe timing is imprecise, HDR metadata injection is
awkward, D3D11 surface handoff requires extra synchronization, and vendor-specific errors surface as
opaque `AVERROR` values. The FFmpeg wrapper layer also ties the project to FFmpeg's LGPL/GPL surface
for the encoder path.

## Decision

Hardware encoders use their native vendor SDKs — not a unified "everything through FFmpeg" path.

### Encoder hierarchy

```
IVideoEncoder
├── NvencVideoEncoder      (current baseline — NVENC API)
├── AmfVideoEncoder        (0.9.0 — AMF SDK)
├── QsvVideoEncoder        (0.10.0 — oneVPL/QSV)
├── X264VideoEncoder       (0.8.0 — software fallback; see ADR 0007)
└── SvtAv1VideoEncoder     (0.8.0 — optional software fallback; see ADR 0007)
```

Supporting infrastructure:

```
VideoEncoderFactory        — constructs the appropriate encoder for a given config + probe result
CapabilityProbe            — queries the active hardware for supported codecs/profiles/limits
EncoderSelectionPolicy     — applies preference ordering and fallback rules
EncoderDiagnosticsAdapter  — maps vendor-specific errors to structured DiagnosticEntry values
```

### Why native SDKs

| Concern | Native SDK | FFmpeg wrapper |
|---|---|---|
| D3D11 surface | Direct handoff, zero copy | Extra synchronization, potential copy |
| Capability detection | Precise (GUID/caps query) | Coarse (trial encode / AVERROR) |
| Forced keyframes | Native signal, frame-accurate | Best-effort, lag possible |
| Rate control | Full mode set per SDK | Subset exposed by wrapper |
| HDR metadata | SEI/parameter-set injection | Wrapper-dependent support |
| Vendor errors | Typed return codes | Opaque AVERROR |

### Media Foundation scope

Media Foundation may remain as a narrowly-scoped Windows fallback (e.g., a legacy MP4 path) but
is not used for any new preferred encoder path. It will be replaced, not extended (see ADR 0008).

## Consequences

- Each vendor encoder requires its own integration slice (0.9.0 for AMF, 0.10.0 for QSV).
- `EncoderDiagnosticsAdapter` must translate vendor error codes for the Diagnostics page; generic
  `AVERROR` mapping is insufficient.
- The encoder factory + capability probe are the gating infrastructure for 0.5.0; individual
  vendor backends plug in behind that interface in later versions.
- FFmpeg (`libavformat`) is still used for container/mux work (see ADR 0008); this decision applies
  specifically to the encode path.
- Software encoders (x264, SVT-AV1) follow the same `IVideoEncoder` interface — see ADR 0007.
