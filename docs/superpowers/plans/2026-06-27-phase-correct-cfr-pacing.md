# Phase-Correct CFR Frame Pacing (Slice 2, ADR 0035) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the CFR scheduler's "newest captured frame at each tick" with phase-correct frame *selection* — choosing, per 60 Hz output slot, the captured frame whose source present time is nearest the slot's ideal time — so high-refresh/VRR sources record to smooth, judder-free fixed-rate video.

**Architecture:** A small GPU ring of recently-captured textures (each tagged with its `LastPresentTime` QPC) replaces the single "latest" texture in the CFR path of `video_thread.cpp`. Per tick, a **pure** selection function picks the phase-nearest unemitted ring entry; only the selected entry is composited→encoded (no blending, near-zero overhead). DXGI-OD only (it has `LastPresentTime`); WGC capture and the `Newest` mode fall back to today's newest-at-tick. The testable core — ring sizing and the selection/drop-accounting decision — is extracted as pure functions; the GPU ring mechanics are dev-machine-verified by a real recording.

**Tech Stack:** C++20, D3D11 (capture textures), `recorder_core` (engine), Qt 6.9 (settings UI), GoogleTest, toml++ (preset).

## Global Constraints

Apply to every task. Verbatim from ADR 0035 + the locked design decisions:

- **GPU-only, selection not interpolation.** Keep a ring of recently-captured textures + each one's `LastPresentTime`; at each tick **select** the ring entry whose present time is nearest the slot time. No GPU→CPU readback, no blending. Only the selected entry is composited/encoded.
- **No elevation.** `LastPresentTime` comes from `DXGI_OUTDUPL_FRAME_INFO` (unprivileged desktop-duplication). The pacer works at standard user level for **monitor capture**. **Window/region (WGC) has no `LastPresentTime` → falls back to newest-at-tick.**
- **Modes:** `FramePacingMode { Smooth = 0, Newest = 1 }`. **Default `Smooth`** (recording is the use case; latency irrelevant). `Newest` is kept for lowest-latency and also falls back to newest-at-tick.
- **Ring size is adaptive:** `N = clamp(ceil(monitor_refresh_hz / output_fps) + 2, 4, 12)`; **fallback `N = 8` when refresh is unknown (0) or fps is 0** (covers up to ~240→60). The slow-source case (game < output fps) needs no large ring (it duplicates); ring size is driven by the source-faster-than-output case.
- **Drop/dup accounting must stay truthful.** A duplicated slot = no fresh ring entry near the slot time. A dropped frame = a captured ring entry never selected (skipped because a phase-nearer/newer one was chosen, or evicted on ring overflow). The existing `duplicated_video_frames` / `dropped_or_skipped_video_frames` stats must keep reporting honestly.
- **VFR is unaffected** (it already preserves source timing). This is a CFR-path-only change.
- **Bounded latency is acceptable:** phase-correct selection may pick a frame up to ~1 source-frame older than the absolute newest — irrelevant for recordings (hence Smooth default).
- **Pre-1.0:** preset schema **bump + reset** (no migration). The new `frame_pacing` field bumps `kPresetSchemaVersion` 18→19.
- **Build/verify:** VS-tree `build/windows-x64-debug`; Qt bin on PATH for renders. Full build (no `--target`) + `ctest -j6` before commit. Hot-path GPU behavior is **dev-machine-verified by a real recording** (a high-refresh/VRR source); not headless-verifiable.

---

## File Structure

| File | Responsibility | New? |
|------|----------------|------|
| `libs/recorder_core/include/recorder_core/frame_pacing.h` | Pure: `FramePacingMode`, `ComputePacingRingSize`, `PacingDecision`, `SelectFrameForSlot` | Create |
| `libs/recorder_core/src/frame_pacing.cpp` | Implementations of the pure pacing functions | Create |
| `libs/recorder_core/include/recorder_core/recorder_session.h` | Add `FramePacingMode cfr_pacing_mode = Smooth` to `RecorderConfig` (near `cfr`, ~line 289) | Modify |
| `libs/recorder_core/src/video_thread.cpp` | CFR path (1416–1711): GPU ring + phase-correct selection + accounting | Modify |
| `app/models/VideoSettingsModel.h` | Add `recorder_core::FramePacingMode frame_pacing = Smooth` (near `cfr`, line 11) | Modify |
| `app/models/RecordingPreset.{h,cpp}` | Default + sanitize the field; bump `kPresetSchemaVersion` 18→19 | Modify |
| `app/settings/RecordingPresetStore.cpp` | TOML serialize/deserialize `frame_pacing` | Modify |
| `app/MainWindow.cpp` | Map `VideoSettingsModel.frame_pacing` → `RecorderConfig.cfr_pacing_mode` in live-config capture; handle `fix.frame_pacing.smooth` | Modify |
| `app/pages/ConfigPage.{h,cpp}` | Expert Video select: Smooth/Newest pacing | Modify |
| `app/diagnostics/RecommendationEngine.{h,cpp}` | Conditional `fix.frame_pacing.smooth` Auto FixAction on judder when mode==Newest | Modify |
| `docs/decisions/0035-phase-correct-cfr-frame-pacing.md` | Record delivered; `docs/roadmap.md`: move pacing from 0.10.0 → 0.8.0 | Modify |

---

## Task 1: Settings plumbing — `FramePacingMode` + preset schema 18→19

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/recorder_session.h` (RecorderConfig, ~line 289)
- Modify: `app/models/VideoSettingsModel.h` (line 11 area)
- Modify: `app/models/RecordingPreset.{h,cpp}` (default + sanitize + `kPresetSchemaVersion` line 19)
- Modify: `app/settings/RecordingPresetStore.cpp` (~line 554 write / ~816 read)
- Test: `app/tests/test_audio_encoding_preset.cpp` (the preset roundtrip suite) — or the existing preset-store test file

**Interfaces:**
- Produces: `enum class recorder_core::FramePacingMode : uint8_t { Smooth = 0, Newest = 1 };` (declared in `frame_pacing.h`, Task 2 creates that header — but the enum is needed here first, so **Task 1 creates `frame_pacing.h` with only the enum**; Task 2 adds the functions). `RecorderConfig.cfr_pacing_mode`, `VideoSettingsModel.frame_pacing`, `RecordingPreset…video.frame_pacing`, all default `Smooth`. `kPresetSchemaVersion == 19`.

- [ ] **Step 1: Create `frame_pacing.h` with the enum only:**

```cpp
#pragma once
#include <cstdint>

namespace recorder_core {

// CFR output pacing. Smooth = phase-correct present-time-nearest selection (default,
// the recording use case). Newest = lowest-latency newest-at-tick (and the WGC fallback).
enum class FramePacingMode : uint8_t {
    Smooth = 0,
    Newest = 1,
};

} // namespace recorder_core
```

- [ ] **Step 2: Add the field to `RecorderConfig`** (`recorder_session.h`, right after the `bool cfr = true;` at ~line 289). Include `frame_pacing.h` at the top:

```cpp
    // CFR frame pacing (ADR 0035). Smooth = phase-correct selection; Newest = newest-at-tick.
    // Ignored for VFR and for WGC capture (no LastPresentTime → newest-at-tick).
    FramePacingMode cfr_pacing_mode = FramePacingMode::Smooth;
```

- [ ] **Step 3: Add to `VideoSettingsModel.h`** (after `bool cfr = true;`, line 11), include `recorder_core/frame_pacing.h`:

```cpp
    recorder_core::FramePacingMode frame_pacing = recorder_core::FramePacingMode::Smooth;
```

- [ ] **Step 4: Write the failing preset-roundtrip test.** In the preset store test file, add a case asserting the field defaults to `Smooth` and survives a save→load roundtrip when set to `Newest`:

```cpp
TEST(RecordingPresetStore, FramePacingRoundtrips) {
    using recorder_core::FramePacingMode;
    RecordingPreset p = RecordingPreset::Defaults();
    EXPECT_EQ(p.config.video.frame_pacing, FramePacingMode::Smooth);   // default
    p.config.video.frame_pacing = FramePacingMode::Newest;
    // serialize + deserialize via the store's TOML path (mirror an existing roundtrip test here)
    const std::string toml = RecordingPresetStore::ToToml(p);
    const RecordingPreset back = RecordingPresetStore::FromToml(toml).value();
    EXPECT_EQ(back.config.video.frame_pacing, FramePacingMode::Newest);
}
```

(Use the real serialize/deserialize entry points the existing roundtrip tests use — match their names.)

- [ ] **Step 5: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target <preset-store-test-target>` then ctest the case.
Expected: FAIL — `frame_pacing` not a member / not serialized.

- [ ] **Step 6: Default + sanitize + bump schema.** In `RecordingPreset.h` bump `inline constexpr int kPresetSchemaVersion = 18;` → `19`. In the defaults builder (~line 111 area) set `preset.config.video.frame_pacing = recorder_core::FramePacingMode::Smooth;`. In the sanitize path (`RecordingPreset.cpp`), clamp any out-of-range integer value to `Smooth` (mirror how `cfr`/enum fields are sanitized). In `RecordingPresetStore.cpp` write the field as an integer (`vid_tbl.emplace("frame_pacing", static_cast<int>(vid.frame_pacing));` near line 554) and read it back (`vid.frame_pacing = static_cast<FramePacingMode>(TomlInt(tbl["video"]["frame_pacing"], 0));` near line 816, default 0 = Smooth).

- [ ] **Step 7: Run to verify it passes**

Run: ctest the roundtrip case + the existing preset schema-version test (expects 19 now — update it if it pins the number).
Expected: PASS.

- [ ] **Step 8: Map in MainWindow.** In `MainWindow.cpp` where `VideoSettingsModel` → `RecorderConfig` for `Record()` (the live-config capture, near where `cfr` is copied), add `config.cfr_pacing_mode = video_settings_.frame_pacing;`.

- [ ] **Step 9: Commit**

```bash
git add libs/recorder_core/include/recorder_core/frame_pacing.h libs/recorder_core/include/recorder_core/recorder_session.h app/models/VideoSettingsModel.h app/models/RecordingPreset.h app/models/RecordingPreset.cpp app/settings/RecordingPresetStore.cpp app/MainWindow.cpp <preset-test-file>
git commit -m "feat(pacing): FramePacingMode setting + preset schema 18->19 (ADR 0035)"
```

---

## Task 2: Pure ring-size helper `ComputePacingRingSize` (TDD)

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/frame_pacing.h` (add declaration)
- Create: `libs/recorder_core/src/frame_pacing.cpp`
- Test: `libs/recorder_core/tests/test_frame_pacing.cpp` (new gtest; add target to `libs/recorder_core/CMakeLists.txt`)

**Interfaces:**
- Produces: `std::size_t recorder_core::ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps);`

- [ ] **Step 1: Write the failing test** `test_frame_pacing.cpp`:

```cpp
#include "recorder_core/frame_pacing.h"
#include <gtest/gtest.h>
using namespace recorder_core;

TEST(PacingRingSize, AdaptiveFromRatio) {
    EXPECT_EQ(ComputePacingRingSize(144, 60), 5u);  // ceil(144/60)=3, +2 = 5
    EXPECT_EQ(ComputePacingRingSize(240, 60), 6u);  // ceil(240/60)=4, +2 = 6
    EXPECT_EQ(ComputePacingRingSize(60, 60), 4u);   // ceil=1, +2=3 -> clamped up to min 4
}
TEST(PacingRingSize, ClampsToMax) {
    EXPECT_EQ(ComputePacingRingSize(1000, 60), 12u); // huge ratio clamps to 12
}
TEST(PacingRingSize, UnknownRefreshFallsBackTo8) {
    EXPECT_EQ(ComputePacingRingSize(0, 60), 8u);
    EXPECT_EQ(ComputePacingRingSize(144, 0), 8u);
}
```

- [ ] **Step 2: Add the gtest target** to `libs/recorder_core/CMakeLists.txt` (mirror an existing `recorder_core` unit-test target; `frame_pacing.cpp` is pure, links no D3D):

```cmake
exosnap_add_gtest(
    NAME frame_pacing_tests
    TEST_PREFIX frame_pacing.
    SOURCES tests/test_frame_pacing.cpp src/frame_pacing.cpp
    LIBRARIES "")
target_include_directories(frame_pacing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

(Match the repo's actual `exosnap_add_gtest` signature used by other `recorder_core` tests.)

- [ ] **Step 3: Run → FAIL** (`ComputePacingRingSize` undefined).

- [ ] **Step 4: Declare in `frame_pacing.h`:**

```cpp
#include <cstddef>
// ... inside namespace recorder_core ...
// Ring size for phase-correct pacing: clamp(ceil(refresh/fps)+2, 4, 12); 8 when refresh
// or fps is unknown (0). Sized for the source-faster-than-output case (e.g. 240->60).
[[nodiscard]] std::size_t ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps);
```

- [ ] **Step 5: Implement in `frame_pacing.cpp`:**

```cpp
#include "recorder_core/frame_pacing.h"
#include <algorithm>

namespace recorder_core {

std::size_t ComputePacingRingSize(uint32_t monitor_refresh_hz, uint32_t output_fps) {
    constexpr std::size_t kMin = 4, kMax = 12, kFallback = 8;
    if (monitor_refresh_hz == 0 || output_fps == 0) {
        return kFallback;
    }
    const std::size_t ratio = (monitor_refresh_hz + output_fps - 1) / output_fps; // ceil
    return std::clamp(ratio + 2, kMin, kMax);
}

} // namespace recorder_core
```

- [ ] **Step 6: Run → PASS.** `ctest --test-dir build/windows-x64-debug -R "frame_pacing\." -j6`

- [ ] **Step 7: Commit**

```bash
git add libs/recorder_core/include/recorder_core/frame_pacing.h libs/recorder_core/src/frame_pacing.cpp libs/recorder_core/tests/test_frame_pacing.cpp libs/recorder_core/CMakeLists.txt
git commit -m "feat(pacing): adaptive ring-size helper + tests"
```

---

## Task 3: Pure frame-selection `SelectFrameForSlot` (TDD)

**Files:**
- Modify: `libs/recorder_core/include/recorder_core/frame_pacing.h`, `libs/recorder_core/src/frame_pacing.cpp`
- Test: `libs/recorder_core/tests/test_frame_pacing.cpp`

**Interfaces:**
- Produces:
```cpp
struct PacingDecision {
    bool emit = false;            // true → encode ring[index]; false → duplicate previous slot
    std::size_t index = 0;        // valid iff emit
    uint32_t newly_dropped = 0;   // fresh entries strictly older than the chosen one (skipped)
};
// ring_present_qpc: present-time QPC of each LIVE ring entry, ASCENDING capture order.
// slot_qpc: ideal present time of this output slot. last_emitted_present_qpc: present time
// of the last frame already encoded (0 if none). Only entries strictly newer than
// last_emitted are "fresh" (eligible) — guarantees monotonic, non-repeating selection.
PacingDecision SelectFrameForSlot(std::span<const uint64_t> ring_present_qpc,
                                  uint64_t slot_qpc, uint64_t last_emitted_present_qpc,
                                  FramePacingMode mode);
```

- [ ] **Step 1: Write the failing tests:**

```cpp
#include <span>
TEST(SelectFrame, SmoothPicksNearestFresh) {
    const uint64_t ring[] = {100, 200, 300, 400}; // present QPCs, ascending
    // slot=250, last_emitted=0 → nearest is 200 (idx1) or 300 (idx2); |250-200|=|250-300| → tie→lower idx
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 250, 0, FramePacingMode::Smooth);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 1u);                 // 200 chosen
    EXPECT_EQ(d.newly_dropped, 1u);         // entry 100 skipped (fresh, older than chosen)
}
TEST(SelectFrame, NewestPicksLastFresh) {
    const uint64_t ring[] = {100, 200, 300};
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 150, 0, FramePacingMode::Newest);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 2u);                 // newest (300)
    EXPECT_EQ(d.newly_dropped, 2u);         // 100 and 200 skipped
}
TEST(SelectFrame, FiltersAlreadyEmitted) {
    const uint64_t ring[] = {100, 200, 300};
    // last_emitted=200 → only 300 is fresh
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 250, 200, FramePacingMode::Smooth);
    EXPECT_TRUE(d.emit);
    EXPECT_EQ(d.index, 2u);
    EXPECT_EQ(d.newly_dropped, 0u);
}
TEST(SelectFrame, NoFreshEntriesDuplicates) {
    const uint64_t ring[] = {100, 200};
    auto d = SelectFrameForSlot(std::span<const uint64_t>(ring), 999, 200, FramePacingMode::Smooth);
    EXPECT_FALSE(d.emit);                   // all <= last_emitted → duplicate
    EXPECT_EQ(d.newly_dropped, 0u);
}
TEST(SelectFrame, EmptyRingDuplicates) {
    auto d = SelectFrameForSlot(std::span<const uint64_t>(), 100, 0, FramePacingMode::Smooth);
    EXPECT_FALSE(d.emit);
}
```

- [ ] **Step 2: Run → FAIL** (`SelectFrameForSlot`/`PacingDecision` undefined).

- [ ] **Step 3: Declare in `frame_pacing.h`** (add `#include <span>`, `#include <cstdint>`): the `PacingDecision` struct + the function signature from Interfaces above.

- [ ] **Step 4: Implement in `frame_pacing.cpp`:**

```cpp
#include <cstdint>
#include <span>

PacingDecision SelectFrameForSlot(std::span<const uint64_t> ring_present_qpc,
                                  uint64_t slot_qpc, uint64_t last_emitted_present_qpc,
                                  FramePacingMode mode) {
    PacingDecision d;
    // Find the fresh window: entries strictly newer than the last emitted present time.
    std::size_t first_fresh = ring_present_qpc.size();
    for (std::size_t i = 0; i < ring_present_qpc.size(); ++i) {
        if (ring_present_qpc[i] > last_emitted_present_qpc) { first_fresh = i; break; }
    }
    if (first_fresh == ring_present_qpc.size()) {
        return d; // no fresh entry → duplicate
    }
    std::size_t chosen = first_fresh;
    if (mode == FramePacingMode::Newest) {
        chosen = ring_present_qpc.size() - 1;             // last (newest) fresh entry
    } else {
        // Smooth: nearest present time to slot among fresh entries (ascending → first min wins on tie).
        uint64_t best_dist = UINT64_MAX;
        for (std::size_t i = first_fresh; i < ring_present_qpc.size(); ++i) {
            const uint64_t p = ring_present_qpc[i];
            const uint64_t dist = p >= slot_qpc ? p - slot_qpc : slot_qpc - p;
            if (dist < best_dist) { best_dist = dist; chosen = i; }
        }
    }
    d.emit = true;
    d.index = chosen;
    d.newly_dropped = static_cast<uint32_t>(chosen - first_fresh); // fresh entries older than chosen, skipped
    return d;
}
```

- [ ] **Step 5: Run → PASS.** `ctest --test-dir build/windows-x64-debug -R "frame_pacing\." -j6`

- [ ] **Step 6: Commit**

```bash
git add libs/recorder_core/include/recorder_core/frame_pacing.h libs/recorder_core/src/frame_pacing.cpp libs/recorder_core/tests/test_frame_pacing.cpp
git commit -m "feat(pacing): pure phase-correct frame selection + drop accounting + tests"
```

---

## Task 4: GPU ring + phase-correct selection in the CFR scheduler (hot-path, dev-verified)

**Files:**
- Modify: `libs/recorder_core/src/video_thread.cpp` (CFR path 1416–1711: drain 1473–1521, selection 1611–1612, accounting 1516/1549/1665/1671)

**Interfaces:**
- Consumes: `ComputePacingRingSize`, `SelectFrameForSlot`, `PacingDecision`, `m_state.config.cfr_pacing_mode`.

> **Hot-path GPU I/O — not headless-verifiable.** The selection LOGIC is already unit-tested (Task 3). This task wires it to D3D11 textures; verification is compile + a **dev-machine recording** of a high-refresh/VRR source, confirming the output is smooth and drop/dup stats stay sane. Keep the change surgical — only the DXGI-OD CFR drain + per-tick selection; do NOT touch the VFR path or the WGC newest-at-tick fallback.

- [ ] **Step 1: Add the ring state** at the top of the CFR block (~line 1422, beside `odCapturedTex`). Mirror `odCapturedTex`'s `D3D11_TEXTURE2D_DESC`; allocate `N = ComputePacingRingSize(monitor_refresh_hz, frame_rate_num/den)` textures (resolve the refresh from the same source the diagnostics use; if unavailable pass 0 → fallback 8). Only build the ring when `useOdCapture && m_state.config.cfr_pacing_mode == FramePacingMode::Smooth`; otherwise keep today's single-texture path:

```cpp
struct CaptureRingEntry { winrt::com_ptr<ID3D11Texture2D> tex; uint64_t presentQpc = 0; };
std::vector<CaptureRingEntry> captureRing;
size_t ringHead = 0; uint64_t lastEmittedPresentQpc = 0;
const bool usePhaseCorrect = useOdCapture && m_state.config.cfr_pacing_mode == FramePacingMode::Smooth;
if (usePhaseCorrect) {
    const size_t ringN = ComputePacingRingSize(monitorRefreshHz, frame_rate_num /*assume den=1*/);
    captureRing.resize(ringN);
    for (auto& e : captureRing) { /* CreateTexture2D with odCapturedTex's desc; non-fatal on failure → disable phase-correct */ }
}
```

- [ ] **Step 2: Drain into the ring** (replace the DXGI-OD drain's "keep newest" at ~1484–1520 when `usePhaseCorrect`): for each acquired frame, `CopyResource` into `captureRing[ringHead].tex`, record `presentQpc` from `info.LastPresentTime`, advance `ringHead` round-robin. **Eviction accounting:** if the entry being overwritten had `presentQpc > lastEmittedPresentQpc` (was fresh, never emitted) → `++droppedFrames`. Keep the existing `OnSourcePresentInterval` tap unchanged. When `!usePhaseCorrect`, keep the existing single-texture drain verbatim.

- [ ] **Step 3: Phase-correct selection per tick** (replace the `rawSourceTex = newest` at ~1611–1612 when `usePhaseCorrect`): build an ascending `std::vector<uint64_t>` of the live ring entries' `presentQpc` (skip empty slots; the round-robin buffer must be linearised oldest→newest), compute `slotQpc` = the QPC corresponding to this tick's ideal present time (`recording_start_qpc + cfr_frame_idx * frame_interval` in QPC units — reuse the same QPC clock the present tap uses), then:

```cpp
const PacingDecision dec = SelectFrameForSlot(presentQpcsAscending, slotQpc,
                                              lastEmittedPresentQpc, FramePacingMode::Smooth);
droppedFrames += dec.newly_dropped;
if (dec.emit) {
    rawSourceTex = captureRing[liveIndexToRingSlot[dec.index]].tex.get();   // selected fresh frame
    lastEmittedPresentQpc = presentQpcsAscending[dec.index];
} else {
    rawSourceTex = nullptr;   // → existing refNv12 duplicate path (++duplicatedFrames) or CFR-skip drop
}
```

The existing composite→VideoProcessorBlt→encode, the `refNv12` duplicate path (1654/1663–1665), and the CFR-skip drop (1671) stay as-is — `rawSourceTex == nullptr` routes to them exactly like today.

- [ ] **Step 4: Newest mode + WGC unchanged.** When `m_state.config.cfr_pacing_mode == FramePacingMode::Newest` OR `!useOdCapture` (WGC), `usePhaseCorrect` is false → the original newest-at-tick code runs verbatim. Confirm no behavior change on that path.

- [ ] **Step 5: Build + verify compile.**

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: links. (No unit test — selection logic is covered by Task 3.)

- [ ] **Step 6: Commit**

```bash
git add libs/recorder_core/src/video_thread.cpp
git commit -m "feat(pacing): GPU ring + phase-correct selection in the CFR scheduler (DXGI-OD)"
```

- [ ] **Step 7: Dev-machine recording verification** (honest boundary). On a high-refresh or VRR monitor: record a smooth-motion source in Smooth vs Newest mode; confirm Smooth removes judder, the file is valid 60 fps, and `duplicated`/`dropped` stats are plausible (not wildly inflated). Record the outcome; do not commit any recording.

---

## Task 5: Expert Video settings select (Smooth / Newest)

**Files:**
- Modify: `app/pages/ConfigPage.{h,cpp}` (Expert Video section, near the frame-timing/`cfr` control)
- Test: `app/tests/test_config_page.cpp`

**Interfaces:**
- Consumes: `VideoSettingsModel.frame_pacing`. Produces: a control named `framePacingSelect` reflecting/setting `frame_pacing`.

- [ ] **Step 1: Write the failing test** in `test_config_page.cpp`: after constructing the page and applying a `VideoSettingsModel` with `frame_pacing = Newest`, the `framePacingSelect` widget exists and reflects Newest; toggling it to Smooth updates the emitted model.

```cpp
TEST(ConfigPageTest, FramePacingSelectReflectsAndSetsModel) {
    ConfigPage page;
    auto* sel = page.findChild<QComboBox*>(QStringLiteral("framePacingSelect"));
    ASSERT_NE(sel, nullptr) << "framePacingSelect must exist in Expert Video";
    // set model Newest → control shows index for Newest; set control Smooth → model Smooth
}
```

(Use the real control type ConfigPage uses for similar enums — combo or segmented control — and its real apply/emit entry points.)

- [ ] **Step 2: Run → FAIL** (no `framePacingSelect`).

- [ ] **Step 3: Add the control** to the Expert Video section: a labelled select "Frame pacing" with options "Smooth (phase-correct)" / "Newest (lowest latency)", default Smooth, plus an info hint ("Smooth removes judder from high-refresh/VRR sources; Newest minimises latency"). Wire its change to `video_ui_state_.frame_pacing` and the existing `videoSettingsChanged` emission; populate it in the page's `applyVideoSettings`/refresh path (mirror the `cfr`/frame-timing control).

- [ ] **Step 4: Run → PASS.** `ctest --test-dir build/windows-x64-debug -R "ConfigPage" -j6`

- [ ] **Step 5: Commit**

```bash
git add app/pages/ConfigPage.h app/pages/ConfigPage.cpp app/tests/test_config_page.cpp
git commit -m "feat(pacing): Expert Video frame-pacing select (Smooth/Newest)"
```

---

## Task 6: `fix.frame_pacing.smooth` Auto FixAction on judder (TDD)

**Files:**
- Modify: `app/diagnostics/RecommendationEngine.{h,cpp}` (`checkRefreshRateMismatch`, the FixAction at ~158–164)
- Modify: `app/MainWindow.cpp` (FixAction handler, ~691–731)
- Test: `app/tests/test_diagnostics.cpp`

**Interfaces:**
- Consumes: the engine's config must expose the current pacing mode. Add `FramePacingMode frame_pacing` to the `capability::UserRecorderConfig` the engine reads (or thread it through the engine ctor). Produces: when judder fires **and** `frame_pacing == Newest`, a second `DiagnosticResult`-attached or sibling FixAction `id == "fix.frame_pacing.smooth"`, `safety == Auto`, recommending Smooth. When already `Smooth`, it is **not** offered.

> **Decision (locked):** the judder result keeps its existing `fix.fps.cap` action; the Smooth pacing fix is offered as a **second** result (id `rec.pacing.smooth`) so a result carries one primary `fix_action`. Emit `rec.pacing.smooth` only on judder + Newest.

- [ ] **Step 1: Write the failing tests** in `test_diagnostics.cpp`: with a live-judder snapshot (jitter > 4 ms, CFR) and config `frame_pacing == Newest`, `Generate()` contains a result `id == "rec.pacing.smooth"` whose `fix_action->id == "fix.frame_pacing.smooth"` and `safety == FixAction::Safety::Auto`. With `frame_pacing == Smooth`, that result is absent.

```cpp
TEST(RecommendationEngineTest, JudderInNewestOffersSmoothPacingAutoFix) {
    // build snapshot with live judder + config.frame_pacing = Newest
    // engine.Generate() → find id=="rec.pacing.smooth", fix_action->id=="fix.frame_pacing.smooth", safety==Auto
}
TEST(RecommendationEngineTest, JudderInSmoothOffersNoPacingFix) {
    // same judder but config.frame_pacing = Smooth → no "rec.pacing.smooth"
}
```

- [ ] **Step 2: Run → FAIL.**

- [ ] **Step 3: Thread the mode + emit the result.** Add `FramePacingMode frame_pacing = FramePacingMode::Smooth;` to `capability::UserRecorderConfig` (and populate it where `UserConfigFromSettings` builds it from `VideoSettingsModel`). In `checkRefreshRateMismatch`, after pushing the existing judder result, when the live-judder arm fired (or the static mismatch) **and** `config_.frame_pacing == FramePacingMode::Newest`, push a second result:

```cpp
if (config_.frame_pacing == recorder_core::FramePacingMode::Newest) {
    DiagnosticResult pr = MakeResult(
        "rec.pacing.smooth", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        "Smooth frame pacing recommended",
        "Phase-correct pacing removes judder from high-refresh / VRR sources.",
        "Your recording uses Newest frame pacing; the measured judder is exactly what "
        "Smooth (phase-correct) pacing fixes.",
        "Frame pacing: Newest",
        "Switch to Smooth frame pacing in Advanced Video settings.");
    FixAction fa;
    fa.id = "fix.frame_pacing.smooth";
    fa.label = "Switch to Smooth pacing";
    fa.safety = FixAction::Safety::Auto;     // safe, reversible, config-only
    fa.reversible = true;
    fa.changes_summary = "Sets video frame pacing to Smooth (phase-correct). Reversible in Advanced Video settings.";
    pr.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(pr));
}
```

- [ ] **Step 4: MainWindow Auto-apply.** In the FixAction handler (~691–731), add to the Auto branch: `else if (fix_id == QStringLiteral("fix.frame_pacing.smooth")) video_settings_.frame_pacing = recorder_core::FramePacingMode::Smooth;` then persist + refresh the ConfigPage select (so the change is visible). It is Auto, so it applies after the existing confirm/preview using `changes_summary`.

- [ ] **Step 5: Run → PASS.** `ctest --test-dir build/windows-x64-debug -R "diagnostics\." -j6`

- [ ] **Step 6: Commit**

```bash
git add app/diagnostics/RecommendationEngine.h app/diagnostics/RecommendationEngine.cpp app/MainWindow.cpp app/tests/test_diagnostics.cpp
git commit -m "feat(pacing): judder-recommends-Smooth Auto FixAction (ADR 0035 closes the loop with 0033)"
```

---

## Task 7: Full verification + ADR/roadmap + render

**Files:**
- Modify: `docs/decisions/0035-phase-correct-cfr-frame-pacing.md`, `docs/roadmap.md`

- [ ] **Step 1: Full build (no `--target`)** so all test targets compile with the new `frame_pacing.cpp` dependency.

Run: `cmake --build build/windows-x64-debug`
Expected: clean. If a `recorder_core`-consuming test target fails to find `frame_pacing.*`, add the source to its target (it's a new engine TU).

- [ ] **Step 2: Full ctest.**

Run: `ctest --test-dir build/windows-x64-debug -j6`
Expected: all pass (known HistoryStore/PresetStore `-j` flakes → re-run serially to confirm).

- [ ] **Step 3: Render-verify** the Expert Video select renders the Smooth/Newest control (Qt on PATH, the settings visual-test id).

- [ ] **Step 4: Docs.** In `docs/decisions/0035-...md` add a "Delivered" note (adaptive ring formula, pure-selection seam, Auto FixAction, schema 19); status stays Proposed until the 0.8.0 release-mechanics flip. In `docs/roadmap.md` move "Phase-correct CFR frame pacing" from the 0.10.0 row into 0.8.0 (the user pulled it forward).

- [ ] **Step 5: Commit**

```bash
git add docs/decisions/0035-phase-correct-cfr-frame-pacing.md docs/roadmap.md
git commit -m "docs: record phase-correct CFR pacing delivered; move to 0.8.0 (ADR 0035)"
```

---

## Self-Review

- **Spec coverage (ADR 0035):** GPU-only selection-not-blending (T3/T4) ✓ · ring of textures + LastPresentTime (T4) ✓ · adaptive ring size (T2) ✓ · no elevation / DXGI-OD only, WGC fallback (T4) ✓ · Smooth/Newest modes, default Smooth (T1/T5) ✓ · drop/dup accounting truthful (T3 `newly_dropped` + T4 eviction) ✓ · VFR unaffected (T4 touches only CFR+OD path) ✓ · diagnostics recommends Smooth FixAction (T6) ✓ · schema bump+reset (T1) ✓.
- **Placeholders:** the only non-literal regions are the explicitly-labelled hot-path GPU wiring (T4) — its decision logic is fully specified + unit-tested in T3; the D3D texture mechanics are the honest dev-verify boundary. Pure tasks (T2/T3) carry complete code + tests.
- **Type consistency:** `FramePacingMode{Smooth=0,Newest=1}` defined once (T1, `frame_pacing.h`); `ComputePacingRingSize` (T2) and `SelectFrameForSlot`/`PacingDecision` (T3) declared in the same header and consumed in T4; `RecorderConfig.cfr_pacing_mode` (T1) read in T4; `VideoSettingsModel.frame_pacing` (T1) used in T5/T6; `capability::UserRecorderConfig.frame_pacing` (T6) drives the conditional FixAction.
- **Known traps embedded:** preset schema test pins the version number (update to 19, T1); full build before commit catches any recorder_core test target needing the new TU (T7); the ring round-robin→ascending linearisation is called out explicitly (T4 Step 3).
