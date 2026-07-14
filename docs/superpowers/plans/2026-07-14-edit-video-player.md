# Edit Video Player Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the "Video preview — coming in 0.11" placeholder in the Edit/Output/Save overlay
with a real FFmpeg-decode + Qt-paint video player with synchronized audio, driving the
already-built trim/scrub/playhead UI against real decoded frames instead of a synthetic clock.

**Architecture:** A new companion-FFmpeg-build release enables the decoders that are currently
completely absent from the vendored `avcodec`. A new UI-agnostic `recorder_core` engine
(`EditPlayerEngine` + `WasapiAudioRenderer` + `EditPlayerSession`) demuxes/decodes the MKV edit
master, converts frames to BGRA on the CPU (no `swscale` — extends the existing tested
`yuv_to_bgra` helper), and renders audio through a brand-new WASAPI render client that becomes the
playback master clock. A new `EditPlayerSurface` Qt widget (modeled on the existing
`CameraPreview`) paints the frames; `EditExportPage` wires it to the existing play/pause/scrub/trim
UI, replacing only the synthetic clock's video/audio backing, not the UI itself.

**Tech Stack:** C++20, Qt 6 Widgets, FFmpeg (`avformat`/`avcodec`/`avutil`/`swresample`, vendored via
`cmake/VendorFFmpeg.cmake`), WASAPI (`Audioclient.h`/`mmdeviceapi.h`), GoogleTest via
`exosnap_add_gtest`.

## Global Constraints

- Engine code (`libs/recorder_core`) must contain **no Qt types** — CLAUDE.md: "Keep the engine
  UI-agnostic." Only `app/` files may include Qt headers.
- **Never drive the running application** — no mouse/keyboard synthesis, no window automation.
  Starting `exosnap.exe` once to confirm no startup crash is allowed; nothing interactive.
- Live playback/audio correctness (does it actually look/sound right) is a **manual verification
  step for the user** — no test in this plan depends on real WASAPI hardware or a real compressed
  video/audio bitstream (none can be synthesized in this repo's test environment: no encoder is
  vendored, no GPU in CI).
- Product default video codecs: H.264, HEVC, AV1 (NVENC). Product default audio codecs: Opus, AAC,
  PCM (16/24/32-bit int, 32-bit float), FLAC (16/24-bit). Default sample rates: 44.1/48/96 kHz.
  Default keyframe interval: 2 s (selectable 1 s / 0.5 s) — Settings → Advanced → Video.
- `EditContext::mkv_master_path` is always the file to decode (the edit master; identical to
  `output_path` for MKV recordings) — never `output_path` directly, which may be a remuxed MP4.
- Follow existing `recorder_core` conventions exactly: RAII guards for FFmpeg context types
  (`InputCtxGuard`-style), `logging::log(LogLevel, component, message, fields)` for structured
  logs, `bool Method(..., std::string& out_error)` for fallible setup, trailing-underscore private
  members, PascalCase free functions / methods, snake_case locals.
- Run `scripts/run-tests.ps1 -Filter recorder_core.` (or the specific new binary name) for focused
  verification during each task; the full gate (`scripts/run-tests.ps1` with no filter, full Debug
  build, `check-format.ps1`, `check-quality.ps1 -StaticOnly`) runs once at the end (Task 11), per
  AGENTS.md's Fast Iteration Policy.
- `scripts/run-tests.ps1` sets `EXOSNAP_CONFIG_DIR`, `QT_QPA_PLATFORM=offscreen`, and Qt/FFmpeg on
  `PATH` automatically — use it, not raw `ctest`.

---

## Task 1: Companion FFmpeg build — enable decoders (external repo)

**Files (in a separate clone of `Exoridus/exosnap-ffmpeg-build`, NOT this repo):**
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: a new GitHub Release tag (`r4`) of `Exoridus/exosnap-ffmpeg-build` with decoders
  enabled, plus its SHA256 and asset URL — consumed by Task 2.

- [ ] **Step 1: Clone the companion repo into the scratchpad and create a branch**

```bash
cd "$TEMP_SCRATCHPAD" 2>/dev/null || cd /tmp
gh repo clone Exoridus/exosnap-ffmpeg-build
cd exosnap-ffmpeg-build
git checkout -b add-edit-player-decoders
```

- [ ] **Step 2: Add the decoder whitelist entries**

Open `.github/workflows/build.yml` and find the `CONFIGURE_FLAGS` array (currently ends its
`--enable-parser=...`/`--enable-bsf=...` block around the lines documented in
`cmake/VendorFFmpeg.cmake`'s r1→r2→r3 history comment). Add, right after the existing
`--enable-parser=mpegaudio` line and before `--enable-bsf=aac_adtstoasc`:

```
            --enable-decoder=h264
            --enable-decoder=hevc
            --enable-decoder=av1
            --enable-decoder=opus
            --enable-decoder=aac
            --enable-decoder=flac
            --enable-decoder=pcm_s16le
            --enable-decoder=pcm_s24le
            --enable-decoder=pcm_s32le
            --enable-decoder=pcm_f32le
```

Also update the header comment above `CONFIGURE_FLAGS` (mirroring the existing r1→r2, r2→r3
comment style already in that file) with a new line:

```
      # r3 -> r4: added --enable-decoder=* for h264/hevc/av1/opus/aac/flac/pcm_* --
      # the Edit-page video player needs to decode every codec ExoSnap itself can
      # produce. Previously avcodec carried zero decoders (mux/demux-only build).
```

- [ ] **Step 3: Commit and push**

```bash
git add .github/workflows/build.yml
git commit -m "Enable h264/hevc/av1/opus/aac/flac/pcm decoders for the Edit-page video player"
git push -u origin add-edit-player-decoders
```

- [ ] **Step 4: Open the PR**

```bash
gh pr create --title "Enable decoders for the Edit-page video player" --body "$(cat <<'EOF'
## Summary
- Adds --enable-decoder flags for h264, hevc, av1, opus, aac, flac, and the pcm_* variants
  ExoSnap itself can produce.
- The current r3 build has zero decoders compiled in (mux/demux-only whitelist) -- the ExoSnap
  Edit-page video player needs real decode to replace its "coming in 0.11" placeholder.

## Test plan
- [ ] CI build succeeds and produces the new DLL set
- [ ] Release tagged r4 after merge
EOF
)"
```

- [ ] **Step 5: STOP — wait for the user**

Report the PR URL to the user and stop. The user merges the PR and cuts the `r4` release tag. Do
not proceed to Task 2 until the user confirms the release exists and provides (or you fetch via
`gh release view r4 --repo Exoridus/exosnap-ffmpeg-build`) the release asset's SHA256.

---

## Task 2: Bump `VendorFFmpeg.cmake` to the r4 decoder release

**Files:**
- Modify: `cmake/VendorFFmpeg.cmake:31,38-39`

**Interfaces:**
- Consumes: the `r4` release tag + asset SHA256 from Task 1.
- Produces: `FFmpeg::avcodec` (and the `FFmpeg::mux` bundle) now contain working decoders,
  consumed starting at Task 5.

- [ ] **Step 1: Get the new release's asset URL and SHA256**

```bash
gh release view r4 --repo Exoridus/exosnap-ffmpeg-build --json assets \
  --jq '.assets[] | select(.name == "ffmpeg-win64-lgpl-shared.zip") | .url'
```

Download and hash it (or read the SHA256 the release workflow already prints into
`BUILD-INFO.txt` inside the archive, matching how r1-r3 were pinned).

- [ ] **Step 2: Update the pin**

Edit `cmake/VendorFFmpeg.cmake`:

```cmake
set(EXOSNAP_FFMPEG_VERSION "r4-n8.1.1"
    CACHE STRING "Pinned exosnap-ffmpeg-build release version (informational)")
```

and

```cmake
FetchContent_Declare(
    ffmpeg_prebuilt
    URL      "https://github.com/Exoridus/exosnap-ffmpeg-build/releases/download/r4/ffmpeg-win64-lgpl-shared.zip"
    URL_HASH "SHA256=<the real hash from Step 1>"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
```

Also update the file's header comment block (mirroring the existing r1→r2→r3 note style) with:

```
# r3 -> r4: added --enable-decoder=h264,hevc,av1,opus,aac,flac,pcm_s16le,pcm_s24le,
# pcm_s32le,pcm_f32le. Previous releases were mux/demux-only (zero decoders); the
# Edit-page video player needs real decode.
```

- [ ] **Step 3: Reconfigure and confirm the new DLLs are staged**

```powershell
cmake --preset windows-x64-debug
```

Expected: configure succeeds, re-downloads the r4 archive (fresh `URL_HASH`), and
`build/windows-x64-debug/_deps/ffmpeg_prebuilt-src/bin/avcodec-62.dll` exists (version number may
differ if the FFmpeg point release changed — confirm against the actual downloaded `BUILD-INFO.txt`
rather than assuming the number is unchanged).

- [ ] **Step 4: Build and confirm no link regressions**

```powershell
cmake --build --preset windows-x64-debug-exosnap
```

Expected: exit 0. This only proves linking still works — no decode code exists yet.

- [ ] **Step 5: Commit**

```bash
git add cmake/VendorFFmpeg.cmake
git commit -m "Bump vendored FFmpeg to r4 (adds decoders for the Edit-page video player)"
```

---

## Task 3: Extend `yuv_to_bgra` with fully-planar YUV420 → BGRA conversion

Software decoders (H.264/HEVC/AV1) produce `AV_PIX_FMT_YUV420P` / `YUV420P10LE` — fully-planar,
separate Y/U/V planes — not the semi-planar NV12/P010 layout `PlanarYuv420Frame` models today (that
struct was written for DXGI capture/encode surfaces). Reuses the exact same tested BT.709/BT.601,
8-/10-bit, Full/Limited fixed-point math already in `yuv_to_bgra.cpp` (`ComputeCoefs`,
`ClampFixedToByte` stay file-private and are called from both the existing and the new function —
no duplicated coefficient logic).

**Files:**
- Modify: `libs/recorder_core/src/yuv_to_bgra.h`
- Modify: `libs/recorder_core/src/yuv_to_bgra.cpp`
- Modify: `libs/recorder_core/tests/test_yuv_to_bgra.cpp`

**Interfaces:**
- Produces: `recorder_core::FullPlanarYuv420Frame`, `recorder_core::ConvertFullPlanarYuv420ToBgra`
  — consumed by Task 5's `EditPlayerEngine`.

- [ ] **Step 1: Write the failing tests**

Append to `libs/recorder_core/tests/test_yuv_to_bgra.cpp` (add `ConvertFullPlanarYuv420ToBgra` and
`FullPlanarYuv420Frame` to the `using` block at the top alongside the existing ones):

```cpp
using recorder_core::ConvertFullPlanarYuv420ToBgra;
using recorder_core::FullPlanarYuv420Frame;
```

Then, at the end of the file (before the final `DegenerateInputsAreNoOps` block, or after it — any
top-level position is fine), add:

```cpp
// --- Fully-planar YUV420 (separate Y/U/V planes -- FFmpeg AV_PIX_FMT_YUV420P /
// YUV420P10LE software-decoder output) golden vectors. Reuses the exact NV12
// goldens above -- same normalized Y'/Cb'/Cr' values, different memory layout.

TEST(FullPlanarYuv420ToBgra, GoldenBt709Limited8Bit) {
    // Same golden pixel as GoldenBt709Limited's third case: Y=126,Cr=180 -> (221,100,128).
    constexpr uint32_t kW = 2, kH = 2;
    std::vector<uint8_t> y_plane(kW * kH, 126);
    std::vector<uint8_t> u_plane(1, 128); // 2x2 luma -> 1x1 chroma
    std::vector<uint8_t> v_plane(1, 180);

    FullPlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = kW;
    src.u_plane = u_plane.data();
    src.u_stride_bytes = 1;
    src.v_plane = v_plane.data();
    src.v_stride_bytes = 1;
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 8;

    recorder_core::YuvToBgraParams params;
    params.matrix = recorder_core::MatrixCoefficients::Bt709;
    params.range = recorder_core::ColorRange::Limited;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), kW * 4);

    for (uint32_t i = 0; i < kW * kH; ++i) {
        const uint8_t* px = out.data() + i * 4;
        EXPECT_NEAR(static_cast<int>(px[2]), 221, 1) << "R at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[1]), 100, 1) << "G at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[0]), 128, 1) << "B at pixel " << i;
        EXPECT_EQ(px[3], 255);
    }
}

TEST(FullPlanarYuv420ToBgra, GoldenBt709Limited10Bit) {
    // Same golden as GoldenP010Bt709Limited's third case: Y=504,Cr=720 -> (221,100,128),
    // but as 16-bit little-endian words with the FULL 10-bit value (no <<6 left-justify --
    // that packing is P010-specific; planar YUV420P10LE stores the plain 10-bit value).
    constexpr uint32_t kW = 2, kH = 2;
    std::vector<uint16_t> y_plane(kW * kH, 504);
    std::vector<uint16_t> u_plane(1, 512);
    std::vector<uint16_t> v_plane(1, 720);

    FullPlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(y_plane.data());
    src.y_stride_bytes = static_cast<uint32_t>(kW * sizeof(uint16_t));
    src.u_plane = reinterpret_cast<const uint8_t*>(u_plane.data());
    src.u_stride_bytes = static_cast<uint32_t>(sizeof(uint16_t));
    src.v_plane = reinterpret_cast<const uint8_t*>(v_plane.data());
    src.v_stride_bytes = static_cast<uint32_t>(sizeof(uint16_t));
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 10;

    recorder_core::YuvToBgraParams params;
    params.matrix = recorder_core::MatrixCoefficients::Bt709;
    params.range = recorder_core::ColorRange::Limited;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), kW * 4);

    for (uint32_t i = 0; i < kW * kH; ++i) {
        const uint8_t* px = out.data() + i * 4;
        EXPECT_NEAR(static_cast<int>(px[2]), 221, 1) << "R at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[1]), 100, 1) << "G at pixel " << i;
        EXPECT_NEAR(static_cast<int>(px[0]), 128, 1) << "B at pixel " << i;
        EXPECT_EQ(px[3], 255);
    }
}

TEST(FullPlanarYuv420ToBgra, NonUniformChromaBlocksEveryPixel) {
    // Same 4x4 four-quadrant scenario as YuvToBgra.NonUniformChromaBlocksEveryPixel,
    // re-expressed with separate U/V planes instead of interleaved UV.
    constexpr uint32_t kW = 4, kH = 4;
    std::vector<uint8_t> y_plane(kW * kH, 128);
    // 2x2 chroma grid (one sample per 2x2 luma block).
    const std::vector<uint8_t> u_plane = {128, 180, 128, 180};
    const std::vector<uint8_t> v_plane = {180, 128, 128, 180};
    struct Rgb {
        int r, g, b;
    };
    const Rgb expected_blocks[2][2] = {
        {{210, 104, 128}, {128, 118, 224}},
        {{128, 128, 128}, {210, 94, 224}},
    };

    FullPlanarYuv420Frame src;
    src.y_plane = y_plane.data();
    src.y_stride_bytes = kW;
    src.u_plane = u_plane.data();
    src.u_stride_bytes = kW / 2;
    src.v_plane = v_plane.data();
    src.v_stride_bytes = kW / 2;
    src.width = kW;
    src.height = kH;
    src.bits_per_sample = 8;

    recorder_core::YuvToBgraParams params;
    params.matrix = recorder_core::MatrixCoefficients::Bt709;
    params.range = recorder_core::ColorRange::Full;

    std::vector<uint8_t> out(kW * kH * 4, 0);
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), kW * 4);

    for (uint32_t r = 0; r < kH; ++r) {
        for (uint32_t c = 0; c < kW; ++c) {
            const Rgb& want = expected_blocks[r / 2][c / 2];
            const uint8_t* px = out.data() + (r * kW + c) * 4;
            EXPECT_NEAR(static_cast<int>(px[2]), want.r, 1) << "R at (" << r << "," << c << ")";
            EXPECT_NEAR(static_cast<int>(px[1]), want.g, 1) << "G at (" << r << "," << c << ")";
            EXPECT_NEAR(static_cast<int>(px[0]), want.b, 1) << "B at (" << r << "," << c << ")";
            EXPECT_EQ(px[3], 255);
        }
    }
}

TEST(FullPlanarYuv420ToBgra, DegenerateInputsAreNoOps) {
    std::vector<uint8_t> out(16, 0xAB);
    FullPlanarYuv420Frame src; // all zero/null by default
    recorder_core::YuvToBgraParams params;
    ConvertFullPlanarYuv420ToBgra(src, params, out.data(), 4);
    for (uint8_t b : out)
        EXPECT_EQ(b, 0xAB);
}
```

- [ ] **Step 2: Run the tests and confirm they fail to compile**

```powershell
cmake --build --preset windows-x64-debug --target test_yuv_to_bgra
```

Expected: FAIL — `FullPlanarYuv420Frame`/`ConvertFullPlanarYuv420ToBgra` not declared.

- [ ] **Step 3: Add the struct to the header**

In `libs/recorder_core/src/yuv_to_bgra.h`, after the existing `PlanarYuv420Frame` struct and before
`ConvertYuv420ToBgra`'s declaration, add:

```cpp
// Describes one fully-planar 4:2:0 YUV frame: separate Y, U, V planes (no
// chroma interleaving). This is the layout FFmpeg's software decoders use
// (AV_PIX_FMT_YUV420P for 8-bit, AV_PIX_FMT_YUV420P10LE for 10-bit) -- as
// opposed to PlanarYuv420Frame's semi-planar interleaved-UV layout, which
// models the DXGI capture/encode surfaces (NV12/P010).
//
// *_stride_bytes are the row pitch and may exceed width/2 (chroma) or width
// (luma) * bytes-per-sample when the source buffer is padded.
//
// 10-bit (YUV420P10LE) samples are plain 16-bit little-endian values in
// [0, 1023] -- unlike P010, there is no <<6 left-justification.
struct FullPlanarYuv420Frame {
    const uint8_t* y_plane = nullptr;
    uint32_t y_stride_bytes = 0;
    const uint8_t* u_plane = nullptr;
    uint32_t u_stride_bytes = 0;
    const uint8_t* v_plane = nullptr;
    uint32_t v_stride_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits_per_sample = 8; // 8 (YUV420P) or 10 (YUV420P10LE)
};

// Converts one fully-planar 4:2:0 YUV frame to top-down BGRA8888 (B, G, R, A
// byte order; alpha always 255/opaque). Same matrix/range semantics as
// ConvertYuv420ToBgra -- only the input memory layout differs.
//
// out_bgra must have at least `height * out_stride_bytes` bytes available;
// out_stride_bytes must be >= src.width * 4. Does nothing if src.width,
// src.height, a plane pointer, or out_bgra is 0/null.
void ConvertFullPlanarYuv420ToBgra(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                   uint8_t* out_bgra, uint32_t out_stride_bytes);
```

- [ ] **Step 4: Implement in the .cpp**

In `libs/recorder_core/src/yuv_to_bgra.cpp`, add after `ConvertYuv420ToBgra` (which already ends
at the closing brace before `ConvertAyuvToBgra`):

```cpp
void ConvertFullPlanarYuv420ToBgra(const FullPlanarYuv420Frame& src, const YuvToBgraParams& params,
                                   uint8_t* out_bgra, uint32_t out_stride_bytes) {
    if (src.width == 0 || src.height == 0 || src.y_plane == nullptr || src.u_plane == nullptr ||
        src.v_plane == nullptr || out_bgra == nullptr)
        return;

    const FixedCoefs c = ComputeCoefs(params.matrix, params.range, src.bits_per_sample);

    if (src.bits_per_sample > 8) {
        // YUV420P10LE: plain 16-bit little-endian values in [0, 1023] (no P010
        // left-justification -- unlike the semi-planar path above).
        for (uint32_t row = 0; row < src.height; ++row) {
            const auto* y_row =
                reinterpret_cast<const uint16_t*>(src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes);
            const auto* u_row =
                reinterpret_cast<const uint16_t*>(src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes);
            const auto* v_row =
                reinterpret_cast<const uint16_t*>(src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes);
            uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
            for (uint32_t col = 0; col < src.width; col += 2u) {
                const int32_t u_val = static_cast<int32_t>(u_row[col / 2u]) - c.c_off;
                const int32_t v_val = static_cast<int32_t>(v_row[col / 2u]) - c.c_off;
                const int32_t b_term = c.c_bu * u_val;
                const int32_t g_term = -c.c_gu * u_val - c.c_gv * v_val;
                const int32_t r_term = c.c_rv * v_val;
                const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
                for (uint32_t p = col; p < pair_end; ++p) {
                    const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[p]) - c.y_off);
                    uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
                    px[0] = ClampFixedToByte(luma + b_term); // B
                    px[1] = ClampFixedToByte(luma + g_term); // G
                    px[2] = ClampFixedToByte(luma + r_term); // R
                    px[3] = 255u;                            // A
                }
            }
        }
        return;
    }

    // YUV420P: 8-bit samples, separate U/V planes.
    for (uint32_t row = 0; row < src.height; ++row) {
        const uint8_t* y_row = src.y_plane + static_cast<size_t>(row) * src.y_stride_bytes;
        const uint8_t* u_row = src.u_plane + static_cast<size_t>(row / 2u) * src.u_stride_bytes;
        const uint8_t* v_row = src.v_plane + static_cast<size_t>(row / 2u) * src.v_stride_bytes;
        uint8_t* out_row = out_bgra + static_cast<size_t>(row) * out_stride_bytes;
        for (uint32_t col = 0; col < src.width; col += 2u) {
            const int32_t u_val = static_cast<int32_t>(u_row[col / 2u]) - c.c_off;
            const int32_t v_val = static_cast<int32_t>(v_row[col / 2u]) - c.c_off;
            const int32_t b_term = c.c_bu * u_val;
            const int32_t g_term = -c.c_gu * u_val - c.c_gv * v_val;
            const int32_t r_term = c.c_rv * v_val;
            const uint32_t pair_end = (col + 2u <= src.width) ? (col + 2u) : src.width;
            for (uint32_t p = col; p < pair_end; ++p) {
                const int32_t luma = c.c_y * (static_cast<int32_t>(y_row[p]) - c.y_off);
                uint8_t* px = out_row + static_cast<size_t>(p) * 4u;
                px[0] = ClampFixedToByte(luma + b_term); // B
                px[1] = ClampFixedToByte(luma + g_term); // G
                px[2] = ClampFixedToByte(luma + r_term); // R
                px[3] = 255u;                            // A
            }
        }
    }
}
```

- [ ] **Step 5: Run the tests and confirm they pass**

```powershell
cmake --build --preset windows-x64-debug --target test_yuv_to_bgra
pwsh scripts/run-tests.ps1 -Filter test_yuv_to_bgra
```

Expected: PASS, all cases including the 4 new ones.

- [ ] **Step 6: Commit**

```bash
git add libs/recorder_core/src/yuv_to_bgra.h libs/recorder_core/src/yuv_to_bgra.cpp libs/recorder_core/tests/test_yuv_to_bgra.cpp
git commit -m "Add fully-planar YUV420 to BGRA conversion for software-decoded frames"
```

---

## Task 4: `PlaybackClock` — pure audio-clock and frame-selection logic

Pure, hardware-free logic: given a count of audio frames rendered so far, compute the playback
clock in milliseconds; given the clock and a small set of available decoded-video timestamps,
decide which frame to show and how many older ones to drop. No FFmpeg, no COM, no Qt.

**Files:**
- Create: `libs/recorder_core/src/playback_clock.h`
- Create: `libs/recorder_core/src/playback_clock.cpp`
- Create: `libs/recorder_core/tests/test_playback_clock.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt`

**Interfaces:**
- Produces: `recorder_core::AudioClockMs(uint64_t frames_rendered, uint32_t sample_rate_hz)`,
  `recorder_core::SelectFrameForClock(std::span<const int64_t> available_pts_ms, int64_t clock_ms)`
  returning `recorder_core::FrameSelection { std::optional<size_t> index; size_t dropped_count; }`
  — consumed by Task 8's `EditPlayerSession`.

- [ ] **Step 1: Write the failing tests**

Create `libs/recorder_core/tests/test_playback_clock.cpp`:

```cpp
#include <gtest/gtest.h>

#include "playback_clock.h"

#include <vector>

namespace {

using recorder_core::AudioClockMs;
using recorder_core::SelectFrameForClock;

TEST(AudioClockMs, ZeroFramesIsZero) {
    EXPECT_EQ(AudioClockMs(0, 48000), 0);
}

TEST(AudioClockMs, OneSecondAt48k) {
    EXPECT_EQ(AudioClockMs(48000, 48000), 1000);
}

TEST(AudioClockMs, HalfSecondAt44_1k) {
    // 22050 frames / 44100 Hz = 0.5s = 500ms.
    EXPECT_EQ(AudioClockMs(22050, 44100), 500);
}

TEST(AudioClockMs, ZeroSampleRateIsZeroNotDivByZero) {
    EXPECT_EQ(AudioClockMs(48000, 0), 0);
}

TEST(SelectFrameForClock, EmptyQueueSelectsNothing) {
    std::vector<int64_t> pts_ms;
    const auto sel = SelectFrameForClock(pts_ms, 1000);
    EXPECT_FALSE(sel.index.has_value());
    EXPECT_EQ(sel.dropped_count, 0u);
}

TEST(SelectFrameForClock, ClockBeforeFirstFrameSelectsNothingYet) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 50);
    EXPECT_FALSE(sel.index.has_value());
    EXPECT_EQ(sel.dropped_count, 0u);
}

TEST(SelectFrameForClock, PicksLatestFrameAtOrBeforeClock) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 250);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 1u); // pts 200
    EXPECT_EQ(sel.dropped_count, 1u); // pts 100 dropped
}

TEST(SelectFrameForClock, ExactMatchSelectsThatFrame) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 200);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 1u);
    EXPECT_EQ(sel.dropped_count, 1u); // pts 100 dropped -- dropped_count is purely positional,
                                      // per FrameSelection::dropped_count's own doc comment; an
                                      // exact pts match is not a special case
}

TEST(SelectFrameForClock, ClockPastAllFramesSelectsLastAndDropsRest) {
    std::vector<int64_t> pts_ms = {100, 200, 300};
    const auto sel = SelectFrameForClock(pts_ms, 999);
    ASSERT_TRUE(sel.index.has_value());
    EXPECT_EQ(*sel.index, 2u);
    EXPECT_EQ(sel.dropped_count, 2u);
}

} // namespace
```

- [ ] **Step 2: Register the test target and confirm it fails**

Add to `libs/recorder_core/CMakeLists.txt`, near the other pure/header-free-logic tests (e.g. next
to `test_preview_publish_gate`):

```cmake
# Pure playback-clock math for the Edit-page video player: audio-clock derivation
# from rendered-sample count, and video-frame selection/drop-counting against
# that clock. No FFmpeg/COM/Qt.
exosnap_add_gtest(
    NAME test_playback_clock
    TEST_PREFIX recorder_core.
    SOURCES tests/test_playback_clock.cpp
            src/playback_clock.cpp
)
target_include_directories(test_playback_clock PRIVATE src)
```

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target test_playback_clock
```

Expected: FAIL — `playback_clock.h`/`.cpp` do not exist yet.

- [ ] **Step 3: Create the header**

Create `libs/recorder_core/src/playback_clock.h`:

```cpp
#pragma once

// Pure playback-clock math for the Edit-page video player (no FFmpeg/COM/Qt).
//
// AudioClockMs turns "how many audio frames has the WASAPI render client
// actually written so far" into a playback position in milliseconds -- this
// is the playback master clock (see docs/superpowers/specs/
// 2026-07-14-edit-video-player-design.md).
//
// SelectFrameForClock turns "which decoded video frames are queued, and where
// is the clock now" into "which one to display, and how many older ones to
// drop as real, honest playback drops" -- no catch-up blending, no silent
// resync.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace recorder_core {

// frames_rendered: cumulative audio frames written to the render endpoint.
// sample_rate_hz: the render format's sample rate. Returns 0 if
// sample_rate_hz is 0 (defensive -- never divides by zero).
int64_t AudioClockMs(uint64_t frames_rendered, uint32_t sample_rate_hz) noexcept;

struct FrameSelection {
    // Index into the caller's available_pts_ms of the frame to display, or
    // nullopt if the clock is before the first available frame (nothing to
    // show yet -- keep showing whatever is already on screen).
    std::optional<size_t> index;
    // How many frames strictly before the selected index are now stale and
    // should be dropped/discarded by the caller (real drops, not blended).
    size_t dropped_count = 0;
};

// available_pts_ms must be sorted ascending (the natural decode order).
// Picks the LATEST frame whose pts is <= clock_ms. If clock_ms is before the
// first entry, returns {nullopt, 0} (nothing selected, nothing to drop yet).
FrameSelection SelectFrameForClock(std::span<const int64_t> available_pts_ms, int64_t clock_ms) noexcept;

} // namespace recorder_core
```

- [ ] **Step 4: Implement**

Create `libs/recorder_core/src/playback_clock.cpp`:

```cpp
#include "playback_clock.h"

#include <algorithm>

namespace recorder_core {

int64_t AudioClockMs(uint64_t frames_rendered, uint32_t sample_rate_hz) noexcept {
    if (sample_rate_hz == 0)
        return 0;
    return static_cast<int64_t>((frames_rendered * 1000ull) / sample_rate_hz);
}

FrameSelection SelectFrameForClock(std::span<const int64_t> available_pts_ms, int64_t clock_ms) noexcept {
    FrameSelection sel;
    if (available_pts_ms.empty())
        return sel;

    // upper_bound: first element strictly greater than clock_ms. The frame to
    // show is the one just before it (the latest <= clock_ms).
    const auto it = std::upper_bound(available_pts_ms.begin(), available_pts_ms.end(), clock_ms);
    if (it == available_pts_ms.begin())
        return sel; // clock is before the first frame -- nothing to show yet

    const size_t selected_index = static_cast<size_t>(std::distance(available_pts_ms.begin(), it)) - 1u;
    sel.index = selected_index;
    sel.dropped_count = selected_index; // every frame strictly before it is now stale
    return sel;
}

} // namespace recorder_core
```

- [ ] **Step 5: Run the tests and confirm they pass**

```powershell
cmake --build --preset windows-x64-debug --target test_playback_clock
pwsh scripts/run-tests.ps1 -Filter test_playback_clock
```

Expected: PASS, all 9 cases.

- [ ] **Step 6: Commit**

```bash
git add libs/recorder_core/src/playback_clock.h libs/recorder_core/src/playback_clock.cpp \
        libs/recorder_core/tests/test_playback_clock.cpp libs/recorder_core/CMakeLists.txt
git commit -m "Add pure playback-clock math for the Edit-page video player"
```

---

## Task 5: `EditPlayerEngine` — open file, color-metadata mapping, single-frame seek decode

The demux/decode engine's foundation: open the MKV master, locate video/audio streams, read the
container's own color tags (so decode uses the SAME BT.709/range the file was actually written
with, not a hardcoded assumption), and decode exactly one frame at a requested timestamp (the
scrub/trim-handle-drag path). Continuous playback decode is Task 6, layered on top of this same
class.

**Files:**
- Create: `libs/recorder_core/include/recorder_core/edit_player_engine.h`
- Create: `libs/recorder_core/src/edit_player_engine.cpp`
- Create: `libs/recorder_core/tests/test_edit_player_engine.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt`

**Interfaces:**
- Consumes: `recorder_core::FullPlanarYuv420Frame`/`ConvertFullPlanarYuv420ToBgra` (Task 3),
  `recorder_core::ColorMetadata`/`MatrixCoefficients`/`ColorRange` (existing `color_metadata.h`).
- Produces: `recorder_core::DecodedVideoFrame` (BGRA bytes + dims + pts), `recorder_core::EditPlayerEngine`
  with `Open`, `Close`, `HasVideoStream`, `HasAudioStream`, `DecodeFrameAt(int64_t target_us)` —
  consumed by Task 6 (continuous decode) and Task 8 (`EditPlayerSession`).

- [ ] **Step 1: Write the failing tests**

Create `libs/recorder_core/tests/test_edit_player_engine.cpp`:

```cpp
#include <gtest/gtest.h>

#include "recorder_core/edit_player_engine.h"

#include <filesystem>
#include <fstream>

namespace {

using recorder_core::EditPlayerEngine;

TEST(EditPlayerEngine, OpenNonexistentFileFails) {
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
    EXPECT_FALSE(err.empty());
}

TEST(EditPlayerEngine, OpenGarbageFileFails) {
    // A file that exists but is not a container libavformat can probe.
    const auto path = std::filesystem::temp_directory_path() / "edit_player_engine_garbage_test.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << "not a media file, just some bytes 1234567890";
    }
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(path, err));
    EXPECT_FALSE(err.empty());
    std::filesystem::remove(path);
}

TEST(EditPlayerEngine, ClosedEngineReportsNoStreams) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.HasVideoStream());
    EXPECT_FALSE(engine.HasAudioStream());
}

TEST(EditPlayerEngine, DecodeFrameAtWithoutOpenReturnsNullopt) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.DecodeFrameAt(0).has_value());
}

} // namespace
```

(Positive-path decode of a real compressed video/audio frame cannot be unit-tested here: no
encoder is vendored and CI has no GPU, so no valid H.264/HEVC/AV1 bitstream can be produced in this
environment. Real decode correctness is verified by the user recording a short clip and opening it
in the Edit page — see Task 11.)

- [ ] **Step 2: Register the test target and confirm it fails**

Add to `libs/recorder_core/CMakeLists.txt`, after the `test_playback_clock` entry from Task 4:

```cmake
# EditPlayerEngine: file-open/stream-discovery/error-path coverage only (no
# real compressed bitstream can be synthesized in this test environment --
# real decode is user-live-verified, see the plan's Task 11).
exosnap_add_gtest(
    NAME test_edit_player_engine
    TEST_PREFIX recorder_core.
    SOURCES tests/test_edit_player_engine.cpp
            src/edit_player_engine.cpp
            src/yuv_to_bgra.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_edit_player_engine PRIVATE src include)
```

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target test_edit_player_engine
```

Expected: FAIL — `recorder_core/edit_player_engine.h` does not exist yet.

- [ ] **Step 3: Create the public header**

Create `libs/recorder_core/include/recorder_core/edit_player_engine.h`:

```cpp
#pragma once

// EditPlayerEngine -- demux/decode engine for the Edit-page video player
// (docs/superpowers/specs/2026-07-14-edit-video-player-design.md).
//
// UI-agnostic (no Qt types) per CLAUDE.md. Opens the MKV edit master
// (EditContext::mkv_master_path), decodes video frames to ready-to-paint BGRA
// (internally reusing yuv_to_bgra.h -- a private recorder_core header never
// exposed here, since decoders emit fully-planar YUV420/YUV420P10LE that this
// engine converts before handing anything to a caller), and decodes audio to
// a fixed 48 kHz stereo interleaved float32 PCM stream (matching the
// product's own internal mix-bus format).
//
// This header covers Open/Close/stream-discovery/single-frame seek-decode
// (the scrub and trim-handle-drag path). Continuous playback decode
// (StartPlaybackDecode/StopPlaybackDecode) is declared here too but
// implemented alongside this class's .cpp in the next task.

#include <recorder_core/color_metadata.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace recorder_core {

// One decoded video frame, ready to paint: top-down BGRA8888, already
// color-converted using the SOURCE FILE's own container color tags (falls
// back to ColorMetadata::Sdr709() when the container's tags are unspecified,
// matching the product's own SDR default).
struct DecodedVideoFrame {
    int64_t pts_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_bytes = 0; // == width * 4, no padding
    // Shared (not unique) because frames cross from the engine's decode
    // thread to the UI thread via a queued call, which copies the callback's
    // arguments -- a shared_ptr avoids an extra full-frame copy on that hop.
    std::shared_ptr<const std::vector<uint8_t>> bgra;
};

// One block of decoded audio: 48 kHz stereo interleaved float32 PCM.
struct DecodedAudioBlock {
    int64_t pts_us = 0;
    uint32_t frame_count = 0; // sample frames (2 floats each)
    std::shared_ptr<const std::vector<float>> interleaved_stereo;
};

using VideoFrameCallback = std::function<void(DecodedVideoFrame)>;
using AudioBlockCallback = std::function<void(DecodedAudioBlock)>;

class EditPlayerEngine {
  public:
    EditPlayerEngine();
    ~EditPlayerEngine();

    EditPlayerEngine(const EditPlayerEngine&) = delete;
    EditPlayerEngine& operator=(const EditPlayerEngine&) = delete;

    // Opens `path` (the MKV edit master) and locates its video/audio streams.
    // Returns false with a human-readable message in out_error on failure
    // (missing file, unreadable container, no decodable video stream).
    bool Open(const std::filesystem::path& path, std::string& out_error);

    // Closes the file and stops any running playback decode (see Task 6).
    void Close();

    [[nodiscard]] bool HasVideoStream() const noexcept;
    [[nodiscard]] bool HasAudioStream() const noexcept;

    // Seeks to the keyframe at or before target_us and decodes forward to the
    // first frame at or after target_us. Synchronous; intended for the scrub
    // / trim-handle-drag path, called from a caller-owned worker thread (see
    // EditPlayerSession, Task 8) so it never blocks the UI thread. Returns
    // nullopt if not open, there is no video stream, or decode fails.
    [[nodiscard]] std::optional<DecodedVideoFrame> DecodeFrameAt(int64_t target_us);

    // ---- Continuous playback decode (implemented in Task 6) ----

    // Starts a background thread that decodes forward continuously from
    // `start_us`, delivering frames/audio via the callbacks below until
    // StopPlaybackDecode() is called. No-op if not open or already running.
    void StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio);

    // Stops and joins the playback decode thread, if running. Safe to call
    // even if not running (no-op). Called from Close() and the destructor.
    void StopPlaybackDecode();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core
```

- [ ] **Step 4: Implement `Open`/`Close`/stream discovery/`DecodeFrameAt`**

Create `libs/recorder_core/src/edit_player_engine.cpp`. This step covers everything except
`StartPlaybackDecode`/`StopPlaybackDecode`, which get real bodies in Task 6 (this task's version
compiles them as safe no-ops so the class is already usable standalone):

```cpp
#include "recorder_core/edit_player_engine.h"

#include "recorder_core/logging/logging.h"
#include "yuv_to_bgra.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

#include <atomic>
#include <mutex>
#include <span>
#include <thread>

// MSVC + C++: av_err2str uses a C99 compound literal, not valid C++. Mirrors
// the override already used in mp4_remuxer.cpp.
static inline const char* av_err2str_cpp(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}
#ifdef av_err2str
#undef av_err2str
#endif
#define av_err2str(e) av_err2str_cpp(e)

namespace recorder_core {

namespace {

constexpr const char* kLogComponent = "edit_player_engine";

void LogWarn(const char* msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
}

// RAII wrapper for AVFormatContext* (input), matching mp4_remuxer.cpp's InputCtxGuard.
struct InputCtxGuard {
    AVFormatContext* ctx = nullptr;
    ~InputCtxGuard() {
        if (ctx)
            avformat_close_input(&ctx);
    }
};

// RAII wrapper for one AVCodecContext*.
struct CodecCtxGuard {
    AVCodecContext* ctx = nullptr;
    ~CodecCtxGuard() {
        if (ctx)
            avcodec_free_context(&ctx);
    }
};

// Maps a container's own CICP color tags to recorder_core's color-metadata
// enums, falling back to the product's SDR BT.709/Limited default when the
// container left a tag unspecified -- matches the fallback mp4_remuxer.cpp
// already applies when copying color description forward.
MatrixCoefficients MapMatrix(AVColorSpace cs) noexcept {
    switch (cs) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return MatrixCoefficients::Bt601;
    case AVCOL_SPC_BT2020_NCL:
        return MatrixCoefficients::Bt2020Ncl;
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_UNSPECIFIED:
    default:
        return MatrixCoefficients::Bt709;
    }
}

ColorRange MapRange(AVColorRange r) noexcept {
    switch (r) {
    case AVCOL_RANGE_JPEG:
        return ColorRange::Full;
    case AVCOL_RANGE_MPEG:
        return ColorRange::Limited;
    case AVCOL_RANGE_UNSPECIFIED:
    default:
        return ColorRange::Limited; // product SDR default
    }
}

} // namespace

struct EditPlayerEngine::Impl {
    InputCtxGuard fmt;
    CodecCtxGuard video_codec;
    CodecCtxGuard audio_codec;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    ColorRange range = ColorRange::Limited;
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;

    std::thread playback_thread;
    std::atomic<bool> playback_running{false};
    std::atomic<bool> playback_cancel{false};
    std::mutex playback_mutex; // guards start/stop against concurrent calls

    [[nodiscard]] bool IsOpen() const noexcept {
        return fmt.ctx != nullptr;
    }
};

EditPlayerEngine::EditPlayerEngine() : impl_(std::make_unique<Impl>()) {
}

EditPlayerEngine::~EditPlayerEngine() {
    Close();
}

bool EditPlayerEngine::Open(const std::filesystem::path& path, std::string& out_error) {
    Close(); // tear down any previous session first

    const std::string path_str = path.string();

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        out_error = std::string("avformat_open_input failed: ") + av_err2str(ret);
        return false;
    }
    impl_->fmt.ctx = fmt_ctx;

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        out_error = std::string("avformat_find_stream_info failed: ") + av_err2str(ret);
        Close();
        return false;
    }

    // Locate the best video and audio streams (av_find_best_stream picks the
    // most likely primary stream of each type -- correct for our own
    // single-video-track, up-to-N-audio-track recordings).
    const int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_idx < 0) {
        out_error = "no decodable video stream found";
        Close();
        return false;
    }

    AVStream* vst = fmt_ctx->streams[video_idx];
    const AVCodec* vcodec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (vcodec == nullptr) {
        out_error = "no decoder available for the video codec";
        Close();
        return false;
    }
    AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
    if (vctx == nullptr || avcodec_parameters_to_context(vctx, vst->codecpar) < 0 || avcodec_open2(vctx, vcodec, nullptr) < 0) {
        if (vctx)
            avcodec_free_context(&vctx);
        out_error = "failed to open the video decoder";
        Close();
        return false;
    }
    impl_->video_codec.ctx = vctx;
    impl_->video_stream_idx = video_idx;
    impl_->matrix = MapMatrix(vst->codecpar->color_space);
    impl_->range = MapRange(vst->codecpar->color_range);

    if (audio_idx >= 0) {
        AVStream* ast = fmt_ctx->streams[audio_idx];
        const AVCodec* acodec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (acodec != nullptr) {
            AVCodecContext* actx = avcodec_alloc_context3(acodec);
            if (actx != nullptr && avcodec_parameters_to_context(actx, ast->codecpar) >= 0 &&
                avcodec_open2(actx, acodec, nullptr) >= 0) {
                impl_->audio_codec.ctx = actx;
                impl_->audio_stream_idx = audio_idx;
            } else if (actx) {
                avcodec_free_context(&actx);
                LogWarn("failed to open the audio decoder -- continuing video-only");
            }
        } else {
            LogWarn("no decoder available for the audio codec -- continuing video-only");
        }
    }

    return true;
}

void EditPlayerEngine::Close() {
    StopPlaybackDecode();
    if (impl_->video_codec.ctx)
        avcodec_free_context(&impl_->video_codec.ctx);
    if (impl_->audio_codec.ctx)
        avcodec_free_context(&impl_->audio_codec.ctx);
    if (impl_->fmt.ctx)
        avformat_close_input(&impl_->fmt.ctx);
    impl_->video_stream_idx = -1;
    impl_->audio_stream_idx = -1;
}

bool EditPlayerEngine::HasVideoStream() const noexcept {
    return impl_->IsOpen() && impl_->video_stream_idx >= 0;
}

bool EditPlayerEngine::HasAudioStream() const noexcept {
    return impl_->IsOpen() && impl_->audio_stream_idx >= 0;
}

namespace {

// Decodes forward from the decoder's current position until a frame with
// pts_us >= target_us is produced (or EOF). Shared by DecodeFrameAt (Task 5)
// and the continuous playback loop (Task 6, which calls the same primitive
// per GOP boundary). Returns nullopt on EOF/failure.
std::optional<DecodedVideoFrame> DecodeForwardToTarget(AVFormatContext* fmt_ctx, AVCodecContext* vctx,
                                                       int video_stream_idx, int64_t target_us,
                                                       MatrixCoefficients matrix, ColorRange range) {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (pkt == nullptr || frame == nullptr) {
        if (pkt)
            av_packet_free(&pkt);
        if (frame)
            av_frame_free(&frame);
        return std::nullopt;
    }

    std::optional<DecodedVideoFrame> result;
    const AVRational tb = fmt_ctx->streams[video_stream_idx]->time_base;

    while (true) {
        const int read_ret = av_read_frame(fmt_ctx, pkt);
        if (read_ret < 0) {
            avcodec_send_packet(vctx, nullptr); // flush
        } else if (pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt);
            continue;
        } else {
            avcodec_send_packet(vctx, pkt);
            av_packet_unref(pkt);
        }

        for (;;) {
            const int recv_ret = avcodec_receive_frame(vctx, frame);
            if (recv_ret == AVERROR(EAGAIN)) {
                break; // need more input
            }
            if (recv_ret < 0) {
                // EOF or real decode error: stop either way.
                av_packet_free(&pkt);
                av_frame_free(&frame);
                return result; // last frame decoded before EOF, if any
            }

            const int64_t pts_us = av_rescale_q(frame->pts, tb, AVRational{1, AV_TIME_BASE});

            if (frame->width > 0 && frame->height > 0 &&
                (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUV420P10LE)) {
                FullPlanarYuv420Frame src;
                src.y_plane = frame->data[0];
                src.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
                src.u_plane = frame->data[1];
                src.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
                src.v_plane = frame->data[2];
                src.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);
                src.width = static_cast<uint32_t>(frame->width);
                src.height = static_cast<uint32_t>(frame->height);
                src.bits_per_sample = (frame->format == AV_PIX_FMT_YUV420P10LE) ? 10u : 8u;

                YuvToBgraParams params;
                params.matrix = matrix;
                params.range = range;

                DecodedVideoFrame out;
                out.pts_us = pts_us;
                out.width = src.width;
                out.height = src.height;
                out.stride_bytes = src.width * 4u;
                auto bgra = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(out.stride_bytes) * out.height);
                ConvertFullPlanarYuv420ToBgra(src, params, bgra->data(), out.stride_bytes);
                out.bgra = std::move(bgra);
                result = std::move(out);
            }

            if (pts_us >= target_us) {
                av_packet_free(&pkt);
                av_frame_free(&frame);
                return result;
            }
        }

        if (read_ret < 0) {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            return result; // reached EOF while flushing
        }
    }
}

} // namespace

std::optional<DecodedVideoFrame> EditPlayerEngine::DecodeFrameAt(int64_t target_us) {
    if (!impl_->IsOpen() || impl_->video_stream_idx < 0)
        return std::nullopt;

    AVFormatContext* fmt_ctx = impl_->fmt.ctx;
    AVCodecContext* vctx = impl_->video_codec.ctx;
    AVStream* vst = fmt_ctx->streams[impl_->video_stream_idx];

    const int64_t seek_ts = av_rescale_q(target_us, AVRational{1, AV_TIME_BASE}, vst->time_base);
    const int seek_ret = av_seek_frame(fmt_ctx, impl_->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0) {
        LogWarn((std::string("av_seek_frame failed: ") + av_err2str(seek_ret)).c_str());
        return std::nullopt;
    }
    avcodec_flush_buffers(vctx);

    return DecodeForwardToTarget(fmt_ctx, vctx, impl_->video_stream_idx, target_us, impl_->matrix, impl_->range);
}

// Real bodies added in Task 6; safe no-ops here so the class already links
// and behaves correctly for the scrub-only (DecodeFrameAt) path.
void EditPlayerEngine::StartPlaybackDecode(int64_t /*start_us*/, VideoFrameCallback /*on_video*/,
                                           AudioBlockCallback /*on_audio*/) {
}

void EditPlayerEngine::StopPlaybackDecode() {
}

} // namespace recorder_core
```

- [ ] **Step 5: Run the tests and confirm they pass**

```powershell
cmake --build --preset windows-x64-debug --target test_edit_player_engine
pwsh scripts/run-tests.ps1 -Filter test_edit_player_engine
```

Expected: PASS, all 4 cases.

- [ ] **Step 6: Add the new files to the `recorder_core` library target**

In `libs/recorder_core/CMakeLists.txt`, add to the `add_library(recorder_core STATIC ...)` list
(around the existing `mp4_remuxer.h`/`.cpp` entries):

```cmake
    include/recorder_core/edit_player_engine.h
    src/edit_player_engine.cpp
    src/playback_clock.h
    src/playback_clock.cpp
```

Rebuild the full library to confirm no conflicts:

```powershell
cmake --build --preset windows-x64-debug --target recorder_core
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/include/recorder_core/edit_player_engine.h \
        libs/recorder_core/src/edit_player_engine.cpp \
        libs/recorder_core/tests/test_edit_player_engine.cpp \
        libs/recorder_core/CMakeLists.txt
git commit -m "Add EditPlayerEngine: open/close, color-metadata mapping, single-frame seek decode"
```

---

## Task 6: `EditPlayerEngine` — continuous playback decode thread

Adds the real bodies for `StartPlaybackDecode`/`StopPlaybackDecode`: a background thread that
decodes video AND audio forward continuously from a start position, delivering both via callbacks,
until stopped. Audio is resampled to the engine's fixed 48 kHz stereo float32 output format via
`swresample` (mirrors `OutputFormatAudioSrc::BuildSwrContext`'s exact API usage).

**Files:**
- Modify: `libs/recorder_core/src/edit_player_engine.cpp`
- Modify: `libs/recorder_core/tests/test_edit_player_engine.cpp`

**Interfaces:**
- Consumes: Task 5's `EditPlayerEngine::Impl`, `DecodeForwardToTarget`.
- Produces: working `StartPlaybackDecode`/`StopPlaybackDecode` — consumed by Task 8's
  `EditPlayerSession`.

- [ ] **Step 1: Write the failing test (thread lifecycle only)**

Add to `libs/recorder_core/tests/test_edit_player_engine.cpp`, inside the anonymous namespace:

```cpp
TEST(EditPlayerEngine, StartStopPlaybackDecodeWithoutOpenIsSafeNoOp) {
    EditPlayerEngine engine;
    std::atomic<int> video_calls{0};
    engine.StartPlaybackDecode(0, [&](recorder_core::DecodedVideoFrame) { ++video_calls; },
                               [](recorder_core::DecodedAudioBlock) {});
    engine.StopPlaybackDecode();
    EXPECT_EQ(video_calls.load(), 0);
}

TEST(EditPlayerEngine, StopPlaybackDecodeWithoutStartIsSafeNoOp) {
    EditPlayerEngine engine;
    engine.StopPlaybackDecode(); // must not crash / hang
    SUCCEED();
}
```

Add `#include <atomic>` near the top of the test file's includes.

(No "destructor joins a REAL running decode thread" test is written here: `StartPlaybackDecode`
returns immediately without spawning a thread unless `impl_->IsOpen()` — i.e. `Open()` already
succeeded against a real, decodable file, which (per Task 5's own testing note) cannot be
synthesized in this test environment. A test that called `StartPlaybackDecode` on an un-opened
engine and asserted "no hang" would be asserting the exact same no-op path the two tests above
already cover, just under a misleading name. The real thread-join-while-playing path is exercised
live in Task 11's live-verify pass, using an actual recorded file.)

- [ ] **Step 2: Run and confirm they compile+pass against the current no-op bodies**

```powershell
cmake --build --preset windows-x64-debug --target test_edit_player_engine
pwsh scripts/run-tests.ps1 -Filter test_edit_player_engine
```

Expected: PASS (the Task 5 no-op bodies already satisfy these three cases — this step just proves
the lifecycle contract before the real implementation replaces the no-op).

- [ ] **Step 3: Implement the real playback thread**

In `libs/recorder_core/src/edit_player_engine.cpp`, add `#include <libswresample/swresample.h>`
to the `extern "C"` block at the top, and add this RAII guard next to `CodecCtxGuard`:

```cpp
struct SwrCtxGuard {
    SwrContext* ctx = nullptr;
    ~SwrCtxGuard() {
        if (ctx)
            swr_free(&ctx);
    }
};
```

Add `SwrCtxGuard resampler;` as a member of `Impl` (next to `playback_mutex`).

Replace the two no-op bodies at the bottom of the file with:

```cpp
namespace {

constexpr uint32_t kPlaybackOutSampleRate = 48000;
constexpr uint32_t kPlaybackOutChannels = 2;

} // namespace

void EditPlayerEngine::StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio) {
    std::lock_guard<std::mutex> lock(impl_->playback_mutex);
    if (!impl_->IsOpen() || impl_->playback_running.load())
        return;

    impl_->playback_cancel.store(false);
    impl_->playback_running.store(true);

    impl_->playback_thread = std::thread([this, start_us, on_video = std::move(on_video), on_audio = std::move(on_audio)]() {
        AVFormatContext* fmt_ctx = impl_->fmt.ctx;
        AVCodecContext* vctx = impl_->video_codec.ctx;
        AVCodecContext* actx = impl_->audio_codec.ctx;

        // Seek every stream to the start position before decoding forward.
        if (impl_->video_stream_idx >= 0) {
            AVStream* vst = fmt_ctx->streams[impl_->video_stream_idx];
            const int64_t seek_ts = av_rescale_q(start_us, AVRational{1, AV_TIME_BASE}, vst->time_base);
            av_seek_frame(fmt_ctx, impl_->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(vctx);
        }
        if (actx != nullptr)
            avcodec_flush_buffers(actx);

        // Set up the audio resampler (decoder's native format -> 48kHz stereo float32).
        if (actx != nullptr) {
            AVChannelLayout out_layout{};
            av_channel_layout_default(&out_layout, static_cast<int>(kPlaybackOutChannels));
            SwrContext* swr = nullptr;
            const int swr_ret = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT,
                                                    static_cast<int>(kPlaybackOutSampleRate), &actx->ch_layout,
                                                    actx->sample_fmt, actx->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&out_layout);
            if (swr_ret >= 0 && swr != nullptr && swr_init(swr) >= 0) {
                impl_->resampler.ctx = swr;
            } else if (swr != nullptr) {
                swr_free(&swr);
                LogWarn("audio resampler init failed -- continuing video-only for this playback session");
            }
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        while (!impl_->playback_cancel.load()) {
            const int read_ret = av_read_frame(fmt_ctx, pkt);
            if (read_ret < 0) {
                if (vctx)
                    avcodec_send_packet(vctx, nullptr);
                if (actx)
                    avcodec_send_packet(actx, nullptr);
            } else if (pkt->stream_index == impl_->video_stream_idx && vctx != nullptr) {
                avcodec_send_packet(vctx, pkt);
            } else if (pkt->stream_index == impl_->audio_stream_idx && actx != nullptr) {
                avcodec_send_packet(actx, pkt);
            }
            if (read_ret >= 0)
                av_packet_unref(pkt);

            // Drain video frames.
            if (vctx != nullptr) {
                const AVRational tb = fmt_ctx->streams[impl_->video_stream_idx]->time_base;
                for (;;) {
                    const int recv_ret = avcodec_receive_frame(vctx, frame);
                    if (recv_ret < 0)
                        break;
                    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUV420P10LE) {
                        FullPlanarYuv420Frame src;
                        src.y_plane = frame->data[0];
                        src.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
                        src.u_plane = frame->data[1];
                        src.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
                        src.v_plane = frame->data[2];
                        src.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);
                        src.width = static_cast<uint32_t>(frame->width);
                        src.height = static_cast<uint32_t>(frame->height);
                        src.bits_per_sample = (frame->format == AV_PIX_FMT_YUV420P10LE) ? 10u : 8u;

                        YuvToBgraParams params;
                        params.matrix = impl_->matrix;
                        params.range = impl_->range;

                        DecodedVideoFrame out;
                        out.pts_us = av_rescale_q(frame->pts, tb, AVRational{1, AV_TIME_BASE});
                        out.width = src.width;
                        out.height = src.height;
                        out.stride_bytes = src.width * 4u;
                        auto bgra =
                            std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(out.stride_bytes) * out.height);
                        ConvertFullPlanarYuv420ToBgra(src, params, bgra->data(), out.stride_bytes);
                        out.bgra = std::move(bgra);
                        on_video(std::move(out));
                    }
                    av_frame_unref(frame);
                }
            }

            // Drain audio frames.
            if (actx != nullptr && impl_->resampler.ctx != nullptr) {
                const AVRational tb = fmt_ctx->streams[impl_->audio_stream_idx]->time_base;
                for (;;) {
                    const int recv_ret = avcodec_receive_frame(actx, frame);
                    if (recv_ret < 0)
                        break;
                    const int64_t max_out_frames =
                        swr_get_out_samples(impl_->resampler.ctx, frame->nb_samples);
                    auto pcm = std::make_shared<std::vector<float>>(
                        static_cast<size_t>(max_out_frames) * kPlaybackOutChannels);
                    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm->data());
                    const int produced = swr_convert(impl_->resampler.ctx, &out_ptr, static_cast<int>(max_out_frames),
                                                     const_cast<const uint8_t**>(frame->data), frame->nb_samples);
                    av_frame_unref(frame);
                    if (produced <= 0)
                        continue;
                    pcm->resize(static_cast<size_t>(produced) * kPlaybackOutChannels);
                    DecodedAudioBlock block;
                    block.pts_us = av_rescale_q(frame->pts, tb, AVRational{1, AV_TIME_BASE});
                    block.frame_count = static_cast<uint32_t>(produced);
                    block.interleaved_stereo = std::move(pcm);
                    on_audio(std::move(block));
                }
            }

            if (read_ret < 0)
                break; // EOF reached (and flush-drained above)
        }

        av_packet_free(&pkt);
        av_frame_free(&frame);
        impl_->playback_running.store(false);
    });
}

void EditPlayerEngine::StopPlaybackDecode() {
    impl_->playback_cancel.store(true);
    if (impl_->playback_thread.joinable())
        impl_->playback_thread.join();
    impl_->playback_running.store(false);
}
```

Remove the two old no-op bodies this replaces.

- [ ] **Step 4: Run the tests and confirm they pass**

```powershell
cmake --build --preset windows-x64-debug --target test_edit_player_engine
pwsh scripts/run-tests.ps1 -Filter test_edit_player_engine
```

Expected: PASS, all 6 cases (4 from Task 5 + 2 new).

- [ ] **Step 5: Rebuild the full library**

```powershell
cmake --build --preset windows-x64-debug --target recorder_core
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add libs/recorder_core/src/edit_player_engine.cpp libs/recorder_core/tests/test_edit_player_engine.cpp
git commit -m "Add EditPlayerEngine continuous playback decode thread with audio resampling"
```

---

## Task 7: `WasapiAudioRenderer` — new WASAPI render client

No WASAPI render path exists anywhere in the codebase (only capture). New class, styled exactly
like `WasapiCaptureSrc` (raw COM pointers, explicit `Shutdown()`, no smart-pointer wrapper): opens
the system default render endpoint in shared mode, accepts pushed 48 kHz stereo float32 PCM,
resamples to the device's own mix format if it differs, and exposes the render clock (cumulative
frames actually written) that becomes the playback master clock (Task 4's `AudioClockMs`).

**Files:**
- Create: `libs/recorder_core/include/recorder_core/wasapi_audio_render.h`
- Create: `libs/recorder_core/src/wasapi_audio_render.cpp`
- Create: `libs/recorder_core/tests/test_wasapi_audio_render.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt`

**Interfaces:**
- Produces: `recorder_core::WasapiAudioRenderer` with `Init`, `Start`, `Stop`, `PushSamples`,
  `FramesRendered`, `SampleRate`, `Shutdown` — consumed by Task 8's `EditPlayerSession`.

- [ ] **Step 1: Write the failing test (pure ring-buffer math only, no real device)**

Create `libs/recorder_core/tests/test_wasapi_audio_render.cpp`:

```cpp
#include <gtest/gtest.h>

#include "recorder_core/wasapi_audio_render.h"

namespace {

using recorder_core::WasapiAudioRenderer;

// Construction/destruction without Init() must be safe -- no COM object was
// ever created, Shutdown() (called from the destructor) must handle that.
TEST(WasapiAudioRenderer, ConstructDestructWithoutInitIsSafe) {
    WasapiAudioRenderer renderer;
    SUCCEED();
}

TEST(WasapiAudioRenderer, FramesRenderedStartsAtZero) {
    WasapiAudioRenderer renderer;
    EXPECT_EQ(renderer.FramesRendered(), 0u);
}

TEST(WasapiAudioRenderer, PushSamplesWithoutInitIsSafeNoOp) {
    WasapiAudioRenderer renderer;
    const std::vector<float> silence(200, 0.0f); // 100 stereo frames
    renderer.PushSamples(silence.data(), 100);
    SUCCEED();
}

TEST(WasapiAudioRenderer, StopWithoutStartIsSafeNoOp) {
    WasapiAudioRenderer renderer;
    renderer.Stop();
    SUCCEED();
}

} // namespace
```

(A real `Init()` against the actual default render endpoint requires real audio hardware/session —
excluded from CI per this repo's existing convention for hardware-touching WASAPI code; live
verification happens per Task 11.)

- [ ] **Step 2: Register the test target and confirm it fails**

Add to `libs/recorder_core/CMakeLists.txt`:

```cmake
# WasapiAudioRenderer lifecycle safety (construct/destruct/push/stop without a
# real device session). No hardware in CI -- real render is user-live-verified.
exosnap_add_gtest(
    NAME test_wasapi_audio_render
    TEST_PREFIX recorder_core.
    SOURCES tests/test_wasapi_audio_render.cpp
            src/wasapi_audio_render.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_wasapi_audio_render PRIVATE src include)
```

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target test_wasapi_audio_render
```

Expected: FAIL — header does not exist yet.

- [ ] **Step 3: Create the public header**

Create `libs/recorder_core/include/recorder_core/wasapi_audio_render.h`:

```cpp
#pragma once

// WasapiAudioRenderer -- the Edit-page video player's audio-out path and
// playback master clock (docs/superpowers/specs/2026-07-14-edit-video-player-
// design.md). No WASAPI render path existed anywhere in this codebase before
// this class; only capture (WasapiCaptureSrc, wasapi_loopback.cpp) did.
//
// Opens the system default render endpoint (eRender/eConsole -- no in-app
// device picker, matching the design's scope decision) in shared mode.
// PushSamples() accepts 48 kHz stereo interleaved float32 PCM (the same fixed
// format EditPlayerEngine's playback decode produces) from any thread; an
// internal ring buffer plus an event-driven render callback thread write it
// to the device, resampling to the device's own mix format first if it
// differs from 48kHz/stereo/float32.
//
// FramesRendered() (cumulative frames actually written to the device) is the
// playback master clock -- feed it to AudioClockMs() (playback_clock.h).

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;
// Forward-declared at GLOBAL scope deliberately (not inside namespace
// recorder_core below): the real definition lives in libswresample's
// swresample.h, which this public header must not include (keeps FFmpeg out
// of the public API surface). Writing `struct SwrContext* resampler_` INSIDE
// the recorder_core namespace block instead would declare a distinct
// recorder_core::SwrContext type via elaborated-type-specifier injection --
// a different type from the real ::SwrContext the .cpp's reinterpret_cast
// needs to match. Declaring it here, before the namespace, ensures both this
// header and wasapi_audio_render.cpp's casts refer to the same global type.
struct SwrContext;

namespace recorder_core {

class WasapiAudioRenderer {
  public:
    WasapiAudioRenderer();
    ~WasapiAudioRenderer();

    WasapiAudioRenderer(const WasapiAudioRenderer&) = delete;
    WasapiAudioRenderer& operator=(const WasapiAudioRenderer&) = delete;

    // Opens the system default render endpoint in shared mode. Returns false
    // with a message in out_error on failure (no default device, format
    // negotiation failure).
    bool Init(std::string& out_error);

    // Starts/stops the render callback thread. Init() must have succeeded.
    // No-op (not an error) if not initialized or already in the requested state.
    void Start();
    void Stop();

    // Appends `frame_count` stereo frames (frame_count * 2 floats,
    // interleaved L,R) to the internal ring buffer. Safe to call from any
    // thread. No-op if not initialized.
    void PushSamples(const float* interleaved_stereo, uint32_t frame_count);

    // Cumulative frames actually written to the render endpoint so far --
    // the playback master clock. 0 before Init()/Start().
    [[nodiscard]] uint64_t FramesRendered() const noexcept;

    // The render endpoint's actual sample rate (post-Init; 0 before Init()).
    [[nodiscard]] uint32_t SampleRate() const noexcept;

    // Stops the render thread, releases the COM device/client objects, and
    // clears the ring buffer. Safe to call multiple times / without Init().
    void Shutdown();

  private:
    void RenderThreadMain();

    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    void* buffer_event_ = nullptr; // HANDLE, opaque here to keep windows.h out of this header
    SwrContext* resampler_ = nullptr; // opaque (::SwrContext, forward-declared above), only used if
                                      // the device format != 48k/stereo/float32

    uint32_t device_sample_rate_ = 0;
    uint32_t device_channels_ = 0;
    uint32_t device_bytes_per_sample_ = 4; // bytes/sample in device_data buffers (float32 default, 2 if int16)
    uint32_t buffer_frame_count_ = 0;

    std::thread render_thread_;
    std::atomic<bool> running_{false};

    std::mutex ring_mutex_;
    std::deque<float> ring_; // interleaved stereo float32 @ 48kHz, pre-device-resample

    std::atomic<uint64_t> frames_rendered_{0};

    bool initialized_ = false;
};

} // namespace recorder_core
```

- [ ] **Step 4: Implement**

Create `libs/recorder_core/src/wasapi_audio_render.cpp`:

```cpp
#include "recorder_core/wasapi_audio_render.h"

#include "recorder_core/logging/logging.h"

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

extern "C" {
#include <libswresample/swresample.h>
}

#include <algorithm>

namespace recorder_core {

namespace {
constexpr const char* kLogComponent = "wasapi_audio_render";
constexpr uint32_t kEngineSampleRate = 48000;
constexpr uint32_t kEngineChannels = 2;
constexpr REFERENCE_TIME kBufferDurationHns = 2000000; // 200 ms shared-mode buffer

void LogError(const char* msg) {
    logging::log(logging::LogLevel::Error, kLogComponent, msg);
}
void LogWarn(const char* msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
}
} // namespace

WasapiAudioRenderer::WasapiAudioRenderer() = default;

WasapiAudioRenderer::~WasapiAudioRenderer() {
    Shutdown();
}

bool WasapiAudioRenderer::Init(std::string& out_error) {
    Shutdown(); // tear down any previous session first

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized_here = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        out_error = "CoInitializeEx failed";
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        out_error = "CoCreateInstance(MMDeviceEnumerator) failed";
        if (com_initialized_here)
            CoUninitialize();
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    enumerator->Release();
    if (FAILED(hr) || device_ == nullptr) {
        out_error = "no default audio render endpoint";
        if (com_initialized_here)
            CoUninitialize();
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || audio_client_ == nullptr) {
        out_error = "IMMDevice::Activate(IAudioClient) failed";
        Shutdown();
        return false;
    }

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr) || mix_format == nullptr) {
        out_error = "IAudioClient::GetMixFormat failed";
        Shutdown();
        return false;
    }
    device_sample_rate_ = mix_format->nSamplesPerSec;
    device_channels_ = mix_format->nChannels;
    device_bytes_per_sample_ = 4; // default float32; corrected below if the device format differs

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDurationHns, 0,
                                   mix_format, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mix_format);
        out_error = "IAudioClient::Initialize failed";
        Shutdown();
        return false;
    }

    // Resampler: engine format (48k/stereo/float32) -> the device's actual
    // mix format, only allocated if they differ.
    if (device_sample_rate_ != kEngineSampleRate || device_channels_ != kEngineChannels ||
        mix_format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) {
        AVSampleFormat out_fmt = AV_SAMPLE_FMT_FLT;
        if (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_format);
            out_fmt = (ext->SubFormat.Data1 == 3 /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */) ? AV_SAMPLE_FMT_FLT
                                                                                        : AV_SAMPLE_FMT_S16;
        } else if (mix_format->wFormatTag == WAVE_FORMAT_PCM) {
            out_fmt = AV_SAMPLE_FMT_S16;
        }
        // The render thread's silence-padding math (RenderThreadMain) needs the
        // ACTUAL bytes-per-sample the resampler is about to write, not an
        // assumed float32 -- a PCM/int16 device would otherwise get the wrong
        // padding stride.
        device_bytes_per_sample_ = (out_fmt == AV_SAMPLE_FMT_S16) ? 2u : 4u;

        AVChannelLayout in_layout{}, out_layout{};
        av_channel_layout_default(&in_layout, static_cast<int>(kEngineChannels));
        av_channel_layout_default(&out_layout, static_cast<int>(device_channels_));
        SwrContext* swr = nullptr;
        const int swr_ret = swr_alloc_set_opts2(&swr, &out_layout, out_fmt, static_cast<int>(device_sample_rate_),
                                                &in_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(kEngineSampleRate), 0,
                                                nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (swr_ret < 0 || swr == nullptr || swr_init(swr) < 0) {
            if (swr)
                swr_free(&swr);
            CoTaskMemFree(mix_format);
            out_error = "audio render resampler init failed";
            Shutdown();
            return false;
        }
        resampler_ = reinterpret_cast<struct SwrContext*>(swr);
    }
    CoTaskMemFree(mix_format);

    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr)) {
        out_error = "IAudioClient::GetBufferSize failed";
        Shutdown();
        return false;
    }

    buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (buffer_event_ == nullptr) {
        out_error = "CreateEventW failed";
        Shutdown();
        return false;
    }
    hr = audio_client_->SetEventHandle(static_cast<HANDLE>(buffer_event_));
    if (FAILED(hr)) {
        out_error = "IAudioClient::SetEventHandle failed";
        Shutdown();
        return false;
    }

    hr = audio_client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr) || render_client_ == nullptr) {
        out_error = "IAudioClient::GetService(IAudioRenderClient) failed";
        Shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void WasapiAudioRenderer::Start() {
    if (!initialized_ || running_.load())
        return;
    frames_rendered_.store(0);
    running_.store(true);
    audio_client_->Start();
    render_thread_ = std::thread(&WasapiAudioRenderer::RenderThreadMain, this);
}

void WasapiAudioRenderer::Stop() {
    if (!running_.load())
        return;
    running_.store(false);
    if (buffer_event_ != nullptr)
        SetEvent(static_cast<HANDLE>(buffer_event_)); // wake the render thread so it can observe running_==false
    if (render_thread_.joinable())
        render_thread_.join();
    if (audio_client_ != nullptr)
        audio_client_->Stop();
}

void WasapiAudioRenderer::PushSamples(const float* interleaved_stereo, uint32_t frame_count) {
    if (!initialized_ || interleaved_stereo == nullptr || frame_count == 0)
        return;
    std::lock_guard<std::mutex> lock(ring_mutex_);
    ring_.insert(ring_.end(), interleaved_stereo, interleaved_stereo + static_cast<size_t>(frame_count) * kEngineChannels);
}

uint64_t WasapiAudioRenderer::FramesRendered() const noexcept {
    return frames_rendered_.load();
}

uint32_t WasapiAudioRenderer::SampleRate() const noexcept {
    return device_sample_rate_;
}

void WasapiAudioRenderer::RenderThreadMain() {
    // Pro-audio thread priority, matching the capture side's convention of
    // treating the WASAPI event thread as latency-critical.
    HANDLE task_handle = nullptr;
    DWORD task_index = 0;
    task_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    std::vector<float> engine_buf; // pulled from ring_, engine format
    std::vector<uint8_t> device_buf; // resampled to device format, if needed

    while (running_.load()) {
        const DWORD wait_ret = WaitForSingleObject(static_cast<HANDLE>(buffer_event_), 200);
        if (!running_.load())
            break;
        if (wait_ret != WAIT_OBJECT_0)
            continue;

        UINT32 padding_frames = 0;
        if (FAILED(audio_client_->GetCurrentPadding(&padding_frames)))
            continue;
        const UINT32 available_frames = buffer_frame_count_ - padding_frames;
        if (available_frames == 0)
            continue;

        // Pull up to available_frames worth of engine-format samples out of the ring.
        const size_t want_floats = static_cast<size_t>(available_frames) * kEngineChannels;
        engine_buf.clear();
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            const size_t take = std::min(want_floats, ring_.size());
            engine_buf.assign(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
            ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
        }
        const uint32_t engine_frames = static_cast<uint32_t>(engine_buf.size() / kEngineChannels);

        BYTE* device_data = nullptr;
        if (FAILED(render_client_->GetBuffer(available_frames, &device_data)))
            continue;

        if (engine_frames == 0) {
            // Nothing decoded yet (e.g. paused/starved) -- render silence rather
            // than blocking, so the device clock (and playback clock derived
            // from it) keeps advancing honestly instead of stalling.
            render_client_->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        } else if (resampler_ == nullptr) {
            const size_t copy_bytes = std::min<size_t>(engine_buf.size() * sizeof(float),
                                                        static_cast<size_t>(available_frames) * kEngineChannels *
                                                            sizeof(float));
            memcpy(device_data, engine_buf.data(), copy_bytes);
            if (engine_frames < available_frames) {
                memset(device_data + copy_bytes, 0,
                      (static_cast<size_t>(available_frames) - engine_frames) * kEngineChannels * sizeof(float));
            }
            render_client_->ReleaseBuffer(available_frames, 0);
        } else {
            auto* swr = reinterpret_cast<SwrContext*>(resampler_);
            const uint8_t* in_ptr = reinterpret_cast<const uint8_t*>(engine_buf.data());
            const int produced = swr_convert(swr, &device_data, static_cast<int>(available_frames), &in_ptr,
                                             static_cast<int>(engine_frames));
            if (produced >= 0 && static_cast<uint32_t>(produced) < available_frames) {
                // Pad any shortfall with silence rather than leaving garbage, using the
                // ACTUAL device sample format's byte stride (set in Init() -- float32 or
                // int16, whichever the resampler was configured to emit), not an assumed one.
                const size_t device_frame_bytes = static_cast<size_t>(device_channels_) * device_bytes_per_sample_;
                uint8_t* tail = device_data + static_cast<size_t>(produced) * device_frame_bytes;
                const size_t tail_bytes =
                    (static_cast<size_t>(available_frames) - static_cast<size_t>(produced)) * device_frame_bytes;
                memset(tail, 0, tail_bytes);
            }
            render_client_->ReleaseBuffer(available_frames, produced > 0 ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
        }

        frames_rendered_.fetch_add(available_frames);
    }

    if (task_handle != nullptr)
        AvRevertMmThreadCharacteristics(task_handle);
}

void WasapiAudioRenderer::Shutdown() {
    Stop();
    if (buffer_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(buffer_event_));
        buffer_event_ = nullptr;
    }
    if (resampler_ != nullptr) {
        auto* swr = reinterpret_cast<SwrContext*>(resampler_);
        swr_free(&swr);
        resampler_ = nullptr;
    }
    if (render_client_ != nullptr) {
        render_client_->Release();
        render_client_ = nullptr;
    }
    if (audio_client_ != nullptr) {
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
    device_sample_rate_ = 0;
    device_channels_ = 0;
    device_bytes_per_sample_ = 4;
    buffer_frame_count_ = 0;
    initialized_ = false;
}

} // namespace recorder_core
```

Note: `AvSetMmThreadCharacteristicsW`/`AvRevertMmThreadCharacteristics` need `avrt.lib` — add it in
Step 6 below.

- [ ] **Step 5: Run the tests and confirm they pass**

```powershell
cmake --build --preset windows-x64-debug --target test_wasapi_audio_render
pwsh scripts/run-tests.ps1 -Filter test_wasapi_audio_render
```

Expected: PASS, all 4 cases (none of them call `Init()`, so no real device is touched).

- [ ] **Step 6: Add the new files + `avrt.lib` to the `recorder_core` library target**

In `libs/recorder_core/CMakeLists.txt`, add to the source list:

```cmake
    include/recorder_core/wasapi_audio_render.h
    src/wasapi_audio_render.cpp
```

and add `avrt.lib` to the existing `target_link_libraries(recorder_core PRIVATE ...)` list (next to
the other system libs like `ole32.lib`).

```powershell
cmake --build --preset windows-x64-debug --target recorder_core
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/include/recorder_core/wasapi_audio_render.h \
        libs/recorder_core/src/wasapi_audio_render.cpp \
        libs/recorder_core/tests/test_wasapi_audio_render.cpp \
        libs/recorder_core/CMakeLists.txt
git commit -m "Add WasapiAudioRenderer: the Edit-page video player's audio-out and master clock"
```

---

## Task 8: `EditPlayerSession` — orchestrator

Owns one `EditPlayerEngine` + one `WasapiAudioRenderer`, wires them together with Task 4's
`PlaybackClock` logic, and exposes the small Play/Pause/Seek surface the app layer drives. Falls
back to no audio renderer at all when the file has no audio stream (per the design's fallback —
the app layer is responsible for driving the wall-clock fallback in that case; this class just
truthfully reports `HasAudioStream()`).

**Files:**
- Create: `libs/recorder_core/include/recorder_core/edit_player_session.h`
- Create: `libs/recorder_core/src/edit_player_session.cpp`
- Create: `libs/recorder_core/tests/test_edit_player_session.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt`

**Interfaces:**
- Consumes: `recorder_core::EditPlayerEngine` (Task 5/6), `recorder_core::WasapiAudioRenderer`
  (Task 7), `recorder_core::AudioClockMs`/`SelectFrameForClock` (Task 4).
- Produces: `recorder_core::EditPlayerSession` with `Open`, `Close`, `HasAudioStream`, `Play`,
  `Pause`, `SeekTo`, `SetOnFrameReady(std::function<void(DecodedVideoFrame)>)` — consumed by
  Task 10 (`EditExportPage` wiring).

- [ ] **Step 1: Write the failing tests**

Create `libs/recorder_core/tests/test_edit_player_session.cpp`:

```cpp
#include <gtest/gtest.h>

#include "recorder_core/edit_player_session.h"

#include <filesystem>

namespace {

using recorder_core::EditPlayerSession;

TEST(EditPlayerSession, OpenNonexistentFileFails) {
    EditPlayerSession session;
    std::string err;
    EXPECT_FALSE(session.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
}

TEST(EditPlayerSession, ClosedSessionReportsNoAudioStream) {
    EditPlayerSession session;
    EXPECT_FALSE(session.HasAudioStream());
}

TEST(EditPlayerSession, PlayPauseSeekWithoutOpenAreSafeNoOps) {
    EditPlayerSession session;
    session.Play();
    session.Pause();
    session.SeekTo(0);
    SUCCEED();
}

TEST(EditPlayerSession, CloseWithoutOpenIsSafeNoOp) {
    EditPlayerSession session;
    session.Close();
    SUCCEED();
}

} // namespace
```

- [ ] **Step 2: Register the test target and confirm it fails**

Add to `libs/recorder_core/CMakeLists.txt`:

```cmake
exosnap_add_gtest(
    NAME test_edit_player_session
    TEST_PREFIX recorder_core.
    SOURCES tests/test_edit_player_session.cpp
    LIBRARIES recorder_core
)
target_include_directories(test_edit_player_session PRIVATE src include)
```

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target test_edit_player_session
```

Expected: FAIL — header does not exist.

- [ ] **Step 3: Create the public header**

Create `libs/recorder_core/include/recorder_core/edit_player_session.h`:

```cpp
#pragma once

// EditPlayerSession -- top-level orchestrator for the Edit-page video player
// (docs/superpowers/specs/2026-07-14-edit-video-player-design.md). Owns one
// EditPlayerEngine (decode) and, when the file has an audio stream, one
// WasapiAudioRenderer (playback + master clock). UI-agnostic per CLAUDE.md.
//
// Ownership split with the app layer: this class does NOT know about Qt
// timers. When HasAudioStream() is false, the caller (EditExportPage) is
// responsible for driving playback position from its own existing wall-clock
// timer and calling SeekTo() to request frames at that position -- this
// mirrors the design's documented fallback (the existing preview_elapsed_/
// onPreviewTick logic becomes the real fallback path, not a Qt concern this
// class needs to duplicate).

#include <recorder_core/edit_player_engine.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace recorder_core {

class EditPlayerSession {
  public:
    EditPlayerSession();
    ~EditPlayerSession();

    EditPlayerSession(const EditPlayerSession&) = delete;
    EditPlayerSession& operator=(const EditPlayerSession&) = delete;

    // Opens `path` (the MKV edit master) and, if it has an audio stream,
    // initializes the WASAPI render client. Returns false with a message in
    // out_error if the file cannot be opened (audio-renderer init failure is
    // non-fatal here -- logged and the session falls back to HasAudioStream()
    // == false, matching "no audio stream" behavior, per the design's stance
    // that a broken audio device should degrade to silent video, not break
    // the whole preview).
    bool Open(const std::filesystem::path& path, std::string& out_error);
    void Close();

    [[nodiscard]] bool HasAudioStream() const noexcept;

    // Sets the callback invoked (from an internal thread -- NOT the caller's
    // thread) whenever a new frame is ready to display, during both
    // continuous playback and single-frame scrub/trim-drag seeks.
    void SetOnFrameReady(std::function<void(DecodedVideoFrame)> callback);

    // Starts continuous playback (decode thread + audio renderer, if
    // present) from the current position. No-op if not open.
    void Play();

    // Pauses continuous playback. No-op if not open or not playing.
    void Pause();

    // Requests a single frame at target_us (scrub / trim-handle-drag path).
    // If currently playing, this pauses playback first (matching the
    // existing UI contract: scrubbing pauses, resume-on-release is the
    // caller's job, same as today's onScrubStarted/onScrubFinished). A newer
    // SeekTo() call supersedes an in-flight older one.
    void SeekTo(int64_t target_us);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core
```

- [ ] **Step 4: Implement**

Create `libs/recorder_core/src/edit_player_session.cpp`:

```cpp
#include "recorder_core/edit_player_session.h"

#include "recorder_core/wasapi_audio_render.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace recorder_core {

struct EditPlayerSession::Impl {
    EditPlayerEngine engine;
    WasapiAudioRenderer audio;
    bool has_audio = false;
    bool playing = false;

    std::function<void(DecodedVideoFrame)> on_frame;
    std::mutex callback_mutex; // guards on_frame against concurrent Set/invoke

    // Scrub seek: only ever one in flight. A new SeekTo() bumps the
    // generation counter; the worker thread checks it before delivering a
    // frame so a superseded seek's result is silently dropped -- the
    // "throttled, live" scrub behavior from the design.
    std::atomic<uint64_t> seek_generation{0};
    std::thread seek_thread;
    std::mutex seek_thread_mutex;

    void DeliverFrame(DecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        if (on_frame)
            on_frame(std::move(frame));
    }
};

EditPlayerSession::EditPlayerSession() : impl_(std::make_unique<Impl>()) {
}

EditPlayerSession::~EditPlayerSession() {
    Close();
}

bool EditPlayerSession::Open(const std::filesystem::path& path, std::string& out_error) {
    Close();

    if (!impl_->engine.Open(path, out_error))
        return false;

    impl_->has_audio = impl_->engine.HasAudioStream();
    if (impl_->has_audio) {
        std::string audio_err;
        if (!impl_->audio.Init(audio_err)) {
            // Non-fatal: degrade to silent-video, matching the "no audio
            // stream" fallback contract rather than failing the whole open.
            impl_->has_audio = false;
        }
    }
    return true;
}

void EditPlayerSession::Close() {
    Pause();
    {
        std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
        impl_->seek_generation.fetch_add(1); // supersede any in-flight seek
        if (impl_->seek_thread.joinable())
            impl_->seek_thread.join();
    }
    impl_->audio.Shutdown();
    impl_->engine.Close();
    impl_->has_audio = false;
}

bool EditPlayerSession::HasAudioStream() const noexcept {
    return impl_->has_audio;
}

void EditPlayerSession::SetOnFrameReady(std::function<void(DecodedVideoFrame)> callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mutex);
    impl_->on_frame = std::move(callback);
}

void EditPlayerSession::Play() {
    if (impl_->playing)
        return;
    impl_->playing = true;

    if (impl_->has_audio)
        impl_->audio.Start();

    impl_->engine.StartPlaybackDecode(
        0,
        [this](DecodedVideoFrame frame) { impl_->DeliverFrame(std::move(frame)); },
        [this](DecodedAudioBlock block) {
            if (impl_->has_audio && block.interleaved_stereo)
                impl_->audio.PushSamples(block.interleaved_stereo->data(), block.frame_count);
        });
}

void EditPlayerSession::Pause() {
    if (!impl_->playing)
        return;
    impl_->playing = false;
    impl_->engine.StopPlaybackDecode();
    if (impl_->has_audio)
        impl_->audio.Stop();
}

void EditPlayerSession::SeekTo(int64_t target_us) {
    Pause(); // scrubbing pauses; resume-on-release is the caller's job (matches existing UI contract)

    const uint64_t my_generation = impl_->seek_generation.fetch_add(1) + 1;

    std::lock_guard<std::mutex> lock(impl_->seek_thread_mutex);
    if (impl_->seek_thread.joinable())
        impl_->seek_thread.join(); // the previous seek already saw a bumped generation and is winding down

    impl_->seek_thread = std::thread([this, target_us, my_generation]() {
        auto frame = impl_->engine.DecodeFrameAt(target_us);
        if (frame.has_value() && impl_->seek_generation.load() == my_generation)
            impl_->DeliverFrame(std::move(*frame));
    });
}

} // namespace recorder_core
```

- [ ] **Step 5: Run the tests and confirm they pass**

```powershell
cmake --build --preset windows-x64-debug --target test_edit_player_session
pwsh scripts/run-tests.ps1 -Filter test_edit_player_session
```

Expected: PASS, all 4 cases.

- [ ] **Step 6: Add the new files to the `recorder_core` library target**

```cmake
    include/recorder_core/edit_player_session.h
    src/edit_player_session.cpp
```

```powershell
cmake --build --preset windows-x64-debug --target recorder_core
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/include/recorder_core/edit_player_session.h \
        libs/recorder_core/src/edit_player_session.cpp \
        libs/recorder_core/tests/test_edit_player_session.cpp \
        libs/recorder_core/CMakeLists.txt
git commit -m "Add EditPlayerSession orchestrator (engine + audio renderer + scrub throttling)"
```

---

## Task 9: `EditPlayerSurface` Qt widget + visual-test scenario

App-layer widget, modeled directly on the existing `CameraPreview` (`app/ui/widgets/CameraPreview.h`/
`.cpp`): paints a letterboxed `QImage`, shows a placeholder when empty. This task is Qt-only and
has no dependency on Tasks 5-8 — it can be built and visually verified standalone before wiring.

**Files:**
- Create: `app/ui/widgets/EditPlayerSurface.h`
- Create: `app/ui/widgets/EditPlayerSurface.cpp`
- Modify: `app/CMakeLists.txt`
- Modify: `app/visual_tests/VisualScenario.h`
- Modify: `app/visual_tests/VisualScenario.cpp`
- Modify: `app/MainWindow.cpp` (visual-scenario dispatch)

**Interfaces:**
- Produces: `exosnap::ui::widgets::EditPlayerSurface` with `setFrame(QImage)`, `clearFrame()` —
  consumed by Task 10.

- [ ] **Step 1: Create the widget header**

Create `app/ui/widgets/EditPlayerSurface.h`:

```cpp
#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

namespace exosnap::ui::widgets {

// The Edit-page video player's paint surface. Modeled directly on
// CameraPreview: shows the current decoded frame letterboxed inside a
// rounded dark panel, or a placeholder message when there is no frame yet
// (before the first decode, or a decode failure -- see EditPlayerSession's
// Open() contract).
class EditPlayerSurface : public QWidget {
    Q_OBJECT
  public:
    explicit EditPlayerSurface(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Shows a decoded frame. An empty/null image falls back to the placeholder.
    void setFrame(QImage frame);

    // Drops the current frame and shows the placeholder text again.
    void clearFrame();

    // Sets the message shown when no frame is present (supports '\n').
    void setPlaceholderText(const QString& text);

    [[nodiscard]] bool hasFrame() const noexcept {
        return !frame_.isNull();
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QImage frame_;
    QString placeholder_ = QStringLiteral("Preview unavailable");
};

} // namespace exosnap::ui::widgets
```

- [ ] **Step 2: Implement (adapted from `CameraPreview.cpp`, no mirror option, dark panel to match `editExportPlayer`'s existing look)**

Create `app/ui/widgets/EditPlayerSurface.cpp`:

```cpp
#include "EditPlayerSurface.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace exosnap::ui::widgets {

EditPlayerSurface::EditPlayerSurface(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("editPlayerSurface"));
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize EditPlayerSurface::sizeHint() const {
    return QSize(640, 360);
}

void EditPlayerSurface::setFrame(QImage frame) {
    frame_ = std::move(frame);
    update();
}

void EditPlayerSurface::clearFrame() {
    if (frame_.isNull())
        return;
    frame_ = QImage{};
    update();
}

void EditPlayerSurface::setPlaceholderText(const QString& text) {
    if (placeholder_ == text)
        return;
    placeholder_ = text;
    if (frame_.isNull())
        update();
}

void EditPlayerSurface::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frame_rect = rect().adjusted(0.5, 0.5, -0.5, -0.5);

    QLinearGradient bg_grad(frame_rect.topLeft(), frame_rect.bottomRight());
    bg_grad.setColorAt(0.0, QColor("#181612"));
    bg_grad.setColorAt(1.0, QColor("#0e0d0b"));
    painter.setBrush(bg_grad);
    painter.setPen(QPen(QColor("#353330"), 1.0));
    painter.drawRoundedRect(frame_rect, 5.0, 5.0);

    if (!frame_.isNull()) {
        painter.save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(frame_rect, 5.0, 5.0);
        painter.setClipPath(clip_path);

        const double sx = static_cast<double>(width()) / frame_.width();
        const double sy = static_cast<double>(height()) / frame_.height();
        const double s = std::min(sx, sy);
        const int dw = static_cast<int>(frame_.width() * s);
        const int dh = static_cast<int>(frame_.height() * s);
        const int dx = (width() - dw) / 2;
        const int dy = (height() - dh) / 2;
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRect(dx, dy, dw, dh), frame_);
        painter.restore();
        return;
    }

    if (!placeholder_.isEmpty()) {
        painter.setPen(QColor(255, 255, 255, 120));
        const QRectF text_rect = frame_rect.adjusted(16, 16, -16, -16);
        painter.drawText(text_rect, Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
    }
}

} // namespace exosnap::ui::widgets
```

- [ ] **Step 3: Register the new files in the app target**

In `app/CMakeLists.txt`, add `ui/widgets/EditPlayerSurface.h` and `ui/widgets/EditPlayerSurface.cpp`
to the `exosnap` target's source list, next to the existing `ui/widgets/CameraPreview.h`/`.cpp`
entries (same list, alphabetically or grouped the same way the existing widgets are).

- [ ] **Step 4: Add a visual-test scenario**

In `app/visual_tests/VisualScenario.h`, add a new field to the scenario struct (next to the
existing `finalizing_overlay_mode`-style trailing fields, same convention):

```cpp
    // Edit-page video player surface: "empty" (placeholder only) or "frame"
    // (a synthetic solid-color test image, proving the letterbox/aspect-fit
    // math -- no real decoder runs in the harness, consistent with how every
    // other visual scenario works).
    QString edit_player_surface_mode; // "" (not applicable) | "empty" | "frame"
```

In `app/visual_tests/VisualScenario.cpp`, add two named scenarios (mirroring the
`record-finalizing-stopping`/`record-finalizing-saving` pair added for `FinalizingOverlay`):

```cpp
    // "edit-player-surface-empty": placeholder-only state.
    if (id == QStringLiteral("edit-player-surface-empty")) {
        VisualScenario s;
        s.page = VisualPage::EditExport;
        s.edit_player_surface_mode = QStringLiteral("empty");
        return s;
    }
    // "edit-player-surface-frame": a synthetic 320x180 solid-color test image,
    // proving letterbox/aspect-fit layout without a real decoder.
    if (id == QStringLiteral("edit-player-surface-frame")) {
        VisualScenario s;
        s.page = VisualPage::EditExport;
        s.edit_player_surface_mode = QStringLiteral("frame");
        return s;
    }
```

(Match whatever the existing `if (id == ...)` dispatch chain in this file actually looks like at
the point of implementation — `VisualScenario.cpp` was modified in the current uncommitted session
work per the design doc's prerequisite reading, so re-check its exact current shape before editing
rather than assuming the snippet above is a verbatim drop-in.)

- [ ] **Step 5: Wire the scenario into `MainWindow::applyVisualScenario`**

In `app/MainWindow.cpp`, find the `case visual::VisualPage::EditExport:` (or add one if it does not
exist yet) inside `applyVisualScenario`, and add:

```cpp
    if (!scenario.edit_player_surface_mode.isEmpty()) {
        auto* surface = edit_export_page_->findChild<exosnap::ui::widgets::EditPlayerSurface*>(
            QStringLiteral("editPlayerSurface"));
        if (surface) {
            if (scenario.edit_player_surface_mode == QStringLiteral("frame")) {
                QImage test_img(320, 180, QImage::Format_RGB32);
                test_img.fill(QColor("#3a6b5c")); // arbitrary solid color -- proves paint path, not color accuracy
                surface->setFrame(test_img);
            } else {
                surface->clearFrame();
            }
        }
    }
```

(Confirm the exact member name for the Edit/Output/Save page instance — `edit_export_page_` is
inferred from this plan's file reading of `EditExportPage`/`EditExportOverlay`; verify against
`MainWindow.h`'s actual member name before writing this, since it was not directly read during
planning.)

- [ ] **Step 6: Build and render both scenarios**

```powershell
cmake --build --preset windows-x64-debug-exosnap
build\windows-x64-debug\app\exosnap.exe --visual-test edit-player-surface-empty --visual-test-screenshot $env:TEMP\edit-player-surface-empty.png
build\windows-x64-debug\app\exosnap.exe --visual-test edit-player-surface-frame --visual-test-screenshot $env:TEMP\edit-player-surface-frame.png
```

Inspect both PNGs: the empty one shows the dark rounded panel with "Preview unavailable" centered;
the frame one shows the solid-color test image letterboxed inside the same panel, no clipping.

- [ ] **Step 7: Add the scenario-suite regression tests**

Add both new scenario ids to whatever existing `visual_scenario_tests` assertions enumerate all
scenario ids (`ScenarioIdsAreUnique`, etc., per the pattern already used for the
`record-finalizing-*` scenarios). Run:

```powershell
pwsh scripts/run-tests.ps1 -Filter visual_scenario_tests
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add app/ui/widgets/EditPlayerSurface.h app/ui/widgets/EditPlayerSurface.cpp app/CMakeLists.txt \
        app/visual_tests/VisualScenario.h app/visual_tests/VisualScenario.cpp app/MainWindow.cpp
git commit -m "Add EditPlayerSurface widget and its visual-test scenario"
```

---

## Task 10: Wire `EditPlayerSession` + `EditPlayerSurface` into `EditExportPage`

Replaces the `player_sub_` placeholder label with the new `EditPlayerSurface`, and connects the
existing play/pause/scrub/trim UI to the real engine via `EditPlayerSession`. All cross-thread
frame delivery from the engine to the UI uses `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`
(the same WinRT→UI marshalling convention already used elsewhere in this codebase per project
memory).

**Files:**
- Modify: `app/pages/EditExportPage.h`
- Modify: `app/pages/EditExportPage.cpp`
- Modify: `app/tests/test_transport_dock.cpp` or a new `app/tests/test_edit_export_page.cpp` if one
  does not already exist (check first — see Step 1).

**Interfaces:**
- Consumes: `recorder_core::EditPlayerSession`, `recorder_core::DecodedVideoFrame` (Task 8),
  `exosnap::ui::widgets::EditPlayerSurface` (Task 9).

- [ ] **Step 1: Check for existing `EditExportPage` widget tests**

```bash
find app/tests -iname "*edit_export*"
```

If a test file exists, extend it in Step 6 below; if not, this task does not add a new one (no
existing test infrastructure to extend safely within this task's bound) — rely on the visual-test
scenario from Task 9 plus the manual live-verify in Task 11 for this page's behavior, consistent
with how `EditExportPage.cpp`'s existing trim/scrub logic (`onTrimHandleReleased`, etc.) has no
dedicated unit test file today either (it's UI-integration code, verified via the widget's actual
behavior in the running app per this project's established split between unit-tested pure logic
and live/visual-verified UI wiring).

- [ ] **Step 2: Add the new members to `EditExportPage.h`**

In `app/pages/EditExportPage.h`, add near the top:

```cpp
#include <recorder_core/edit_player_session.h>
```

Replace the `player_sub_` member declaration:

```cpp
    QLabel* player_sub_ = nullptr;
```

with:

```cpp
    ui::widgets::EditPlayerSurface* player_surface_ = nullptr;
```

Add a forward declaration near the top with the other `namespace ui::widgets` forward declarations:

```cpp
namespace ui::widgets {
class EditTimeline;
class EditPlayerSurface;
}
```

Add a new private member and a new private slot:

```cpp
    std::unique_ptr<recorder_core::EditPlayerSession> player_session_;

  private slots:
    void onDecodedFrameReady(QImage frame); // marshalled onto the UI thread via invokeMethod
```

(Add `#include <memory>` if not already present.)

- [ ] **Step 3: Replace `player_sub_` construction with `EditPlayerSurface` in `buildUi()`**

In `app/pages/EditExportPage.cpp`, replace:

```cpp
    player_sub_ = new QLabel(QStringLiteral("Video preview — coming in 0.11"), player_frame_);
    player_sub_->setAlignment(Qt::AlignCenter);
```

with:

```cpp
    player_surface_ = new ui::widgets::EditPlayerSurface(player_frame_);
```

and update `player_layout->addWidget(player_sub_);` to `player_layout->addWidget(player_surface_);`.
Add `#include "../ui/widgets/EditPlayerSurface.h"` to the includes block at the top of the file.

- [ ] **Step 4: Open/close the session in `setEditContext()`**

In `setEditContext()` (the function that already resets `keyframe_timestamps_`/markers/the preview
clock for a new clip), add, right after the existing keyframe-extraction block and before the
"Reset the preview clock" comment:

```cpp
    // --- Open the real decoder session for the new clip (replaces the previous one, if any) ---
    player_session_ = std::make_unique<recorder_core::EditPlayerSession>();
    if (!ctx_.mkv_master_path.isEmpty()) {
        std::string open_err;
        const bool opened =
            player_session_->Open(std::filesystem::path(ctx_.mkv_master_path.toStdWString()), open_err);
        if (opened) {
            player_session_->SetOnFrameReady([this](recorder_core::DecodedVideoFrame frame) {
                QImage img(frame.bgra->data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                          static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32);
                // QImage does not own frame.bgra's storage; keep the shared_ptr alive for the
                // image's lifetime by copying it into a detached QImage on the UI thread instead
                // of holding a view across the thread hop. invokeMethod's queued call copies its
                // arguments (the QImage, by value) onto the UI thread's event queue, and the
                // lambda that captured `frame` (and therefore frame.bgra) stays alive until this
                // call returns, so .copy() below is taken while the buffer is still valid.
                QMetaObject::invokeMethod(this, "onDecodedFrameReady", Qt::QueuedConnection,
                                          Q_ARG(QImage, img.copy()));
            });
        } else if (player_surface_) {
            player_surface_->clearFrame(); // "Preview unavailable" — matches the design's error fallback
        }
    }
```

Add `#include <filesystem>` if not already present in this file.

- [ ] **Step 5: Implement the new slot and hook it into the surface**

Add near the other slot implementations:

```cpp
void EditExportPage::onDecodedFrameReady(QImage frame) {
    if (player_surface_)
        player_surface_->setFrame(std::move(frame));
}
```

- [ ] **Step 6: Wire play/pause/scrub/trim to the session**

Modify `setPreviewPlaying(bool playing)`:

```cpp
void EditExportPage::setPreviewPlaying(bool playing) {
    if (playing == preview_playing_)
        return;
    if (playing && durationMs() <= 0)
        return; // unknown duration: nothing to play against
    preview_playing_ = playing;
    if (preview_playing_) {
        preview_elapsed_->restart();
        preview_timer_->start();
        if (player_session_)
            player_session_->Play();
    } else {
        preview_timer_->stop();
        if (player_session_)
            player_session_->Pause();
    }
    refreshPlayButton();
}
```

(`preview_timer_`/`preview_elapsed_` stay running unconditionally — they still drive the playhead
UI position via `onPreviewTick` exactly as before; when `HasAudioStream()` is true this becomes a
secondary/backup position source, since the actual frame selection is driven by
`EditPlayerSession`'s internal audio clock. This keeps the existing playhead-position code path
completely unchanged, matching the design's "the trim-handle/playhead UI is already correct"
framing — only the frame source changes.)

Modify `onScrubMoved`:

```cpp
void EditExportPage::onScrubMoved(qint64 position_ms) {
    preview_position_ms_ = ClampPlayheadMs(position_ms, durationMs());
    if (player_session_)
        player_session_->SeekTo(preview_position_ms_ * 1000); // ms -> us
}
```

Modify `onTrimHandleReleased`: after the existing snap-to-keyframe/marker logic already updates
`trim_start_us_`/`trim_end_us_` and calls `timeline_->setTrimRangeMs(...)`, add a seek so the
player shows the snapped boundary the user just landed on:

```cpp
    // Show the frame at the (possibly snapped) boundary the handle landed on.
    if (player_session_) {
        const int64_t shown_us = (start_ms <= 0) ? trim_end_us_ : trim_start_us_;
        if (shown_us != recorder_core::TrimRange::kNoTimestamp)
            player_session_->SeekTo(shown_us);
    }
```

(Insert this at the end of the existing function body, after the `timeline_->setTrimRangeMs(...)`
call.)

- [ ] **Step 7: Tear down on hide, matching the existing `export_thread_` join pattern**

In `hideEvent()`, add (check the existing body first — this task assumes it currently does not
already touch `player_session_`, since that member is new in this task):

```cpp
void EditExportPage::hideEvent(QHideEvent* event) {
    if (player_session_)
        player_session_->Close();
    QWidget::hideEvent(event); // preserve whatever the existing body already does — verify order
                                // against the current function before inserting, since this task
                                // did not re-read hideEvent()'s current full body during planning.
}
```

(Confirm the exact current contents of `hideEvent()` before editing — it was declared in the header
read during brainstorming but its body was not read during this planning pass. Insert the
`player_session_->Close()` call without removing or reordering whatever it already does.)

- [ ] **Step 8: Build**

```powershell
cmake --build --preset windows-x64-debug-exosnap
```

Fix any compile errors surfaced here — this is the first point every prior task's pieces compile
together against the real `EditExportPage.cpp`, so mismatches between this task's assumptions
(exact current `hideEvent()` body, exact current `VisualScenario.cpp` dispatch shape, exact
`MainWindow.h` member name for the edit/export page) and the actual current file contents are
expected and get fixed here, not treated as plan failures.

- [ ] **Step 9: Startup-check (per CLAUDE.md's QSS/new-widget crash-check rule)**

```powershell
build\windows-x64-debug\app\exosnap.exe
```

Start it, confirm no crash within ~3 seconds, then close it. Do not click through it (no live app
driving) — this is only the one-time startup-survives check CLAUDE.md explicitly allows.

- [ ] **Step 10: Commit**

```bash
git add app/pages/EditExportPage.h app/pages/EditExportPage.cpp
git commit -m "Wire EditPlayerSession into EditExportPage: real decode replaces the synthetic preview"
```

---

## Task 11: Full gate, docs, and the live-verify handoff

**Files:**
- Modify: `docs/product-spec.md:638-640` (§8 "Current boundary")
- Modify: `docs/product-spec.md:955-962` (§15 "Not present in current builds")
- Modify: `.workspace/live-verify-checklist-0.9.md` (if it exists — check first; this repo's memory
  references it) or wherever the project's current live-verify list lives.

- [ ] **Step 1: Update the product spec's "Current boundary" line (§8)**

Change:

```
**Current boundary:** trim, markers, and stream-copy export are implemented and reachable end to
end, including the playhead/scrub interaction (a position clock). Decoded video frames inside the
player area and the Split Chapter action remain deferred to a later release (0.11 per ADR 0022).
```

to:

```
**Current boundary:** trim, markers, stream-copy export, and real decoded-frame preview (video +
synchronized audio, FFmpeg-decode + Qt-paint) are implemented and reachable end to end, including
the playhead/scrub/trim-handle interactions against the real decoder. The Split Chapter action
remains deferred to a later release (0.11 per ADR 0022).
```

- [ ] **Step 2: Update §15's "Not present in current builds" list**

Remove `video preview playback inside the Edit/Output/Save overlay` from that sentence's list
(it currently reads something like `...container chapter export is deliberately out of scope for
the MVP); video preview playback inside the Edit/Output/Save overlay; HDR beyond BT.2020...` — cut
just that clause, keeping the surrounding list's punctuation valid).

- [ ] **Step 3: Add a live-verify checklist entry**

Check whether `.workspace/live-verify-checklist-0.9.md` exists (per this project's memory, a
0.9-release-gate live-verify checklist is tracked there or in a similarly named file — confirm the
exact path first with `find .workspace -iname "*live-verify*"`). Add an entry:

```
- [ ] Edit-page video player: record a short clip (a few seconds, any codec combination), open
  Edit/Output/Save, confirm: (a) the player shows real decoded video, not a placeholder; (b) audio
  is audible and stays in sync with the video during Play; (c) scrubbing the playhead updates the
  frame live and throttles smoothly during a fast drag; (d) dragging a trim handle shows the
  snapped-to-keyframe frame on release; (e) a recording made with all audio sources muted still
  shows video (silent, no crash, no hang).
```

- [ ] **Step 4: Full gate**

```powershell
pwsh scripts/check-format.ps1
git diff --check
cmake --build --preset windows-x64-debug
pwsh scripts/run-tests.ps1
pwsh scripts/check-quality.ps1 -StaticOnly
cmake --build --preset windows-x64-release-exosnap
```

Expected: all steps exit 0. Fix any failures before proceeding — this is the one full-gate run for
the whole feature, per AGENTS.md's "minimal validation during development; complete validation once
at the final gate."

- [ ] **Step 5: Commit the docs**

```bash
git add docs/product-spec.md .workspace/live-verify-checklist-0.9.md
git commit -m "Update product spec: real Edit-page video player replaces the 0.11 placeholder"
```

- [ ] **Step 6: Report to the user**

Summarize: what changed, which files, that the companion FFmpeg repo needed and got a new release
(link the merged PR), that live playback/audio/sync correctness is NOT covered by any automated
test in this plan and needs the user's own live-verify pass (Step 3's checklist entry), and any
open items carried over from the design doc's "Open questions" section (queue depth tuning, AV1
software-decode scrub-latency risk, WASAPI buffer latency tuning) as follow-ups, not blockers.

---

## Self-Review Notes

- **Spec coverage:** every section of `docs/superpowers/specs/2026-07-14-edit-video-player-design.md`
  maps to a task — companion-repo prerequisite (Task 1-2), engine decode module (Task 5-6), video
  pixel path (Task 3), audio playback + master clock (Task 7-8), threading/lifecycle/scrub
  throttling (Task 6, 8), UI integration (Task 9-10), error handling (Task 5's Open failure path +
  Task 10 Step 4's fallback), testing strategy (every task's own test step + Task 11's live-verify
  handoff).
- **Placeholder scan:** no TBD/TODO left in any step; the three places this plan explicitly asks the
  executing agent to re-verify current file contents before editing (Task 9 Step 4-5, Task 10 Step
  7) are flagged as such because this planning pass did not read those exact current bodies in
  full — that is an honest gap disclosure, not a placeholder, and each includes a concrete
  fallback instruction (insert without reordering/removing existing behavior).
- **Type consistency:** `DecodedVideoFrame`/`DecodedAudioBlock` (Task 5) are used identically in
  Task 6, 8, 10. `EditPlayerEngine::Open/Close/HasVideoStream/HasAudioStream/DecodeFrameAt/
  StartPlaybackDecode/StopPlaybackDecode` (Task 5-6) match `EditPlayerSession`'s usage in Task 8
  exactly. `EditPlayerSurface::setFrame/clearFrame` (Task 9) match Task 10's call sites exactly.
  `FullPlanarYuv420Frame`/`ConvertFullPlanarYuv420ToBgra` (Task 3) match Task 5-6's usage exactly.
