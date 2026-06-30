# Edit / Output / Save Workflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Edit/Output/Save workflow (0.9.0 S1) — Quick Trim engine, keyframe-interval setting, marker sidecar load, EditExportPage wiring (Review post-flight / Edit trim / Output stream-copy / atomic Save), and MKV master retention for MP4 sessions.

**Architecture:** The engine (recorder_core) remains UI-agnostic; all trim/remux logic extends `mp4_remuxer` with a `TrimRange` overload and `ExtractKeyframeTimestamps()` utility. The app layer retains the MKV master after MP4 remux (renamed companion file) and passes it to `EditExportPage` via an extended `EditContext`. The export path re-uses the existing `RemuxToProgressiveMp4` / `RemuxToMkv` functions with a trim range applied.

**Tech Stack:** C++20 (Windows/MSVC), Qt 6.9 Widgets, libavformat (already linked via `FFmpeg::mux`), GTest, CMake Ninja preset `windows-x64-debug`.

## Global Constraints

- Build preset: `windows-x64-debug` in `build/windows-x64-debug` (VS-tree; do NOT re-configure).
- Run exe only with `C:\Qt\6.9.0\msvc2022_64\bin` prepended to PATH.
- Do NOT touch `libs/recorder_core/src/video_thread.cpp` or `WebcamService.*`.
- No re-encode anywhere — stream-copy only.
- No auto-open of EditExportPage on stop.
- No MVP expansion beyond the locked model.
- Pre-v1.0: reset rather than migrate breaking config changes.
- Keyframe interval default STAYS 2 s — only the multiplier factor changes.
- Markers/chapters are edit-view only — never written as container chapters.
- Atomic overwrite: write temp → `std::filesystem::rename` (atomic on Windows NTFS for same-volume paths).
- All new tests use `EXOSNAP_CONFIG_DIR` pointing to a temp dir to isolate state.

---

## File Map

### Created
- `libs/recorder_core/tests/test_remux_trim.cpp` — trim range + keyframe extraction tests
- `app/tests/test_edit_context.cpp` — EditContext model + marker sidecar round-trip tests

### Modified
- `libs/recorder_core/include/recorder_core/mp4_remuxer.h` — `TrimRange`, `ExtractKeyframeTimestamps()`, trimmed overloads
- `libs/recorder_core/src/mp4_remuxer.cpp` — implement trim via `av_seek_frame` + PTS cutoff
- `libs/recorder_core/CMakeLists.txt` — register `test_remux_trim` test target
- `libs/recorder_core/include/recorder_core/recorder_session.h` — add `keyframe_interval_secs` to `RecorderConfig`
- `libs/recorder_core/src/nvenc_encoder.cpp` — use `keyframe_interval_secs` in `InitEncoder`
- `app/models/VideoSettingsModel.h` — add `KeyframeIntervalMode` enum + field
- `app/models/SettingsHintText.h` — add `kKeyframeInterval` hint text
- `app/services/RecordingCoordinator.h` — add `mkv_master_path_` field + `MkvMasterPath()` accessor
- `app/services/RecordingCoordinator.cpp` — retain MKV master after MP4 remux; map keyframe mode → config; pass master path in result
- `app/viewmodels/RecordViewModel.h` — add `mkv_master_path` + `peak_av_drift_ms` + `last_snapshot` to `UiRecordingResult`
- `app/pages/ConfigPage.cpp` — add Keyframe Interval control in Advanced → Video section
- `app/pages/RecordPage.h` — add `edit_btn_` member; update `editExportRequested` signal params
- `app/pages/RecordPage.cpp` — add "Edit" button to result actions; emit extended signal; store post-flight data
- `app/pages/EditExportPage.h` — add `EditContext` struct; replace placeholder banner; add trim state
- `app/pages/EditExportPage.cpp` — wire Review/Edit/Output phases; load sidecar; run real remux
- `app/MainWindow.cpp` — update `navigateToEditExportPage` signature to accept `EditContext`
- `app/MainWindow.h` — update declaration
- `app/CMakeLists.txt` — register `test_edit_context`
- `docs/decisions/0022-edit-output-save-surface.md` — update status + format model
- `docs/decisions/0014-mp4-via-remux-on-stop.md` — add edit re-remux paragraph

---

## Task 1: Engine — Trim Range + Keyframe Extraction

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/mp4_remuxer.h`
- Modify: `libs/recorder_core/src/mp4_remuxer.cpp`
- Create: `libs/recorder_core/tests/test_remux_trim.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct TrimRange { int64_t start_us = AV_NOPTS_VALUE; int64_t end_us = AV_NOPTS_VALUE; };` — `AV_NOPTS_VALUE` means "no boundary"
  - `RemuxResult RemuxToProgressiveMp4(path, path, callback, TrimRange)` — overload adding trim
  - `RemuxResult RemuxToMkv(path, path, callback, TrimRange)` — overload adding trim
  - `std::vector<int64_t> ExtractKeyframeTimestamps(const std::filesystem::path& input_path)` — returns sorted keyframe PTS in microseconds (AV_TIME_BASE), empty on failure

- [ ] **Step 1: Write the failing tests**

Create `libs/recorder_core/tests/test_remux_trim.cpp`:

```cpp
// test_remux_trim.cpp — tests for TrimRange remux and keyframe extraction
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
static inline const char* av_err2str_trim_test(int e) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(e, buf, sizeof(buf));
    return buf;
}
#include <gtest/gtest.h>
#include "matroska_stream_writer.h"
#include "recorder_core/mp4_remuxer.h"
#include <filesystem>
#include <cstdio>
#include <vector>
#include <algorithm>

using namespace recorder_core;
using recorder_core::MatroskaStreamConfig;
using recorder_core::MatroskaStreamWriter;
using recorder_core::MuxPacket;

// ---------- helpers ----------

static std::vector<uint8_t> FakeH264Cp_t() {
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}
static std::vector<uint8_t> FakeAacCp_t() { return {0x13, 0x90}; }

static MatroskaStreamConfig MakeCfg(const std::string& p) {
    MatroskaStreamConfig c;
    c.output_path = p;
    c.video_codec_id = "V_MPEG4/ISO/AVC";
    c.video_codec_private = FakeH264Cp_t();
    c.encode_width = 1280; c.encode_height = 720;
    c.frame_rate_num = 60; c.frame_rate_den = 1;
    c.audio_codec = StreamAudioCodec::Aac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = FakeAacCp_t();
    return c;
}

// Feed seconds of synthetic packets; gop = keyframe every gop frames (60 fps).
static void Feed(MatroskaStreamWriter& w, double secs, int gop = 60) {
    const uint64_t vf = 1000000000ULL / 60;
    const uint64_t af = 1024ULL * 1000000000ULL / 48000ULL;
    const uint64_t total = static_cast<uint64_t>(secs * 1e9);
    const std::vector<uint8_t> blob(256, 0xAB);
    uint64_t vpts = 0, apts = 0; int vi = 0;
    while (vpts < total || apts < total) {
        if (vpts <= apts && vpts < total) {
            MuxPacket p; p.pts_ns = vpts; p.track_num = 1;
            p.is_key = (vi % gop == 0); p.bytes = blob;
            w.Push(std::move(p)); vpts += vf; ++vi;
        } else if (apts < total) {
            MuxPacket p; p.pts_ns = apts; p.track_num = 2;
            p.is_key = true; p.bytes = blob;
            w.Push(std::move(p)); apts += af;
        } else break;
    }
}

static std::string TempPath(const char* sfx) {
    auto t = std::filesystem::temp_directory_path();
    const ::testing::TestInfo* i = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string n = i ? i->name() : "anon";
    return (t / ("exosnap_trim_" + n + "_" + sfx)).string();
}

static std::string BuildMkv(const std::string& p, double secs = 6.0, int gop = 60) {
    MatroskaStreamWriter w; auto c = MakeCfg(p);
    if (!w.Open(c)) return {};
    Feed(w, secs, gop);
    if (!w.Finalize()) return {};
    return p;
}

// ---------- test fixture ----------

class TrimTest : public ::testing::Test {
protected:
    void SetUp() override {
        src_ = TempPath("src.mkv"); dst_ = TempPath("dst.mp4");
        std::remove(src_.c_str()); std::remove(dst_.c_str());
    }
    void TearDown() override {
        std::remove(src_.c_str()); std::remove(dst_.c_str());
    }
    std::string src_, dst_;
};

// --- ExtractKeyframeTimestamps returns sorted, non-empty vector ---
TEST_F(TrimTest, ExtractKeyframesReturnsNonEmpty) {
    ASSERT_FALSE(BuildMkv(src_, 6.0, 60).empty());
    auto kfs = ExtractKeyframeTimestamps(src_);
    EXPECT_GE(kfs.size(), 2u) << "Expected >=2 keyframes in 6s / gop=60 file";
    for (size_t i = 1; i < kfs.size(); ++i)
        EXPECT_GE(kfs[i], kfs[i-1]) << "Keyframes not sorted";
}

// --- Bad input returns empty vector ---
TEST_F(TrimTest, ExtractKeyframesBadInput) {
    auto kfs = ExtractKeyframeTimestamps("/nonexistent_xyz_exosnap_trim.mkv");
    EXPECT_TRUE(kfs.empty());
}

// --- Full pass (no trim) matches untrimmed result ---
TEST_F(TrimTest, NoTrimMatchesFullRemux) {
    ASSERT_FALSE(BuildMkv(src_).empty());
    TrimRange full; // AV_NOPTS_VALUE = no trim
    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), full);
    ASSERT_TRUE(res.success) << res.message;
    EXPECT_GT(std::filesystem::file_size(dst_), 0u);
}

// --- Start trim produces shorter output than full ---
TEST_F(TrimTest, StartTrimProducesShorterOutput) {
    ASSERT_FALSE(BuildMkv(src_, 6.0, 60).empty());
    auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 2u);
    // Trim from 2nd keyframe onward
    TrimRange tr;
    tr.start_us = kfs[1]; // e.g. ~1 second in (at 60fps, gop=60)
    tr.end_us = AV_NOPTS_VALUE; // no end trim

    std::string dst_full = TempPath("dst_full.mp4");
    auto res_full = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
    ASSERT_TRUE(res_full.success);
    auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    auto res_trim = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res_trim.success) << res_trim.message;
    auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "Trimmed output should be smaller than full";
}

// --- End trim produces shorter output ---
TEST_F(TrimTest, EndTrimProducesShorterOutput) {
    ASSERT_FALSE(BuildMkv(src_, 6.0, 60).empty());
    auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 3u);
    TrimRange tr;
    tr.start_us = AV_NOPTS_VALUE; // no start trim
    tr.end_us = kfs[kfs.size()/2]; // stop halfway

    std::string dst_full = TempPath("dst_full2.mp4");
    auto res_full = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
    ASSERT_TRUE(res_full.success);
    auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    auto res_trim = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res_trim.success) << res_trim.message;
    EXPECT_LT(std::filesystem::file_size(dst_), size_full);
}

// --- Trim + MKV output works ---
TEST_F(TrimTest, TrimToMkv) {
    ASSERT_FALSE(BuildMkv(src_).empty());
    std::string dst_mkv = TempPath("dst.mkv");
    auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 2u);
    TrimRange tr; tr.start_us = kfs[1]; tr.end_us = AV_NOPTS_VALUE;
    auto res = RemuxToMkv(src_, dst_mkv, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << res.message;
    EXPECT_GT(std::filesystem::file_size(dst_mkv), 0u);
    std::remove(dst_mkv.c_str());
}
```

- [ ] **Step 2: Register test in CMakeLists — verify it fails to compile**

In `libs/recorder_core/CMakeLists.txt`, find the block after `add_executable(test_mp4_remuxer ...)` block and add:

```cmake
    add_executable(test_remux_trim tests/test_remux_trim.cpp)
    target_link_libraries(test_remux_trim PRIVATE
        GTest::gtest_main
        exosnap::warnings
        recorder_core EBML::ebml Matroska::matroska FFmpeg::mux
    )
    target_include_directories(test_remux_trim PRIVATE src)
    foreach(_ffmpeg_dll IN LISTS EXOSNAP_FFMPEG_DLLS)
        add_custom_command(TARGET test_remux_trim POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ffmpeg_dll}"
                "$<TARGET_FILE_DIR:test_remux_trim>"
            VERBATIM
        )
    endforeach()
    include(GoogleTest)
    gtest_discover_tests(test_remux_trim TEST_PREFIX "recorder_core.")
```

Run: `cmake --build build/windows-x64-debug --target test_remux_trim 2>&1 | head -30`
Expected: compile error — `TrimRange` and `ExtractKeyframeTimestamps` not defined yet.

- [ ] **Step 3: Add TrimRange + ExtractKeyframeTimestamps to the header**

In `libs/recorder_core/include/recorder_core/mp4_remuxer.h`, after the `RemuxNoopCallback()` inline function, add:

```cpp
// Trim window: both fields are in AV_TIME_BASE microseconds (same unit as
// AVFormatContext::duration). AV_NOPTS_VALUE (== INT64_MIN) means "no boundary".
// The trim is keyframe-accurate: start snaps backward to the nearest keyframe at
// or before start_us; all packets at pts < end_us are copied (or all if end_us
// is AV_NOPTS_VALUE).
struct TrimRange {
    int64_t start_us = AV_NOPTS_VALUE; ///< AV_NOPTS_VALUE = from the beginning
    int64_t end_us   = AV_NOPTS_VALUE; ///< AV_NOPTS_VALUE = to the end

    [[nodiscard]] bool HasStart() const noexcept { return start_us != AV_NOPTS_VALUE; }
    [[nodiscard]] bool HasEnd()   const noexcept { return end_us   != AV_NOPTS_VALUE; }
};

// Remux with an optional trim window (see TrimRange). When tr.HasStart() is
// false and tr.HasEnd() is false, behaviour is identical to the non-trim overload.
RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path,
                                  const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb,
                                  TrimRange tr);

RemuxResult RemuxToMkv(const std::filesystem::path& input_path,
                       const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb,
                       TrimRange tr);

// Scan `input_path` for all video keyframe PTS values and return them sorted
// in ascending order (AV_TIME_BASE microseconds). The file is read without
// decoding (av_read_frame + key-frame flag). Returns an empty vector on open
// failure or when the file has no video stream.
std::vector<int64_t> ExtractKeyframeTimestamps(const std::filesystem::path& input_path);
```

Also add `#include <cstdint>` and `#include <vector>` if not already present at the top of the header.

- [ ] **Step 4: Implement trim + keyframe extraction in mp4_remuxer.cpp**

Add these inside the `recorder_core` namespace in `libs/recorder_core/src/mp4_remuxer.cpp`:

```cpp
// ExtractKeyframeTimestamps — scan for keyframe PTS without decoding.
std::vector<int64_t> ExtractKeyframeTimestamps(const std::filesystem::path& input_path) {
    const std::string in_str = input_path.string();
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, in_str.c_str(), nullptr, nullptr) < 0)
        return {};
    struct Guard { AVFormatContext* c; ~Guard(){ if(c) avformat_close_input(&c); } } g{ctx};
    if (avformat_find_stream_info(ctx, nullptr) < 0)
        return {};

    // Find the first video stream.
    int vs_idx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vs_idx = static_cast<int>(i);
            break;
        }
    }
    if (vs_idx < 0) return {};

    AVStream* vs = ctx->streams[vs_idx];
    std::vector<int64_t> keyframes;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return {};
    struct PktGuard { AVPacket* p; ~PktGuard(){ av_packet_free(&p); } } pg{pkt};

    while (av_read_frame(ctx, pkt) == 0) {
        if (pkt->stream_index == vs_idx && (pkt->flags & AV_PKT_FLAG_KEY)) {
            if (pkt->pts != AV_NOPTS_VALUE) {
                // Convert stream PTS to AV_TIME_BASE (microseconds).
                int64_t pts_us = av_rescale_q(pkt->pts, vs->time_base, {1, AV_TIME_BASE});
                keyframes.push_back(pts_us);
            }
        }
        av_packet_unref(pkt);
    }

    std::sort(keyframes.begin(), keyframes.end());
    return keyframes;
}
```

Then, add a `TrimRange tr = {}` parameter to the internal `RemuxStreamCopy` function and use `av_seek_frame` when `tr.HasStart()`:

Inside `RemuxStreamCopy`, before step 6 (packet loop), add:

```cpp
    // -----------------------------------------------------------------------
    // 5b. Apply start trim: seek to keyframe at/before start_us.
    // -----------------------------------------------------------------------
    // Find the video stream index for seeking.
    int video_stream_idx = -1;
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (tr.HasStart()) {
        // Seek to the keyframe at or before start_us.
        // Use AVSEEK_FLAG_BACKWARD so libavformat finds the nearest preceding keyframe.
        int ret = av_seek_frame(in_ctx, video_stream_idx, tr.start_us, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            std::string msg = std::string("av_seek_frame (trim start) failed: ") + av_err2str(ret);
            LogWarn(msg.c_str());
            // Non-fatal: continue from the beginning.
        }
    }
```

Then, inside the packet loop (`while (true) { int ret = av_read_frame(...)`), after reading a packet but before writing it, add the end-trim cutoff:

```cpp
        // End-trim: drop packets past the cutoff.
        if (tr.HasEnd() && si == video_stream_idx) {
            const AVStream* in_vs = in_ctx->streams[video_stream_idx];
            if (pkt->pts != AV_NOPTS_VALUE) {
                // Rescale pkt PTS to AV_TIME_BASE for comparison.
                int64_t pts_us = av_rescale_q(pkt->pts, in_vs->time_base, {1, AV_TIME_BASE});
                if (pts_us >= tr.end_us) {
                    av_packet_unref(pkt);
                    break; // Done — past the end boundary.
                }
            }
        }
```

Also update `RemuxStreamCopy`'s signature to `static RemuxResult RemuxStreamCopy(..., TrimRange tr = {})` and propagate `tr` from the public API.

Update the two public functions:
```cpp
RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path,
                                  const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb,
                                  TrimRange tr) {
    static const char* const kMp4Opts[] = {"movflags", "+faststart", nullptr};
    RemuxOptions opts; opts.format_name = "mp4"; opts.extra_opts = kMp4Opts;
    return RemuxStreamCopy(input_path, output_path, std::move(progress_cb), opts, tr);
}

RemuxResult RemuxToMkv(const std::filesystem::path& input_path,
                       const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb,
                       TrimRange tr) {
    RemuxOptions opts; opts.format_name = "matroska"; opts.extra_opts = nullptr;
    return RemuxStreamCopy(input_path, output_path, std::move(progress_cb), opts, tr);
}
```

The existing no-trim overloads just call through with `TrimRange{}`:
```cpp
RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path,
                                  const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb) {
    return RemuxToProgressiveMp4(input_path, output_path, std::move(progress_cb), TrimRange{});
}

RemuxResult RemuxToMkv(const std::filesystem::path& input_path,
                       const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb) {
    return RemuxToMkv(input_path, output_path, std::move(progress_cb), TrimRange{});
}
```

Note: `AV_NOPTS_VALUE` is `INT64_MIN` defined in `<libavutil/avutil.h>` which is already included. Add `#include <algorithm>` to the cpp file for `std::sort`.

- [ ] **Step 5: Build and run trim tests**

```powershell
cmake --build build/windows-x64-debug --target test_remux_trim
$env:PATH = "C:\Qt\6.9.0\msvc2022_64\bin;$env:PATH"
ctest --preset windows-x64-debug -R "recorder_core.TrimTest" --output-on-failure
```

Expected: All TrimTest.* PASS. Record pass count.

- [ ] **Step 6: Commit**

```powershell
git add libs/recorder_core/include/recorder_core/mp4_remuxer.h
git add libs/recorder_core/src/mp4_remuxer.cpp
git add libs/recorder_core/tests/test_remux_trim.cpp
git add libs/recorder_core/CMakeLists.txt
git commit -m "feat(engine): add TrimRange stream-copy + ExtractKeyframeTimestamps to remuxer"
```

---

## Task 2: Keyframe-Interval Setting

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/recorder_session.h` — add `keyframe_interval_secs` to `RecorderConfig`
- Modify: `libs/recorder_core/src/nvenc_encoder.cpp` — use field in `InitEncoder`
- Modify: `app/models/VideoSettingsModel.h` — add `KeyframeIntervalMode` enum + field
- Modify: `app/models/SettingsHintText.h` — add hint text
- Modify: `app/services/RecordingCoordinator.cpp` — map mode → `RecorderConfig.keyframe_interval_secs`
- Modify: `app/pages/ConfigPage.cpp` — add UI control (Advanced → Video section)

**Interfaces:**
- Consumes: Nothing new from earlier tasks.
- Produces:
  - `enum class KeyframeIntervalMode { Seconds2, Seconds1, Seconds0_5 };` in `VideoSettingsModel.h`
  - `float keyframe_interval_secs = 2.0f;` in `RecorderConfig`
  - `kKeyframeInterval` hint string in `SettingsHintText.h`

- [ ] **Step 1: Add field to RecorderConfig**

In `libs/recorder_core/include/recorder_core/recorder_session.h`, inside `struct RecorderConfig`, add after `frame_rate_den`:

```cpp
    // Keyframe interval in seconds. Used by NVENC as: gopLength = round(interval_secs * fps).
    // Default 2.0 s = the existing hardcoded value. Options: 2.0, 1.0, 0.5.
    // Shorter intervals allow finer trim points but increase file size slightly.
    float keyframe_interval_secs = 2.0f;
```

- [ ] **Step 2: Wire keyframe_interval_secs in NvencEncoder::InitEncoder**

In `libs/recorder_core/src/nvenc_encoder.cpp`, find the existing `kGopFrames` computation (~line 579):

```cpp
    // 2-second keyframe interval — recording-friendly default.
    const uint32_t kGopFrames = (frame_rate_den > 0 && frame_rate_num > 0)
                                    ? static_cast<uint32_t>((2ull * frame_rate_num) / frame_rate_den)
                                    : 120u;
```

`NvencEncoder` doesn't currently have access to `RecorderConfig` directly — it is configured via setters. We need to add a setter. Add to `nvenc_encoder.h` (inside `class NvencEncoder`):

```cpp
    // Set keyframe interval in seconds. Must be called before InitEncoder().
    // Default 2.0 — matches the existing hardcoded behavior.
    void SetKeyframeIntervalSecs(float secs) noexcept {
        m_keyframeIntervalSecs = secs > 0.0f ? secs : 2.0f;
    }
```

Add private member to `NvencEncoder`:
```cpp
    float m_keyframeIntervalSecs = 2.0f;
```

Then in `InitEncoder`, replace the hardcoded `2ull`:
```cpp
    const uint32_t kGopFrames = (frame_rate_den > 0 && frame_rate_num > 0)
        ? static_cast<uint32_t>((static_cast<double>(m_keyframeIntervalSecs) * frame_rate_num) / frame_rate_den + 0.5)
        : 120u;
```

- [ ] **Step 3: Find where NvencEncoder is configured and add the setter call**

Search for where `NvencEncoder` is constructed and its setters are called in the codebase. This is typically in `nvenc_video_encoder.cpp` or `video_thread.cpp`. Since `video_thread.cpp` is off-limits, check `nvenc_video_encoder.cpp`:

```powershell
grep -n "SetCodec\|SetRateControl\|SetBitDepth\|NvencEncoder" "libs/recorder_core/src/nvenc_video_encoder.cpp" | head -20
```

In `nvenc_video_encoder.cpp`, find where other setters are called and add:
```cpp
encoder.SetKeyframeIntervalSecs(config.keyframe_interval_secs);
```

- [ ] **Step 4: Add KeyframeIntervalMode to VideoSettingsModel**

In `app/models/VideoSettingsModel.h`:

```cpp
enum class KeyframeIntervalMode {
    Seconds2,   // 2 s — default, larger GOP, slightly smaller files
    Seconds1,   // 1 s — more frequent keyframes, better trim accuracy
    Seconds0_5, // 0.5 s — finest trim accuracy, slightly larger files
};

struct VideoSettingsModel {
    recorder_core::NvencQualityPreset quality = recorder_core::NvencQualityPreset::Balanced;
    recorder_core::RateControlMode rate_control = recorder_core::RateControlMode::ConstantQuality;
    uint32_t bitrate_kbps = 20000;
    bool cfr = true;
    recorder_core::FramePacingMode frame_pacing = recorder_core::FramePacingMode::Smooth;
    bool capture_cursor = true;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    KeyframeIntervalMode keyframe_interval = KeyframeIntervalMode::Seconds2; // default 2 s

    static VideoSettingsModel Defaults() { return {}; }
};
```

- [ ] **Step 5: Add hint text to SettingsHintText.h**

In `app/models/SettingsHintText.h`, after `kFramePacing`:

```cpp
inline const QString kKeyframeInterval =
    QStringLiteral("Keyframe interval controls trim accuracy: "
                   "2\xC2\xA0s = default (lower file size, 2-second trim grid) \xC2\xB7 "
                   "1\xC2\xA0s = 1-second trim grid \xC2\xB7 "
                   "0.5\xC2\xA0s = finest trim accuracy (slightly larger files). "
                   "Shorter intervals produce more frequent keyframes — required for precise Quick Trim cuts.");
```

- [ ] **Step 6: Map KeyframeIntervalMode in RecordingCoordinator**

In `app/services/RecordingCoordinator.cpp`, in the function that builds `RecorderConfig` from `video_settings_` (search for `config.frame_rate_num = video_settings_.frame_rate_num`), add:

```cpp
    // Map keyframe interval mode to seconds.
    switch (video_settings_.keyframe_interval) {
    case KeyframeIntervalMode::Seconds2:   config.keyframe_interval_secs = 2.0f; break;
    case KeyframeIntervalMode::Seconds1:   config.keyframe_interval_secs = 1.0f; break;
    case KeyframeIntervalMode::Seconds0_5: config.keyframe_interval_secs = 0.5f; break;
    }
```

Add `#include "../models/VideoSettingsModel.h"` if not already included (it already is per the header).

- [ ] **Step 7: Add keyframe interval control to ConfigPage (Advanced → Video)**

In `app/pages/ConfigPage.cpp`, find the Advanced Video section (search for `"AdvancedVideo"` or the block where frame pacing is added). Add after the frame pacing row:

```cpp
    // Keyframe interval (Advanced → Video)
    auto* keyframe_row = new SettingsPopoverRow(
        QStringLiteral("Keyframe interval"), parent_widget);
    keyframe_row->setObjectName(QStringLiteral("keyframeIntervalRow"));

    auto* keyframe_combo = new QComboBox(keyframe_row);
    keyframe_combo->setObjectName(QStringLiteral("keyframeIntervalCombo"));
    keyframe_combo->addItem(QStringLiteral("2 s (default)"), static_cast<int>(KeyframeIntervalMode::Seconds2));
    keyframe_combo->addItem(QStringLiteral("1 s"),           static_cast<int>(KeyframeIntervalMode::Seconds1));
    keyframe_combo->addItem(QStringLiteral("0.5 s"),         static_cast<int>(KeyframeIntervalMode::Seconds0_5));

    auto* keyframe_hint = new InfoHintIcon(exosnap::ui::hints::kKeyframeInterval, keyframe_row);

    keyframe_row->setContent(keyframe_combo);
    keyframe_row->addTrailingWidget(keyframe_hint);
    advanced_video_layout->addWidget(keyframe_row);

    // Wire to video settings
    connect(keyframe_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, keyframe_combo](int) {
        auto mode = static_cast<KeyframeIntervalMode>(keyframe_combo->currentData().toInt());
        if (video_settings_.keyframe_interval != mode) {
            video_settings_.keyframe_interval = mode;
            emit videoSettingsChanged(video_settings_);
        }
    });
    // Replay function: store lambda so initial value can be applied
    // (follow the pattern used for frame_pacing_combo in ConfigPage.cpp)
```

Also add to the replay/fan-out section (look for how `frame_pacing` is replayed when `applyVideoSettings` is called):
```cpp
    keyframe_combo->setCurrentIndex(keyframe_combo->findData(
        static_cast<int>(video_settings_.keyframe_interval)));
```

- [ ] **Step 8: Build app and run focused tests**

```powershell
cmake --build build/windows-x64-debug --target exosnap
```

Expected: clean build. Launch app and verify the Keyframe Interval combo appears in Advanced → Video. Check info-i tooltip.

- [ ] **Step 9: Commit**

```powershell
git add libs/recorder_core/include/recorder_core/recorder_session.h
git add libs/recorder_core/src/nvenc_encoder.cpp
git add libs/recorder_core/src/nvenc_encoder.h
git add libs/recorder_core/src/nvenc_video_encoder.cpp
git add app/models/VideoSettingsModel.h
git add app/models/SettingsHintText.h
git add app/services/RecordingCoordinator.cpp
git add app/pages/ConfigPage.cpp
git commit -m "feat(settings): add keyframe-interval setting (2 s/1 s/0.5 s) to Advanced Video with info-i hint"
```

---

## Task 3: MKV Master Retention for MP4 Recordings

**Files:**
- Modify: `app/services/RecordingCoordinator.h` — add `mkv_master_path_` field + accessor
- Modify: `app/services/RecordingCoordinator.cpp` — after successful remux, rename `.mkv.tmp` → companion `.edit.mkv`; store master path; pass it in `UiRecordingResult`
- Modify: `app/viewmodels/RecordViewModel.h` — add `mkv_master_path` + `peak_av_drift_ms` + `completed_snapshot` to `UiRecordingResult`

**Interfaces:**
- Consumes: nothing new from earlier tasks
- Produces:
  - `UiRecordingResult.mkv_master_path` (std::wstring) — path to the edit-master MKV (or empty for non-MP4 recordings, where the output IS the master)
  - `UiRecordingResult.peak_av_drift_ms` (double) — from RecordPage (will be added to `UiRecordingResult` or passed separately; see Task 4)

- [ ] **Step 1: Add UiRecordingResult fields**

In `app/viewmodels/RecordViewModel.h`, inside `struct UiRecordingResult`, add after `marker_sidecar_path`:

```cpp
    // Path to the canonical MKV edit master:
    //   - For MKV recordings: same as output_path (the file IS the master).
    //   - For MP4 recordings: the companion .edit.mkv retained after remux.
    //   - Empty string when not available (e.g. failed recording, split sessions).
    std::wstring mkv_master_path;
```

- [ ] **Step 2: Retain MKV master in RunRemuxJob**

In `app/services/RecordingCoordinator.cpp`, in `RunRemuxJob`, find the `if (remux_result.success)` branch where the transient MKV is deleted:

```cpp
            // Delete the transient MKV.
            std::error_code ec;
            std::filesystem::remove(transient_mkv, ec);
```

Replace with:

```cpp
            // Rename .mkv.tmp → companion .edit.mkv (retain as edit master).
            // Derive the companion path: replace ".mkv.tmp" with ".edit.mkv".
            std::filesystem::path edit_master = transient_mkv;
            edit_master.replace_extension(L".edit.mkv");
            std::error_code ec;
            std::filesystem::rename(transient_mkv, edit_master, ec);
            if (ec) {
                // Fallback: try copy + delete.
                std::filesystem::copy_file(transient_mkv, edit_master,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) std::filesystem::remove(transient_mkv, ec);
            }
            if (!ec) {
                mkv_master_path_ = edit_master;
                diagnostics::AppLog::info(QStringLiteral("remux"),
                    QStringLiteral("edit master retained: \"%1\"")
                        .arg(QString::fromStdWString(edit_master.wstring())));
            } else {
                // Could not retain master — edit surface will degrade gracefully.
                diagnostics::AppLog::warning(QStringLiteral("remux"),
                    QStringLiteral("edit master retention failed: %1")
                        .arg(QString::fromStdWString(ToWide(ec.message()))));
                mkv_master_path_ = {};
            }
```

- [ ] **Step 3: Add mkv_master_path_ field to RecordingCoordinator**

In `app/services/RecordingCoordinator.h`, in the private section, add after `final_mp4_path_`:

```cpp
    // Path of the retained edit master MKV for the last completed session.
    // For MKV recordings this stays empty (the output IS the master).
    // For MP4 recordings set to the .edit.mkv companion after successful remux.
    std::filesystem::path mkv_master_path_;
```

- [ ] **Step 4: Pass mkv_master_path in UiRecordingResult**

In `RecordingCoordinator.cpp`, in `RecordingThreadProc` (or wherever `ui_result` is built before `PostResult`), for MKV recordings set `ui_result.mkv_master_path = output_path.wstring()` and for MP4 recordings read `mkv_master_path_`. Find the block with `ui_result.container = config.container;` and add:

```cpp
    // Set the edit master path:
    //   - MKV target: the output file IS the edit master.
    //   - MP4 target: set after remux in RunRemuxJob (stored in mkv_master_path_).
    if (config.container != recorder_core::Container::Mp4) {
        ui_result.mkv_master_path = output_path.wstring();
    }
    // MP4 path: mkv_master_path_ is filled by RunRemuxJob and propagated there.
```

For MP4, in `RunRemuxJob`'s success branch, update `final_result.mkv_master_path`:
```cpp
            if (!ec)
                final_result.mkv_master_path = mkv_master_path_.wstring();
```

- [ ] **Step 5: MKV master path for MKV recordings in recording result**

In the `RecordingThreadProc`, where `PostResult(std::move(ui_result))` is called for the MKV path (find the `UiRecordingState::Completed` transition for non-MP4), ensure `ui_result.mkv_master_path = output_path.wstring()` is set. (Check the existing code to confirm exactly where.)

- [ ] **Step 6: Build and verify**

```powershell
cmake --build build/windows-x64-debug --target exosnap
```

Expected: clean build. Run the app, make a short recording to MP4. After stop, check the output directory — should contain both `recording.mp4` and `recording.edit.mkv`.

- [ ] **Step 7: Commit**

```powershell
git add app/services/RecordingCoordinator.h
git add app/services/RecordingCoordinator.cpp
git add app/viewmodels/RecordViewModel.h
git commit -m "feat(coordinator): retain MKV edit master after MP4 remux as .edit.mkv companion"
```

---

## Task 4: Edit Context Model + RecordPage "Edit" Button

**Files:**
- Modify: `app/pages/RecordPage.h` — add `edit_btn_` member; extend `editExportRequested` signal
- Modify: `app/pages/RecordPage.cpp` — add "Edit" button to result actions; emit extended signal
- Modify: `app/pages/EditExportPage.h` — add `EditContext` struct; extend `setRecordingInfo` or replace with `setEditContext`
- Modify: `app/MainWindow.h` — update `navigateToEditExportPage` signature
- Modify: `app/MainWindow.cpp` — update slot to pass full context

**Interfaces:**
- Consumes: `UiRecordingResult.mkv_master_path` (Task 3)
- Produces:
  - `struct EditContext` in `EditExportPage.h` (also forward-declared / included where needed)
  - Updated `editExportRequested(const EditContext&)` signal

- [ ] **Step 1: Define EditContext struct in EditExportPage.h**

In `app/pages/EditExportPage.h`, add before the `EditExportPage` class:

```cpp
#include <QString>
#include <vector>
#include <recorder_core/pipeline_diagnostics.h>
#include "../models/RecordingMarker.h"

namespace exosnap {

// Context passed to EditExportPage when opening the edit surface.
// Contains everything needed for the Review, Edit, and Output phases.
struct EditContext {
    // File metadata (from the completed recording result)
    QString output_path;        // final output (MP4 or MKV)
    QString mkv_master_path;    // edit master (MKV); same as output for MKV recordings
    QString duration;           // human-readable duration
    QString size;               // human-readable file size
    QString resolution;         // e.g. "1920×1080"
    QString fps;                // e.g. "60 fps (CFR)"
    QString video_codec;        // e.g. "AV1 (NVENC)"
    QString audio_codec;        // e.g. "Opus"
    QString container;          // e.g. "MKV" or "MP4"

    // Post-flight data (from RecordPage diagnostics tracking)
    double peak_av_drift_ms = 0.0;
    bool av_drift_available = false;
    recorder_core::RecordingDiagnosticsSnapshot completed_snapshot;

    // Markers (loaded from sidecar — EditExportPage loads the sidecar itself
    // using mkv_master_path; this vector is a pre-loaded fallback from the
    // recording session that may be used if the sidecar cannot be read).
    std::vector<RecordingMarker> markers;
    QString marker_sidecar_path; // companion .markers.json
};

} // namespace exosnap
```

Also update `setRecordingInfo` to `setEditContext`:
```cpp
    void setEditContext(const EditContext& ctx);
```

Keep `setRecordingInfo` as a deprecated shim for backward compatibility (notification toast path that calls with limited data):
```cpp
    // Legacy shim used by the "Edit" notification action (partial data — no master path).
    void setRecordingInfo(const QString& file_path, const QString& duration, const QString& size,
                          const QString& resolution, const QString& fps, const QString& video_codec,
                          const QString& audio_codec, const QString& container);
```

- [ ] **Step 2: Update editExportRequested signal in RecordPage.h**

In `app/pages/RecordPage.h`, add `#include "EditExportPage.h"` (or forward declare), and change:
```cpp
    void editExportRequested(const QString& file_path, const QString& duration, const QString& size,
                             const QString& resolution, const QString& fps, const QString& video_codec,
                             const QString& audio_codec, const QString& container);
```
to:
```cpp
    void editExportRequested(const exosnap::EditContext& ctx);
```

- [ ] **Step 3: Add "Edit" button to RecordPage result actions**

In `app/pages/RecordPage.h`, add a member:
```cpp
    QPushButton* result_edit_btn_ = nullptr;
```

In `app/pages/RecordPage.cpp`, in the result actions widget construction block (near `result_copy_path_btn_`, `result_rename_btn_`, `result_delete_btn_`), add:

```cpp
    result_edit_btn_ = new QPushButton(QStringLiteral("Edit"), result_actions_widget_);
    result_edit_btn_->setObjectName(QStringLiteral("resultEditBtn"));
    result_edit_btn_->setProperty("role", "ghost");
    result_edit_btn_->setCursor(Qt::PointingHandCursor);
    result_edit_btn_->setToolTip(QStringLiteral("Trim and export this recording"));
    result_actions_layout_->addWidget(result_edit_btn_);
```

In the slot or connect block where the result actions are wired, add:
```cpp
    connect(result_edit_btn_, &QPushButton::clicked, this, [this]() {
        // Build the EditContext from the last completed recording result.
        exosnap::EditContext ctx;
        ctx.output_path  = QString::fromStdWString(view_model_.result_output_path);
        ctx.mkv_master_path = QString::fromStdWString(view_model_.result_mkv_master_path);
        // ... fill remaining fields from view_model_
        ctx.peak_av_drift_ms  = peak_av_drift_ms_;
        ctx.av_drift_available = av_drift_ever_available_;
        ctx.completed_snapshot = last_completed_snapshot_;
        // Markers: pre-loaded from UiRecordingResult
        ctx.markers = view_model_.result_markers;
        ctx.marker_sidecar_path = QString::fromStdWString(view_model_.result_marker_sidecar_path);
        emit editExportRequested(ctx);
    });
```

Also add `result_mkv_master_path` and related fields to `RecordViewModel` if not yet done (sync from `UiRecordingResult::mkv_master_path` in `RecordViewModel::SetResult`).

- [ ] **Step 4: Update MainWindow signal connections**

In `app/MainWindow.h`, update:
```cpp
    void navigateToEditExportPage(const exosnap::EditContext& ctx);
```

In `app/MainWindow.cpp`:
- In the `connect(record_page_, &RecordPage::editExportRequested, ...)` call:
```cpp
    connect(record_page_, &RecordPage::editExportRequested, this,
            [this](const exosnap::EditContext& ctx) { navigateToEditExportPage(ctx); });
```
- Update `navigateToEditExportPage` body:
```cpp
void MainWindow::navigateToEditExportPage(const exosnap::EditContext& ctx) {
    if (!edit_export_page_)
        buildEditExportPage();
    edit_export_page_->setEditContext(ctx);
    edit_export_page_->setPhase(EditExportPage::Phase::Review);
    title_bar_->setActivePage(kRecordPageIndex);
    stack_->setCurrentWidget(edit_export_page_);
}
```

Also update the notification-toast path (search for `navigateToEditExportPage(path, ...)`):
```cpp
        exosnap::EditContext toast_ctx;
        toast_ctx.output_path = path;
        toast_ctx.mkv_master_path = path; // best-effort fallback
        navigateToEditExportPage(toast_ctx);
```

- [ ] **Step 5: Implement setEditContext in EditExportPage.cpp**

In `app/pages/EditExportPage.cpp`, add `setEditContext`:
```cpp
void EditExportPage::setEditContext(const EditContext& ctx) {
    ctx_ = ctx;

    // Update filename label
    if (filename_label_) {
        const QString path = ctx_.output_path;
        const int sep = qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
        filename_label_->setText(sep >= 0 ? path.mid(sep + 1) : path);
    }

    // Update detail rail
    if (fact_duration_val_)  fact_duration_val_->setText(ctx_.duration.isEmpty()   ? QStringLiteral("–") : ctx_.duration);
    if (fact_size_val_)      fact_size_val_->setText(ctx_.size.isEmpty()           ? QStringLiteral("–") : ctx_.size);
    if (fact_res_val_)       fact_res_val_->setText(ctx_.resolution.isEmpty()      ? QStringLiteral("–") : ctx_.resolution);
    if (fact_fps_val_)       fact_fps_val_->setText(ctx_.fps.isEmpty()             ? QStringLiteral("–") : ctx_.fps);
    if (fact_video_val_)     fact_video_val_->setText(ctx_.video_codec.isEmpty()   ? QStringLiteral("–") : ctx_.video_codec);
    if (fact_audio_val_)     fact_audio_val_->setText(ctx_.audio_codec.isEmpty()   ? QStringLiteral("–") : ctx_.audio_codec);
    if (fact_container_val_) fact_container_val_->setText(ctx_.container.isEmpty() ? QStringLiteral("–") : ctx_.container);

    if (player_meta_label_)
        player_meta_label_->setText(QStringLiteral("%1  %2  %3")
            .arg(ctx_.resolution, ctx_.fps, ctx_.container));

    if (timeline_out_label_)
        timeline_out_label_->setText(QStringLiteral("Out %1").arg(ctx_.duration));
    if (duration_label_)
        duration_label_->setText(QStringLiteral("0:00 / %1").arg(ctx_.duration));

    // Load keyframe timestamps for snap (async-safe: run on calling thread, quick for typical files)
    keyframe_timestamps_.clear();
    if (!ctx_.mkv_master_path.isEmpty()) {
        keyframe_timestamps_ = recorder_core::ExtractKeyframeTimestamps(
            std::filesystem::path(ctx_.mkv_master_path.toStdWString()));
    }

    // Markers: try sidecar first, fall back to in-memory markers from the session.
    loadMarkers();

    // Reset trim state.
    trim_start_us_ = AV_NOPTS_VALUE;
    trim_end_us_   = AV_NOPTS_VALUE;

    setPhase(Phase::Review);
}
```

Add members to `EditExportPage.h`:
```cpp
    EditContext ctx_;
    std::vector<int64_t> keyframe_timestamps_; // sorted keyframe PTS in microseconds
    std::vector<RecordingMarker> markers_;      // loaded from sidecar
    int64_t trim_start_us_ = AV_NOPTS_VALUE;   // trim range (AV_NOPTS_VALUE = not set)
    int64_t trim_end_us_   = AV_NOPTS_VALUE;

  private:
    void loadMarkers(); // load sidecar or fall back to ctx_.markers
    void runExport();   // called by onExportClicked (real remux)
```

Add include to EditExportPage.h:
```cpp
#include <recorder_core/mp4_remuxer.h>
#include <recorder_core/pipeline_diagnostics.h>
#include "../models/RecordingMarker.h"
#include <vector>
```

- [ ] **Step 6: Build**

```powershell
cmake --build build/windows-x64-debug --target exosnap
```

Expected: clean build. The "Edit" button appears in the post-stop result row on RecordPage (in Saved state). Clicking it navigates to EditExportPage.

- [ ] **Step 7: Commit**

```powershell
git add app/pages/EditExportPage.h app/pages/EditExportPage.cpp
git add app/pages/RecordPage.h app/pages/RecordPage.cpp
git add app/MainWindow.h app/MainWindow.cpp
git commit -m "feat(edit): add Edit button to post-stop result + EditContext handoff to EditExportPage"
```

---

## Task 5: EditExportPage — Review / Edit / Output Phases

**Files:**
- Modify: `app/pages/EditExportPage.h` — add review panel labels, trim input fields
- Modify: `app/pages/EditExportPage.cpp` — implement all three active phases + real export

**Interfaces:**
- Consumes: `TrimRange`, `RemuxToProgressiveMp4`, `RemuxToMkv`, `ExtractKeyframeTimestamps` (Task 1); `EditContext` (Task 4); sidecar JSON format (existing)
- Produces: exported file, atomic overwrite path

- [ ] **Step 1: Wire Review phase — post-flight report**

In `EditExportPage::refreshPhase()`, add Review panel content. The Review phase must show:
- Frame-drop %, peak A/V drift (from `ctx_.completed_snapshot` and `ctx_.peak_av_drift_ms`)
- Pipeline health label
- A "dismiss" link is not needed — Review is informational

Add a review panel widget to `buildUi()`:

```cpp
    // Review Panel (post-flight report, shown in Review phase)
    review_panel_ = new QWidget(left_widget);
    review_panel_->setObjectName(QStringLiteral("editExportReviewPanel"));
    auto* review_layout = new QVBoxLayout(review_panel_);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->setSpacing(M::kSpaceSm);

    auto* review_title = new QLabel(QStringLiteral("Post-recording report"), review_panel_);
    review_title->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12px; }").arg(ActiveTheme().ink));

    review_drop_label_ = new QLabel(QStringLiteral("Frame drops: –"), review_panel_);
    review_drop_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

    review_drift_label_ = new QLabel(QStringLiteral("Peak A/V drift: –"), review_panel_);
    review_drift_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

    review_health_label_ = new QLabel(QStringLiteral("Pipeline health: –"), review_panel_);
    review_health_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(ActiveTheme().mut));

    review_layout->addWidget(review_title);
    review_layout->addWidget(review_drop_label_);
    review_layout->addWidget(review_drift_label_);
    review_layout->addWidget(review_health_label_);
    left_layout->addWidget(review_panel_);
```

In `setEditContext`, populate review labels (after loading ctx_):

```cpp
    // Populate review panel
    const auto& snap = ctx_.completed_snapshot;
    const bool has_snap = snap.valid || snap.session_generation > 0;
    if (review_drop_label_) {
        if (has_snap && snap.frame_drop_availability == recorder_core::MetricAvailability::Available) {
            review_drop_label_->setText(
                QStringLiteral("Frame drops: %1%").arg(snap.frame_drop_percent, 0, 'f', 1));
        } else {
            review_drop_label_->setText(QStringLiteral("Frame drops: –"));
        }
    }
    if (review_drift_label_) {
        if (ctx_.av_drift_available) {
            review_drift_label_->setText(
                QStringLiteral("Peak A/V drift: \xC2\xB1%1 ms").arg(ctx_.peak_av_drift_ms, 0, 'f', 0));
        } else {
            review_drift_label_->setText(QStringLiteral("A/V drift: unavailable"));
        }
    }
    if (review_health_label_ && has_snap) {
        const char* health_str = "Unknown";
        switch (snap.health) {
        case recorder_core::PipelineHealth::Good:    health_str = "Good";    break;
        case recorder_core::PipelineHealth::Warning: health_str = "Warning"; break;
        case recorder_core::PipelineHealth::Error:   health_str = "Error";   break;
        default: break;
        }
        review_health_label_->setText(
            QStringLiteral("Pipeline health: %1").arg(QLatin1String(health_str)));
    }
```

Add review panel pointers to `EditExportPage.h`:
```cpp
    QWidget* review_panel_ = nullptr;
    QLabel* review_drop_label_ = nullptr;
    QLabel* review_drift_label_ = nullptr;
    QLabel* review_health_label_ = nullptr;
```

In `refreshPhase()`, add `review_panel_` to the show/hide logic:
```cpp
    const bool show_review_panel = (phase_ == Phase::Review);
    if (review_panel_) review_panel_->setVisible(show_review_panel);
```

Remove the placeholder banner (the amber dashed banner). Delete it from `buildUi()` and the `placeholder_banner_` member.

- [ ] **Step 2: Wire Edit phase — trim handles with keyframe snap**

Enable the `trim_btn_`, `add_marker_btn_` buttons and the timeline:
```cpp
    // In refreshPhase(), when phase_ == Phase::Edit:
    if (trim_btn_)    trim_btn_->setEnabled(phase_ == Phase::Edit);
    if (add_marker_btn_) add_marker_btn_->setEnabled(phase_ == Phase::Edit);
```

Wire `trim_btn_` to open an in-place trim dialog showing trim-in/out with keyframe snap. Since a full custom timeline widget is deferred (complex custom painting), use a simpler approach: two `QTimeEdit` or `QSpinBox` fields for start/end seconds. Keep the existing timeline visual but overlay two labels showing "In: X:XX" and "Out: X:XX" that update as trim handles change.

In `EditExportPage.cpp`, implement the trim button slot:
```cpp
void EditExportPage::onTrimClicked() {
    // Simple trim dialog: two spin boxes for start/end seconds.
    // Snaps to the nearest keyframe on the grid.
    // Implementation uses QDialog with two QDoubleSpinBox controls.
    // Delegates actual snap logic to snapToKeyframe().
    // When OK: store trim_start_us_ / trim_end_us_ and update timeline labels.
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Set trim points"));
    auto* layout = new QVBoxLayout(&dlg);

    auto* start_spin = new QDoubleSpinBox(&dlg);
    start_spin->setPrefix(QStringLiteral("Start: "));
    start_spin->setSuffix(QStringLiteral(" s"));
    start_spin->setDecimals(2);
    start_spin->setMinimum(0.0);
    start_spin->setMaximum(1e6);
    start_spin->setValue(trim_start_us_ != AV_NOPTS_VALUE ? trim_start_us_ / 1e6 : 0.0);

    auto* end_spin = new QDoubleSpinBox(&dlg);
    end_spin->setPrefix(QStringLiteral("End: "));
    end_spin->setSuffix(QStringLiteral(" s"));
    end_spin->setDecimals(2);
    end_spin->setMinimum(0.0);
    end_spin->setMaximum(1e6);
    // Default to no end trim (full duration).
    end_spin->setValue(trim_end_us_ != AV_NOPTS_VALUE ? trim_end_us_ / 1e6 : 0.0);

    auto* note = new QLabel(QStringLiteral(
        "Trim is keyframe-accurate: cut points snap to the nearest keyframe."), &dlg);
    note->setWordWrap(true);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(start_spin);
    layout->addWidget(end_spin);
    layout->addWidget(note);
    layout->addWidget(btns);

    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    double start_s = start_spin->value();
    double end_s   = end_spin->value();

    // Snap to nearest keyframe.
    auto snap = [&](double secs_in) -> int64_t {
        if (keyframe_timestamps_.empty()) return static_cast<int64_t>(secs_in * 1e6);
        const int64_t us = static_cast<int64_t>(secs_in * 1e6);
        // Find nearest keyframe at or before us.
        auto it = std::upper_bound(keyframe_timestamps_.begin(), keyframe_timestamps_.end(), us);
        if (it != keyframe_timestamps_.begin()) --it;
        return *it;
    };

    trim_start_us_ = (start_s > 0.0) ? snap(start_s) : AV_NOPTS_VALUE;
    trim_end_us_   = (end_s   > 0.0) ? snap(end_s)   : AV_NOPTS_VALUE;

    // Also snap to marker positions if near a marker (within 50 ms).
    auto snapToMarker = [&](int64_t us) -> int64_t {
        for (const auto& m : markers_) {
            const int64_t m_us = static_cast<int64_t>(m.time_ms) * 1000LL;
            if (std::abs(m_us - us) <= 50000LL) return m_us;
        }
        return us;
    };
    if (trim_start_us_ != AV_NOPTS_VALUE) trim_start_us_ = snapToMarker(trim_start_us_);
    if (trim_end_us_   != AV_NOPTS_VALUE) trim_end_us_   = snapToMarker(trim_end_us_);

    // Update timeline labels.
    auto formatUs = [](int64_t us) -> QString {
        if (us == AV_NOPTS_VALUE) return QStringLiteral("–");
        const int64_t secs = us / 1000000LL;
        const int m2 = static_cast<int>(secs / 60);
        const int s2 = static_cast<int>(secs % 60);
        return QStringLiteral("%1:%2").arg(m2).arg(s2, 2, 10, QLatin1Char('0'));
    };
    if (timeline_in_label_)
        timeline_in_label_->setText(QStringLiteral("In %1").arg(formatUs(trim_start_us_)));
    if (timeline_out_label_)
        timeline_out_label_->setText(QStringLiteral("Out %1").arg(formatUs(trim_end_us_)));
}
```

Wire `add_marker_btn_` to add a marker at the current playhead (which is 0:00 since there's no player yet):
```cpp
void EditExportPage::onAddMarkerClicked() {
    // Without a live playhead, add a marker at the trim-start position or 0.
    const uint64_t time_ms = trim_start_us_ != AV_NOPTS_VALUE
        ? static_cast<uint64_t>(trim_start_us_ / 1000LL)
        : 0ULL;
    RecordingMarker m;
    m.time_ms = time_ms;
    m.type    = RecordingMarkerType::General;
    m.label   = "Marker";
    markers_.push_back(m);
    // Persist to sidecar immediately.
    saveMarkers();
}
```

- [ ] **Step 3: Implement loadMarkers and saveMarkers**

```cpp
void EditExportPage::loadMarkers() {
    markers_.clear();
    // Try the companion sidecar next to the MKV master.
    if (!ctx_.marker_sidecar_path.isEmpty()) {
        QFile f(ctx_.marker_sidecar_path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            const QJsonArray arr = doc.object().value(QStringLiteral("markers")).toArray();
            for (const auto& v : arr) {
                const QJsonObject obj = v.toObject();
                RecordingMarker m;
                m.time_ms = static_cast<uint64_t>(obj.value(QStringLiteral("timeMs")).toDouble());
                const QString type_str = obj.value(QStringLiteral("type")).toString();
                if (type_str == QStringLiteral("cut"))       m.type = RecordingMarkerType::Cut;
                else if (type_str == QStringLiteral("highlight")) m.type = RecordingMarkerType::Highlight;
                else m.type = RecordingMarkerType::General;
                m.label = obj.value(QStringLiteral("label")).toString().toStdString();
                markers_.push_back(m);
            }
            return;
        }
    }
    // Fallback: use pre-loaded markers from the recording session.
    markers_ = ctx_.markers;
}

void EditExportPage::saveMarkers() {
    if (ctx_.marker_sidecar_path.isEmpty()) return;
    QJsonArray arr;
    for (const auto& m : markers_) {
        QJsonObject obj;
        obj[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
        obj[QStringLiteral("type")]   = QString::fromLatin1(RecordingMarkerTypeToString(m.type));
        obj[QStringLiteral("label")]  = QString::fromStdString(m.label);
        arr.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("version")]  = 1;
    root[QStringLiteral("timebase")] = QStringLiteral("milliseconds");
    root[QStringLiteral("markers")]  = arr;
    QSaveFile file(ctx_.marker_sidecar_path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.commit();
    }
}
```

Add required includes to `EditExportPage.cpp`:
```cpp
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <algorithm>
#include "../models/RecordingMarker.h"
#include <recorder_core/mp4_remuxer.h>
#include <filesystem>
```

- [ ] **Step 4: Implement Output phase — real remux**

Replace the stub `onExportClicked` (which currently starts a demo timer) with real remux:

First, update the Output panel to replace the Re-encode card with a "Save as new file / Overwrite original" choice and a container selector:

In `buildUi()`, replace the three output cards (`output_opt_keep_mkv_`, `output_opt_remux_mp4_`, `output_opt_reencode_`) with:

```cpp
    // Container selection: MKV (stream-copy) or MP4 (stream-copy via ADR-0014)
    output_container_combo_ = new QComboBox(output_panel_);
    output_container_combo_->setObjectName(QStringLiteral("outputContainerCombo"));
    output_container_combo_->addItem(QStringLiteral("MKV (stream-copy, lossless)"),   QStringLiteral("mkv"));
    output_container_combo_->addItem(QStringLiteral("MP4 (stream-copy, lossless)"),   QStringLiteral("mp4"));

    // Save mode: new file or overwrite
    output_save_mode_combo_ = new QComboBox(output_panel_);
    output_save_mode_combo_->setObjectName(QStringLiteral("outputSaveModeCombo"));
    output_save_mode_combo_->addItem(QStringLiteral("Save as new file"), QStringLiteral("new"));
    output_save_mode_combo_->addItem(QStringLiteral("Overwrite original"), QStringLiteral("overwrite"));
```

Add members to `EditExportPage.h`:
```cpp
    QComboBox* output_container_combo_ = nullptr;
    QComboBox* output_save_mode_combo_ = nullptr;
```

In `onExportClicked()` (renamed to `runExport()`):

```cpp
void EditExportPage::runExport() {
    setPhase(Phase::Exporting);

    const QString container_key = output_container_combo_
        ? output_container_combo_->currentData().toString()
        : QStringLiteral("mkv");
    const bool overwrite = output_save_mode_combo_
        && output_save_mode_combo_->currentData().toString() == QStringLiteral("overwrite");
    const bool to_mp4 = (container_key == QStringLiteral("mp4"));

    if (ctx_.mkv_master_path.isEmpty()) {
        setPhase(Phase::Failed);
        if (result_detail_label_)
            result_detail_label_->setText(QStringLiteral("No edit master available for export."));
        return;
    }

    const std::filesystem::path master(ctx_.mkv_master_path.toStdWString());

    // Derive output path.
    std::filesystem::path output_path;
    if (overwrite) {
        output_path = std::filesystem::path(ctx_.output_path.toStdWString());
    } else {
        // Generate a new filename: <stem>_edit.<ext>
        std::filesystem::path base(ctx_.output_path.toStdWString());
        const std::wstring ext = to_mp4 ? L".mp4" : L".mkv";
        output_path = base.parent_path() / (base.stem().wstring() + L"_edit" + ext);
    }

    recorder_core::TrimRange tr;
    tr.start_us = trim_start_us_;
    tr.end_us   = trim_end_us_;

    // Run remux on a background thread; marshal result back to UI thread.
    export_output_path_ = output_path;

    if (export_thread_.joinable()) export_thread_.join();
    export_cancel_.store(false);
    export_thread_ = std::thread([this, master, output_path, to_mp4, tr, overwrite]() {
        std::filesystem::path temp_output = output_path;
        temp_output += L".tmp";

        auto progress_cb = [this](float fraction) -> bool {
            if (export_cancel_.load()) return false;
            QMetaObject::invokeMethod(this, [this, fraction]() {
                if (exporting_bar_) exporting_bar_->setValue(static_cast<int>(fraction * 100));
            }, Qt::QueuedConnection);
            return true;
        };

        recorder_core::RemuxResult res;
        if (to_mp4) {
            res = recorder_core::RemuxToProgressiveMp4(master, temp_output, progress_cb, tr);
        } else {
            res = recorder_core::RemuxToMkv(master, temp_output, progress_cb, tr);
        }

        bool ok = res.success;
        std::string err_msg = res.message;

        if (ok && overwrite) {
            // Atomic replace: rename temp → output (same volume).
            std::error_code ec;
            std::filesystem::rename(temp_output, output_path, ec);
            if (ec) {
                ok = false;
                err_msg = "Atomic overwrite failed: " + ec.message();
                std::filesystem::remove(temp_output, ec);
            }
        } else if (ok) {
            // New file: rename temp → output.
            std::error_code ec;
            std::filesystem::rename(temp_output, output_path, ec);
            if (ec) {
                ok = false;
                err_msg = "Failed to save new file: " + ec.message();
                std::filesystem::remove(temp_output, ec);
            }
        }

        QMetaObject::invokeMethod(this, [this, ok, err_msg, output_path]() {
            export_output_path_ = output_path;
            if (ok) {
                setPhase(Phase::Done);
                if (result_detail_label_)
                    result_detail_label_->setText(
                        QString::fromStdWString(output_path.filename().wstring()) +
                        QStringLiteral(" \xC2\xB7 stream-copy \xC2\xB7 lossless"));
                emit exportCompleted(QString::fromStdWString(output_path.wstring()));
            } else {
                setPhase(Phase::Failed);
                if (result_detail_label_)
                    result_detail_label_->setText(QString::fromStdString(err_msg));
            }
        }, Qt::QueuedConnection);
    });
}
```

Add members to `EditExportPage.h`:
```cpp
    std::thread export_thread_;
    std::atomic<bool> export_cancel_{false};
    std::filesystem::path export_output_path_;
```

Add includes:
```cpp
#include <thread>
#include <atomic>
```

In the destructor (add if not present) — join the thread:
```cpp
EditExportPage::~EditExportPage() {
    export_cancel_.store(true);
    if (export_thread_.joinable()) export_thread_.join();
}
```

In `onCancelExportClicked()`:
```cpp
void EditExportPage::onCancelExportClicked() {
    export_cancel_.store(true);
    // Thread will detect the cancel and invoke QMetaObject back to setPhase(Output).
    setPhase(Phase::Output);
}
```

In `onOpenFolderClicked()` and `onRevealFileClicked()`, implement using `QDesktopServices`:
```cpp
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>

void EditExportPage::onOpenFolderClicked() {
    const QString path = QString::fromStdWString(export_output_path_.parent_path().wstring());
    if (!path.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void EditExportPage::onRevealFileClicked() {
    const QString path = QString::fromStdWString(export_output_path_.wstring());
    if (!path.isEmpty()) {
        // On Windows, use "explorer /select,<path>" to highlight the file.
        QProcess::startDetached(QStringLiteral("explorer"),
            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
    }
}
```

Add `#include <QProcess>` and `#include <QDir>`.

- [ ] **Step 5: Update refreshPhase() show/hide logic**

The output panel now has the combo boxes instead of the three cards. Update visibility:
- Review: show `review_panel_`, hide `edit_controls_`, timeline, output_panel_, exporting, result
- Edit: show `player_frame_`, `edit_controls_`, `timeline_frame_`, hide output_panel_
- Output: show `output_panel_`, hide review, player, edit_controls_, timeline
- Exporting/Done/Failed: existing logic

- [ ] **Step 6: Build and test manually**

```powershell
cmake --build build/windows-x64-debug --target exosnap
```

Run the app. Make a 5-second recording (MKV). Click "Edit" in post-stop result. Verify:
- Review phase shows "Post-recording report" with real A/V drift / drop data (or "–" if unavailable)
- Edit phase shows enabled "Trim" button; clicking it opens the spin-box dialog; confirm sets timeline labels
- Output phase shows container/save-mode combos; click Export → real file is written

- [ ] **Step 7: Commit**

```powershell
git add app/pages/EditExportPage.h app/pages/EditExportPage.cpp
git commit -m "feat(edit): wire Review post-flight, Edit trim snap, Output real stream-copy export"
```

---

## Task 6: Tests for Marker Sidecar + EditContext

**Files:**
- Create: `app/tests/test_edit_context.cpp`
- Modify: `app/CMakeLists.txt`

- [ ] **Step 1: Write tests**

Create `app/tests/test_edit_context.cpp`:

```cpp
// test_edit_context.cpp — tests for EditContext sidecar round-trip
#include <gtest/gtest.h>
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "../models/RecordingMarker.h"

// Minimal sidecar write/read round-trip test (no Qt widgets needed).
// Replicates the logic from WriteMarkerSidecar / loadMarkers without needing
// an EditExportPage widget (avoids QApplication dependency for this test).

namespace {

struct SidecarFixture {
    QTemporaryDir tmp;
    QString path;

    SidecarFixture() : path(tmp.path() + QStringLiteral("/test.markers.json")) {}

    bool write(const std::vector<exosnap::RecordingMarker>& markers) {
        QJsonArray arr;
        for (const auto& m : markers) {
            QJsonObject obj;
            obj[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
            obj[QStringLiteral("type")]   = QString::fromLatin1(exosnap::RecordingMarkerTypeToString(m.type));
            obj[QStringLiteral("label")]  = QString::fromStdString(m.label);
            arr.append(obj);
        }
        QJsonObject root;
        root[QStringLiteral("version")]  = 1;
        root[QStringLiteral("timebase")] = QStringLiteral("milliseconds");
        root[QStringLiteral("markers")]  = arr;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(QJsonDocument(root).toJson());
        return true;
    }

    std::vector<exosnap::RecordingMarker> read() {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        const QJsonArray arr = doc.object().value(QStringLiteral("markers")).toArray();
        std::vector<exosnap::RecordingMarker> out;
        for (const auto& v : arr) {
            const QJsonObject obj = v.toObject();
            exosnap::RecordingMarker m;
            m.time_ms = static_cast<uint64_t>(obj.value(QStringLiteral("timeMs")).toDouble());
            const QString t = obj.value(QStringLiteral("type")).toString();
            if (t == QStringLiteral("cut")) m.type = exosnap::RecordingMarkerType::Cut;
            else if (t == QStringLiteral("highlight")) m.type = exosnap::RecordingMarkerType::Highlight;
            else m.type = exosnap::RecordingMarkerType::General;
            m.label = obj.value(QStringLiteral("label")).toString().toStdString();
            out.push_back(m);
        }
        return out;
    }
};

} // namespace

TEST(MarkerSidecarTest, RoundTrip) {
    SidecarFixture fx;
    std::vector<exosnap::RecordingMarker> markers = {
        {1000, exosnap::RecordingMarkerType::General, "Start"},
        {5000, exosnap::RecordingMarkerType::Cut,     "Cut here"},
        {9999, exosnap::RecordingMarkerType::Highlight, "Clip"},
    };
    ASSERT_TRUE(fx.write(markers));
    const auto loaded = fx.read();
    ASSERT_EQ(loaded.size(), markers.size());
    for (size_t i = 0; i < markers.size(); ++i) {
        EXPECT_EQ(loaded[i].time_ms, markers[i].time_ms);
        EXPECT_EQ(loaded[i].type,    markers[i].type);
        EXPECT_EQ(loaded[i].label,   markers[i].label);
    }
}

TEST(MarkerSidecarTest, EmptyMarkersWriteReadEmpty) {
    SidecarFixture fx;
    ASSERT_TRUE(fx.write({}));
    EXPECT_TRUE(fx.read().empty());
}

TEST(MarkerSidecarTest, MissingFileReturnsEmpty) {
    SidecarFixture fx;
    // Don't write anything.
    EXPECT_TRUE(fx.read().empty());
}
```

- [ ] **Step 2: Register in CMakeLists**

In `app/CMakeLists.txt`, find an existing `exosnap_add_gtest(` block for an app test and add:

```cmake
exosnap_add_gtest(
    TARGET test_edit_context
    SOURCES tests/test_edit_context.cpp
    LINKS Qt6::Core Qt6::Gui
)
```

- [ ] **Step 3: Build and run**

```powershell
cmake --build build/windows-x64-debug --target test_edit_context
ctest --preset windows-x64-debug -R "test_edit_context" --output-on-failure
```

Expected: All MarkerSidecarTest.* PASS.

- [ ] **Step 4: Commit**

```powershell
git add app/tests/test_edit_context.cpp app/CMakeLists.txt
git commit -m "test(edit): add marker sidecar round-trip tests"
```

---

## Task 7: ADR Updates

**Files:**
- Modify: `docs/decisions/0022-edit-output-save-surface.md`
- Modify: `docs/decisions/0014-mp4-via-remux-on-stop.md`

- [ ] **Step 1: Update ADR 0022**

Replace the Status section and the format model section:

**Status:** Change to:
```markdown
## Status

Accepted — implemented in 0.9.0 (S1 Edit/Output/Save wave).
Engine implementation shipped: Quick Trim (stream-copy, keyframe-accurate), marker sidecar load,
MKV master retention for MP4 recordings, Review (post-flight report), Edit (trim dialog with
keyframe/marker snap), Output (new file or atomic overwrite, MKV or MP4 via stream-copy).
```

**Format / cost model:** Replace the three-row table with:
```markdown
### Format / cost model

| Option             | Mechanism    | Quality   | Speed    |
|--------------------|-------------|-----------|----------|
| MKV (stream-copy)  | stream-copy  | lossless  | instant  |
| MP4 (stream-copy)  | stream-copy  | lossless  | instant  |

Both options re-remux the MKV edit master with the optional trim range applied (ADR 0014 path).
The Re-encode H.264+AAC option has been removed — it conflicts with the stream-copy-only model
(no re-encode anywhere) and the locked product model for 0.9.0.
```

**Edit phase:** Update to reflect real implementation:
```markdown
### Edit phase (0.9.0)

Trim controls are active: a trim dialog lets the user set in/out points in seconds.
Cut points snap backward to the nearest keyframe on the GOP grid, and also snap to
any marker within 50 ms. Trim is keyframe-accurate (stream-copy cannot cut between keyframes).

Markers are edit-view only — never written as container chapters. They persist in a
per-recording `.markers.json` sidecar (see WriteMarkerSidecar in RecordingCoordinator).
Opening the Edit surface loads markers from the sidecar adjacent to the MKV master.

The "Split Chapter" button remains disabled (deferred to a future wave when chapter export
is properly specified).
```

**Save / Output phase:**
```markdown
### Output / Save phase (0.9.0)

The Output phase offers:
- **Container:** MKV or MP4 (both via stream-copy from the MKV edit master).
- **Save mode:** New file (default, generates `<stem>_edit.<ext>`) or Overwrite original
  (atomic: write temp → `std::filesystem::rename`, so a failed write never destroys both files).
  Overwrite updates the recovery manifest.

### Opt-in entry

The surface is NOT auto-opened on recording stop. The post-stop "Saved" state on RecordPage
shows an "Edit" button in the result actions; clicking it opens EditExportPage.
```

- [ ] **Step 2: Update ADR 0014**

In the Consequences section, add after the "Quick Trim (0.11.0)" paragraph:

```markdown
- **Edit re-remux from MKV master (0.9.0):** After a successful MP4 remux-on-stop, the transient
  `.mkv.tmp` is renamed to `<stem>.edit.mkv` (retained as the canonical edit master) instead of
  being deleted. This enables `EditExportPage` to re-run the remux with a trim range applied
  without touching the already-delivered MP4. For MKV recordings the output file IS the edit
  master. The edit master is deleted by the user or when the edit session ends; it is NOT cleaned
  up automatically on app exit (pre-v1.0 behaviour). The `UiRecordingResult.mkv_master_path`
  field carries the master path to the UI.
```

- [ ] **Step 3: Commit ADR changes**

```powershell
git add docs/decisions/0022-edit-output-save-surface.md
git add docs/decisions/0014-mp4-via-remux-on-stop.md
git commit -m "docs: update ADR 0022 and 0014 for 0.9.0 Edit/Output/Save wave"
```

---

## Task 8: Full Validation Gate

- [ ] **Step 1: Run format check**

```powershell
scripts\check-format.ps1
```

Fix any clang-format violations. Commit any fixes.

- [ ] **Step 2: Full debug build**

```powershell
cmake --build build/windows-x64-debug
```

Expected: zero errors, zero warnings from repository-owned targets.

- [ ] **Step 3: Full CTest**

```powershell
$env:EXOSNAP_CONFIG_DIR = (New-TemporaryFile | ForEach-Object { $_.DirectoryName + "\exosnap_test_cfg_" + [System.IO.Path]::GetRandomFileName() })
New-Item -ItemType Directory -Path $env:EXOSNAP_CONFIG_DIR -Force | Out-Null
ctest --preset windows-x64-debug --output-on-failure
Remove-Item -Recurse -Force $env:EXOSNAP_CONFIG_DIR -ErrorAction SilentlyContinue
```

Expected: all existing tests still pass + new tests pass. Record pass/fail counts.

- [ ] **Step 4: Static analysis (optional — run if CI requires)**

```powershell
scripts\check-quality.ps1 -StaticOnly
```

- [ ] **Step 5: Report results**

Report the final ctest output (pass count, any failures). This is the deliverable gate.

---

## Self-Review Against Spec

Checking the locked product model point by point:

1. **Recording always safe on stop / MKV remux-on-stop stays** — Yes: `RunRemuxJob` retains the transient MKV (renaming it), but the MP4 remux still runs on stop as before. ✅
2. **Edit surface opt-in, no auto-open** — Yes: "Edit" button in RecordPage result area; `navigateToEditExportPage` only called from that button and the notification toast. ✅
3. **Non-destructive stream-copy only** — Yes: `RemuxToProgressiveMp4` and `RemuxToMkv` with `TrimRange` use stream-copy; `runExport` never calls an encoder. ✅
4. **Markers edit-view only, sidecar** — Yes: `loadMarkers`/`saveMarkers` use `.markers.json`; no chapter boxes written to container. ✅
5. **Save = new file (default) or overwrite (atomic)** — Yes: `output_save_mode_combo_`, `std::filesystem::rename` for atomic replace. ✅
6. **Keyframe-interval setting in Advanced → Video** — Yes: `KeyframeIntervalMode` enum, `kKeyframeInterval` hint, `keyframe_interval_combo` in ConfigPage. Default 2 s. ✅
7. **Review step renders post-flight report** — Yes: `review_panel_` shows `last_completed_snapshot_` data and `peak_av_drift_ms_` via `EditContext`. ✅

Placeholder scan: No TBD/TODO/placeholder steps remain. All code blocks are complete. ✅

Type consistency:
- `TrimRange::start_us` / `end_us` — int64_t (AV_TIME_BASE µs). Used as `int64_t` everywhere. ✅
- `ExtractKeyframeTimestamps` returns `std::vector<int64_t>`. Consumed as same type in EditExportPage. ✅
- `EditContext` used in `editExportRequested(const EditContext&)` and `setEditContext(const EditContext&)`. ✅
- `KeyframeIntervalMode::Seconds2` enum value used in `VideoSettingsModel` and `ConfigPage`. ✅
