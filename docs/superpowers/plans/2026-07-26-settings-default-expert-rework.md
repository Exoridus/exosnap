# Settings Default/Expert Rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-gate the Settings page around the "Expert = incompatibility risk / format expertise" criterion, grow the quality ladder to five tiers, add free frame-rate entry, and apply the approved layout/label polish — per `docs/superpowers/specs/2026-07-26-settings-default-expert-rework-design.md` (the spec; read it first, it is the authority for every label and gate).

**Architecture:** `ConfigPage.cpp` keeps its one-applier gating model (`updateExpertModeVisibility()`); rows move between the lazily-built expert subtrees and eagerly-built default sections. The quality ladder grows in `engine` (`codec_types.h`) and every label surface follows. Frame rate stays a rational `num/den` in `VideoSettingsModel`; only its validation and UI change.

**Tech Stack:** C++20, Qt 6.9 Widgets, GoogleTest via `exosnap_add_gtest`, QSS theme tokens.

## Global Constraints

- en-US spelling in every user-visible string: "color", never "colour".
- Exact label "Merge with above" (unchanged, CLAUDE.md canon).
- Quality tier labels, CQ-first: `CQ 35 · Draft`, `CQ 30 · Efficient`, `CQ 24 · Balanced`, `CQ 19 · High`, `CQ 16 · Ultra` (the `·` is U+00B7).
- Default frame-rate list: 15/30/60 selectable + "120 fps (unavailable)" disabled item. Expert replaces the combo with a 1–240 fps spinbox in the same row.
- Shared control width stays 160 px (`settingsRowInput`); shared row height becomes 46 px (input min-height 36 + 5 px vertical margins).
- `ConfigPage.cpp` is a high-churn single-writer file (AGENTS.md): Tasks 4–10 touch it and MUST run strictly sequentially, never in parallel agents.
- Expert subtrees are built lazily on first expert-enable; anything moving to Default must be built eagerly in the constructor.
- Tests: run via `pwsh scripts/run-tests.ps1 -Filter <binary>.` from repo root (never raw ctest). Build first with `cmake --build --preset windows-x64-debug` when C++ changed.
- Branch: continue on `settings-default-expert-rework`.
- Already true in the shipped app — NO task needed (spec items that were
  mockup-only fixes): buttons are square (`QPushButton` radius-md 10 in
  `exosnap_dark.qss:1876`), the mic post-processing group already has a
  header + live status + disclosure (`micPostProcessingHeader/Status/
  Disclosure`), and the Updates card never had a "Verified…" footnote.

---

### Task 1: Five-tier quality ladder in engine

**Files:**
- Modify: `libs/engine/include/exosnap/engine/codec_types.h:40-90`
- Modify: `app/models/RecordingPreset.cpp:170-203` (built-ins comment/usages)
- Modify: `app/diagnostics/ConfigSummary.cpp:79-96`
- Test: `libs/engine/tests/test_nvenc_rc_params.cpp:143-170`

**Interfaces:**
- Produces: `enum class NvencQualityPreset { High, Balanced, Efficient, Draft, Ultra }` (old `Small` renamed to `Efficient`; new members appended so existing underlying values 0/1/2 are unchanged), `CanonicalCq()` returning 19/24/30/35/16 respectively, `NearestQualityPreset(uint32_t)` snapping to the five anchors {16,19,24,30,35} with ties toward the LOWER CQ (higher quality), `IsCanonicalCq()` true for exactly those five.

- [ ] **Step 1: Update the quality-mapping tests to the five-tier ladder**

In `test_nvenc_rc_params.cpp` replace the three `QualityPresetMapping` tests:

```cpp
TEST(QualityPresetMapping, CanonicalCqMatchesTheFiveTierLadder) {
    EXPECT_EQ(exosnap::engine::CanonicalCq(exosnap::engine::NvencQualityPreset::Ultra), 16u);
    EXPECT_EQ(exosnap::engine::CanonicalCq(exosnap::engine::NvencQualityPreset::High), 19u);
    EXPECT_EQ(exosnap::engine::CanonicalCq(exosnap::engine::NvencQualityPreset::Balanced), 24u);
    EXPECT_EQ(exosnap::engine::CanonicalCq(exosnap::engine::NvencQualityPreset::Efficient), 30u);
    EXPECT_EQ(exosnap::engine::CanonicalCq(exosnap::engine::NvencQualityPreset::Draft), 35u);
}

TEST(QualityPresetMapping, NearestPresetRoundTripsAndSnapsBetweenValues) {
    using exosnap::engine::NvencQualityPreset;
    using exosnap::engine::NearestQualityPreset;
    for (auto p : {NvencQualityPreset::Ultra, NvencQualityPreset::High, NvencQualityPreset::Balanced,
                   NvencQualityPreset::Efficient, NvencQualityPreset::Draft}) {
        EXPECT_EQ(NearestQualityPreset(exosnap::engine::CanonicalCq(p)), p);
    }
    EXPECT_EQ(NearestQualityPreset(1u), NvencQualityPreset::Ultra);
    EXPECT_EQ(NearestQualityPreset(17u), NvencQualityPreset::Ultra);
    EXPECT_EQ(NearestQualityPreset(20u), NvencQualityPreset::High);
    EXPECT_EQ(NearestQualityPreset(23u), NvencQualityPreset::Balanced);
    EXPECT_EQ(NearestQualityPreset(28u), NvencQualityPreset::Efficient);
    EXPECT_EQ(NearestQualityPreset(33u), NvencQualityPreset::Draft);
    EXPECT_EQ(NearestQualityPreset(51u), NvencQualityPreset::Draft);
    // Midpoints resolve toward the lower CQ (higher quality):
    EXPECT_EQ(NearestQualityPreset(27u), NvencQualityPreset::Balanced);
    EXPECT_EQ(NearestQualityPreset(32u), NvencQualityPreset::Efficient);
}

TEST(QualityPresetMapping, IsCanonicalCqOnlyForTheFiveNamedValues) {
    for (uint32_t cq = exosnap::engine::kNvencCqMin; cq <= exosnap::engine::kNvencCqMax; ++cq) {
        const bool expected = (cq == 16u || cq == 19u || cq == 24u || cq == 30u || cq == 35u);
        EXPECT_EQ(exosnap::engine::IsCanonicalCq(cq), expected) << "cq=" << cq;
    }
}
```

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset windows-x64-debug` then `pwsh scripts/run-tests.ps1 -Filter test_nvenc_rc_params` → FAIL (Efficient/Draft/Ultra undefined).

- [ ] **Step 3: Implement in `codec_types.h`**

Rename `Small` → `Efficient` in the enum and append `Draft`, `Ultra` AFTER the existing members (preserves the numeric values behind existing `static_cast<int>` item data). Rewrite the three helpers:

```cpp
enum class NvencQualityPreset { High, Balanced, Efficient, Draft, Ultra };

inline constexpr uint32_t CanonicalCq(NvencQualityPreset preset) noexcept {
    switch (preset) {
    case NvencQualityPreset::Ultra: return 16;
    case NvencQualityPreset::High: return 19;
    case NvencQualityPreset::Efficient: return 30;
    case NvencQualityPreset::Draft: return 35;
    case NvencQualityPreset::Balanced: break;
    }
    return 24;
}

inline constexpr NvencQualityPreset NearestQualityPreset(uint32_t cq) noexcept {
    constexpr NvencQualityPreset kByQuality[] = {NvencQualityPreset::Ultra, NvencQualityPreset::High,
                                                 NvencQualityPreset::Balanced, NvencQualityPreset::Efficient,
                                                 NvencQualityPreset::Draft};
    NvencQualityPreset best = NvencQualityPreset::Ultra;
    uint32_t best_d = ~0u;
    for (NvencQualityPreset p : kByQuality) {
        const uint32_t c = CanonicalCq(p);
        const uint32_t d = cq > c ? cq - c : c - cq;
        if (d < best_d) { best_d = d; best = p; }  // strict '<': earlier (lower-CQ) wins ties
    }
    return best;
}

inline constexpr bool IsCanonicalCq(uint32_t cq) noexcept {
    return cq == 16u || cq == 19u || cq == 24u || cq == 30u || cq == 35u;
}
```

- [ ] **Step 4: Fix every `NvencQualityPreset::Small` reference repo-wide**

`grep -rn "NvencQualityPreset::Small"` — known sites: `app/models/RecordingPreset.cpp:189` (Efficiency built-in → `CanonicalCq(NvencQualityPreset::Efficient)`), `app/tests/test_recording_preset_registry.cpp:26,235,442`, `app/pages/ConfigPage.cpp` (handled again in Task 4 — here just rename to compile), `app/tests/test_config_page.cpp` (rename only). Update `app/diagnostics/ConfigSummary.cpp:79-96`: `NvencQualityName()` returns `"Ultra"/"High"/"Balanced"/"Efficient"/"Draft"`; `QualityName(uint32_t cq)` keeps its `CQ %1 (Name)` / `CQ %1 (~Name)` shape.

- [ ] **Step 5: Build + run** — `cmake --build --preset windows-x64-debug` then `pwsh scripts/run-tests.ps1 -Filter "test_nvenc_rc_params|recording_preset"` → PASS (the diagnostics test file runs in Task 12's full gate).

- [ ] **Step 6: Commit** — `git commit -m "feat(recorder): five-tier NVENC quality ladder (Draft/Efficient/Balanced/High/Ultra)"`

---

### Task 2: Frame-rate validation opens to 1–240

**Files:**
- Modify: `app/models/RecordingPreset.cpp:254-267` (`SanitizePresetConfig` fps whitelist)
- Test: `app/tests/test_recording_preset.cpp:477-512`, `app/tests/test_recording_preset_store.cpp:733-790`

**Interfaces:**
- Produces: sanitize rule — valid iff `frame_rate_den == 1 && frame_rate_num >= 1 && frame_rate_num <= 240`; anything else resets to 60/1. 120 is now storable (Expert free entry is not capability-checked; the Default combo still refuses it, Task 5).

- [ ] **Step 1: Rewrite the sanitize tests**

Replace `Sanitize_FrameRateUnsupported_ResetTo60_1` (line 495) and `Sanitize_FrameRate120Unavailable_ResetTo60_1` (line 504) with:

```cpp
TEST(RecordingPreset, Sanitize_FrameRateFreeValueInRange_Kept) {
    RecordingPresetConfig c = MakeDefaultPreset().config;
    c.video.frame_rate_num = 47; c.video.frame_rate_den = 1;
    c = SanitizePresetConfig(c);
    EXPECT_EQ(c.video.frame_rate_num, 47u);
    EXPECT_EQ(c.video.frame_rate_den, 1u);
}

TEST(RecordingPreset, Sanitize_FrameRate120_KeptAsExpertValue) {
    RecordingPresetConfig c = MakeDefaultPreset().config;
    c.video.frame_rate_num = 120; c.video.frame_rate_den = 1;
    c = SanitizePresetConfig(c);
    EXPECT_EQ(c.video.frame_rate_num, 120u);
}

TEST(RecordingPreset, Sanitize_FrameRateAboveCeiling_ResetTo60_1) {
    RecordingPresetConfig c = MakeDefaultPreset().config;
    c.video.frame_rate_num = 241; c.video.frame_rate_den = 1;
    c = SanitizePresetConfig(c);
    EXPECT_EQ(c.video.frame_rate_num, 60u);
}

TEST(RecordingPreset, Sanitize_FrameRateNonIntegerRational_ResetTo60_1) {
    RecordingPresetConfig c = MakeDefaultPreset().config;
    c.video.frame_rate_num = 30000; c.video.frame_rate_den = 1001;
    c = SanitizePresetConfig(c);
    EXPECT_EQ(c.video.frame_rate_num, 60u);
    EXPECT_EQ(c.video.frame_rate_den, 1u);
}
```

Keep `Sanitize_FrameRateNum0_ResetTo60_1` / `Sanitize_FrameRateDen0_ResetTo60_1` as they are. In `test_recording_preset_store.cpp`: `FrameRate120Unavailable_ResetsTo60fps` (line 760) becomes `FrameRate120_PersistsThroughStore` asserting 120 survives; `FrameRatePersists_50fps` (733) still passes (50 is in range) — leave it.

- [ ] **Step 2: Run to verify failures** — `pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset"` → new tests FAIL.

- [ ] **Step 3: Implement** — in `SanitizePresetConfig` replace the `kValidFps` whitelist block (lines 258-267) with:

```cpp
// Free frame rate (Expert entry): integer fps 1–240, den always 1. The
// Default combo offers 15/30/60 and displays the nearest for any other
// stored value; validity is intentionally wider than the list.
const bool fps_valid = config.video.frame_rate_den == 1 && config.video.frame_rate_num >= 1 &&
                       config.video.frame_rate_num <= 240;
if (!fps_valid) {
    config.video.frame_rate_num = 60;
    config.video.frame_rate_den = 1;
}
```

- [ ] **Step 4: Run to verify pass** — `pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset"` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "feat(preset): frame rate accepts any integer 1-240 fps"`

---

### Task 3: APP audio row config survives every capture target

**Files:**
- Modify: `app/models/RecordingPreset.cpp` (the app-row drop in `SanitizePresetConfig`, near the audio-row block — grep `Sanitize_AppAudioRow` test to locate the exact lines)
- Modify: `app/viewmodels/PresentationStateBuilder.cpp:42-53`
- Test: `app/tests/test_recording_preset.cpp:252-266`, plus a new PresentationStateBuilder test in the file that already covers `BuildAudioConfiguration` (grep `BuildAudioConfiguration` under `app/tests/`)

**Interfaces:**
- Produces: `BuildAudioConfiguration(...)` snapshot gains `app.active` (bool: window target) while `app.visible` becomes ALWAYS true; sanitize no longer strips the app row for display targets.

- [ ] **Step 1: Flip the tests.** `Sanitize_AppAudioRow_DroppedForDisplayTarget` (line 252) becomes `Sanitize_AppAudioRow_KeptForDisplayTarget` asserting the app row's enabled/merge fields survive sanitize with a display target. Add a builder test: with `target_kind == Display` expect `snap.app.visible == true && snap.app.active == false`; with `Window` expect both true.
- [ ] **Step 2: Run to verify failures** — `pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset|presentation"` → FAIL.
- [ ] **Step 3: Implement.** Remove the display-target drop in `SanitizePresetConfig`. In `PresentationStateBuilder.cpp:42-53` set `app.visible = true;` unconditionally and add `app.active = (audio_state.target_kind == capability::CaptureTargetKind::Window);` (add the `active` field to the snapshot struct where `visible` is declared).
- [ ] **Step 4: Run to verify pass**, then `git commit -m "feat(audio): application-audio row is a persisted setting for every capture target"`. (ConfigPage consumes `app.active` in Task 6 — until then the UI still keys off `visible`, which is now always true; the interim state is visually harmless and short-lived.)

---

### Task 4: ConfigPage quality UI — five tiers everywhere

**Files:**
- Modify: `app/pages/ConfigPage.cpp:929-1026` (quality_combo_, segments, quality_preset_combo_), `:2970-2990` (updateQualitySegmentSelection + compare hint)
- Modify: `app/models/SettingsCompareData.cpp:56-65`, `app/models/SettingsHintText.h:35-44`
- Test: `app/tests/test_config_page.cpp:566-628, 887-905, 2900-2918`

**Interfaces:**
- Consumes: Task 1 enum. Produces: `qualityPresetCombo` with five CQ-first items; hidden seam `videoQualityCombo` + five segments `qualitySegmentDraft/Efficient/Balanced/High/Ultra`.

- [ ] **Step 1: Update tests first.** `QualitySegment_HasSimpleLabels` (887) asserts five segments with texts `"Draft"`, `"Efficient"`, `"Balanced"`, `"High"`, `"Ultra"` and objectNames `qualitySegmentDraft…Ultra`. `QualitySegmentClick_EachSegmentUpdatesModel` (566): five clicks, `emit_count == 5`, each asserts `CanonicalCq` of its tier. `CqSpinBox_SegmentSelectionFollowsNearestPreset` (2900): 17→Ultra, 20→High, 29→Efficient, 33→Draft. Add:

```cpp
TEST_F(ConfigPageTest, QualityPresetCombo_HasFiveCqFirstLabels) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* combo = page.findChild<QComboBox*>("qualityPresetCombo");
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 5);
    EXPECT_EQ(combo->itemText(0), QStringLiteral("CQ 35 · Draft"));
    EXPECT_EQ(combo->itemText(1), QStringLiteral("CQ 30 · Efficient"));
    EXPECT_EQ(combo->itemText(2), QStringLiteral("CQ 24 · Balanced"));
    EXPECT_EQ(combo->itemText(3), QStringLiteral("CQ 19 · High"));
    EXPECT_EQ(combo->itemText(4), QStringLiteral("CQ 16 · Ultra"));
}
```

- [ ] **Step 2: Run to verify failures** — `pwsh scripts/run-tests.ps1 -Build -Filter config_page_tests` → FAIL.
- [ ] **Step 3: Implement.** `quality_combo_` (931-933): five items `"Draft"/"Efficient"/"Balanced"/"High"/"Ultra"` with matching enum data. Segments (969-974): five `makeQualitySegment` calls, objectNames as tested, ascending CQ order Draft→Ultra? No — keep ascending QUALITY order left→right: Draft, Efficient, Balanced, High, Ultra. `quality_preset_combo_` (988-993): the five CQ-first labels in the same order. `updateQualitySegmentSelection` (2970-2977) needs no logic change (findData handles five). Compare-hint values (2980-2988): switch over five presets → `"Draft"…"Ultra"`. `SettingsCompareData.cpp` `kQuality` rows become five: `{"Draft","CQ 35 · smallest files · visibly soft",false}`, `{"Efficient","CQ 30 · small files · softer",false}`, `{"Balanced","CQ 24 · best size-to-quality",true}`, `{"High","CQ 19 · sharp · larger files",false}`, `{"Ultra","CQ 16 · sharpest · largest files",false}`. `SettingsHintText.h` `kQualityPreset` → `"Draft ≈ CQ 35 … Ultra ≈ CQ 16 (sharpest, largest files).<br>Lower CQ = higher quality."`; `kConstantQuality` → `"16 = Ultra · 19 = High · 24 = Balanced · 30 = Efficient · 35 = Draft."`.
- [ ] **Step 4: Run to verify pass**, then `git commit -m "feat(settings): five-tier quality ladder in the quality card"`.

---

### Task 5: ConfigPage frame rate — new list + Expert free-entry spinbox; frame-timing labels

**Files:**
- Modify: `app/pages/ConfigPage.cpp:1037-1070` (combo + timing), `:2148-2172` (handlers), `:3001-3041` (hydration), `:5272-5462` (gate), `app/pages/ConfigPage.h` (new members)
- Test: `app/tests/test_config_page.cpp:257-307, 630-689`

**Interfaces:**
- Produces: `frame_rate_combo_` options {15,30,60} + disabled 120; new `QSpinBox* frame_rate_spin_` objectName `frameRateSpin` (range 1–240, suffix `" fps"`, width 160, `settingsRowInput=true`), hidden by default, swapped with the combo by `updateExpertModeVisibility()`; `timing_combo_` items `"CFR · Constant"` (data 1), `"VFR · Variable"` (data 0).

- [ ] **Step 1: Update/add tests.**
  - `FrameRateControl_UsesRealValues` (257): expect `count() == 4`; `findData(15/30/60)` all `>= 0`; `findData(24) == -1`; selecting data 30 → `frame_rate_num == 30`.
  - New:

```cpp
TEST_F(ConfigPageTest, FrameRate_ExpertSwapsComboForFreeSpinbox) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* combo = page.findChild<QComboBox*>("frameRateCombo");
    auto* spin = page.findChild<QSpinBox*>("frameRateSpin");
    ASSERT_NE(combo, nullptr); ASSERT_NE(spin, nullptr);
    EXPECT_TRUE(combo->isHidden());
    EXPECT_FALSE(spin->isHidden());
    EXPECT_EQ(spin->minimum(), 1); EXPECT_EQ(spin->maximum(), 240);
    spin->setValue(48);
    // leaving Expert keeps the value and displays the nearest list entry
    page.setExpertModeEnabled(false);
    EXPECT_FALSE(combo->isHidden());
    EXPECT_EQ(combo->currentData().toInt(), 60);  // nearest of {15,30,60} to 48
}

TEST_F(ConfigPageTest, TimingCombo_UsesDescriptiveLabels) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* timing = page.findChild<QComboBox*>("timingCombo");
    ASSERT_NE(timing, nullptr);
    EXPECT_EQ(timing->itemText(0), QStringLiteral("CFR · Constant"));
    EXPECT_EQ(timing->itemText(1), QStringLiteral("VFR · Variable"));
}
```

  - `FormatSummary_RefreshesOnFrameRateComboChange` (630): switch its `findData(24)` to `findData(15)` and expect "15 fps".
- [ ] **Step 2: Run to verify failures.**
- [ ] **Step 3: Implement.**
  - Options loop 1042: `for (const int fps : {15, 30, 60})`; keep the disabled-120 block (1045-1051) verbatim.
  - Create `frame_rate_spin_` right after the combo, add BOTH to the same `makeSettingsRow` content layout; `frame_rate_spin_->setVisible(false)`.
  - New handler `onFrameRateSpinChanged(int fps)`: guard `updating_frame_rate_`, set `video_settings_.frame_rate_num = fps; frame_rate_den = 1;` then the same three refresh calls as `onFrameRateChanged` (2155-2158).
  - Hydration `updateFrameRateSelection` (3001): seed the spinbox with the exact value; seed the combo with the nearest ENABLED item — helper:

```cpp
namespace { int NearestListedFps(uint32_t num) {
    int best = 60; uint32_t best_d = ~0u;
    for (int fps : {15, 30, 60}) {
        const uint32_t d = num > uint32_t(fps) ? num - fps : fps - num;
        if (d < best_d) { best_d = d; best = fps; }
    }
    return best;
} }
```

  - `updateExpertModeVisibility()`: add `frame_rate_combo_->setVisible(!expert); frame_rate_spin_->setVisible(expert);` next to the `quality_preset_row_widget_` block (5301). Also re-seed both from `video_settings_` there.
  - Timing labels 1063-1064: `addItem(QStringLiteral("CFR · Constant"), 1); addItem(QStringLiteral("VFR · Variable"), 0);` — note the item ORDER flips so CFR is index 0; `updateTimingSelection` (3012) uses `findData`, unaffected; the MP4-disables-VFR item lookup must use `findData(0)`, not a hardcoded index (check 3020-3035 and fix if positional). Compare-hint short values "CFR"/"VFR" stay.
  - Lock path 5874 and wheel filter 2010: add the spinbox alongside the combo.
- [ ] **Step 4: Run to verify pass**, then `git commit -m "feat(settings): frame-rate list 15/30/60 with Expert free entry; descriptive timing labels"`.

---

### Task 6: Audio card re-gating (the big one)

**Files:**
- Modify: `app/pages/ConfigPage.cpp:1149-1305` (audio card build), `:3758-4335` (buildAudioExpertSection → split), `:3386-3535` (applyAudioConfigurationState), `:3587` (updateAudioFormatControlVisibility), `:5355-5455` (gate), `app/pages/ConfigPage.h`
- Test: `app/tests/test_config_page.cpp` (audio sections), `app/tests/test_settings_tiers.cpp:469-493`

**Interfaces:**
- Consumes: Task 3's `app.active`. Produces: a new eager `buildAudioDefaultSettingsSection()` holding (in this order): Mic channel mode · Audio bitrate · Channels · Bit depth (codec-cond) · FLAC compression (codec-cond) · Mic gain · Brickwall limiter (+ceiling while on); `audio_expert_section_` (still lazy) holding only: Opus frame duration · Opus complexity · Sample rate · Audio clock slaving; the mic post-processing group moves into the DEFAULT section (after the limiter block), un-indented, with parameter rows visible only while their stage is on.

- [ ] **Step 1: Update tests first.**
  - `test_settings_tiers.cpp` `ConfigPage_AudioExpertControls_Exist` (485): expert section now contains only the four expert members; add a sibling test `ConfigPage_AudioDefaultControls_VisibleWithoutExpert` asserting `micChannelModeCombo`, `audioBitrateKbpsSpin`, `audioChannelsCombo`, `micGainSlider`, `limiterCheck`, `micPostProcessingHeader` are all findable and `!isHidden()` with expert OFF (no `setExpertModeEnabled` call).
  - `test_config_page.cpp`: `MicPostProcessing_DisclosureStartsCollapsedAndExpands` (2113) and `_HeaderStatusReflectsActiveStages` (2140): drop their `setExpertModeEnabled(true)` prelude — the group is default-visible now. Add:

```cpp
TEST_F(ConfigPageTest, MicPostProcessing_ParamRowsOnlyWhileStageOn) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* gate_check = page.findChild<QWidget*>("micGateCheck");
    auto* gate_spin = page.findChild<QWidget*>("micGateThresholdSpin");
    ASSERT_NE(gate_check, nullptr); ASSERT_NE(gate_spin, nullptr);
    // gate defaults off -> its threshold row is hidden even when expanded
    EXPECT_TRUE(gate_spin->parentWidget()->isHidden() || gate_spin->isHidden());
}
```

    (Use the real objectNames from cpp:4153-4202; verify with grep before writing.)
  - APP-row tests: `SetAudioUiState_DisplayMode_HidesAppSection` (1097) becomes `..._RecedesAppSection`: `AppSectionVisible()` stays TRUE, `settingsAudioAppCheck` stays `isEnabled()`, and the app meter is inactive. Same inversion for `AudioPolicy_WindowToDisplay_AppSectionHides` (1204), `AudioState_NoStaleAppRow_AfterWindowToDisplay` (1379). `LockOrderInvariant_*` (1279-1336): the App check is no longer disabled for display targets — assert enabled unless recording-locked.
  - Merge on first row: add `SettingsAudio_AppRowHasNoMergeCluster` asserting `settingsAudioAppMerge` is hidden (`isHidden()`), while `settingsAudioSysMerge`/`settingsAudioMicMerge` stay visible. `SettingsAudio_MergeControlUsesDocumentedLabel` (2988) unchanged.
- [ ] **Step 2: Run to verify failures.**
- [ ] **Step 3: Implement**, in this order:
  1. Extract a member function `buildAudioDefaultSettingsSection()` from the existing `buildAudioExpertSection()` code, moving the listed controls verbatim (keep every objectName). Call it in the constructor right after the mic-device row (cpp:1285); insert its widget at the old `audio_expert_insert_index_` position. Row order per the Interfaces block.
  2. `audio_expert_section_` keeps the remaining four rows; its lazy-build and gate lines (5355-5356) stay.
  3. Mic post-processing: move `mic_post_header_`/`mic_post_content_` insertion into the default section (at the end, after the limiter block); change `mic_post_content_` margins `(20,0,0,12)` → `(0,0,0,12)` (un-indent); rename the AGC stage label `"Automatic gain control"` → `"AGC"` (cpp:4153-4202 region); wrap each param row in its stage's visibility: connect each stage check's `toggled` to `param_row->setVisible(checked)` and seed on build.
  4. `updateAudioFormatControlVisibility` (3587): delete the `!expert_mode_enabled_` early-return — bit depth/FLAC rows are codec-conditional only now.
  5. APP row: in `applyAudioConfigurationState` (3497-3498) replace `setVisible(snap.app.visible)` with: section always visible; `app_enabled_check_->setEnabled(!locked)`; drive muted title styling + meter inactivity + the explanatory `source_label` text `"Takes effect while a specific application window is the capture target."` from `snap.app.active`.
  6. Hide the APP merge cluster: after the `makeSourceRowInto` call for APP (1214-1219), `app_separate_check_->hide();` plus its label and info icon (capture them from the lambda via an out-param or hide by iterating the row's children — prefer extending the lambda with a `bool with_merge` parameter).
  7. Mic-gain slider metrics: `mic_gain_slider_` fixed width 120 → 116 and `mic_gain_db_label_` width 42 → 40 with row spacing 4, so track + value span exactly the shared 160 px control width (same rule Task 9 applies to the webcam sliders).
  7. Re-gating seed code in `updateExpertModeVisibility` (5357-5455): the re-seeding of controls that moved to Default must run on every hydration, not just expert toggles — move those seed calls into `setAudioSettings`/the default section's build tail as appropriate.
- [ ] **Step 4: Run to verify pass** — `pwsh scripts/run-tests.ps1 -Build -Filter "config_page_tests|settings_tiers_tests"`.
- [ ] **Step 5: Commit** — `git commit -m "feat(settings): audio card re-gated - everyday mic controls live in Default"`

---

### Task 7: Output card — split rows to Default, "Split by time", header dropped

**Files:**
- Modify: `app/pages/ConfigPage.cpp:1413, 4450-4581` (split section), `:5297-5298` (gate)
- Test: `app/tests/test_config_page.cpp:1674-1868, 2060`, `app/tests/test_settings_tiers.cpp:277-313`

**Interfaces:**
- Produces: split controls built eagerly and never expert-gated; label `"Split by time"` (was "Split recording"); the `"Automatic split"` sub-label row is deleted and its `kSplitRecording` hint moves onto the Split-by-time row.

- [ ] **Step 1: Update tests.** `ConfigPage_SplitModeComboInExpander_HiddenByDefault` (tiers:289): the interval ROW is hidden because the toggle is off — not because of Expert; rewrite to assert visibility follows `splitModeToggle` with expert OFF. Add `SplitControls_VisibleWithoutExpertMode`. Existing `SplitModeToggle_OffHidesIntervalRow_OnShowsIt` (1804) and friends: remove any `setExpertModeEnabled(true)` preludes. Add a label assertion: the Output card contains a label with exact text `"Split by time"` and none with `"Split recording"` or `"Automatic split"` (use the `HasLabelText` helper scoped to `out_panel_`; note the Hotkeys card still has a "Split recording" HOTKEY row — scope the search to the Output panel widget).
- [ ] **Step 2: Run to verify failures.**
- [ ] **Step 3: Implement.** Call `buildSplitExpertSection()` unconditionally in the constructor (after cpp:1413 records the insert index); delete the gate lines 5297-5298 and the lazy guard's expert dependency; delete the `"Automatic split"` `makeOutputSubLabelWithHint` row (4527-4528) and attach `ui::hints::kSplitRecording` as the InfoHint of the `"Split by time"` header row (4536); rename the label string at 4536. Keep all objectNames (`splitModeToggle`, `splitModeCombo`, …) so the S5 stability test (2060) holds. Rename the member/function comments from "expert" wording where trivially adjacent (do not rename members themselves — churn without benefit).
- [ ] **Step 4: Run to verify pass**, then `git commit -m "feat(settings): automatic split is a Default feature named Split by time"`.

---

### Task 8: Container & codecs card — HDR row to Default, en-US labels

**Files:**
- Modify: `app/pages/ConfigPage.cpp:5059-5097` (HDR row build), `:2510-2585` (visibility), fmt-expert label strings in `buildFormatQualityExpertSections()` (4696-5237)
- Test: `app/tests/test_config_page.cpp:2729-2777`

**Interfaces:**
- Produces: HDR row built eagerly inside `fmt_panel_` (immediately after the audio-codec row), still display-conditional via `updateVideoHdrModeControl()`, options relabelled `"Tone-map to SDR"` / `"Native HDR10"`; the "Colour range" row label becomes `"Color range"`.

- [ ] **Step 1: Update tests.** `HdrRow_HiddenWithoutHdrActiveDisplay_ShownWithOne` (2729): drop the `setExpertModeEnabled(true)` prelude — the row must obey the display gate with expert OFF. Add an assertion that the combo's item 1 text is `"Native HDR10"`. Add `FmtCard_UsesAmericanColorSpelling`: no visible label under `fmt_panel_` contains `"Colour"`.
- [ ] **Step 2: Run to verify failures.**
- [ ] **Step 3: Implement.** Move the HDR block (5059-5097) out of `buildFormatQualityExpertSections()` into the constructor's fmt-card section (after the audio-codec row, before `fmt_expert_section_`'s insert point); keep members and objectNames; `updateVideoHdrModeControl()` unchanged (its `setVisible` already encodes the display gate — verify it is invoked from the constructor tail so the row starts hidden until capabilities arrive). Relabel `"Record native HDR10"` → `"Native HDR10"` (5083) and drop the combo's 200px fixed width to the shared 160. Rename `"Colour range"` → `"Color range"` where the row label is created inside `buildFormatQualityExpertSections()` (grep `Colour` in ConfigPage.cpp). Sweep remaining `colour` occurrences in `app/` user-visible strings: `SettingsHintText.h`, `SettingsCompareData.cpp`, `WebcamSetupPanel.cpp` ("Key colour" — Task 9 touches the row anyway, rename here if trivial), tooltips.
- [ ] **Step 4: Run to verify pass**, then `git commit -m "feat(settings): HDR handling joins Default; en-US color spelling"`.

---

### Task 9: Webcam panel — preview on top, key-color chip, rescan on preview

**Files:**
- Modify: `app/ui/widgets/WebcamSetupPanel.cpp` (whole layout), `app/ui/widgets/WebcamSetupPanel.h`
- Modify: `app/visual_tests/VisualScenario.cpp:466-490` (chroma scenarios)
- Test: `app/tests/test_webcam_setup_panel.cpp`, `app/tests/test_config_page.cpp:418-497`

**Interfaces:**
- Produces: vertical layout — `camera_preview_` full-width on top (min-height 150), then rows: Record webcam · Camera · Resolution/FPS · Mirror image · Overlay opacity · Chroma key (toggle only); while chroma is on: `Key color` row (a flat button objectName `webcamPanelKeyColorBtn` showing `chroma_swatch_` + hex text, opens the existing `QColorDialog` path) · Tolerance · Softness · Spill reduction. The four color chip buttons (`webcamPanelChromaGreenBtn`/`BlueBtn`/`MagentaBtn`/`CustomBtn`) are DELETED. `rescan_btn_` is re-parented onto `camera_preview_` (26×26, positioned bottom-right in `resizeEvent`), keeping objectName `webcamPanelRescanBtn`.

- [ ] **Step 1: Update tests.** In `test_webcam_setup_panel.cpp`: replace chip-button tests with `KeyColorButton_OpensPickerAndShowsHex` (button exists, hidden while chroma off, visible while on, text contains `#`), and `RescanButton_LivesOnThePreview` (`rescan_btn_->parentWidget() == camera_preview_`). `WebcamSetupPanel_HasCompactRescanNotLargeOpenSetup` (config_page:454): keep semantics, update the parent expectation. Assert label `"Key color"` exists and `"Key colour"` does not.
- [ ] **Step 2: Run to verify failures.**
- [ ] **Step 3: Implement** the layout restructure. Keep `addSliderRow` but change its metrics: slider fixed width 116, value label width 40, row spacing 4 (track + value = 160 flush). Slider rows for tolerance/softness/spill move inside `chroma_body_` (already conditional). Preview: `setMinimumHeight(150)`, horizontal size policy Expanding, remove the old left-column width caps (180–300). Rescan: re-parent, `raise()` in `resizeEvent`, `move(width()-btn-6, height()-btn-6)`.
- [ ] **Step 4: Visual scenarios.** `settings-webcam-chroma-green/-blue/-custom` (VisualScenario.cpp:472-484) drove the chip buttons — repoint them at the color value itself (scenario applies the QColor via the panel's apply/settings path; grep how `-chroma-custom` seeds today and mirror it).
- [ ] **Step 5: Run to verify pass** — `pwsh scripts/run-tests.ps1 -Build -Filter "webcam|config_page_tests|visual_scenarios"`, then `git commit -m "feat(webcam): full-width preview, key-color picker row, rescan on the preview"`.

---

### Task 10: Developer card always visible + column re-split + row height/icon polish

**Files:**
- Modify: `app/pages/ConfigPage.cpp:492-523` (makeCardTitle/glyph), `:550-607` (makeSettingsRow), `:844-869, 1145-1146, 1305, 1844-1857, 4589-4687, 5294-5295` (columns + developer), hand-rolled row margins in the expert/default section builders
- Modify: `app/ui/theme/exosnap_dark.qss` (only if a selector references the glyph chip size — currently none needed)
- Test: `app/tests/test_config_page.cpp:1999-2016, 3076-3160`

**Interfaces:**
- Produces: `buildDeveloperCard()` called eagerly in the constructor; card never gated. Columns — left: fmt · quality · webcam · presence · hotkeys; right: out · audio · updates · appearance · developer. Card glyphs render 18×18 in `ActiveTheme().text0` at stroke 2.0 (no 28px gutter). `makeSettingsRow` content margins become `(0,5,0,5)` → 46px rows; every hand-rolled row's `(0,12,0,12)` follows suit.

- [ ] **Step 1: Update tests.** `DeveloperCard_HiddenByDefault` (1999) becomes `DeveloperCard_VisibleByDefault` (findable + `!isHidden()` without expert); `DeveloperCard_VisibleWhenExpertModeEnabled` stays (still true). `CrashReportsToggle_HiddenBeforeExpertMode` (3076) becomes `..._VisibleByDefault`; `..._SetterAppliesBeforeAndAfterLazyBuild` (3105) simplifies to a plain setter test (no lazy build anymore). The `settings_tiers` developer tests (346-424) drop their expert preludes.
- [ ] **Step 2: Run to verify failures.**
- [ ] **Step 3: Implement.**
  - Developer: call `buildDeveloperCard()` in the constructor; delete gate lines 5294-5295 and the lazy-build guard's expert coupling; the card hint text drops the word "Expert" (`"Debug controls. …"`, cpp:4607-4610).
  - Columns: re-order the `addWidget` calls — left gets `fmt_panel_` (1145), `quality_panel_` (1146), `webcam_panel` (from 1853), `presence_panel_` (from 1854), `hotkeys_panel_` (1848); right gets `out_panel` (1852), `audio_panel` (from 1305), `updates_panel_` (1855), `appearance_panel_` (1856), developer (insert index now on `right_layout` — update `developer_insert_index_` capture and the `left_col_` member to the right column, rename it `developer_col_`). Keep `scrollToSection` targets (6038-6064) — they reference panels, not columns.
  - Glyphs: `makeCardTitle` chip `setFixedSize(18,18)`; `cardGlyphPixmap(icon_key, ActiveTheme().text0, 18, dpr)` with stroke width 2.0 (cpp:457-484).
  - Row height: `makeSettingsRow` margins `(0,12,0,12)` → `(0,5,0,5)` (cpp:550-607); grep `0, 12, 0, 12` within ConfigPage.cpp and the audio/split/fmt builders and apply the same; mic-gain/webcam slider rows keep their own metrics from Tasks 6/9.
- [ ] **Step 4: Run to verify pass** — `pwsh scripts/run-tests.ps1 -Build -Filter "config_page_tests|settings_tiers_tests"`, then `git commit -m "feat(settings): developer card in Default, rebalanced columns, 46px rows, plain title glyphs"`.

---

### Task 11: Updates hint text

**Files:**
- Modify: `app/pages/ConfigPage.cpp:1780-1788`
- Test: `app/tests/test_info_hints_config.cpp` (hint presence count only — verify it still passes)

- [ ] **Step 1:** Change the auto-check InfoHint text to: `"Check in the background and notify you when a new version is available. Downloads are signature-verified and installed by the separate Updater — nothing installs without you clicking Update."` (There is no footnote label to remove — the mockup's "Verified…" line never existed in the app.)
- [ ] **Step 2:** `pwsh scripts/run-tests.ps1 -Build -Filter "config_page_tests|info_hints"` → PASS, then `git commit -m "feat(settings): updates hint explains verification and the Updater handoff"`.

---

### Task 12: Visual-test scenarios follow the new reality

**Files:**
- Modify: `app/visual_tests/VisualScenario.cpp:839-931, 1319-1350`, `app/MainWindow.cpp:3235-3261` (only if a scenario field changes), `app/tests/test_visual_scenarios.cpp:314`

- [ ] **Step 1:** Rename/retarget: `settings-format-24-cfr` → `settings-format-15-cfr` (seed 15 fps), `settings-format-30-cfr`/`-60-cfr` keep, `settings-format-120-unavailable` keeps (asserts the disabled item), add `settings-format-expert-free-fps` (`settings_expert_mode = true`, seed 48 fps). `settings-hdr-*` scenarios (897-913) drop `settings_expert_mode = true` (HDR row is Default now). `ScenarioParserRejectsInvalidFrameRate` (test:314): update to the new validity rule (reject 0 and 241, accept 47).
- [ ] **Step 2:** Build + run `pwsh scripts/run-tests.ps1 -Build -Filter "visual_scenarios"` → PASS, then render a smoke set: `settings-display`, `settings-expert-on`, `settings-audio-expert-inline`, `settings-format-15-cfr`, `settings-webcam-chroma-green` via the `--visual-test` harness and eyeball the shots against the mockup renders.
- [ ] **Step 3:** Commit — `git commit -m "test(visual): settings scenarios cover the reworked page"`.

---

### Task 13: product-spec.md + CLAUDE.md

**Files:**
- Modify: `docs/product-spec.md` — §2:54-56, §3:83-111, §5:194-214, §6:250-271, §8:604-614, §12:851-874, §13 (no change needed beyond spelling), plus the en-US sweep lines 75, 77, 292, 354, 358, 443, 445-446, 550, 674, 676, 761, 764, 770, 782
- Modify: `CLAUDE.md:18` (APP row bullet) and `:24` (Settings sections bullet)

- [ ] **Step 1:** Update product-spec: frame-rate list (add the explicit 15/30/60 + disabled 120 + Expert 1–240 free entry with nearest-display rule); five-tier ladder table under §6; §5 APP row "always present, configurable; takes effect while a window is the capture target" + first-row-no-merge rule; §8 "Split by time"/"Split by size" labels, no section header; §12 re-drawn Expert list (bit depth, color range, NVENC preset, keyframe interval, chroma subsampling, rate control/CQ/bitrate, free fps, frame pacing, sample rate, Opus internals, clock slaving) and the note that no CARD is expert-gated; colour→color sweep. CLAUDE.md line 18: `The APP row is always present and configurable; it takes effect while a specific application window is the capture target. For screen capture the shipped default is SYS on, MIC off`.
- [ ] **Step 2:** `git commit -m "docs(spec): settings rework lands in product-spec and CLAUDE.md"`.

---

### Task 14: Full gate

- [ ] **Step 1:** `scripts\check-format.ps1` → clean (run `scripts\apply-format.ps1` first if it exists; otherwise fix by hand).
- [ ] **Step 2:** `cmake --build --preset windows-x64-debug` (full, includes all test targets).
- [ ] **Step 3:** `pwsh scripts/run-tests.ps1 -ExcludeLabel live` → all green. Paste failures verbatim if any; do not proceed on red.
- [ ] **Step 4:** Start the app once (Qt bin on PATH per the capture script convention) to confirm no startup crash, close it.
- [ ] **Step 5:** `git push -u origin settings-default-expert-rework`, open the PR (`gh pr create`), full review cycle, then `gh pr merge --squash --auto`.
