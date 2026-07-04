# ADR 0039: NVENC Encoder Preset (P1–P7) as a Uniform Expert Setting

## Status

Accepted — implemented on `feat/nvenc-preset` (stacked on `fix/settings-honesty`).

## Context

The NVENC SDK exposes seven speed/quality presets (`NV_ENC_PRESET_P1_GUID` … `P7_GUID`):
P1 is fastest with the lowest quality, P7 slowest with the best quality. Until now the
engine hardcoded the preset per codec with no user control:

- **AV1 and HEVC: P4** — P6 on AV1 has internal pipeline depth that returned
  `NV_ENC_ERR_NEED_MORE_INPUT` on every frame even with lookahead disabled; P4 produces
  frames synchronously. HEVC used P4 for the same pipeline-depth concern.
- **H.264: P6** — synchronous at P6, so the higher preset was kept.

The only UI trace was a Debug-only dummy row (`roadmapDummy_encoderPreset`, ADR 0031's
"roadmap-dummy" pattern), invisible in Release.

The preset is orthogonal to both existing video-quality controls: it selects NVENC's
internal encoding-pipeline tradeoff, while `NvencQualityPreset` only tunes CQP QP values
and `RateControlMode` (ADR 0009) selects CQP/VBR/CBR.

## Decision

1. **One uniform model, no per-codec branching.** A canonical
   `recorder_core::NvencPreset { P1..P7 }` enum, carried as `RecorderConfig::nvenc_preset`
   and `OutputSettingsModel::nvenc_preset`, resolved to the SDK GUID by the pure, testable
   `NvencPresetToGuid()` mapping (same pattern as `ApplyColorMetadataToNvenc`). The mapping
   applies identically to H.264, HEVC, and AV1 — P1–P7 exist uniformly for all three
   codecs, so the setting is **never capability-gated**; only the recording lock disables it.
2. **Default P4 for all codecs — including H.264 (visible change: previously P6).**
   Rationale: P4 is NVIDIA's balanced default; a uniform default removes the last
   special-case branch from `FetchPresetConfig`; and the new Expert combo makes P6/P7
   a one-click opt-in for anyone who wants the previous H.264 behavior back. AV1/HEVC
   defaults are unchanged.
3. **Ordering preserved.** `FetchPresetConfig` resolves the GUID before
   `nvEncGetEncodePresetConfigEx`; the color mapping and the rate-control overrides
   (`ComputeNvencRcParams`) still apply **after** the preset fetch, so user rate control
   always wins over preset-supplied RC defaults.
4. **Graceful degradation, not gating, for deep presets.** P5–P7 on AV1/HEVC can buffer
   frames (`NEED_MORE_INPUT`); `EncodeFrame` already drains this via the
   `m_pendingPts`/`m_pendingSlots` FIFOs, at the cost of encode latency and 8-slot
   input-ring pressure. Live-verified: **AV1 + P7** (the historical `NEED_MORE_INPUT`
   problem case) and AV1 + P1 both record and finalize valid MKVs on real hardware.
5. **Additive persistence, schema stays 20.** Preset TOML gains
   `output.nvenc_preset = "p1".."p7"`; a missing or unknown value loads the struct
   default (P4) without a reset. The field participates in `NormalizedConfigEquals`
   and `ConfigDirtyEquivalent`.
6. **UI.** Real Expert combo ("Encoder preset (NVENC)", Container & codecs expert
   section, after Colour range) bound via `MergeFormatSelection` (the settings-honesty
   rule: every format-editor field must be carried by the one merge function, proven by
   a red-proof test). The Debug dummy row is removed. Takes effect from the next
   recording. `probe_record` gains `--preset p1..p7` (default p4).

## Consequences

- Users can trade encode speed against quality per preset profile, uniformly across codecs.
- H.264 recordings made with defaults use P4 instead of P6 from this change on; the
  Expert combo restores P6 explicitly.
- No tuning-info UI, no multi-pass options, no per-GPU preset capability probing — out of
  scope; the P1–P7 set is uniform in the NVENC API across supported GPUs.
