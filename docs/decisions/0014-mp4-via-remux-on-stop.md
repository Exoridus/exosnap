# ADR 0014: MP4 via Remux-on-Stop (Remux-first, no fMP4 Recording Writer)

## Status

Accepted — resolves the deferred fMP4 backend choice from ADR 0008.

## Context

ADR 0008 identified two candidate backends for MP4 output and deferred the choice to the start
of the 0.2.0 slice. The four evaluation criteria were: crash-resilient fragment semantics,
remux/trim capability, GPL surface, and binary size. Parallel evaluations of a controlled
ISO-BMFF writer and libavformat were completed in June 2026.

**Controlled ISO-BMFF writer findings:**
- Full scope (writer + reader + validation matrix): 50–73 person-days.
  - Writer alone (with `mfra/tfra`): 23–35 days.
  - Reader required for remux-on-stop and Quick Trim: 18–26 additional days.
  - Validation/compatibility matrix: 9–12 additional days.
- No viable permissively-licensed C++ ISO-BMFF library exists (Bento4 is GPL-2.0; mp4v2 is
  unmaintained); every box type must be hand-coded from scratch.
- Apple/QuickTime compatibility (`hvc1`/`hev1`, empty `stbl` boxes, `esds` descriptor encoding,
  brand signaling) requires real hardware validation that cannot be short-circuited by spec reading.
- The simpler MKV container — with a ready-made library — still required ~1,150 LOC + 7
  correctness-fix commits + 1,065 LOC of tests before reaching production quality.

**libavformat spike findings (practically verified):**
- fMP4 with `+frag_keyframe+empty_moov+default_base_moof` survives hard process kills: kills at
  3 s, 8 s, and 15 s into a 30 s recording recovered 2.03 s, 7.03 s, and 14.03 s respectively
  (loss ≤ 1 fragment; the unflushed in-progress fragment is the only loss).
- Remux from fMP4 to progressive MP4 with `+faststart` works via stream-copy; `moov` is confirmed
  before `mdat` in the output; a 16 MB file remuxes in under 200 ms.
- Keyframe-aligned trim via `av_seek_frame()` + stream-copy works correctly.
- License: 4 DLLs (avformat/avcodec/avutil/swresample) under LGPL-2.1-or-later; dynamically
  linked against ExoSnap (GPL-3.0-or-later) — no new obligations.
- Binary cost: ~92 MB uncompressed / ~28–32 MB compressed in the portable ZIP (4 mux-only DLLs);
  a custom minimal build is estimated at 13–20 MB uncompressed (deferred to 0.5.0 packaging slice).
- MSVC gotcha: `av_err2str()` requires a C++ wrapper (`thread_local` buffer); documented.

**Key structural insight:** The original motivation for building an fMP4 recording writer was
crash resilience during recording — specifically to avoid the "moov-at-end" failure mode of
the current MF/SinkWriter path. However, that crash-resilient property is already provided by
the existing libmatroska path: MKV writes sealed clusters incrementally; the file is truncation-
tolerant without any finalization step. The fMP4 recording writer would have been a transient
format that is remuxed to progressive MP4 on stop anyway — precisely the role MKV already plays.

## Decision

### MKV as the sole recording container

Recording always writes to MKV via the existing libmatroska path. This path is production-quality,
crash-resilient by design (incremental cluster writes, no mandatory finalization), and validated
through the MATROSKA-MUX-CORRECTNESS-R1 work.

No fMP4 recording writer is built — neither via libavformat nor as a controlled ISO-BMFF writer.

### Progressive MP4 via remux-on-stop

When the user selects MP4 as the output container, ExoSnap:

1. Records to a transient MKV file (same recording path as for MKV output).
2. On clean stop: remuxes the transient MKV to a progressive MP4 (faststart layout) via
   libavformat stream-copy. The remux runs as a background job with progress feedback to the UI.
3. On remux success: **renames** the transient MKV to `<stem>.edit.mkv` (edit master companion
   file) instead of deleting it. This companion file is the source for subsequent trim/remux
   operations from the Edit/Export surface (ADR 0022). It is retained on disk alongside the MP4
   output until the user explicitly removes it.
4. On remux failure: retains the transient MKV under its original name and surfaces the error to
   the user (the MKV is a valid, playable file).

### libavformat as remux and trim engine

libavformat (BtbN lgpl-shared prebuilt, pinned by SHA256) is introduced as the remux/trim engine.
It is integrated via CMake FetchContent, following the same vendoring pattern as libmatroska/libebml.

The 4 mux-only DLLs (avformat, avcodec, avutil, swresample) are shipped in the portable ZIP.
The BtbN `lgpl-shared` build variant is used — not `gpl-shared`, which includes GPL-licensed
codecs (x264, x265) that are not needed for the mux/remux/trim path.

This same libavformat instance is the engine for Quick Trim (0.11.0) on both MKV and MP4 inputs.

### Removal of the MF/SinkWriter MP4 path

The transitional Media Foundation / SinkWriter MP4 path is removed. Implementation of the
removal is a separate slice; this ADR records the decision. The libavformat remux path fully
replaces it.

### LGPL compliance

- The FFmpeg LICENSE file from the BtbN archive is included in the portable ZIP.
- The About overlay credits FFmpeg and links to the upstream repository.
- Dynamic linking satisfies LGPL §4; users can replace the DLLs without relinking ExoSnap.

## Consequences

- **Stop finalization:** MP4 output requires a background remux job after recording stops.
  The UI must show progress and distinguish "Saving…" (remux in progress) from "Saved" (complete).
  The job is cancellable; cancellation retains the transient MKV.

- **Split recording with MP4 (0.2.0 — MP4-SPLIT-REMUX-R1):** When split is enabled with MP4
  output, each completed segment MKV is remuxed to progressive MP4 in the background while
  recording continues into the next segment. The coordinator maintains a vector of `SegmentRemuxJob`
  threads; "Saved" is only reported when all segment remux jobs have drained successfully. The
  recovery manifest lifecycle (entry-before-start → `finalized=true` before remux → remove on
  success) applies per segment.

- **Disk space:** During a remux, the transient MKV and the output MP4 coexist briefly,
  requiring approximately 2× the final file size in free space. The low-disk guard (0.2.0)
  must account for this remux reserve when computing the hard-stop threshold. For split
  recordings, the reserve is the conservative sum of all pending segment remux job transient
  MKV sizes plus the current live segment estimate.

- **Audio codec gating:** Opus audio is not supported in MP4 (see ADR 0010 compatibility
  registry; libavformat correctly muxes Opus into MKV/WebM but the container compatibility
  classification for Opus-in-MP4 is `Prohibited`). Presets or user selections that combine
  Opus audio with MP4 output are rejected by the compatibility registry before recording begins.

- **HEVC in MP4 — `hvc1`/`hev1` (implemented, 0.7.0):** libavformat defaults to `hev1` for HEVC
  in MP4. For Apple compatibility, `RemuxStreamCopy` now sets
  `codec_tag = MKTAG('h','v','c','1')` on the HEVC video stream (gated to the MP4 muxer; the
  Matroska muxer maps tracks by CodecID and ignores `codec_tag`). This is a zero-cost,
  metadata-only change applied before `avformat_write_header()`. Because the transient MKV already
  stores parameter sets out-of-band in `hvcC`, the plain stream-copy yields a conformant `hvc1`
  file. A unit test (`RemuxerTest.Mp4HevcTaggedHvc1`) builds a `V_MPEGH/ISO/HEVC` MKV, remuxes it,
  and asserts the output video stream is tagged `hvc1` (not `hev1`). Real-file verification
  (`ffprobe` / Bento4 + QuickTime playback) on NVIDIA hardware remains the GPU-smoke gate before
  the registry promotes MP4 + HEVC + AAC beyond `ValidUnvalidated` (ADR 0010).

- **Crash recovery:** Recovery at startup operates on MKV artefacts. The recovery UI (0.2.0)
  offers: finalize/keep as MKV, or export to MP4 via remux.

- **Quick Trim (0.11.0):** The same libavformat instance handles stream-copy trim for both
  MKV and MP4 inputs. No separate trim engine is required.

- **fMP4 direct recording** (record straight to fMP4 without a remux step, eliminating the
  stop-time wait): this remains a documented future option. It is cheap to add later given
  libavformat is already linked, but it is not 0.2.0 scope. The MKV-first path eliminates the
  urgency: recordings are crash-safe and the remux is near-instantaneous for typical file sizes.

- **Binary size:** ~28–32 MB added to the portable ZIP. The maintainer has accepted this cost.
  A custom minimal build (~13–20 MB uncompressed) is explicitly scheduled for the 0.5.0
  packaging slice.
