# Preset Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the preset simplification design (spec: `docs/superpowers/specs/2026-07-09-preset-simplification-design.md`). The live config becomes the persisted truth; a preset is a named snapshot compared via `ConfigDirtyEquivalent`. Four read-only built-ins ship (Default / Quality / Efficiency / Compatibility). The Settings preset row shrinks to a dropdown + `…` menu with two contextual buttons; `PresetManageOverlay` is deleted; the Output page entry point is aligned; `default_id` disappears; preset switching gets an Undo notification; corrupted stores are repaired field-wise instead of fully reset.

**Architecture:** All model logic stays Qt-free in `app/models/RecordingPreset.{h,cpp}` and `app/models/RecordingPresetRegistry.{h,cpp}` (pure functions + in-memory registry). Persistence stays in `app/settings/RecordingPresetStore.{h,cpp}` (toml++, one serialization path). UI stays in `app/pages/ConfigPage.*` / `app/pages/OutputPage.*`; orchestration (live mirrors, apply fan-out, persistence timing, undo state) stays in `app/MainWindow.*`. The recorder engine is untouched. Notifications reuse the existing typed `NotificationEvent` pipeline (`app/notifications/`), which requires new enum entries — there is **no generic "notification with callback" API** (see Risks).

**Tech Stack:** C++20 (MSVC), Qt 6.9 Widgets, toml++, GTest, CMake Ninja preset `windows-x64-debug`, `scripts/run-tests.ps1` as the only test entry point.

## Global Constraints

- Pre-1.0: breaking changes to persisted formats are allowed; incompatible data is repaired or reset, never migrated wholesale (exception: the targeted `color_range full→limited` migration, ADR 0032, is preserved).
- The engine (`libs/engine`) remains UI-agnostic and is not touched by this plan.
- Default profile stays **MKV + AV1 + Opus + CFR 60 fps** (unchanged `MakeDefaultPreset()`).
- No internal slice/task names, plan references, or codenames in commits, PRs, or code comments.
- Test runner: `pwsh scripts/run-tests.ps1 -Filter <binary-regex>` (matches test **binary** names, e.g. `recording_preset_store_tests`). Full suite once at the final gate. Add `-Build` when sources changed since the last build.
- Build preset `windows-x64-debug` in `build/windows-x64-debug`; do NOT re-configure. Run the app exe only with `C:\Qt\6.9.0\msvc2022_64\bin` prepended to PATH.
- `--target exosnap` does NOT build tests — build the full tree before ctest (memory: run-tests policy).
- UI strings are English app-wide (the spec text is German; use: `Save as new…`, `Rename…`, `Export…`, `Import…`, `Reset`, `Delete`, `Undo`, `(changed)`).
- After QSS edits, launch the app once — an invalid `${token}` crashes at startup (memory: QSS token validation).
- Each task ends with the touched test binaries green and a commit; the suite is never red between tasks.

## Ordering rationale (deviation from the draft order)

The draft proposed persistence first. This plan reorders to **1. environment fields → 2. built-ins → 3. persistence** because:

1. The store's new `Save()` must exclude built-ins from `presets.toml`; that filter needs `IsBuiltInPresetId()` (Task 2) to exist first, otherwise Task 3 would persist built-ins and re-migrate them later.
2. The `[live]` loader's fallback ("unreadable live → boot Default, selection repair → Default") relies on the registry *always* containing `kDefaultPresetId`, which the built-in seeding (Task 2) guarantees structurally.
3. The environment-field helpers (Task 1) are needed by the preset-switch/reset paths Task 3 touches, and they are the smallest independently valuable change (a monitor/HDR flip no longer flags `(changed)`).

`default_id` removal is folded into Task 3 (not Task 4): removing it from the store while the registry/UI still read it would force throwaway glue. The one transient artifact — `PresetManageOverlay`'s Set-default button becomes a no-op between Tasks 3 and 4 — is acceptable on a feature branch and is deleted with the overlay in Task 4.

---

## File Map

### Created
- `app/tests/test_output_page.cpp` — Output page preset-row contract tests (Task 6)

### Modified
- `app/models/RecordingPreset.h` / `.cpp` — env-field helpers, built-in factories, name folding, schema 23
- `app/models/RecordingPresetRegistry.h` / `.cpp` — built-in seeding + read-only enforcement, case-insensitive names, `default_id_` removal, delete-falls-to-Default
- `app/settings/RecordingPresetStore.h` / `.cpp` — `[live]` table, field-wise repair, `default_id` removal, built-in exclusion, `(imported)` suffix removal, `ExportAllUserPresetsToFile` removal
- `app/MainWindow.h` / `.cpp` — boot-to-live, debounced live persistence, undo state, handler removals, overlay removal
- `app/pages/ConfigPage.h` / `.cpp` — slim preset row (dropdown + `…` + contextual buttons), name-dialog validation
- `app/pages/OutputPage.h` / `.cpp` — aligned preset row, no emit-during-sync
- `app/notifications/NotificationEvent.h` — `SettingsRepaired`, `PresetSwitched` types; `UndoPresetSwitch` action
- `app/notifications/NotificationManager.cpp` — dismiss intervals for the new types
- `app/ui/overlay/NotificationToastWindow.cpp` — tone/icon/button mapping for the new types
- `app/visual_tests/VisualScenario.cpp` (+ the `applyVisualSettingsScenario` call sites in `MainWindow.cpp`) — preset scenarios follow the new contract
- `app/ui/theme/exosnap_dark.qss` (+ sibling themes) — drop dead `presetDirtyIndicator` rule
- `app/CMakeLists.txt` — remove `preset_manage_overlay_tests`, add `output_page_tests`
- `app/tests/test_recording_preset.cpp`, `test_recording_preset_registry.cpp`, `test_recording_preset_store.cpp`, `test_preset_export_import.cpp`, `test_config_page.cpp`
- `docs/product-spec.md` — §3 preset paragraph replaced; built-ins table added

### Deleted
- `app/ui/dialogs/PresetManageOverlay.h` / `.cpp`
- `app/tests/test_preset_manage_overlay.cpp`

---

## Task 1: `bit_depth` and `hdr_mode` become environment fields

**Files:**
- Modify: `app/models/RecordingPreset.h`, `app/models/RecordingPreset.cpp`
- Modify: `app/tests/test_recording_preset.cpp`

**Context (verified):**
- `ConfigDirtyEquivalent` compares `output.bit_depth` at `RecordingPreset.cpp:746-748` and `output.hdr_mode` at `RecordingPreset.cpp:758-760`; capture is already excluded (comment block at `RecordingPreset.cpp:724-732`).
- `NormalizedConfigEquals` (`RecordingPreset.cpp:441`) is the persistence round-trip comparator and must NOT change (its own comment says so).
- `SanitizePresetConfig` clamps 10-bit → 8-bit for H.264 at `RecordingPreset.cpp:197-204` — the one sanctioned exception where a codec switch may touch `bit_depth`.
- Existing test `RecordingPreset.NormalizedEquals_BitDepthDifference_NotEqual` (`app/tests/test_recording_preset.cpp:234-244`) asserts `EXPECT_FALSE(ConfigDirtyEquivalent(a, b))` for a bit-depth diff — it must flip to `EXPECT_TRUE`.

**Interfaces:**
- Consumes: `RecordingPresetConfig`, `OutputSettingsModel::Defaults()`, `PresetCaptureTarget`.
- Produces (in `app/models/RecordingPreset.h`, namespace `exosnap`):
  ```cpp
  // Environment fields describe the machine/display, not the user's recording
  // intent: capture identity, video bit depth, HDR handling. Presets neither
  // set nor override them, and they never count toward the (changed) state.

  // Returns `config` with the environment fields copied from `env` — used when
  // applying a preset so a switch never overrides the live environment.
  [[nodiscard]] RecordingPresetConfig WithEnvironmentFields(RecordingPresetConfig config,
                                                            const RecordingPresetConfig& env);

  // Returns `config` with the environment fields reset to model defaults —
  // used when snapshotting the live config into a named preset.
  [[nodiscard]] RecordingPresetConfig StripEnvironmentFields(RecordingPresetConfig config);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `app/tests/test_recording_preset.cpp` (and update the existing test at line 234: keep the `NormalizedConfigEquals` `EXPECT_FALSE`, flip the `ConfigDirtyEquivalent` assertion at line 243 to `EXPECT_TRUE`):

```cpp
// ===========================================================================
// Environment fields — bit_depth / hdr_mode never count as (changed)
// ===========================================================================

// Production call site: RecordingPresetRegistry::IsSelectedDirty
// (RecordingPresetRegistry.cpp) -> MainWindow dirty recompute on every
// settings-changed handler.
TEST(RecordingPreset, DirtyEquivalent_BitDepthDifference_NotChanged) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    a.output.video_codec = capability::VideoCodec::HevcNvenc;
    RecordingPresetConfig b = a;
    b.output.bit_depth = capability::BitDepth::Bit10;

    EXPECT_FALSE(NormalizedConfigEquals(a, b)); // persistence equality stays strict
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));   // environment field: not (changed)
}

TEST(RecordingPreset, DirtyEquivalent_HdrModeDifference_NotChanged) {
    RecordingPresetConfig a = MakeDefaultPreset().config;
    RecordingPresetConfig b = a;
    b.output.hdr_mode = exosnap::engine::HdrMode::Hdr10;

    EXPECT_FALSE(NormalizedConfigEquals(a, b));
    EXPECT_TRUE(ConfigDirtyEquivalent(a, b));
}

// Production call site: MainWindow::onPresetSelected / onResetChanges wrap the
// preset's saved config in WithEnvironmentFields(saved, captureLiveConfig())
// before applyPresetConfig, so a switch never overrides the environment.
TEST(RecordingPreset, WithEnvironmentFields_PreservesLiveEnvironment) {
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.output.video_codec = capability::VideoCodec::HevcNvenc;
    live.output.bit_depth = capability::BitDepth::Bit10;
    live.output.hdr_mode = exosnap::engine::HdrMode::Hdr10;
    live.capture.kind = PresetCaptureKind::Window;
    live.capture.window_key = "game.exe";

    RecordingPresetConfig preset = MakeDefaultPreset().config;
    preset.video.cq = 16; // intent field — must come from the preset

    const RecordingPresetConfig applied = WithEnvironmentFields(preset, live);
    EXPECT_EQ(applied.output.bit_depth, capability::BitDepth::Bit10);
    EXPECT_EQ(applied.output.hdr_mode, exosnap::engine::HdrMode::Hdr10);
    EXPECT_EQ(applied.capture.kind, PresetCaptureKind::Window);
    EXPECT_EQ(applied.capture.window_key, "game.exe");
    EXPECT_EQ(applied.video.cq, 16u);
}

// The H.264 clamp remains the only sanctioned override (spec exception):
// applying an H.264 preset onto a 10-bit environment sanitizes to 8-bit.
TEST(RecordingPreset, WithEnvironmentFields_H264Clamp_StillForcesEightBit) {
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.output.bit_depth = capability::BitDepth::Bit10;

    RecordingPresetConfig preset = MakeDefaultPreset().config;
    preset.output.container = capability::Container::Mp4;
    preset.output.video_codec = capability::VideoCodec::H264Nvenc;
    preset.output.audio_codec = capability::AudioCodec::AacMf;

    const RecordingPresetConfig applied = SanitizePresetConfig(WithEnvironmentFields(preset, live));
    EXPECT_EQ(applied.output.bit_depth, capability::BitDepth::Bit8);
}

// Production call site: RecordingPresetRegistry::AddPreset snapshots via
// StripEnvironmentFields so exported preset files carry no environment claims.
TEST(RecordingPreset, StripEnvironmentFields_ResetsToModelDefaults) {
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.output.video_codec = capability::VideoCodec::HevcNvenc;
    live.output.bit_depth = capability::BitDepth::Bit10;
    live.output.hdr_mode = exosnap::engine::HdrMode::Hdr10;
    live.capture.kind = PresetCaptureKind::Region;
    live.capture.has_region = true;
    live.capture.region = exosnap::engine::CaptureRegion{0, 0, 1280, 720};

    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    const RecordingPresetConfig stripped = StripEnvironmentFields(live);
    EXPECT_EQ(stripped.output.bit_depth, defaults.bit_depth);
    EXPECT_EQ(stripped.output.hdr_mode, defaults.hdr_mode);
    EXPECT_EQ(stripped.capture.kind, PresetCaptureKind::Display);
    EXPECT_FALSE(stripped.capture.has_region);
    EXPECT_EQ(stripped.output.video_codec, capability::VideoCodec::HevcNvenc); // intent kept
}
```

- [ ] **Step 2: Run and see red**

```
pwsh scripts/run-tests.ps1 -Build -Filter recording_preset_tests
```

Expected: compile failure `error C3861: 'WithEnvironmentFields': identifier not found` (a compile-failing test IS the red state). After declaring the two functions with stub bodies (`return config;`), expected gtest failures:
`DirtyEquivalent_BitDepthDifference_NotChanged` → `Value of: ConfigDirtyEquivalent(a, b)  Actual: false  Expected: true`, plus the strip/with tests failing on `EXPECT_EQ`.

- [ ] **Step 3: Implement**

In `RecordingPreset.cpp`, delete the two comparisons from `ConfigDirtyEquivalent` (ONLY there — leave `NormalizedConfigEquals` intact):

```cpp
    // DELETE from ConfigDirtyEquivalent (lines ~746-748 and ~758-760):
    //   if (a.output.bit_depth != b.output.bit_depth) { return false; }
    //   if (a.output.hdr_mode != b.output.hdr_mode) { return false; }
```

Extend the exclusion comment at the top of `ConfigDirtyEquivalent` (lines 724-732) to name all three environment fields. Then implement:

```cpp
// ---------------------------------------------------------------------------
// Environment fields
// ---------------------------------------------------------------------------

RecordingPresetConfig WithEnvironmentFields(RecordingPresetConfig config, const RecordingPresetConfig& env) {
    config.capture = env.capture;
    config.output.bit_depth = env.output.bit_depth;
    config.output.hdr_mode = env.output.hdr_mode;
    return config;
}

RecordingPresetConfig StripEnvironmentFields(RecordingPresetConfig config) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    config.capture = PresetCaptureTarget{};
    config.output.bit_depth = defaults.bit_depth;
    config.output.hdr_mode = defaults.hdr_mode;
    return config;
}
```

- [ ] **Step 4: Sweep dependent assertions**

`ConfigDirtyEquivalent` is also asserted in `app/tests/test_audio_gain_preset.cpp`, `test_audio_encoding_preset.cpp`, `test_video_rate_control.cpp`, `test_recording_preset_registry.cpp` — those exercise audio/video intent fields and stay green, but run them to prove it. Also grep the whole `app/` tree for `ConfigDirtyEquivalent` and `bit_depth` in the same file to catch any assertion this research missed:

```
pwsh scripts/run-tests.ps1 -Filter "recording_preset_tests|recording_preset_registry_tests|audio_gain_preset_tests|audio_encoding_preset_tests|video_rate_control_tests"
```

- [ ] **Step 5: Commit**

```
git add -A && git commit -m "feat(presets): treat bit depth and HDR mode as environment fields

Capture identity was already excluded from the preset changed-comparison;
bit depth and HDR mode describe the display and source the same way. They
no longer count as changes against the selected preset, and new helpers
let apply/snapshot paths preserve or strip them explicitly. The H.264
8-bit clamp in sanitization is unchanged."
```

---

## Task 2: Four read-only built-ins, case-insensitive unique names, import suffix `(2)`

**Files:**
- Modify: `app/models/RecordingPreset.h`, `app/models/RecordingPreset.cpp`
- Modify: `app/models/RecordingPresetRegistry.h`, `app/models/RecordingPresetRegistry.cpp`
- Modify: `app/settings/RecordingPresetStore.cpp` (drop ` (imported)` suffix)
- Modify: `app/MainWindow.cpp` (`refreshPresetUi()` sets real `built_in`)
- Modify: `app/tests/test_recording_preset.cpp`, `test_recording_preset_registry.cpp`, `test_preset_export_import.cpp`

**Context (verified):**
- `kDefaultPresetId = "preset.default"` (`RecordingPreset.h:43`); `MakeDefaultPreset()` builds MKV+AV1+Opus, `cq = CanonicalCq(High)` = 19 (`RecordingPreset.cpp:110`, `codec_types.h:52-62`), `nvenc_preset` default P4 via `OutputSettingsModel::Defaults()`.
- CQ 16 is deliberately non-canonical: `IsCanonicalCq` (`codec_types.h:85-88`) is false for 16, `NearestQualityPreset(16)` = High — the quality segment UI already renders non-canonical values with a `~` prefix (`test_config_page.cpp:2565` `CqSpinBox_SegmentSelectionFollowsNearestPreset`), so no UI work is needed for the Quality preset.
- `RecordingPresetRegistry::DeduplicateName` (`RecordingPresetRegistry.cpp:279-300`) already appends ` (2)`, ` (3)` but compares **case-sensitively**; `RenameSelected` (`:197-217`) likewise. Spec requires trimmed + case-insensitive.
- `MainWindow::refreshPresetUi()` hardcodes `co.built_in = false` (`MainWindow.cpp:2544` and `:2552`); ConfigPage already disables Rename/Delete and shows a "Built-in preset" badge when the flag is true (`ConfigPage.cpp:3620-3659`), OutputPage likewise (`OutputPage.cpp:202-235`) — activating the flag is therefore backend-only in this task.
- Store import suffix ` (imported)` on id collision: `RecordingPresetStore.cpp:1383-1388`. Registry `ImportPreset` (`RecordingPresetRegistry.cpp:163-172`) already name-dedupes — the suffix is redundant and is replaced by the `(2)` dedupe.
- `RecordingPresetRegistry` invariants comment (`RecordingPresetRegistry.h:12-24`); `AddDefaultPreset` (`:69`), `DuplicateSelected` (`:78`), `ResetAllToDefault` (`:100`) still exist after this task (UI removal happens in Task 4); they must simply keep working against the 4-seeded registry.

**Interfaces:**
- Produces (in `RecordingPreset.h`):
  ```cpp
  inline constexpr std::string_view kQualityPresetId = "preset.quality";
  inline constexpr std::string_view kEfficiencyPresetId = "preset.efficiency";
  inline constexpr std::string_view kCompatibilityPresetId = "preset.compatibility";

  // The four read-only shipped presets, Default first. None of them sets an
  // environment field (capture / bit_depth / hdr_mode stay at model defaults).
  [[nodiscard]] std::vector<RecordingPreset> MakeBuiltInPresets();

  [[nodiscard]] bool IsBuiltInPresetId(std::string_view id);

  // Trim + ASCII-lowercase fold for name uniqueness ("streaming" == "Streaming ").
  // Non-ASCII case folding is intentionally not attempted.
  [[nodiscard]] std::string FoldPresetName(std::string_view name);
  ```
- Produces (in `RecordingPresetRegistry.h`):
  ```cpp
  // True when `id` names one of the shipped read-only presets.
  [[nodiscard]] static bool IsBuiltIn(std::string_view id);
  // True when a preset other than `exclude_id` already uses `name`
  // (trimmed, case-insensitive). Built-in names are always taken.
  [[nodiscard]] bool IsNameTaken(std::string_view name, std::string_view exclude_id = {}) const;
  ```
- Changed semantics: `SaveSelected` / `RenameSelected` / `DeleteSelected` return `false` for built-ins; `DeleteSelected` of a user preset moves the selection to `kDefaultPresetId` (live config untouched — the caller no longer re-applies); `AddPreset` snapshots via `StripEnvironmentFields`; the registry constructor and `LoadState` always seed all four built-ins first (persisted copies of built-in ids are dropped, persisted user names colliding with built-in names are deduped).

- [ ] **Step 1: Write the failing tests**

`app/tests/test_recording_preset.cpp`:

```cpp
// ===========================================================================
// Built-in presets
// ===========================================================================

// Production call site: RecordingPresetRegistry constructor / LoadState seed
// the registry from MakeBuiltInPresets(); MainWindow::refreshPresetUi maps
// RecordingPresetRegistry::IsBuiltIn into ConfigPage/OutputPage ProfileOption.
TEST(RecordingPreset, MakeBuiltInPresets_FourPresets_ExpectedValues) {
    const std::vector<RecordingPreset> b = MakeBuiltInPresets();
    ASSERT_EQ(b.size(), 4u);

    EXPECT_EQ(b[0].id, kDefaultPresetId);
    EXPECT_EQ(b[0].name, "Default");
    EXPECT_EQ(b[0].config.video.cq, 19u);
    EXPECT_EQ(b[0].config.output.nvenc_preset, exosnap::engine::NvencPreset::P4);

    EXPECT_EQ(b[1].id, kQualityPresetId);
    EXPECT_EQ(b[1].name, "Quality");
    EXPECT_EQ(b[1].config.video.cq, 16u);
    EXPECT_EQ(b[1].config.output.nvenc_preset, exosnap::engine::NvencPreset::P6);
    EXPECT_EQ(b[1].config.output.container, capability::Container::Matroska);

    EXPECT_EQ(b[2].id, kEfficiencyPresetId);
    EXPECT_EQ(b[2].name, "Efficiency");
    EXPECT_EQ(b[2].config.video.cq, 30u);
    EXPECT_EQ(b[2].config.output.nvenc_preset, exosnap::engine::NvencPreset::P6);

    EXPECT_EQ(b[3].id, kCompatibilityPresetId);
    EXPECT_EQ(b[3].name, "Compatibility");
    EXPECT_EQ(b[3].config.output.container, capability::Container::Mp4);
    EXPECT_EQ(b[3].config.output.video_codec, capability::VideoCodec::H264Nvenc);
    EXPECT_EQ(b[3].config.output.audio_codec, capability::AudioCodec::AacMf);
    EXPECT_EQ(b[3].config.video.cq, 19u);
    EXPECT_EQ(b[3].config.output.nvenc_preset, exosnap::engine::NvencPreset::P4);

    // No built-in claims an environment field.
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    for (const auto& preset : b) {
        EXPECT_EQ(preset.config.output.bit_depth, defaults.bit_depth);
        EXPECT_EQ(preset.config.output.hdr_mode, defaults.hdr_mode);
        EXPECT_TRUE(preset.config.capture.display_key.empty());
        EXPECT_TRUE(preset.config.capture.window_key.empty());
        EXPECT_FALSE(preset.config.capture.has_region);
    }
}

TEST(RecordingPreset, IsBuiltInPresetId_MatchesExactlyTheFour) {
    EXPECT_TRUE(IsBuiltInPresetId(kDefaultPresetId));
    EXPECT_TRUE(IsBuiltInPresetId(kQualityPresetId));
    EXPECT_TRUE(IsBuiltInPresetId(kEfficiencyPresetId));
    EXPECT_TRUE(IsBuiltInPresetId(kCompatibilityPresetId));
    EXPECT_FALSE(IsBuiltInPresetId("preset.0123456789abcdef"));
    EXPECT_FALSE(IsBuiltInPresetId(""));
}

TEST(RecordingPreset, FoldPresetName_TrimsAndLowercases) {
    EXPECT_EQ(FoldPresetName("  Streaming "), "streaming");
    EXPECT_EQ(FoldPresetName("QUALITY"), "quality");
    EXPECT_EQ(FoldPresetName("   "), "");
}
```

`app/tests/test_recording_preset_registry.cpp` (new tests; existing tests updated in Step 4):

```cpp
// Production call site: MainWindow ctor -> RecordingPresetRegistry() and
// LoadState() after RecordingPresetStore::Load().
TEST(RecordingPresetRegistry, Constructor_SeedsFourBuiltIns_DefaultSelected) {
    RecordingPresetRegistry reg;
    EXPECT_EQ(reg.Count(), 4u);
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
    EXPECT_NE(reg.FindById(kQualityPresetId), nullptr);
    EXPECT_NE(reg.FindById(kEfficiencyPresetId), nullptr);
    EXPECT_NE(reg.FindById(kCompatibilityPresetId), nullptr);
}

// Production call sites: MainWindow::onSavePreset (until it is removed),
// onRenamePreset, onDeletePreset — the registry is the enforcement layer,
// the UI disable is only cosmetics.
TEST(RecordingPresetRegistry, BuiltIn_SaveRenameDelete_Refused) {
    RecordingPresetRegistry reg;
    ASSERT_TRUE(reg.SetSelected(std::string(kQualityPresetId)));

    RecordingPresetConfig cfg = MakeDefaultPreset().config;
    cfg.video.cq = 40;
    EXPECT_FALSE(reg.SaveSelected(cfg));
    EXPECT_EQ(reg.SelectedPreset().config.video.cq, 16u); // untouched

    EXPECT_FALSE(reg.RenameSelected("My Quality"));
    EXPECT_EQ(reg.SelectedPreset().name, "Quality");

    EXPECT_FALSE(reg.DeleteSelected());
    EXPECT_EQ(reg.Count(), 4u);
}

TEST(RecordingPresetRegistry, RenameSelected_CaseInsensitiveCollision_Refused) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Streaming");
    reg.AddPreset(MakeDefaultPreset().config, "Other");
    EXPECT_FALSE(reg.RenameSelected(" streaming "));
    EXPECT_FALSE(reg.RenameSelected("quality")); // built-in names are reserved
    EXPECT_TRUE(reg.RenameSelected("Recording"));
}

TEST(RecordingPresetRegistry, IsNameTaken_FoldsAndExcludesSelf) {
    RecordingPresetRegistry reg;
    const std::string id = reg.AddPreset(MakeDefaultPreset().config, "Streaming");
    EXPECT_TRUE(reg.IsNameTaken("STREAMING "));
    EXPECT_TRUE(reg.IsNameTaken("default")); // reserved
    EXPECT_FALSE(reg.IsNameTaken("Streaming", id)); // renaming to own name is fine
    EXPECT_FALSE(reg.IsNameTaken("Fresh"));
}

// Production call site: MainWindow::onDeletePreset — after delete the
// selection falls to Default and the live config is NOT re-applied.
TEST(RecordingPresetRegistry, DeleteUserPreset_SelectionFallsToDefault) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Mine");
    ASSERT_TRUE(reg.DeleteSelected());
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
    EXPECT_EQ(reg.Count(), 4u);
}

// Production call site: MainWindow ctor LoadState — a stale persisted copy of
// a built-in id is dropped; a user preset named like a built-in is deduped.
TEST(RecordingPresetRegistry, LoadState_DropsPersistedBuiltInIds_DedupesReservedNames) {
    RecordingPreset stale_default = MakeDefaultPreset();
    stale_default.config.video.cq = 51; // must not survive

    RecordingPreset user;
    user.id = GeneratePresetId();
    user.name = "quality"; // collides with built-in name (folded)
    user.config = MakeDefaultPreset().config;

    RecordingPresetRegistry reg;
    reg.LoadState({stale_default, user}, user.id, std::string(kDefaultPresetId));
    EXPECT_EQ(reg.Count(), 5u);
    EXPECT_EQ(reg.FindById(kDefaultPresetId)->config.video.cq, 19u);
    EXPECT_EQ(reg.FindById(user.id)->name, "quality (2)");
    EXPECT_EQ(reg.SelectedId(), user.id);
}

// Production call site: RecordingPresetRegistry::AddPreset via
// MainWindow::onSavePresetAs — snapshots never carry environment fields.
TEST(RecordingPresetRegistry, AddPreset_StripsEnvironmentFields) {
    RecordingPresetRegistry reg;
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.output.video_codec = capability::VideoCodec::HevcNvenc;
    live.output.bit_depth = capability::BitDepth::Bit10;
    live.capture.display_key = "monitor-1";
    const std::string id = reg.AddPreset(live, "Mine");
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    EXPECT_EQ(reg.FindById(id)->config.output.bit_depth, defaults.bit_depth);
    EXPECT_TRUE(reg.FindById(id)->config.capture.display_key.empty());
}
```

`app/tests/test_preset_export_import.cpp` — update `IdCollision_NewIdAssigned_ConfigPreserved` (`:134`) so it asserts the name is **unchanged** by the store (no ` (imported)` suffix), and add:

```cpp
// Production call site: MainWindow::onImportProfiles ->
// RecordingPresetStore::ImportPresetsFromFile + RecordingPresetRegistry::
// ImportPreset. Import never rejects; names collide into "(2)", "(3)".
TEST(PresetExportImport, ImportNameCollision_GetsNumericSuffix_NotImportedSuffix) {
    RecordingPresetRegistry reg;
    reg.AddPreset(MakeDefaultPreset().config, "Streaming");

    RecordingPreset incoming;
    incoming.id = GeneratePresetId();
    incoming.name = " streaming "; // folded collision
    incoming.config = MakeDefaultPreset().config;
    reg.ImportPreset(incoming);

    const RecordingPreset* found = reg.FindById(incoming.id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "streaming (2)");
    EXPECT_EQ(found->name.find(" (imported)"), std::string::npos);
}
```

- [ ] **Step 2: Run and see red**

```
pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset_tests|recording_preset_registry_tests|preset_export_import_tests"
```

Expected: compile failure `'MakeBuiltInPresets': identifier not found`; after stubbing declarations, failures such as `Constructor_SeedsFourBuiltIns_DefaultSelected` → `Expected equality of these values: reg.Count() Which is: 1 ... 4u`.

- [ ] **Step 3: Implement the model**

`RecordingPreset.cpp`:

```cpp
std::vector<RecordingPreset> MakeBuiltInPresets() {
    std::vector<RecordingPreset> result;
    result.push_back(MakeDefaultPreset());

    // Quality: maximum sharpness; costs disk and GPU. Default already sits at
    // the canonical High tier (cq 19), so Quality deliberately goes below the
    // canonical ladder to cq 16 (the segment UI renders it as "~High").
    RecordingPreset quality = MakeDefaultPreset();
    quality.id = std::string(kQualityPresetId);
    quality.name = "Quality";
    quality.config.video.cq = 16;
    quality.config.output.nvenc_preset = exosnap::engine::NvencPreset::P6;
    result.push_back(std::move(quality));

    // Efficiency: small files at usable quality. P6 buys compression with GPU
    // time instead of quality loss.
    RecordingPreset efficiency = MakeDefaultPreset();
    efficiency.id = std::string(kEfficiencyPresetId);
    efficiency.name = "Efficiency";
    efficiency.config.video.cq = exosnap::engine::CanonicalCq(exosnap::engine::NvencQualityPreset::Small);
    efficiency.config.output.nvenc_preset = exosnap::engine::NvencPreset::P6;
    result.push_back(std::move(efficiency));

    // Compatibility: editing, upload, GPUs without AV1 encode (pre-RTX-40).
    RecordingPreset compatibility = MakeDefaultPreset();
    compatibility.id = std::string(kCompatibilityPresetId);
    compatibility.name = "Compatibility";
    compatibility.config.output.container = capability::Container::Mp4;
    compatibility.config.output.video_codec = capability::VideoCodec::H264Nvenc;
    compatibility.config.output.audio_codec = capability::AudioCodec::AacMf;
    result.push_back(std::move(compatibility));

    return result;
}

bool IsBuiltInPresetId(std::string_view id) {
    return id == kDefaultPresetId || id == kQualityPresetId || id == kEfficiencyPresetId ||
           id == kCompatibilityPresetId;
}

std::string FoldPresetName(std::string_view name) {
    std::string folded = NormalizePresetName(name);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return folded;
}
```

(`GeneratePresetId()` already never collides with `"preset."`+hex16 built-in ids because the built-in suffixes are not 16 hex chars — no extra guard needed, but extend the `GeneratePresetId` doc comment at `RecordingPreset.h:105-107` to say "never equals any built-in id".)

- [ ] **Step 4: Implement the registry + update existing registry tests**

`RecordingPresetRegistry.cpp` changes:
- Constructor: `presets_ = MakeBuiltInPresets();` (selected/default stay `kDefaultPresetId`).
- `LoadState`: start `accepted = MakeBuiltInPresets()` and pre-seed `seen_ids` with the four built-in ids; while appending user presets, run the name through the fold-aware `DeduplicateName`. Keep the existing selected/default repair.
- `DeduplicateName` and `RenameSelected` and new `IsNameTaken`: compare with `FoldPresetName(...)` on both sides.
- `SaveSelected` / `RenameSelected` / `DeleteSelected`: first line `if (IsBuiltIn(selected_id_)) return false;`.
- `DeleteSelected`: replace the after/before fallback with `selected_id_ = std::string(kDefaultPresetId);` (always present). Keep the `default_id_` repair for now (removed in Task 3).
- `AddPreset`: `preset.config = StripEnvironmentFields(std::move(config));` before `SanitizePreset`.
- `IsBuiltIn`: static, delegates to `IsBuiltInPresetId`.
- `ImportPreset`: unchanged apart from the fold-aware dedupe. Also strip environment fields here (imported files may carry them from older exports).

Existing tests to update in `test_recording_preset_registry.cpp` (count/selection expectations shift from 1 seeded preset to 4):
`Constructor_SeedsDefault` (`:32`), `DeleteSelected_Count1_ReturnsFalse` (`:198` — becomes the built-in refusal case), `DeleteSelected_With2Presets_RemovesSelected_SelectsFallback` / `_FallbackToNextElement` / `_FallbackToPreviousWhenNoNext` (`:205/:232/:245` — collapse into the falls-to-Default rule), `DeleteSelected_DefaultPreset_DefaultFallsBack` (`:216` — Default is now undeletable), `ResetAllToDefault_Count1_IdsReset` (`:284` — expects 4), `LoadState_*` (`:408-456` — built-ins present). `Registry_ImportPreset_DeduplicatesName` (`test_preset_export_import.cpp:283`) — expectation now `(2)` suffix semantics (already matches).

Store side: delete the suffix block at `RecordingPresetStore.cpp:1383-1388`, keeping the fresh-id assignment:

```cpp
        if (used_ids.count(preset.id) > 0) {
            preset.id = GeneratePresetId();
            // Name collisions are resolved by the registry's numeric dedupe
            // ("name (2)") at insert time — no marker suffix here.
        }
```

MainWindow side (`MainWindow.cpp:2540-2556`): `co.built_in = RecordingPresetRegistry::IsBuiltIn(preset.id);` and the same for `oo.built_in`. (ConfigPage/OutputPage already render and gate on the flag — no page change needed in this task.)

- [ ] **Step 5: Green + full-suite sanity + commit**

```
pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset_tests|recording_preset_registry_tests|preset_export_import_tests|recording_preset_store_tests|config_page_tests"
```

(`recording_preset_store_tests` must stay green: `Load()` still enforces schema 22 in this task and the store never sees the registry.) Note: with the flag active, selecting a built-in now shows the "Built-in preset" badge and disables Rename/Delete in both pages — expected, spec-conform.

```
git add -A && git commit -m "feat(presets): ship four read-only built-in presets

Default (MKV/AV1/Opus, CQ 19, P4), Quality (CQ 16, P6), Efficiency
(CQ 30, P6), and Compatibility (MP4/H.264/AAC, CQ 19, P4) are seeded in
code and can no longer be overwritten, renamed, or deleted — the registry
enforces this, the UI flags were already wired. Preset names are now
unique case-insensitively, built-in names are reserved, imports resolve
name collisions with a numeric suffix instead of an '(imported)' marker,
and snapshots no longer capture environment fields."
```

---

## Task 3: Persistence — schema 23, `[live]` table, field-wise repair, `default_id` removal, boot-to-live

**Files:**
- Modify: `app/models/RecordingPreset.h` (schema constant + comment)
- Modify: `app/settings/RecordingPresetStore.h`, `.cpp`
- Modify: `app/models/RecordingPresetRegistry.h`, `.cpp` (remove `default_id_`, `SetDefault`, `DefaultId`; `LoadState` 2-arg)
- Modify: `app/MainWindow.h`, `.cpp` (boot-to-live, debounced persistence, set-default removal, repaired notification)
- Modify: `app/pages/ConfigPage.h`, `.cpp` (drop `default_id` from `setPresetOptions`, drop star suffix + Set-as-default action)
- Modify: `app/notifications/NotificationEvent.h`, `app/notifications/NotificationManager.cpp`, `app/ui/overlay/NotificationToastWindow.cpp` (`SettingsRepaired` type)
- Modify: `app/tests/test_recording_preset_store.cpp`, `test_recording_preset_registry.cpp`, `test_config_page.cpp`, plus `applyVisualSettingsScenario` call sites (`app/visual_tests/VisualScenario.cpp` scenarios `settings-preset-*` at `:549-570` and their MainWindow injection path)

**Context (verified):**
- Schema constant `kPresetSchemaVersion = 22` and gate constant `kPresetSchemaMigratableFrom = 19` (`RecordingPreset.h:33-37`). `Load()` full-resets on any version other than 22 or 19 (`RecordingPresetStore.cpp:1134-1138`); the v19 `color_range full→limited` migration is at `:1173-1176` (ADR 0032).
- `PresetFromToml` (`RecordingPresetStore.cpp:708-1015`) already defaults every missing/wrong-typed field — "field-wise repair" is achieved mostly by REMOVING the version gate, not by new parsing code.
- `Save(presets, selected_id, default_id)` writes `default_id` at `:1237`; `PersistedPresetState` carries it (`RecordingPresetStore.h:18-25`).
- Startup: `MainWindow.cpp:527-543` loads, `SetSelected(DefaultId())` (`:536`), mirrors seeded from `SelectedSavedConfig()` (`:539-543`). Two later re-applies of `SelectedSavedConfig()`: `coordinatorInitialized` (`:825-826`) and `buildConfigPage` (`:4369`) — both must switch to the boot live config / live mirrors.
- Dirty-recompute call sites that must now also schedule a live save: `MainWindow.cpp:753-763, 766-772, 778-789, 4144-4190, 4460-4470, 4510-4520, 4645-4655` (each computes `IsSelectedDirty(captureLiveConfig())`).
- Set-default UI: `set_default_preset_action_` (`ConfigPage.cpp:774`, handler `:3721-3723`, state `:3660-3662`), signal `setDefaultPresetRequested` (`ConfigPage.h:221`), MainWindow handler `onSetDefaultPreset` (`MainWindow.cpp:2713-2717`), overlay connect (`:800-801`), star suffix in `setPresetOptions` (`ConfigPage.cpp:3585-3589`).
- `notification_manager_` is constructed long after the preset load (toast init ~`MainWindow.cpp:3540`), so the repaired notification must be deferred via a flag.
- `closeEvent` is at `MainWindow.cpp:2220` — flush point for the debounced save.

**Interfaces:**
- Produces (`RecordingPresetStore.h`):
  ```cpp
  struct PersistedPresetState {
      std::vector<RecordingPreset> user_presets; // built-ins are code-defined, never persisted
      std::string selected_id;                   // repaired to kDefaultPresetId when unknown
      std::optional<RecordingPresetConfig> live; // nullopt: [live] missing/unreadable -> boot Default
      bool repaired = false;                     // parse failure, schema mismatch, or dropped items
  };

  [[nodiscard]] PersistedPresetState Load() const;
  void Save(const std::vector<RecordingPreset>& presets, const std::string& selected_id,
            const RecordingPresetConfig& live) const; // silently skips built-in ids
  ```
- Produces (`RecordingPreset.h`):
  ```cpp
  inline constexpr int kPresetSchemaVersion = 23;
  // Files at or below this schema get the targeted color_range full->limited
  // rewrite (ADR 0032) on top of the ordinary field-wise repair.
  inline constexpr int kPresetSchemaColorRangeMigratedThrough = 19;
  ```
  (`kPresetSchemaMigratableFrom` is renamed — it no longer gates loading at all.)
- Changed (`RecordingPresetRegistry.h`): `void LoadState(std::vector<RecordingPreset> user_presets, std::string selected_id);` — `SetDefault`, `DefaultId`, `default_id_` deleted; selection repair target is always `kDefaultPresetId`.
- Changed (`ConfigPage.h`): `void setPresetOptions(const std::vector<ProfileOption>& options, const QString& selected_id, bool dirty);`
- Produces (`MainWindow.h`, private):
  ```cpp
  void schedulePersistLiveState();      // 750 ms single-shot debounce -> persistPresetState()
  void onLiveConfigChanged();           // dirty recompute + schedulePersistLiveState()
  RecordingPresetConfig boot_live_config_;   // applied again on coordinatorInitialized
  QTimer* live_persist_timer_ = nullptr;
  bool preset_store_repaired_ = false;  // enqueue SettingsRepaired once toasts exist
  ```
- Produces (`NotificationEvent.h`): `NotificationType::SettingsRepaired` (auto-dismiss 8 s, no action).

- [ ] **Step 1: Write the failing store tests**

Rewrite/extend `app/tests/test_recording_preset_store.cpp`. All `Save()` call sites in the file change mechanically from `(presets, selected, default_id)` to `(presets, selected, live_config)`. New/rewritten tests (each names its production call site):

```cpp
// Production call site: MainWindow::persistPresetState() (Save) and the
// MainWindow ctor preset-load block (Load).
TEST(RecordingPresetStore, LiveTable_RoundTrips) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.video.cq = 33;
    live.output.container = capability::Container::Mp4;
    live.output.video_codec = capability::VideoCodec::H264Nvenc;
    live.output.audio_codec = capability::AudioCodec::AacMf;
    live.output.bit_depth = capability::BitDepth::Bit8;

    store.Save({}, std::string(kDefaultPresetId), live);
    const PersistedPresetState state = store.Load();

    ASSERT_TRUE(state.live.has_value());
    EXPECT_TRUE(NormalizedConfigEquals(*state.live, SanitizePresetConfig(live)));
    EXPECT_EQ(state.selected_id, kDefaultPresetId);
    EXPECT_FALSE(state.repaired);
    CleanupFile(path);
}

TEST(RecordingPresetStore, Save_ExcludesBuiltIns_KeepsUserPresets) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);

    RecordingPreset user = MakeRegionPreset();
    std::vector<RecordingPreset> all = MakeBuiltInPresets();
    all.push_back(user);

    store.Save(all, user.id, MakeDefaultPreset().config);
    const PersistedPresetState state = store.Load();
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, user.id);
    EXPECT_EQ(state.selected_id, user.id);
    CleanupFile(path);
}

// default_id must be gone from the on-disk format (update the existing
// TomlOnDisk_IsValidTomlWithExpectedKeys test at :1405 alongside this).
TEST(RecordingPresetStore, TomlOnDisk_HasLiveTable_NoDefaultId) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);
    store.Save({}, std::string(kDefaultPresetId), MakeDefaultPreset().config);

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(f.readAll());
    EXPECT_TRUE(text.contains(QStringLiteral("[live]")));
    EXPECT_TRUE(text.contains(QStringLiteral("schema_version = 23")));
    EXPECT_FALSE(text.contains(QStringLiteral("default_id")));
    CleanupFile(path);
}

// Field-wise repair replaces the full reset on version mismatch.
// Production call site: MainWindow ctor load — a schema-22 file keeps its
// user presets and live values instead of resetting.
TEST(RecordingPresetStore, SchemaMismatch_KeepsData_FlagsRepaired) {
    const QString path = UniqueTempPath();
    RecordingPresetStore store(path);
    RecordingPreset user = MakeRegionPreset();
    RecordingPresetConfig live = MakeDefaultPreset().config;
    live.video.cq = 27;
    store.Save({user}, user.id, live);

    // Rewrite the version stamp to the previous schema.
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    text.replace(QStringLiteral("schema_version = 23"), QStringLiteral("schema_version = 22"));
    ASSERT_TRUE(WriteTomlString(path, text));

    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.repaired);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_EQ(state.user_presets[0].id, user.id);
    ASSERT_TRUE(state.live.has_value());
    EXPECT_EQ(state.live->video.cq, 27u);
    CleanupFile(path);
}

// Case (a) of the spec: a corrupted live VALUE is clamped field-wise; a
// missing/unreadable [live] table yields nullopt (caller boots Default).
TEST(RecordingPresetStore, LiveTable_CorruptField_ClampedNotReset) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral(
        "schema_version = 23\n"
        "selected_id = \"preset.default\"\n"
        "presets = []\n"
        "[live]\n"
        "countdown_seconds = 7\n"          // invalid -> clamped to 0
        "[live.video]\n"
        "cq = 9999\n")));                  // out of range -> default kept
    const PersistedPresetState state = store_or(path); // RecordingPresetStore store(path);
    ASSERT_TRUE(state.live.has_value());
    EXPECT_EQ(state.live->countdown_seconds, 0);
    EXPECT_EQ(state.live->video.cq, 19u);
    CleanupFile(path);
}

TEST(RecordingPresetStore, LiveTable_Missing_ReturnsNullopt) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral(
        "schema_version = 23\nselected_id = \"preset.default\"\npresets = []\n")));
    RecordingPresetStore store(path);
    EXPECT_FALSE(store.Load().live.has_value());
    CleanupFile(path);
}

TEST(RecordingPresetStore, UnparseableFile_ReturnsDefaults_Repaired) {
    const QString path = UniqueTempPath();
    ASSERT_TRUE(WriteTomlString(path, QStringLiteral("this is not toml [ ===")));
    RecordingPresetStore store(path);
    const PersistedPresetState state = store.Load();
    EXPECT_TRUE(state.user_presets.empty());
    EXPECT_FALSE(state.live.has_value());
    EXPECT_EQ(state.selected_id, kDefaultPresetId);
    EXPECT_TRUE(state.repaired);
    CleanupFile(path);
}

// ADR 0032 migration survives the repair rework: schema <= 19 rewrites a
// stored "full" to "limited" (presets AND live), newer schemas respect it.
TEST(RecordingPresetStore, ColorRangeMigration_AppliesThroughSchema19_Only) {
    // Build a minimal schema-19 file with color_range = "full" and assert
    // Limited after load; then a schema-22 file with "full" stays Full.
    // (Port the bodies of MigrationV19_MaterializedFullBecomesLimited (:596)
    // and MigrationV20_ExplicitFullRespected (:628) onto the new API; also
    // rewrite MigrationV18_StillResets (:667) into
    // Migration_Schema18_FieldwiseRepair_AndColorRangeRewritten.)
}
```

Also rewrite the now-wrong reset-expectation tests: `SchemaV20_OneVersionBehindCurrent_Resets` (`:583`), `WrongSchemaVersion_ReturnsReset` (`:1315`), `OldSchemaVersion_Load_ReturnsReset` (`:1456`) — all become "keeps data, `repaired == true`". `InvalidDefaultId_*` (`:1203`, `:1227`) are deleted; `InvalidSelectedId_FallsBackToDefaultId` (`:1176`) becomes `InvalidSelectedId_FallsBackToDefaultPreset`. `EmptyPath_Load_SeedsDefault_NoCrash` (`:1295`) / `EmptyPath_Save_NoCrash` (`:1304`) keep their contract (empty path → defaults, no-op save); note these two are the ONLY tests allowed to use an empty path, and they exist purely for the no-crash contract — every other test must run against a real temp file so it exercises the same parse path `MainWindow.cpp:528` hits.

Registry test (2-arg `LoadState`), replacing `LoadState_InvalidDefaultFallsToFirst` (`:426`):

```cpp
// Case (c) of the spec: the selected preset vanished -> selection falls to
// Default; live config (held by MainWindow) is untouched and now compares
// against Default, typically showing (changed).
TEST(RecordingPresetRegistry, LoadState_SelectedMissing_FallsToDefault) {
    RecordingPresetRegistry reg;
    reg.LoadState({}, "preset.feedbeefdeadbeef");
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
}
```

- [ ] **Step 2: Run and see red**

```
pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset_store_tests|recording_preset_registry_tests"
```

Expected: compile failures on the new `Save`/`Load`/`LoadState` signatures (`error C2660: 'exosnap::RecordingPresetStore::Save': function does not take 3 arguments` once signatures shift), then assertion failures like `SchemaMismatch_KeepsData_FlagsRepaired` → `Expected: state.user_presets.size() ... Actual: 0` while the old full-reset path is still in place.

- [ ] **Step 3: Implement the store**

Mechanics in `RecordingPresetStore.cpp`:
1. Factor the config body out of `PresetToToml`/`PresetFromToml` (lines 565-704 / 708-1015):
   ```cpp
   toml::table ConfigToToml(const RecordingPresetConfig& config);           // capture/output/video/audio/webcam/countdown
   RecordingPresetConfig ConfigFromToml(const toml::table& tbl);            // field-wise, defaults for missing keys
   ```
   `PresetToToml` = `id` + `name` + merge of `ConfigToToml`; `PresetFromToml` = id/name guard + `ConfigFromToml`. Zero behavior change — round-trip tests prove it.
2. `Load()`: drop the version gate (`:1134-1138`). `repaired = (schema_version != kPresetSchemaVersion)`; `migrate_color_range = (schema_version >= 0 && schema_version <= kPresetSchemaColorRangeMigratedThrough)`. Parse presets as today but ALSO skip `IsBuiltInPresetId(raw.id)` items; parse `doc["live"].as_table()` → `SanitizePresetConfig(ConfigFromToml(...))` with the same color-range rewrite; selected repair: if `selected_id` is neither a built-in id nor in `user_presets`, set `kDefaultPresetId` (registry re-repairs anyway). Missing file / empty path / parse failure → `{ {}, kDefaultPresetId, nullopt, repaired }` (`repaired` true only for parse failure, not first run).
3. `Save()`: emit `schema_version = 23`, `selected_id`, `live = ConfigToToml(live)`, `presets` array filtered by `!IsBuiltInPresetId(p.id)`. Delete the `default_id` emplace (`:1237`) and `MakeResetState()` (`:1082-1089`, now unused).
4. `RecordingPreset.h`: bump to 23, rename the migration constant, extend the header comment block (`:17-37`) with a `v23` paragraph (live table, no default_id, field-wise repair policy).

- [ ] **Step 4: Implement registry + MainWindow + ConfigPage**

Registry: delete `default_id_`, `SetDefault`, `DefaultId`, the default-repair blocks in `LoadState` (`RecordingPresetRegistry.cpp:63-74`) and `DeleteSelected` (`:243-252`); `LoadState(user_presets, selected_id)` repairs selection straight to `kDefaultPresetId`. Update the invariants comment (`RecordingPresetRegistry.h:12-24`).

MainWindow startup block (`MainWindow.cpp:527-543`) becomes:

```cpp
    // ---- Load preset store (live config is the truth; presets are snapshots) ----
    PersistedPresetState loaded_presets = preset_store_.Load();
    preset_registry_.LoadState(std::move(loaded_presets.user_presets), loaded_presets.selected_id);
    if (loaded_presets.live.has_value()) {
        boot_live_config_ = SanitizePresetConfig(*loaded_presets.live);
    } else {
        // No readable live config: start on Default, not (changed).
        preset_registry_.SetSelected(std::string(kDefaultPresetId));
        boot_live_config_ = preset_registry_.SelectedSavedConfig();
    }
    if (loaded_presets.repaired) {
        preset_store_repaired_ = true;
        diagnostics::AppLog::warning(QStringLiteral("presets"),
                                     QStringLiteral("Preset store repaired field-wise on load"));
        preset_store_.Save(preset_registry_.Presets(), preset_registry_.SelectedId(), boot_live_config_);
    }

    // Initialize live mirrors from the restored live config.
    output_settings_ = boot_live_config_.output;
    video_settings_ = boot_live_config_.video;
    live_audio_ = boot_live_config_.audio;
    live_webcam_ = boot_live_config_.webcam;
```

Then:
- `applyPresetConfig(startup_cfg)` (`:1047`) → `applyPresetConfig(boot_live_config_)`; `coordinatorInitialized` re-apply (`:825-826`) → `applyPresetConfig(boot_live_config_)`; `buildConfigPage` re-apply (`:4369`) → `applyPresetConfig(captureLiveConfig())` (page rebuild after startup must reflect current live state, not the boot snapshot).
- `persistPresetState()` (`:2574-2576`) → `preset_store_.Save(preset_registry_.Presets(), preset_registry_.SelectedId(), captureLiveConfig());`
- Add `onLiveConfigChanged()` (dirty recompute + `schedulePersistLiveState()`), and replace every duplicated dirty-recompute block (`:753-789`, `:4144-4190`, `:4460-4470`, `:4510-4520`, `:4645-4655`) with a call to it:
  ```cpp
  void MainWindow::onLiveConfigChanged() {
      const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
      if (config_page_)
          config_page_->setPresetDirty(dirty);
      schedulePersistLiveState();
  }

  void MainWindow::schedulePersistLiveState() {
      if (!live_persist_timer_) {
          live_persist_timer_ = new QTimer(this);
          live_persist_timer_->setSingleShot(true);
          live_persist_timer_->setInterval(750); // coalesce slider drags into one write
          connect(live_persist_timer_, &QTimer::timeout, this, [this]() { persistPresetState(); });
      }
      live_persist_timer_->start();
  }
  ```
- `closeEvent` (`:2220`): before the existing shutdown work, flush: `if (live_persist_timer_ && live_persist_timer_->isActive()) { live_persist_timer_->stop(); persistPresetState(); }`
- `onPresetSelected` (`:2627-2641`) and `onResetChanges` (`:2695-2699`): wrap the applied config — `applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), captureLiveConfig()));` — and call `persistPresetState()` in both (a reset now changes the persisted live state).
- Delete `onSetDefaultPreset` (`:2713-2717`), its ConfigPage connect (`:2335` in ConfigPage.cpp wiring), and the overlay connect (`:800-801`). Delete `refreshPresetUi`'s `DefaultId()` usage (`:2562`).
- Kill the boot-to-default line `preset_registry_.SetSelected(preset_registry_.DefaultId());` (old `:536` — covered by the block above).

ConfigPage: `setPresetOptions` loses the `default_id` parameter and the star-suffix block (`ConfigPage.cpp:3585-3589`); remove `default_preset_id_`, `set_default_preset_action_`, `onSetDefaultPreset`, `setDefaultPresetRequested`, and the `is_default` logic in `updatePresetActionState` (`:3611`, `:3660-3662`). Update `test_config_page.cpp`: delete `SetDefaultPresetAction_*` (`:1541`, `:1565`) and `SetPresetOptions_MarkesDefaultWithStar_WhenNotSelected` (`:1321`); adjust every `setPresetOptions(...)` call to the 3-arg form. Update the visual scenarios `settings-preset-default` / `settings-preset-modified` (`VisualScenario.cpp:549-570`): drop `preset_default_name` and fix the injection call site (compiler-guided).

Notifications (`SettingsRepaired`): add the enum value in `NotificationEvent.h:13-20`; `kDismissMs_SettingsRepaired = 8000` + `DismissIntervalMs` case in `NotificationManager.{h,cpp}` (`:49-62`); tone/icon/title cases in the three `NotificationType` switches in `NotificationToastWindow.cpp` (`:141`, `:160`, `:188` — reuse the info tone of `UpdateAvailable`); no action buttons. In `MainWindow` after toast init (~`:3540`):

```cpp
    if (preset_store_repaired_) {
        preset_store_repaired_ = false;
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::SettingsRepaired;
        event.title = QStringLiteral("Settings repaired");
        event.body = QStringLiteral("Some saved recording settings were invalid and were reset to their defaults.");
        notification_manager_->Enqueue(std::move(event));
    }
```

- [ ] **Step 5: Green + commit**

```
pwsh scripts/run-tests.ps1 -Build -Filter "recording_preset_store_tests|recording_preset_registry_tests|config_page_tests|notification"
```

Then launch the app once (Qt bin on PATH) and confirm: change a setting, quit within a second, relaunch → the change survived (debounce flush). This live check is GUI-only and cannot be covered headless — say so in the task report.

```
git add -A && git commit -m "feat(presets): persist the live config and repair the store field-wise

presets.toml gains a [live] table restored on startup, so the app boots
into exactly the state it was closed in; presets are named snapshots and
built-ins are no longer written to disk. default_id is gone - startup no
longer selects a preset, so a startup-default marker has nothing left to
do. A schema mismatch or corrupted value now repairs field by field
instead of resetting everything (the color-range migration is kept), and
a repair raises a notification instead of failing silently."
```

---

## Task 4: ConfigPage preset row rework + `PresetManageOverlay` deletion

**Files:**
- Modify: `app/pages/ConfigPage.h`, `.cpp`
- Modify: `app/MainWindow.h`, `.cpp`
- Modify: `app/models/RecordingPresetRegistry.h`, `.cpp` (remove `AddDefaultPreset`, `DuplicateSelected`, `SaveSelected`, `ResetAllToDefault`)
- Delete: `app/ui/dialogs/PresetManageOverlay.h`, `.cpp`; `app/tests/test_preset_manage_overlay.cpp`
- Modify: `app/CMakeLists.txt` (drop the `preset_manage_overlay_tests` block `:1196-1212`; remove `PresetManageOverlay.cpp` from the app target's source list)
- Modify: `app/tests/test_config_page.cpp`, `test_recording_preset_registry.cpp`
- Modify: `app/ui/theme/exosnap_dark.qss` (dead `presetDirtyIndicator` rule `:1940`; check sibling theme files for the same selector)
- Modify: `app/visual_tests/VisualScenario.cpp` (scenario expectations follow the new row)

**Context (verified):**
- Toolbar build: `ConfigPage.cpp:688-820` (six controls + indicator); menu with nine actions `:762-785`; state logic `updatePresetActionState()` `:3610-3667`; handlers `:3669-3727`; connects `:2328-2353`.
- MainWindow handlers to delete: `onSavePreset` (`:2643`), `onNewPreset` (`:2661`), `onDuplicatePreset` (`:2668`), `onResetToDefaults` (`:2701`), `onExportAllUserProfiles` (`:2738`), `openPresetManageOverlay`/`refreshPresetManageOverlay` (`:2799-2809`, call sites `:2571`, `:2792`); overlay construction `:653`, connects `:793-809`, include `:29`, member `MainWindow.h:337`.
- `RecordingPresetStore::ExportAllUserPresetsToFile` survives until Task 6 (OutputPage still references export-all until then).
- ConfigPage has an inline save-error affordance for visual tests: `applyVisualPresetSaveError` (`ConfigPage.h:119-123`) — repurpose its copy for the name-collision state, do not delete.

**Interfaces:**
- ConfigPage keeps signals: `presetSelected(id)`, `savePresetAsRequested(name)`, `renamePresetRequested(name)`, `deletePresetRequested()`, `resetChangesRequested()`, `exportCurrentPresetRequested(path)`, `importPresetsRequested(path)`.
- ConfigPage deletes signals: `savePresetRequested`, `newPresetRequested`, `duplicatePresetRequested`, `resetToDefaultsRequested`, `managePresetsRequested`.
- Produces (ConfigPage, public static — unit-testable name validation used by both dialogs):
  ```cpp
  // True when `name` is empty after trimming or collides (trimmed,
  // case-insensitive) with any option label other than `exclude_id`'s.
  [[nodiscard]] static bool presetNameRejected(const QString& name,
                                               const std::vector<ProfileOption>& options,
                                               const QString& exclude_id);
  ```
- New widgets (stable objectNames for tests): `preset_save_as_btn_` → keeps `presetSaveAsButton`; new `preset_reset_btn_` (`presetResetButton`), new `preset_delete_btn_` (`presetDeleteButton`); `profile_overflow_btn_` keeps `presetManageButton` with text `…`; menu actions `save_preset_as_action_` ("Save as new…"), `rename_preset_action_` ("Rename…"), `export_preset_action_` ("Export…"), `import_presets_action_` ("Import…").
- Registry deletions: `AddDefaultPreset`, `DuplicateSelected`, `SaveSelected`, `ResetAllToDefault` (no remaining callers after this task — verify with grep before deleting).

- [ ] **Step 1: Write the failing UI tests**

Replace the preset-row tests in `test_config_page.cpp` (delete: `PresetSaveButton_HasStableObjectName` `:1275`, `PresetDirtyIndicator_HasStableObjectName` `:1285`, `SetPresetOptions_DirtyTrue_ShowsDirtyIndicatorAndEnablesSave` `:1376`, `SetPresetOptions_DirtyFalse_...` `:1394`, `SetPresetDirty_TogglesIndicatorAndSaveButton` `:1411`, `SaveButton_Click_EmitsSavePresetRequested` `:1524`; keep and adapt `ComboSelection_EmitsPresetSelected` `:1434`, `SetPresetOptionsDoesNotEmitPresetSelected` `:1454`, `SetRecordingControlsLocked_DisablesPresetSaveButtons` `:1469`). New tests — all exercise `setPresetOptions`/`setPresetDirty`, which is exactly what `MainWindow::refreshPresetUi()` (`MainWindow.cpp:2561`) and the dirty recompute call in production:

```cpp
// Spec rule 1: Save as new + Reset appear exactly when the live config is (changed).
TEST_F(ConfigPageTest, ChangedState_ShowsSaveAsNewAndReset) {
    ConfigPage page(OutputSettingsModel::Defaults(), VideoSettingsModel{});
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.default"), /*dirty=*/false);

    auto* save_as = page.findChild<QPushButton*>(QStringLiteral("presetSaveAsButton"));
    auto* reset = page.findChild<QPushButton*>(QStringLiteral("presetResetButton"));
    ASSERT_NE(save_as, nullptr);
    ASSERT_NE(reset, nullptr);
    EXPECT_FALSE(save_as->isVisibleTo(&page));
    EXPECT_FALSE(reset->isVisibleTo(&page));

    page.setPresetDirty(true);
    EXPECT_TRUE(save_as->isVisibleTo(&page));
    EXPECT_TRUE(reset->isVisibleTo(&page));
}

// Spec rule 2: Delete appears for a user preset regardless of (changed),
// and never for a built-in.
TEST_F(ConfigPageTest, DeleteButton_UserPresetOnly_IndependentOfChanged) {
    ConfigPage page(OutputSettingsModel::Defaults(), VideoSettingsModel{});
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});

    auto* del = [&] {
        page.setPresetOptions(opts, QStringLiteral("preset.abc"), /*dirty=*/false);
        return page.findChild<QPushButton*>(QStringLiteral("presetDeleteButton"));
    }();
    ASSERT_NE(del, nullptr);
    EXPECT_TRUE(del->isVisibleTo(&page)); // clean user preset is deletable

    page.setPresetOptions(opts, QStringLiteral("preset.default"), /*dirty=*/true);
    EXPECT_FALSE(del->isVisibleTo(&page)); // built-in: never
}

TEST_F(ConfigPageTest, OverflowMenu_HasExactlyFourActions_RenameDisabledForBuiltIn) {
    ConfigPage page(OutputSettingsModel::Defaults(), VideoSettingsModel{});
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.default"), false);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    ASSERT_NE(manage_btn->menu(), nullptr);
    QStringList texts;
    for (QAction* a : manage_btn->menu()->actions())
        if (!a->isSeparator())
            texts << a->text();
    EXPECT_EQ(texts, (QStringList() << QStringLiteral("Save as new…") << QStringLiteral("Rename…")
                                    << QStringLiteral("Export…") << QStringLiteral("Import…")));
    // Save as new stays reachable even when clean; Rename is built-in-gated.
    EXPECT_TRUE(manage_btn->menu()->actions().first()->isEnabled());
    for (QAction* a : manage_btn->menu()->actions())
        if (a->text() == QStringLiteral("Rename…"))
            EXPECT_FALSE(a->isEnabled());
}

// (changed) is a hint rendered in the combo text, not a warning widget.
TEST_F(ConfigPageTest, ChangedSuffix_AppendedToSelectedComboText) {
    ConfigPage page(OutputSettingsModel::Defaults(), VideoSettingsModel{});
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.default"), false);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default"));
    page.setPresetDirty(true);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default (changed)"));
    page.setPresetDirty(false);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default"));
}

// Removed affordances stay removed (guards against regressions).
TEST_F(ConfigPageTest, LegacyPresetControls_AreGone) {
    ConfigPage page(OutputSettingsModel::Defaults(), VideoSettingsModel{});
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetSaveButton")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetExportButton")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetImportButton")), nullptr);
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator")), nullptr);
}

// Production call site: the QInputDialog loops in onSavePresetAs/onRenamePreset
// validate through this exact predicate before emitting.
TEST_F(ConfigPageTest, PresetNameRejected_FoldsTrimsAndExcludesSelf) {
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Streaming"), false, false, true, {}});
    EXPECT_TRUE(ConfigPage::presetNameRejected(QStringLiteral("  streaming "), opts, QString()));
    EXPECT_TRUE(ConfigPage::presetNameRejected(QStringLiteral("default"), opts, QString()));
    EXPECT_TRUE(ConfigPage::presetNameRejected(QStringLiteral("   "), opts, QString()));
    EXPECT_FALSE(ConfigPage::presetNameRejected(QStringLiteral("Streaming"), opts, QStringLiteral("preset.abc")));
    EXPECT_FALSE(ConfigPage::presetNameRejected(QStringLiteral("Fresh"), opts, QString()));
}
```

- [ ] **Step 2: Run and see red**

```
pwsh scripts/run-tests.ps1 -Build -Filter config_page_tests
```

Expected first red: `error C2039: 'presetNameRejected': is not a member of 'exosnap::ConfigPage'`; after declaring, `ChangedState_ShowsSaveAsNewAndReset` fails with `Value of: save_as->isVisibleTo(&page)  Actual: true  Expected: false` (Save As is currently always visible).

- [ ] **Step 3: Implement ConfigPage**

Toolbar block (`ConfigPage.cpp:688-820`) rebuilt as: label · combo · `preset_save_as_btn_` ("Save as new…") · `preset_reset_btn_` ("Reset") · `preset_delete_btn_` ("Delete") · `profile_overflow_btn_` (text `…`, same objectName) · stretch · expert controls. Delete `preset_save_btn_`, `preset_export_btn_`, `preset_import_btn_`, `preset_dirty_indicator_`, `profile_status_label_` stays (built-in/unavailable badge). Menu:

```cpp
        auto* profile_menu = new QMenu(profile_overflow_btn_);
        save_preset_as_action_ = profile_menu->addAction(QStringLiteral("Save as new…"));
        rename_preset_action_ = profile_menu->addAction(QStringLiteral("Rename…"));
        profile_menu->addSeparator();
        export_preset_action_ = profile_menu->addAction(QStringLiteral("Export…"));
        import_presets_action_ = profile_menu->addAction(QStringLiteral("Import…"));
        profile_overflow_btn_->setMenu(profile_menu);
```

`updatePresetActionState()` becomes the two independent visibility rules plus the changed-suffix:

```cpp
void ConfigPage::updatePresetActionState() {
    const bool locked = controls_locked_;
    const bool user_preset = !active_preset_id_.isEmpty() && !active_preset_is_built_in_;

    if (preset_save_as_btn_) {
        preset_save_as_btn_->setVisible(preset_dirty_);
        preset_save_as_btn_->setEnabled(preset_dirty_ && !locked);
    }
    if (preset_reset_btn_) {
        preset_reset_btn_->setVisible(preset_dirty_);
        preset_reset_btn_->setEnabled(preset_dirty_ && !locked);
    }
    if (preset_delete_btn_) {
        preset_delete_btn_->setVisible(user_preset);
        preset_delete_btn_->setEnabled(user_preset && !locked);
    }
    if (save_preset_as_action_)
        save_preset_as_action_->setEnabled(!locked); // permanently reachable
    if (rename_preset_action_)
        rename_preset_action_->setEnabled(user_preset && !locked);
    if (export_preset_action_)
        export_preset_action_->setEnabled(!active_preset_id_.isEmpty());
    if (import_presets_action_)
        import_presets_action_->setEnabled(!locked);

    // "(changed)" hint in the combo text — informative, not a warning.
    if (profile_combo_) {
        const int idx = profile_combo_->currentIndex();
        if (idx >= 0 && idx < static_cast<int>(profile_options_.size())) {
            const QSignalBlocker blocker(profile_combo_);
            const QString base = profile_options_[static_cast<std::size_t>(idx)].label;
            profile_combo_->setItemText(idx, preset_dirty_ ? base + QStringLiteral(" (changed)") : base);
        }
    }
    // ... keep the existing profile_status_label_ badge block (:3620-3634) ...
}
```

Name-validating dialog loops (both dialogs; the collision is rejected inside the dialog, per spec):

```cpp
void ConfigPage::onSavePresetAs() {
    QString name = active_profile_name_;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Save as new preset"), QStringLiteral("Preset name:"),
                                     QLineEdit::Normal, name, &ok);
        if (!ok)
            return;
        if (!presetNameRejected(name, profile_options_, QString()))
            break;
        QMessageBox::warning(this, QStringLiteral("Save as new preset"),
                             QStringLiteral("That name is empty or already in use. Preset names are unique."));
    }
    emit savePresetAsRequested(name.trimmed());
}
```

(`onRenamePreset` mirrors it with `exclude_id = active_preset_id_`.) Implementation of the predicate (fold = trim + `toCaseFolded()`, Qt-side equivalent of the model's ASCII fold — labels come from the registry, which already enforced uniqueness, so the two folds agree on the data that can occur):

```cpp
bool ConfigPage::presetNameRejected(const QString& name, const std::vector<ProfileOption>& options,
                                    const QString& exclude_id) {
    const QString folded = name.trimmed().toCaseFolded();
    if (folded.isEmpty())
        return true;
    for (const auto& opt : options) {
        if (opt.id == exclude_id)
            continue;
        if (opt.label.trimmed().toCaseFolded() == folded)
            return true;
    }
    return false;
}
```

Wire: `preset_reset_btn_` → `resetChangesRequested`; `preset_delete_btn_` → existing `onDeletePreset` confirm flow (`:3697-3705`); `export_preset_action_` / `import_presets_action_` → the existing file-dialog lambdas (`:2342-2353`). Delete handlers/signals/members listed in Interfaces; delete `onManagePresets` and `managePresetsRequested`.

- [ ] **Step 4: Delete the overlay + MainWindow cleanup + registry pruning**

- Delete `app/ui/dialogs/PresetManageOverlay.{h,cpp}` and `app/tests/test_preset_manage_overlay.cpp`; remove the CMake test block (`app/CMakeLists.txt:1196-1212`) and the overlay source from the app target's source list (grep `PresetManageOverlay` in `app/CMakeLists.txt`).
- MainWindow: remove include (`:29`), member (`MainWindow.h:337`), construction (`:653`), connects (`:793-809`), `openPresetManageOverlay`/`refreshPresetManageOverlay` (`:2799-2809`) and their call sites (`:2571`, `:2792`, `:4215`), handlers `onSavePreset`, `onNewPreset`, `onDuplicatePreset`, `onResetToDefaults`, `onExportAllUserProfiles` (kept only if OutputPage still connects to it — it does until Task 6; if so, keep the handler and delete it in Task 6).
- Registry: delete `AddDefaultPreset`, `DuplicateSelected`, `SaveSelected`, `ResetAllToDefault` after `grep -r` confirms zero callers (OutputPage's duplicate/reset-all connects are also deleted here if they block compilation — otherwise Task 6). Delete their tests (`test_recording_preset_registry.cpp:54-75`, `:99-153`, `:284-299`).
- QSS: remove the `QLabel[labelRole="presetDirtyIndicator"]` rule (`exosnap_dark.qss:1940`) and any sibling-theme copy; keep `QToolButton#presetManageButton` (`:1909`).
- Visual scenarios `settings-preset-default`/`settings-preset-modified` (`VisualScenario.cpp:556-570`): expectations move from dirty-indicator/save-button to Save-as-new/Reset visibility.

- [ ] **Step 5: Green + visual check + commit**

```
pwsh scripts/run-tests.ps1 -Build -Filter "config_page_tests|recording_preset_registry_tests"
```

Launch the app (Qt bin on PATH) and verify on rendered pixels (memory: visual verification): clean built-in → only combo + `…`; edit a value → `Default (changed)` + Save as new + Reset; user preset clean → Delete visible. Then:

```
git add -A && git commit -m "feat(settings): shrink the preset row to a dropdown and a menu

The preset toolbar loses Save, Save As, Export, Import, the Unsaved
indicator, and the nine-entry overflow menu. What remains: the preset
dropdown (with a calm '(changed)' hint), contextual Save as new / Reset
buttons while the live config differs, Delete for user presets, and a
four-entry menu (Save as new, Rename, Export, Import). Name dialogs
reject duplicates inline. The separate preset manager overlay is removed
- the dropdown covers the expected preset counts."
```

---

## Task 5: Undo notification for preset switches

**Files:**
- Modify: `app/notifications/NotificationEvent.h`, `app/notifications/NotificationManager.h`, `.cpp`
- Modify: `app/ui/overlay/NotificationToastWindow.cpp`
- Modify: `app/MainWindow.h`, `.cpp`
- Modify: `app/tests/test_notification_toast.cpp`, `app/tests/test_recording_preset_registry.cpp`

**Context (verified):**
- The notification pipeline is enum-typed end to end: `NotificationType`/`NotificationAction` (`NotificationEvent.h:13-39`), per-type dwell in `NotificationManager::DismissIntervalMs` (`NotificationManager.cpp:49-62`), per-action button labels in `NotificationToastWindow.cpp:318-356`, per-type tone/icon in three switches (`:141`, `:160`, `:188`), and the action dispatch switch `MainWindow::dispatchNotificationAction` (`MainWindow.cpp:3721-3816`) reached via the `actionTriggered` connect (`:3546`). **There is no generic action-callback notification** — the smallest honest implementation is one new type + one new action threaded through these five places. Do not build a callback API for one consumer.
- The undo payload (a full `RecordingPresetConfig`) cannot ride in `NotificationEvent::action_payload` (QString) — MainWindow holds it as state; a newer switch simply overwrites it (single-slot undo, latest switch wins).

**Interfaces:**
- Produces (`NotificationEvent.h`): `NotificationType::PresetSwitched`, `NotificationAction::UndoPresetSwitch`.
- Produces (`NotificationManager.h`): `static constexpr int kDismissMs_PresetSwitched = 8000;` + `DismissIntervalMs` case.
- Produces (`MainWindow.h`):
  ```cpp
  struct PresetSwitchUndo {
      RecordingPresetConfig previous_live;
      std::string previous_selected_id;
  };
  std::optional<PresetSwitchUndo> pending_preset_undo_;
  ```

- [ ] **Step 1: Write the failing tests**

`test_notification_toast.cpp` (production call site: `MainWindow::onPresetSelected` enqueues; `NotificationToastWindow` renders the button; the click routes through `actionTriggered` → `dispatchNotificationAction`):

```cpp
TEST_F(NotificationToastTest, PresetSwitched_ShowsUndoPrimaryButton) {
    notifications::NotificationEvent event;
    event.type = notifications::NotificationType::PresetSwitched;
    event.title = QStringLiteral("Switched to 'Quality'");
    event.action = notifications::NotificationAction::UndoPresetSwitch;
    // Follow the file's existing pattern for resolving button specs from an
    // event (same helper the OpenFolder/Edit cases use) and assert:
    //   - exactly one primary button labeled "Undo"
    //   - it carries NotificationAction::UndoPresetSwitch
}

TEST(NotificationManager, PresetSwitched_AutoDismissesAfter8s) {
    EXPECT_EQ(notifications::NotificationManager::DismissIntervalMs(
                  notifications::NotificationType::PresetSwitched),
              8000);
}
```

`test_recording_preset_registry.cpp` (production call site: the `UndoPresetSwitch` case in `MainWindow::dispatchNotificationAction` calls `SetSelected(previous_selected_id)` with a Default fallback):

```cpp
// Undo restores the previous selection; when that preset has since been
// deleted, the selection falls to Default (the fallback the dispatch uses).
TEST(RecordingPresetRegistry, UndoSelectionFallback_MissingPrevious_FallsToDefault) {
    RecordingPresetRegistry reg;
    const std::string mine = reg.AddPreset(MakeDefaultPreset().config, "Mine");
    ASSERT_TRUE(reg.SetSelected(std::string(kQualityPresetId)));
    ASSERT_TRUE(reg.SetSelected(mine)); // undo target exists -> restored
    ASSERT_TRUE(reg.DeleteSelected());  // now it is gone; selection sits on Default
    EXPECT_FALSE(reg.SetSelected(mine));
    EXPECT_EQ(reg.SelectedId(), kDefaultPresetId);
}
```

- [ ] **Step 2: Run and see red**

```
pwsh scripts/run-tests.ps1 -Build -Filter "notification|recording_preset_registry_tests"
```

Expected: `error C2065: 'PresetSwitched': undeclared identifier` in the toast test; after adding the enum value only, the manager test fails with `Expected equality ... DismissIntervalMs(...) Which is: 0 ... 8000` (unhandled case falls through to sticky).

- [ ] **Step 3: Implement**

Enum + interval + toast mapping (info tone like `UpdateAvailable`; button case):

```cpp
    case NotificationAction::UndoPresetSwitch:
        buttons.push_back({QStringLiteral("Undo"), true, NotificationAction::UndoPresetSwitch});
        break;
```

`MainWindow::onPresetSelected` (post-Task 3 shape) gains the snapshot + toast:

```cpp
void MainWindow::onPresetSelected(const QString& id) {
    if (syncing_preset_ui_)
        return;
    if (id.toStdString() == preset_registry_.SelectedId())
        return; // combo refresh echo — not a switch
    if (!record_page_ || !record_page_->canApplyPresetNow()) {
        refreshPresetUi();
        diagnostics::AppLog::warning(QStringLiteral("preset"),
                                     QStringLiteral("preset switch rejected: recording in progress"));
        return;
    }

    PresetSwitchUndo undo;
    undo.previous_live = captureLiveConfig();
    undo.previous_selected_id = preset_registry_.SelectedId();

    if (!preset_registry_.SetSelected(id.toStdString()))
        return;
    pending_preset_undo_ = std::move(undo);

    applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), captureLiveConfig()));
    persistPresetState();

    if (notification_manager_) {
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::PresetSwitched;
        event.title = QStringLiteral("Switched to '%1'")
                          .arg(QString::fromStdString(preset_registry_.SelectedPreset().name));
        event.action = notifications::NotificationAction::UndoPresetSwitch;
        notification_manager_->Enqueue(std::move(event));
    }
}
```

Dispatch case in `dispatchNotificationAction` (`MainWindow.cpp:3721`):

```cpp
    case NotificationAction::UndoPresetSwitch: {
        if (!pending_preset_undo_)
            break;
        const PresetSwitchUndo undo = std::move(*pending_preset_undo_);
        pending_preset_undo_.reset();
        if (!preset_registry_.SetSelected(undo.previous_selected_id))
            preset_registry_.SetSelected(std::string(kDefaultPresetId));
        applyPresetConfig(undo.previous_live); // restores live state AND environment
        persistPresetState();
        break;
    }
```

Honest limitation to state in the task report: the end-to-end click path (toast button → `actionTriggered` → dispatch → pages update) is GUI-only; headless coverage stops at (a) the button spec, (b) the dwell time, (c) the registry fallback, (d) `WithEnvironmentFields` (Task 1). Verify the full loop once by hand: switch preset → toast appears → Undo → previous values AND previous selection are back.

- [ ] **Step 4: Green + commit**

```
pwsh scripts/run-tests.ps1 -Build -Filter "notification|recording_preset_registry_tests|config_page_tests"
```

```
git add -A && git commit -m "feat(presets): switching presets offers Undo instead of asking first

A preset switch applies immediately - no confirmation dialog - and raises
a toast with an Undo action that restores both the previous live settings
and the previously selected preset. This is the single place in the new
model where work could be lost, so it is the single place with a safety
net."
```

---

## Task 6: Align OutputPage; retire `ExportAllUserPresetsToFile`; guard review

**Files:**
- Modify: `app/pages/OutputPage.h`, `.cpp`
- Modify: `app/MainWindow.h`, `.cpp` (connect block `:4683-4697`; delete `onExportAllUserProfiles` `:2738-2761`; `syncing_preset_ui_` removal `MainWindow.h:431`)
- Modify: `app/settings/RecordingPresetStore.h`, `.cpp` (delete `ExportAllUserPresetsToFile`, `:1289-1315`)
- Modify: `app/tests/test_preset_export_import.cpp`
- Create: `app/tests/test_output_page.cpp`; register `output_page_tests` in `app/CMakeLists.txt`

**Context (verified):**
- OutputPage today: own toolbar with 3 buttons + 9-entry menu (`OutputPage.cpp:84-104`), own visibility model (`updateProfileActionState`, `:202-235` — Save-as-new only for *modified built-ins*, which contradicts the new rules), and `setProfileOptions` **emits `activeProfileChanged` during sync** via the trailing `onProfileSelectionChanged(...)` call (`OutputPage.cpp:165` → emit at `:199`). That re-entrant emit is the reason `syncing_preset_ui_` exists in MainWindow (`refreshPresetUi`, `MainWindow.cpp:2559-2569`; guard read in `onPresetSelected` `:2628`).
- ConfigPage's sync path is already safe without the flag: `setPresetOptions` wraps the combo in `QSignalBlocker` (`ConfigPage.cpp:3580`).
- `ExportAll_ImportAll_CountAndConfigEqual` (`test_preset_export_import.cpp:103`) is the main consumer of `ExportAllUserPresetsToFile`; `ImportPresetsFromFile` keeps multi-item support (files may still contain several presets).

**Interfaces:**
- OutputPage keeps: `setProfileOptions(options, active_id, modified)`, `activeProfileChanged(id)`, `renameActiveProfileRequested(name)`, `deleteActiveProfileRequested()`, `resetActiveProfileRequested()`, `importProfilesRequested(path)`, `exportSelectedProfileRequested(path)`.
- OutputPage replaces `newFromCurrentRequested` + `saveModifiedBuiltInAsNewRequested` with one `saveAsNewRequested(const QString& name)`; deletes `newFromSafeDefaultRequested`, `duplicateActiveProfileRequested`, `exportAllUserProfilesRequested`, `resetAllSettingsAndProfilesRequested`.
- MainWindow connects `saveAsNewRequested` → `onSavePresetAs` (same handler as ConfigPage).
- `syncing_preset_ui_` is deleted; `applying_preset_` is **kept** with a sharpened comment (see Risks — it protects `live_audio_` from the kind-default rows `applyCapturePolicy` emits mid-apply, `MainWindow.cpp:2500-2510`).

- [ ] **Step 1: Write the failing tests**

`app/tests/test_output_page.cpp` (new target mirrors the deleted overlay target's CMake pattern, `app/CMakeLists.txt` — model on the old block at `:1196-1212` with `SOURCES tests/test_output_page.cpp pages/OutputPage.cpp models/OutputSettingsModel.cpp ui/theme/ExoSnapTheme.cpp ...` plus a `QApplication` fixture, memory: gtest isolation):

```cpp
// Production call site: MainWindow::refreshPresetUi() -> setProfileOptions.
// The sync path must not emit: MainWindow drops its re-entrancy guard on the
// strength of this exact guarantee.
TEST_F(OutputPageTest, SetProfileOptions_DoesNotEmitActiveProfileChanged) {
    OutputPage page{OutputSettingsModel::Defaults()};
    int emitted = 0;
    QObject::connect(&page, &OutputPage::activeProfileChanged, [&](const QString&) { ++emitted; });
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});
    page.setProfileOptions(opts, QStringLiteral("preset.abc"), false);
    EXPECT_EQ(emitted, 0);
}

// Same two visibility rules as the Settings row.
TEST_F(OutputPageTest, TwoRules_SaveAsNewResetOnChanged_DeleteOnUserPreset) {
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});

    auto* save_as = page.findChild<QPushButton*>(QStringLiteral("outputPresetSaveAsButton"));
    auto* reset = page.findChild<QPushButton*>(QStringLiteral("outputPresetResetButton"));
    auto* del = page.findChild<QPushButton*>(QStringLiteral("outputPresetDeleteButton"));
    ASSERT_NE(save_as, nullptr); ASSERT_NE(reset, nullptr); ASSERT_NE(del, nullptr);

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*modified=*/false);
    EXPECT_FALSE(save_as->isVisibleTo(&page));
    EXPECT_FALSE(reset->isVisibleTo(&page));
    EXPECT_FALSE(del->isVisibleTo(&page));

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*modified=*/true);
    EXPECT_TRUE(save_as->isVisibleTo(&page));   // built-in + changed: Save as new + Reset
    EXPECT_TRUE(reset->isVisibleTo(&page));
    EXPECT_FALSE(del->isVisibleTo(&page));

    page.setProfileOptions(opts, QStringLiteral("preset.abc"), /*modified=*/false);
    EXPECT_TRUE(del->isVisibleTo(&page));       // clean user preset: Delete only
    EXPECT_FALSE(save_as->isVisibleTo(&page));
}

TEST_F(OutputPageTest, OverflowMenu_HasExactlyFourActions) {
    // Mirror of the ConfigPage menu test: Save as new… / Rename… / Export… / Import…,
    // Rename disabled while a built-in is selected.
}
```

`test_preset_export_import.cpp`: rewrite `ExportAll_ImportAll_CountAndConfigEqual` (`:103`) to hand-write a multi-item TOML document (two `[[presets]]` tables at `schema_version = 23`) and assert `ImportPresetsFromFile` returns both — multi-import stays supported without the export-all writer.

- [ ] **Step 2: Run and see red**

```
pwsh scripts/run-tests.ps1 -Build -Filter "output_page_tests|preset_export_import_tests"
```

Expected: `SetProfileOptions_DoesNotEmitActiveProfileChanged` → `Expected equality ... emitted Which is: 1 ... 0`; the two-rules test fails on the missing objectNames (`save_as == nullptr`).

- [ ] **Step 3: Implement**

OutputPage: give the three buttons the objectNames above; rebuild the menu to the four entries; `updateProfileActionState()` adopts the ConfigPage rules verbatim (`modified` drives Save-as-new/Reset; `!built_in` drives Delete/Rename). Split `setProfileOptions` so sync only *renders*:

```cpp
void OutputPage::setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                                   bool active_profile_modified) {
    profile_options_ = options;
    active_profile_is_modified_ = active_profile_modified;

    QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    for (const auto& option : profile_options_)
        profile_combo_->addItem(option.label, option.id);
    const int index = profile_combo_->findData(active_profile_id);
    if (index >= 0)
        profile_combo_->setCurrentIndex(index);
    applySelectionState(profile_combo_->currentIndex()); // render only — NO emit
}

void OutputPage::onProfileSelectionChanged(int index) { // user interaction path only
    if (index < 0 || index >= static_cast<int>(profile_options_.size()))
        return;
    applySelectionState(index);
    emit activeProfileChanged(profile_options_[static_cast<std::size_t>(index)].id);
}
```

Save-as-new prompt: one `promptSaveAsNew()` (validating against `profile_options_` labels with the same trimmed/case-folded predicate — reuse `ConfigPage::presetNameRejected` by including `ConfigPage.h`, or duplicate the 10-line predicate locally to avoid the page cross-include; prefer the local copy and note it) → `emit saveAsNewRequested(name)`.

MainWindow: update the connect block (`:4683-4697`) — `saveAsNewRequested` → `onSavePresetAs`; delete the connects for the removed signals; delete `onExportAllUserProfiles` (`:2738-2761`). Store: delete `ExportAllUserPresetsToFile` (decl `RecordingPresetStore.h:72-73`, impl `:1289-1315`); drop the export-all mention from the header comment (`:55-62`).

Guard review (explicit step, per spec "prüfen, nicht blind löschen"):
- `syncing_preset_ui_`: with ConfigPage blocking combo signals (`ConfigPage.cpp:3580`) and OutputPage no longer emitting during sync (test above), the guard has no remaining trigger — delete it (`MainWindow.h:431`, writes `:2559/:2569`, read `:2628`).
- `applying_preset_`: KEEP. During `applyPresetConfig`, `record_page_->applyCapturePolicy()` can rebuild audio rows and emit `audioSettingsChanged` with kind-default rows before `applyPersistedAudioSettings` runs (ordering note at `MainWindow.cpp:2500-2510`); without the guard those intermediate emits would clobber `live_audio_` and — new in this plan — trigger spurious live-config persistence mid-apply. Replace the stale comment "handlers early-return while applying_preset_" rationale with this one.

- [ ] **Step 4: Green + commit**

```
pwsh scripts/run-tests.ps1 -Build -Filter "output_page_tests|preset_export_import_tests|config_page_tests|recording_preset_store_tests"
```

```
git add -A && git commit -m "feat(output): align the Output page preset row with Settings

The Output page preset entry now behaves exactly like the Settings row:
one dropdown, contextual Save as new / Reset while changed, Delete for
user presets, and a four-entry menu. Export always works on the selected
preset, so the export-all path is retired; multi-preset files still
import. One redundant UI-sync guard is removed now that neither page
emits selection changes while being refreshed."
```

---

## Task 7: Update `docs/product-spec.md`

**Files:**
- Modify: `docs/product-spec.md`

**Context (verified):** §3 (`docs/product-spec.md:72-94`) carries the defaults table and the paragraph at `:92-94` describing "A preset manage dialog supports rename, duplicate, delete, and set-default." — the dialog, duplicate, and set-default no longer exist.

- [ ] **Step 1: Replace the preset paragraph and add the built-ins table**

Replace lines 92-94 with:

```markdown
Four read-only built-in presets ship with the app. They cannot be renamed, overwritten, or
deleted; **Save as new** derives a user preset from any of them.

| Preset | Container | Codecs | CQ | NVENC preset | Intent |
|--------|-----------|--------|----|--------------|--------|
| Default | MKV | AV1 + Opus | 19 | P4 | balanced |
| Quality | MKV | AV1 + Opus | 16 | P6 | maximum sharpness; costs disk and GPU |
| Efficiency | MKV | AV1 + Opus | 30 | P6 | small files at usable quality |
| Compatibility | MP4 | H.264 + AAC | 19 | P4 | editing, upload, GPUs without AV1 encode |

The **live configuration is the source of truth** and is persisted silently and continuously;
the app restarts into exactly the state it was closed in. A preset is a named snapshot the
live configuration is compared against: when they differ, the selector shows `Name (changed)`
as a calm hint — there is no Save button, no unsaved-changes warning, and no discard dialog.
Capture identity, video bit depth, and HDR mode are *environment* facts: presets neither set
nor override them, and they never count as changes (the existing H.264 8-bit clamp is the one
sanctioned exception).

The preset row offers a dropdown plus a `…` menu (*Save as new…*, *Rename…* — disabled for
built-ins —, *Export…*, *Import…*). While `(changed)`, contextual **Save as new** and **Reset**
buttons appear; while a user preset is selected, **Delete** appears (regardless of changes).
Switching presets applies immediately; a notification offers **Undo**, which restores both the
previous live configuration and the previous selection. Preset names are unique
(trimmed, case-insensitive; built-in names reserved): the naming dialogs reject collisions,
imports resolve them with a numeric suffix (`name (2)`).

Presets are stored in a human-readable TOML store and can be exported and imported for
sharing. Presets are validated and sanitized before storage; invalid values are clamped rather
than rejected silently, and a damaged store is repaired field by field (a repair raises a
notification) instead of being reset wholesale.
```

- [ ] **Step 2: Sweep the rest of the spec**

Grep `docs/product-spec.md` for `manage dialog`, `set-default`, `duplicate`, `Unsaved` and reconcile any other mention with the new model (line 8's pre-1.0 schema note stays; the defaults table `:76-90` stays valid).

- [ ] **Step 3: Commit**

```
git add -A && git commit -m "docs(spec): describe the simplified preset model

Live config as persisted truth, '(changed)' as a hint, four read-only
built-ins, the two-rule preset row, undo on switch, unique names, and
field-wise store repair replace the old manage-dialog description."
```

---

## Final gate (after Task 7)

- [ ] `git diff --check`
- [ ] Full build of the tree (not `--target exosnap`)
- [ ] `pwsh scripts/run-tests.ps1` (full suite; use `-ExcludeLabel live` on a no-GPU host)
- [ ] Launch the app once: theme loads (QSS validity), preset row renders, switch → Undo round-trip, restart restores live state
- [ ] `grep -r "default_id\|PresetManageOverlay\|ExportAllUserPresetsToFile\|syncing_preset_ui_\| (imported)\|managePresetsRequested" app/ docs/product-spec.md` → only historical ADR/comment references may remain

---

## Self-Review

### Spec coverage matrix

| Spec point | Covered by |
|---|---|
| 1. Live config = truth, silent persistence, `(changed)` hint, no Save/discard | Task 3 (persistence + boot-to-live + debounce), Task 4 (`(changed)` suffix, Save/indicator removal) |
| 2. `bit_depth`/`hdr_mode` as environment fields (not applied, not compared, not set) | Task 1 (compare + helpers), Task 2 (`AddPreset` strips), Task 3/5 (apply paths use `WithEnvironmentFields`) |
| 3. Switch without confirmation + Undo notification (live + selection restored) | Task 5 |
| 4. Four read-only built-ins with the specified values | Task 2 |
| 5. Unique names (trimmed, case-insensitive, reserved built-ins); dialogs reject; import suffixes `(2)` | Task 2 (model/registry/store), Task 4 (ConfigPage dialogs), Task 6 (OutputPage dialog) |
| 6. Dropdown + `…` menu; Save as new/Reset on changed; Delete on user preset; 4 menu entries; Reset = back to selected preset | Task 4 (ConfigPage), Task 6 (OutputPage) |
| 7. `PresetManageOverlay` + its test deleted; OutputPage aligned | Task 4 (deletion), Task 6 (alignment) |
| 8. `[live]` table; `selected_id` stays; `default_id` gone (store+registry+action); schema 22→23 | Task 3 |
| 9. Field-wise repair; ADR-0032 migration kept; cases (a)/(b)/(c); repair notification | Task 3 (store tests for a/b, registry test + MainWindow for c, `SettingsRepaired` toast) |
| 10. Removed elements (Save/Save As/Export/Import buttons, `· Unsaved`, 5 menu entries, Duplicate, Set-default, New-from-default, Reset-all, `ExportAllUserPresetsToFile`, guards) | Tasks 3 (set-default), 4 (toolbar/menu/handlers/registry ops), 6 (`ExportAllUserPresetsToFile`, `syncing_preset_ui_`; `applying_preset_` deliberately kept — see Risks) |
| 11. `ProfileOption.built_in` activated + registry enforcement | Task 2 |
| 12. `docs/product-spec.md` §3 replaced, built-ins added | Task 7 |

### Placeholder scan
No "TBD"/"handle edge cases"/forward references. Two intentionally summarized test bodies exist and each states exactly what to assert and which existing test to port: `ColorRangeMigration_AppliesThroughSchema19_Only` (Task 3 — ports `:596`/`:628`/`:667` bodies onto the new API) and `OverflowMenu_HasExactlyFourActions` for OutputPage (Task 6 — explicit mirror of the fully-written ConfigPage version in Task 4). The `store_or(path)` token in one Task 3 test carries its expansion in the trailing comment on the same line.

### Type consistency
- `Save(const std::vector<RecordingPreset>&, const std::string&, const RecordingPresetConfig&)` matches the `persistPresetState()` call (`Presets()` → `const std::vector<RecordingPreset>&`, `SelectedId()` → `const std::string&`, `captureLiveConfig()` → `RecordingPresetConfig`).
- `PersistedPresetState::live` is `std::optional<RecordingPresetConfig>`; every startup branch handles both states.
- `LoadState(std::vector<RecordingPreset>, std::string)` — the Task 2 test still uses the 3-arg form (Task 2 predates the signature change); Task 3 updates that test when the signature shifts, which the Task 3 red-step compile error enforces.
- `ConfigPage::ProfileOption` aggregate init `{id, label, built_in, modified, available, availability_reason}` matches the declaration order (`ConfigPage.h:57-64`); same for `OutputPage::ProfileOption` (`OutputPage.h:19-26`).
- `WithEnvironmentFields`/`StripEnvironmentFields` take/return `RecordingPresetConfig` by value, matching the existing sanitizer style (`SanitizePresetConfig`, `RecordingPreset.h:127`).
- New enum entries extend `uint8_t`-backed enums (`NotificationEvent.h:13`, `:28`) — no ABI concern; all `switch`es over `NotificationType` listed in Task 3/5 get cases, keeping `-Wswitch`-style completeness.

### Known risks / deviations found during code reading
1. **No action-notification API exists** — the notification pipeline is closed-enum end to end (`NotificationEvent.h:13-39`, button mapping `NotificationToastWindow.cpp:318-356`, dispatch `MainWindow.cpp:3721`). The plan threads two new types + one action through the five fixed touch points instead of inventing a callback API; the undo payload lives in `MainWindow::pending_preset_undo_` because `action_payload` is a `QString`.
2. **`applying_preset_` must stay** (deviation from spec item 10's optimistic reading): during `applyPresetConfig`, `applyCapturePolicy` can rebuild audio rows and emit kind-default `audioSettingsChanged` before the preset's rows are pushed (`MainWindow.cpp:2500-2510`); without the guard the new debounced live-persistence would also fire mid-apply. Only `syncing_preset_ui_` is removable, and only after Task 6 stops OutputPage emitting during sync (`OutputPage.cpp:165→199` is the current trigger).
3. **Treating capture as switch-preserved is a real behavior change**: today a preset switch applies the preset's stored capture target (`MainWindow.cpp:2508`); the spec's environment-field logic ("Ein Preset-Wechsel überschreibt sie nicht" for fields *in the same category as capture*) implies switching must stop retargeting capture. The plan implements that via `WithEnvironmentFields` — flag it in the PR description for explicit product sign-off. Related observation, out of scope: `MakeDefaultPreset()`'s audio rows (`RecordingPreset.cpp:119-122`: SystemOutput enabled, Mic disabled, no App row) do not match the product-spec defaults table ("APP, SYS, MIC — all enabled", `product-spec.md:89`); pre-existing, untouched here, worth a follow-up.
