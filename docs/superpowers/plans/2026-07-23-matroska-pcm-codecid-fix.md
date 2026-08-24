# Matroska PCM CodecID Bugfix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the Matroska CodecID strings ExoSnap writes for PCM audio tracks — `"A_PCM/INT_LIT"` / `"A_PCM/FLOAT_IEEE"` (underscore before the last segment) are **not** the strings any spec-compliant Matroska demuxer recognizes; the correct strings, confirmed against both FFmpeg's own `ff_mkv_codec_tags` table and the IETF CELLAR Matroska codec specification, are `"A_PCM/INT/LIT"` / `"A_PCM/FLOAT/IEEE"` (slash-delimited throughout). This is why the 2026-07-12 investigation found "FFmpeg's matroska demuxer can't read PCM tracks, demuxes as codec `none`" — it was never a gap in the vendored FFmpeg build, it is ExoSnap writing a non-standard string. This bug affects the already-shipped Float32-PCM-in-MKV feature's compatibility with FFmpeg-based tools and any other compliant player, independent of any FFmpeg build/vendoring work.

**Architecture:** No architecture change. One string literal is wrong in one call site (`MatroskaStreamWriter::Open`'s track-header-writing code). Fix the two literals, update the two existing tests that currently assert the *wrong* string as expected output (they were written against the bug), add two new tests that prove real interop by demuxing the written file with FFmpeg's own matroska demuxer (the previous tests only did a raw byte-search in the output — sufficient to catch a regression in "did we write the string we intended," but not sufficient to catch "is the string we intended actually the spec-correct one," which is exactly how this bug went unnoticed).

**Tech Stack:** C++20, libebml/libmatroska (the writer), FFmpeg libavformat (the new demux-based regression tests), GoogleTest.

## Global Constraints

- This plan does not touch FFmpeg vendoring, the ffmpeg-build repo, or any encoder/container feature work. It is a pure correctness fix to existing, already-shipped PCM-in-MKV behavior (ADR 0027, #195).
- Full build (not just `--target exosnap`) before running tests — this project's convention for anything touching `engine`.
- Never interact with a running ExoSnap instance; this task has no UI surface, ctest is the verification path.
- The fix must be provable via a demux-level test (open the written file with FFmpeg's own `avformat_open_input`/`avformat_find_stream_info` and assert the resulting `AVCodecID`), not merely a byte-search — a byte-search only proves "we wrote what we intended," not "what we intended is correct," which is precisely how the original bug shipped.

---

### Task 1: Fix the PCM CodecID strings and prove real FFmpeg-demuxer interop

**Files:**
- Modify: `libs/engine/src/matroska_stream_writer.cpp:393-394`
- Modify: `libs/engine/src/matroska_stream_writer.h:106,143` (comments only)
- Modify: `libs/engine/src/pcm_audio_encoder.h:10,21,70` (comments only)
- Modify: `libs/engine/src/pcm_audio_encoder.cpp:163` (comment only)
- Modify: `libs/engine/src/audio_thread.cpp:77,82` (comments only)
- Modify: `libs/engine/src/mux_thread.cpp:191` (comment only)
- Modify: `libs/engine/src/recorder_session.cpp:259,263,313` (comments/log message)
- Modify: `libs/engine/tests/test_matroska_stream_writer.cpp`
- Modify: `libs/engine/CMakeLists.txt:1164-1170`

**Interfaces:**
- Consumes: `exosnap::engine::MatroskaStreamWriter::Open(const MatroskaStreamConfig&) -> bool` (existing, unchanged signature), `exosnap::engine::StreamAudioCodec::Pcm` (existing enum value), `MatroskaStreamConfig::audio_float` (existing bool field).
- Produces: no new public API. The two new tests are the only new surface, and they are test-only.

**Background — read before starting:** The Matroska specification (IETF CELLAR draft, and mirrored verbatim in FFmpeg's `libavformat/matroska.c` `ff_mkv_codec_tags[]` table) defines PCM CodecIDs as `"A_PCM/INT/BIG"`, `"A_PCM/INT/LIT"`, and `"A_PCM/FLOAT/IEEE"` — every segment separated by `/`, matching the general Matroska CodecID convention (`V_MPEG4/ISO/AVC`, `S_TEXT/UTF8`, etc.). `matroska_stream_writer.cpp:393-394` currently writes `"A_PCM/INT_LIT"` and `"A_PCM/FLOAT_IEEE"` — the last separator is `_` instead of `/`. Any demuxer doing a literal string match against its codec table (FFmpeg's matroska demuxer does exactly this) will not recognize the track and will report `codec_id == AV_CODEC_ID_NONE` for it — this is the exact "audio stream demuxes as codec `none`" symptom recorded in project memory from 2026-07-12. The existing tests `Pcm_WritesPcmCodecIdAndBitDepth` and `PcmFloat_WritesFloatCodecIdAndBitDepth32` (`test_matroska_stream_writer.cpp:372-446`) currently assert the *buggy* strings are present — they were written against the bug and must be corrected, not just left passing.

- [ ] **Step 1: Add FFmpeg to the test target's link libraries**

  In `libs/engine/CMakeLists.txt:1164-1170`, change:
  ```cmake
  exosnap_add_gtest(
      NAME test_matroska_stream_writer
      TEST_PREFIX engine.
      SOURCES tests/test_matroska_stream_writer.cpp
      LIBRARIES engine EBML::ebml Matroska::matroska
  )
  target_include_directories(test_matroska_stream_writer PRIVATE src)
  ```
  to:
  ```cmake
  exosnap_add_gtest(
      NAME test_matroska_stream_writer
      TEST_PREFIX engine.
      SOURCES tests/test_matroska_stream_writer.cpp
      LIBRARIES engine EBML::ebml Matroska::matroska FFmpeg::mux
  )
  target_include_directories(test_matroska_stream_writer PRIVATE src)
  ```
  (`FFmpeg::mux` is the existing convenience target from `cmake/VendorFFmpeg.cmake` bundling avformat+avcodec+avutil+swresample; it is already used the same way by `test_mp4_remuxer` a few lines below in the same file.)

- [ ] **Step 2: Write the new failing demux-interop tests (against the still-buggy code)**

  In `libs/engine/tests/test_matroska_stream_writer.cpp`, add near the top (after the existing `#include` block at line 15):
  ```cpp
  extern "C" {
  #include <libavformat/avformat.h>
  }
  ```

  Then add these two tests directly after `PcmFloat_WritesFloatCodecIdAndBitDepth32` (after its closing `}` — the test currently ends around line 447; add after that closing brace):
  ```cpp
  // 2c. The CodecID written for integer PCM must be the exact string FFmpeg's
  //     matroska demuxer (and any other spec-compliant demuxer) recognizes.
  //     A byte-search only proves "we wrote what we intended" -- this proves
  //     "what we intended is actually readable."
  TEST_F(StreamWriterTest, Pcm_CodecIdIsReadableByFfmpegMatroskaDemuxer) {
      MatroskaStreamConfig c;
      c.output_path = tmp_;
      c.video_codec_id = "V_AV1";
      c.video_codec_private = FakeAv1Cp();
      c.encode_width = 1280;
      c.encode_height = 720;
      c.frame_rate_num = 60;
      c.frame_rate_den = 1;
      c.audio_codec = StreamAudioCodec::Pcm;
      c.audio_track_count = 1;
      c.audio_tracks[0].codec_private = {};

      MatroskaStreamWriter w;
      ASSERT_TRUE(w.Open(c));
      FeedSeconds(w, 3.0, 30, 32);
      ASSERT_TRUE(w.Finalize());
      ASSERT_FALSE(w.failed()) << w.error();

      AVFormatContext* fmt_ctx = nullptr;
      ASSERT_EQ(avformat_open_input(&fmt_ctx, tmp_.c_str(), nullptr, nullptr), 0)
          << "FFmpeg could not open the file MatroskaStreamWriter produced";
      ASSERT_GE(avformat_find_stream_info(fmt_ctx, nullptr), 0);

      int audio_stream_idx = -1;
      for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
          if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
              audio_stream_idx = static_cast<int>(i);
              break;
          }
      }
      ASSERT_NE(audio_stream_idx, -1) << "No audio stream found by the demuxer";
      EXPECT_EQ(fmt_ctx->streams[static_cast<unsigned>(audio_stream_idx)]->codecpar->codec_id,
                AV_CODEC_ID_PCM_S16LE)
          << "FFmpeg's matroska demuxer did not recognize the PCM CodecID -- the string "
             "MatroskaStreamWriter wrote does not match FFmpeg's ff_mkv_codec_tags table";

      avformat_close_input(&fmt_ctx);
  }

  // 2d-float. Same proof for the float-PCM CodecID.
  TEST_F(StreamWriterTest, PcmFloat_CodecIdIsReadableByFfmpegMatroskaDemuxer) {
      MatroskaStreamConfig c;
      c.output_path = tmp_;
      c.video_codec_id = "V_AV1";
      c.video_codec_private = FakeAv1Cp();
      c.encode_width = 1280;
      c.encode_height = 720;
      c.frame_rate_num = 60;
      c.frame_rate_den = 1;
      c.audio_codec = StreamAudioCodec::Pcm;
      c.audio_track_count = 1;
      c.audio_tracks[0].codec_private = {};
      c.audio_bit_depth = 32;
      c.audio_float = true;

      MatroskaStreamWriter w;
      ASSERT_TRUE(w.Open(c)) << w.error();
      FeedSeconds(w, 3.0, 30, 32);
      ASSERT_TRUE(w.Finalize());
      ASSERT_FALSE(w.failed()) << w.error();

      AVFormatContext* fmt_ctx = nullptr;
      ASSERT_EQ(avformat_open_input(&fmt_ctx, tmp_.c_str(), nullptr, nullptr), 0)
          << "FFmpeg could not open the file MatroskaStreamWriter produced";
      ASSERT_GE(avformat_find_stream_info(fmt_ctx, nullptr), 0);

      int audio_stream_idx = -1;
      for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
          if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
              audio_stream_idx = static_cast<int>(i);
              break;
          }
      }
      ASSERT_NE(audio_stream_idx, -1) << "No audio stream found by the demuxer";
      EXPECT_EQ(fmt_ctx->streams[static_cast<unsigned>(audio_stream_idx)]->codecpar->codec_id,
                AV_CODEC_ID_PCM_F32LE)
          << "FFmpeg's matroska demuxer did not recognize the float-PCM CodecID -- the string "
             "MatroskaStreamWriter wrote does not match FFmpeg's ff_mkv_codec_tags table";

      avformat_close_input(&fmt_ctx);
  }
  ```

- [ ] **Step 3: Run the new tests to verify they fail against the still-buggy writer**

  Configure/build `engine` tests (adjust build dir to your local preset), then:
  ```
  ctest --test-dir build/windows-x64-debug -R "recorder_core.Pcm_CodecIdIsReadableByFfmpegMatroskaDemuxer|recorder_core.PcmFloat_CodecIdIsReadableByFfmpegMatroskaDemuxer" -V
  ```
  Expected: **FAIL** on both — the `EXPECT_EQ` lines report `codec_id` as `AV_CODEC_ID_NONE` (value `0`), not `AV_CODEC_ID_PCM_S16LE`/`AV_CODEC_ID_PCM_F32LE`. This confirms the new tests actually catch the bug before you fix it.

- [ ] **Step 4: Fix the CodecID strings**

  In `libs/engine/src/matroska_stream_writer.cpp:393-394`, change:
  ```cpp
              libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue(m_config.audio_float ? "A_PCM/FLOAT_IEEE"
                                                                                              : "A_PCM/INT_LIT");
  ```
  to:
  ```cpp
              libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue(m_config.audio_float ? "A_PCM/FLOAT/IEEE"
                                                                                              : "A_PCM/INT/LIT");
  ```

- [ ] **Step 5: Update the two existing tests that asserted the buggy strings**

  In `libs/engine/tests/test_matroska_stream_writer.cpp:395-398`, change:
  ```cpp
      // CodecID "A_PCM/INT_LIT" present in the rendered container.
      const std::string kPcmId = "A_PCM/INT_LIT";
      const auto id_it = std::search(d.begin(), d.end(), kPcmId.begin(), kPcmId.end());
      EXPECT_NE(id_it, d.end()) << "A_PCM/INT_LIT CodecID not found in output";
  ```
  to:
  ```cpp
      // CodecID "A_PCM/INT/LIT" present in the rendered container.
      const std::string kPcmId = "A_PCM/INT/LIT";
      const auto id_it = std::search(d.begin(), d.end(), kPcmId.begin(), kPcmId.end());
      EXPECT_NE(id_it, d.end()) << "A_PCM/INT/LIT CodecID not found in output";
  ```

  At `test_matroska_stream_writer.cpp:435-438`, change:
  ```cpp
      // CodecID "A_PCM/FLOAT_IEEE" present in the rendered container.
      const std::string kPcmFloatId = "A_PCM/FLOAT_IEEE";
      const auto id_it = std::search(d.begin(), d.end(), kPcmFloatId.begin(), kPcmFloatId.end());
      EXPECT_NE(id_it, d.end()) << "A_PCM/FLOAT_IEEE CodecID not found in output";
  ```
  to:
  ```cpp
      // CodecID "A_PCM/FLOAT/IEEE" present in the rendered container.
      const std::string kPcmFloatId = "A_PCM/FLOAT/IEEE";
      const auto id_it = std::search(d.begin(), d.end(), kPcmFloatId.begin(), kPcmFloatId.end());
      EXPECT_NE(id_it, d.end()) << "A_PCM/FLOAT/IEEE CodecID not found in output";
  ```

  At `test_matroska_stream_writer.cpp:440-445`, change:
  ```cpp
      // The plain int CodecID must NOT appear as this track's CodecID: search for
      // the more specific "A_PCM/INT_LIT" string, which is not a substring of
      // "A_PCM/FLOAT_IEEE" and so must be absent from a float-only track.
      const std::string kPcmIntId = "A_PCM/INT_LIT";
      const auto int_id_it = std::search(d.begin(), d.end(), kPcmIntId.begin(), kPcmIntId.end());
      EXPECT_EQ(int_id_it, d.end()) << "A_PCM/INT_LIT must not appear for a float-PCM track";
  ```
  to:
  ```cpp
      // The plain int CodecID must NOT appear as this track's CodecID: search for
      // the more specific "A_PCM/INT/LIT" string, which is not a substring of
      // "A_PCM/FLOAT/IEEE" and so must be absent from a float-only track.
      const std::string kPcmIntId = "A_PCM/INT/LIT";
      const auto int_id_it = std::search(d.begin(), d.end(), kPcmIntId.begin(), kPcmIntId.end());
      EXPECT_EQ(int_id_it, d.end()) << "A_PCM/INT/LIT must not appear for a float-PCM track";
  ```

- [ ] **Step 6: Update stale comments referencing the old (wrong) strings**

  These are comment-only occurrences (verified via grep — none of them feed a `SetValue`/write call); update the text so nothing in the codebase documents the wrong CodecID string anymore:
  - `libs/engine/src/matroska_stream_writer.h:106` and `:143`: replace `A_PCM/INT_LIT`/`A_PCM/FLOAT_IEEE` with `A_PCM/INT/LIT`/`A_PCM/FLOAT/IEEE`.
  - `libs/engine/src/pcm_audio_encoder.h:10,21,70`: same replacement.
  - `libs/engine/src/pcm_audio_encoder.cpp:163`: same replacement.
  - `libs/engine/src/audio_thread.cpp:77,82`: same replacement.
  - `libs/engine/src/mux_thread.cpp:191`: same replacement.
  - `libs/engine/src/recorder_session.cpp:259,313`: same replacement (comments).
  - `libs/engine/src/recorder_session.cpp:263`: this one is a user/log-facing error message (`"WebM and MP4 cannot carry A_PCM/INT_LIT in this build"`) — reword to not depend on the exact CodecID string, e.g. `"WebM and MP4 cannot carry PCM audio in this build"`.

- [ ] **Step 7: Run the full engine test suite to verify the fix and check for regressions**

  Full build (not just `--target exosnap` — this project's convention for anything touching `engine`):
  ```
  cmake --build build/windows-x64-debug
  ```
  Then:
  ```
  ctest --test-dir build/windows-x64-debug -R "recorder_core.*Pcm" -V
  ```
  Expected: **PASS** — all of `Pcm_WritesPcmCodecIdAndBitDepth`, `PcmFloat_WritesFloatCodecIdAndBitDepth32`, `Pcm_CodecIdIsReadableByFfmpegMatroskaDemuxer`, and `PcmFloat_CodecIdIsReadableByFfmpegMatroskaDemuxer` now pass. Also run the broader matroska/recovery suites to catch any other test asserting the old string:
  ```
  ctest --test-dir build/windows-x64-debug -R "recorder_core.(StreamWriterTest|MatroskaMuxStructure|RecoveryService)" -V
  ```
  Expected: **PASS** (no other test should have depended on the buggy string — confirmed by the earlier `grep` sweep in Step 6, but re-run to be sure).

- [ ] **Step 8: Commit**

  ```bash
  git add libs/engine/src/matroska_stream_writer.cpp \
          libs/engine/src/matroska_stream_writer.h \
          libs/engine/src/pcm_audio_encoder.h \
          libs/engine/src/pcm_audio_encoder.cpp \
          libs/engine/src/audio_thread.cpp \
          libs/engine/src/mux_thread.cpp \
          libs/engine/src/recorder_session.cpp \
          libs/engine/tests/test_matroska_stream_writer.cpp \
          libs/engine/CMakeLists.txt
  git commit -m "$(cat <<'EOF'
  Fix non-standard Matroska PCM CodecID strings (A_PCM/INT_LIT -> A_PCM/INT/LIT)

  ExoSnap wrote "A_PCM/INT_LIT" and "A_PCM/FLOAT_IEEE" (underscore before the
  last segment). The Matroska spec and FFmpeg's own ff_mkv_codec_tags table
  use "A_PCM/INT/LIT" and "A_PCM/FLOAT/IEEE" (slash-delimited throughout).
  Any demuxer doing a literal string match -- including FFmpeg's own matroska
  demuxer -- failed to recognize the track and reported codec_id == NONE.
  This was previously misdiagnosed as a vendored-FFmpeg-build limitation.
  EOF
  )"
  ```

  Note: no PR/merge instructions are given here deliberately — follow this
  project's normal review flow for landing the change (this plan does not
  assume direct-to-main authorization the way the earlier DXGI-magnifier
  plan had).
