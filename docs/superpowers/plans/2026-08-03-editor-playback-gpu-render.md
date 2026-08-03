# Editor Playback GPU Render Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.
>
> **Task granularity note (explicit user instruction, 2026-08-03):** this plan is written at
> coarse, per-agent task granularity, not the fine one-assertion-per-step TDD shape the
> writing-plans skill defaults to. This mirrors the pattern that already shipped successfully for
> the `IVideoEncoder` refactor (PR #304): a small sequential prep step defines shared
> headers/types on one base commit, then disjoint worktree-isolated agents implement their whole
> section end to end and run the test suite once at the end, not per micro-step. Each task below
> still has concrete file lists, exact interfaces, and real code for the parts that are easy to
> get subtly wrong (struct layouts, HLSL math) — nothing here is a placeholder — but a task's
> internal steps are "implement the whole thing, then test it," not five to ten TDD steps.

**Goal:** Replace the editor's CPU-side YUV→BGRA conversion (and CPU-side HDR10 tone-map) with a
GPU shader pass rendered directly to a native child window, eliminating the per-frame heap
allocation and the conversion cost that makes 4:4:4/4K/240fps unplayable, while also fixing the
40fps-effective-cap judder on ordinary 60fps material.

**Architecture:** `EditPlayerEngine`'s decode thread stops converting and instead hands out
ref-counted decoded planes (a new `RawDecodedVideoFrame`, FFmpeg-agnostic). A new pure-D3D11
class, `EditFrameGpuConverter` (`libs/recorder_core`, UI-agnostic, same shape as `HdrToneMapper`),
uploads those planes as textures and runs a shader pass. A new app-layer class,
`EditPlayerRenderer` (`app/services`, Qt-aware), owns a D3D11 device/swap chain/render thread
behind a native child HWND hosted by `EditPlayerSurface`, mirroring `DxgiPreviewRenderer`/
`PreviewSurface`'s already-proven pattern for the Record page's live preview.

**Tech Stack:** C++20, Qt 6.9 Widgets, D3D11 (HLSL shaders compiled at runtime via `D3DCompile`,
matching every existing GPU shader class in this codebase), FFmpeg (existing decode dependency,
unchanged), GoogleTest + a WARP (software) D3D11 device for headless shader tests.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-08-03-editor-playback-gpu-render-design.md`. Every task below
  implements a named section of it; do not deviate from its Non-goals section.
- `recorder_core` stays UI-agnostic (no Qt types) — this is an existing project rule
  (`CLAUDE.md`), and `edit_player_engine.h`'s own header comment states it explicitly. The new
  `RawDecodedVideoFrame` struct must not include any Qt or FFmpeg type in its public shape (see
  Task 1).
- Follow the codebase's existing GPU-shader-class shape exactly: borrowed (non-owned)
  `ID3D11Device*`/`ID3D11DeviceContext*`, `Init`/`Convert` returning `bool` + `std::string& err`,
  lazy SRV/RTV caching keyed by texture pointer, `winrt::com_ptr` for COM ownership. This is not a
  style preference — `gpu_hdr_tonemap.h`/`.cpp` is the literal template every reviewer will compare
  the new class against.
- No new abstraction/interface/factory layer for dependency injection between the three tracks
  below. The existing codebase does not do this for its other GPU shader classes (`HdrToneMapper`,
  `HdrPqConverter`, `GpuCompositor` are all used as concrete types, not behind an interface), and
  introducing one here would be exactly the "speculative overengineering" `CLAUDE.md` warns
  against. The tracks are decoupled by the prep task's stub implementations instead (see Task 1).

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `libs/recorder_core/include/recorder_core/edit_player_engine.h` | + `DecodedPixelFormat`, `RawDecodedVideoFrame`, `RawVideoFrameCallback`; + `StartPlaybackDecodeRaw`/`DecodeFrameAtRaw` declarations | 1 (declare), 3 (implement) |
| `libs/recorder_core/src/edit_player_engine.cpp` | Real `StartPlaybackDecodeRaw`/`DecodeFrameAtRaw` bodies; FFmpeg decoder thread-count fix | 1 (stub bodies + threading fix), 3 (real bodies) |
| `libs/recorder_core/include/recorder_core/edit_frame_gpu_converter.h` | `EditFrameGpuConverter` public interface | 1 |
| `libs/recorder_core/src/edit_frame_gpu_converter.cpp` | Real shader implementation | 1 (stub), 2 (real) |
| `libs/recorder_core/tests/test_edit_frame_gpu_converter.cpp` | WARP-device shader correctness tests | 2 |
| `libs/recorder_core/tests/test_edit_player_engine.cpp` | + AVFrame-refcount-through-queue test | 3 |
| `app/services/EditPlayerRenderer.h` / `.cpp` | D3D11 device/swap chain/render thread behind a native child HWND | 1 (stub), 4 (real) |
| `app/ui/widgets/EditPlayerSurface.h` / `.cpp` | Becomes a native-child-HWND host (like `PreviewSurface`) instead of a `QWidget::paintEvent` painter | 4 |
| `app/pages/EditExportPage.cpp` / `.h` | Wire the Raw callback into `EditPlayerRenderer` instead of `onDecodedFrameReady(QImage)` | 4 |
| `app/tests/test_edit_export_overlay.cpp` (or sibling test file) | Lifecycle/resize coverage for the new render path | 4 |

---

## Task 1: Prep — shared types, stubs, threading fix (sequential, base commit)

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/edit_player_engine.h`
- Modify: `libs/recorder_core/src/edit_player_engine.cpp`
- Create: `libs/recorder_core/include/recorder_core/edit_frame_gpu_converter.h`
- Create: `libs/recorder_core/src/edit_frame_gpu_converter.cpp` (stub)
- Create: `app/services/EditPlayerRenderer.h`
- Create: `app/services/EditPlayerRenderer.cpp` (stub)
- Modify: `libs/recorder_core/CMakeLists.txt`, `app/CMakeLists.txt` (new source files + test targets)

**Interfaces produced (every later task builds against these signatures verbatim):**

```cpp
// edit_player_engine.h — additive, alongside the existing BGRA
// DecodedVideoFrame/VideoFrameCallback/StartPlaybackDecode/DecodeFrameAt, which
// stay untouched and in production use until Task 5 deletes them.

enum class DecodedPixelFormat : uint8_t {
    Yuv420P8,  // AV_PIX_FMT_YUV420P
    Yuv420P10, // AV_PIX_FMT_YUV420P10LE (10-bit codes in [0,1023], no P010 <<6 justification)
    Yuv444P8,  // AV_PIX_FMT_YUV444P
};

// One decoded video frame, NOT yet color-converted: the raw planes plus enough
// metadata for a GPU converter to do it. Deliberately FFmpeg-free (no AVFrame*,
// no libavutil types) so this header stays includable from Qt/app code without
// pulling in FFmpeg headers — see `backing_frame` below.
struct RawDecodedVideoFrame {
    int64_t pts_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DecodedPixelFormat format = DecodedPixelFormat::Yuv420P8;
    // Row pitches in bytes, as reported by the decoder (may exceed the
    // tight width-derived size when the source buffer is padded) -- same
    // convention as FullPlanarYuv420Frame/FullPlanar444Frame in yuv_to_bgra.h.
    uint32_t y_stride_bytes = 0;
    uint32_t u_stride_bytes = 0;
    uint32_t v_stride_bytes = 0;
    const uint8_t* y_plane = nullptr;
    const uint8_t* u_plane = nullptr;
    const uint8_t* v_plane = nullptr;
    // True for a natively-HDR10 (PQ) source that needs the tone-map path
    // rather than the ordinary matrix/range conversion -- mirrors
    // IsPqTonemapSource in edit_player_engine.cpp. Only meaningful when
    // format == Yuv420P10.
    bool is_pq_source = false;
    // Color matrix/range to convert with (from the container's own tags, or
    // ColorMetadata::Sdr709() when unspecified) -- same meaning as today's
    // YuvToBgraParams-driven BGRA path.
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    ColorRange range = ColorRange::Limited;
    // Keeps the underlying decoder buffer (an FFmpeg AVFrame's ref-counted
    // data) alive for as long as any copy of this struct references the plane
    // pointers above. The pointee is meaningless to callers and MUST NOT be
    // interpreted -- it exists only so the shared_ptr's deleter runs
    // av_frame_free when the last reference drops. Callers must not retain
    // y_plane/u_plane/v_plane past this frame's own lifetime.
    std::shared_ptr<void> backing_frame;
};

using RawVideoFrameCallback = std::function<void(RawDecodedVideoFrame)>;

// Added to class EditPlayerEngine, alongside the existing DecodeFrameAt/
// StartPlaybackDecode (unchanged). Same threading/pacing contract as those
// (see the existing StartPlaybackDecode doc comment) -- only the payload
// differs.
[[nodiscard]] std::optional<RawDecodedVideoFrame> DecodeFrameAtRaw(int64_t target_us);
void StartPlaybackDecodeRaw(int64_t start_us, RawVideoFrameCallback on_video, AudioBlockCallback on_audio,
                            std::function<int64_t()> current_media_time_us);
```

```cpp
// edit_frame_gpu_converter.h (new file, namespace recorder_core)
#pragma once

#include <recorder_core/edit_player_engine.h> // RawDecodedVideoFrame

#include <d3d11.h>
#include <winrt/base.h>

#include <string>
#include <unordered_map>

namespace recorder_core {

// GPU replacement for yuv_to_bgra.h's CPU conversion, for the editor playback
// path only. Same shape as HdrToneMapper (gpu_hdr_tonemap.h): borrowed
// device/context, Init/Convert, lazy SRV/RTV caching. Uploads a
// RawDecodedVideoFrame's planes as textures and renders the matching
// conversion (+ HDR10 PQ tone-map when frame.is_pq_source) into dst.
//
// Threading: single-thread, like HdrToneMapper -- the caller's render thread
// owns every call.
class EditFrameGpuConverter {
  public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& err);

    // Uploads frame's planes (reusing/resizing cached per-plane textures only
    // when format or dimensions change from the previous call) and renders
    // the conversion into dst, a BGRA8 (DXGI_FORMAT_B8G8R8A8_UNORM) render
    // target of exactly frame.width x frame.height. hdr_peak_scale is the
    // display peak in reference-white multiples (HdrPeakScale()); ignored
    // unless frame.is_pq_source.
    bool Convert(const RawDecodedVideoFrame& frame, ID3D11Texture2D* dst, float hdr_peak_scale, std::string& err);

  private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    // Task 2 fills in: shaders, per-plane textures/SRVs, sampler, constant
    // buffers. Left undeclared here on purpose -- Task 2 owns this file's
    // private section entirely; nothing outside this class reaches into it.
};

} // namespace recorder_core
```

**Prep stub body** (`edit_frame_gpu_converter.cpp` — compiles, runs, does nothing color-correct
yet; Task 2 replaces this file's contents entirely):

```cpp
#include <recorder_core/edit_frame_gpu_converter.h>

namespace recorder_core {

bool EditFrameGpuConverter::Init(ID3D11Device* device, ID3D11DeviceContext* context, std::string& err) {
    if (device == nullptr || context == nullptr) {
        err = "EditFrameGpuConverter::Init invalid arguments";
        return false;
    }
    device_ = device;
    context_ = context;
    return true;
}

// STUB (Task 1 prep only): clears dst to a fixed debug color instead of
// converting, so Task 4 can build and test the render pipeline (child HWND,
// swap chain, present loop, resize) before Task 2's real shaders land. Task 2
// replaces this whole file.
bool EditFrameGpuConverter::Convert(const RawDecodedVideoFrame& frame, ID3D11Texture2D* dst, float /*hdr_peak_scale*/,
                                    std::string& err) {
    if (context_ == nullptr || dst == nullptr || frame.width == 0 || frame.height == 0) {
        err = "EditFrameGpuConverter::Convert called before Init or with invalid arguments";
        return false;
    }
    winrt::com_ptr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateRenderTargetView(dst, nullptr, rtv.put()))) {
        err = "EditFrameGpuConverter::Convert CreateRenderTargetView failed";
        return false;
    }
    const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f}; // unmissable stub marker
    context_->ClearRenderTargetView(rtv.get(), magenta);
    return true;
}

} // namespace recorder_core
```

**`EditPlayerRenderer` stub** (`app/services/EditPlayerRenderer.h`/`.cpp` — Task 4 replaces the
body, keeps the public interface):

```cpp
// app/services/EditPlayerRenderer.h
#pragma once

#include <recorder_core/edit_frame_gpu_converter.h>
#include <recorder_core/edit_player_engine.h>

#include <cstdint>
#include <memory>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

namespace exosnap {

// GPU render path for the Edit page's video player, hosted by a native child
// HWND created by EditPlayerSurface -- same relationship DxgiPreviewRenderer
// has with PreviewSurface, deliberately much smaller (no capture graph, no
// webcam PiP, no cursor sprite, no snapshot path).
class EditPlayerRenderer {
  public:
    EditPlayerRenderer();
    ~EditPlayerRenderer();
    EditPlayerRenderer(const EditPlayerRenderer&) = delete;
    EditPlayerRenderer& operator=(const EditPlayerRenderer&) = delete;

    bool Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight);
    void Resize(uint32_t hwndWidth, uint32_t hwndHeight);
    // Presents one decoded frame. Thread-safety/threading model: Task 4 decides
    // and documents here (own render thread vs. caller's thread) as part of its
    // implementation -- not fixed by this stub.
    void PresentFrame(recorder_core::RawDecodedVideoFrame frame, float hdr_peak_scale);
    void ShowPlaceholder(const std::wstring& text);
    void Shutdown();

  private:
    HWND parentHwnd_ = nullptr;
    HWND childHwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    std::unique_ptr<recorder_core::EditFrameGpuConverter> converter_;
};

} // namespace exosnap
```

```cpp
// app/services/EditPlayerRenderer.cpp (Task 1 stub -- creates the child HWND
// and a swap chain, but PresentFrame just clears to black via the stub
// converter's debug color; no shader work happens here yet)
#include "EditPlayerRenderer.h"

#include <d3d11.h>
#include <dxgi1_2.h>

namespace exosnap {

EditPlayerRenderer::EditPlayerRenderer() = default;
EditPlayerRenderer::~EditPlayerRenderer() { Shutdown(); }

bool EditPlayerRenderer::Initialize(HWND parentHwnd, uint32_t hwndWidth, uint32_t hwndHeight) {
    // Task 4 implements real child-HWND creation (mirror
    // PreviewSurface::tryStartDxgiPreview's WA_NativeWindow/winId() call site
    // and DxgiPreviewRenderer::InitD3D11/InitSwapChain for the D3D11 device +
    // swap chain setup) and DPI-aware sizing. This stub intentionally does
    // nothing so Task 1 stays small; Task 4 owns this file.
    parentHwnd_ = parentHwnd;
    (void)hwndWidth;
    (void)hwndHeight;
    return false; // honestly reports "not yet implemented" rather than faking success
}

void EditPlayerRenderer::Resize(uint32_t, uint32_t) {}
void EditPlayerRenderer::PresentFrame(recorder_core::RawDecodedVideoFrame, float) {}
void EditPlayerRenderer::ShowPlaceholder(const std::wstring&) {}
void EditPlayerRenderer::Shutdown() {}

} // namespace exosnap
```

**FFmpeg decoder threading fix** (small, independent, done here rather than as its own agent
track per the user's instruction to keep it out of the GPU core path but fold it into this spec):
in `edit_player_engine.cpp`, find the `avcodec_open2` call for the video decoder (opened with no
`thread_count` set today per `probe_edit_playback` step E). Set it before opening:

```cpp
codec_ctx->thread_count = static_cast<int>(std::thread::hardware_concurrency());
if (codec_ctx->thread_count <= 0) {
    codec_ctx->thread_count = 1;
}
```

- [ ] **Step 1:** Add `DecodedPixelFormat`, `RawDecodedVideoFrame`, `RawVideoFrameCallback`, and
      the two new method declarations to `edit_player_engine.h` exactly as above. Add trivial
      bodies to `edit_player_engine.cpp`'s `Impl` (`DecodeFrameAtRaw` returns `std::nullopt`;
      `StartPlaybackDecodeRaw` is a no-op — both honestly report "not implemented" rather than
      faking data, matching this plan's stub convention).
- [ ] **Step 2:** Apply the FFmpeg `thread_count` fix at the video decoder's `avcodec_open2` call
      site.
- [ ] **Step 3:** Create `edit_frame_gpu_converter.h`/`.cpp` exactly as above; add both to
      `libs/recorder_core/CMakeLists.txt`'s source list and a new
      `test_edit_frame_gpu_converter` test target (empty test file with one placeholder-free smoke
      test: construct `EditFrameGpuConverter`, `Init()` against a WARP device — see Task 2 for how
      to create one — assert it returns `true`).
- [ ] **Step 4:** Create `app/services/EditPlayerRenderer.h`/`.cpp` exactly as above; add to
      `app/CMakeLists.txt`.
- [ ] **Step 5:** Full build (`cmake --build build/windows-x64-debug --config Debug`) and full
      existing test suite (`ctest --test-dir build/windows-x64-debug -C Debug -R
      "recorder_core\.|edit_export"`) — must be unchanged/green; this task adds new code but
      changes no existing behavior.
- [ ] **Step 6:** Commit: `prep(editor-playback): add GPU converter/renderer seams + FFmpeg decoder threading fix`.

---

## Task 2: GPU color-conversion shaders (worktree, parallel, Opus)

**Depends on:** Task 1's `edit_frame_gpu_converter.h` (interface only — do not wait for Tasks 3/4).

**Files:**
- Modify (replace stub body entirely): `libs/recorder_core/src/edit_frame_gpu_converter.cpp`
- Create: `libs/recorder_core/tests/test_edit_frame_gpu_converter.cpp`

**Reference material to read first:** `libs/recorder_core/src/gpu_hdr_tonemap.cpp` (the exact
structural template: `D3DCompile`, `CreateVertexShader`/`CreatePixelShader`, SRV/RTV caching,
sampler + constant buffer creation, the `Draw(3, 0)` full-screen-triangle convention, unbinding
after draw) and `libs/recorder_core/tests/test_gpu_hdr_tonemap.cpp` (the WARP-device test
pattern this task's tests must follow). `libs/recorder_core/src/yuv_to_bgra.cpp`'s `WeightsFor`/
`ComputeCoefs` (the exact matrix/range math to mirror, in float instead of fixed-point) and
`libs/recorder_core/src/hdr_preview.cpp`/`hdr_pq.h`/`hdr_tonemap.h` (the exact HDR10 PQ tone-map
chain to port).

**Interfaces produced:** none beyond what Task 1 already declared in the header — this task only
fills in the `.cpp`.

### Shader 1: generic SDR YUV→BGRA (covers all three `DecodedPixelFormat` values)

One shader variant handles `Yuv420P8`, `Yuv420P10`, and `Yuv444P8`: all three are 3-plane
(Y/U/V) formats where only the per-clip matrix/range coefficients and the raw sample's max code
value differ, and GPU texture sampling is already resolution-normalized (a 4:2:0 chroma plane at
half resolution and a 4:4:4 chroma plane at full resolution are addressed identically via `Load()`
at `pixelPos / chromaSubsample`, where `chromaSubsample` is 2 for 4:2:0 and 1 for 4:4:4 — computed
once on the CPU side from `frame.format`, not branched in the shader).

Upload each plane as a `DXGI_FORMAT_R8_UINT` (8-bit) or `DXGI_FORMAT_R16_UINT` (10-bit) texture
(UINT, not UNORM — the 10-bit samples are plain values in `[0, 1023]`, not left-justified like
P010, so automatic UNORM normalization by 65535 would be wrong; reading raw integers and
normalizing by the correct `max_code` in the shader avoids that entirely, and also matches the
existing CPU code's exact `DequantY10Limited`-style handling of "no `<<6` justification").

Compute the constant-buffer coefficients on the CPU exactly like `ComputeCoefs` in
`yuv_to_bgra.cpp`, but as plain floats (no fixed-point shift) and pre-divided by `max_code` so the
shader's output is directly in `[0, 1]`:

```cpp
// Mirrors yuv_to_bgra.cpp's WeightsFor/ComputeCoefs. Called once per Convert()
// when frame.matrix/frame.range/frame.format changed since the last call.
struct GpuYuvConstants {
    float c_rv, c_gu, c_gv, c_bu; // matrix weights, already divided by max_code
    float y_scale;                // also pre-divided by max_code
    float y_off, c_off;           // raw code-unit offsets (16/64 luma, 128/512 chroma)
    float max_code;               // 255.0 (8-bit) or 1023.0 (10-bit)
};
```

Pixel shader (register layout matches `HdrToneMapper`'s convention: `t0`/`s0`/`b0`, extended to
three textures):

```hlsl
Texture2D<uint> YPlane : register(t0);
Texture2D<uint> UPlane : register(t1);
Texture2D<uint> VPlane : register(t2);

cbuffer YuvToBgraConstants : register(b0) {
    float c_rv, c_gu, c_gv, c_bu;
    float y_scale;
    float y_off, c_off, max_code;
};

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    uint chromaW, chromaH, lumaW, lumaH, dummy;
    YPlane.GetDimensions(lumaW, lumaH);
    UPlane.GetDimensions(chromaW, chromaH);
    int2 px = int2(position.xy);
    int2 cpx = (chromaW == lumaW) ? px : (px / 2); // 4:4:4 vs 4:2:0

    float y = float(YPlane.Load(int3(px, 0)));
    float u = float(UPlane.Load(int3(cpx, 0)));
    float v = float(VPlane.Load(int3(cpx, 0)));

    float yn = (y - y_off) * y_scale;
    float un = (u - c_off) * (y_scale * 0.0 + 1.0); // placeholder killed below -- see note
    // NOTE for implementer: un/vn must use the SAME per-channel scale ComputeCoefs
    // folds into c_rv/c_gu/c_gv/c_bu already (rv*c_scale, gu*c_scale, etc, each
    // pre-divided by max_code) -- do not apply a separate chroma scale here. Fix
    // this pixel shader to:
    //   float un = u - c_off;
    //   float vn = v - c_off;
    //   float r = yn + c_rv * vn;
    //   float g = yn - c_gu * un - c_gv * vn;
    //   float b = yn + c_bu * un;
    // (c_rv/c_gu/c_gv/c_bu already carry the /max_code scale from the CPU side).
    float un_fixed = u - c_off;
    float vn_fixed = v - c_off;
    float r = yn + c_rv * vn_fixed;
    float g = yn - c_gu * un_fixed - c_gv * vn_fixed;
    float b = yn + c_bu * un_fixed;
    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
```

(The plan calls out the un/vn scale explicitly because it is the one place a copy-paste from
`ComputeCoefs` is easy to get subtly wrong — the CPU version applies `c_scale` once, when the
matrix weight is folded in, not twice.)

Vertex shader: identical full-screen-triangle trick already used by `gpu_hdr_tonemap.cpp`'s
`kVertexShaderSrc` — reuse that exact HLSL source (it is generic, does not depend on tone-mapping).

### Shader 2: HDR10 PQ tone-map (used only when `frame.is_pq_source`)

Same Y/U/V texture inputs (always `Yuv420P10`, `R16_UINT`), but ports the `hdr_preview.cpp` chain
(`DequantY10Limited`/`DequantC10Limited` → `YcbcrToPqRgb` → `PqEotf` → `Bt2020ToBt709` →
`HdrToneMapChannel` → `Bt709Oetf`) instead of the linear matrix:

```hlsl
Texture2D<uint> YPlane : register(t0);
Texture2D<uint> UPlane : register(t1); // Cb
Texture2D<uint> VPlane : register(t2); // Cr

cbuffer PqTonemapConstants : register(b0) {
    float peak_scale; // HdrPeakScale(), >= 1.0
    float pad0, pad1, pad2;
};

static const float kKr2020 = 0.2627;
static const float kKb2020 = 0.0593;
static const float kKg2020 = 0.6780;
static const float kPqM1 = 0.1593017578125;
static const float kPqM2 = 78.84375;
static const float kPqC1 = 0.8359375;
static const float kPqC2 = 18.8515625;
static const float kPqC3 = 18.6875;
static const float kPqLinearToScrgb = 125.0; // 10000.0 / 80.0
static const float3x3 kBt2020ToBt709 = float3x3(
     1.6604910023, -0.5876411389, -0.0728498633,
    -0.1245504746,  1.1328998971, -0.0083494226,
    -0.0181507634, -0.1005788980,  1.1187296614
);

float PqEotf(float signal) {
    float n = saturate(signal);
    float np = pow(n, 1.0 / kPqM2);
    float num = np - kPqC1;
    float den = kPqC2 - kPqC3 * np;
    float base = (num > 0.0 && den > 0.0) ? (num / den) : 0.0;
    return pow(base, 1.0 / kPqM1);
}

float HdrToneMapChannel(float x, float peak) {
    const float knee = 0.80;
    x = max(x, 0.0);
    if (x <= knee || peak <= knee) return min(x, 1.0);
    float head = 1.0 - knee;
    float maxExcess = peak - knee;
    float e = x - knee;
    float s = e * (1.0 + e / (maxExcess * maxExcess)) / (1.0 + e);
    return min(knee + head * s, 1.0);
}

float Bt709Oetf(float l) {
    l = saturate(l);
    return l < 0.018 ? 4.5 * l : 1.099 * pow(l, 0.45) - 0.099;
}

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int2 px = int2(position.xy);
    int2 cpx = px / 2; // native HDR10 is always 4:2:0
    float yv = (float(YPlane.Load(int3(px, 0))) - 64.0) / 876.0;
    float cbv = (float(UPlane.Load(int3(cpx, 0))) - 512.0) / 896.0;
    float crv = (float(VPlane.Load(int3(cpx, 0))) - 512.0) / 896.0;

    float rp = yv + crv * (2.0 * (1.0 - kKr2020));
    float bp = yv + cbv * (2.0 * (1.0 - kKb2020));
    float gp = (yv - kKr2020 * rp - kKb2020 * bp) / kKg2020;

    float3 lin2020 = float3(PqEotf(rp), PqEotf(gp), PqEotf(bp));
    float3 lin709 = mul(kBt2020ToBt709, lin2020);

    float3 sdrLinear = float3(
        HdrToneMapChannel(lin709.r * kPqLinearToScrgb, peak_scale),
        HdrToneMapChannel(lin709.g * kPqLinearToScrgb, peak_scale),
        HdrToneMapChannel(lin709.b * kPqLinearToScrgb, peak_scale));

    float3 outRgb = float3(Bt709Oetf(sdrLinear.r), Bt709Oetf(sdrLinear.g), Bt709Oetf(sdrLinear.b));
    return float4(outRgb, 1.0);
}
```

`EditFrameGpuConverter::Convert` selects shader 1 or shader 2 based on `frame.is_pq_source`,
(re)creates per-plane textures only when `frame.format`/`frame.width`/`frame.height` differ from
the previous call (cache them as private members, same idea as `HdrToneMapper`'s `srv_cache_`/
`rtv_cache_` but keyed by plane role, not by external texture pointer, since these textures are
owned by this class), and follows `HdrToneMapper::Convert`'s exact draw sequence (`OMSetRenderTargets`
→ `RSSetViewports` → `IASetPrimitiveTopology(TRIANGLELIST)` → `VSSetShader`/`PSSetShader` →
`PSSetConstantBuffers` → `PSSetShaderResources` (three slots) → `Draw(3, 0)` → unbind).

**Testing:**

- [ ] **Step 1:** Write `test_edit_frame_gpu_converter.cpp` using the exact WARP-device helper
      from `test_gpu_hdr_tonemap.cpp` (`D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, ...)`).
      For each of the three `DecodedPixelFormat` values: build a small synthetic
      `RawDecodedVideoFrame` (e.g. 16x16, deterministic non-constant Y/U/V values covering the
      full code range, matching how `probe_edit_playback`'s `StepC`/`StepC2` seed their synthetic
      buffers), run it through `EditFrameGpuConverter::Convert` into a staging-readable render
      target, read it back, and compare pixel-for-pixel (small tolerance, e.g. `<= 2` per channel
      for rounding, matching `test_gpu_hdr_tonemap.cpp`'s tolerance philosophy) against
      `ConvertFullPlanarYuv420ToBgraScalar`/`ConvertFullPlanar444ToBgraScalar` on the same input.
      Also test `is_pq_source == true` against `P010PqMonitorConverter::Convert` at a couple of
      `peak_scale` values (1.0, 12.5 for a 1000-nit display).
      Cover both `MatrixCoefficients::Bt709` and `Bt601` to prove the constant buffer actually
      varies with the clip's tagged matrix, not just BT.709.
- [ ] **Step 2:** Implement the real `edit_frame_gpu_converter.cpp` (shaders above, texture
      upload/caching, constant-buffer computation mirroring `ComputeCoefs`) until all tests pass.
- [ ] **Step 3:** Run `ctest --test-dir build/windows-x64-debug -C Debug -R test_edit_frame_gpu_converter`
      and the full `recorder_core` suite once, confirm nothing else regressed.
- [ ] **Step 4:** Commit: `feat(edit): GPU YUV->BGRA + HDR10 tone-map shader for editor playback`.

---

## Task 3: Decode-side raw frame path (worktree, parallel, Sonnet)

**Depends on:** Task 1's `RawDecodedVideoFrame`/`RawVideoFrameCallback`/method declarations only.
Does not depend on Task 2 or Task 4.

**Files:**
- Modify: `libs/recorder_core/src/edit_player_engine.cpp` (replace the Task 1 stub bodies of
  `DecodeFrameAtRaw`/`StartPlaybackDecodeRaw` with real implementations)
- Modify: `libs/recorder_core/tests/test_edit_player_engine.cpp`

**What changes:** the existing `ConvertToDecodedFrame` (BGRA path, `edit_player_engine.cpp:702-758`)
stays untouched and in use by the old `StartPlaybackDecode`/`DecodeFrameAt`. Alongside it, a new
`WrapRawDecodedFrame` function produces a `RawDecodedVideoFrame` directly from a held `AVFrame*`
via ref-counting instead of converting:

```cpp
// New helper, parallel to ConvertToDecodedFrame but producing the raw type.
RawDecodedVideoFrame WrapRawDecodedFrame(AVFrame* frame, int64_t pts_us, MatrixCoefficients matrix,
                                         ColorRange range, bool is_pq_source) {
    RawDecodedVideoFrame out;
    out.pts_us = pts_us;
    out.width = static_cast<uint32_t>(frame->width);
    out.height = static_cast<uint32_t>(frame->height);
    out.matrix = matrix;
    out.range = range;
    out.is_pq_source = is_pq_source;
    out.format = (frame->format == AV_PIX_FMT_YUV444P)      ? DecodedPixelFormat::Yuv444P8
                 : (frame->format == AV_PIX_FMT_YUV420P10LE) ? DecodedPixelFormat::Yuv420P10
                                                              : DecodedPixelFormat::Yuv420P8;
    out.y_plane = frame->data[0];
    out.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
    out.u_plane = frame->data[1];
    out.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
    out.v_plane = frame->data[2];
    out.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);

    // Ref-count the frame instead of copying its pixel data: av_frame_alloc a
    // fresh AVFrame*, av_frame_ref it to the decoded frame (bumps the buffer
    // refcount, no pixel copy), then hand it to shared_ptr<void> with a
    // deleter that av_frame_free's it. This is the whole reason RawDecodedVideoFrame
    // exists -- ConvertToDecodedFrame's `new uint8_t[bgra_bytes]` per frame is
    // gone; this path allocates only an AVFrame struct (a few hundred bytes),
    // not a multi-megabyte pixel buffer.
    AVFrame* ref = av_frame_alloc();
    av_frame_ref(ref, frame);
    out.backing_frame = std::shared_ptr<void>(ref, [](void* p) {
        AVFrame* f = static_cast<AVFrame*>(p);
        av_frame_free(&f);
    });
    return out;
}
```

`StartPlaybackDecodeRaw`/`DecodeFrameAtRaw` are structurally identical to the existing
`StartPlaybackDecode`/`DecodeFrameAt` (same demux/decode-thread topology from the 2026-08-01
design, same clock-gated skip decision, same queue backpressure) — they call `WrapRawDecodedFrame`
where the existing code calls `ConvertToDecodedFrame`, and the frame queue's element type changes
from `DecodedVideoFrame` to `RawDecodedVideoFrame` for this new pair of entry points only (the old
pair keeps its own separately-typed queue, since both code paths coexist until Task 5).

**Testing:**

- [ ] **Step 1:** Add a test to `test_edit_player_engine.cpp`: open a real short fixture clip
      (e.g. `.workspace/test-fixtures/audiotest_60fps.mkv`), call `StartPlaybackDecodeRaw`,
      collect a handful of delivered `RawDecodedVideoFrame`s, and assert: `backing_frame` is
      non-null for each, `y_plane`/`u_plane`/`v_plane` are non-null and readable (e.g. checksum
      the Y plane's first row) for as long as the `RawDecodedVideoFrame` copy is kept alive, and
      — the actual regression this guards — hold onto ONE frame's `RawDecodedVideoFrame` past
      `StopPlaybackDecode()`/engine destruction and confirm its planes are still valid (proves the
      `shared_ptr` keeps the decoder's buffer alive independent of engine teardown, the same
      guarantee the old `shared_ptr<uint8_t[]>` BGRA path already gave callers).
- [ ] **Step 2:** Add a test asserting `DecodeFrameAtRaw` on a natively-HDR10 fixture (if one
      exists in `.workspace/test-fixtures`; if not, note this as a gap for the live-verify step —
      do not fabricate one) returns `is_pq_source == true` and `format == Yuv420P10`.
- [ ] **Step 3:** Add a probe-level or unit assertion for the Task 1 threading fix: extend
      `probe_edit_playback`'s step E to assert `ctx->thread_count > 1` post-fix (it already prints
      the value; add the assertion) and confirm step B's throughput did not regress.
- [ ] **Step 4:** Implement `WrapRawDecodedFrame` and the two real method bodies until all tests
      pass. Run the full `recorder_core` suite once (`ctest --test-dir build/windows-x64-debug -C
      Debug -R "recorder_core\."`) to confirm the untouched BGRA path still passes unchanged.
- [ ] **Step 5:** Commit: `feat(edit): raw-frame playback decode path + FFmpeg decoder threading fix`.

---

## Task 4: Native render host + UI wiring (worktree, parallel, Sonnet)

**Depends on:** Task 1's `EditPlayerRenderer.h`/stub `.cpp` and `RawDecodedVideoFrame`. Does NOT
depend on Task 2's real shaders or Task 3's real decode path landing first — this task builds and
tests against the Task 1 stub converter (which clears to magenta) and can drive it with
synthetic/fixture-independent `RawDecodedVideoFrame`s constructed directly in its own tests.

**Files:**
- Modify: `app/services/EditPlayerRenderer.cpp` (replace stub body)
- Modify: `app/ui/widgets/EditPlayerSurface.h`/`.cpp`
- Modify: `app/pages/EditExportPage.h`/`.cpp`
- Modify/create: a test file covering the new lifecycle (extend
  `app/tests/test_edit_export_overlay.cpp` or add a sibling)

**Reference material to read first:** `app/ui/widgets/PreviewSurface.cpp:260-328`
(`tryStartDxgiPreview` — the exact `setAttribute(Qt::WA_NativeWindow); winId();` + DPI-aware
sizing + `Initialize(hwnd, ...)` call sequence to mirror) and
`app/services/DxgiPreviewRenderer.cpp`'s `InitD3D11`/`InitSwapChain`/`RenderFrame` (the D3D11
device + swap chain + present-loop boilerplate this task's much-smaller `EditPlayerRenderer`
should follow the shape of, without the WGC/webcam-PiP/cursor/snapshot machinery that class also
carries).

**What changes:**

- `EditPlayerSurface` gains `setAttribute(Qt::WA_NativeWindow); winId();` (same as
  `PreviewSurface`) and creates an `EditPlayerRenderer` owning a child HWND sized to the widget's
  DPI-scaled dimensions, instead of storing/painting a `QImage` in `paintEvent`. `resizeEvent`
  calls `EditPlayerRenderer::Resize`.
- The placeholder text (`setPlaceholderText`/`clearFrame` today) moves into
  `EditPlayerRenderer::ShowPlaceholder`, which renders it as a texture the render pass composites
  (or, simpler and sufficient for this design: draws it with GDI into an off-screen bitmap once
  per text change, uploads it as an SRV, and blits it — the same class of "sprite" technique
  `DxgiPreviewRenderer`'s OSD sprites already use, without needing that class's generality).
- `EditExportPage` switches its playback wiring from
  `EditPlayerEngine::StartPlaybackDecode`/`onDecodedFrameReady(QImage)` to
  `StartPlaybackDecodeRaw`/a new slot that forwards each `RawDecodedVideoFrame` to
  `player_surface_`'s `EditPlayerRenderer::PresentFrame`. The scrub path (`DecodeFrameAt`) switches
  to `DecodeFrameAtRaw` the same way.
- `EditPlayerRenderer::PresentFrame` decides internally (its own render thread, matching
  `DxgiPreviewRenderer`'s ownership model, or the calling thread if the implementer determines a
  dedicated thread is unnecessary at this smaller scope — document the choice in the class's
  header comment) whether to upload+draw the frame or drop it, consulting the same
  clock-gate rule as the spec's "Presentation cadence" section: a frame at or before the current
  playback clock time draws; a frame the clock has already passed is dropped without touching the
  GPU. `EditPlayerSession` already exposes the playback clock position the same way it does today
  for the BGRA path — reuse that, do not build a second clock.

**Testing:**

- [ ] **Step 1:** Write a test that creates an `EditPlayerSurface` inside a `QApplication`-backed
      test fixture (per `feedback_gtest_isolation_qapplication` — this project's widget tests
      already need their own `QApplication` fixture; follow the existing pattern in
      `test_edit_export_overlay.cpp`), shows it, feeds it a handful of synthetic
      `RawDecodedVideoFrame`s built directly in the test (no real decode needed — small
      hand-filled Y/U/V buffers, any `DecodedPixelFormat`), and asserts no crash and that a frame
      was presented (e.g. via `EditPlayerRenderer::HasPresentedFrame()`-style accessor, added to
      the class if useful — mirror `DxgiPreviewRenderer::HasPresentedFrame()`'s existing shape).
      Since Task 1's stub converter clears to a fixed color rather than converting, this test
      cannot assert per-pixel correctness (that is Task 2's job) — it asserts the pipeline runs:
      HWND created, resize doesn't crash, `Shutdown()` tears down cleanly.
- [ ] **Step 2:** Write a resize test: create the surface at one size, call the Qt resize path,
      assert `EditPlayerRenderer::Resize` was reached and the child HWND's dimensions updated
      (matching `PreviewSurface`'s existing resize test coverage, if any — check
      `test_preview_surface_webcam.cpp` for the pattern this project already uses to assert child
      HWND geometry from a test).
- [ ] **Step 3:** Implement the real `EditPlayerSurface`/`EditPlayerRenderer`/`EditExportPage`
      changes until both tests (and all pre-existing `EditExportPage`/`EditPlayerSurface` tests)
      pass.
- [ ] **Step 4:** Run the full `app` test suite once
      (`ctest --test-dir build/windows-x64-debug -C Debug -R "edit_export|edit_player"`).
- [ ] **Step 5:** Commit: `feat(edit): native child-HWND GPU render host for the editor player`.

---

## Task 5: Integration (sequential, after Tasks 2-4 land)

**Files:** touches the same files Tasks 1/2/3/4 touched, to delete the now-superseded BGRA path.

- [ ] **Step 1:** Merge the three worktree branches into `feat/editor-playback-gpu-render` (no-fast-
      forward merges, same approach as the `IVideoEncoder` refactor's Agent A/B merge — resolve
      any conflicts by inspection, they should be rare since the three tasks touch disjoint files
      apart from the shared header Task 1 already finalized).
- [ ] **Step 2:** Delete the now-dead BGRA path: `DecodedVideoFrame` (BGRA struct),
      `VideoFrameCallback`, `EditPlayerEngine::StartPlaybackDecode`/`DecodeFrameAt`,
      `ConvertToDecodedFrame`, and `EditExportPage`'s old `onDecodedFrameReady(QImage)` slot. The
      CPU functions in `yuv_to_bgra.cpp` (`ConvertFullPlanarYuv420ToBgra`/`ConvertFullPlanar444ToBgra`
      and friends) and `P010PqMonitorConverter` stay — they are Task 2's pinned test reference, per
      the design's Testing section, not dead code.
- [ ] **Step 3:** Rename `RawDecodedVideoFrame` → `DecodedVideoFrame`, `RawVideoFrameCallback` →
      `VideoFrameCallback`, `StartPlaybackDecodeRaw` → `StartPlaybackDecode`, `DecodeFrameAtRaw` →
      `DecodeFrameAt` throughout, now that only one meaning exists. Update every call site's
      renamed references (compiler will find them all).
- [ ] **Step 4:** Full rebuild (Debug and Release) and full test suite once:
      `ctest --test-dir build/windows-x64-debug -C Debug` and the Release equivalent used
      elsewhere in this project's verification routine.
- [ ] **Step 5:** Update `docs/superpowers/specs/2026-08-03-editor-playback-gpu-render-design.md`'s
      Status line from `proposed` to `implemented, 2026-08-03` (or the actual completion date).
- [ ] **Step 6:** Commit: `merge(edit): land the GPU editor playback render path`.
- [ ] **Step 7 (not automatable — hand off to the user):** live-verify per the spec's Testing
      section — an ordinary 60fps/4:2:0 recording played smoothly end to end (regression guard on
      the common case), and, if the user has or is willing to produce one, a 4:4:4/4K/240fps
      Expert recording to judge whether this design actually clears that budget in practice. Do
      not claim this combination is "fixed" without that live check; the design doc is explicit
      that removing the two biggest known costs does not by itself prove the resulting total
      clears the 4.17 ms/frame budget once GPU upload/present and decode-with-threading costs are
      counted.

      **Step 7 live-verify checklist** (nothing below is reachable by the widget tests or the
      `--visual-test` harness — the harness deliberately never calls `startGpuRendering()`, so
      every one of these needs a real run with a real clip open on the Edit page):

      - [ ] **Qt controls over the native child HWND — visible AND clickable.**
            `play_pause_btn_` and `player_meta_label_` share the player's grid cell with
            `player_surface_`, deliberately floating over the video. `EditPlayerSurface` now sets
            `WA_NativeWindow` and hosts a D3D11 child HWND, which occludes ordinary Qt painting
            unless the siblings get auto-promoted to native windows and land above it in Z order.
            The code already does the defensive thing (`SetWindowPos(childHwnd_, HWND_BOTTOM, …)`
            in `EditPlayerRenderer::Initialize`, byte-for-byte the same call and timing
            `DxgiPreviewRenderer` uses — verified during the final review, no discrepancy found),
            but it has never been confirmed against *this* page's widget tree. If the button ends
            up occluded it is also unclickable: a native child HWND swallows the input a Qt
            sibling underneath would need, which breaks the Edit page's play control outright.
            **Confirm the play/pause button and the meta label remain visible AND clickable over
            the video surface once GPU rendering is active** — click play, click pause, watch the
            glyph change.
      - [ ] **Backward scrub / trim-handle preview updates the picture.** Play a clip with audio
            well past its start, pause, then scrub backwards and drag both trim handles. Each
            should repaint to the frame under the handle. (This is the regression the final
            review's finding #1 fixed — a stale present-gate clock silently dropped every
            backward-seeking frame. Covered by a unit test on the renderer's gate, but the
            end-to-end path through `EditExportPage` is only observable live.)
      - [ ] **Scrub after end-of-clip.** Let a clip play to its end (it pauses at `total`), then
            scrub back. Same failure mode as above, different entry point.

---

## Self-Review Notes

- **Spec coverage:** Problem/measurements (context above, unchanged from spec) → Tasks 2+3.
  Architecture (native child HWND, `EditPlayerRenderer`, `EditFrameGpuConverter`) → Tasks 1+4.
  Data flow change (ref-counted `AVFrame`, no per-frame alloc) → Tasks 1+3. Pixel formats in scope
  → Task 2 (all three explicitly tested). Presentation cadence (clock-gated) → Task 4. FFmpeg
  decoder threading → Task 1+3. Non-goals (no hardware decode, no scrub/seek/audio changes) →
  respected throughout, `DecodeFrameAt`'s synchronous single-frame contract is preserved
  verbatim in `DecodeFrameAtRaw`. Testing section's WARP-device shader tests → Task 2; queue
  payload/refcount test → Task 3; lifecycle/resize test → Task 4; live-verify → Task 5 Step 7.
- **Placeholder scan:** the one inline `// NOTE for implementer` in Task 2's first shader listing
  is not a placeholder — it is a worked correction of a deliberately-shown-wrong intermediate line,
  left in so the implementer sees the mistake and the fix side by side (the un/vn double-scaling
  bug is the single easiest thing to get wrong copying `ComputeCoefs` into HLSL). The shader as
  actually specified (the `un_fixed`/`vn_fixed`/`r`/`g`/`b` lines) is complete and correct.
- **Type consistency:** `RawDecodedVideoFrame`, `DecodedPixelFormat`, `RawVideoFrameCallback`,
  `EditFrameGpuConverter::{Init,Convert}`, `EditPlayerRenderer::{Initialize,Resize,PresentFrame,
  ShowPlaceholder,Shutdown}` are defined once in Task 1 and used with identical signatures in
  Tasks 2-4; Task 5's rename step is the only place names change, and it changes them everywhere
  in one mechanical pass.
