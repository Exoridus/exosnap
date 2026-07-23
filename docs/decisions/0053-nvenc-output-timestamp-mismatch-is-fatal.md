# ADR 0053: NVENC outputTimeStamp Mismatch Is a Fatal Encode Error

## Status

Accepted.

## Context

Submitting frames to NVENC ahead of their completion (rather than blocking on each frame's
bitstream before submitting the next) requires a reliable way to associate a completed
bitstream with the specific frame that produced it, instead of assuming submission order
always equals completion order. NVENC's `NV_ENC_LOCK_BITSTREAM::outputTimeStamp` field exists
for exactly this purpose: the encoder is expected to echo back whatever unique timestamp was
set on `NV_ENC_PIC_PARAMS::inputTimeStamp` at submission time.

NVIDIA's vendored SDK header documents this field only as "Presentation timestamp associated
with the encoded output" — it does not state in so many words that the value is a guaranteed,
exact echo of the submitted input timestamp.

Two checks were made before deciding whether a mismatch should abort a recording or merely be
logged:

1. **Live verification on real hardware.** A dedicated dev-only probe tool drove the actual
   shipped encoder code (not a reimplementation) through real recording sessions on an RTX
   5070 Ti, covering all three supported codecs (AV1, HEVC, H.264) at both the P4 and P7
   encoder presets — 360 frames total, zero mismatches.
2. **Source-level verification against FFmpeg's NVENC integration.** FFmpeg's `nvenc.c` has
   shipped in production for close to a decade, across NVIDIA's entire supported NVENC
   hardware and driver range, on both Windows and Linux. It assigns the driver's
   `outputTimeStamp` directly and unconditionally to the output packet's presentation
   timestamp, with no cross-check against the value it submitted. A broken echo on any
   hardware generation would have produced visibly wrong output timestamps for a huge
   population of users — a defect of that size would not have gone unnoticed or unfixed this
   long.

Weighed against this: the risk of quietly delivering a corrupted PTS-to-packet mapping in a
finished recording (silent data corruption) versus the risk of aborting a small number of
otherwise-fine recordings on some untested hardware/driver combination where the echo turns
out not to hold.

## Decision

A mismatch between the timestamp submitted for a frame and the timestamp echoed back for its
completed bitstream is treated as a **fatal encode error**, not a warning. The encoder logs the
mismatch (with both values) and aborts the encode; this surfaces through the same
video-encode-failure path as any other fatal encoder error, stopping the recording rather than
muxing a packet whose PTS association can no longer be trusted.

This does not change how keyframe-cadence prediction is handled: a mismatch between the
predicted and actual keyframe placement remains warn-only, because it only affects where HDR
metadata is attached (legal either way), not which packets are treated as keyframes for muxing
— the driver's actual keyframe flag stays authoritative regardless of the prediction.

## Consequences

- If some GPU/driver combination that was never tested turns out not to honor the echo
  contract, affected users would see a recording abort with an encode-failure message instead
  of a recording that finishes with a subtly wrong internal PTS mapping. Given the FFmpeg
  precedent above, this is judged unlikely enough to accept.
- There is no setting to relax this back to a warning. If evidence surfaces that this was the
  wrong call — e.g. real users hitting this abort on hardware that is otherwise recording
  correctly — the fix is to revisit this decision, not to add a runtime escape hatch.
- The mismatch counter in engine diagnostics can in practice never read above 1 for a given
  session, since the encode aborts the first time it fires.
