# FDK-AAC Removal and Structural Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the AAC encoder migration that ADR 0052 deliberately deferred (repin the vendored FFmpeg build to the now-published encoder-enabled release, cut over, and delete FDK-AAC entirely), and clear two small, long-standing dead-code items from the technical-cleanup roadmap (`.workspace/exosnap-technical-cleanup-and-multivendor-roadmap.md`): a duplicate/orphaned `NvencVideoEncoder` header, and the still-unimplemented `recorder_facade` placeholder.

**Architecture:** No architectural change. This is a like-for-like encoder swap behind the existing `IAudioEncoder` interface (`FfmpegAacEncoder` already implements it and is already unit-tested; only its wiring and the old implementation's removal are pending), plus two zero-consumer file/target deletions. No user-visible behavior changes: AAC stays AAC-LC, 44.1/48 kHz, mono/stereo, default 192 kbit/s, raw access units + `AudioSpecificConfig`.

**Tech Stack:** C++20, CMake `FetchContent`, GoogleTest (`exosnap_add_gtest`), FFmpeg `avcodec`/`avutil`/`swresample` (LGPL-2.1+, vendored via `cmake/VendorFFmpeg.cmake`).

## Global Constraints

- No FDK-AAC code, fetch, or license text may remain after Task 3. No `--enable-nonfree` or GPL-only FFmpeg components may be introduced.
- AAC behavior must not change: AAC-LC only, 44.1 kHz and 48 kHz, mono and stereo, default bitrate 192 kbit/s (range 64–320 clamped), raw AAC access units (no ADTS), `CodecPrivateBytes()` returns `AudioSpecificConfig` for the Matroska `A_AAC` writer and MP4 remux path.
- Every task must end with a full build (`recorder_core` + its tests) and the relevant `ctest` subset green before moving on. This project's `--target exosnap` alone does not build tests — do a full build.
- Never interact with a running ExoSnap instance (no mouse/keyboard synthesis, no window automation). These are non-UI backend/build changes; no `--visual-test` or app launch is needed for any task in this plan.
- Per explicit user authorization for this cleanup pass: merge each task directly to `main` after it's green — no PR required.
- `docs/product-spec.md` needs no update — AAC-LC, container/codec matrix, and defaults are unchanged (ADR 0052 already states this).

---

### Task 1: Repin vendored FFmpeg to release r5 (encoder-enabled)

**Files:**
- Modify: `cmake/VendorFFmpeg.cmake:15-17,34,41-42`
- Test: `libs/recorder_core/tests/test_ffmpeg_aac_encoder.cpp` (existing file, no code changes — its `GTEST_SKIP()` guards are self-adapting and should stop skipping once linked against an encoder-enabled `avcodec`)

**Interfaces:**
- Consumes: nothing new — `FFmpeg::avcodec` / `FFmpeg::avutil` / `FFmpeg::swresample` imported targets already exist.
- Produces: an `avcodec` DLL where `avcodec_find_encoder(AV_CODEC_ID_AAC)` succeeds, which `FfmpegAacEncoder::Init()` (already implemented in `libs/recorder_core/src/ffmpeg_aac_encoder.cpp`) depends on.

Release `Exoridus/exosnap-ffmpeg-build` tag `r5` was published 2026-07-19 and adds `--enable-encoder=aac` to the configure whitelist (confirmed via `gh release view r5` — asset `ffmpeg-win64-lgpl-shared.zip`, DLL names unchanged: `avformat-62.dll avcodec-62.dll avutil-60.dll swresample-6.dll`). This was the external blocker ADR 0052 described ("the maintainer cuts the release by pushing the r5 tag") — it is no longer a blocker.

- [ ] **Step 1: Update the version comment block and cache variable**

In `cmake/VendorFFmpeg.cmake`, replace lines 15-17:

```cmake
# FFmpeg build: Exoridus/exosnap-ffmpeg-build release r4 (upstream n8.1.1)
# Release tag:  r4
# License:      LGPL-2.1-or-later (compatible with ExoSnap GPL-3.0-or-later)
```

with:

```cmake
# FFmpeg build: Exoridus/exosnap-ffmpeg-build release r5 (upstream n8.1.1)
# Release tag:  r5
# License:      LGPL-2.1-or-later (compatible with ExoSnap GPL-3.0-or-later)
```

Then add a new comment line after the existing `# r3 -> r4: ...` block (after line 27, before the blank line at 28):

```cmake
# r4 -> r5: added --enable-encoder=aac. Enables FfmpegAacEncoder (ADR 0052) to
# actually produce output; r1-r4 had zero encoders (mux/demux/decode only).
```

- [ ] **Step 2: Bump the pinned version cache variable**

Replace line 34:

```cmake
set(EXOSNAP_FFMPEG_VERSION "r4-n8.1.1"
```

with:

```cmake
set(EXOSNAP_FFMPEG_VERSION "r5-n8.1.1"
```

- [ ] **Step 3: Update the FetchContent URL and hash**

Replace lines 41-42:

```cmake
    URL      "https://github.com/Exoridus/exosnap-ffmpeg-build/releases/download/r4/ffmpeg-win64-lgpl-shared.zip"
    URL_HASH "SHA256=580DEC5F22E213465D613F49A4903FE98C943B46480E9ED5F8BBAF8A6E1F4206"
```

with:

```cmake
    URL      "https://github.com/Exoridus/exosnap-ffmpeg-build/releases/download/r5/ffmpeg-win64-lgpl-shared.zip"
    URL_HASH "SHA256=D40D2B3D14CD6065B57AB7735E977B01FED447AC534A4E19B74C490E1C3BBEED"
```

- [ ] **Step 4: Reconfigure and rebuild, confirming the new archive downloads**

Run a clean CMake reconfigure so `FetchContent` re-resolves the URL (delete or repoint the existing `_deps/ffmpeg_prebuilt-subbuild` cache for this build tree if CMake does not pick up the URL change automatically — check the build directory's `_deps/ffmpeg_prebuilt-src` timestamp to confirm it re-downloaded). Then do a full build of `recorder_core` and its tests.

- [ ] **Step 5: Run the FFmpeg AAC encoder tests and confirm the encode-path cases now PASS instead of SKIP**

Run: `ctest --test-dir <build-dir> -R test_ffmpeg_aac_encoder -V`

Expected: all cases pass, and in particular the ones previously gated by `GTEST_SKIP() << "AAC encoder not compiled into this avcodec build."` (test file lines ~120, 133, 162, 175) now execute for real instead of skipping. If they still skip, the DLL did not actually update — stop and diagnose (check the build dir's downloaded FFmpeg `bin/avcodec-62.dll` size/timestamp against the r5 asset) rather than proceeding.

- [ ] **Step 6: Commit**

```bash
git add cmake/VendorFFmpeg.cmake
git commit -m "Repin vendored FFmpeg to exosnap-ffmpeg-build r5 (adds AAC encoder)"
```

- [ ] **Step 7: Merge directly to main and push** (per Global Constraints — PR skipped by explicit authorization)

---

### Task 1.5: Fix FfmpegAacEncoder's packet PTS computation (discovered by Task 1's repin)

**Files:**
- Modify: `libs/recorder_core/src/ffmpeg_aac_encoder.h:87` (add a new counter member)
- Modify: `libs/recorder_core/src/ffmpeg_aac_encoder.cpp:165-189` (`ReceiveAvailable`), `:150-159` (`Init`'s reset block), `:322-330` (`Shutdown`'s reset block)
- Test: `libs/recorder_core/tests/test_ffmpeg_aac_encoder.cpp` (existing file, no code changes needed — this task's acceptance signal is `FeedFullFrames_ProducesMonotonicPts` flipping from FAIL to PASS)

**Interfaces:**
- Consumes: nothing new.
- Produces: `EncodedAudioPacket::pts_ns` values that strictly increase packet-to-packet, matching `FdkAacEncoder`'s existing (proven-correct, about-to-be-deleted-in-Task-3) approach exactly.

**Root cause (diagnosed by the Task 1 implementer, confirmed by re-reading the code):** `ReceiveAvailable` currently computes each packet's `pts_ns` by reading `m_pkt->pts` back out of the FFmpeg encode round-trip (`libs/recorder_core/src/ffmpeg_aac_encoder.cpp:179`, `int64_t pts = (m_pkt->pts != AV_NOPTS_VALUE) ? m_pkt->pts : 0;`). Against the real native AAC encoder (only reachable since Task 1's repin — every prior test run against r4 skipped this code path entirely because `Init()` always failed first), the first packets come back with `m_pkt->pts == AV_NOPTS_VALUE` — almost certainly because AAC-LC has encoder priming/lookahead delay and `AVFrame::time_base` is never set on `m_frame` before `avcodec_send_frame`, so avcodec's internal pts bookkeeping can't establish an output pts for the earliest packets. The code's `: 0` fallback silently maps every one of those to `pts_ns == 0`, so packet 0 and packet 1 both report `pts_ns == 0` — not strictly increasing, failing the test.

`FdkAacEncoder` (`libs/recorder_core/src/fdk_aac_encoder.cpp`) never had this problem because it never reads a codec-provided pts at all: it tracks its own running per-channel output-sample counter (`m_accumulated_frames`), incremented by the fixed AAC-LC frame size (1024 samples) every time a packet is emitted, and computes `pkt.pts_ns` purely from that counter (`m_accumulated_frames * 1000000000ULL / m_sample_rate`). This is deterministic, has no dependency on what the codec round-trips back, and is exactly the pattern to port into `FfmpegAacEncoder`.

- [ ] **Step 1: Add a new counter member**

In `libs/recorder_core/src/ffmpeg_aac_encoder.h`, add immediately after the existing `uint64_t m_input_samples = 0;` line (currently line 87):

```cpp
    uint64_t m_output_samples = 0; // per-channel samples represented by packets already emitted (drives pts_ns)
```

(Named distinctly from `m_input_samples` — which tracks samples fed *into* the encoder for `AVFrame::pts` — and distinctly from the `accumulated_frames` out-parameter on `FeedFloat32`'s signature, which is a separate `IAudioEncoder`-interface concept this class already updates independently. `m_output_samples` tracks samples represented by packets already *emitted*, mirroring `FdkAacEncoder::m_accumulated_frames`.)

- [ ] **Step 2: Replace the pts computation in ReceiveAvailable**

In `libs/recorder_core/src/ffmpeg_aac_encoder.cpp`, replace the body of `ReceiveAvailable` (currently lines 165-189):

```cpp
void FfmpegAacEncoder::ReceiveAvailable(uint64_t pts_origin_ns, std::vector<EncodedAudioPacket>& out_packets) {
    if (m_ctx == nullptr || m_pkt == nullptr) {
        return;
    }
    for (;;) {
        int ret = avcodec_receive_packet(m_ctx, m_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LogWarn("avcodec_receive_packet failed: " + AvErr(ret));
            break;
        }

        int64_t pts = (m_pkt->pts != AV_NOPTS_VALUE) ? m_pkt->pts : 0;
        if (pts < 0) {
            pts = 0;
        }

        EncodedAudioPacket pkt;
        const uint64_t rate = (m_sample_rate > 0) ? m_sample_rate : 1;
        pkt.pts_ns = pts_origin_ns + static_cast<uint64_t>(pts) * 1000000000ULL / rate;
        pkt.bytes.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        out_packets.push_back(std::move(pkt));

        av_packet_unref(m_pkt);
    }
}
```

with:

```cpp
void FfmpegAacEncoder::ReceiveAvailable(uint64_t pts_origin_ns, std::vector<EncodedAudioPacket>& out_packets) {
    if (m_ctx == nullptr || m_pkt == nullptr) {
        return;
    }
    for (;;) {
        int ret = avcodec_receive_packet(m_ctx, m_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LogWarn("avcodec_receive_packet failed: " + AvErr(ret));
            break;
        }

        // The native AAC encoder does not reliably round-trip AVFrame::pts through
        // avcodec_receive_packet for every packet (encoder priming/lookahead can
        // leave early packets at AV_NOPTS_VALUE) -- mirror FdkAacEncoder exactly:
        // derive pts_ns from our own running output-sample counter instead of
        // trusting the codec's returned packet pts.
        EncodedAudioPacket pkt;
        const uint64_t rate = (m_sample_rate > 0) ? m_sample_rate : 1;
        pkt.pts_ns = pts_origin_ns + m_output_samples * 1000000000ULL / rate;
        m_output_samples += static_cast<uint64_t>(kFrameSizeSamples);
        pkt.bytes.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        out_packets.push_back(std::move(pkt));

        av_packet_unref(m_pkt);
    }
}
```

- [ ] **Step 3: Reset the new counter alongside the existing ones**

In `libs/recorder_core/src/ffmpeg_aac_encoder.cpp`, in `Init`'s reset block (currently lines 152-157):

```cpp
    m_sample_rate = sample_rate;
    m_channels = channels;
    m_input_samples = 0;
    m_pts_origin_ns = 0;
    m_pts_origin_set = false;
    return true;
```

add `m_output_samples = 0;` alongside `m_input_samples = 0;`:

```cpp
    m_sample_rate = sample_rate;
    m_channels = channels;
    m_input_samples = 0;
    m_output_samples = 0;
    m_pts_origin_ns = 0;
    m_pts_origin_set = false;
    return true;
```

And in `Shutdown`'s reset block (currently around lines 323-329):

```cpp
    m_sample_rate = 0;
    m_channels = 0;
    m_frame_size = kFrameSizeSamples;
    m_input_samples = 0;
    m_pts_origin_ns = 0;
    m_pts_origin_set = false;
    m_codec_private.clear();
```

add `m_output_samples = 0;` alongside `m_input_samples = 0;`:

```cpp
    m_sample_rate = 0;
    m_channels = 0;
    m_frame_size = kFrameSizeSamples;
    m_input_samples = 0;
    m_output_samples = 0;
    m_pts_origin_ns = 0;
    m_pts_origin_set = false;
    m_codec_private.clear();
```

- [ ] **Step 4: Full build**

- [ ] **Step 5: Run the FFmpeg AAC encoder tests and confirm ALL cases pass (not just the target one)**

Run: `ctest --test-dir <build-dir> -R test_ffmpeg_aac_encoder -V`

Expected: 21 tests run, 21 PASS or SKIP (only the two self-adapting `GTEST_SKIP()` cases — `Init_MissingEncoder_FailsGracefullyNotCrash`, `FeedAndFlushAfterFailedInit_AreNoOps` — may legitimately skip; every other case, including all 4 parameterizations of `FeedFullFrames_ProducesMonotonicPts`, must PASS). Zero FAIL. If anything unexpected still fails, do not paper over it — report BLOCKED with the exact failure.

- [ ] **Step 6: Commit**

```bash
git add libs/recorder_core/src/ffmpeg_aac_encoder.h libs/recorder_core/src/ffmpeg_aac_encoder.cpp
git commit -m "Fix FfmpegAacEncoder packet PTS: derive from output-sample counter, not codec round-trip"
```

- [ ] **Step 7: Merge directly to main and push** (per Global Constraints — PR skipped by explicit authorization). This can merge independently of Task 1 having merged first or not — if Task 1's commit (`cmake/VendorFFmpeg.cmake` repin) is not yet on `main`, merge it first (it has no conflicts with this task's files), then this one on top.

---

### Task 2: Cut AAC recording over to FfmpegAacEncoder

**Files:**
- Modify: `libs/recorder_core/src/audio_thread.cpp:99-118` (the `AudioCodec::AacMf` case in `MakeEncoderSetup`)
- Test: existing `libs/recorder_core/tests/test_session_e2e_real_file.cpp` and any existing audio-thread AAC-path tests (run, don't modify) — these exercise the real recording path end-to-end and are the regression guard for this swap.

**Interfaces:**
- Consumes: `recorder_core::FfmpegAacEncoder` (`libs/recorder_core/src/ffmpeg_aac_encoder.h`), specifically `SetBitrateKbps(uint32_t)` and the `IAudioEncoder` virtual contract — both already implemented and already used by `test_ffmpeg_aac_encoder.cpp`.
- Produces: `MakeEncoderSetup`'s `AudioCodec::AacMf` branch now returns a `setup` whose `.encoder` is a `FfmpegAacEncoder`, matching the shape every other branch in that switch already uses.

- [ ] **Step 1: Replace the FdkAacEncoder construction with FfmpegAacEncoder**

In `libs/recorder_core/src/audio_thread.cpp`, replace the `case AudioCodec::AacMf:` block (lines 99-117 in the pre-change file) — delete the `TODO(ADR 0052)` comment block and the two lines it describes:

```cpp
    case AudioCodec::AacMf: {
        auto enc = std::make_unique<FdkAacEncoder>();
        enc->SetBitrateKbps(config.audio_bitrate_kbps);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "FDK-AAC encoder init: ";
```

with:

```cpp
    case AudioCodec::AacMf: {
        // FFmpeg's native AAC-LC encoder (ADR 0052, cut over once
        // exosnap-ffmpeg-build r5 shipped an encoder-enabled avcodec DLL).
        auto enc = std::make_unique<FfmpegAacEncoder>();
        enc->SetBitrateKbps(config.audio_bitrate_kbps);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "FFmpeg AAC encoder init: ";
```

Leave whatever follows the `init_error_prefix` line (e.g. `empty_codec_private_error`, `break;`) untouched.

- [ ] **Step 2: Swap the include**

In the same file's include block, replace:

```cpp
#include "fdk_aac_encoder.h"
```

with:

```cpp
#include "ffmpeg_aac_encoder.h"
```

(Leave this include in place even though Task 3 deletes `fdk_aac_encoder.h` next — Task 3 removes the file, this task only stops referencing it, so ordering between the two tasks doesn't matter for correctness, but do them in this order to keep each task's diff minimal and independently bisectable.)

- [ ] **Step 3: Full build**

Build `recorder_core`, `recorder_core`'s tests, and the `exosnap` app target (audio_thread.cpp is linked into more than just tests).

- [ ] **Step 4: Run the audio/session tests that exercise real AAC encoding end-to-end**

Run: `ctest --test-dir <build-dir> -R "test_session_e2e_real_file|audio_thread" -V`

Expected: PASS. If any test asserts on FDK-specific error message text (e.g. `"FDK-AAC encoder init: "`), update that assertion's expected string to `"FFmpeg AAC encoder init: "` as part of this step — grep for the old string first: `grep -rn "FDK-AAC encoder init" libs/recorder_core/tests/`.

- [ ] **Step 5: Commit**

```bash
git add libs/recorder_core/src/audio_thread.cpp
git commit -m "Cut AudioCodec::AacMf over to FfmpegAacEncoder (ADR 0052)"
```

(If Step 4 required a test-file edit, `git add` that file too and mention it in the commit body.)

- [ ] **Step 6: Merge directly to main and push**

---

### Task 3: Delete FdkAacEncoder and its dependency

**Files:**
- Delete: `libs/recorder_core/src/fdk_aac_encoder.h`
- Delete: `libs/recorder_core/src/fdk_aac_encoder.cpp`
- Delete: `libs/recorder_core/tests/test_fdk_aac_encoder.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt:284` (remove `src/fdk_aac_encoder.cpp` from the source list), `:383` (remove `fdk-aac` from `target_link_libraries`), `:574-580` (remove the `test_fdk_aac_encoder` `exosnap_add_gtest` block)
- Modify: `third_party/CMakeLists.txt:89-114` (remove the entire `# --- fdk-aac-free ...` block)
- Modify: `THIRD_PARTY_NOTICES.md:46-70` (remove the `### FDK-AAC (fdk-aac-free, LC-only fork)` section)
- Modify: `tests/package/license_bundle_test.cpp:54,71` (remove `"fdk-aac.txt"` and `"FDK-AAC"` from whatever lists those lines belong to)
- Modify: `libs/recorder_core/include/recorder_core/interfaces/IAudioEncoder.h:3` (update the stale comment)
- Modify: `libs/recorder_core/tests/test_audio_encoding_params.cpp:12-16,29-33,138-159` (port the `FdkAacBitrateResolve_*` tests to `FfmpegAacEncoder`)

**Interfaces:**
- Consumes: `recorder_core::FfmpegAacEncoder::ResolveBitrateKbps(uint32_t) -> uint32_t` (already implemented, static, public — mirrors `FdkAacEncoder::ResolveBitrateKbps` exactly per `ffmpeg_aac_encoder.h`'s own doc comment).
- Produces: nothing new — this task only deletes.

- [ ] **Step 1: Delete the FdkAacEncoder implementation and its test**

```bash
git rm libs/recorder_core/src/fdk_aac_encoder.h libs/recorder_core/src/fdk_aac_encoder.cpp libs/recorder_core/tests/test_fdk_aac_encoder.cpp
```

- [ ] **Step 2: Remove FdkAacEncoder from the CMake source list**

In `libs/recorder_core/CMakeLists.txt`, delete this line (currently line 284, immediately before `src/ffmpeg_aac_encoder.cpp`):

```cmake
    src/fdk_aac_encoder.cpp
```

- [ ] **Step 3: Remove the fdk-aac link dependency**

In the same file's `target_link_libraries(recorder_core PRIVATE ...)` block, delete this line (currently line 383, between `Matroska::matroska` and `FFmpeg::mux`):

```cmake
        fdk-aac
```

- [ ] **Step 4: Remove the test_fdk_aac_encoder test registration**

In the same file, delete this whole block (currently lines 574-580):

```cmake
exosnap_add_gtest(
    NAME test_fdk_aac_encoder
    TEST_PREFIX recorder_core.
    SOURCES tests/test_fdk_aac_encoder.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_fdk_aac_encoder PRIVATE src)

```

Also trim the now-stale sentence in the `test_ffmpeg_aac_encoder` block's leading comment (currently lines 582-586) that says *"NOTE: against the currently pinned r4 FFmpeg DLL (no encoders) the encode-path cases GTEST_SKIP; only the graceful-missing-encoder case runs for real."* — replace that comment block with:

```cmake
# FfmpegAacEncoder (ADR 0052): native FFmpeg AAC-LC encoder, the sole AAC path
# now that FdkAacEncoder has been removed. Links recorder_core (which carries
# the encoder + FFmpeg::mux/swresample transitively); the FFmpeg DLLs are
# staged next to the test binary by exosnap_add_gtest.
```

- [ ] **Step 5: Remove the fdk-aac third_party fetch**

In `third_party/CMakeLists.txt`, delete the entire block from the `# --- fdk-aac-free (FDK-AAC 2.0.2, LC-only fork) — ADR 0043 ---` comment (currently line 89) through `unset(_exosnap_saved_bsl)` (currently line 114) inclusive, plus the blank line that follows it.

- [ ] **Step 6: Remove the FDK-AAC entry from THIRD_PARTY_NOTICES.md**

Delete the `### FDK-AAC (fdk-aac-free, LC-only fork)` section (currently lines 46-70, i.e. from that heading up to but not including the following `### FLAC` heading).

- [ ] **Step 7: Remove FDK-AAC from the license bundle test's expected lists**

In `tests/package/license_bundle_test.cpp`, remove `"fdk-aac.txt",` from the list containing it (line 54) and remove `"FDK-AAC",` from the list containing it (line 71) — adjust surrounding commas so the lists stay syntactically valid C++ (e.g. if `"FDK-AAC"` was followed by a comma and other entries continue after it, just delete the token and its trailing comma; if it was the last entry, delete the preceding comma instead).

- [ ] **Step 8: Update the stale doc comment in IAudioEncoder.h**

In `libs/recorder_core/include/recorder_core/interfaces/IAudioEncoder.h`, replace:

```cpp
// Windows implementation: FdkAacEncoder.
```

with:

```cpp
// Windows implementation: FfmpegAacEncoder.
```

- [ ] **Step 9: Port the FdkAacBitrateResolve tests to FfmpegAacEncoder**

In `libs/recorder_core/tests/test_audio_encoding_params.cpp`:

Replace the include guard block (currently lines 13-15):

```cpp
#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
#include "fdk_aac_encoder.h"
#endif
```

with:

```cpp
#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
#include "ffmpeg_aac_encoder.h"
#endif
```

Replace the `using` block (currently lines 30-32):

```cpp
#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
using recorder_core::FdkAacEncoder;
#endif
```

with:

```cpp
#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
using recorder_core::FfmpegAacEncoder;
#endif
```

Replace the five test cases (currently lines 140-158):

```cpp
TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_Zero_IsDefault192) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(0u), 192u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_BelowMin_ClampsTo64) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(10u), 64u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_Valid_IsPassedThrough) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(192u), 192u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_Max_IsPassedThrough) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(320u), 320u);
}

TEST(AudioEncodingParamsTest, FdkAacBitrateResolve_AboveMax_ClampsTo320) {
    EXPECT_EQ(FdkAacEncoder::ResolveBitrateKbps(999u), 320u);
}
```

with:

```cpp
TEST(AudioEncodingParamsTest, FfmpegAacBitrateResolve_Zero_IsDefault192) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(0u), 192u);
}

TEST(AudioEncodingParamsTest, FfmpegAacBitrateResolve_BelowMin_ClampsTo64) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(10u), 64u);
}

TEST(AudioEncodingParamsTest, FfmpegAacBitrateResolve_Valid_IsPassedThrough) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(192u), 192u);
}

TEST(AudioEncodingParamsTest, FfmpegAacBitrateResolve_Max_IsPassedThrough) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(320u), 320u);
}

TEST(AudioEncodingParamsTest, FfmpegAacBitrateResolve_AboveMax_ClampsTo320) {
    EXPECT_EQ(FfmpegAacEncoder::ResolveBitrateKbps(999u), 320u);
}
```

- [ ] **Step 10: Full clean reconfigure + build**

A clean CMake reconfigure is important here since a whole `FetchContent` dependency (`fdk-aac`) was removed — stale generated build files can otherwise reference the deleted target. Build `recorder_core`, its full test suite, `tests/package` (license_bundle_test), and the `exosnap` app target.

- [ ] **Step 11: Run the full recorder_core test suite plus the license bundle test**

Run: `ctest --test-dir <build-dir> -R "recorder_core\.|license_bundle" -V`

Expected: all PASS, zero references to `fdk`/`FDK` anywhere in build output or test names. Also grep the working tree to confirm nothing was missed: `grep -rli "fdk" --include=*.cpp --include=*.h --include=CMakeLists.txt --include=*.md .` should return nothing under `libs/`, `third_party/`, `tests/`, or `THIRD_PARTY_NOTICES.md` (the `docs/decisions/0043-*.md` and `docs/decisions/0052-*.md` ADR files are historical record and should still mention FDK-AAC by name — leave those untouched).

- [ ] **Step 12: Commit**

```bash
git add -A -- libs/recorder_core third_party/CMakeLists.txt THIRD_PARTY_NOTICES.md tests/package/license_bundle_test.cpp
git commit -m "Remove FDK-AAC now that FfmpegAacEncoder is the active AAC path (ADR 0052)"
```

- [ ] **Step 13: Merge directly to main and push**

---

### Task 4: Delete the orphaned NvencVideoEncoder header (CV-CLEAN-001)

**Files:**
- Delete: `libs/recorder_core/src/platform/windows/NvencVideoEncoder.h`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing — pure dead-code removal.

Confirmed dead: `libs/recorder_core/src/platform/windows/NvencVideoEncoder.h` (51 lines) has no corresponding `.cpp` and zero `#include` references anywhere in the tree (`grep -rln "platform/windows/NvencVideoEncoder.h"` returns nothing). The real, actively-used encoder is `libs/recorder_core/src/nvenc_video_encoder.h`/`.cpp` (96+ lines, included by `video_thread.cpp` and covered by `tests/test_nvenc_video_encoder_interface.cpp`, and is what `CMakeLists.txt` actually compiles). Do not touch the live one. Do not touch the other two files in `platform/windows/` (`WasapiLoopbackSrc.h`, `WgcCaptureSrc.h`) — they are out of scope for this task; verify they're still referenced elsewhere before assuming otherwise, but this task only removes the one confirmed-dead file.

- [ ] **Step 1: Verify no references exist (repeat the check fresh, in case something changed since planning)**

Run: `grep -rn "platform/windows/NvencVideoEncoder\|platform\\\\windows\\\\NvencVideoEncoder" --include=*.cpp --include=*.h --include=CMakeLists.txt .`

Expected: no output. If something now references it, STOP — do not delete, re-investigate instead.

- [ ] **Step 2: Delete the file**

```bash
git rm libs/recorder_core/src/platform/windows/NvencVideoEncoder.h
```

- [ ] **Step 3: Full build**

Build `recorder_core` and its tests. Since nothing referenced this file, the build should be unaffected — this step exists to catch anything the grep in Step 1 missed (e.g. a build-generated file list).

- [ ] **Step 4: Run the NVENC interface test to confirm the live encoder is untouched**

Run: `ctest --test-dir <build-dir> -R test_nvenc_video_encoder_interface -V`

Expected: PASS, unchanged from before this task.

- [ ] **Step 5: Commit**

```bash
git add -A -- libs/recorder_core/src/platform/windows/NvencVideoEncoder.h
git commit -m "Remove orphaned NvencVideoEncoder.h scaffold (dead code, zero includes)"
```

- [ ] **Step 6: Merge directly to main and push**

---

### Task 5: Remove the recorder_facade placeholder (CV-CLEAN-002)

**Files:**
- Delete: `libs/recorder_facade/` (entire directory: `CMakeLists.txt`, `src/facade_c.cpp`, `include/recorder_facade/facade_c.h`)
- Modify: `CMakeLists.txt:122` (remove `add_subdirectory(libs/recorder_facade)`)
- Modify: `.github/workflows/ci.yml:196` (remove the `-I libs/recorder_facade/include` fragment from that cppcheck/analysis invocation)
- Modify: `scripts/check-quality.ps1:206,239` (remove the two `'-I', 'libs/recorder_facade/include',` entries)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing — pure removal. Confirmed zero consumers: `recorder_facade` is a `SHARED` library (`add_library(recorder_facade SHARED src/facade_c.cpp)`) whose only public symbol is `recorder_facade_ping(void)`, linked by nothing in `app/`, `libs/` (other than itself), or `tests/` — the only two places it's mentioned at all are the root `CMakeLists.txt`'s `add_subdirectory` call and the two static-analysis scripts' include-path flags.

Per the roadmap doc's CV-CLEAN-002, the alternative was "define a real C ABI with versioned structs, ownership rules, and error handling" — that only makes sense once something actually needs to consume ExoSnap's core as a library from outside the Qt app, which is not the case for this pre-1.0 MVP. Removing it now avoids a misleading production target with no useful contract; a real facade can be added later, from scratch, when a concrete consumer exists.

- [ ] **Step 1: Verify no consumers exist (repeat fresh)**

Run: `grep -rln "recorder_facade" --include=*.cpp --include=*.h . | grep -v "libs/recorder_facade"`

Expected: only `CMakeLists.txt`, `.github/workflows/ci.yml`, `scripts/check-quality.ps1`. If anything else shows up, STOP and re-investigate before deleting.

- [ ] **Step 2: Delete the directory**

```bash
git rm -r libs/recorder_facade
```

- [ ] **Step 3: Remove the add_subdirectory call**

In the root `CMakeLists.txt`, delete the line:

```cmake
add_subdirectory(libs/recorder_facade)
```

- [ ] **Step 4: Remove the include-path flag from CI**

In `.github/workflows/ci.yml` around line 196, that line currently reads (as one shell-continuation fragment among several `-I` flags):

```
-I libs/recorder_core/include -I libs/capability/include -I libs/recorder_facade/include `
```

Remove ` -I libs/recorder_facade/include` from it, leaving the other `-I` flags and the line-continuation backtick intact.

- [ ] **Step 5: Remove the include-path entries from the quality script**

In `scripts/check-quality.ps1`, delete both occurrences (lines 206 and 239) of:

```powershell
        '-I', 'libs/recorder_facade/include',
```

- [ ] **Step 6: Full clean reconfigure + build**

A clean reconfigure matters here since a whole subdirectory/target was removed from the CMake tree.

- [ ] **Step 7: Run the full test suite to confirm nothing depended on the facade target existing**

Run: `ctest --test-dir <build-dir>`

Expected: same pass count as before this task (minus zero — nothing should have depended on `recorder_facade`).

- [ ] **Step 8: Run the quality script locally if feasible, to confirm the `-I` removal didn't break invocation**

If `scripts/check-quality.ps1` can be run standalone in this environment, run it and confirm it still executes without a "path not found" or argument-count error. If it can't be run standalone here (e.g. requires CI-only tooling), skip this and rely on Step 4/5's textual correctness plus CI's own future run.

- [ ] **Step 9: Commit**

```bash
git add -A -- libs/recorder_facade CMakeLists.txt .github/workflows/ci.yml scripts/check-quality.ps1
git commit -m "Remove recorder_facade placeholder (zero consumers, no real ABI contract)"
```

- [ ] **Step 10: Merge directly to main and push**

---

## Explicitly out of scope

**CV-WEBCAM-002** (replace `WebcamFrameProvider::TryGetFrame`'s copy-out `std::vector<uint8_t>&` API with an immutable `shared_ptr<const WebcamFrame>` snapshot) is deliberately **not** in this plan. Bundle 3 (PR #243, merged 2026-07-20) already added the generation counter (`out_generation`) to this exact interface and eliminated the main perf cost the roadmap flagged — redundant recomposition when the webcam hasn't produced a new sample. The remaining full API reshape is real but lower-marginal-value now, touches `video_thread.cpp` and `WebcamService.cpp/h` (the same hot files Bundle 3 and this session's PreviewSurface-worktree integration just modified), and deserves its own reviewed plan rather than being folded in here. Revisit as a separate bundle if profiling shows the remaining copy is significant.

**CV-BUG-004** (exhaustive switch for unknown `AudioCodec` values) and the rest of Phase B/C items from the roadmap doc were not re-audited for this plan — only the two Phase C items explicitly confirmed dead (CV-CLEAN-001, CV-CLEAN-002) are included. Re-check the roadmap doc for the remaining items in a future pass.
