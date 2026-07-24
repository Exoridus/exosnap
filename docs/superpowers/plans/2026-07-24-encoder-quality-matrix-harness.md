# Encoder Quality Matrix Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a dev-only harness that objectively measures NVENC quality-per-bitrate (SSIM/VMAF/BD-rate) against ExoSnap's real encoder code path, so future encoder-quality changes have evidence instead of guesswork.

**Architecture:** A new pure-function library slice in `libs/recorder_core` (Y4M parsing, I420→NV12 conversion, IVF framing — CI-tested, no GPU) backs a new dev-only CLI probe (`tools/probes/probe_encode_file`, gated behind `EXOSNAP_BUILD_PROBES=ON`, never packaged) that drives the real `NvencVideoEncoder`. A Python orchestration script (`scripts/dev/encoder_quality_matrix.py`) sweeps preset/rate-control combinations through the probe, scores each against a reference clip with an external `ffmpeg` (`libvmaf`), and computes BD-rate.

**Tech Stack:** C++20 (recorder_core conventions), Python 3 stdlib only, external `ffmpeg` CLI with `libvmaf` (developer machine only, never vendored).

## Global Constraints

- Nothing in this plan ships in the product build or the packaged installer — every new file lives under `tools/probes/` (gated `EXOSNAP_BUILD_PROBES=ON`, default OFF) or `scripts/dev/`/`docs/development/`.
- No `libvmaf`/quality-metric dependency is vendored or linked into `recorder_core` or any shipped binary — metrics only ever run through the developer's own `ffmpeg` CLI, invoked as a subprocess from the Python script.
- 8-bit 4:2:0 only in this pass (`ChromaSubsampling::Cs420`, `BitDepth::Bit8`, `DXGI_FORMAT_NV12`). No 10-bit/P010, no HDR.
- Pure functions (Y4M parsing, I420→NV12, IVF framing, BD-rate math) get CI-run unit tests with no GPU dependency. The GPU-driving parts (`probe_encode_file`'s `main.cpp`, the Python script's actual encode/measure loop) are dev-only, verified manually/live — never added to CI, matching how every other `tools/probes/*` target and every live-hardware check in this project already works.
- `encoder_quality_matrix.py` is stdlib-only (no numpy/pandas/etc.), matching `scripts/dev/analyze-encode-perf.py`.
- No internal process/plan labels (task numbers, spec-slice codenames) in commit messages, code comments, or docs — technical substance only.

---

## File Structure

**New, CI-tested pure functions (product library, but zero product-runtime cost — nothing calls them except the new probe and their own tests):**
- `libs/recorder_core/src/y4m_reader.h` / `.cpp` — parses a YUV4MPEG2 header line and reads one raw I420 frame at a time from an in-memory buffer.
- `libs/recorder_core/src/yuv_convert.h` / `.cpp` — converts one I420 frame (3 separate planes) to one NV12 frame (Y plane + interleaved UV plane) as a flat byte buffer ready for a single `UpdateSubresource` call.
- `libs/recorder_core/src/elementary_stream_writer.h` / `.cpp` — builds IVF file/frame headers for AV1 output. (H.264/HEVC need no framing function: NVENC already emits Annex-B start-coded bitstream — see `annexb_to_avcc.cpp` for existing confirmation of this — so those two codecs are a direct byte concatenation in `main.cpp`, not a separate pure function.)
- `libs/recorder_core/tests/test_y4m_reader.cpp`, `test_yuv_convert.cpp`, `test_elementary_stream_writer.cpp` — new gtest targets, registered in `libs/recorder_core/CMakeLists.txt` next to the existing `test_nvenc_*` registrations.

**New dev-only probe (never built by CI, never packaged):**
- `tools/probes/probe_encode_file/CMakeLists.txt`
- `tools/probes/probe_encode_file/src/main.cpp` — CLI parsing, D3D11 device, drives `NvencVideoEncoder`, writes the output elementary stream.
- `tools/probes/CMakeLists.txt` — add one `add_subdirectory(probe_encode_file)` line.

**New dev tooling (Python, docs):**
- `scripts/dev/encoder_quality_matrix.py` — BD-rate math (pure, `--self-test`-covered) + orchestration (subprocess calls to the probe and to `ffmpeg`).
- `docs/development/encoder-quality-matrix.md` — workflow doc.
- `docs/roadmap.md` — one-line link addition on the 1.0 quality-gate line.

---

### Task 1: Y4M reader (pure, CI-tested)

**Files:**
- Create: `libs/recorder_core/src/y4m_reader.h`
- Create: `libs/recorder_core/src/y4m_reader.cpp`
- Test: `libs/recorder_core/tests/test_y4m_reader.cpp`

**Interfaces:**
- Consumes: nothing outside the standard library.
- Produces: `recorder_core::Y4mHeader { width, height, fps_num, fps_den, header_bytes }`,
  `recorder_core::ParseY4mHeader(std::string_view, std::string& out_error) -> std::optional<Y4mHeader>`,
  `recorder_core::I420FrameSize(uint32_t width, uint32_t height) -> size_t`,
  `recorder_core::Y4mFrame { data_offset, data_size, next_offset }`,
  `recorder_core::ReadY4mFrame(std::string_view data, size_t offset, uint32_t width, uint32_t height, std::string& out_error) -> std::optional<Y4mFrame>`.
  Task 5 (the probe's `main.cpp`) calls all four of these by name.

- [ ] **Step 1: Write the failing test**

Create `libs/recorder_core/tests/test_y4m_reader.cpp`:

```cpp
#include "../src/y4m_reader.h"

#include <gtest/gtest.h>

namespace recorder_core {
namespace {

TEST(ParseY4mHeader, ParsesWidthHeightFpsAndHeaderLength) {
    const std::string data = "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420jpeg\nFRAME\n";
    std::string err;
    const auto header = ParseY4mHeader(data, err);
    ASSERT_TRUE(header.has_value()) << err;
    EXPECT_EQ(header->width, 1920u);
    EXPECT_EQ(header->height, 1080u);
    EXPECT_EQ(header->fps_num, 30u);
    EXPECT_EQ(header->fps_den, 1u);
    // "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420jpeg\n" is 46 bytes.
    EXPECT_EQ(header->header_bytes, 46u);
}

TEST(ParseY4mHeader, AcceptsTagsInAnyOrder) {
    const std::string data = "YUV4MPEG2 C420mpeg2 H480 F60:1 W640\n";
    std::string err;
    const auto header = ParseY4mHeader(data, err);
    ASSERT_TRUE(header.has_value()) << err;
    EXPECT_EQ(header->width, 640u);
    EXPECT_EQ(header->height, 480u);
    EXPECT_EQ(header->fps_num, 60u);
}

TEST(ParseY4mHeader, RejectsWrongMagic) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader("NOTYUV4MPEG2 W1 H1 F1:1 C420\n", err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(ParseY4mHeader, RejectsMissingHeaderTerminator) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader("YUV4MPEG2 W1 H1 F1:1 C420", err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(ParseY4mHeader, RejectsUnsupportedChromaFormat) {
    std::string err;
    const auto header = ParseY4mHeader("YUV4MPEG2 W1 H1 F1:1 C422\n", err);
    EXPECT_FALSE(header.has_value());
    EXPECT_NE(err.find("chroma"), std::string::npos) << err;
}

TEST(ParseY4mHeader, RejectsMissingChromaTag) {
    std::string err;
    EXPECT_FALSE(ParseY4mHeader("YUV4MPEG2 W1 H1 F1:1\n", err).has_value());
}

TEST(I420FrameSize, ComputesLumaPlusTwoQuarterChromaPlanes) {
    // 4x2: Y = 8 bytes, each chroma plane = 2x1 = 2 bytes -> 8 + 2 + 2 = 12.
    EXPECT_EQ(I420FrameSize(4, 2), 12u);
    // 1920x1080: 1920*1080 + 2*(960*540) = 2073600 + 1036800 = 3110400.
    EXPECT_EQ(I420FrameSize(1920, 1080), 3110400u);
}

TEST(ReadY4mFrame, ReadsOneFrameAndAdvancesOffset) {
    // width=4 height=2 -> I420 frame size 12. Two frames back to back.
    std::string data = "FRAME\n";
    data += std::string(12, '\x11'); // frame 0 payload
    data += "FRAME\n";
    data += std::string(12, '\x22'); // frame 1 payload

    std::string err;
    const auto f0 = ReadY4mFrame(data, 0, 4, 2, err);
    ASSERT_TRUE(f0.has_value()) << err;
    EXPECT_EQ(f0->data_offset, 6u); // after "FRAME\n"
    EXPECT_EQ(f0->data_size, 12u);
    EXPECT_EQ(data[f0->data_offset], '\x11');
    EXPECT_EQ(f0->next_offset, 6u + 12u);

    const auto f1 = ReadY4mFrame(data, f0->next_offset, 4, 2, err);
    ASSERT_TRUE(f1.has_value()) << err;
    EXPECT_EQ(data[f1->data_offset], '\x22');
    EXPECT_EQ(f1->next_offset, data.size());
}

TEST(ReadY4mFrame, ReturnsNulloptWithEmptyErrorAtCleanEof) {
    const std::string data = "FRAME\n" + std::string(12, '\0');
    std::string err;
    const auto f0 = ReadY4mFrame(data, 0, 4, 2, err);
    ASSERT_TRUE(f0.has_value());
    const auto eof = ReadY4mFrame(data, f0->next_offset, 4, 2, err);
    EXPECT_FALSE(eof.has_value());
    EXPECT_TRUE(err.empty());
}

TEST(ReadY4mFrame, RejectsMalformedFrameMarker) {
    const std::string data = "WRONG\n" + std::string(12, '\0');
    std::string err;
    EXPECT_FALSE(ReadY4mFrame(data, 0, 4, 2, err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(ReadY4mFrame, RejectsTruncatedFrameData) {
    const std::string data = "FRAME\n" + std::string(5, '\0'); // needs 12 bytes, only 5 present
    std::string err;
    EXPECT_FALSE(ReadY4mFrame(data, 0, 4, 2, err).has_value());
    EXPECT_FALSE(err.empty());
}

} // namespace
} // namespace recorder_core
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target test_y4m_reader --config Debug`
Expected: FAIL — `y4m_reader.h` does not exist yet (compile error).

- [ ] **Step 3: Write the header**

Create `libs/recorder_core/src/y4m_reader.h`:

```cpp
#pragma once

// Pure YUV4MPEG2 (.y4m) reader: parses the header line and reads raw I420
// frame data from an in-memory buffer. No file I/O here so the parsing logic
// is unit-testable without a real file — the probe (dev-only, GPU-driving)
// owns reading the file into memory and calling these.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace recorder_core {

struct Y4mHeader {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 0;
    uint32_t fps_den = 0;
    // Byte length of the header line including its trailing '\n' — frame
    // data starts at this offset in the source buffer.
    size_t header_bytes = 0;
};

// Parses the first line of a .y4m file, e.g.
// "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420jpeg\n". Only 8-bit 4:2:0 chroma
// (C420, C420jpeg, C420mpeg2) is accepted — anything else, or a missing C
// tag, is an error naming what was rejected. `data` only needs to contain at
// least the header line; trailing frame data (if present) is ignored here.
std::optional<Y4mHeader> ParseY4mHeader(std::string_view data, std::string& out_error);

// Number of raw bytes one 8-bit 4:2:0 (I420) frame occupies: a full-resolution
// Y plane plus two quarter-resolution chroma planes.
constexpr size_t I420FrameSize(uint32_t width, uint32_t height) noexcept {
    return static_cast<size_t>(width) * height +
          2 * ((static_cast<size_t>(width) / 2) * (static_cast<size_t>(height) / 2));
}

struct Y4mFrame {
    // Offset into the source buffer where this frame's raw I420 pixel data
    // begins (immediately after its "FRAME" marker line).
    size_t data_offset = 0;
    size_t data_size = 0;
    // Offset where the next frame's "FRAME" marker (or end of stream) begins.
    size_t next_offset = 0;
};

// Reads one frame's "FRAME" marker line plus its raw I420 payload, starting
// at `offset` in `data` (which must point at the start of a marker line).
// Returns std::nullopt with `out_error` set on a malformed marker or
// truncated payload. Returns std::nullopt with an EMPTY `out_error` at a
// clean end of stream (offset == data.size()) — that is not an error, it is
// how the caller's read loop knows to stop.
std::optional<Y4mFrame> ReadY4mFrame(std::string_view data, size_t offset, uint32_t width, uint32_t height,
                                     std::string& out_error);

} // namespace recorder_core
```

- [ ] **Step 4: Write the implementation**

Create `libs/recorder_core/src/y4m_reader.cpp`:

```cpp
#include "y4m_reader.h"

#include <charconv>

namespace recorder_core {

namespace {

bool ParseUint(std::string_view s, uint32_t& out) {
    const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

} // namespace

std::optional<Y4mHeader> ParseY4mHeader(std::string_view data, std::string& out_error) {
    const size_t nl = data.find('\n');
    if (nl == std::string_view::npos) {
        out_error = "y4m header: no newline terminator found";
        return std::nullopt;
    }
    const std::string_view line = data.substr(0, nl);

    size_t pos = 0;
    const size_t firstSpace = line.find(' ');
    const std::string_view magic = line.substr(0, firstSpace == std::string_view::npos ? line.size() : firstSpace);
    if (magic != "YUV4MPEG2") {
        out_error = "y4m header: expected YUV4MPEG2 magic, got '" + std::string(magic) + "'";
        return std::nullopt;
    }
    pos = (firstSpace == std::string_view::npos) ? line.size() : firstSpace + 1;

    Y4mHeader header;
    bool haveWidth = false, haveHeight = false, haveFps = false, haveChroma = false;

    while (pos < line.size()) {
        size_t tagEnd = line.find(' ', pos);
        if (tagEnd == std::string_view::npos)
            tagEnd = line.size();
        const std::string_view tag = line.substr(pos, tagEnd - pos);
        pos = tagEnd + 1;
        if (tag.empty())
            continue;

        switch (tag[0]) {
        case 'W':
            if (!ParseUint(tag.substr(1), header.width)) {
                out_error = "y4m header: bad width tag '" + std::string(tag) + "'";
                return std::nullopt;
            }
            haveWidth = true;
            break;
        case 'H':
            if (!ParseUint(tag.substr(1), header.height)) {
                out_error = "y4m header: bad height tag '" + std::string(tag) + "'";
                return std::nullopt;
            }
            haveHeight = true;
            break;
        case 'F': {
            const size_t colon = tag.find(':');
            if (colon == std::string_view::npos || !ParseUint(tag.substr(1, colon - 1), header.fps_num) ||
                !ParseUint(tag.substr(colon + 1), header.fps_den)) {
                out_error = "y4m header: bad framerate tag '" + std::string(tag) + "'";
                return std::nullopt;
            }
            haveFps = true;
            break;
        }
        case 'C': {
            const std::string_view chroma = tag.substr(1);
            if (chroma != "420" && chroma != "420jpeg" && chroma != "420mpeg2") {
                out_error = "y4m header: unsupported chroma format '" + std::string(chroma) +
                            "' (only 8-bit 4:2:0 is supported)";
                return std::nullopt;
            }
            haveChroma = true;
            break;
        }
        default:
            // I (interlace), A (aspect), X (comment/extension): accepted, ignored.
            break;
        }
    }

    if (!haveWidth || !haveHeight || !haveFps || !haveChroma) {
        out_error = "y4m header: missing required tag (need W, H, F, and C)";
        return std::nullopt;
    }

    header.header_bytes = nl + 1;
    return header;
}

std::optional<Y4mFrame> ReadY4mFrame(std::string_view data, size_t offset, uint32_t width, uint32_t height,
                                     std::string& out_error) {
    if (offset == data.size())
        return std::nullopt; // clean EOF: out_error stays empty

    if (offset > data.size()) {
        out_error = "y4m frame: offset past end of buffer";
        return std::nullopt;
    }

    const size_t nl = data.find('\n', offset);
    if (nl == std::string_view::npos) {
        out_error = "y4m frame: no newline terminator on FRAME marker";
        return std::nullopt;
    }
    const std::string_view markerLine = data.substr(offset, nl - offset);
    // The marker is "FRAME" optionally followed by per-frame parameters
    // ("FRAME Ip ..."); only the literal prefix matters here.
    if (markerLine.substr(0, 5) != "FRAME") {
        out_error = "y4m frame: expected FRAME marker, got '" + std::string(markerLine) + "'";
        return std::nullopt;
    }

    Y4mFrame frame;
    frame.data_offset = nl + 1;
    frame.data_size = I420FrameSize(width, height);
    if (frame.data_offset + frame.data_size > data.size()) {
        out_error = "y4m frame: truncated frame data (need " + std::to_string(frame.data_size) + " bytes, have " +
                    std::to_string(data.size() - frame.data_offset) + ")";
        return std::nullopt;
    }
    frame.next_offset = frame.data_offset + frame.data_size;
    return frame;
}

} // namespace recorder_core
```

- [ ] **Step 5: Register the gtest target**

In `libs/recorder_core/CMakeLists.txt`, find the block registering `test_split_sentinel_policy` (added for the async NVENC work — search for `NAME test_split_sentinel_policy`) and add immediately after its `target_include_directories` line:

```cmake
# test_y4m_reader: pure unit tests for the YUV4MPEG2 header/frame parser
# backing the encoder quality matrix harness's probe (no GPU needed).
exosnap_add_gtest(
    NAME test_y4m_reader
    TEST_PREFIX recorder_core.
    SOURCES tests/test_y4m_reader.cpp src/y4m_reader.cpp
)
target_include_directories(test_y4m_reader PRIVATE src)
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target test_y4m_reader --config Debug`
Run: `ctest --test-dir build/windows-x64-debug -C Debug -R test_y4m_reader --output-on-failure`
Expected: all `recorder_core.ParseY4mHeader.*`, `recorder_core.I420FrameSize.*`, `recorder_core.ReadY4mFrame.*` cases PASS.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/src/y4m_reader.h libs/recorder_core/src/y4m_reader.cpp \
        libs/recorder_core/tests/test_y4m_reader.cpp libs/recorder_core/CMakeLists.txt
git commit -m "feat(recorder_core): add a pure YUV4MPEG2 header/frame reader"
```

---

### Task 2: I420→NV12 converter (pure, CI-tested)

**Files:**
- Create: `libs/recorder_core/src/yuv_convert.h`
- Create: `libs/recorder_core/src/yuv_convert.cpp`
- Test: `libs/recorder_core/tests/test_yuv_convert.cpp`

**Interfaces:**
- Consumes: nothing beyond the standard library. Independent of Task 1 (takes raw plane pointers, not a `Y4mFrame`).
- Produces: `recorder_core::ConvertI420ToNv12(const uint8_t* i420, uint32_t width, uint32_t height, std::vector<uint8_t>& out_nv12)`.
  Task 5's `main.cpp` calls this by name; its output buffer is what gets passed straight to `ID3D11DeviceContext::UpdateSubresource`.

- [ ] **Step 1: Write the failing test**

Create `libs/recorder_core/tests/test_yuv_convert.cpp`:

```cpp
#include "../src/yuv_convert.h"

#include <gtest/gtest.h>

namespace recorder_core {
namespace {

TEST(ConvertI420ToNv12, OutputSizeMatchesNv12Layout) {
    // 4x2 I420: Y=8, U=2, V=2 -> 12 bytes in. NV12 out: Y=8, interleaved UV=4 -> 12 bytes out.
    const uint8_t i420[12] = {1, 2, 3, 4, 5, 6, 7, 8, // Y (4x2)
                              9, 10,                  // U (2x1)
                              11, 12};                // V (2x1)
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420, 4, 2, nv12);
    ASSERT_EQ(nv12.size(), 12u);
}

TEST(ConvertI420ToNv12, CopiesLumaPlaneUnchanged) {
    const uint8_t i420[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420, 4, 2, nv12);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(nv12[static_cast<size_t>(i)], i420[i]) << "luma byte " << i;
}

TEST(ConvertI420ToNv12, InterleavesUAndVAfterLuma) {
    // Y = 8 bytes of 0. U = {9, 10}. V = {11, 12}.
    const uint8_t i420[12] = {0, 0, 0, 0, 0, 0, 0, 0, 9, 10, 11, 12};
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420, 4, 2, nv12);
    // NV12 UV plane starts at offset 8: U0 V0 U1 V1 = 9 11 10 12.
    EXPECT_EQ(nv12[8], 9);
    EXPECT_EQ(nv12[9], 11);
    EXPECT_EQ(nv12[10], 10);
    EXPECT_EQ(nv12[11], 12);
}

TEST(ConvertI420ToNv12, HandlesLargerEvenDimensions) {
    // 8x4: Y=32, U=8 (4x2), V=8 (4x2) -> 48 bytes in; NV12 out = 32 + 16 = 48.
    std::vector<uint8_t> i420(48);
    for (size_t i = 0; i < i420.size(); ++i)
        i420[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> nv12;
    ConvertI420ToNv12(i420.data(), 8, 4, nv12);
    ASSERT_EQ(nv12.size(), 48u);
    // Luma unchanged.
    for (int i = 0; i < 32; ++i)
        EXPECT_EQ(nv12[static_cast<size_t>(i)], i420[static_cast<size_t>(i)]);
    // First interleaved pair: U plane starts at i420[32], V plane at i420[40].
    EXPECT_EQ(nv12[32], i420[32]); // U0
    EXPECT_EQ(nv12[33], i420[40]); // V0
    EXPECT_EQ(nv12[34], i420[33]); // U1
    EXPECT_EQ(nv12[35], i420[41]); // V1
}

} // namespace
} // namespace recorder_core
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target test_yuv_convert --config Debug`
Expected: FAIL — `yuv_convert.h` does not exist yet.

- [ ] **Step 3: Write the header**

Create `libs/recorder_core/src/yuv_convert.h`:

```cpp
#pragma once

// Pure CPU-side I420 (planar 4:2:0) -> NV12 (semi-planar 4:2:0) conversion.
// NV12 is what the D3D11 encode texture ring uses everywhere else in this
// codebase (see video_thread.cpp's nv12Textures ring), and its Y-plane-then-
// interleaved-UV-plane layout is exactly what a single
// ID3D11DeviceContext::UpdateSubresource call with row pitch = width expects
// — the same upload pattern already used by tools/probes/probe_nvenc_async.

#include <cstdint>
#include <vector>

namespace recorder_core {

// Converts one I420 frame (width*height Y bytes, then (width/2)*(height/2) U
// bytes, then (width/2)*(height/2) V bytes — see Y4mFrame/I420FrameSize) into
// one NV12 frame (width*height Y bytes, then interleaved U/V at half
// resolution: U0 V0 U1 V1 ...). `width` and `height` must both be even.
// `out_nv12` is resized to exactly the NV12 frame size and fully overwritten.
void ConvertI420ToNv12(const uint8_t* i420, uint32_t width, uint32_t height, std::vector<uint8_t>& out_nv12);

} // namespace recorder_core
```

- [ ] **Step 4: Write the implementation**

Create `libs/recorder_core/src/yuv_convert.cpp`:

```cpp
#include "yuv_convert.h"

namespace recorder_core {

void ConvertI420ToNv12(const uint8_t* i420, uint32_t width, uint32_t height, std::vector<uint8_t>& out_nv12) {
    const size_t lumaSize = static_cast<size_t>(width) * height;
    const size_t chromaW = width / 2;
    const size_t chromaH = height / 2;
    const size_t chromaPlaneSize = chromaW * chromaH;

    out_nv12.resize(lumaSize + 2 * chromaPlaneSize);

    // Luma: identical layout in both formats.
    std::copy_n(i420, lumaSize, out_nv12.begin());

    const uint8_t* uPlane = i420 + lumaSize;
    const uint8_t* vPlane = uPlane + chromaPlaneSize;
    uint8_t* uv = out_nv12.data() + lumaSize;
    for (size_t i = 0; i < chromaPlaneSize; ++i) {
        uv[2 * i] = uPlane[i];
        uv[2 * i + 1] = vPlane[i];
    }
}

} // namespace recorder_core
```

- [ ] **Step 5: Register the gtest target**

In `libs/recorder_core/CMakeLists.txt`, immediately after the `test_y4m_reader` block from Task 1, add:

```cmake
# test_yuv_convert: pure unit tests for I420->NV12 conversion backing the
# encoder quality matrix harness's probe (no GPU needed).
exosnap_add_gtest(
    NAME test_yuv_convert
    TEST_PREFIX recorder_core.
    SOURCES tests/test_yuv_convert.cpp src/yuv_convert.cpp
)
target_include_directories(test_yuv_convert PRIVATE src)
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target test_yuv_convert --config Debug`
Run: `ctest --test-dir build/windows-x64-debug -C Debug -R test_yuv_convert --output-on-failure`
Expected: all `recorder_core.ConvertI420ToNv12.*` cases PASS.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/src/yuv_convert.h libs/recorder_core/src/yuv_convert.cpp \
        libs/recorder_core/tests/test_yuv_convert.cpp libs/recorder_core/CMakeLists.txt
git commit -m "feat(recorder_core): add a pure I420-to-NV12 converter"
```

---

### Task 3: Elementary stream writer — IVF framing for AV1 (pure, CI-tested)

**Files:**
- Create: `libs/recorder_core/src/elementary_stream_writer.h`
- Create: `libs/recorder_core/src/elementary_stream_writer.cpp`
- Test: `libs/recorder_core/tests/test_elementary_stream_writer.cpp`

**Interfaces:**
- Consumes: nothing beyond the standard library.
- Produces: `recorder_core::BuildIvfFileHeader(width, height, fps_num, fps_den, frame_count) -> std::vector<uint8_t>` (32 bytes),
  `recorder_core::BuildIvfFrameHeader(frame_size_bytes, frame_index) -> std::vector<uint8_t>` (12 bytes).
  Task 5's `main.cpp` calls both by name when `--vcodec av1` is selected; H.264/HEVC skip this file entirely and write `EncodedVideoPacket::bytes` directly (NVENC already emits Annex-B start codes for those two codecs — confirmed by the existing `annexb_to_avcc.cpp`/`annexb_to_hvcc.cpp` converters, which only make sense if the input already has start codes).

- [ ] **Step 1: Write the failing test**

Create `libs/recorder_core/tests/test_elementary_stream_writer.cpp`:

```cpp
#include "../src/elementary_stream_writer.h"

#include <gtest/gtest.h>

namespace recorder_core {
namespace {

uint16_t ReadU16Le(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t ReadU32Le(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
          (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint64_t ReadU64Le(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | b[off + static_cast<size_t>(i)];
    return v;
}

TEST(BuildIvfFileHeader, IsExactly32BytesWithDkifSignature) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    ASSERT_EQ(h.size(), 32u);
    EXPECT_EQ(h[0], 'D');
    EXPECT_EQ(h[1], 'K');
    EXPECT_EQ(h[2], 'I');
    EXPECT_EQ(h[3], 'F');
}

TEST(BuildIvfFileHeader, EncodesVersionAndHeaderLength) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    EXPECT_EQ(ReadU16Le(h, 4), 0u);  // version
    EXPECT_EQ(ReadU16Le(h, 6), 32u); // header length
}

TEST(BuildIvfFileHeader, EncodesAv01FourCc) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    EXPECT_EQ(h[8], 'A');
    EXPECT_EQ(h[9], 'V');
    EXPECT_EQ(h[10], '0');
    EXPECT_EQ(h[11], '1');
}

TEST(BuildIvfFileHeader, EncodesDimensionsFramerateAndFrameCount) {
    const auto h = BuildIvfFileHeader(1920, 1080, 60, 1, 300);
    EXPECT_EQ(ReadU16Le(h, 12), 1920u);
    EXPECT_EQ(ReadU16Le(h, 14), 1080u);
    EXPECT_EQ(ReadU32Le(h, 16), 60u);  // rate
    EXPECT_EQ(ReadU32Le(h, 20), 1u);   // scale
    EXPECT_EQ(ReadU32Le(h, 24), 300u); // frame count
    EXPECT_EQ(ReadU32Le(h, 28), 0u);   // reserved
}

TEST(BuildIvfFrameHeader, IsExactly12BytesWithSizeThenPts) {
    const auto h = BuildIvfFrameHeader(12345, 7);
    ASSERT_EQ(h.size(), 12u);
    EXPECT_EQ(ReadU32Le(h, 0), 12345u);
    EXPECT_EQ(ReadU64Le(h, 4), 7u);
}

TEST(BuildIvfFrameHeader, HandlesLargeFrameIndex) {
    const auto h = BuildIvfFrameHeader(1, 0x1'0000'0001ull);
    EXPECT_EQ(ReadU64Le(h, 4), 0x1'0000'0001ull);
}

} // namespace
} // namespace recorder_core
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target test_elementary_stream_writer --config Debug`
Expected: FAIL — `elementary_stream_writer.h` does not exist yet.

- [ ] **Step 3: Write the header**

Create `libs/recorder_core/src/elementary_stream_writer.h`:

```cpp
#pragma once

// Pure IVF header builders for AV1 elementary-stream output. IVF is the
// simple framed container aomenc/libaom's own tools use for a raw AV1
// bitstream (32-byte file header + one 12-byte header per frame) — needed
// because, unlike NVENC's H.264/HEVC output, raw AV1 OBUs from NVENC are not
// self-delimited the way Annex-B start codes make H.264/HEVC self-delimited.
// H.264/HEVC need no equivalent function here: see elementary_stream_writer's
// caller for why.

#include <cstdint>
#include <vector>

namespace recorder_core {

// Builds the 32-byte IVF file header: "DKIF" signature, u16 version (0), u16
// header length (32), 4-byte FourCC ("AV01"), u16 width, u16 height, u32
// frame-rate numerator ("rate"), u32 frame-rate denominator ("scale"), u32
// frame count, u32 reserved (0). All multi-byte fields little-endian.
std::vector<uint8_t> BuildIvfFileHeader(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                                        uint32_t frame_count);

// Builds the 12-byte IVF per-frame header that precedes each frame's raw
// bitstream bytes: u32 frame size in bytes, then u64 presentation timestamp.
// `frame_index` is used directly as the PTS (the file header already
// declares the frame rate, so a simple 0, 1, 2, ... sequence is enough for
// ffmpeg/ffprobe to reconstruct correct timing).
std::vector<uint8_t> BuildIvfFrameHeader(uint32_t frame_size_bytes, uint64_t frame_index);

} // namespace recorder_core
```

- [ ] **Step 4: Write the implementation**

Create `libs/recorder_core/src/elementary_stream_writer.cpp`:

```cpp
#include "elementary_stream_writer.h"

namespace recorder_core {

namespace {

void AppendU16Le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32Le(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void AppendU64Le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

} // namespace

std::vector<uint8_t> BuildIvfFileHeader(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                                        uint32_t frame_count) {
    std::vector<uint8_t> out;
    out.reserve(32);
    out.push_back('D');
    out.push_back('K');
    out.push_back('I');
    out.push_back('F');
    AppendU16Le(out, 0);  // version
    AppendU16Le(out, 32); // header length
    out.push_back('A');
    out.push_back('V');
    out.push_back('0');
    out.push_back('1');
    AppendU16Le(out, static_cast<uint16_t>(width));
    AppendU16Le(out, static_cast<uint16_t>(height));
    AppendU32Le(out, fps_num); // rate
    AppendU32Le(out, fps_den); // scale
    AppendU32Le(out, frame_count);
    AppendU32Le(out, 0); // reserved
    return out;
}

std::vector<uint8_t> BuildIvfFrameHeader(uint32_t frame_size_bytes, uint64_t frame_index) {
    std::vector<uint8_t> out;
    out.reserve(12);
    AppendU32Le(out, frame_size_bytes);
    AppendU64Le(out, frame_index);
    return out;
}

} // namespace recorder_core
```

- [ ] **Step 5: Register the gtest target**

In `libs/recorder_core/CMakeLists.txt`, immediately after the `test_yuv_convert` block from Task 2, add:

```cmake
# test_elementary_stream_writer: pure unit tests for IVF header framing
# (AV1 elementary-stream output) backing the encoder quality matrix harness's
# probe (no GPU needed).
exosnap_add_gtest(
    NAME test_elementary_stream_writer
    TEST_PREFIX recorder_core.
    SOURCES tests/test_elementary_stream_writer.cpp src/elementary_stream_writer.cpp
)
target_include_directories(test_elementary_stream_writer PRIVATE src)
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target test_elementary_stream_writer --config Debug`
Run: `ctest --test-dir build/windows-x64-debug -C Debug -R test_elementary_stream_writer --output-on-failure`
Expected: all `recorder_core.BuildIvfFileHeader.*` and `recorder_core.BuildIvfFrameHeader.*` cases PASS.

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/src/elementary_stream_writer.h libs/recorder_core/src/elementary_stream_writer.cpp \
        libs/recorder_core/tests/test_elementary_stream_writer.cpp libs/recorder_core/CMakeLists.txt
git commit -m "feat(recorder_core): add pure IVF header builders for AV1 elementary-stream output"
```

---

### Task 4: `probe_encode_file` — CLI probe driving the real NVENC path

**Files:**
- Create: `tools/probes/probe_encode_file/CMakeLists.txt`
- Create: `tools/probes/probe_encode_file/src/main.cpp`
- Modify: `tools/probes/CMakeLists.txt:12` (after the existing `add_subdirectory(probe_nvenc_async)` line)

**Interfaces:**
- Consumes: `ParseY4mHeader`, `I420FrameSize`, `ReadY4mFrame` (Task 1); `ConvertI420ToNv12` (Task 2); `BuildIvfFileHeader`, `BuildIvfFrameHeader` (Task 3); `recorder_core::NvencVideoEncoder` and its `Open/Configure/RegisterSlotTexture/AcquireFreeSlot/EncodeFrame/ReapCompleted/Flush/Destroy` (existing, `nvenc_video_encoder.h`); `recorder_core::VideoCodec`, `NvencPreset`, `RateControlMode`, `ColorMetadata::Sdr709()` (existing, `codec_types.h`/`color_metadata.h`).
- Produces: the `probe_encode_file` executable. Task 6 (the Python script) invokes it as a subprocess and reads its stdout/exit code and the file it writes at `--out`.

- [ ] **Step 1: Write the CMake target**

Create `tools/probes/probe_encode_file/CMakeLists.txt`:

```cmake
# probe_encode_file — dev-only probe: reads a Y4M reference clip, encodes it
# through the real NvencVideoEncoder (the exact class the product uses), and
# writes a raw elementary stream. Backs scripts/dev/encoder_quality_matrix.py.
# Never packaged.
#
# Requires the full recorder_core build (NVENC headers present); skipped otherwise.

if(NOT TARGET recorder_core)
    message(STATUS "probe_encode_file: recorder_core target not available — skipping")
    return()
endif()

add_executable(probe_encode_file
    src/main.cpp
)

target_link_libraries(probe_encode_file PRIVATE
    recorder_core
    exosnap::warnings
    d3d11.lib
)

target_include_directories(probe_encode_file PRIVATE
    "${CMAKE_SOURCE_DIR}/libs/recorder_core/src"
)

target_compile_definitions(probe_encode_file PRIVATE NOMINMAX)

set_property(TARGET probe_encode_file PROPERTY CXX_STANDARD 20)
set_property(TARGET probe_encode_file PROPERTY CXX_STANDARD_REQUIRED ON)
```

- [ ] **Step 2: Wire it into the probes subdirectory**

In `tools/probes/CMakeLists.txt`, after the existing `add_subdirectory(probe_nvenc_async)` line, add:

```cmake
add_subdirectory(probe_encode_file)
```

- [ ] **Step 3: Write `main.cpp`**

Create `tools/probes/probe_encode_file/src/main.cpp`:

```cpp
// probe_encode_file — reads a Y4M reference clip, drives it through the real
// NvencVideoEncoder (the same class video_thread.cpp uses), and writes a raw
// elementary stream: Annex-B concatenation for H.264/HEVC (NVENC already
// emits start-coded NAL units for those two codecs), IVF framing for AV1
// (whose raw OBU output is not self-delimited the way Annex-B is). No muxing
// — ffmpeg can decode either output directly. Backs
// scripts/dev/encoder_quality_matrix.py. Never touches the ExoSnap
// application itself — this is a standalone CLI dev tool.
//
// Usage:
//   probe_encode_file --y4m clip.y4m --out out.h264 --vcodec h264 --preset p4 --rc cq --cq 24
//   probe_encode_file --y4m clip.y4m --out out.ivf  --vcodec av1  --preset p7 --rc vbr --bitrate 8000 --keyint 2
//
// Options:
//   --y4m       <path>   input YUV4MPEG2 file (8-bit 4:2:0 only)
//   --out       <path>   output elementary-stream file (.h264/.h265 Annex-B, .ivf for AV1)
//   --vcodec    av1|h264|hevc
//   --preset    p1..p7   NVENC speed/quality preset (default p4)
//   --rc        cq|vbr|cbr  rate-control mode (default cq)
//   --cq        <n>      CQ value 1-51, used when --rc cq (default 24)
//   --bitrate   <kbps>   target bitrate, used when --rc vbr|cbr (default 6000)
//   --keyint    <secs>   keyframe interval in seconds (default 2.0)
//   --bframes   <n>      accepted but NOT YET applied by the encoder — printed as such
//   --lookahead           accepted but NOT YET applied by the encoder — printed as such
//   --temporal-aq         accepted but NOT YET applied by the encoder — printed as such
//
// Exit code 0 on success; prints one summary line and returns 1 on any failure.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "elementary_stream_writer.h"
#include "nvenc_video_encoder.h"
#include "y4m_reader.h"
#include "yuv_convert.h"

#include <recorder_core/codec_types.h>
#include <recorder_core/color_metadata.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace recorder_core;
using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string y4m_path;
    std::string out_path;
    VideoCodec vcodec = VideoCodec::Av1Nvenc;
    NvencPreset preset = NvencPreset::P4;
    RateControlMode rc = RateControlMode::ConstantQuality;
    uint32_t cq = 24;
    uint32_t bitrate_kbps = 6000;
    float keyint_secs = 2.0f;
    int bframes = 0;
    bool lookahead = false;
    bool temporal_aq = false;
};

bool ParseVideoCodec(const std::string& s, VideoCodec& out) {
    if (s == "av1") {
        out = VideoCodec::Av1Nvenc;
        return true;
    }
    if (s == "h264") {
        out = VideoCodec::H264Nvenc;
        return true;
    }
    if (s == "hevc" || s == "h265") {
        out = VideoCodec::HevcNvenc;
        return true;
    }
    return false;
}

bool ParsePreset(const std::string& s, NvencPreset& out) {
    static const std::pair<const char*, NvencPreset> kPresets[] = {
        {"p1", NvencPreset::P1}, {"p2", NvencPreset::P2}, {"p3", NvencPreset::P3}, {"p4", NvencPreset::P4},
        {"p5", NvencPreset::P5}, {"p6", NvencPreset::P6}, {"p7", NvencPreset::P7},
    };
    for (const auto& [name, val] : kPresets) {
        if (s == name) {
            out = val;
            return true;
        }
    }
    return false;
}

bool ParseRateControl(const std::string& s, RateControlMode& out) {
    if (s == "cq") {
        out = RateControlMode::ConstantQuality;
        return true;
    }
    if (s == "vbr") {
        out = RateControlMode::VariableBitrate;
        return true;
    }
    if (s == "cbr") {
        out = RateControlMode::ConstantBitrate;
        return true;
    }
    return false;
}

bool ParseOptions(int argc, char** argv, Options& out, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto needValue = [&](std::string& target) -> bool {
            if (i + 1 >= argc) {
                err = "missing value for " + arg;
                return false;
            }
            target = argv[++i];
            return true;
        };

        if (arg == "--y4m") {
            if (!needValue(out.y4m_path))
                return false;
        } else if (arg == "--out") {
            if (!needValue(out.out_path))
                return false;
        } else if (arg == "--vcodec") {
            std::string v;
            if (!needValue(v) || !ParseVideoCodec(v, out.vcodec)) {
                err = "--vcodec requires av1|h264|hevc";
                return false;
            }
        } else if (arg == "--preset") {
            std::string v;
            if (!needValue(v) || !ParsePreset(v, out.preset)) {
                err = "--preset requires p1..p7";
                return false;
            }
        } else if (arg == "--rc") {
            std::string v;
            if (!needValue(v) || !ParseRateControl(v, out.rc)) {
                err = "--rc requires cq|vbr|cbr";
                return false;
            }
        } else if (arg == "--cq") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.cq = static_cast<uint32_t>(std::stoul(v));
        } else if (arg == "--bitrate") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.bitrate_kbps = static_cast<uint32_t>(std::stoul(v));
        } else if (arg == "--keyint") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.keyint_secs = std::stof(v);
        } else if (arg == "--bframes") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.bframes = std::stoi(v);
        } else if (arg == "--lookahead") {
            out.lookahead = true;
        } else if (arg == "--temporal-aq") {
            out.temporal_aq = true;
        } else {
            err = "unknown option " + arg;
            return false;
        }
    }
    if (out.y4m_path.empty() || out.out_path.empty()) {
        err = "--y4m and --out are required";
        return false;
    }
    return true;
}

bool CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &featureLevel, 1,
                                         D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr)) {
        printf("[probe] D3D11CreateDevice failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size < 0)
        return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(out.data(), size);
    return static_cast<bool>(f) || f.eof();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!ParseOptions(argc, argv, opt, err)) {
        printf("[probe] argument error: %s\n", err.c_str());
        return 1;
    }

    printf("[probe] reading %s\n", opt.y4m_path.c_str());
    std::string fileData;
    if (!ReadWholeFile(opt.y4m_path, fileData)) {
        printf("[probe] failed to read %s\n", opt.y4m_path.c_str());
        return 1;
    }

    const auto header = ParseY4mHeader(fileData, err);
    if (!header.has_value()) {
        printf("[probe] y4m header error: %s\n", err.c_str());
        return 1;
    }
    printf("[probe] %ux%u @ %u/%u fps\n", header->width, header->height, header->fps_num, header->fps_den);

    printf("[probe] applied encoder fields: vcodec=%d preset=%d rc=%d cq=%u bitrate_kbps=%u keyint_secs=%.2f\n",
          static_cast<int>(opt.vcodec), static_cast<int>(opt.preset), static_cast<int>(opt.rc), opt.cq,
          opt.bitrate_kbps, opt.keyint_secs);
    if (opt.bframes != 0 || opt.lookahead || opt.temporal_aq) {
        printf("[probe] NOTE: --bframes/--lookahead/--temporal-aq were requested (bframes=%d lookahead=%d "
              "temporal_aq=%d) but NvencVideoEncoder has no setter for them yet — NOT applied. This run measures "
              "the baseline encoder only.\n",
              opt.bframes, opt.lookahead ? 1 : 0, opt.temporal_aq ? 1 : 0);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(device, context))
        return 1;

    NvencVideoEncoder enc;
    enc.SetCodec(opt.vcodec);
    enc.SetPreset(opt.preset);
    enc.SetCq(opt.cq);
    enc.SetRateControl(opt.rc, opt.bitrate_kbps);
    enc.SetKeyframeIntervalSecs(opt.keyint_secs);
    enc.SetColor(ColorMetadata::Sdr709());

    if (!enc.Open(device.Get(), err)) {
        printf("[probe] Open failed: %s\n", err.c_str());
        return 1;
    }
    if (!enc.Configure(header->width, header->height, header->fps_num, header->fps_den, err)) {
        printf("[probe] Configure failed: %s\n", err.c_str());
        return 1;
    }

    constexpr int kSlotCount = 8;
    std::vector<ComPtr<ID3D11Texture2D>> textures(kSlotCount);
    for (int i = 0; i < kSlotCount; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = header->width;
        desc.Height = header->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, textures[static_cast<size_t>(i)].GetAddressOf());
        if (FAILED(hr)) {
            printf("[probe] CreateTexture2D[%d] failed 0x%08lX\n", i, static_cast<unsigned long>(hr));
            return 1;
        }
        if (!enc.RegisterSlotTexture(i, textures[static_cast<size_t>(i)].Get(), err)) {
            printf("[probe] RegisterSlotTexture[%d] failed: %s\n", i, err.c_str());
            return 1;
        }
    }

    std::vector<EncodedVideoPacket> allPackets;
    std::vector<uint8_t> nv12;
    size_t offset = header->header_bytes;
    uint64_t frameIdx = 0;
    bool encodeError = false;

    for (;;) {
        const auto frame = ReadY4mFrame(fileData, offset, header->width, header->height, err);
        if (!frame.has_value()) {
            if (!err.empty()) {
                printf("[probe] frame %llu: %s\n", static_cast<unsigned long long>(frameIdx), err.c_str());
                encodeError = true;
            }
            break; // clean EOF or error — either way, stop reading
        }
        offset = frame->next_offset;

        int32_t slot = enc.AcquireFreeSlot();
        if (slot < 0) {
            std::vector<EncodedVideoPacket> reaped;
            std::string rerr;
            enc.ReapCompleted(reaped, rerr, 50);
            for (auto& p : reaped)
                allPackets.push_back(std::move(p));
            slot = enc.AcquireFreeSlot();
            if (slot < 0) {
                printf("[probe] frame %llu: no free input slot even after ReapCompleted\n",
                      static_cast<unsigned long long>(frameIdx));
                encodeError = true;
                break;
            }
        }

        ConvertI420ToNv12(reinterpret_cast<const uint8_t*>(fileData.data()) + frame->data_offset, header->width,
                          header->height, nv12);
        context->UpdateSubresource(textures[static_cast<size_t>(slot)].Get(), 0, nullptr, nv12.data(), header->width,
                                   0);

        const uint64_t ptsNs =
            frameIdx * 1'000'000'000ull * header->fps_den / (header->fps_num == 0 ? 1 : header->fps_num);
        std::vector<EncodedVideoPacket> pkts;
        std::string encErr;
        if (!enc.EncodeFrame(slot, ptsNs, header->width, header->height, pkts, encErr)) {
            printf("[probe] frame %llu: EncodeFrame failed: %s\n", static_cast<unsigned long long>(frameIdx),
                  encErr.c_str());
            encodeError = true;
            break;
        }
        for (auto& p : pkts)
            allPackets.push_back(std::move(p));

        std::vector<EncodedVideoPacket> reaped;
        std::string rerr;
        if (!enc.ReapCompleted(reaped, rerr, 0)) {
            printf("[probe] frame %llu: ReapCompleted failed: %s\n", static_cast<unsigned long long>(frameIdx),
                  rerr.c_str());
            encodeError = true;
            break;
        }
        for (auto& p : reaped)
            allPackets.push_back(std::move(p));

        ++frameIdx;
    }

    if (!encodeError) {
        std::vector<EncodedVideoPacket> flushed;
        std::string flushErr;
        if (!enc.Flush(flushed, flushErr)) {
            printf("[probe] Flush reported an error: %s\n", flushErr.c_str());
            encodeError = true;
        }
        for (auto& p : flushed)
            allPackets.push_back(std::move(p));
    }

    enc.Destroy();

    if (encodeError) {
        printf("[probe] RESULT: FAIL\n");
        return 1;
    }

    std::ofstream out(opt.out_path, std::ios::binary);
    if (!out) {
        printf("[probe] failed to open %s for writing\n", opt.out_path.c_str());
        return 1;
    }

    if (opt.vcodec == VideoCodec::Av1Nvenc) {
        const auto fileHeader = BuildIvfFileHeader(header->width, header->height, header->fps_num, header->fps_den,
                                                    static_cast<uint32_t>(allPackets.size()));
        out.write(reinterpret_cast<const char*>(fileHeader.data()), static_cast<std::streamsize>(fileHeader.size()));
        for (size_t i = 0; i < allPackets.size(); ++i) {
            const auto& pkt = allPackets[i];
            const auto frameHeader = BuildIvfFrameHeader(static_cast<uint32_t>(pkt.bytes.size()), i);
            out.write(reinterpret_cast<const char*>(frameHeader.data()),
                      static_cast<std::streamsize>(frameHeader.size()));
            out.write(reinterpret_cast<const char*>(pkt.bytes.data()),
                      static_cast<std::streamsize>(pkt.bytes.size()));
        }
    } else {
        // H.264/HEVC: NVENC already emits Annex-B start-coded NAL units —
        // straight concatenation is a valid, directly ffprobe-decodable
        // elementary stream.
        for (const auto& pkt : allPackets)
            out.write(reinterpret_cast<const char*>(pkt.bytes.data()),
                      static_cast<std::streamsize>(pkt.bytes.size()));
    }
    out.close();

    size_t totalBytes = 0;
    for (const auto& pkt : allPackets)
        totalBytes += pkt.bytes.size();

    printf("[probe] frames encoded: %zu, total bytes: %zu, wrote %s\n", allPackets.size(), totalBytes,
          opt.out_path.c_str());
    printf("[probe] RESULT: PASS\n");
    return 0;
}
```

- [ ] **Step 4: Build it**

Run: `cmake --preset windows-x64-debug -DEXOSNAP_BUILD_PROBES=ON`
Run: `cmake --build build/windows-x64-debug --target probe_encode_file --config Debug`
Expected: builds cleanly, produces `build/windows-x64-debug/tools/probes/probe_encode_file/Debug/probe_encode_file.exe`.

- [ ] **Step 5: Manual smoke test with a synthetic Y4M file (dev-only, real GPU — not CI)**

This step needs a real NVENC-capable GPU. Generate a tiny synthetic clip and encode it:

```bash
ffmpeg -f lavfi -i testsrc=size=320x240:rate=30:duration=2 -pix_fmt yuv420p /tmp/smoke.y4m
build/windows-x64-debug/tools/probes/probe_encode_file/Debug/probe_encode_file.exe \
    --y4m /tmp/smoke.y4m --out /tmp/smoke.h264 --vcodec h264 --preset p4 --rc cq --cq 24
ffprobe -hide_banner -show_streams /tmp/smoke.h264
```

Expected: `probe_encode_file` prints `RESULT: PASS` with `frames encoded: 60`; `ffprobe` shows one H.264 video stream at 320x240. Repeat with `--vcodec av1 --out /tmp/smoke.ivf` and confirm `ffprobe` reports an AV1 stream with `format_name=ivf`. Repeat once more with `--vcodec hevc --out /tmp/smoke.h265` and confirm an HEVC stream.

- [ ] **Step 6: Commit**

```bash
git add tools/probes/probe_encode_file/CMakeLists.txt tools/probes/probe_encode_file/src/main.cpp \
        tools/probes/CMakeLists.txt
git commit -m "feat(tools): add probe_encode_file, a dev-only Y4M-to-NVENC-to-elementary-stream probe"
```

---

### Task 5: `encoder_quality_matrix.py` — BD-rate math with self-test (pure, runnable without a GPU)

**Files:**
- Create: `scripts/dev/encoder_quality_matrix.py`

**Interfaces:**
- Consumes: nothing from earlier tasks yet (this step is the pure math + CLI skeleton only; Task 6 adds the subprocess orchestration that calls `probe_encode_file` and `ffmpeg`).
- Produces: `bd_rate(rates_a, metrics_a, rates_b, metrics_b) -> float` (percent; negative = B saves bitrate vs. A at equal quality), reused by Task 6's per-cell comparison loop.

- [ ] **Step 1: Write the script with `--self-test`**

Create `scripts/dev/encoder_quality_matrix.py`:

```python
#!/usr/bin/env python3
"""Encoder quality matrix harness for ExoSnap's NVENC path.

Sweeps preset/rate-control combinations through probe_encode_file, scores
each encode against a reference Y4M clip with an external ffmpeg (libvmaf),
and reports BD-rate (bitrate delta at equal quality) across the sweep.

This is dev-only tooling: it is never run in CI (needs real NVENC hardware
and a local ffmpeg build with libvmaf) and nothing here ships in the product.
See docs/development/encoder-quality-matrix.md for the full workflow.

Self-test (no GPU, no ffmpeg needed):
    python encoder_quality_matrix.py --self-test
"""

import argparse
import math
import sys


def bd_rate(rates_a, metrics_a, rates_b, metrics_b):
    """BD-rate (Bjontegaard-delta rate) between curve A (baseline) and curve B
    (candidate), in percent. Negative means B needs less bitrate than A for
    the same quality (an improvement); positive means B is worse.

    Each curve is >= 4 (rate, metric) points. Rates are log-transformed (the
    standard BD-rate construction: rate-distortion curves are close to linear
    in log(rate) vs. quality), fit with a cubic polynomial, then the integral
    of the fitted log-rate over the metric range common to both curves is
    compared. This mirrors the piecewise log-interpolation approach used by
    the reference BD-rate implementations (e.g. the JCT-VC Excel/Matlab
    tools), reimplemented here with only the Python standard library.
    """
    if len(rates_a) < 4 or len(rates_b) < 4:
        raise ValueError("bd_rate needs at least 4 (rate, metric) points per curve")
    if len(rates_a) != len(metrics_a) or len(rates_b) != len(metrics_b):
        raise ValueError("rates and metrics must be the same length")

    log_rates_a = [math.log10(r) for r in rates_a]
    log_rates_b = [math.log10(r) for r in rates_b]

    coeffs_a = _polyfit3(metrics_a, log_rates_a)
    coeffs_b = _polyfit3(metrics_b, log_rates_b)

    lo = max(min(metrics_a), min(metrics_b))
    hi = min(max(metrics_a), max(metrics_b))
    if hi <= lo:
        raise ValueError("curves do not overlap in quality range")

    integral_a = (_polyint3(coeffs_a, hi) - _polyint3(coeffs_a, lo)) / (hi - lo)
    integral_b = (_polyint3(coeffs_b, hi) - _polyint3(coeffs_b, lo)) / (hi - lo)

    return (10 ** (integral_b - integral_a) - 1) * 100.0


def _polyfit3(x, y):
    """Least-squares cubic fit: returns [c0, c1, c2, c3] for
    y = c0 + c1*x + c2*x^2 + c3*x^3. Stdlib-only (solves the 4x4 normal
    equations directly with Gaussian elimination — no numpy).
    """
    n = len(x)
    powers = [[xi**p for p in range(7)] for xi in x]  # x^0..x^6, needed for the normal equations

    # Normal equations: A^T A c = A^T y, where A's columns are x^0..x^3.
    ata = [[sum(powers[i][a + b] for i in range(n)) for b in range(4)] for a in range(4)]
    aty = [sum(powers[i][a] * y[i] for i in range(n)) for a in range(4)]

    return _solve_linear_system(ata, aty)


def _polyint3(coeffs, x):
    """Definite-integral-to-x of c0 + c1*x + c2*x^2 + c3*x^3."""
    c0, c1, c2, c3 = coeffs
    return c0 * x + c1 * x**2 / 2 + c2 * x**3 / 3 + c3 * x**4 / 4


def _solve_linear_system(a, b):
    """Solves a*x = b for a square matrix `a` (list of rows) via Gaussian
    elimination with partial pivoting. Stdlib-only 4x4 solver — small and
    fixed-size enough that numerical stability is not a practical concern
    for this harness's rate-distortion curves.
    """
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]

    for col in range(n):
        pivot = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot][col]) < 1e-12:
            raise ValueError("singular matrix in bd_rate curve fit")
        m[col], m[pivot] = m[pivot], m[col]
        for r in range(n):
            if r == col:
                continue
            factor = m[r][col] / m[col][col]
            for c in range(col, n + 1):
                m[r][c] -= factor * m[col][c]

    return [m[i][n] / m[i][i] for i in range(n)]


def _self_test():
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    # Identical curves -> ~0% BD-rate.
    rates = [1000, 2000, 4000, 8000]
    metrics = [80, 85, 90, 93]
    same = bd_rate(rates, metrics, rates, metrics)
    check(f"identical curves near 0% (got {same:.4f})", abs(same) < 0.01)

    # Candidate needs exactly half the bitrate at every point for the same
    # quality -> BD-rate close to -50%.
    rates_b = [r / 2 for r in rates]
    half = bd_rate(rates, metrics, rates_b, metrics)
    check(f"half-bitrate curve near -50% (got {half:.2f})", -55.0 < half < -45.0)

    # Candidate needs double the bitrate for the same quality -> positive,
    # roughly symmetric in log-space to the halving case above.
    rates_double = [r * 2 for r in rates]
    double = bd_rate(rates, metrics, rates_double, metrics)
    check(f"double-bitrate curve is positive (got {double:.2f})", double > 45.0)

    # Fewer than 4 points must raise, not silently misbehave.
    try:
        bd_rate([1, 2, 3], [1, 2, 3], rates, metrics)
        failures.append("expected ValueError for <4 points")
    except ValueError:
        pass

    if failures:
        print("SELF-TEST FAILED:")
        for f in failures:
            print(f"  - {f}")
        return False

    print("SELF-TEST PASSED")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="run built-in self-tests and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return 0 if _self_test() else 1

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run the self-test to verify it passes**

Run: `python scripts/dev/encoder_quality_matrix.py --self-test`
Expected: prints `SELF-TEST PASSED` and exits 0.

- [ ] **Step 3: Commit**

```bash
git add scripts/dev/encoder_quality_matrix.py
git commit -m "feat(dev): add BD-rate computation with a self-test for the quality matrix script"
```

---

### Task 6: `encoder_quality_matrix.py` — matrix orchestration (dev-only, real GPU + ffmpeg)

**Files:**
- Modify: `scripts/dev/encoder_quality_matrix.py`

**Interfaces:**
- Consumes: `bd_rate` (Task 5, same file); the `probe_encode_file` executable (Task 4) and an external `ffmpeg` binary, both invoked via `subprocess`.
- Produces: a CSV file and a Markdown summary on disk; no other code depends on this task's output programmatically.

- [ ] **Step 1: Add the matrix-cell and CLI orchestration code**

Append to `scripts/dev/encoder_quality_matrix.py` (before the existing `def main(argv=None):` line, replacing that function, and adding the new pieces above it):

```python
import csv
import datetime
import json
import os
import re
import subprocess
import tempfile


# One measurement point: a specific preset/rate-control/value combination.
class MatrixCell:
    def __init__(self, preset, rc, value):
        self.preset = preset  # "p1".."p7"
        self.rc = rc  # "cq" or "vbr"
        self.value = value  # CQ int (for cq) or bitrate kbps int (for vbr)

    def label(self):
        return f"{self.preset}-{self.rc}-{self.value}"


def default_matrix():
    """The baseline sweep from the workflow doc: P4 and P7, each under CQ
    (the product default) and VBR, at a spread of rate-control points wide
    enough for a 4-point BD-rate curve fit.
    """
    cells = []
    for preset in ("p4", "p7"):
        for cq in (19, 24, 30, 36):
            cells.append(MatrixCell(preset, "cq", cq))
        for kbps in (3000, 6000, 12000, 24000):
            cells.append(MatrixCell(preset, "vbr", kbps))
    return cells


def check_ffmpeg_has_libvmaf(ffmpeg_path):
    result = subprocess.run([ffmpeg_path, "-filters"], capture_output=True, text=True, check=False)
    if "libvmaf" not in result.stdout:
        raise SystemExit(
            f"'{ffmpeg_path} -filters' does not list libvmaf — this ffmpeg build was not compiled with "
            "--enable-libvmaf. See docs/development/encoder-quality-matrix.md for where to get one."
        )


def probe_encode(probe_path, y4m_path, out_path, vcodec, cell):
    args = [
        probe_path,
        "--y4m",
        y4m_path,
        "--out",
        out_path,
        "--vcodec",
        vcodec,
        "--preset",
        cell.preset,
        "--rc",
        cell.rc,
    ]
    if cell.rc == "cq":
        args += ["--cq", str(cell.value)]
    else:
        args += ["--bitrate", str(cell.value)]

    result = subprocess.run(args, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"probe_encode_file failed for {cell.label()}:\n{result.stdout}\n{result.stderr}")
    return out_path


_VMAF_RE = re.compile(r"VMAF score:\s*([\d.]+)")
_SSIM_RE = re.compile(r"All:([\d.]+)")
_PSNR_RE = re.compile(r"average:([\d.]+)")


def measure_quality(ffmpeg_path, distorted_path, reference_path, log_dir, label):
    """Runs ffmpeg's libvmaf filter (which also reports SSIM/PSNR when asked)
    comparing `distorted_path` (the probe's encode, decoded) against the
    original `reference_path` Y4M. Returns a dict with vmaf/ssim/psnr floats.
    """
    vmaf_log = os.path.join(log_dir, f"{label}.vmaf.json")
    filter_arg = (
        f"libvmaf=log_path={vmaf_log}:log_fmt=json:"
        "feature=name=psnr|name=float_ssim"
    )
    args = [
        ffmpeg_path,
        "-hide_banner",
        "-i",
        distorted_path,
        "-i",
        reference_path,
        "-lavfi",
        filter_arg,
        "-f",
        "null",
        "-",
    ]
    result = subprocess.run(args, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg quality measurement failed for {label}:\n{result.stderr}")

    with open(vmaf_log, "r", encoding="utf-8") as f:
        report = json.load(f)
    pooled = report["pooled_metrics"]
    return {
        "vmaf": pooled["vmaf"]["mean"],
        "ssim": pooled.get("float_ssim", {}).get("mean"),
        "psnr": pooled.get("psnr", {}).get("mean"),
    }


def run_matrix(args):
    check_ffmpeg_has_libvmaf(args.ffmpeg)

    with open(args.clip, "r", encoding="utf-8") as f:
        # Y4M header is a single text line; read just enough to log it.
        header_line = f.readline().strip()
    print(f"Reference clip: {args.clip}")
    print(f"Y4M header: {header_line}")

    clip_name = os.path.splitext(os.path.basename(args.clip))[0]
    work_dir = tempfile.mkdtemp(prefix="exosnap_quality_matrix_")
    ext = "ivf" if args.vcodec == "av1" else ("h265" if args.vcodec == "hevc" else "h264")

    rows = []
    for cell in default_matrix():
        out_path = os.path.join(work_dir, f"{clip_name}-{cell.label()}.{ext}")
        print(f"encoding {cell.label()} ...")
        probe_encode(args.probe, args.clip, out_path, args.vcodec, cell)
        bitrate_kbps = os.path.getsize(out_path) * 8 / 1000 / _clip_duration_seconds(args.clip)

        print(f"measuring {cell.label()} ...")
        quality = measure_quality(args.ffmpeg, out_path, args.clip, work_dir, cell.label())

        rows.append(
            {
                "preset": cell.preset,
                "rc": cell.rc,
                "value": cell.value,
                "bitrate_kbps": bitrate_kbps,
                **quality,
            }
        )

    write_report(args.output, args.vcodec, args.clip, rows, args.ffmpeg)
    print(f"Wrote {args.output}.csv and {args.output}.md")


def _clip_duration_seconds(y4m_path):
    """Frame count * frame interval, read from the Y4M header + a frame count
    obtained by dividing the payload size by the per-frame I420 size."""
    with open(y4m_path, "rb") as f:
        header_line = f.readline()
    text = header_line.decode("ascii")
    width = int(re.search(r"W(\d+)", text).group(1))
    height = int(re.search(r"H(\d+)", text).group(1))
    fps_match = re.search(r"F(\d+):(\d+)", text)
    fps = int(fps_match.group(1)) / int(fps_match.group(2))
    frame_size = width * height + 2 * ((width // 2) * (height // 2))
    payload_size = os.path.getsize(y4m_path) - len(header_line)
    frame_count = payload_size // (frame_size + len(b"FRAME\n"))
    return frame_count / fps


def write_report(output_base, vcodec, clip_path, rows, ffmpeg_path):
    csv_path = f"{output_base}.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["preset", "rc", "value", "bitrate_kbps", "vmaf", "ssim", "psnr"])
        writer.writeheader()
        writer.writerows(rows)

    md_path = f"{output_base}.md"
    ffmpeg_version = subprocess.run(
        [ffmpeg_path, "-version"], capture_output=True, text=True, check=False
    ).stdout.splitlines()[0]

    with open(md_path, "w", encoding="utf-8") as f:
        f.write(f"# Encoder quality matrix — {vcodec}\n\n")
        f.write(f"Clip: `{clip_path}`\n\n")
        f.write(f"Date: {datetime.date.today().isoformat()}\n\n")
        f.write(f"ffmpeg: `{ffmpeg_version}`\n\n")
        f.write("| Preset | RC | Value | Bitrate (kbps) | VMAF | SSIM | PSNR |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(
                f"| {r['preset']} | {r['rc']} | {r['value']} | {r['bitrate_kbps']:.0f} | "
                f"{r['vmaf']:.2f} | {r['ssim']:.4f} | {r['psnr']:.2f} |\n"
            )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="run built-in self-tests and exit")
    parser.add_argument("--clip", help="reference Y4M clip")
    parser.add_argument("--vcodec", choices=["av1", "h264", "hevc"], help="codec to sweep")
    parser.add_argument("--probe", default="build/windows-x64-debug/tools/probes/probe_encode_file/Debug/probe_encode_file.exe",
                        help="path to the probe_encode_file executable")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable with libvmaf support")
    parser.add_argument("--output", default="quality-matrix-result", help="output basename (writes .csv and .md)")
    args = parser.parse_args(argv)

    if args.self_test:
        return 0 if _self_test() else 1

    if not args.clip or not args.vcodec:
        parser.error("--clip and --vcodec are required unless --self-test is given")

    run_matrix(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Re-run the self-test to confirm the new imports don't break it**

Run: `python scripts/dev/encoder_quality_matrix.py --self-test`
Expected: still prints `SELF-TEST PASSED` and exits 0 (the new orchestration code is inert until `--clip`/`--vcodec` are given).

- [ ] **Step 3: Manual end-to-end run (dev-only, real GPU + ffmpeg — not CI)**

```bash
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30:duration=3 -pix_fmt yuv420p /tmp/sample.y4m
python scripts/dev/encoder_quality_matrix.py --clip /tmp/sample.y4m --vcodec h264 --output /tmp/matrix-h264
```

Expected: prints one `encoding ...` / `measuring ...` line pair per matrix cell (16 cells: 2 presets x (4 CQ + 4 VBR) points), then `Wrote /tmp/matrix-h264.csv and /tmp/matrix-h264.md`. Open the `.md` file and confirm VMAF/SSIM/PSNR values are present and bitrate increases from CQ 36 -> CQ 19 within each preset.

- [ ] **Step 4: Commit**

```bash
git add scripts/dev/encoder_quality_matrix.py
git commit -m "feat(dev): add matrix orchestration to encoder_quality_matrix.py (probe + ffmpeg/libvmaf)"
```

---

### Task 7: Clip-set preparation + workflow documentation

**Files:**
- Create: `docs/development/encoder-quality-matrix.md`
- Modify: `docs/roadmap.md` (one-line link addition — exact location found in Step 3 below)

**Interfaces:**
- Consumes: nothing (documentation only).
- Produces: nothing consumed by other code; this is the durable reference for running Tasks 4-6 as a workflow.

- [ ] **Step 1: Pick and convert an initial reference clip**

Find an existing test recording to source a clip from (any `.mkv`/`.mp4` produced by prior smoke tests or manual recordings works — the goal is real screen-capture content, not synthetic test patterns, so quality numbers reflect what users actually record):

```bash
ffmpeg -i <existing-recording>.mkv -ss 00:00:05 -t 15 -pix_fmt yuv420p -vf scale=1920:1080 desktop-scroll.y4m
```

Adjust `-ss`/`-t` to land on a representative 10-30s span (e.g. a section with on-screen text/UI motion for a "desktop/text-scroll" clip, or fast on-screen motion for a "gameplay fast" clip if a gameplay recording is available). Repeat for each category the design calls for (fast gameplay, slow gameplay, desktop/text-scroll) as source material becomes available — this step is intentionally repeatable, not a one-time action, so the clip set can grow later without re-deriving the command.

- [ ] **Step 2: Write the workflow doc**

Create `docs/development/encoder-quality-matrix.md`:

```markdown
# Encoder quality matrix: workflow

Dev-only tooling to objectively measure NVENC quality-per-bitrate (SSIM/VMAF/BD-rate)
against ExoSnap's real encoder code path. Nothing here ships in the product — see
`tools/probes/probe_encode_file` and `scripts/dev/encoder_quality_matrix.py`.

This is the NVIDIA-only piece of a larger, cross-vendor 1.0 quality gate the roadmap
reserves (`docs/roadmap.md`) — a down payment on that gate, not the gate itself.

## Prerequisites

- An NVIDIA GPU with NVENC (the same requirement as the product).
- An `ffmpeg` build with `libvmaf` compiled in. A full build from
  <https://www.gyan.dev/ffmpeg/builds/> (or any build whose `ffmpeg -filters` output lists
  `libvmaf`) works — no ExoSnap-specific patches needed. Verify with:

  ```bash
  ffmpeg -filters | grep libvmaf
  ```

  If nothing prints, get a different build; the matrix script checks this itself and refuses
  to run otherwise.
- A local build of `probe_encode_file`: `cmake --preset windows-x64-debug -DEXOSNAP_BUILD_PROBES=ON`
  then `cmake --build build/windows-x64-debug --target probe_encode_file --config Debug`.

## Reference clip set

Y4M (8-bit 4:2:0) clips, 10-30 seconds each, covering:

- Fast gameplay (high motion, frequent scene changes)
- Slow gameplay (low motion, stable scenes)
- Desktop / text-scroll (sharp edges, low motion, the case most sensitive to blocking)

Source clips from real recordings, not synthetic test patterns, so results reflect actual
usage. Convert a section of any existing recording:

```bash
ffmpeg -i <recording>.mkv -ss <start> -t <duration> -pix_fmt yuv420p -vf scale=1920:1080 <name>.y4m
```

This command is the whole clip-set-generation "tool" — repeatable any time a new or better
reference clip is needed; there is no separate script.

## Running the matrix

Per codec, per clip:

```bash
python scripts/dev/encoder_quality_matrix.py \
    --clip desktop-scroll.y4m \
    --vcodec av1 \
    --output docs/development/quality-results/2026-07-24-rtx5070ti-av1-desktop-scroll
```

Repeat for `--vcodec h264`/`hevc` and for each clip. Each run sweeps P4 and P7, each under CQ
(the product default) and VBR, at 4 rate-control points — enough for a BD-rate curve fit.
Writes `<output>.csv` (raw data) and `<output>.md` (human-readable table).

## Result storage

File results under `docs/development/quality-results/<date>-<gpu>-<codec>-<clip>.md` (matching
the `--output` path above) — tracked in git so results are diffable across runs.

## Computing BD-rate between two runs

`bd_rate()` in `scripts/dev/encoder_quality_matrix.py` takes two (bitrate, VMAF) curves — the
baseline and the candidate — and returns the percent bitrate delta at equal quality (negative =
candidate is better). Import it directly from a small script, or extend
`encoder_quality_matrix.py` with a `--compare` mode when a concrete before/after comparison is
needed (not built speculatively here — see the harness design's non-goals).

## Gate rule for shipping an encoder-quality change

A future encoder-quality change (enabling B-frames/lookahead/temporal AQ, or changing an
existing default) may become a shipped default when, over the full reference clip set:

1. Median BD-rate improves by at least 5% in the target rate-control mode, and
2. No single clip regresses by more than 2% BD-rate, and
3. p99 encode latency (measurement infrastructure already shipped) still stays inside the
   frame budget for the target configuration (60 fps -> under ~16 ms).

These thresholds are a deliberate, revisable choice, not a law of nature — changing them is
cheap; not having any threshold at all would be the real mistake.

## What this does not cover

- 10-bit/HDR clips (8-bit only in this pass).
- Multipass/`lookaheadLevel`/UHQ tuning (no measurement basis yet for these newer SDK
  features).
- Cross-vendor comparison (AMD/Intel) — that is the separate, later 1.0 roadmap gate this
  harness is a down payment on.
```

- [ ] **Step 3: Link from the roadmap**

Run: `grep -n "1.0" docs/roadmap.md | head -20` to find the roadmap's 1.0 quality-gate line, then add a one-line reference to `docs/development/encoder-quality-matrix.md` next to it (exact wording depends on the surrounding line found — keep it to a single added sentence, matching the roadmap's existing terse style; do not restructure the surrounding table/list).

- [ ] **Step 4: Commit**

```bash
git add docs/development/encoder-quality-matrix.md docs/roadmap.md
git commit -m "docs: add the encoder quality matrix workflow guide"
```

---

## Self-Review Notes

- **Spec coverage:** D6's three deliverables (`probe_encode_file`, `encoder_quality_matrix.py`, `docs/development/encoder-quality-matrix.md`) map to Tasks 4, 5+6, and 7. D6's CQ+VBR dual-measurement requirement is in `default_matrix()` (Task 6). D7's gate rule is documented verbatim in Task 7's doc. The "honest about unapplied bframes/lookahead/temporal-aq flags" requirement is in Task 4's `main.cpp` printf. The "abort clearly if ffmpeg lacks libvmaf" requirement is `check_ffmpeg_has_libvmaf` in Task 6. The "log ffmpeg version/driver/GPU/probe commit" requirement is partially covered (ffmpeg version, in `write_report`) — driver version, GPU name, and probe commit hash are deferred: no existing dev script in this repo currently queries driver/GPU name programmatically outside the full product build's capability layer, and wiring that in is unrelated scope creep for a probe this size. Flagged here rather than silently dropped; add if a future run's reproducibility actually needs it.
- **Placeholder scan:** no TBD/TODO markers; every step has complete, real code.
- **Type consistency:** `NvencVideoEncoder` setter names (`SetCodec`, `SetPreset`, `SetCq`, `SetRateControl`, `SetKeyframeIntervalSecs`, `SetColor`) match `nvenc_video_encoder.h` exactly (verified against the current header, not assumed). `EncodedVideoPacket` fields (`bytes`, `pts_ns`, `keyframe`) match `packet_types.h` exactly. Function names introduced in Tasks 1-3 (`ParseY4mHeader`, `ReadY4mFrame`, `I420FrameSize`, `ConvertI420ToNv12`, `BuildIvfFileHeader`, `BuildIvfFrameHeader`) are used with identical spelling and signatures in Task 4's `main.cpp`.
