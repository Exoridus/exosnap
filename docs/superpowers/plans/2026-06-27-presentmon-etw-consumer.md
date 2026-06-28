# PresentMon In-Process ETW Consumer (Slice 1: ETW-Cluster A+B+C) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `PresentMonProvider` shell with a real in-process PresentMon ETW consumer that reports present-mode/tearing, wire it into the diagnostics snapshot + VRR/CFR correlation, add the "Exclusive Fullscreen → Borderless" killer FixAction, and add a minimal DPC/ISR latency check on the same elevation-gated ETW infrastructure.

**Architecture:** A vendored static lib (`presentmon_consumer`, Intel PresentMon `PresentData`, MIT) is fetched behind `EXOSNAP_WITH_PRESENTMON` (already an option, default ON). The app-layer `PresentMonProvider` owns an isolated `PresentMonEtwSession` (real-time ETW session + consumer worker thread, compiled only when `EXOSNAP_HAS_PRESENTMON`). Pure functions (present-mode mapping, killer-check, DPC threshold, correlation enrichment) carry the test coverage; the ETW I/O is thin, `#ifdef`-guarded, and verified only on the dev machine with a real game. The provider is **app-layer overlay**: it fills the `RecordingDiagnosticsSnapshot` present-mode fields (left `Unavailable` by the engine) before the page renders — the same one-direction dependency as `DiskSpaceProvider`. DXGI-OD stays authoritative for monitor present cadence (no elevation); PresentMon only fills the WGC/window gap and adds present-mode/tearing universally.

**Tech Stack:** C++20, CMake `FetchContent`, Win32 ETW (`evntrace.h`/`tdh.h`, `advapi32`/`tdh.lib`), Intel PresentMon `PresentData`, Qt 6.9 Widgets (UI), GoogleTest.

## Global Constraints

These apply to **every** task. Exact values, copied from ADR 0033 / the locked brainstorm decisions:

- **Build env:** VS-tree generator at `build/windows-x64-debug` (Ninja tree does not reconfigure under VS 2026). Qt bin (`C:\Qt\6.9.0\msvc2022_64\bin`) must be on `PATH` for any render/visual-test step.
- **Verification before commit:** full build (`cmake --build build/windows-x64-debug` — **no** `--target`, so Page/Widget test targets build) **and** `ctest --test-dir build/windows-x64-debug -j6`. New deps must be added to the SOURCES/LIBRARIES of every test target that compiles diagnostics sources directly (`present_provider_tests` `app/CMakeLists.txt:946`, `diagnostics_page_tests` `app/CMakeLists.txt:1244`, `exosnap_diagnostics_test_support` `app/CMakeLists.txt:433`).
- **Feature flag:** `EXOSNAP_WITH_PRESENTMON` (already `option(... ON)` at `CMakeLists.txt:141`) gates the vendored lib; it sets `EXOSNAP_HAS_PRESENTMON=1` on the consuming targets. **The provider source must compile and pass its gating tests when `EXOSNAP_HAS_PRESENTMON` is undefined** (graceful: always `available=false`).
- **PresentMon pin:** `v1.10.0` (SHA `2ce1158783e570539119f577d894252b395cadca`) via `FetchContent` — **never** a rolling `latest` (BtbN-pin lesson). v1.10.0 is the last 1.x line whose `PresentData` exposes the simple embeddable API (`TraceSession::Start` + `PMTraceConsumer::DequeuePresentEvents`); 2.x refactored into the IntelPresentMon middleware and is **not** the target. License registered via `_exosnap_install_license("PresentMon (MIT)" …)` from `LICENSE.txt`. Static lib built with `/w` (MSVC) over only the `PresentData/*.cpp` sources.
- **Verified PresentMon API (v1.10.0, do not re-guess):** `PresentData/PresentMonTraceConsumer.hpp` — `enum class PresentMode { Hardware_Legacy_Flip=1, Hardware_Legacy_Copy_To_Front_Buffer=2, Hardware_Independent_Flip=3, Composed_Flip=4, Composed_Copy_GPU_GDI=5, Composed_Copy_CPU_GDI=6, Hardware_Composed_Independent_Flip=8 }` (no 7). `struct PresentEvent { uint64_t PresentStartTime; uint32_t ProcessId; uint64_t ScreenTime; int32_t SyncInterval; uint64_t Hwnd; PresentMode PresentMode; bool SupportsTearing; … }`. `struct PMTraceConsumer` default-constructs with `mTrackDisplay=true, mTrackGPU=false, mTrackInput=false` (exactly what we want) and exposes thread-safe `void DequeuePresentEvents(std::vector<std::shared_ptr<PresentEvent>>&)`. `struct TraceSession { ULONG Start(PMTraceConsumer*, MRTraceConsumer* /*nullptr*/, wchar_t const* etlPath /*nullptr=realtime*/, wchar_t const* sessionName); void Stop(); LARGE_INTEGER mTimestampFrequency; TRACEHANDLE mTraceHandle; static ULONG StopNamedSession(wchar_t const*); }` — `Start` opens the realtime session, enables the providers, and calls `OpenTrace` itself; the consumer only needs draining.
- **Elevation gate (verbatim):** the provider is available **iff** `present_diagnostics_optin && IElevationProvider::IsElevated() && session_open`. Only the already-built relaunch-as-admin path unlocks it (no "Performance Log Users" UI).
- **Signal-source policy:** DXGI-OD remains authoritative for **monitor** present cadence (unprivileged). PresentMon fills the **WGC/window/game** gap and provides present-mode + tearing universally as *attribution*. Never double-count cadence; never couple the monitor judder diagnosis to elevation.
- **Sentinel honesty:** when the provider is not active or no present has been observed, fields stay `MetricAvailability::Unavailable` and the UI shows the em-dash (`kDash`). **Never fabricate a `Composed`/zero present.**
- **Pre-1.0:** schema **bump + reset**, never migrate — *only if* a persisted field is added (this slice adds none; see Task 9 note).
- **Verification boundary (honest):** real present/DPC data needs elevation **and** a running game — **not** headless-verifiable. Headless covers compile/link, graceful-`Unavailable`, and every pure mapping/correlation/threshold function. Live present-mode/tearing/DPC data is verified on the dev machine with a game and recorded as such.

---

## File Structure

| File | Responsibility | New? |
|------|----------------|------|
| `third_party/CMakeLists.txt` | `FetchContent` PresentMon (pinned), `presentmon_consumer` STATIC over `PresentData/`, MIT license | Modify |
| `CMakeLists.txt` | Link `presentmon_consumer` + `EXOSNAP_HAS_PRESENTMON` under the existing option | Modify |
| `app/diagnostics/PresentModeMapping.h/.cpp` | Pure: `RawPresentEvent` → `diagnostics::PresentSample`; no Win32/ETW headers | Create |
| `app/diagnostics/PresentMonEtwSession.h/.cpp` | ETW realtime session + consumer thread; only built when `EXOSNAP_HAS_PRESENTMON` | Create |
| `app/diagnostics/PresentMonProvider.h/.cpp` | Owns optional session (`#ifdef`), gates, returns latest sample, start/stop on opt-in | Modify |
| `app/diagnostics/DpcLatencyProvider.h/.cpp` | Minimal DPC/ISR latency: kernel system-trace session + `DpcLatencyReading`; pure threshold helper in `RecommendationEngine` | Create |
| `app/diagnostics/RecommendationEngine.h/.cpp` | Killer-check (Exclusive Fullscreen → Borderless), present-mode correlation enrichment, DPC threshold check | Modify |
| `app/pages/DiagnosticsPage.h/.cpp` | `setPresentProvider(IPresentProvider*)` + `setDpcProvider(...)`; overlay present-mode/tearing/latency onto the snapshot | Modify |
| `app/MainWindow.cpp` | Own the Win32 providers; start/stop session in `onPresentDiagnosticsOptInToggled`; inject into the page | Modify |
| `app/CMakeLists.txt` | Add new sources to `exosnap`, `present_provider_tests`, `diagnostics_page_tests`, `exosnap_diagnostics_test_support`; new `present_mapping_tests` | Modify |
| `app/tests/test_present_mode_mapping.cpp` | Pure mapping unit tests | Create |
| `app/tests/test_present_provider.cpp` | Extend: gating with `EXOSNAP_HAS_PRESENTMON` undefined | Modify |
| `app/tests/test_diagnostics.cpp` | Killer-check + correlation + DPC threshold unit tests | Modify |
| `docs/decisions/0033-diagnostics-engine-and-fixaction.md` | Document delivered subset/pin + DPC-minimal scope (status stays Proposed — flipped in the separate release slice) | Modify |

---

## Task 1: Vendor `presentmon_consumer` static lib

**Files:**
- Modify: `third_party/CMakeLists.txt` (append a new guarded block)
- Modify: `CMakeLists.txt:141` area (link + compile-def under the existing option)

**Interfaces:**
- Produces: CMake target `PresentMon::consumer` (alias of `presentmon_consumer`) exposing `PresentData/` headers; compile-def `EXOSNAP_HAS_PRESENTMON=1` on `exosnap`.

- [ ] **Step 1: Add the vendoring block** to `third_party/CMakeLists.txt` (follow the RNNoise pattern at lines 228–256). Use `file(GLOB)` for the vendored sources — we never add files to the upstream tree, so glob is the robust choice against tag drift:

```cmake
# ---- PresentMon ETW present-diagnostics consumer (MIT) — ADR 0033 -----------
# In-process ETW consumer subset (PresentData/ only — NOT the tool repo's main()s).
# Pinned to a fixed release tag (never a rolling latest — BtbN-pin lesson).
if(EXOSNAP_WITH_PRESENTMON)
    FetchContent_Declare(
        presentmon
        GIT_REPOSITORY https://github.com/GameTechDev/PresentMon.git
        GIT_TAG        2ce1158783e570539119f577d894252b395cadca   # == v1.10.0 (pinned by SHA). See ADR 0033.
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(presentmon)

    # Vendored subset: the PresentData ETW consumer translation units only.
    file(GLOB _presentmon_consumer_sources CONFIGURE_DEPENDS
        "${presentmon_SOURCE_DIR}/PresentData/*.cpp")
    add_library(presentmon_consumer STATIC ${_presentmon_consumer_sources})
    target_include_directories(presentmon_consumer PUBLIC "${presentmon_SOURCE_DIR}/PresentData")
    set_target_properties(presentmon_consumer PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(presentmon_consumer PRIVATE /w)
    else()
        target_compile_options(presentmon_consumer PRIVATE -w)
    endif()
    # PresentData consumes ETW/TDH directly.
    target_link_libraries(presentmon_consumer PUBLIC tdh advapi32)
    add_library(PresentMon::consumer ALIAS presentmon_consumer)
    _exosnap_install_license("PresentMon (MIT)"
        "${presentmon_SOURCE_DIR}/LICENSE.txt" "presentmon.txt")
endif()
```

- [ ] **Step 2: Wire link + compile-def** in the root `CMakeLists.txt`, immediately after the `app`/`exosnap` target is defined (search for where `recorder_core` is linked to `exosnap`). The app target consumes it because `PresentMonEtwSession.cpp` lives in `app/diagnostics`:

```cmake
if(EXOSNAP_WITH_PRESENTMON)
    target_link_libraries(exosnap PRIVATE PresentMon::consumer)
    target_compile_definitions(exosnap PRIVATE EXOSNAP_HAS_PRESENTMON=1)
endif()
```

- [ ] **Step 3: Verify the lib fetches and compiles**

Run: `cmake --build build/windows-x64-debug --target presentmon_consumer`
Expected: configures (clones PresentMon at SHA `2ce1158…`), compiles the `PresentData/*.cpp` set — verified to be exactly `Debug.cpp GpuTrace.cpp MixedRealityTraceConsumer.cpp PresentMonTraceConsumer.cpp TraceConsumer.cpp TraceSession.cpp` (the glob picks these up; `GpuTrace`/`MixedReality` compile but stay unused since we keep `mTrackGPU=false` and pass `mrConsumer=nullptr` — acceptable, no extra runtime cost). The `.cpp` files include `ETW/*.h` headers relative to `PresentData/`, which the `PUBLIC` include dir covers. Produces `presentmon_consumer.lib`. If a source needs an upstream define, the build error names it — add `target_compile_definitions(presentmon_consumer PRIVATE …)` and re-run.

- [ ] **Step 4: Commit**

```bash
git add third_party/CMakeLists.txt CMakeLists.txt
git commit -m "build(diagnostics): vendor PresentMon PresentData ETW consumer (MIT, EXOSNAP_WITH_PRESENTMON)"
```

---

## Task 2: Pure present-mode mapping (`PresentModeMapping`)

**Files:**
- Create: `app/diagnostics/PresentModeMapping.h`, `app/diagnostics/PresentModeMapping.cpp`
- Create: `app/tests/test_present_mode_mapping.cpp`
- Modify: `app/CMakeLists.txt` (new `present_mapping_tests` gtest)

**Interfaces:**
- Produces: `struct RawPresentEvent { int present_mode_code; int sync_interval; bool tearing_flag; double interval_ms; bool valid; };` and `diagnostics::PresentSample MapPresentEvent(const RawPresentEvent&);`. `present_mode_code` uses PresentMon's `PresentMode` integer values (`0=Unknown`, `1=Hardware_Legacy_Flip`/exclusive, `2=Hardware_Legacy_Copy_To_Front_Buffer`/exclusive, `3=Hardware_Independent_Flip`, `4=Composed_Flip`, `5=Composed_Copy_*`, etc.). The mapping collapses these to our 4-value `diagnostics::PresentMode`.
- Consumes: `diagnostics::PresentSample` / `diagnostics::PresentMode` from `PresentProvider.h`.

- [ ] **Step 1: Write the failing test** `app/tests/test_present_mode_mapping.cpp`:

```cpp
#include "diagnostics/PresentModeMapping.h"
#include <gtest/gtest.h>

using namespace exosnap::diagnostics;

TEST(PresentModeMapping, InvalidEventIsUnavailable) {
    RawPresentEvent ev{};
    ev.valid = false;
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_FALSE(s.available);
    EXPECT_EQ(s.mode, PresentMode::Unknown);
}

TEST(PresentModeMapping, ComposedFlipMapsToComposed) {
    RawPresentEvent ev{/*present_mode_code=*/4, /*sync_interval=*/1,
                       /*tearing_flag=*/false, /*interval_ms=*/16.6, /*valid=*/true};
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_TRUE(s.available);
    EXPECT_EQ(s.mode, PresentMode::Composed);
    EXPECT_FALSE(s.tearing);
    EXPECT_DOUBLE_EQ(s.present_interval_ms, 16.6);
}

TEST(PresentModeMapping, IndependentFlipMapsThrough) {
    RawPresentEvent ev{3, 1, false, 8.3, true};   // Hardware_Independent_Flip
    EXPECT_EQ(MapPresentEvent(ev).mode, PresentMode::IndependentFlip);
}

TEST(PresentModeMapping, HardwareComposedIndependentFlipIsIndependentFlip) {
    RawPresentEvent ev{8, 1, false, 8.3, true};   // Hardware_Composed_Independent_Flip
    EXPECT_EQ(MapPresentEvent(ev).mode, PresentMode::IndependentFlip);
}

TEST(PresentModeMapping, LegacyFlipMapsToExclusiveFullscreen) {
    RawPresentEvent ev{1, 0, true, 6.9, true};
    const PresentSample s = MapPresentEvent(ev);
    EXPECT_EQ(s.mode, PresentMode::ExclusiveFullscreen);
    EXPECT_TRUE(s.tearing);   // sync_interval 0 + tearing flag
}

TEST(PresentModeMapping, SyncIntervalZeroImpliesTearing) {
    RawPresentEvent ev{3, /*sync_interval=*/0, /*tearing_flag=*/false, 7.0, true};
    EXPECT_TRUE(MapPresentEvent(ev).tearing);  // interval 0 = uncapped/tearing-capable
}
```

- [ ] **Step 2: Add the gtest target** to `app/CMakeLists.txt` (mirror `present_provider_tests` at line 946 — note: **no** PresentMon dep needed; this is pure):

```cmake
exosnap_add_gtest(
    NAME present_mapping_tests
    TEST_PREFIX present_mapping.
    SOURCES
        tests/test_present_mode_mapping.cpp
        diagnostics/PresentModeMapping.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(present_mapping_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target present_mapping_tests`
Expected: FAIL — `PresentModeMapping.h` not found.

- [ ] **Step 4: Write `PresentModeMapping.h`:**

```cpp
#pragma once

#include "PresentProvider.h"

namespace exosnap::diagnostics {

// A single completed present as observed by the PresentMon consumer, reduced to the
// few fields our diagnosis needs. Kept Win32/ETW-free so the mapping is unit-testable
// without the vendored lib. `present_mode_code` carries PresentMon's PresentMode enum
// integer; `sync_interval` is the DXGI present sync-interval (0 == tearing-capable).
struct RawPresentEvent {
    int present_mode_code = 0;
    int sync_interval = 1;
    bool tearing_flag = false;
    double interval_ms = 0.0;
    bool valid = false;
};

// Collapse a raw present into the diagnostics PresentSample. Invalid input yields a
// neutral Unavailable sample (never a fabricated Composed/zero).
[[nodiscard]] PresentSample MapPresentEvent(const RawPresentEvent& ev);

} // namespace exosnap::diagnostics
```

- [ ] **Step 5: Write `PresentModeMapping.cpp`:**

```cpp
#include "diagnostics/PresentModeMapping.h"

namespace exosnap::diagnostics {

namespace {
// PresentMon PresentData PresentMode enum (VERIFIED, PresentData/PresentMonTraceConsumer.hpp@v1.10.0):
//   1 Hardware_Legacy_Flip, 2 Hardware_Legacy_Copy_To_Front_Buffer       -> exclusive fullscreen
//   3 Hardware_Independent_Flip, 8 Hardware_Composed_Independent_Flip     -> independent flip
//   4 Composed_Flip, 5 Composed_Copy_GPU_GDI, 6 Composed_Copy_CPU_GDI     -> composed
//   (there is no code 7; 0 == Unknown)
PresentMode ClassifyMode(int code) {
    switch (code) {
        case 1:
        case 2:
            return PresentMode::ExclusiveFullscreen;
        case 3:
        case 8:
            return PresentMode::IndependentFlip;
        case 4:
        case 5:
        case 6:
            return PresentMode::Composed;
        default:
            return PresentMode::Unknown;
    }
}
} // namespace

PresentSample MapPresentEvent(const RawPresentEvent& ev) {
    PresentSample s;
    if (!ev.valid) {
        return s; // available == false, mode == Unknown
    }
    s.available = true;
    s.mode = ClassifyMode(ev.present_mode_code);
    // Tearing iff the producer flagged it OR it presented uncapped (sync interval 0).
    s.tearing = ev.tearing_flag || ev.sync_interval == 0;
    s.present_interval_ms = ev.interval_ms;
    return s;
}

} // namespace exosnap::diagnostics
```

- [ ] **Step 6: Run to verify it passes**

Run: `ctest --test-dir build/windows-x64-debug -R "present_mapping\." -j6`
Expected: PASS (5 tests).

- [ ] **Step 7: Commit**

```bash
git add app/diagnostics/PresentModeMapping.h app/diagnostics/PresentModeMapping.cpp app/tests/test_present_mode_mapping.cpp app/CMakeLists.txt
git commit -m "feat(diagnostics): pure PresentMon present-mode mapping + tests"
```

---

## Task 3: Killer-check — Exclusive Fullscreen → Borderless FixAction

**Files:**
- Modify: `app/diagnostics/RecommendationEngine.h` (constructor gains an optional `PresentSample`; new private `checkExclusiveFullscreen`)
- Modify: `app/diagnostics/RecommendationEngine.cpp`
- Modify: `app/tests/test_diagnostics.cpp`

**Interfaces:**
- Consumes: `diagnostics::PresentSample` (Task 2 / `PresentProvider.h`), `FixAction` (`DiagnosticResult.h:37`).
- Produces: a `DiagnosticResult` with `id == "rec.present.exclusive"` carrying an Assisted `FixAction` (`id == "fix.present.borderless"`) when the captured source presents in `ExclusiveFullscreen`.

- [ ] **Step 1: Write the failing test** in `app/tests/test_diagnostics.cpp` (follow the existing `RecommendationEngine` test fixture there):

```cpp
TEST(RecommendationEngineTest, ExclusiveFullscreenRaisesBorderlessFixAction) {
    using namespace exosnap::diagnostics;
    capability::CapabilitySet caps;                 // default/empty is fine
    capability::UserRecorderConfig config;
    PresentSample present;
    present.available = true;
    present.mode = PresentMode::ExclusiveFullscreen;

    RecommendationEngine engine(caps, config, /*monitor_refresh_rate=*/0,
                                /*output_drive_free_bytes=*/0, /*is_profile_supported=*/true,
                                /*output_filesystem_name=*/"NTFS",
                                /*live_snapshot=*/nullptr, /*present=*/&present);
    const DiagnosticChecklist list = engine.Run();

    const auto it = std::find_if(list.items.begin(), list.items.end(),
        [](const DiagnosticResult& r){ return r.id == "rec.present.exclusive"; });
    ASSERT_NE(it, list.items.end());
    ASSERT_TRUE(it->fix_action.has_value());
    EXPECT_EQ(it->fix_action->id, "fix.present.borderless");
    EXPECT_EQ(it->fix_action->safety, FixAction::Safety::Assisted);
}

TEST(RecommendationEngineTest, ComposedPresentRaisesNoExclusiveCheck) {
    using namespace exosnap::diagnostics;
    capability::CapabilitySet caps;
    capability::UserRecorderConfig config;
    PresentSample present;
    present.available = true;
    present.mode = PresentMode::Composed;
    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", nullptr, &present);
    const DiagnosticChecklist list = engine.Run();
    EXPECT_TRUE(std::none_of(list.items.begin(), list.items.end(),
        [](const DiagnosticResult& r){ return r.id == "rec.present.exclusive"; }));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target diagnostics_tests`
Expected: FAIL — `RecommendationEngine` has no 8-arg constructor / no `present` param.

- [ ] **Step 3: Extend the constructor** in `RecommendationEngine.h` — add a trailing `const PresentSample* present = nullptr` parameter and a `std::optional<PresentSample> present_;` member plus `void checkExclusiveFullscreen(DiagnosticChecklist&) const;`. In `RecommendationEngine.cpp` capture it (mirror the existing `live_snapshot` handling at lines 36–53):

```cpp
// in the constructor body, after the live_snapshot block:
if (present != nullptr && present->available) {
    present_ = *present;
}
```

- [ ] **Step 4: Implement `checkExclusiveFullscreen`** and call it from `Run()` (alongside `checkRefreshRateMismatch`):

```cpp
void RecommendationEngine::checkExclusiveFullscreen(DiagnosticChecklist& checklist) const {
    if (!present_.has_value() || present_->mode != PresentMode::ExclusiveFullscreen) {
        return;
    }
    DiagnosticResult r;
    r.id = "rec.present.exclusive";
    r.severity = Severity::Warning;
    r.summary = "Captured source is in exclusive fullscreen";
    r.detail =
        "The source presents in legacy exclusive fullscreen. Desktop/window capture often records "
        "a black frame in this mode. Switch the game to borderless (windowed-fullscreen) so the "
        "compositor can present it for capture.";
    r.current_value = "Present mode: Exclusive fullscreen";
    r.recommendation = "Set the game to Borderless / Windowed Fullscreen.";

    FixAction fa;
    fa.id = "fix.present.borderless";
    fa.label = "How to switch to borderless";
    fa.safety = FixAction::Safety::Assisted;   // app cannot flip a foreign game's mode
    fa.reversible = true;
    fa.changes_summary =
        "Opens guidance for switching the captured game to borderless fullscreen (the app cannot "
        "change another application's display mode for you).";
    r.fix_action = fa;
    checklist.items.push_back(r);
}
```

(Match `DiagnosticResult`'s real field names — confirm `severity`, `current_value`, `fix_action` against `DiagnosticResult.h` before writing; the snippet uses the names seen in `RecommendationEngine.cpp:133-141`.)

- [ ] **Step 5: Run to verify it passes**

Run: `ctest --test-dir build/windows-x64-debug -R "diagnostics\." -j6`
Expected: PASS including the two new cases.

- [ ] **Step 6: Commit**

```bash
git add app/diagnostics/RecommendationEngine.h app/diagnostics/RecommendationEngine.cpp app/tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): exclusive-fullscreen killer check + borderless FixAction"
```

---

## Task 4: Present-mode correlation enrichment

**Files:**
- Modify: `app/diagnostics/RecommendationEngine.cpp` (`checkRefreshRateMismatch`, lines 68–143)
- Modify: `app/tests/test_diagnostics.cpp`

**Interfaces:**
- Consumes: `present_` (Task 3), `live_present_jitter_ms_` / `live_coalesce_ratio_` (existing).
- Produces: when live judder fires **and** `present_->mode` is `IndependentFlip`/`ExclusiveFullscreen`, the existing VRR/CFR `DiagnosticResult` detail names the present mode as attribution (no new id; enriches the existing `rec.fps.*` result).

> **Real names (verified against `RecommendationEngine.cpp`):** the refresh/judder result id is **`rec.001`** (NOT `rec.fps`). The engine method is **`Generate()`** (not `Run()`); the checklist vector is **`results`** (not `items`). The detail is built into a local `std::string detail` then stored in `r.detail` via `MakeResult(...)` at the end of the live-judder arm; the if/else closes, then a `FixAction fa` block runs before `checklist.results.push_back(std::move(r))`. Confirm against the file before editing.

- [ ] **Step 1: Write the failing test** in `test_diagnostics.cpp`: build a snapshot with `present_cadence_availability == Available`, `source_present_jitter_ms` above `kJitterMs (4.0)`, CFR true, plus a `PresentSample{IndependentFlip, available}`. Assert the `rec.001` result's `detail` contains the substring `"independent flip"`.

```cpp
TEST(RecommendationEngineTest, JudderDetailNamesPresentModeAttribution) {
    using namespace exosnap::diagnostics;
    recorder_core::RecordingDiagnosticsSnapshot snap;
    snap.valid = true;
    snap.video_encoder.cfr = true;
    snap.capture.present_cadence_availability = recorder_core::MetricAvailability::Available;
    snap.capture.source_present_jitter_ms = 6.0;     // > kJitterMs
    snap.capture.source_coalesce_ratio = 2.0;        // > kCoalesceRatio
    capability::CapabilitySet caps; capability::UserRecorderConfig config;
    config.frame_rate_num = 60; config.frame_rate_den = 1;
    PresentSample present; present.available = true; present.mode = PresentMode::IndependentFlip;

    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", &snap, &present);
    const DiagnosticChecklist list = engine.Generate();
    const auto it = std::find_if(list.results.begin(), list.results.end(),
        [](const DiagnosticResult& r){ return r.id == "rec.001"; });
    ASSERT_NE(it, list.results.end());
    EXPECT_NE(it->detail.find("independent flip"), std::string::npos);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target diagnostics_tests` then `ctest --test-dir build/windows-x64-debug -R "diagnostics\.RecommendationEngineTest.JudderDetail" -j6`
Expected: FAIL (`detail` lacks the attribution string).

- [ ] **Step 3: Append attribution** after the `if (live_judder) { … } else { … }` block builds `r` (i.e. after the `else` closes, ~line 137) and **before** the `FixAction fa;` block (~line 139), so it enriches `r.detail` in both arms:

```cpp
// Present-mode attribution (PresentMon, ADR 0033): when available, name *how* the source
// presents so the diagnosis reads as a root cause, not just a number.
if (present_.has_value()) {
    switch (present_->mode) {
        case PresentMode::IndependentFlip:
            r.detail += " The source is presenting via independent flip (variable-rate "
                        "flip-model), which the fixed CFR cadence cannot phase-match.";
            break;
        case PresentMode::ExclusiveFullscreen:
            r.detail += " The source is in exclusive fullscreen; its present cadence is "
                        "independent of the desktop refresh.";
            break;
        default:
            break;
    }
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `ctest --test-dir build/windows-x64-debug -R "diagnostics\." -j6`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/diagnostics/RecommendationEngine.cpp app/tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): present-mode attribution in VRR/CFR judder correlation"
```

---

## Task 5: ETW session wrapper (`PresentMonEtwSession`) — isolated I/O

**Files:**
- Create: `app/diagnostics/PresentMonEtwSession.h`, `app/diagnostics/PresentMonEtwSession.cpp`
- Modify: `CMakeLists.txt` (already links `presentmon_consumer` to `exosnap` from Task 1 — no change if the source is in the `exosnap` target's sources; add the new `.cpp` to `app/CMakeLists.txt` exosnap sources)

**Interfaces:**
- Produces: `class PresentMonEtwSession` with `bool Start();` (opens session + spawns consumer thread; returns false on failure — e.g. `ERROR_ACCESS_DENIED` when not elevated), `void Stop();`, `PresentSample Latest() const;` (mutex-guarded snapshot), `bool IsOpen() const;`. Optional `void SetTargetProcessId(unsigned long pid);` to scope reporting to the captured window's process (0 = dominant non-composed presenter).
- Consumes: `presentmon_consumer` (`PresentData/PresentMonTraceConsumer.hpp`), `MapPresentEvent` (Task 2).

> **This task is ETW I/O — not headless-verifiable.** It is `#ifdef EXOSNAP_HAS_PRESENTMON`-guarded; the whole file compiles to an empty TU otherwise. No unit test; verified by compile/link (Step 3) and the dev-machine game pass (Task 11). The code below uses the **verified** v1.10.0 `PresentData` API (see Global Constraints) — `TraceSession::Start` does the session open + provider enable + `OpenTrace`; we only run `ProcessTrace` on a worker and drain `DequeuePresentEvents`. No hand-rolled `StartTrace`/`EnableTraceEx2`.

- [ ] **Step 1: Write `PresentMonEtwSession.h`:**

```cpp
#pragma once

#include "PresentProvider.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace exosnap::diagnostics {

// Owns a real-time ETW present-trace session and a consumer worker that feeds the
// vendored PresentMon PresentData decoder. Latest() returns the most recent mapped
// present (Unavailable until one is seen). Requires elevation; Start() returns false
// (graceful) when the session cannot be opened.
class PresentMonEtwSession {
  public:
    PresentMonEtwSession();
    ~PresentMonEtwSession();
    PresentMonEtwSession(const PresentMonEtwSession&) = delete;
    PresentMonEtwSession& operator=(const PresentMonEtwSession&) = delete;

    [[nodiscard]] bool Start();
    void Stop();
    [[nodiscard]] bool IsOpen() const { return open_.load(std::memory_order_acquire); }
    [[nodiscard]] PresentSample Latest() const;
    void SetTargetProcessId(unsigned long pid) { target_pid_.store(pid, std::memory_order_relaxed); }

  private:
    void ConsumeLoop();                    // runs ProcessTrace (blocking) on worker_
    void OnPresentCompleted(const RawPresentEvent&);  // called from the decoder

    std::atomic<bool> open_{false};
    std::atomic<unsigned long> target_pid_{0};
    std::thread worker_;
    mutable std::mutex sample_mutex_;
    mutable PresentSample latest_;          // guarded by sample_mutex_
    mutable uint64_t last_present_qpc_ = 0;  // reader-side drain state (Latest())
    mutable int64_t qpc_freq_ = 0;
    void* impl_ = nullptr;                   // opaque SessionImpl* (PMTraceConsumer + TraceSession)
};

} // namespace exosnap::diagnostics
```

- [ ] **Step 2: Write `PresentMonEtwSession.cpp`** — guarded; real ETW calls. Outside the guard, every method is a graceful no-op so non-PresentMon builds link:

```cpp
#include "diagnostics/PresentMonEtwSession.h"
#include "diagnostics/PresentModeMapping.h"

#ifdef EXOSNAP_HAS_PRESENTMON

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <vector>
// Vendored PresentMon PresentData (pinned v1.10.0):
#include "TraceSession.hpp"
#include "PresentMonTraceConsumer.hpp"

namespace exosnap::diagnostics {

namespace {
constexpr wchar_t kSessionName[] = L"ExoSnapPresentMon";

// Build a RawPresentEvent from a completed PresentEvent + the prior present's QPC and
// the session's QPC frequency. interval_ms is the inter-present gap; 0 for the first.
RawPresentEvent ToRaw(const PresentEvent& pe, uint64_t prev_qpc, int64_t qpc_freq) {
    RawPresentEvent ev;
    ev.valid = true;
    ev.present_mode_code = static_cast<int>(pe.PresentMode);
    ev.sync_interval = pe.SyncInterval;
    ev.tearing_flag = pe.SupportsTearing;
    ev.interval_ms = (prev_qpc != 0 && pe.PresentStartTime > prev_qpc && qpc_freq != 0)
        ? static_cast<double>(pe.PresentStartTime - prev_qpc) * 1000.0 / static_cast<double>(qpc_freq)
        : 0.0;
    return ev;
}

struct SessionImpl {
    PMTraceConsumer pm;        // default ctor: mTrackDisplay=true, mTrackGPU/Input=false
    TraceSession session;      // PresentMon's session helper (open + enable + OpenTrace)
};
} // namespace

PresentMonEtwSession::PresentMonEtwSession() = default;

bool PresentMonEtwSession::Start() {
    if (open_.load(std::memory_order_acquire)) return true;
    auto* s = new SessionImpl();
    // A stale session from a previous crashed instance would block Start; clear it first.
    TraceSession::StopNamedSession(kSessionName);
    // realtime: etlPath=nullptr; no WinMR: mrConsumer=nullptr.
    const ULONG st = s->session.Start(&s->pm, nullptr, nullptr, kSessionName);
    if (st != ERROR_SUCCESS) {   // ERROR_ACCESS_DENIED when not elevated -> graceful degrade
        delete s;
        return false;
    }
    impl_ = s;
    open_.store(true, std::memory_order_release);
    worker_ = std::thread(&PresentMonEtwSession::ConsumeLoop, this);
    return true;
}

void PresentMonEtwSession::ConsumeLoop() {
    auto* s = static_cast<SessionImpl*>(impl_);
    // ProcessTrace blocks, routing events into s->pm (TraceSession set up the callback),
    // until Stop() calls TraceSession::Stop() -> CloseTrace.
    ::ProcessTrace(&s->session.mTraceHandle, 1, nullptr, nullptr);
}

void PresentMonEtwSession::OnPresentCompleted(const RawPresentEvent& ev) {
    const PresentSample mapped = MapPresentEvent(ev);
    std::lock_guard lk(sample_mutex_);
    latest_ = mapped;
}

PresentSample PresentMonEtwSession::Latest() const {
    auto* s = static_cast<SessionImpl*>(impl_);
    if (s != nullptr) {
        // Drain on the reader side (DequeuePresentEvents is thread-safe). Keep the most
        // recent present that matches the target PID filter (0 = any non-composed-dominant).
        std::vector<std::shared_ptr<PresentEvent>> presents;
        s->pm.DequeuePresentEvents(presents);
        const unsigned long want_pid = target_pid_.load(std::memory_order_relaxed);
        for (const auto& p : presents) {
            if (want_pid != 0 && p->ProcessId != want_pid) continue;
            const RawPresentEvent raw = ToRaw(*p, last_present_qpc_, qpc_freq_);
            last_present_qpc_ = p->PresentStartTime;
            const PresentSample mapped = MapPresentEvent(raw);
            std::lock_guard lk(sample_mutex_);
            latest_ = mapped;
        }
        qpc_freq_ = s->session.mTimestampFrequency.QuadPart;
    }
    std::lock_guard lk(sample_mutex_);
    return latest_;
}

void PresentMonEtwSession::Stop() {
    if (!open_.exchange(false)) return;
    auto* s = static_cast<SessionImpl*>(impl_);
    s->session.Stop();                    // CloseTrace -> unblocks ProcessTrace
    if (worker_.joinable()) worker_.join();
    delete s;
    impl_ = nullptr;
}

PresentMonEtwSession::~PresentMonEtwSession() { Stop(); }

} // namespace exosnap::diagnostics

#else  // !EXOSNAP_HAS_PRESENTMON — graceful no-op build

namespace exosnap::diagnostics {
PresentMonEtwSession::PresentMonEtwSession() = default;
PresentMonEtwSession::~PresentMonEtwSession() = default;
bool PresentMonEtwSession::Start() { return false; }
void PresentMonEtwSession::Stop() {}
PresentSample PresentMonEtwSession::Latest() const { return PresentSample{}; }
} // namespace exosnap::diagnostics

#endif
```

> **Note on `Latest()` draining + threading:** `DequeuePresentEvents` is thread-safe, so draining on the reader (UI ~1–4 Hz) side keeps the worker thread doing only `ProcessTrace`. `Latest()` is therefore non-`const` in effect — make `last_present_qpc_`/`qpc_freq_`/`latest_` mutable members (add `uint64_t last_present_qpc_=0; int64_t qpc_freq_=0;` mutable to the header) or move the drain into `ConsumeLoop` with a periodic timer. The header in Step 1 already marks `latest_` mutable via the `mutable std::mutex`; add the two mutable QPC members alongside it. Pick the reader-drain shown here (simplest; no second thread).

- [ ] **Step 3: Add the source** to the `exosnap` target in `app/CMakeLists.txt` (the main app sources list) and verify it links in both flag states:

Run (default, `EXOSNAP_WITH_PRESENTMON=ON`): `cmake --build build/windows-x64-debug --target exosnap`
Expected: links; the `EnableTraceEx2`/present-drain `...` sections must be filled against the pinned `PMTraceConsumer` API — if the type/method names differ in `v1.10.0`, the compile error names them; adapt and re-run.

Run (off): reconfigure a throwaway build dir with `-DEXOSNAP_WITH_PRESENTMON=OFF` and build `exosnap` — expected: links via the `#else` no-op TU. (Document the command in the commit; do not disturb the primary build dir.)

- [ ] **Step 4: Commit**

```bash
git add app/diagnostics/PresentMonEtwSession.h app/diagnostics/PresentMonEtwSession.cpp app/CMakeLists.txt
git commit -m "feat(diagnostics): in-process PresentMon ETW session (guarded, graceful-degrade)"
```

---

## Task 6: Wire `PresentMonProvider` to the session

**Files:**
- Modify: `app/diagnostics/PresentMonProvider.h`, `app/diagnostics/PresentMonProvider.cpp`
- Modify: `app/tests/test_present_provider.cpp`
- Modify: `app/CMakeLists.txt` (`present_provider_tests` must compile `PresentMonEtwSession.cpp` + `PresentModeMapping.cpp`; link `PresentMon::consumer` only when the option is on)

**Interfaces:**
- Consumes: `PresentMonEtwSession` (Task 5).
- Produces: `PresentMonProvider::Sample()` returns the session's `Latest()` when the gate is open and the session is running; `IsAvailable()` is `opt_in_ && elevation_.IsElevated() && session_.IsOpen()`. `SetOptIn(bool)` starts/stops the session to match the gate.

- [ ] **Step 1: Write the failing test** — extend `test_present_provider.cpp`: when `EXOSNAP_HAS_PRESENTMON` is **undefined** (the test target does not define it), the session never opens, so even elevated+opt-in yields `IsAvailable() == false` and `Sample().available == false`:

```cpp
TEST(PresentProviderTest, UnavailableWhenEtwConsumerNotCompiledIn) {
    const StubElevationProvider elevated(true);
    PresentMonProvider provider(elevated, /*opt_in=*/true);
    // No EXOSNAP_HAS_PRESENTMON for this test target -> session can't open.
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_FALSE(provider.Sample().available);
}
```

(The existing `AvailableOnlyWhenElevatedAndOptIn` test now asserts the **pre-session** gate; update it to expect `IsAvailable()==false` even for elevated+opt-in when the consumer isn't compiled in, OR split the gate into a testable `GateOpen()` predicate. Use the latter: add `[[nodiscard]] bool GateOpen() const` returning `opt_in_ && elevation_.IsElevated()` and keep the old test on `GateOpen()`.)

- [ ] **Step 2: Update `present_provider_tests`** in `app/CMakeLists.txt:946` — add the two new sources; **do not** define `EXOSNAP_HAS_PRESENTMON` here (so the no-op session is used and the test stays Qt-only, no ETW dep):

```cmake
exosnap_add_gtest(
    NAME present_provider_tests
    TEST_PREFIX present.
    SOURCES
        tests/test_present_provider.cpp
        diagnostics/PresentMonProvider.cpp
        diagnostics/PresentMonEtwSession.cpp
        diagnostics/PresentModeMapping.cpp
    LIBRARIES Qt6::Core
)
target_include_directories(present_provider_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target present_provider_tests`
Expected: FAIL — `PresentMonProvider` has no `GateOpen()` / does not own a session.

- [ ] **Step 4: Update `PresentMonProvider.h`** — add a `PresentMonEtwSession session_;` member, `GateOpen()`, and adjust the doc-comment to drop "always Unavailable":

```cpp
#pragma once

#include "ElevationProvider.h"
#include "PresentMonEtwSession.h"
#include "PresentProvider.h"

namespace exosnap::diagnostics {

class PresentMonProvider final : public IPresentProvider {
  public:
    PresentMonProvider(const IElevationProvider& elevation, bool opt_in);

    [[nodiscard]] PresentSample Sample() const override;
    [[nodiscard]] bool IsAvailable() const override;

    // opt_in AND elevation (pre-session gate). Drives whether SetOptIn starts the session.
    [[nodiscard]] bool GateOpen() const;

    // Updates the opt-in and starts/stops the ETW session to match the gate.
    void SetOptIn(bool opt_in);

  private:
    const IElevationProvider& elevation_;
    bool opt_in_;
    mutable PresentMonEtwSession session_;
};

} // namespace exosnap::diagnostics
```

- [ ] **Step 5: Update `PresentMonProvider.cpp`:**

```cpp
#include "diagnostics/PresentMonProvider.h"

namespace exosnap::diagnostics {

PresentMonProvider::PresentMonProvider(const IElevationProvider& elevation, bool opt_in)
    : elevation_(elevation), opt_in_(opt_in) {
    if (GateOpen()) {
        session_.Start();   // graceful: returns false when ETW can't open
    }
}

bool PresentMonProvider::GateOpen() const {
    return opt_in_ && elevation_.IsElevated();
}

bool PresentMonProvider::IsAvailable() const {
    return GateOpen() && session_.IsOpen();
}

PresentSample PresentMonProvider::Sample() const {
    if (!IsAvailable()) {
        return PresentSample{};   // Unavailable — never fabricate
    }
    return session_.Latest();
}

void PresentMonProvider::SetOptIn(bool opt_in) {
    opt_in_ = opt_in;
    if (GateOpen()) {
        session_.Start();
    } else {
        session_.Stop();
    }
}

} // namespace exosnap::diagnostics
```

- [ ] **Step 6: Run to verify it passes**

Run: `ctest --test-dir build/windows-x64-debug -R "present\." -j6`
Expected: PASS (gating tests; session never opens without the dep).

- [ ] **Step 7: Commit**

```bash
git add app/diagnostics/PresentMonProvider.h app/diagnostics/PresentMonProvider.cpp app/tests/test_present_provider.cpp app/CMakeLists.txt
git commit -m "feat(diagnostics): wire PresentMonProvider to the ETW session (gate + lifecycle)"
```

---

## Task 7: Snapshot bridge — inject present-mode into the page

**Files:**
- Modify: `app/pages/DiagnosticsPage.h`, `app/pages/DiagnosticsPage.cpp` (`setData`/`setDiagnosticData` ~line 344; the live-snapshot fill path)
- Modify: `app/MainWindow.cpp` (own the provider; inject)
- Modify: `app/tests/test_diagnostics_page.cpp`
- Modify: `app/CMakeLists.txt` (`diagnostics_page_tests:1244` already compiles `DiagnosticsPage.cpp`; add `PresentMonProvider.cpp`, `PresentMonEtwSession.cpp`, `PresentModeMapping.cpp` to its SOURCES; no ETW define → no dep)

**Interfaces:**
- Produces: `DiagnosticsPage::setPresentProvider(diagnostics::IPresentProvider* provider)` (borrowed, nullable). When set, the page overlays `Sample()` onto the snapshot's `capture.source_present_mode` / `source_tearing` / `present_mode_availability` before constructing the `RecommendationEngine` and rendering `LivePipelinePanel`. Null → fields stay `Unavailable`.
- Consumes: `IPresentProvider` (`PresentProvider.h`), the snapshot fields (`pipeline_diagnostics.h:114-116`).

- [ ] **Step 1: Write the failing test** in `test_diagnostics_page.cpp` — inject a stub provider returning `{IndependentFlip, tearing=true, available}`; after `setData`, assert the `LivePipelinePanel` value for `liveCapturePresentMode` reads `"Independent flip · tearing"` (the existing panel formats it — `LivePipelinePanel.cpp:326-333`):

```cpp
class StubPresent final : public exosnap::diagnostics::IPresentProvider {
  public:
    exosnap::diagnostics::PresentSample Sample() const override {
        exosnap::diagnostics::PresentSample s;
        s.available = true; s.mode = exosnap::diagnostics::PresentMode::IndependentFlip;
        s.tearing = true; return s;
    }
    bool IsAvailable() const override { return true; }
};
// ... in the test: page.setPresentProvider(&stub); page.setData(...);
// find the QLabel named "liveCapturePresentMode" and EXPECT its text contains "Independent flip".
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target diagnostics_page_tests`
Expected: FAIL — `setPresentProvider` does not exist.

- [ ] **Step 3: Add `setPresentProvider`** to `DiagnosticsPage.h` (`diagnostics::IPresentProvider* present_provider_ = nullptr;` member + setter) and, in `setDiagnosticData` after the disk/fs provider block (~line 360), overlay onto the snapshot the page passes downstream:

```cpp
// PresentMon present-mode overlay (ADR 0033). App-layer: the engine leaves these
// Unavailable; we fill them from the injected provider (mirrors the disk/fs providers).
if (present_provider_ != nullptr) {
    const diagnostics::PresentSample ps = present_provider_->Sample();
    if (ps.available) {
        live_snapshot_.capture.source_present_mode = ToSnapshotMode(ps.mode);
        live_snapshot_.capture.source_tearing = ps.tearing;
        live_snapshot_.capture.present_mode_availability =
            recorder_core::MetricAvailability::Available;
    }
}
```

Add a small `ToSnapshotMode` translator (the diagnostics `PresentMode` enum and the recorder_core `PresentMode` enum are distinct types with identical members — `pipeline_diagnostics.h:68` vs `PresentProvider.h:9`):

```cpp
static recorder_core::PresentMode ToSnapshotMode(diagnostics::PresentMode m) {
    switch (m) {
        case diagnostics::PresentMode::Composed: return recorder_core::PresentMode::Composed;
        case diagnostics::PresentMode::IndependentFlip: return recorder_core::PresentMode::IndependentFlip;
        case diagnostics::PresentMode::ExclusiveFullscreen: return recorder_core::PresentMode::ExclusiveFullscreen;
        default: return recorder_core::PresentMode::Unknown;
    }
}
```

(Confirm the page actually holds a `live_snapshot_` member that flows to `LivePipelinePanel` + `RecommendationEngine`; if it uses a local, route the overlay there. Also pass `present_provider_->Sample()` to the `RecommendationEngine` 8th arg for Tasks 3–4.)

- [ ] **Step 4: Inject from MainWindow** — `app/MainWindow.cpp` already owns `elevation_provider_` and `persisted_settings_`. Construct a `diagnostics::PresentMonProvider present_provider_{elevation_provider_, persisted_settings_.present_diagnostics_optin};` member, call `diagnostics_page_->setPresentProvider(&present_provider_);` during page wiring, and in `onPresentDiagnosticsOptInToggled` (`MainWindow.cpp:3423`) add `present_provider_.SetOptIn(enabled);` so the session actually starts/stops on toggle (keep the existing persist + advisory).

- [ ] **Step 5: Update `diagnostics_page_tests` SOURCES** in `app/CMakeLists.txt:1244` to add `diagnostics/PresentMonProvider.cpp`, `diagnostics/PresentMonEtwSession.cpp`, `diagnostics/PresentModeMapping.cpp` (no `EXOSNAP_HAS_PRESENTMON` define → no ETW dep).

- [ ] **Step 6: Run to verify it passes**

Run: `ctest --test-dir build/windows-x64-debug -R "diagnostic\." -j6`
Expected: PASS — panel shows the injected mode.

- [ ] **Step 7: Commit**

```bash
git add app/pages/DiagnosticsPage.h app/pages/DiagnosticsPage.cpp app/MainWindow.cpp app/tests/test_diagnostics_page.cpp app/CMakeLists.txt
git commit -m "feat(diagnostics): bridge PresentMon present-mode into snapshot + live panel"
```

---

## Task 8: DPC/ISR latency — pure threshold check

**Files:**
- Modify: `app/diagnostics/RecommendationEngine.h/.cpp` (new pure `checkDpcLatency`)
- Modify: `app/tests/test_diagnostics.cpp`

**Interfaces:**
- Produces: `struct DpcLatencyReading { double max_latency_us; double avg_latency_us; std::string worst_driver; bool available; };` (declared in `RecommendationEngine.h`); when `available && max_latency_us > kDpcThresholdUs (1000.0)`, a `DiagnosticResult id == "rec.dpc.latency"` with an **External** `FixAction id == "fix.dpc.driver"` naming `worst_driver`.
- Consumes: `FixAction` (`DiagnosticResult.h`).

- [ ] **Step 1: Write the failing test** in `test_diagnostics.cpp`:

```cpp
TEST(RecommendationEngineTest, HighDpcLatencyNamesDriverExternalFix) {
    using namespace exosnap::diagnostics;
    capability::CapabilitySet caps; capability::UserRecorderConfig config;
    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", nullptr, nullptr);
    DpcLatencyReading dpc{/*max*/2500.0, /*avg*/180.0, "nvlddmkm.sys", /*available*/true};
    engine.SetDpcLatency(dpc);
    const DiagnosticChecklist list = engine.Generate();
    const auto it = std::find_if(list.results.begin(), list.results.end(),
        [](const DiagnosticResult& r){ return r.id == "rec.dpc.latency"; });
    ASSERT_NE(it, list.results.end());
    EXPECT_NE(it->detail.find("nvlddmkm.sys"), std::string::npos);
    ASSERT_TRUE(it->fix_action.has_value());
    EXPECT_EQ(it->fix_action->safety, FixAction::Safety::External);
}

TEST(RecommendationEngineTest, LowDpcLatencyRaisesNothing) {
    using namespace exosnap::diagnostics;
    capability::CapabilitySet caps; capability::UserRecorderConfig config;
    RecommendationEngine engine(caps, config, 0, 0, true, "NTFS", nullptr, nullptr);
    engine.SetDpcLatency({200.0, 60.0, "", true});
    const DiagnosticChecklist list = engine.Generate();
    EXPECT_TRUE(std::none_of(list.results.begin(), list.results.end(),
        [](const DiagnosticResult& r){ return r.id == "rec.dpc.latency"; }));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build/windows-x64-debug --target diagnostics_tests`
Expected: FAIL — no `DpcLatencyReading` / `SetDpcLatency`.

- [ ] **Step 3: Add `DpcLatencyReading` + `SetDpcLatency` + `checkDpcLatency`** to `RecommendationEngine.h/.cpp`. Store `std::optional<DpcLatencyReading> dpc_;`, call `checkDpcLatency` from `Generate()`. Use the existing `MakeResult(id, group, severity, title, summary, detail, current_value, recommendation)` helper + `DiagnosticSeverity::Notice` + `checklist.has_notice` + `checklist.results.push_back` (match the file's pattern; do NOT hand-build the result or invent a `Severity::Warning` — the enum is `DiagnosticSeverity{Pass,Notice,Blocker}`):

```cpp
void RecommendationEngine::checkDpcLatency(DiagnosticChecklist& checklist) const {
    constexpr double kDpcThresholdUs = 1000.0;   // 1 ms sustained DPC = audible/stutter risk
    if (!dpc_.has_value() || !dpc_->available || dpc_->max_latency_us <= kDpcThresholdUs) {
        return;
    }
    const std::string driver = dpc_->worst_driver.empty() ? "an unidentified kernel driver"
                                                           : dpc_->worst_driver;
    const std::string max_str = std::to_string(static_cast<long>(dpc_->max_latency_us));
    DiagnosticResult r = MakeResult(
        "rec.dpc.latency", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        "High kernel DPC/ISR latency detected",
        "Kernel driver latency can cause recording stutter even when the game feels smooth.",
        "Peak DPC latency reached " + max_str + " us, attributed to " + driver +
            ". High DPC latency causes recording stutter/audio crackle even when the game itself "
            "feels smooth.",
        "Max DPC: " + max_str + " us",
        "Update or roll back " + driver + " (GPU/audio/network/chipset driver).");
    FixAction fa;
    fa.id = "fix.dpc.driver";
    fa.label = "Driver latency guidance";
    fa.safety = FixAction::Safety::External;   // app cannot change kernel drivers
    fa.reversible = false;
    fa.changes_summary = "Shows which driver to update/roll back; the app cannot change it for you.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `ctest --test-dir build/windows-x64-debug -R "diagnostics\." -j6`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/diagnostics/RecommendationEngine.h app/diagnostics/RecommendationEngine.cpp app/tests/test_diagnostics.cpp
git commit -m "feat(diagnostics): minimal DPC/ISR latency threshold check + External fix"
```

---

## Task 9: DPC/ISR kernel-trace provider — isolated I/O

**Files:**
- Create: `app/diagnostics/DpcLatencyProvider.h`, `app/diagnostics/DpcLatencyProvider.cpp`
- Modify: `app/MainWindow.cpp` (own + feed `RecommendationEngine::SetDpcLatency` via the page), `app/pages/DiagnosticsPage.*` (`setDpcReading(DpcLatencyReading)` setter)
- Modify: `app/CMakeLists.txt` (add source to `exosnap` + `diagnostics_page_tests`)

**Interfaces:**
- Produces: `class DpcLatencyProvider { bool Start(); void Stop(); DpcLatencyReading Read() const; bool IsOpen() const; };` — opens a kernel system-trace real-time session with `EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT | EVENT_TRACE_FLAG_IMAGE_LOAD`, tracks max/avg DPC duration, resolves the worst routine address → module name via the image-load events. Guarded by `EXOSNAP_HAS_PRESENTMON` (shares the ETW build gate) and graceful when not elevated.

> ETW I/O — not headless-verifiable. `#ifdef`-guarded with a no-op `#else` like Task 5. Driver attribution is **best-effort**: if the routine address can't be resolved to a loaded image, `worst_driver` stays empty and the check (Task 8) degrades to "an unidentified kernel driver". Document this limitation in the ADR.

- [ ] **Step 1: Write `DpcLatencyProvider.h`** mirroring `PresentMonEtwSession`'s lifecycle shape (`Start`/`Stop`/`Read`/`IsOpen`, worker thread running `ProcessTrace`, mutex-guarded accumulator producing a `DpcLatencyReading`).

- [ ] **Step 2: Write `DpcLatencyProvider.cpp`** guarded by `EXOSNAP_HAS_PRESENTMON`. Open a system logger session (`EVENT_TRACE_SYSTEM_LOGGER_MODE` on Win8.1+, `SystemTraceControlGuid`), enable the DPC/ISR/image-load flags via `EVENT_TRACE_PROPERTIES.EnableFlags`, decode `PerfInfo` DPC/ISR events (compute duration from the event's `InitialTime`/timestamps), accumulate max/avg, and map the worst DPC routine address to a module using the `Image_Load` event ranges. `#else` branch: all methods no-op, `Read()` returns `{0,0,"",false}`.

- [ ] **Step 3: Wire into MainWindow + page** — own a `DpcLatencyProvider dpc_provider_;`, start/stop it on the same opt-in toggle as the present provider, and on diagnostics refresh call `diagnostics_page_->setDpcReading(dpc_provider_.Read());` which forwards into `RecommendationEngine::SetDpcLatency`.

- [ ] **Step 4: Verify compile/link** in both flag states (as Task 5 Step 3). No unit test (I/O).

Run: `cmake --build build/windows-x64-debug --target exosnap`
Expected: links. Adapt PerfInfo decoding to the resolved struct layouts if the compiler flags missing fields.

- [ ] **Step 5: Commit**

```bash
git add app/diagnostics/DpcLatencyProvider.h app/diagnostics/DpcLatencyProvider.cpp app/MainWindow.cpp app/pages/DiagnosticsPage.h app/pages/DiagnosticsPage.cpp app/CMakeLists.txt
git commit -m "feat(diagnostics): kernel DPC/ISR latency provider (guarded, best-effort driver attribution)"
```

> **Schema note:** this slice adds **no** persisted field — the existing `present_diagnostics_optin` (schema 17) gates the whole ETW bundle (present + DPC). Relabel the toggle copy in `ConfigPage.cpp:2470` to "Present, tearing & latency diagnostics" (UI string only, no persistence change → **no schema bump**, no reset).

---

## Task 10: Full-suite verification + render check

**Files:** none (verification only)

- [ ] **Step 1: Full build (no `--target`)** so every Page/Widget test target compiles with the new sources:

Run: `cmake --build build/windows-x64-debug`
Expected: builds clean.

- [ ] **Step 2: Full ctest**

Run: `ctest --test-dir build/windows-x64-debug -j6`
Expected: all pass (note the known `-j` flakes for HistoryStore/PresetStore — re-run any flaky single test serially to confirm).

- [ ] **Step 3: Render-verify the diagnostics states** (Qt bin on PATH). Capture the Diagnostics page in the unavailable state (non-elevated: present-mode/latency rows show em-dash + "Needs administrator") and, via the stub-injected available state if a visual-test fixture exists, the populated state:

Run: `Start-Process -Wait exosnap.exe --visual-test diagnostics --visual-test-screenshot`
Expected: screenshot shows the present-mode row rendering the em-dash sentinel (unavailable) — judge on rendered pixels, not QSS values.

- [ ] **Step 4: Commit** any test-target fixups surfaced by the full build.

---

## Task 11: ADR + dev-machine live verification + docs

**Files:**
- Modify: `docs/decisions/0033-diagnostics-engine-and-fixaction.md`

- [ ] **Step 1: Document the delivered subset/pin** in ADR 0033 (a "Delivered (Slice 1)" note): the resolved PresentMon tag (`v1.10.0` or whatever Task 1 Step 3 settled on), the exact `PresentData/` source set the glob picked up, the DPC-minimal scope (max/avg + best-effort driver, External fix), the reused `present_diagnostics_optin` gate, and the no-schema-bump decision. **Leave Status = Proposed** — the Proposed→Accepted flip happens in the separate 0.8.0 release-mechanics slice, after the CFR-pacing slice (D).

- [ ] **Step 2: Dev-machine live verification (honest boundary).** On the elevated dev machine with a real game: (a) toggle the opt-in on → confirm the UAC relaunch path leaves the session open; (b) run a borderless game → present-mode reads `Independent flip`/`Composed`; (c) run an exclusive-fullscreen game → present-mode reads `Exclusive fullscreen` **and** the `rec.present.exclusive` killer FixAction appears; (d) confirm the DPC row shows a plausible max latency + driver name. Record the outcomes (pass/limitation) in the commit message and an ADR "Live-verified" note. Do **not** commit any recording artifact.

- [ ] **Step 3: Commit**

```bash
git add docs/decisions/0033-diagnostics-engine-and-fixaction.md
git commit -m "docs(adr-0033): record delivered PresentMon subset/pin + DPC-minimal scope; live-verified"
```

---

## Out of scope (separate slices, both still in 0.8.0)

- **Slice 2 — Phase-correct CFR frame pacing (ADR 0035, "CFR-Resampler"):** its own spec→plan→impl. No elevation, no PresentMon; reuses the same `LastPresentTime` tap.
- **Release mechanics 0.8.0:** version bump `0.7.0→0.8.0` (`CMakeLists.txt:6` + README + README-PORTABLE + KNOWN_LIMITATIONS — `portable_docs_test` trap), ADR 0033 **Proposed→Accepted**, roadmap "Next step" → 0.8.0, package manifests. Runs **after** Slice 2.

---

## Self-Review

- **Spec coverage:** Vendoring (T1) ✓ · real ETW session (T5) ✓ · provider→snapshot bridge (T6–T7) ✓ · correlation enrichment (T4) ✓ · killer-check (T3) ✓ · DPC minimal (T8–T9) ✓ · graceful-unavailable (T2/T6 tests) ✓ · render verify (T10) ✓ · ADR (T11) ✓. The CFR-resampler and release mechanics are explicitly deferred to their own slices per the locked slicing decision.
- **Type consistency:** `RecommendationEngine` constructor is extended once (Task 3) to its final 8-arg form `(caps, config, refresh, free_bytes, profile_supported, fs_name, live_snapshot*, present*)`; `SetDpcLatency`/`DpcLatencyReading` added in Task 8. `diagnostics::PresentMode` (4-value, `PresentProvider.h`) vs `recorder_core::PresentMode` (snapshot) are bridged by `ToSnapshotMode` (Task 7). `MapPresentEvent`/`RawPresentEvent` defined in Task 2 and consumed in Task 5.
- **Placeholders:** the only `...` are inside the explicitly-labelled ETW-I/O skeletons (T5/T9), which must be completed against the pinned PresentMon API — this is called out as the honest verification boundary, not a hidden TODO. All pure-function tasks carry complete code.
- **Known traps embedded:** test targets that compile diagnostics sources directly are updated in T2/T6/T7/T9; full build before commit (T10); provider compiles with `EXOSNAP_HAS_PRESENTMON` undefined (T5/T6 `#else`).
