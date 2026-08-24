# Retained Video Frames and Cursor-Correct Pacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce explicit visual generation counters for screen/webcam/cursor/overlay/color-pipeline state, use them to stop DXGI cursor-only events from being treated as desktop presents, recomposite a held WGC/OD frame whenever cursor or overlay state moved (not just webcam), and reuse the already-converted NV12/P010 reference frame and its encoder-slot copies when nothing visible changed — with diagnostics counters proving each reuse actually fires.

**Architecture:** Pure, unit-testable value types and classifiers (`VisualFrameKey`, `ClassifyOdAcquire`) live in new `libs/engine/include/exosnap/engine/` headers with their own gtest targets. `VideoThread` (`libs/engine/src/video_thread.cpp`) is the only place that wires them into the live D3D11/DXGI capture loop — it already contains the CFR duplicate-frame cache (`refNv12`) and the held-screen recomposite gate (`ShouldRecompositeHeldScreen`, `libs/engine/include/exosnap/engine/frame_pacing.h`) this plan formalizes and extends. No UI/app-layer files change except `WebcamService`'s frame-provider interface, which gains a generation out-parameter.

**Tech Stack:** C++20 (engine already builds with `cxx_std_20`; `std::span`/defaulted `operator<=>` are already in use), GoogleTest via the repo's `exosnap_add_gtest()` CMake helper, Windows D3D11/DXGI Output Duplication.

## Global Constraints

- C++20. Use defaulted `operator<=>`/`operator==` on aggregates the same way `frame_pacing.h`'s existing types do.
- New pure-logic types/classifiers go in `libs/engine/include/exosnap/engine/*.h` (header-only where practical) with a dedicated `tests/test_*.cpp` gtest target registered **only in the full (non-skeleton) build block** of `libs/engine/CMakeLists.txt` — i.e. after the `return()` at line 261, following the placement of `frame_pacing_tests` (~line 1295-1302). This matches how `frame_pacing.h`/`ShouldRecompositeHeldScreen` are already tested in isolation from the D3D11-heavy parts of `engine`.
- Diagnostics counters follow the exact existing pattern in `PipelineDiagnosticsAggregator`: a private `uint64_t foo_ = 0;` member, a public `void OnFoo() noexcept { std::lock_guard lk(mutex_); ++foo_; }`, and a field copied into `RecordingDiagnosticsSnapshot` inside `BuildSnapshot()`.
- Do not modify `NvencEncoder`/`InputSlot` (`libs/engine/src/nvenc_encoder.h`) — encoder-slot content identity is tracked as new `VideoThread`-local state, not inside the NVENC wrapper, exactly as the source spec's `EncoderSlotState` is described as a separate concept from NVENC's own slot bookkeeping.
- Out of scope for this plan (explicitly deferred, not partially started): `CV-RETAIN-005` (pipeline "recipe" dispatch) and `CV-RETAIN-006` (overlay-pass state bundling) — the source document itself rates both as low-confidence/likely-small wins ("Der Gewinn ist wahrscheinlich klein" / "wahrscheinlich kleiner als ExoJS' Commandbuffer-Cache"). Revisit only after the diagnostics counters this plan adds (Task 9) show the earlier, higher-confidence reuse stages are not enough. Also out of scope: `CV-WEBCAM-002`'s full copy-out-API-to-shared-snapshot rewrite — Task 3 adds the generation counter only; eliminating the per-tick CPU copy in `WebcamService::TryGetFrame` needs that separate, larger rewrite.
- Every task that touches `video_thread.cpp` must build the full app target (not just the affected gtest) before being marked done, since that file has no direct unit test harness of its own — correctness there is proven by the pure-logic unit tests of the helpers it calls, plus a full build succeeding. Follow [[feedback_build_full_test_suite]]: `--target exosnap` alone does not build tests.

---

### Task 1: `VisualFrameKey` and `VisualGenerations` types (CV-RETAIN-001)

**Files:**
- Create: `libs/engine/include/exosnap/engine/visual_generations.h`
- Test: `libs/engine/tests/test_visual_generations.cpp`
- Modify: `libs/engine/CMakeLists.txt` (register the new test target)

**Interfaces:**
- Produces:
  ```cpp
  namespace exosnap::engine {
  struct VisualGenerations {
      uint64_t screen = 0;
      uint64_t webcam = 0;
      uint64_t cursor = 0;
      uint64_t overlay = 0;
      uint64_t color_pipeline = 0;
  };
  struct VisualFrameKey {
      uint64_t screen_generation = 0;
      uint64_t webcam_generation = 0;
      uint64_t cursor_generation = 0;
      uint64_t overlay_generation = 0;
      uint64_t color_pipeline_generation = 0;
      auto operator<=>(const VisualFrameKey&) const = default;
  };
  [[nodiscard]] constexpr VisualFrameKey MakeVisualFrameKey(const VisualGenerations& gens) noexcept;
  }  // namespace exosnap::engine
  ```

- [ ] **Step 1: Write the failing test**

Create `libs/engine/tests/test_visual_generations.cpp`:

```cpp
#include "exosnap/engine/visual_generations.h"
#include <gtest/gtest.h>

using namespace exosnap::engine;

TEST(VisualFrameKey, DefaultKeysAreEqual) {
    EXPECT_EQ(VisualFrameKey{}, VisualFrameKey{});
}

TEST(VisualFrameKey, DiffersWhenAnySingleGenerationDiffers) {
    VisualFrameKey base{};

    VisualFrameKey screenChanged{};
    screenChanged.screen_generation = 1;
    EXPECT_NE(base, screenChanged);

    VisualFrameKey webcamChanged{};
    webcamChanged.webcam_generation = 1;
    EXPECT_NE(base, webcamChanged);

    VisualFrameKey cursorChanged{};
    cursorChanged.cursor_generation = 1;
    EXPECT_NE(base, cursorChanged);

    VisualFrameKey overlayChanged{};
    overlayChanged.overlay_generation = 1;
    EXPECT_NE(base, overlayChanged);

    VisualFrameKey colorChanged{};
    colorChanged.color_pipeline_generation = 1;
    EXPECT_NE(base, colorChanged);
}

TEST(VisualFrameKey, EqualWhenAllFieldsMatch) {
    VisualFrameKey a{1, 2, 3, 4, 5};
    VisualFrameKey b{1, 2, 3, 4, 5};
    EXPECT_EQ(a, b);
}

TEST(MakeVisualFrameKeyTest, CopiesEachGenerationFieldByName) {
    VisualGenerations gens{};
    gens.screen = 10;
    gens.webcam = 20;
    gens.cursor = 30;
    gens.overlay = 40;
    gens.color_pipeline = 50;

    const VisualFrameKey key = MakeVisualFrameKey(gens);
    EXPECT_EQ(key.screen_generation, 10u);
    EXPECT_EQ(key.webcam_generation, 20u);
    EXPECT_EQ(key.cursor_generation, 30u);
    EXPECT_EQ(key.overlay_generation, 40u);
    EXPECT_EQ(key.color_pipeline_generation, 50u);
}

TEST(MakeVisualFrameKeyTest, IsConstexprEvaluable) {
    constexpr VisualGenerations gens{1, 2, 3, 4, 5};
    constexpr VisualFrameKey key = MakeVisualFrameKey(gens);
    static_assert(key.screen_generation == 1);
}
```

Add the CMake target in `libs/engine/CMakeLists.txt` immediately after the `frame_pacing_tests` block (after line ~1302):

```cmake
# Retained-frame plan: VisualFrameKey/VisualGenerations — pure value types, no D3D/NVENC.
exosnap_add_gtest(
    NAME test_visual_generations
    TEST_PREFIX engine.
    SOURCES tests/test_visual_generations.cpp
)
target_include_directories(test_visual_generations PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target test_visual_generations`
Expected: FAIL — `recorder_core/visual_generations.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `libs/engine/include/exosnap/engine/visual_generations.h`:

```cpp
#pragma once
#include <cstdint>

namespace exosnap::engine {

// One monotonically-increasing counter per independently-changing visual
// input. Each counter is bumped only when that specific input actually
// changed — a new accepted screen/webcam sample, a cursor position/
// visibility/shape/capture-toggle change, an overlay geometry/opacity/
// chroma-key change, or an HDR/colour-pipeline reconfiguration. Comparing
// the resulting VisualFrameKey tells the pipeline whether the previous
// composited/converted frame is still valid without ever touching pixels.
struct VisualGenerations {
    uint64_t screen = 0;
    uint64_t webcam = 0;
    uint64_t cursor = 0;
    uint64_t overlay = 0;
    uint64_t color_pipeline = 0;
};

// Snapshot of VisualGenerations at the moment a frame was composited/
// converted. Two keys compare equal iff every input was unchanged, which is
// the sole condition under which a cached composited/converted/encoded
// result may be reused instead of redone.
struct VisualFrameKey {
    uint64_t screen_generation = 0;
    uint64_t webcam_generation = 0;
    uint64_t cursor_generation = 0;
    uint64_t overlay_generation = 0;
    uint64_t color_pipeline_generation = 0;

    auto operator<=>(const VisualFrameKey&) const = default;
};

[[nodiscard]] constexpr VisualFrameKey MakeVisualFrameKey(const VisualGenerations& gens) noexcept {
    return VisualFrameKey{gens.screen, gens.webcam, gens.cursor, gens.overlay, gens.color_pipeline};
}

}  // namespace exosnap::engine
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target test_visual_generations`
Then: `ctest --test-dir build/windows-x64-debug -R "engine\.(VisualFrameKey|MakeVisualFrameKeyTest)" --output-on-failure`
Expected: PASS, 6 tests.

- [ ] **Step 5: Commit**

```bash
git add libs/engine/include/exosnap/engine/visual_generations.h \
        libs/engine/tests/test_visual_generations.cpp \
        libs/engine/CMakeLists.txt
git commit -m "Add VisualFrameKey/VisualGenerations (CV-RETAIN-001)"
```

---

### Task 2: DXGI acquire classification helper (CV-CURSOR-001, pure logic)

**Files:**
- Create: `libs/engine/include/exosnap/engine/od_acquire_classify.h`
- Test: `libs/engine/tests/test_od_acquire_classify.cpp`
- Modify: `libs/engine/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing (pure booleans in, enum out — no dependency on Task 1).
- Produces:
  ```cpp
  namespace exosnap::engine {
  enum class OdAcquireKind : uint8_t { DesktopPresent, CursorOnly, Ignorable };
  [[nodiscard]] constexpr OdAcquireKind ClassifyOdAcquire(bool has_present, bool has_mouse_update,
                                                          bool cursor_capture_enabled) noexcept;
  }
  ```
  `DesktopPresent`: a real desktop frame arrived (`LastPresentTime != 0`) — copy the screen texture, bump `screen`. `CursorOnly`: no desktop frame but a mouse update arrived and cursor capture is on — do NOT copy the screen texture or insert a pacing-ring entry, bump `cursor` only. `Ignorable`: neither a usable present nor an actionable mouse update (including a mouse update while cursor capture is off) — do nothing, not even a cursor-generation bump.

- [ ] **Step 1: Write the failing test**

Create `libs/engine/tests/test_od_acquire_classify.cpp`:

```cpp
#include "exosnap/engine/od_acquire_classify.h"
#include <gtest/gtest.h>

using namespace exosnap::engine;

TEST(ClassifyOdAcquire, PresentIsAlwaysDesktopPresent) {
    EXPECT_EQ(ClassifyOdAcquire(/*has_present=*/true, /*has_mouse_update=*/false, /*cursor_capture_enabled=*/false),
              OdAcquireKind::DesktopPresent);
    EXPECT_EQ(ClassifyOdAcquire(true, true, true), OdAcquireKind::DesktopPresent);
}

TEST(ClassifyOdAcquire, MouseOnlyWithCursorCaptureOnIsCursorOnly) {
    EXPECT_EQ(ClassifyOdAcquire(/*has_present=*/false, /*has_mouse_update=*/true, /*cursor_capture_enabled=*/true),
              OdAcquireKind::CursorOnly);
}

TEST(ClassifyOdAcquire, MouseOnlyWithCursorCaptureOffIsIgnorable) {
    EXPECT_EQ(ClassifyOdAcquire(false, true, /*cursor_capture_enabled=*/false), OdAcquireKind::Ignorable);
}

TEST(ClassifyOdAcquire, NeitherPresentNorMouseUpdateIsIgnorable) {
    EXPECT_EQ(ClassifyOdAcquire(false, false, true), OdAcquireKind::Ignorable);
    EXPECT_EQ(ClassifyOdAcquire(false, false, false), OdAcquireKind::Ignorable);
}
```

Register in `libs/engine/CMakeLists.txt`, right after the `test_visual_generations` block added in Task 1:

```cmake
# Retained-frame plan: separates DXGI cursor-only acquires from real desktop
# presents (CV-CURSOR-001) — pure logic, no D3D/NVENC.
exosnap_add_gtest(
    NAME test_od_acquire_classify
    TEST_PREFIX engine.
    SOURCES tests/test_od_acquire_classify.cpp
)
target_include_directories(test_od_acquire_classify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target test_od_acquire_classify`
Expected: FAIL — `recorder_core/od_acquire_classify.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `libs/engine/include/exosnap/engine/od_acquire_classify.h`:

```cpp
#pragma once
#include <cstdint>

namespace exosnap::engine {

// What one successful DXGI Output Duplication AcquireNextFrame actually
// delivered. DXGI can wake the acquire with only a cursor move/visibility/
// shape change and LastPresentTime == 0 — treating that as a fresh desktop
// frame (the pre-existing behavior this replaces) causes an unnecessary
// screen-texture copy, phase-correct pacing-ring entry, and downstream
// composition/conversion for a frame that never actually changed on screen.
enum class OdAcquireKind : uint8_t {
    DesktopPresent, // LastPresentTime != 0: a real, new desktop frame.
    CursorOnly,     // No desktop frame, but cursor state changed and capture_cursor is on.
    Ignorable,      // Nothing actionable: no present, and no mouse update (or cursor capture is off).
};

[[nodiscard]] constexpr OdAcquireKind ClassifyOdAcquire(bool has_present, bool has_mouse_update,
                                                         bool cursor_capture_enabled) noexcept {
    if (has_present)
        return OdAcquireKind::DesktopPresent;
    if (has_mouse_update && cursor_capture_enabled)
        return OdAcquireKind::CursorOnly;
    return OdAcquireKind::Ignorable;
}

}  // namespace exosnap::engine
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target test_od_acquire_classify`
Then: `ctest --test-dir build/windows-x64-debug -R "engine\.ClassifyOdAcquire" --output-on-failure`
Expected: PASS, 4 tests.

- [ ] **Step 5: Commit**

```bash
git add libs/engine/include/exosnap/engine/od_acquire_classify.h \
        libs/engine/tests/test_od_acquire_classify.cpp \
        libs/engine/CMakeLists.txt
git commit -m "Add ClassifyOdAcquire to separate cursor-only DXGI events from desktop presents (CV-CURSOR-001)"
```

---

### Task 3: Webcam frame generation counter (CV-RETAIN-002)

**Files:**
- Modify: `libs/engine/include/exosnap/engine/recorder_session.h:43` (`WebcamFrameProvider::TryGetFrame` signature)
- Modify: `app/services/WebcamService.h:148,162,187-192` (declaration + new member)
- Modify: `app/services/WebcamService.cpp:713,730-736` (`StoreFrame`/`TryGetFrame` bodies)
- Modify: `libs/engine/src/video_thread.cpp:1153` (the one call site — updated in Task 4, not here, to avoid touching `video_thread.cpp` twice in unrelated tasks; this task keeps the interface change buildable by passing a throwaway local)

**Interfaces:**
- Consumes: nothing new.
- Produces: `WebcamFrameProvider::TryGetFrame(int&, int&, std::vector<uint8_t>&, uint64_t& out_generation)`. `out_generation` is a monotonically increasing counter bumped once per `StoreFrame()` call (once per newly delivered camera sample), independent of whether the pixels actually differ — matching CV-RETAIN-002's explicit "not a pixel comparison" contract, the same way the CFR duplicate cache already treats "new sample" as sufficient without comparing bytes.

- [ ] **Step 1: Update the interface**

`libs/engine/include/exosnap/engine/recorder_session.h:43`, change:

```cpp
    virtual bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra) = 0;
```

to:

```cpp
    // out_generation is bumped once per newly captured sample (StoreFrame call),
    // regardless of whether the pixels changed — callers use it to skip redundant
    // recomposition when the webcam has not produced a new sample since last tick.
    virtual bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                             uint64_t& out_generation) = 0;
```

- [ ] **Step 2: Update `WebcamService` to match, with a new failing assertion**

Add a member to `app/services/WebcamService.h` right after `bool has_frame_ = false;` (line 192):

```cpp
    uint64_t frame_generation_ = 0;
```

Update the override declaration at line 148:

```cpp
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                     uint64_t& out_generation) override;
```

Locate the existing webcam-service test file:

Run: `find app/tests -iname "*webcam_service*"`

If a `test_webcam_service.cpp` exists exercising `TryGetFrame` directly, add this case (matching that file's existing fixture setup for a `WebcamService` instance with `StoreFrame`-equivalent access — if `StoreFrame` is private and the existing tests only reach it via `Start()`+live capture, add the assertion to whichever existing test already calls `TryGetFrame` after a frame is known to have arrived, asserting the generation is now `>= 1` and a second call with no new frame returns the same generation). If no such test exists, skip to Step 3 directly — `WebcamService`'s MF capture loop cannot be driven from a unit test without a real camera, so this class's generation behavior is proven by the `video_thread.cpp` full build in Task 4 plus manual live-verify, consistent with how `TryGetFrame`'s existing copy-out behavior has no dedicated unit test today.

- [ ] **Step 3: Implement**

`app/services/WebcamService.cpp`, change `StoreFrame` (line 730-736):

```cpp
void WebcamService::StoreFrame(int w, int h, std::vector<uint8_t> bgra) {
    std::lock_guard lk(frame_mutex_);
    frame_width_ = w;
    frame_height_ = h;
    latest_bgra_ = std::move(bgra);
    has_frame_ = true;
    ++frame_generation_;
}
```

Find `WebcamService::TryGetFrame`'s current body (grep for `bool WebcamService::TryGetFrame` in `WebcamService.cpp`) and add the new out-parameter, filling it under the same lock that already reads `latest_bgra_`/`has_frame_`:

```cpp
bool WebcamService::TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                                uint64_t& out_generation) {
    std::lock_guard lk(frame_mutex_);
    if (!has_frame_)
        return false;
    out_width = frame_width_;
    out_height = frame_height_;
    out_bgra = latest_bgra_;
    out_generation = frame_generation_;
    return true;
}
```

(Keep every other line of the existing body — the snippet above only shows the parts this task changes; if the real body additionally validates size/format before returning, keep those checks exactly as they are and only add the `out_generation = frame_generation_;` line plus the signature change.)

- [ ] **Step 4: Make the one remaining caller compile**

`libs/engine/src/video_thread.cpp:1153` currently reads:

```cpp
const bool gotCam = m_state.config.webcam.frame_provider->TryGetFrame(camW, camH, camBgra);
```

For this task only (Task 4 does the real generation-tracking wiring), make it compile with a throwaway local so the build is green after this task in isolation:

```cpp
uint64_t camGeneration = 0; // wired into VisualGenerations::webcam in Task 4
const bool gotCam = m_state.config.webcam.frame_provider->TryGetFrame(camW, camH, camBgra, camGeneration);
```

- [ ] **Step 5: Build and verify**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: builds clean (no other `TryGetFrame` overriders/callers exist — confirmed by `grep -rn "TryGetFrame" --include=*.cpp --include=*.h` returning only `WebcamService.{h,cpp}`, `recorder_session.h`, and `video_thread.cpp`).

- [ ] **Step 6: Commit**

```bash
git add libs/engine/include/exosnap/engine/recorder_session.h \
        app/services/WebcamService.h app/services/WebcamService.cpp \
        libs/engine/src/video_thread.cpp
git commit -m "Add a per-sample generation counter to WebcamFrameProvider (CV-RETAIN-002)"
```

---

### Task 4: Wire generations and cursor/desktop classification into the phase-correct (CFR) OD path (CV-CURSOR-001, CV-CURSOR-002, CV-PACING-001)

**Files:**
- Modify: `libs/engine/src/video_thread.cpp` (state declaration near `odCursorVisible`; acquire block at lines 2469-2530; webcam draw lambda at line 1153)

**Interfaces:**
- Consumes: `exosnap::engine::VisualGenerations`/`MakeVisualFrameKey` (Task 1), `exosnap::engine::ClassifyOdAcquire`/`OdAcquireKind` (Task 2), `WebcamFrameProvider::TryGetFrame(..., uint64_t&)` (Task 3).
- Produces: a `VisualGenerations visualGenerations{}` VideoThread-local, live-updated every tick, that Tasks 6-8 read.

- [ ] **Step 1: Declare the shared generation state**

`libs/engine/src/video_thread.cpp` — run `grep -n "bool odCursorVisible = false" libs/engine/src/video_thread.cpp` to find the declaration shared by both the CFR and VFR acquire paths (both read/write `odCursorVisible`/`odCursorPosX`/`odCursorPosY`/`odCursorShapeValid`, confirmed at lines 2513-2515 and 3013-3015). Add immediately after that declaration group:

```cpp
    VisualGenerations visualGenerations{};
    bool lastCursorCaptureEnabled = m_state.config.capture_cursor;
    uint64_t lastWebcamFrameGeneration = 0;
    bool haveWebcamFrameGeneration = false;
```

Add the include at the top of the file (alongside the other `recorder_core/*.h` includes already there):

```cpp
#include "exosnap/engine/visual_generations.h"
#include "exosnap/engine/od_acquire_classify.h"
```

- [ ] **Step 2: Replace the unconditional ring-write with classification**

**Before editing, run `grep -n "diag_recording\|odSrc.ReleaseFrame\|rawTex->Release" libs/engine/src/video_thread.cpp | sed -n '1,40p'` and read lines 2460-2540 in full.** The existing code declares `const bool diag_recording = !m_state.pause_requested.load();` once at line 2481 (just above the block you are replacing) and nowhere else in this branch — your replacement below must NOT redeclare it, since it stays in scope. The existing code also calls `rawTex->Release()` exactly once (old line 2507, right after the `if (usePhaseCorrect) {...} else {...}` block you are replacing) and `odSrc.ReleaseFrame()` exactly once (old line 2517, further down, after the cursor-shape/position update). Your replacement must preserve that "exactly once per acquired frame" invariant on every one of the three classification branches — verify this with the grep above after editing (each of `rawTex->Release()` / `odSrc.ReleaseFrame()` must still appear exactly once per code path, not once per branch times three).

Replace the block at `video_thread.cpp:2482-2506` (the `if (usePhaseCorrect) { ... } else { d3dContext->CopyResource(odCapturedTex.get(), rawTex); }` that currently always copies and always stamps a present QPC) — leaving the pre-existing `diag_recording` declaration at line 2481 and the pre-existing `rawTex->Release()` at (old) line 2507 exactly where they are — with:

```cpp
                    const OdAcquireKind acquireKind = ClassifyOdAcquire(
                        info.LastPresentTime.QuadPart != 0, info.LastMouseUpdateTime.QuadPart != 0,
                        m_state.config.capture_cursor);
                    if (acquireKind == OdAcquireKind::Ignorable) {
                        rawTex->Release();
                        odSrc.ReleaseFrame();
                        if (diag_recording)
                            m_state.diagnostics.OnCursorOnlyCaptureEventIgnored();
                        continue;
                    }
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
                        if (usePhaseCorrect) {
                            // Round-robin write into the ring keyed by source present-QPC.
                            CaptureRingEntry& entry = captureRing[ringHead];
                            // Evicting a fresh, never-emitted frame is a genuine drop.
                            if (entry.presentQpc != 0 && entry.presentQpc > lastEmittedPresentQpc) {
                                ++droppedFrames;
                                if (diag_recording)
                                    m_state.diagnostics.OnFrameDroppedCoalesced();
                            }
                            d3dContext->CopyResource(entry.tex.get(), rawTex);
                            entry.presentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                            ringHead = (ringHead + 1) % captureRing.size();
                            phaseRingHasFrame = true;
                        } else {
                            d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                        }
                        ++visualGenerations.screen;
                        if (diag_recording)
                            m_state.diagnostics.OnScreenGenerationChanged();
                    } else {
                        // CursorOnly: no desktop texture copy, no ring entry — the held
                        // frame (screen unchanged) gets recomposited with the new cursor
                        // by ShouldRecompositeHeldScreen instead (Task 6).
                        if (diag_recording)
                            m_state.diagnostics.OnPhaseRingCursorOnlyEventIgnored();
                    }
```

(The old unconditional synthetic-QPC fallback — `if (entryPresentQpc == 0) { QueryPerformanceCounter(...); }` — is removed entirely: `ClassifyOdAcquire` already guarantees `DesktopPresent` implies `info.LastPresentTime.QuadPart != 0`, so the ring entry always gets a real present QPC and no fabricated timestamp can enter the pacing ring. This is the CV-PACING-001 fix.)

- [ ] **Step 3: Bump the cursor generation**

Immediately after the block from Step 2 (where the old code already updates `odCursorVisible`/`odCursorPosX`/`odCursorPosY` from `info.LastMouseUpdateTime.QuadPart != 0`, around old line 2512-2516), change it to also bump the generation:

```cpp
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap)) {
                            odCursorShapeValid = true;
                            ++visualGenerations.cursor; // shape/bitmap changed
                        }
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        const bool visibilityChanged = odCursorVisible != (info.PointerPosition.Visible != FALSE);
                        const bool positionChanged = odCursorPosX != info.PointerPosition.Position.x ||
                                                     odCursorPosY != info.PointerPosition.Position.y;
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                        if (visibilityChanged || positionChanged)
                            ++visualGenerations.cursor;
                    }
                    if (m_state.config.capture_cursor != lastCursorCaptureEnabled) {
                        lastCursorCaptureEnabled = m_state.config.capture_cursor;
                        ++visualGenerations.cursor;
                    }
```

This keeps the existing shape/position/visibility bookkeeping exactly as-is and only adds the generation bump — satisfying CV-CURSOR-002's four triggers (position, visibility, shape, capture toggle) without a pixel-level bitmap diff, which the source document does not require (it only requires "not every fresh capture texture" — i.e. decoupled from `screen`, which this now is).

Two lines further down, the pre-existing `if (diag_recording) m_state.diagnostics.OnFrameCaptured();` (old lines 2518-2519, right after `odSrc.ReleaseFrame();`) currently fires for every acquire that reaches it — which after Step 2 now includes `CursorOnly` acquires, even though no new *screen* frame was produced. `OnFrameCaptured()`'s doc comment ("a frame the backend actually produced") means this must only fire for `DesktopPresent`. Change it to:

```cpp
                    if (diag_recording && acquireKind == OdAcquireKind::DesktopPresent)
                        m_state.diagnostics.OnFrameCaptured();
```

- [ ] **Step 4: Wire the webcam generation**

At `video_thread.cpp:1153` (inside `drawWebcamGpu`, updated in Task 3 Step 4 to declare a throwaway `camGeneration` local), replace that throwaway with real tracking. The lambda captures by reference (`[&]`), so it can see the enclosing `visualGenerations`/`lastWebcamFrameGeneration`/`haveWebcamFrameGeneration` declared in Step 1:

```cpp
        uint64_t camGeneration = 0;
        const bool gotCam = m_state.config.webcam.frame_provider->TryGetFrame(camW, camH, camBgra, camGeneration);
```

and, right after the existing `if (gotCam) { m_state.diagnostics.OnWebcamConvert(...); }` block (line 1155-1158), add:

```cpp
        if (gotCam && (!haveWebcamFrameGeneration || camGeneration != lastWebcamFrameGeneration)) {
            haveWebcamFrameGeneration = true;
            lastWebcamFrameGeneration = camGeneration;
            ++visualGenerations.webcam;
            m_state.diagnostics.OnWebcamGenerationChanged();
        }
```

- [ ] **Step 5: Build**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: fails until Task 9 adds `OnScreenGenerationChanged`/`OnCursorOnlyCaptureEventIgnored`/`OnPhaseRingCursorOnlyEventIgnored`/`OnWebcamGenerationChanged` to `PipelineDiagnosticsAggregator` — reorder if executing linearly: do Task 9's diagnostics-aggregator additions (just the four methods above, not the full sweep) before this step, or stub them as private no-op calls temporarily removed once Task 9 lands. Recommended: execute Task 9 immediately after this task, before Task 5, so the build stays green.

- [ ] **Step 6: Commit**

```bash
git add libs/engine/src/video_thread.cpp
git commit -m "Classify DXGI cursor-only vs desktop-present acquires in the phase-correct OD path (CV-CURSOR-001/002, CV-PACING-001)"
```

---

### Task 5: Apply the same classification to the VFR OD path (CV-CURSOR-001/002 parity)

**Files:**
- Modify: `libs/engine/src/video_thread.cpp:2995-3040` (VFR acquire block)

**Interfaces:**
- Consumes: `visualGenerations`, `ClassifyOdAcquire` (already in scope from Task 4).

- [ ] **Step 1: Replace the unconditional copy with classification**

**Before editing, read `video_thread.cpp` lines 3006-3034 in full** (this task's target range is larger than a first skim suggests — it runs from the `CopyResource` through the pre-existing `diag_recording` declaration and `OnFrameCaptured()` call, all of which this task's replacement subsumes). The VFR path currently does, across that whole range (no `usePhaseCorrect` branching here since VFR never uses the ring):

```cpp
                    d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap))
                            odCursorShapeValid = true;
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                    }
                    // Convert DXGI LastPresentTime (QPC ticks) to 100ns units
                    if (info.LastPresentTime.QuadPart != 0) {
                        const auto lpt = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        latestFrameTicks100ns =
                            static_cast<int64_t>(lpt / qpcFreq * 10000000ULL + lpt % qpcFreq * 10000000ULL / qpcFreq);
                    }
                    odSrc.ReleaseFrame();
                    // Only count capture/coalesce while actively recording — frames the
                    // backend produces during pause are intentionally discarded, not drops.
                    const bool diag_recording = !m_state.pause_requested.load();
                    if (diag_recording)
                        m_state.diagnostics.OnFrameCaptured();
```

Replace that **entire** range with the VFR-appropriate mirror of Task 4 Steps 2-4 (`diag_recording` declared once, at the top, replacing its old declaration point; `latestFrameTicks100ns` only updates on a real present; `OnFrameCaptured()` only fires on `DesktopPresent`; `odSrc.ReleaseFrame()` appears exactly once per path, matching the original's single call):

```cpp
                    const bool diag_recording = !m_state.pause_requested.load();
                    const OdAcquireKind acquireKind = ClassifyOdAcquire(
                        info.LastPresentTime.QuadPart != 0, info.LastMouseUpdateTime.QuadPart != 0,
                        m_state.config.capture_cursor);
                    if (acquireKind == OdAcquireKind::Ignorable) {
                        rawTex->Release();
                        odSrc.ReleaseFrame();
                        if (diag_recording)
                            m_state.diagnostics.OnCursorOnlyCaptureEventIgnored();
                        continue;
                    }
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
                        d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                        ++visualGenerations.screen;
                        if (diag_recording)
                            m_state.diagnostics.OnScreenGenerationChanged();
                    }
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap)) {
                            odCursorShapeValid = true;
                            ++visualGenerations.cursor;
                        }
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        const bool visibilityChanged = odCursorVisible != (info.PointerPosition.Visible != FALSE);
                        const bool positionChanged = odCursorPosX != info.PointerPosition.Position.x ||
                                                     odCursorPosY != info.PointerPosition.Position.y;
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                        if (visibilityChanged || positionChanged)
                            ++visualGenerations.cursor;
                    }
                    if (m_state.config.capture_cursor != lastCursorCaptureEnabled) {
                        lastCursorCaptureEnabled = m_state.config.capture_cursor;
                        ++visualGenerations.cursor;
                    }
                    // Convert DXGI LastPresentTime (QPC ticks) to 100ns units — only
                    // meaningful for a real desktop present.
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
                        const auto lpt = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        latestFrameTicks100ns =
                            static_cast<int64_t>(lpt / qpcFreq * 10000000ULL + lpt % qpcFreq * 10000000ULL / qpcFreq);
                    }
                    odSrc.ReleaseFrame();
                    if (diag_recording && acquireKind == OdAcquireKind::DesktopPresent)
                        m_state.diagnostics.OnFrameCaptured();
```

After editing, run `grep -n "diag_recording\|odSrc.ReleaseFrame\|rawTex->Release" libs/engine/src/video_thread.cpp` and confirm there is still exactly one `diag_recording` declaration in this function's VFR branch (not two) and exactly one `odSrc.ReleaseFrame()` / one `rawTex->Release()` reached per acquired frame on every classification path (the `Ignorable` branch's own copies inside its `continue`-guarded block, or the shared ones at the bottom for `DesktopPresent`/`CursorOnly` — never both for the same acquire).

- [ ] **Step 2: Build**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: PASS (same diagnostics methods as Task 4, already added by Task 9 if sequenced as recommended).

- [ ] **Step 3: Commit**

```bash
git add libs/engine/src/video_thread.cpp
git commit -m "Apply DXGI cursor-only/desktop-present classification to the VFR OD path"
```

---

### Task 6: Recomposite the held frame on any dynamic overlay change, not just webcam (CV-CURSOR-003)

**Files:**
- Modify: `libs/engine/include/exosnap/engine/frame_pacing.h:75-82` (rename the third parameter for clarity)
- Modify: `libs/engine/tests/test_frame_pacing.cpp:145-179` (update comments to match, arguments are positional so behavior is untouched)
- Modify: `libs/engine/src/video_thread.cpp:2757-2762` (call site)

**Interfaces:**
- Consumes: `ShouldRecompositeHeldScreen` (existing), `visualGenerations` (Task 4/5).
- Produces: a new `VisualFrameKey lastCompositedKey{}` VideoThread-local that Task 7 also reads/writes.

- [ ] **Step 1: Rename the parameter (no behavior change — confirm existing tests still pass)**

`libs/engine/include/exosnap/engine/frame_pacing.h:75-82`, change:

```cpp
[[nodiscard]] constexpr bool ShouldRecompositeHeldScreen(bool has_fresh_source, bool od_holding,
                                                         bool webcam_overlay_active, bool has_held_screen) noexcept {
    if (has_fresh_source)
        return false; // a real frame is available; composite that instead
    if (od_holding || !has_held_screen)
        return false;
    return webcam_overlay_active;
}
```

to:

```cpp
[[nodiscard]] constexpr bool ShouldRecompositeHeldScreen(bool has_fresh_source, bool od_holding,
                                                         bool dynamic_overlay_changed, bool has_held_screen) noexcept {
    if (has_fresh_source)
        return false; // a real frame is available; composite that instead
    if (od_holding || !has_held_screen)
        return false;
    return dynamic_overlay_changed;
}
```

Also update the doc comment above it (lines 56-74) to say "webcam, cursor or overlay-settings state" instead of just "webcam" — it currently only mentions the webcam moving; broaden the second paragraph to mention cursor motion too, matching the new trigger set.

Update the parameter-name comments in `test_frame_pacing.cpp` (e.g. `/*webcam_overlay_active=*/true` at lines 148, 157 etc.) to `/*dynamic_overlay_changed=*/true` — purely cosmetic, the boolean values and assertions are unchanged.

- [ ] **Step 2: Run the existing tests to confirm no regression**

Run: `cmake --build build/windows-x64-debug --target frame_pacing_tests`
Then: `ctest --test-dir build/windows-x64-debug -R HeldScreenRecomposite --output-on-failure`
Expected: PASS, all 6 existing cases (renaming a parameter does not change positional-argument semantics).

- [ ] **Step 3: Compute the richer boolean at the call site**

Add the tracking local near `visualGenerations` (Task 4 Step 1):

```cpp
    VisualFrameKey lastCompositedKey{};
    bool haveLastCompositedKey = false;
```

At `video_thread.cpp:2757-2762`, replace:

```cpp
                ID3D11Texture2D* const heldScreenTex = useOdCapture ? odCapturedTex.get() : heldWgcTex.get();
                if (ShouldRecompositeHeldScreen(rawSourceTex != nullptr, odHolding,
                                                m_state.SnapshotWebcamOverlay().enabled && webcamProviderAvailable,
                                                heldScreenTex != nullptr)) {
                    rawSourceTex = heldScreenTex;
                }
```

with:

```cpp
                ID3D11Texture2D* const heldScreenTex = useOdCapture ? odCapturedTex.get() : heldWgcTex.get();
                const VisualFrameKey currentVisualKey = MakeVisualFrameKey(visualGenerations);
                const bool dynamicOverlayChanged =
                    m_state.SnapshotWebcamOverlay().enabled && webcamProviderAvailable &&
                    (!haveLastCompositedKey || currentVisualKey.webcam_generation != lastCompositedKey.webcam_generation ||
                     currentVisualKey.cursor_generation != lastCompositedKey.cursor_generation ||
                     currentVisualKey.overlay_generation != lastCompositedKey.overlay_generation);
                if (ShouldRecompositeHeldScreen(rawSourceTex != nullptr, odHolding, dynamicOverlayChanged,
                                                heldScreenTex != nullptr)) {
                    rawSourceTex = heldScreenTex;
                }
```

(The webcam-overlay-enabled gate is kept exactly as before — recompositing a held frame is still pointless with no webcam overlay at all, since without it nothing on screen can be moving while the desktop is still. What changes is that a live cursor now also triggers the recomposite, not only a fresh webcam sample.)

- [ ] **Step 4: Stamp `lastCompositedKey` after every real composite**

Every place `frameWritten = true;` is set from an actual composite (not the duplicate-reuse branch) must stamp `lastCompositedKey = currentVisualKey; haveLastCompositedKey = true;`. Locate them:

Run: `grep -n "frameWritten = true" libs/engine/src/video_thread.cpp`

This must show the native-HDR10 branch (after `performSnapshotIfRequested(slot);` near old line 2801-2802) and the SDR branch (near old line 2865-2866) — both already identified in this plan's research as the two places `refNv12`/`refNv12Valid` get written. Add the stamp immediately after each of those two `frameWritten = true;` lines (NOT after the duplicate-reuse branch's `frameWritten = true;` at old line 2872 — a duplicate reuses the previous key by definition, it does not advance it).

- [ ] **Step 5: Build**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add libs/engine/include/exosnap/engine/frame_pacing.h \
        libs/engine/tests/test_frame_pacing.cpp \
        libs/engine/src/video_thread.cpp
git commit -m "Recomposite the held WGC/OD frame on cursor or overlay change, not only webcam (CV-CURSOR-003)"
```

---

### Task 7: Formalize the YUV reference-frame cache with a `VisualFrameKey` tag (CV-RETAIN-003)

**Files:**
- Modify: `libs/engine/src/video_thread.cpp:2310-2311` (declare the tagged cache alongside `refNv12`), `:2797-2800`, `:2860-2863` (stamp the key on write), `:2869-2874` (duplicate-reuse diagnostics)

**Interfaces:**
- Consumes: `VisualFrameKey`, `lastCompositedKey`/`currentVisualKey` (Task 6).
- Produces: `bool refNv12Valid` stays the actual "may I `CopyResource` from `refNv12`" flag (unchanged, avoids touching the two write sites' control flow); a new parallel `VisualFrameKey refNv12Key{}` records what that texture currently contains, consumed by Task 8.

- [ ] **Step 1: Declare the key alongside the existing cache**

At `video_thread.cpp:2310-2311`, where `refNv12`/`refNv12Valid` are declared, add:

```cpp
    winrt::com_ptr<ID3D11Texture2D> refNv12;
    bool refNv12Valid = false;
    VisualFrameKey refNv12Key{}; // what refNv12 currently contains, once refNv12Valid
```

- [ ] **Step 2: Stamp the key at both write sites**

At `video_thread.cpp:2797-2800` (native HDR10 branch) and `:2860-2863` (SDR branch), both currently:

```cpp
                    if (refNv12 != nullptr) {
                        d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                        refNv12Valid = true;
                    }
```

change to:

```cpp
                    if (refNv12 != nullptr) {
                        d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                        refNv12Valid = true;
                        refNv12Key = currentVisualKey; // from Task 6, computed earlier this tick
                    }
```

- [ ] **Step 3: Count real vs. reused conversions at the duplicate branch**

At `video_thread.cpp:2869-2874`:

```cpp
                } else if (refNv12Valid) {
                    // Duplicate: copy the reference encode surface into this slot
                    d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                    frameWritten = true;
                    ++duplicatedFrames;
                    m_state.diagnostics.OnFrameDuplicated();
                }
```

leave the behavior unchanged (this is already the correct VisualFrameKey-equal-implies-reuse path — there is no fresh `rawSourceTex` this tick at all when this branch runs, which is exactly the "VisualFrameKey unchanged" case since `screen` cannot have advanced) and add the new diagnostics counters this task introduces:

```cpp
                } else if (refNv12Valid) {
                    // Duplicate: copy the reference encode surface into this slot.
                    // refNv12Key == currentVisualKey by construction here — nothing
                    // fresh arrived this tick, so no generation could have advanced.
                    d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                    frameWritten = true;
                    ++duplicatedFrames;
                    m_state.diagnostics.OnFrameDuplicated();
                    m_state.diagnostics.OnReusedYuvFrame();
                }
```

and, in the two real-composite branches (right after each `frameWritten = true;` that Task 6 Step 4 already annotated), add the paired counter:

```cpp
                m_state.diagnostics.OnFullComposition();
```

- [ ] **Step 4: Build**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: fails until Task 9 adds `OnReusedYuvFrame`/`OnFullComposition` — sequence Task 9 before this task's build-verify, same as Task 4/5.

- [ ] **Step 5: Commit**

```bash
git add libs/engine/src/video_thread.cpp
git commit -m "Tag the CFR YUV reference-frame cache with VisualFrameKey (CV-RETAIN-003)"
```

---

### Task 8: Skip the redundant encoder-slot copy once a slot already holds the current generation (CV-RETAIN-004)

**Files:**
- Modify: `libs/engine/src/video_thread.cpp` (declare per-slot content tracking near `refNv12`; guard the duplicate-copy branch from Task 7)

**Interfaces:**
- Consumes: `refNv12Key`/`refNv12Valid` (Task 7), `slot` (existing local — the NVENC slot index acquired earlier in the same tick via `nvenc.AcquireFreeSlot()`).
- Produces: `std::array<VisualFrameKey, 8> slotContainedKey{}` / `std::array<bool, 8> slotContainedValid{}` — deliberately declared in `video_thread.cpp`, not inside `NvencEncoder`/`InputSlot` (per Global Constraints: content identity is a `VideoThread` concept, not an NVENC-wrapper one). `8` matches the existing hardcoded ring size in both `NvencEncoder::m_slots` (`nvenc_encoder.h:339`) and this file's own `nv12Textures` array — `CV-PERF-007` (making that size dynamic) is a separate, already-tracked backlog item this task does not touch.

- [ ] **Step 1: Declare the per-slot tracking**

Alongside the `refNv12`/`refNv12Key` declaration from Task 7 Step 1:

```cpp
    std::array<VisualFrameKey, 8> slotContainedKey{};
    std::array<bool, 8> slotContainedValid{};
```

- [ ] **Step 2: Mark the slot valid whenever it receives a genuinely fresh composite**

Immediately after each of the two `frameWritten = true;` sites from Task 6 Step 4 / Task 7 Step 3 (the real-composite branches, where `slot` was just written directly by `finalizeEncodeSurface`/`encodeNativeHdrSlot`, not by copying `refNv12`), add:

```cpp
                if (slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size()) {
                    slotContainedKey[slot] = currentVisualKey;
                    slotContainedValid[slot] = true;
                }
```

- [ ] **Step 3: Skip the `CopyResource` in the duplicate branch when the slot is already current**

Change the duplicate branch from Task 7 Step 3:

```cpp
                } else if (refNv12Valid) {
                    // Duplicate: copy the reference encode surface into this slot.
                    // refNv12Key == currentVisualKey by construction here — nothing
                    // fresh arrived this tick, so no generation could have advanced.
                    d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                    frameWritten = true;
                    ++duplicatedFrames;
                    m_state.diagnostics.OnFrameDuplicated();
                    m_state.diagnostics.OnReusedYuvFrame();
                }
```

to:

```cpp
                } else if (refNv12Valid) {
                    const bool slotAlreadyCurrent = slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size() &&
                                                    slotContainedValid[static_cast<size_t>(slot)] &&
                                                    slotContainedKey[static_cast<size_t>(slot)] == refNv12Key;
                    if (!slotAlreadyCurrent) {
                        d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                        if (slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size()) {
                            slotContainedKey[static_cast<size_t>(slot)] = refNv12Key;
                            slotContainedValid[static_cast<size_t>(slot)] = true;
                        }
                        m_state.diagnostics.OnYuvSlotCopy();
                    } else {
                        m_state.diagnostics.OnYuvSlotCopySkipped();
                    }
                    frameWritten = true;
                    ++duplicatedFrames;
                    m_state.diagnostics.OnFrameDuplicated();
                    m_state.diagnostics.OnReusedYuvFrame();
                }
```

- [ ] **Step 4: Invalidate on encoder (re)configuration**

Find where the encoder is (re)initialized for this recording (grep for `InitEncoder(` in `video_thread.cpp`) — immediately after a successful re-init (e.g. on a resolution/format change mid-session, if that path exists in this file; otherwise this is naturally covered because `slotContainedKey`/`slotContainedValid` are function-local and reconstructed fresh for every new `VideoThread::run()` invocation, i.e. every new recording), add or confirm:

```cpp
    slotContainedValid.fill(false);
```

right after any successful `RegisterSlotTexture` re-registration loop, so a slot that just got a brand-new/resized texture is never treated as "already current" from before the resize.

- [ ] **Step 5: Build**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: fails until Task 9 adds `OnYuvSlotCopy`/`OnYuvSlotCopySkipped` — sequence Task 9 first.

- [ ] **Step 6: Commit**

```bash
git add libs/engine/src/video_thread.cpp
git commit -m "Skip the CFR duplicate CopyResource when the encoder slot already holds the current YUV generation (CV-RETAIN-004)"
```

---

### Task 9: Diagnostics counters (spec section 13)

**Files:**
- Modify: `libs/engine/src/pipeline_diagnostics_aggregator.h` (declare 10 new `On*()` methods + private counters, near the existing `OnFrameDuplicated`/capture-counter groups)
- Modify: `libs/engine/src/pipeline_diagnostics_aggregator.cpp` (implement each, mirroring `OnFrameDuplicated`'s body at `.cpp:305-308`)
- Modify: `libs/engine/include/exosnap/engine/pipeline_diagnostics.h` (add matching fields to `RecordingDiagnosticsSnapshot`, near `frames_duplicated` at line 96)
- Modify: `libs/engine/tests/test_pipeline_diagnostics.cpp` (one assertion per new counter)

Run this task **before** Task 4 (or interleave immediately after Task 4 Step 4 and before its Step 5 build-verify) — Tasks 4, 5, 7 and 8 all call methods this task adds, and none of them will compile standalone otherwise. The steps below are self-contained regardless of when they run.

**Interfaces:**
- Produces (exact names used by Tasks 4/5/7/8 above): `OnScreenGenerationChanged()`, `OnWebcamGenerationChanged()`, `OnCursorOnlyCaptureEventIgnored()`, `OnPhaseRingCursorOnlyEventIgnored()`, `OnFullComposition()`, `OnReusedYuvFrame()`, `OnYuvSlotCopy()`, `OnYuvSlotCopySkipped()`. Two more from the spec's section-13 list are not called by any task above (no code path in this plan produces them honestly) and are intentionally **not added**: `overlay_generation_changes` (nothing in this plan's scope bumps `visualGenerations.overlay` yet — Settings-driven overlay geometry/opacity/chroma-key changes are app-layer state this engine-only plan does not wire up; add it alongside whichever future task first increments `visualGenerations.overlay`) and `video_processor_conversions` (identical to `full_compositions` in this codebase — the `VideoProcessorBlt` call and the compositor pass are not separately skippable today, so a second counter would always read the same number as the first and add nothing).

- [ ] **Step 1: Write the failing test**

Add to `libs/engine/tests/test_pipeline_diagnostics.cpp`, following the file's existing pattern for other counters (grep for `OnFrameDuplicated` in that file to match the exact `Reset()`/`BuildSnapshot()` fixture idiom already used, then add a parallel case):

```cpp
TEST(PipelineDiagnosticsAggregator, RetainedFrameCountersRoundTrip) {
    PipelineDiagnosticsAggregator agg;
    agg.Reset(1, DiagnosticsStaticConfig{});

    agg.OnScreenGenerationChanged();
    agg.OnScreenGenerationChanged();
    agg.OnWebcamGenerationChanged();
    agg.OnCursorOnlyCaptureEventIgnored();
    agg.OnPhaseRingCursorOnlyEventIgnored();
    agg.OnFullComposition();
    agg.OnReusedYuvFrame();
    agg.OnReusedYuvFrame();
    agg.OnYuvSlotCopy();
    agg.OnYuvSlotCopySkipped();
    agg.OnYuvSlotCopySkipped();
    agg.OnYuvSlotCopySkipped();

    const auto snap = agg.BuildSnapshot(PipelineDiagnosticsAggregator::clock::now(), SessionStats{},
                                        DiagnosticsLifecycle::Recording, 1.0);

    EXPECT_EQ(snap.screen_generation_changes, 2u);
    EXPECT_EQ(snap.webcam_generation_changes, 1u);
    EXPECT_EQ(snap.cursor_only_capture_events_ignored, 1u);
    EXPECT_EQ(snap.phase_ring_cursor_only_events_ignored, 1u);
    EXPECT_EQ(snap.full_compositions, 1u);
    EXPECT_EQ(snap.reused_yuv_frames, 2u);
    EXPECT_EQ(snap.yuv_slot_copies, 1u);
    EXPECT_EQ(snap.yuv_slot_copies_skipped, 3u);
}
```

(Match the exact `Reset`/`BuildSnapshot` argument types by first reading how an existing `TEST(PipelineDiagnosticsAggregator, ...)` case in that file constructs `DiagnosticsStaticConfig`/`SessionStats`/`DiagnosticsLifecycle` — copy that fixture pattern verbatim rather than the illustrative call above if the real constructors differ.)

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target test_pipeline_diagnostics`
Expected: FAIL — `no member named 'OnScreenGenerationChanged' in 'exosnap::engine::PipelineDiagnosticsAggregator'` (and similarly for the other seven).

- [ ] **Step 3: Declare the methods and counters**

In `libs/engine/src/pipeline_diagnostics_aggregator.h`, add near the existing `OnFrameDuplicated()` declaration (line 347):

```cpp
    // Retained-frame reuse (CV-RETAIN-001..004, CV-CURSOR-001..003, CV-PACING-001).
    void OnScreenGenerationChanged() noexcept;
    void OnWebcamGenerationChanged() noexcept;
    void OnCursorOnlyCaptureEventIgnored() noexcept;      // Ignorable-classified OD acquire
    void OnPhaseRingCursorOnlyEventIgnored() noexcept;    // CursorOnly-classified: no ring entry made
    void OnFullComposition() noexcept;                    // a real composite+convert ran
    void OnReusedYuvFrame() noexcept;                     // CFR duplicate reused the cached YUV frame
    void OnYuvSlotCopy() noexcept;                        // duplicate path actually copied into the slot
    void OnYuvSlotCopySkipped() noexcept;                 // duplicate path found the slot already current
```

and near `frames_duplicated_` (line 468):

```cpp
    uint64_t screen_generation_changes_ = 0;
    uint64_t webcam_generation_changes_ = 0;
    uint64_t cursor_only_capture_events_ignored_ = 0;
    uint64_t phase_ring_cursor_only_events_ignored_ = 0;
    uint64_t full_compositions_ = 0;
    uint64_t reused_yuv_frames_ = 0;
    uint64_t yuv_slot_copies_ = 0;
    uint64_t yuv_slot_copies_skipped_ = 0;
```

- [ ] **Step 4: Implement, matching `OnFrameDuplicated`'s body exactly**

In `libs/engine/src/pipeline_diagnostics_aggregator.cpp`, near the existing `OnFrameDuplicated()` definition (`.cpp:305-308`):

```cpp
void PipelineDiagnosticsAggregator::OnScreenGenerationChanged() noexcept {
    std::lock_guard lk(mutex_);
    ++screen_generation_changes_;
}
void PipelineDiagnosticsAggregator::OnWebcamGenerationChanged() noexcept {
    std::lock_guard lk(mutex_);
    ++webcam_generation_changes_;
}
void PipelineDiagnosticsAggregator::OnCursorOnlyCaptureEventIgnored() noexcept {
    std::lock_guard lk(mutex_);
    ++cursor_only_capture_events_ignored_;
}
void PipelineDiagnosticsAggregator::OnPhaseRingCursorOnlyEventIgnored() noexcept {
    std::lock_guard lk(mutex_);
    ++phase_ring_cursor_only_events_ignored_;
}
void PipelineDiagnosticsAggregator::OnFullComposition() noexcept {
    std::lock_guard lk(mutex_);
    ++full_compositions_;
}
void PipelineDiagnosticsAggregator::OnReusedYuvFrame() noexcept {
    std::lock_guard lk(mutex_);
    ++reused_yuv_frames_;
}
void PipelineDiagnosticsAggregator::OnYuvSlotCopy() noexcept {
    std::lock_guard lk(mutex_);
    ++yuv_slot_copies_;
}
void PipelineDiagnosticsAggregator::OnYuvSlotCopySkipped() noexcept {
    std::lock_guard lk(mutex_);
    ++yuv_slot_copies_skipped_;
}
```

Find `BuildSnapshot`'s body (grep for `RecordingDiagnosticsSnapshot PipelineDiagnosticsAggregator::BuildSnapshot`) and add, alongside the existing `s.frames_duplicated = frames_duplicated_;`-style copies:

```cpp
    s.screen_generation_changes = screen_generation_changes_;
    s.webcam_generation_changes = webcam_generation_changes_;
    s.cursor_only_capture_events_ignored = cursor_only_capture_events_ignored_;
    s.phase_ring_cursor_only_events_ignored = phase_ring_cursor_only_events_ignored_;
    s.full_compositions = full_compositions_;
    s.reused_yuv_frames = reused_yuv_frames_;
    s.yuv_slot_copies = yuv_slot_copies_;
    s.yuv_slot_copies_skipped = yuv_slot_copies_skipped_;
```

Also reset every new counter to 0 inside `Reset()` — grep for where `frames_duplicated_ = 0;` (or equivalent) already happens in `Reset()`'s body and add the eight new counters next to it.

- [ ] **Step 5: Add the snapshot fields**

In `libs/engine/include/exosnap/engine/pipeline_diagnostics.h`, next to `uint64_t frames_duplicated = 0;` (line 96):

```cpp
    uint64_t screen_generation_changes = 0;
    uint64_t webcam_generation_changes = 0;
    uint64_t cursor_only_capture_events_ignored = 0;
    uint64_t phase_ring_cursor_only_events_ignored = 0;
    uint64_t full_compositions = 0;
    uint64_t reused_yuv_frames = 0;
    uint64_t yuv_slot_copies = 0;
    uint64_t yuv_slot_copies_skipped = 0;
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake --build build/windows-x64-debug --target test_pipeline_diagnostics`
Then: `ctest --test-dir build/windows-x64-debug -R "recorder_core.*RetainedFrameCountersRoundTrip" --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add libs/engine/src/pipeline_diagnostics_aggregator.h \
        libs/engine/src/pipeline_diagnostics_aggregator.cpp \
        libs/engine/include/exosnap/engine/pipeline_diagnostics.h \
        libs/engine/tests/test_pipeline_diagnostics.cpp
git commit -m "Add retained-frame reuse diagnostics counters (spec section 13)"
```

---

## Final integration check (after all 9 tasks)

- [ ] Full build: `cmake --build build/windows-x64-debug --target exosnap`
- [ ] Full test suite (per [[feedback_build_full_test_suite]] — `--target exosnap` alone does not build tests): `cmake --build build/windows-x64-debug` then `ctest --test-dir build/windows-x64-debug --output-on-failure`
- [ ] Start the app once (per CLAUDE.md — confirm no startup crash), close it. Do **not** drive it further; the actual generation/reuse/recomposite behavior needs a human live-verify pass (static desktop + moving cursor + webcam PiP, watching the new diagnostics counters climb) — add that as a follow-up item to the existing live-verify checklist rather than attempting it here.
- [ ] Update `docs/product-spec.md` only if any of the above changed *user-visible* behavior — it should not have; this plan is diagnostics + internal reuse only. If a step surprised you into a visible behavior change, stop and flag it rather than silently updating the spec.
