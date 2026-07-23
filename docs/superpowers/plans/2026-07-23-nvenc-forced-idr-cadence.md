# S4 — NVENC Forced-IDR Cadence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace NVENC's passive `idrPeriod`-driven keyframe cadence with an explicit,
submission-side `NV_ENC_PIC_FLAG_FORCEIDR` at every frame `NextGopKeyframePhase` already
predicts as a keyframe — turning today's "vorhersage über NVENC-Verhalten" into a fact we
actively drive. This is a correctness/robustness fix, independent of and preparatory for
the eventual B-Frames/Lookahead work (M-2), which would otherwise desynchronize the current
prediction-only model.

**Architecture:** `NextGopKeyframePhase` (pure, already implemented and tested — unchanged
by this plan) already predicts keyframe submission indices correctly. Today `EncodeFrame`
only *consults* that prediction to decide whether to attach HDR10 SEI/OBU payloads, and
separately sets `NV_ENC_PIC_FLAG_FORCEIDR` only for explicit segment-boundary requests
(`RequestKeyframe()`). This plan reorders `EncodeFrame` so the SAME `phase.is_keyframe`
result also drives `NV_ENC_PIC_FLAG_FORCEIDR` for the regular GOP cadence, not just for
forced segment boundaries. `idrPeriod = gopLength` (belt-and-braces) stays set, unchanged.

**Tech Stack:** C++20, NVENC SDK 13.0, GoogleTest.

## Global Constraints

- Do not change `NextGopKeyframePhase`'s signature or behavior — it is already correct and
  covered by existing tests (`test_nvenc_gop_aq_config.cpp`); this plan only changes how its
  result is *consumed* in `EncodeFrame`.
- No behavior change to the P-only stream's *observable* keyframe cadence: keyframes must
  still land at exactly the same submission indices as today (0, gopLength, 2×gopLength, ...,
  plus any `RequestKeyframe()` boundary) — only the *mechanism* producing them changes from
  "NVENC's internal `idrPeriod` timer" to "an explicit flag we set". A byte-identical
  bitstream is explicitly NOT required (`NV_ENC_PIC_FLAG_FORCEIDR` is not guaranteed
  byte-identical to an `idrPeriod`-driven IDR — RC/GOP-reset behavior can differ minutely
  even at identical keyframe positions).
- Do not touch `nvenc_encoder.h`'s `GopKeyframePhase` struct, `ComputeGopLength`,
  `ApplyGopToNvenc`, or `ApplySpatialAqToNvenc` — out of scope.
- Does not touch encoder capability-probing, `CapabilitySet`, or any UI — this is a pure
  encoder-internals fix.

---

## File Structure

| File | Responsibility |
|---|---|
| `libs/recorder_core/src/nvenc_encoder.cpp` | `EncodeFrame`: reorder FORCEIDR-flag logic to depend on `phase.is_keyframe`; rewrite the stale "prediction" comment to describe the new "we force it" reality |
| `libs/recorder_core/src/nvenc_encoder.h` | Rewrite `NextGopKeyframePhase`'s doc comment (still describes itself as a passive "predictor"; must now describe itself as the pure decision function that `EncodeFrame` actively enforces) |
| `libs/recorder_core/tests/test_nvenc_gop_aq_config.cpp` | No code change required — existing `NextGopKeyframePhase` tests already cover the cadence math this plan relies on; this plan's task re-runs them as a regression gate |

---

### Task 1: Force IDR on every predicted keyframe, not just segment boundaries

**Files:**
- Modify: `libs/recorder_core/src/nvenc_encoder.cpp:1155-1183` (`EncodeFrame`, the `pic.encodePicFlags`/`forcedIdr`/`NextGopKeyframePhase` block)
- Modify: `libs/recorder_core/src/nvenc_encoder.h:140-148` (`NextGopKeyframePhase` doc comment)
- Test: `libs/recorder_core/tests/test_nvenc_gop_aq_config.cpp` (no new test needed — regression run only, see Step 1)

**Interfaces:**
- Consumes: `NextGopKeyframePhase(uint32_t frame_in_gop, uint32_t gop_length, bool forced_idr) -> GopKeyframePhase` (`nvenc_encoder.h:153`, unchanged signature/behavior).
- Produces: no new symbols. The only externally-observable change is that `EncodeFrame`'s submitted `NV_ENC_PIC_PARAMS::encodePicFlags` now carries `NV_ENC_PIC_FLAG_FORCEIDR` on every submission where `phase.is_keyframe` is true (previously only on `RequestKeyframe()`-triggered ones).

- [ ] **Step 1: Confirm the existing pure-function regression baseline is green (pre-change)**

Run: `cmake --build build/windows-x64-debug --target test_nvenc_gop_aq_config --config Debug && ctest --test-dir build/windows-x64-debug -R "NextGopKeyframePhase|ApplyGopToNvenc|ComputeGopLength" -C Debug --output-on-failure`
Expected: PASS, all existing tests green (this is the baseline this task must not break — `NextGopKeyframePhase` itself is not modified, so this should already pass before Step 3; run it now to have a clean pre-change reference in your report).

- [ ] **Step 2: Read the current `EncodeFrame` block to confirm it matches this plan's assumption**

In `libs/recorder_core/src/nvenc_encoder.cpp`, confirm lines 1155-1183 currently read (adapt to the actual current line numbers if they've shifted, but the code shape should match):

```cpp
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.inputPitch = 0;
    pic.inputBuffer = mapRes.mappedResource;
    pic.outputBitstream = m_bitstreamBuffer;
    pic.bufferFmt = mapRes.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    const bool forcedIdr = m_forceIdrNext;
    if (forcedIdr) {
        // Force an IDR at a segment boundary: the first frame of the new segment
        // must be a self-contained keyframe carrying fresh SPS/PPS so no dependent
        // frame precedes it. Consume the one-shot request.
        pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        m_forceIdrNext = false;
    }

    // Deterministic keyframe (IDR) detection. With no B-frames and no lookahead
    // (enforced in FetchPresetConfig), output order == submission order and IDRs
    // land on submission indices 0, gopLength, 2*gopLength, ...; a forced IDR
    // resets the GOP phase. Advance the phase and decide before submitting.
    // NextGopKeyframePhase is the pure form of this cadence (tested with
    // non-default GOP lengths); it honours the configured m_gopLength, so a
    // user-selected 1 s / 0.5 s keyframe interval is respected here too.
    const GopKeyframePhase phase = NextGopKeyframePhase(m_frameInGop, m_gopLength, forcedIdr);
    const bool isKeyframe = phase.is_keyframe;
    m_frameInGop = phase.frame_in_gop;
```

If the surrounding code has materially changed (not just shifted line numbers), stop and report BLOCKED rather than guessing how to adapt — this plan's Step 3 depends on this exact shape.

- [ ] **Step 3: Reorder — force IDR from `phase.is_keyframe`, not just from the one-shot request**

Replace the block from Step 2 with:

```cpp
    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = width;
    pic.inputHeight = height;
    pic.inputPitch = 0;
    pic.inputBuffer = mapRes.mappedResource;
    pic.outputBitstream = m_bitstreamBuffer;
    pic.bufferFmt = mapRes.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    // One-shot segment-boundary request (RequestKeyframe()), consumed now
    // regardless of the cadence outcome below — it always feeds into this
    // submission's phase decision via NextGopKeyframePhase's forced_idr param.
    const bool forcedIdr = m_forceIdrNext;
    m_forceIdrNext = false;

    // Keyframe cadence is now an ENFORCED fact, not a prediction about NVENC's
    // internal idrPeriod timer. NextGopKeyframePhase is the pure decision
    // function (tested with non-default GOP lengths, nvenc_encoder.h); it
    // honours the configured m_gopLength (a user-selected 1 s / 0.5 s keyframe
    // interval), and folds in the one-shot forced-IDR request. Whatever it
    // decides, we drive it here with NV_ENC_PIC_FLAG_FORCEIDR — idrPeriod stays
    // set as a belt-and-braces backstop, but is no longer the mechanism the
    // keyframe positions actually depend on.
    const GopKeyframePhase phase = NextGopKeyframePhase(m_frameInGop, m_gopLength, forcedIdr);
    const bool isKeyframe = phase.is_keyframe;
    m_frameInGop = phase.frame_in_gop;

    if (isKeyframe) {
        pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
    }
```

Note what changed: the old code's separate `if (forcedIdr) { ...; m_forceIdrNext = false; }` block (which set the flag ONLY for `RequestKeyframe()`-triggered IDRs) is removed; `m_forceIdrNext` is still consumed exactly once per call (same one-shot semantics), but the actual `NV_ENC_PIC_FLAG_FORCEIDR` flag now comes from `isKeyframe` — which is `true` whenever `forcedIdr` was true OR the regular GOP cadence says so (`GopKeyframePhase::is_keyframe = forced_idr || (frame_in_gop == 0u)`, unchanged in the pure function). This is a strict superset of the old flag-setting condition: every case that set the flag before still sets it; the regular-cadence case (`frame_in_gop == 0`, not from `RequestKeyframe()`) now also sets it, which it didn't before.

Everything below this block (the code reading `phase`/`isKeyframe`/`m_frameInGop` for HDR SEI attach, at the former lines 1185-1197) is unchanged — it already reads these same three names, which still exist with the same types.

- [ ] **Step 4: Rewrite the `NextGopKeyframePhase` doc comment in `nvenc_encoder.h`**

In `libs/recorder_core/src/nvenc_encoder.h`, the comment above `struct GopKeyframePhase` (currently lines 140-148) reads:

```cpp
// ---------------------------------------------------------------------------
// NextGopKeyframePhase — pure, testable IDR predictor. Mirrors the deterministic
// cadence EncodeFrame relies on: with no B-frames and no lookahead
// (frameIntervalP=1) output order == submission order, so IDRs land on
// submission indices 0, gopLength, 2*gopLength, ...; a forced IDR resets the
// phase. Given the current frame-in-GOP counter, the configured GOP length, and
// whether a forced IDR was requested this frame, returns whether this frame is a
// keyframe and the counter to carry to the next frame. No GPU/NVENC session.
// ---------------------------------------------------------------------------
```

Replace with:

```cpp
// ---------------------------------------------------------------------------
// NextGopKeyframePhase — pure, testable IDR cadence decision. EncodeFrame does
// not merely predict IDR placement from NVENC's idrPeriod timer — it actively
// sets NV_ENC_PIC_FLAG_FORCEIDR on every submission this function marks as a
// keyframe, so cadence is an enforced fact rather than an assumption about
// driver behavior (idrPeriod stays set as a belt-and-braces backstop only).
// With no B-frames and no lookahead (frameIntervalP=1) output order ==
// submission order, so IDRs land on submission indices 0, gopLength,
// 2*gopLength, ...; a forced IDR resets the phase. Given the current
// frame-in-GOP counter, the configured GOP length, and whether a forced IDR
// was requested this frame, returns whether this frame is a keyframe and the
// counter to carry to the next frame. No GPU/NVENC session.
// ---------------------------------------------------------------------------
```

- [ ] **Step 5: Build and run the regression suite**

Run: `cmake --build build/windows-x64-debug --target test_nvenc_gop_aq_config --config Debug`
Expected: builds clean (this file only changes comments + the caller in `nvenc_encoder.cpp`; `test_nvenc_gop_aq_config.cpp` itself is untouched).

Run: `ctest --test-dir build/windows-x64-debug -R "NextGopKeyframePhase|ApplyGopToNvenc|ComputeGopLength|ApplySpatialAqToNvenc" -C Debug --output-on-failure`
Expected: PASS, identical results to Step 1 (this task changes no pure-function behavior — it only changes how `EncodeFrame`, which has no unit-test seam, consumes an unchanged pure function).

- [ ] **Step 6: Full recorder_core NVENC test suite (regression)**

Run: `ctest --test-dir build/windows-x64-debug -R "nvenc" -C Debug --output-on-failure`
Expected: PASS, no regressions in any `recorder_core.test_nvenc_*` test (rc_params, color_config, chroma_config, gop_aq_config, preset_guid, flush_drain_policy, video_encoder_interface — none of these test `EncodeFrame` directly against real hardware, so none should be affected by this change; a regression here would indicate the reorder broke something the plan didn't anticipate).

- [ ] **Step 7: Manual real-hardware verification (no CI seam exists for `EncodeFrame` itself)**

`EncodeFrame` requires a live NVENC session — there is no unit-test seam for it in this codebase (same situation as the S1 plan's hardware-probe blocks). Verify on this dev machine's real GPU via a real recording, per this project's `--auto-record` CLI/env exception (CLAUDE.md: allowed, never live mouse/keyboard driving; output goes to a scratch directory, never committed):

1. Build the full app: `cmake --build build/windows-x64-debug --target exosnap --config Debug`
2. Run a short recording via the project's `--auto-record` mechanism (check `README.md`/`AGENTS.md` for the exact current invocation and flags — e.g. duration, output-dir override via `EXOSNAP_OUTPUT_DIR`) with the default profile (MKV + AV1 + Opus + CFR 60 fps, 2 s keyframe interval per default) for at least 10 seconds (≥ 300 frames at 60 fps, enough to observe multiple keyframe cycles).
3. Run `ffprobe -show_frames -select_streams v <output-file>` (or `ffprobe -show_entries frame=pict_type,key_frame -of csv` for compact output) on the resulting file.
4. Assert: keyframes appear at frame indices `0, 120, 240, ...` (2 s × 60 fps = gopLength 120 for the default keyframe interval) — i.e. **exactly the same cadence the pure-function tests already predict** (`test_nvenc_gop_aq_config.cpp`'s `DefaultGop120_KeyframesEvery120` test uses this exact expectation). This is the "before/after identical positions" check the original spec envisioned via a matrix-harness run; since that harness (S2/S3 of the parent spec) was deferred, a single post-change recording checked against the already-known-correct pure-function cadence serves the same purpose — the pure function didn't change, so if the recording's positions match its predictions, the enforcement mechanism (this task's actual change) is proven equivalent in observable outcome to the old prediction-only mechanism.
5. Confirm the file is fully decodable end-to-end (`ffprobe` completes without decode errors on the whole file, not just the keyframe scan) — a botched `FORCEIDR` submission would typically manifest as a decode error, not just a misplaced keyframe.
6. Delete the scratch recording afterward (never commit it).

Record the exact command(s) run and the `ffprobe` keyframe-index output in the task report.

- [ ] **Step 8: Commit**

```bash
git add libs/recorder_core/src/nvenc_encoder.cpp libs/recorder_core/src/nvenc_encoder.h
git commit -m "fix(nvenc): drive keyframe cadence with an explicit FORCEIDR instead of relying on idrPeriod"
```

---

## Test-/Verify-Plan

### CI-fähig
- `test_nvenc_gop_aq_config.cpp`'s existing `NextGopKeyframePhase`/`ApplyGopToNvenc`/`ComputeGopLength`/`ApplySpatialAqToNvenc` tests, and the full `recorder_core.test_nvenc_*` suite, as an unmodified regression gate (no new tests needed — this task changes no pure-function behavior, only how an unchanged pure function's result is consumed at a call site with no CI seam).

### Not CI-testable (documented, not silently skipped)
- The actual `EncodeFrame` FORCEIDR-flag behavior change: no unit-test seam exists (real NVENC session required). Verified once, manually, via a real `--auto-record` recording + `ffprobe -show_frames` keyframe-position check (Step 7).

### User-live
- None required beyond Step 7's own verification — this is a narrow, backstop-preserving correctness fix with no user-visible behavior change (same keyframe cadence, same file structure). The broader spec's B-Frames-related user-live items (playback matrices, HDR10+B-Frames, etc.) belong to the later M-2/S7 work this fix prepares for, not to this task.

---

## Out of Scope (do not implement here)

- Any B-Frames/Lookahead/Temporal-AQ engine wiring (S5/S6/S7 of the parent spec, gated on the separate `nvenc-async-pipeline-spec` M-1 work landing first).
- The SSIM/VMAF measurement harness (S2/S3 of the parent spec) — explicitly deferred; not a prerequisite for this task.
- Any change to `RequestKeyframe()`'s public signature or the split-boundary caller in `video_thread.cpp` — it already calls `RequestKeyframe()` and reads the output packet's real `pictureType`-derived keyframe flag, both of which are unaffected by this change.
