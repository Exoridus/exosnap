# Settings Page Visual Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the visual and functional defects on the Quick Settings page found in live review after the Widgets-to-Quick cutover, verified against the current build's own `--visual-test` captures and the pre-cutover Widgets screenshots in `.workspace/visual-reference/main-app/widgets/`.

**Architecture:** No new components except reusing the already-existing `ExoChevron`. Most fixes are targeted QML property/layout corrections in the affected section files; a few are C++ (the hotkey default removal and its stale-conflict-warning follow-up, both in `SettingsAdapter.cpp`/`GlobalHotkeyService.cpp`). Each task is independently buildable and visually verifiable via `--visual-test`.

**Tech Stack:** Qt 6 / Qt Quick (QML), `SettingsAdapter` (C++ adapter layer), the project's `--visual-test` harness.

**Spec:** Mostly a fidelity/polish pass against the existing Quick Settings design intent, not a behavior change — with one deliberate exception: Task 6 removes the Alt+F9 factory default (a real product decision, confirmed with the user 2026-08-19) and updates `docs/product-spec.md` §10 in the same task.

## Global Constraints

- QML: ASCII punctuation in comments, no dev-provenance references (no QCR numbers, no ticket IDs) per `AGENTS.md`.
- Every visual change gets confirmed with a fresh `--visual-test` capture before being called done, not just reasoned about from source.
- Preserve every feature the cutover *added* on top of the old Widgets design (the per-source level meters, the per-row undo the old Widgets Hotkeys card never had) — except where a task explicitly redesigns something with the user's confirmation (Tasks 6-7 replace the Reset/Clear button pair with a dynamic Set/Change/x set — this was reviewed and approved, not an oversight).
- Build: `cmake --build build/windows-x64-ninja-debug --target exosnap` (or the existing configured debug tree). qmllint via `all_qmllint` target before calling a QML task done.
- **MSVC environment on this machine:** a bare shell has no `INCLUDE`/`LIB` — `cl.exe` fails with `fatal error C1083: ... "type_traits": No such file or directory`. `vcvarsall.bat` itself is broken here too (`VSINSTALLDIR` missing its trailing backslash breaks `VsDevCmd.bat`'s internal `ext\vcvars.bat`). The working invocation, verified this session:
  ```
  cmd /c "set `"VSINSTALLDIR=C:\Program Files\Microsoft Visual Studio\18\Community\`" & call `"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 -no_logo > nul 2>&1 & <build command>"
  ```
  Use `&` between the setup and the real command, never `&&` — the VS-bundled `cmake.bat`/`ConnectionManagerExe.bat` report a harmless `init:FAILED` that makes `VsDevCmd.bat` exit 1 even though the environment it set up is fine.

---

## Root cause A (shared by Tasks 1-2): trailing compact controls don't right-align in `ExoSettingRow`

**Evidence:** `settings-full.png` (this session's capture) — `Include webcam`/`Mirror image`/`Chroma key` switches end at x=1332, the row's own right edge (`Camera` combobox, the webcam warning banner) ends at x=1352, a consistent ~20px shortfall. The `Recording overlay content` segmented control ends at x=1278 against the same x=1352 edge, a ~74px shortfall. The Audio card's `Merge with above` checkbox ends at x=693 against the card's own x=761 (`Audio bitrate`, `Channels` row edges).

**Cause:** `ExoSettingRow.qml`'s `controlHost` (a `ColumnLayout`) is itself right-aligned in the row's `GridLayout` (`Layout.alignment: ... Qt.AlignRight`), but that only positions the *box*. `ExoSwitch`, `ExoCheckBox` and `ExoSegmentedControl` are all content-sized (`implicitWidth` = their own glyph/label, never `Layout.fillWidth`), and none of their call sites sets `Layout.alignment: Qt.AlignRight` on the control itself. A `ColumnLayout` child with no explicit alignment defaults to `Qt.AlignLeft`, so the control sits flush against the *left* edge of its own right-aligned box, leaving the gap between the control and the card's true right edge that every one of the three screenshots shows.

**Fix (per call site, not a shared default):** add `Layout.alignment: Qt.AlignRight | Qt.AlignVCenter` to each affected control. Rejected the alternative of defaulting `controlHost`'s children to right-alignment inside `ExoSettingRow.qml` itself (a loop over `controlHost.children` in `Component.onCompleted`) — it would apply to every current and future control type the row ever hosts, including ones that legitimately want left alignment, and a silent behavior change to a shared row primitive is a bigger blast radius than fifteen explicit one-line additions.

### Task 1: Right-align switches and segmented controls in Overlays, Output, Format/Quality, Presence and Audio sections

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsOverlaysSection.qml` (3 `ExoSwitch`, 2 `ExoSegmentedControl`)
- Modify: `app/quick/ExoSnap/Quick/SettingsOutputSection.qml` (2 `ExoSwitch`, at `Split recording` and `Split by size`)
- Modify: `app/quick/ExoSnap/Quick/SettingsAudioSection.qml` (`ExoSwitch` at `Brickwall limiter` and `A/V clock slaving`)
- Modify: `app/quick/ExoSnap/Quick/SettingsAppearanceSection.qml` (`ExoSegmentedControl` at `Appearance`, the accent swatch `Row` at `Accent`)
- Check (same pattern, confirm and fix if present): `app/quick/ExoSnap/Quick/SettingsPresenceSection.qml`, `SettingsWebcamSection.qml`, `SettingsQualitySection.qml`, `SettingsUpdatesSection.qml`

**Interfaces:** None — pure QML property additions, no signal/property renames.

- [ ] **Step 1: Add `Layout.alignment: Qt.AlignRight | Qt.AlignVCenter` to every `ExoSwitch` used as a settings-row trailing control**

Example (`SettingsOverlaysSection.qml`, `Recording overlay` row):

```qml
ExoSwitch {
    checked: root.settings.showRecordingOverlay
    Accessible.name: qsTr("Recording overlay")
    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
    onToggledByUser: value => root.settings.showRecordingOverlay = value
}
```

Repeat for the other five `ExoSwitch` instances named above (three more in `SettingsOverlaysSection.qml`: `Diagnostics overlay`, `Quick control pill`; two in `SettingsOutputSection.qml`; two in `SettingsAudioSection.qml`).

- [ ] **Step 2: Add the same alignment to every `ExoSegmentedControl` and the `Accent` swatch `Row`**

`SettingsOverlaysSection.qml`, both segmented controls:

```qml
ExoSegmentedControl {
    options: root.settings.recordingOverlayPresetOptions.map(option => option.label)
    Accessible.name: qsTr("Recording overlay content")
    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
    currentIndex: root.settings.recordingOverlayPresetOptions
                      .findIndex(option => option.value === root.settings.recordingOverlayPreset)
    onSelected: index => root.settings.recordingOverlayPreset =
                    root.settings.recordingOverlayPresetOptions[index].value
}
```

Same one-line addition on the `Diagnostics overlay content` segmented control, and on `SettingsAppearanceSection.qml`'s `Appearance` segmented control and the `Accent` row (`Row { ... Layout.alignment: Qt.AlignRight | Qt.AlignVCenter }`).

- [ ] **Step 3: Build and capture**

```
cmake --build build/windows-x64-ninja-debug --target exosnap
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a1.png --visual-page 1 --visual-appearance dark --visual-accent aqua --visual-test-size 1600x2600
```

Confirm every switch/segmented control now ends flush with the combobox/banner edges above and below it in the same card.

- [ ] **Step 4: qmllint**

```
cmake --build build/windows-x64-ninja-debug --target all_qmllint
```

Expected: no new findings.

- [ ] **Step 5: Commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsOverlaysSection.qml app/quick/ExoSnap/Quick/SettingsOutputSection.qml app/quick/ExoSnap/Quick/SettingsAudioSection.qml app/quick/ExoSnap/Quick/SettingsAppearanceSection.qml
git commit -m "fix(quick): right-align trailing switches and segmented controls in Settings"
```

### Task 2: Right-align "Merge with above" in the audio source row, and hide it when there is no visible source above

**Evidence:** `settings-full.png` — the `Audio` card's first *visible* row is `System audio` (its own `Merge with above` makes no sense — there is nothing above it once `Application audio` is hidden), and its checkbox ends at x=693 against the card's x=761 right edge.

**Root cause (two distinct bugs in one row):**
1. Same alignment bug as Task 1 (`ExoCheckBox { Layout.fillWidth: true }` stretches the bounding box but the indicator+label stay left-pinned inside it — confirmed by reading `ExoCheckBox.qml`: `indicator.x = root.leftPadding`, never repositioned by the box's width).
2. `SettingsAudioSection.qml` shows `Application audio` conditionally (`visible: root.settings.appAudioVisible`, true only while a specific application window is the capture target) but `System audio`'s `SettingsAudioSourceRow` always renders its `Merge with above` control regardless of whether `Application audio` is actually visible above it.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsAudioSourceRow.qml`
- Modify: `app/quick/ExoSnap/Quick/SettingsAudioSection.qml`

**Interfaces:**
- `SettingsAudioSourceRow` gains `property bool showMergeOption: true` (default preserves current behavior for `Application audio` and `Microphone`, both of which always have a visible row above them).

- [ ] **Step 1: Add the `showMergeOption` property and wire it to the checkbox's visibility**

```qml
// app/quick/ExoSnap/Quick/SettingsAudioSourceRow.qml
ExoSettingRow {
    id: root

    required property bool sourceEnabled
    required property bool separateTrack
    required property bool locked
    required property real meterLevel
    // False only for a source row that renders as the topmost VISIBLE row:
    // "merge with above" is meaningless when there is no visible row above it.
    property bool showMergeOption: true

    signal sourceToggled(bool value)
    signal separateToggled(bool value)

    hint: qsTr("Include this source")
    controlWidth: 300

    RowLayout {
        spacing: ExoTheme.spacingSm
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.sourceEnabled
            enabled: !root.locked
            Accessible.name: qsTr("Enable %1").arg(root.label)
            Layout.alignment: Qt.AlignVCenter
            onToggledByUser: value => root.sourceToggled(value)
        }

        ExoLevelMeter {
            level: root.meterLevel
            active: root.sourceEnabled
            Layout.alignment: Qt.AlignVCenter
        }

        ExoCheckBox {
            text: qsTr("Merge with above")
            checked: !root.separateTrack
            enabled: !root.locked && root.sourceEnabled
            visible: root.showMergeOption
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Accessible.name: qsTr("Merge %1 with the track above").arg(root.label)
            onToggledByUser: value => root.separateToggled(!value)
        }
    }
}
```

Note `ExoCheckBox` switched from `Layout.fillWidth: true` to `Layout.alignment: Qt.AlignRight | Qt.AlignVCenter` — this is the Task-1-style alignment fix applied here too, since this row builds its own `RowLayout` rather than using `ExoSettingRow`'s `controlHost` directly.

- [ ] **Step 2: Bind `showMergeOption` on the `System audio` row to whether `Application audio` is visible above it**

```qml
// app/quick/ExoSnap/Quick/SettingsAudioSection.qml, the "System audio" SettingsAudioSourceRow
SettingsAudioSourceRow {
    label: qsTr("System audio")
    sourceEnabled: root.settings.systemAudioEnabled
    separateTrack: root.settings.systemAudioSeparate
    locked: root.settings.controlsLocked
    meterLevel: root.settings.systemMeter
    stacked: root.stacked
    showMergeOption: root.settings.appAudioVisible
    Layout.fillWidth: true
    onSourceToggled: value => root.settings.systemAudioEnabled = value
    onSeparateToggled: value => root.settings.systemAudioSeparate = value
}
```

The `Application audio` and `Microphone` rows keep the default (`showMergeOption: true`) — `Application audio` never has anything above it needing this guard, and `Microphone` always has a visible row above it (`System audio` is never conditionally hidden).

- [ ] **Step 3: Build and capture, in both source-visibility states**

```
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a2-desktop.png --visual-page 1 --visual-test-size 1600x1200
```

Confirm: with a Desktop/Display capture target (the default, `appAudioVisible` false), `System audio` shows no `Merge with above` control at all, and its checkbox — once `Application audio` IS visible — ends flush with the card's other right edges.

- [ ] **Step 4: qmllint, then commit**

```bash
cmake --build build/windows-x64-ninja-debug --target all_qmllint
git add app/quick/ExoSnap/Quick/SettingsAudioSourceRow.qml app/quick/ExoSnap/Quick/SettingsAudioSection.qml
git commit -m "fix(quick): hide/align 'merge with above' correctly on the audio source rows"
```

### Task 3: Diagnose and fix the empty "Microphone device" selectbox

**Evidence:** `settings-full.png` — the `Microphone device` combobox renders completely blank (no text, not even a "(no microphone)" placeholder), unlike the old Widgets reference (`settings-audio-devices-normal.png`) which showed either a real device name or `(unavailable)`.

**This is investigation-first, not a known fix** — the root cause has to be found before a task is written for it. Likely candidates, in order of likelihood given `ExoSelect { options: root.settings.microphoneDeviceOptions, value: root.settings.microphoneDeviceId }` (`SettingsAudioSection.qml`):
1. `microphoneDeviceOptions` is empty on this machine (no capture device enumerated) and `ExoSelect` renders blank instead of a placeholder when its options list is empty — check `ExoSelect.qml` for a placeholder/empty-state fallback.
2. `microphoneDeviceOptions` is non-empty but `microphoneDeviceId`'s value does not match any option's `value` field, so the select has nothing to display for the current value.

**Files:**
- Investigate: `app/quick/ExoSnap/Quick/ExoSelect.qml` (does it have a `(no selection)` fallback state at all?)
- Investigate: `app/quick/ExoSnap/Quick/SettingsAdapter.cpp` (`microphoneDeviceOptions()`/`microphoneDeviceId()` — what do they report with no real microphone attached to this dev machine, vs. what the old Widgets `micDeviceCombo` showed as `visual-test-mic (unavailable)`)

- [ ] **Step 1: Reproduce with a known device state**

Run the app normally (not `--visual-test`) with a real microphone attached, or check `AppLog` output for `AudioDiscovery` on a `--visual-test` run — confirm whether `inputs:0` or `inputs:1+` is being reported, matching the `settings-top.png` capture's log correlate if available.

- [ ] **Step 2: Read `ExoSelect.qml`'s handling of an empty `options` array and of a `value` with no matching option**

Determine which of the two candidate causes above is real. No code changes in this step — this is the root-cause step, matching `superpowers:systematic-debugging` Phase 1.

- [ ] **Step 3: Fix at the identified layer**

If `ExoSelect` has no empty/no-match placeholder: add one (e.g. render the adapter-provided current label even when it is not in `options`, or show a muted `(no microphone)` string when `options` is empty), matching the old Widgets behavior of always showing SOME text in that field.

If the adapter is failing to emit a real device list on this machine: this becomes its own follow-up investigation (out of this plan's scope — flag it back to the user rather than guessing at `SettingsAdapter.cpp`/engine-level audio enumeration without reproducing it first).

- [ ] **Step 4: Build, capture, qmllint, commit** (concrete commit message depends on Step 3's finding — write it once the fix is known)

---

### Task 4: Replace the "Configure"/"Hide" button with a chevron disclosure, matching the old widget pattern

**Evidence:** old Widgets reference `settings-audio-expert-mic-post-open.png` shows `Microphone post-processing  (i)   Off ⌃` — a status word plus a chevron, no button chrome. Current code (`SettingsAudioSection.qml`) uses `ExoButton { text: micPostProcessing.visible ? qsTr("Hide") : qsTr("Configure"), quiet: true, Layout.fillWidth: true }` — a real button, and `Layout.fillWidth: true` on a text-only button stretches its bounding box while the button's own content stays left-aligned inside it (the same class of bug as Root cause A), which is why "Hide" reads as left-aligned when expanded.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsAudioSection.qml`

**Interfaces:** Reuses the existing `ExoChevron` component (already used by `ExoDisclosure.qml`) — no new component needed.

- [ ] **Step 1: Replace the `ExoButton` trailing control with a status label + chevron, wrapped in a `TapHandler`**

```qml
// app/quick/ExoSnap/Quick/SettingsAudioSection.qml
ExoSettingRow {
    label: qsTr("Microphone post-processing")
    hint: root.settings.micPostProcessingSummary
    stacked: root.stacked
    controlWidth: 100
    Layout.fillWidth: true

    RowLayout {
        spacing: ExoTheme.spacingSm
        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

        Label {
            text: micPostProcessing.visible ? qsTr("Hide") : qsTr("Configure")
            textFormat: Text.PlainText
            color: ExoTheme.textSecondary
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        ExoChevron {
            direction: micPostProcessing.visible ? 90 : -90
            tone: ExoTheme.textMuted
            Layout.preferredWidth: 12

            Behavior on rotation {
                NumberAnimation {
                    duration: ExoTheme.animMedium
                    easing.type: Easing.OutCubic
                }
            }
        }

        TapHandler {
            onTapped: micPostProcessing.visible = !micPostProcessing.visible
        }
    }
}
```

Check `ExoChevron.qml`'s `direction` values before using `90`/`-90` verbatim — `ExoDisclosure.qml` uses `0`/`-90` for expanded/collapsed, confirm the convention and match it rather than inventing a new one.

- [ ] **Step 2: Confirm the whole row (label + chevron), not just the chevron glyph, is the tap target**

A `TapHandler` on the `RowLayout` covers the label and chevron both — verify in the running capture that clicking the word "Configure" (not just the tiny arrow) toggles the panel, matching the old `micPostProcessingDisclosure`'s generous `QToolButton` hit area.

- [ ] **Step 3: Build, capture both collapsed and expanded states, qmllint, commit**

```
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a4.png --visual-page 1 --visual-test-size 1600x1600
```

```bash
git add app/quick/ExoSnap/Quick/SettingsAudioSection.qml
git commit -m "fix(quick): microphone post-processing disclosure is a chevron, not a button"
```

---

### Task 5: Fix every suffixed number field resetting to its minimum on blur

**Evidence:** live report — clicking the `+`/`-` steppers on `Audio bitrate` correctly advances to a valid value (e.g. 192 kbps), but once the field loses focus the value snaps back to 32 kbps (the field's `from`).

**Root cause:** `ExoNumberField.qml` overrides `textFromValue` to append the unit suffix (`"192 kbps"`) but never overrides `valueFromText`. Qt Quick's `SpinBox` uses `valueFromText`/its `validator` to parse the *displayed* text back into a value whenever it re-validates (losing focus is exactly such a point) — the default parser expects a plain number and fails on the trailing `" kbps"`/`" min"`/`" MB"`, and a `SpinBox` falls back to `from` when parsing fails.

The fix is in the shared component, so it repairs every instance with a non-empty `suffix` in one change — confirmed by grep, this is **8 fields, not 3**:

| File | Field | Suffix |
|---|---|---|
| `SettingsAudioSection.qml` | Audio bitrate | kbps |
| `SettingsQualitySection.qml` | Video bitrate (expert, VBR/CBR mode) | kbps |
| `SettingsQualitySection.qml` | Frame rate (expert, custom FPS) | fps |
| `SettingsOutputSection.qml` | Split interval, custom minutes | min |
| `SettingsOutputSection.qml` | Segment size | MB |
| `SettingsMicDspGroup.qml` | High-pass cutoff frequency | Hz |
| `SettingsMicDspGroup.qml` | Noise gate threshold | dB |
| `SettingsMicDspGroup.qml` | AGC target loudness | dB |

Instances with no suffix (`Constant quality (CQ)`, `FLAC compression`, `Opus complexity`, the custom-resolution width/height fields) are unaffected and need no change.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/ExoNumberField.qml`
- Test: `app/quick/tests/tst_ExoNumberField.qml` (create if no existing test file covers this component — check first)

**Interfaces:** No property/signal changes — internal `valueFromText` addition only.

- [ ] **Step 1: Check for an existing QML test file for `ExoNumberField`**

```
find app/quick/tests -iname "*NumberField*"
```

If found, read it before writing Step 2's test — the pattern below assumes none exists.

- [ ] **Step 2: Write a failing QML test that reproduces the bug**

```qml
// app/quick/tests/tst_ExoNumberField.qml
import QtQuick
import QtTest
import ExoSnap.Quick

TestCase {
    name: "ExoNumberFieldSuffixParsing"

    Component {
        id: fieldComponent

        ExoNumberField {
            from: 32
            to: 510
            stepSize: 8
            suffix: "kbps"
            value: 160
        }
    }

    function test_blur_after_step_keeps_the_stepped_value() {
        const field = createTemporaryObject(fieldComponent, null);
        verify(field !== null);
        field.increase();
        compare(field.value, 168);
        field.forceActiveFocus();
        field.focus = false;
        compare(field.value, 168);
    }
}
```

- [ ] **Step 3: Run it to confirm it fails**

```
pwsh scripts/run-qml-tests.ps1 -Filter ExoNumberFieldSuffixParsing
```

(Use whichever qmltestrunner invocation `qt-development-skills:qt-qml-test-run` / the project's existing QML test script actually uses — check `scripts/` for the real entry point name before running; do not guess a flag.)

Expected: FAIL, `field.value` reset to 32 (or stayed at 160 without applying the step — either way, not 168).

- [ ] **Step 4: Add `valueFromText`, stripping the suffix before parsing**

```qml
// app/quick/ExoSnap/Quick/ExoNumberField.qml
textFromValue: (value, locale) => root.suffix === ""
                                  ? Number(value).toLocaleString(locale, 'f', 0)
                                  : qsTr("%1 %2").arg(Number(value).toLocaleString(locale, 'f', 0)).arg(root.suffix)

valueFromText: (text, locale) => {
    const digitsOnly = text.replace(/[^0-9-]/g, '');
    const parsed = Number.fromLocaleString(locale, digitsOnly);
    return Number.isFinite(parsed) ? parsed : root.value;
}
```

Falling back to `root.value` (the value BEFORE the failed parse) rather than to `root.from` on a genuinely unparseable string — the current `from`-fallback is the bug, and a value field should never silently jump to its floor because of a formatting artifact it introduced itself.

- [ ] **Step 5: Run the test again to confirm it passes**

Expected: PASS.

- [ ] **Step 6: Manually verify all eight affected fields**

Build, run the app normally (not harness), Expert mode on. Change each of the eight fields in the table above via its steppers, tab away, confirm it holds: `Audio bitrate`, `Video bitrate` (needs a VBR/CBR rate control mode), `Frame rate` (needs custom FPS), `Split interval` custom minutes (needs `Split by time` on), `Segment size` (needs `Split by size` on), and the three `SettingsMicDspGroup.qml` fields (each needs its own stage switched on: `High-pass filter`, `Noise gate`, `Automatic gain control`).

- [ ] **Step 7: qmllint, commit**

```bash
cmake --build build/windows-x64-ninja-debug --target all_qmllint
git add app/quick/ExoSnap/Quick/ExoNumberField.qml app/quick/tests/tst_ExoNumberField.qml
git commit -m "fix(quick): number fields with a unit suffix no longer reset on blur"
```

---

### Task 6: Remove the Alt+F9 factory default — every hotkey ships unset

**Product decision (2026-08-19), supersedes `docs/product-spec.md` §10:** the spec documents Alt+F9 as `ToggleRecording`'s shipped default specifically because it collides with NVIDIA Instant Replay's own Alt+F9 default "on every launch that app is running" — common enough on this product's target hardware (high-fps recording, NVIDIA GPUs) that the collision is treated as expected noise rather than a real conflict, silently dropped rather than surfacing a notification. That tradeoff is no longer worth its cost: a default that silently fails to register for a large share of the actual audience gives most of those users no working hotkey and no visible sign why, while everyone else carries a special case through the hotkey UI (Reset vs Clear meaning different things on exactly one row) purely to support the one action that has a non-empty default. Removing it makes all five actions uniform — ship unset, let the user pick a combo that does not collide on their machine — and eliminates the entire silent-drop-vs-notify distinction, since that branch only ever existed to protect a *shipped* default from generating noise, and once no default is ever pre-registered, nothing can be silently overwritten by another app at first launch.

**Files:**
- Modify: `app/services/GlobalHotkeyService.cpp` (`DefaultBinding()`)
- Modify: `app/tests/test_global_hotkey_service.cpp` (three tests reference the Alt+F9 default directly — read and update, do not delete their coverage)
- Modify: `docs/product-spec.md` (§10, the paragraph naming Alt+F9/NVIDIA Instant Replay and the silent-vs-notify split)

- [ ] **Step 1: Make every action's default empty**

```cpp
// app/services/GlobalHotkeyService.cpp
QKeySequence GlobalHotkeyService::DefaultBinding(HotkeyAction action) {
    switch (action) {
    case HotkeyAction::ToggleRecording:
        return QKeySequence(); // no default binding, per product decision
    case HotkeyAction::TogglePause:
        return QKeySequence();
    case HotkeyAction::CaptureFrame:
        return QKeySequence(); // no default binding per spec
    case HotkeyAction::AddMarker:
        return QKeySequence(); // no default binding per spec
    case HotkeyAction::SplitRecording:
        return QKeySequence(); // unset by default per SPLIT-RECORDING-R1
    }
```

- [ ] **Step 2: Update the three tests in `test_global_hotkey_service.cpp` that assert the Alt+F9 default**

`IsAtDefaultReflectsCustomizationState` (line ~180) currently expects `ToggleRecording` to start `IsAtDefault() == true` while carrying an actual Alt+F9 binding — change its setup/assertions to reflect an empty default like the other four actions. `ResetToDefaultRestoresDefaultBinding` (line ~236) and the `ResetToDefault(HotkeyAction::TogglePause)` case at line ~342 should still pass unchanged in spirit (reset now restores emptiness instead of Alt+F9) — read each one and adjust its concrete expected `QKeySequence` value, do not just delete coverage.

- [ ] **Step 3: Run the hotkey service test binary**

```
pwsh scripts/run-tests.ps1 -Filter recorder_core.GlobalHotkeyService
```

(Confirm the actual filter name matches this binary's real gtest suite name before running — check an existing invocation in CI or `scripts/run-tests.ps1`'s own help output first.)

- [ ] **Step 4: Update `docs/product-spec.md` §10**

Remove the Alt+F9/NVIDIA Instant Replay example and the "common environmental noise... dropped silently" framing built around it — with no action ever shipping a non-empty default, a non-empty binding can now only exist because the user set it themselves, so every lost binding is a customized one and the spec's own existing rule already covers it: "A binding the user deliberately set to something else worked when they chose it, so losing it is worth telling them about." State plainly that hotkeys ship unset and losing *any* registered binding at startup raises the Rebind notification — there is no longer a silent case to describe.

- [ ] **Step 5: Note for later, not for this task — do not act on this now**

`QuickApplication.cpp`'s startup registration-failure handling (the code deciding silent-log vs notify, per the spec paragraph above) likely has a now-dead branch: the "still equal to shipped default, don't notify" path can never trigger once no default is ever non-empty. Confirming and removing that dead branch is a separate, smaller follow-up — flag it back to the user rather than refactoring unfamiliar control flow speculatively inside this task.

- [ ] **Step 6: Commit**

```bash
git add app/services/GlobalHotkeyService.cpp app/tests/test_global_hotkey_service.cpp docs/product-spec.md
git commit -m "feat(hotkeys): ship every hotkey unset, remove the Alt+F9 default"
```

### Task 7: Redesign the hotkey row — passive badge, Set/Change/x buttons, no Reset

**Design (confirmed with the user 2026-08-19):** the badge (`HotkeyCaptureField`) becomes purely a display — it still shows the binding, "Not set", or "Press a key combination…" while capturing, colored the same way it already is, but is no longer itself clickable. The row's trailing buttons become dynamic instead of a fixed three: unbound shows a single **Set** button (starts capture); bound shows **Change** (starts a new capture) and an **x** (clears to unset). No `Reset` button anywhere — Task 6 made `Reset` and `Clear` the same operation for every action, so a dedicated Reset button would just be a second control that does what x already does. No card-level "Reset all" either — the old Widgets version had one for exactly this per-row gap (no per-row undo at all back then), and with five rows and a working per-row x it no longer earns its place.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/HotkeyCaptureField.qml` (remove the `TapHandler` and `captureRequested` triggering from user interaction with the field itself; keep it as the visual display and the `Keys.onPressed` handling for while it IS capturing, i.e. Escape-to-cancel and the actual key capture)
- Modify: `app/quick/ExoSnap/Quick/SettingsHotkeysSection.qml` (replace the fixed `Reset`/`Clear` button pair with the dynamic Set/Change/x set)

- [ ] **Step 1: Strip click-to-start from `HotkeyCaptureField`, keep it passive**

```qml
// app/quick/ExoSnap/Quick/HotkeyCaptureField.qml
Control {
    id: root

    required property bool capturing
    required property string binding

    signal captureCancelled()
    signal captured(int key, int modifiers)

    implicitHeight: ExoTheme.controlHeight
    implicitWidth: 160
    Accessible.role: Accessible.StaticText

    Keys.onPressed: event => {
        if (!root.capturing)
            return;
        event.accepted = true;
        if (event.key === Qt.Key_Escape) {
            root.captureCancelled();
            return;
        }
        if (event.key === Qt.Key_Shift || event.key === Qt.Key_Control || event.key === Qt.Key_Alt
                || event.key === Qt.Key_Meta) {
            return;
        }
        root.captured(event.key, event.modifiers);
    }

    contentItem: Label {
        text: root.capturing ? qsTr("Press a key combination…")
                             : root.binding === "" ? qsTr("Not set") : root.binding
        textFormat: Text.PlainText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter
        color: root.capturing ? ExoTheme.accent : root.binding === "" ? ExoTheme.textMuted : ExoTheme.text
        font {
            family: root.binding === "" || root.capturing ? ExoTheme.sansFamily : ExoTheme.monoFamily
            pixelSize: ExoTheme.fontSecondary
        }
    }

    background: Rectangle {
        color: root.capturing ? ExoTheme.surfaceRaised : "transparent"
        border.width: root.capturing ? 1 : 0
        border.color: ExoTheme.accent
        radius: ExoTheme.radiusSm
    }
}
```

The field no longer takes focus or handles `Return`/`Space`/`Enter` itself — the `Set`/`Change` button is the thing that starts a capture now, and once capturing starts it is that button (see Step 2) that should hold focus and forward key events, since `HotkeyCaptureField` itself is no longer focusable. Check whether `Keys.onPressed` on a non-focusable `Control` still receives events when a SIBLING has focus — it will not, by normal Qt Quick key delivery rules, so this component likely needs `forceActiveFocus()` called on it FROM the Set/Change button's click handler despite no longer being interactive itself (a focusable-but-not-clickable field is a real, legitimate state — the accessibility role becomes `Accessible.StaticText` while at rest and effectively a capture surface only while `capturing` is true). Re-add `focusPolicy: Qt.StrongFocus` if forcing focus onto it turns out to be necessary — verify this by actually building and tabbing/clicking through the Set button in a real run, not by assuming.

- [ ] **Step 2: Replace the trailing button pair in `SettingsHotkeysSection.qml`**

```qml
// app/quick/ExoSnap/Quick/SettingsHotkeysSection.qml
RowLayout {
    spacing: ExoTheme.spacingSm
    Layout.fillWidth: true

    HotkeyCaptureField {
        capturing: hotkeyRow.capturing
        binding: hotkeyRow.modelData.binding
        enabled: !root.settings.controlsLocked
        Layout.fillWidth: true
        Accessible.name: qsTr("Shortcut for %1").arg(hotkeyRow.modelData.label)
        onCaptureCancelled: root.settings.cancelHotkeyCapture()
        onCaptured: (key, modifiers) => root.settings.commitHotkeyCapture(key, modifiers)
    }

    ExoButton {
        text: hotkeyRow.modelData.binding === "" ? qsTr("Set") : qsTr("Change")
        quiet: true
        enabled: !root.settings.controlsLocked
        onClicked: {
            captureField.forceActiveFocus();
            root.settings.beginHotkeyCapture(hotkeyRow.modelData.action);
        }
    }

    ExoButton {
        text: qsTr("×")
        quiet: true
        visible: hotkeyRow.modelData.binding !== ""
        enabled: !root.settings.controlsLocked
        Accessible.name: qsTr("Clear shortcut for %1").arg(hotkeyRow.modelData.label)
        onClicked: root.settings.clearHotkey(hotkeyRow.modelData.action)
    }
}
```

Give the `HotkeyCaptureField` instance an `id: captureField` so the `Set`/`Change` button's `onClicked` can reach it — the snippet above assumes that id exists; add it in the same edit. Confirm `ExoButton` renders a literal `×` character legibly at its normal size before committing to a bare Unicode glyph as the clear button's whole label — if it reads as unclear on its own, pair it with an accessible-only label or use a small `ExoGlyph` icon instead (check what icon glyphs `ExoGlyph.qml` already offers before inventing a new one).

- [ ] **Step 3: Build, capture both the unbound and bound state of at least one row**

```
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a7-unbound.png --visual-page 1 --visual-test-size 1600x1200
```

For the bound state, either bind one live (build normally, not the harness, click Set, press a combo) and re-run the harness capture against the isolated harness config afterward, or seed a binding directly into the harness config dir's `settings.ini` `[hotkeys]` section the same way Expert Mode was seeded earlier in this session.

- [ ] **Step 4: Manually verify keyboard capture still works end to end**

Run normally (not the harness): click `Set`, confirm the field shows "Press a key combination…", press a real combo, confirm it commits and the buttons become `Change` + `x`. Press Escape mid-capture, confirm it cancels back to the prior state. This exercises the focus-forwarding concern flagged in Step 1 — if pressing a key while "capturing" does nothing, that concern was real and needs a real fix, not a guess.

- [ ] **Step 5: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/HotkeyCaptureField.qml app/quick/ExoSnap/Quick/SettingsHotkeysSection.qml
git commit -m "feat(quick): hotkey rows use a passive badge and dynamic Set/Change/clear buttons"
```

### Task 8: Hotkeys — widen the row so the capturing-state text is not truncated

**Evidence:** live report — "Press a key combina..." is cut off. `HotkeyCaptureField.qml` has `implicitWidth: 160` and `elide: Text.ElideRight`; the row (`SettingsHotkeysSection.qml`) gives the whole trailing `RowLayout` only `controlWidth: 280` to share between the capture field and its buttons. Task 7 already reduces that to two buttons instead of three (`Set`/`Change` + a narrow `x` instead of `Reset` + `Clear`), which recovers some width on its own — measure what is left after Task 7 lands before assuming 360px is still the right number; it may need less now.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsHotkeysSection.qml`

- [ ] **Step 1: Measure the capture field's actual rendered width in Task 7's own capture, and widen `controlWidth` only as much as needed**

Do this AFTER Task 7 is built and captured, not before — Task 7's button-count reduction changes the math this task's fix depends on.

```qml
// app/quick/ExoSnap/Quick/SettingsHotkeysSection.qml
ExoSettingRow {
    id: hotkeyRow
    // ...
    controlWidth: 320 // re-measure against Task 7's build before committing to this number
    Layout.fillWidth: true
    // ...
}
```

- [ ] **Step 2: Build, capture at both 1600px and the 860px minimum width, confirm "Press a key combination…" fits without eliding at 1600px, and degrades gracefully (still readable, not garbled) at 860px**

```
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a8-wide.png --visual-page 1 --visual-test-size 1600x1200
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a8-narrow.png --visual-page 1 --visual-test-size 860x900
```

- [ ] **Step 3: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsHotkeysSection.qml
git commit -m "fix(quick): hotkey capture field no longer truncates its own prompt text"
```

### Task 9: Clearing a hotkey during a failed capture leaves a stale conflict warning

**Evidence:** live report — after starting a capture that hit a conflict and then clearing the binding (now the `x` button from Task 7, previously `Reset`), a "This shortcut is already used by Windows or another application" warning stays visible under a row that is now unset, which is false for an unset binding.

**Root cause (adapter-side, C++, needs its own investigation before a fix is written):** the warning is driven by `root.settings.hotkeyErrorAction`/`hotkeyErrorText`, set during the PRIOR capture attempt. The reported sequence (start capture, the field shows an error state, clear it) is consistent with `clearHotkey()`/`cancelHotkeyCapture()` clearing the *binding* but not also clearing `hotkeyErrorAction`/`hotkeyErrorText` for that action.

**Files:**
- Investigate: `app/quick/ExoSnap/Quick/SettingsAdapter.cpp` (`clearHotkey()`, `cancelHotkeyCapture()`, `hotkeyErrorAction`/`hotkeyErrorText` members)
- Investigate: wherever hotkey conflict detection sets `hotkeyErrorAction` (likely `commitHotkeyCapture()` or the `GlobalHotkeyService` callback it forwards)

- [ ] **Step 1: Read `clearHotkey()` and `cancelHotkeyCapture()`'s current implementations and confirm which one (or both) leaves `hotkeyErrorAction` stale — do not assume without reading the code**

- [ ] **Step 2: Write a targeted adapter test reproducing the exact reported sequence**

```cpp
// app/tests/test_settings_adapter_hotkeys.cpp (add to the existing hotkey test file if one exists — search first)
TEST(SettingsAdapterHotkeyTest, ClearingAHotkeyDropsItsStaleConflictWarning) {
    // Arrange: an adapter with a hotkey action bound, then simulate a capture
    // that produced a conflict (sets hotkeyErrorAction/hotkeyErrorText for it).
    // Act: call clearHotkey(action).
    // Assert: hotkeyErrorAction no longer names this action, and hotkeyErrorText
    // is empty for it.
}
```

(Exact fixture setup depends on `SettingsAdapter`'s real constructor/test harness — read an existing adapter test in the same file/directory before writing this, and match its setup pattern rather than inventing a new one.)

- [ ] **Step 3: Run it, confirm it fails**

- [ ] **Step 4: Fix `clearHotkey()` (and `cancelHotkeyCapture()`, if Step 1 found it also affected) to clear the error state for that action before emitting the model update**

- [ ] **Step 5: Run the test again, confirm it passes; run the full adapter test binary to confirm nothing else regressed**

```
pwsh scripts/run-tests.ps1 -Filter recorder_core.SettingsAdapter
```

(Confirm the actual test binary/filter name first — do not guess.)

- [ ] **Step 6: Commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsAdapter.cpp app/tests/test_settings_adapter_hotkeys.cpp
git commit -m "fix(quick): clearing a hotkey drops its stale conflict warning"
```

---

### Task 10: Output card — destination folder text field is squeezed to an unreadable width

**Evidence:** `settings-full.png` — the `Destination folder` field renders `/ideos\ExoSnap` (the tail end of `C:\Users\User\Videos\ExoSnap`, with the leading characters cut off, no ellipsis). This `ExoSettingRow` has no `controlWidth` override, so it defaults to 220px shared between the `ExoTextField` and the `Browse…` button in the same `RowLayout` — far too narrow for an absolute path.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsOutputSection.qml`

- [ ] **Step 1: Give the `Destination folder` row a wide control column**

```qml
// app/quick/ExoSnap/Quick/SettingsOutputSection.qml
ExoSettingRow {
    label: qsTr("Destination folder")
    hint: qsTr("Where recordings are saved")
    warning: root.settings.folderValidation
    stacked: root.stacked
    controlWidth: 420
    Layout.fillWidth: true

    RowLayout {
        // unchanged
    }
}
```

- [ ] **Step 2: Build, capture, confirm the full path (or a sane ellipsis-from-the-left truncation, if `ExoTextField` supports one — check before assuming plain widening is sufficient for a very long path) is readable**

- [ ] **Step 3: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsOutputSection.qml
git commit -m "fix(quick): destination folder field is wide enough to show a real path"
```

---

### Task 11: Rebalance the two-column card split so Developer/Appearance don't dangle alone

**Evidence:** `settings-hotkeys-default.png` (bottom-of-page capture, this session) — the left column (Format, Quality, Audio, Output — 4 cards) ends far above where the right column (Webcam, Overlays, Presence, Hotkeys, Updates, Appearance, Developer — 7 cards) ends, leaving Appearance and Developer stacked alone against a large empty gap beside them in the left column.

**Constraint from `SettingsPage.qml`'s own design comment (keep this, do not "fix" it away):** the two columns are independent (not a row-aligning grid) *on purpose* — a grid that balances heights would leave dead space beside a short card next to a tall one, and reading order must stay top-to-bottom within each column. The fix here is which cards go in which column, not the column mechanism itself.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsPage.qml` (the `twoColumn` composition only — `oneColumn`'s order is the canonical reading order and must NOT change)

- [ ] **Step 1: Move `Webcam` from the right column to the end of the left ("recording chain") column**

`Webcam` is topically part of what gets recorded, same as `Format`/`Quality`/`Audio`/`Output` — and moving it evens the two columns from a 4/7 split to a 5/6 split.

```qml
// app/quick/ExoSnap/Quick/SettingsPage.qml, twoColumn composition
ColumnLayout {
    spacing: ExoTheme.sectionGap
    Layout.fillWidth: true
    Layout.preferredWidth: 1
    Layout.alignment: Qt.AlignTop

    LayoutItemProxy { target: formatSection }
    LayoutItemProxy { target: qualitySection }
    LayoutItemProxy { target: audioSection }
    LayoutItemProxy { target: outputSection }
    LayoutItemProxy { target: webcamSection }

    Item {
        Layout.fillHeight: true
    }
}

ColumnLayout {
    spacing: ExoTheme.sectionGap
    Layout.fillWidth: true
    Layout.preferredWidth: 1
    Layout.alignment: Qt.AlignTop

    LayoutItemProxy { target: overlaysSection }
    LayoutItemProxy { target: presenceSection }
    LayoutItemProxy { target: hotkeysSection }
    LayoutItemProxy { target: updatesSection }
    LayoutItemProxy { target: appearanceSection }
    LayoutItemProxy { target: developerSection }

    Item {
        Layout.fillHeight: true
    }
}
```

Also update the doc comment above `twoColumn` (currently says "left is the recording chain (format -> quality -> audio -> output)") to include webcam in the list.

- [ ] **Step 2: Build, capture the full page at 1600px width, and measure whether the columns end noticeably closer together than before — this is the one change in this plan most worth eyeballing together rather than shipping on the strength of a single capture**

```
build/windows-x64-ninja-debug/app/exosnap.exe --visual-test <scratch>/settings-a10.png --visual-page 1 --visual-appearance dark --visual-accent aqua --visual-test-size 1600x2600
```

If the columns are still visibly lopsided after moving Webcam alone, do not move a second card unilaterally — stop and confirm with the user which additional card (if any) makes sense topically, since the "recording chain" framing constrains what belongs in the left column.

- [ ] **Step 3: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsPage.qml
git commit -m "fix(quick): move Webcam into the left column so Developer/Appearance don't dangle alone"
```

---

### Task 12: "Crash reports" hint wraps to two lines where the copy was written for one

**Evidence:** `settings-full.png` and `settings-hotkeys-default.png` both show `Reports are privacy-scrubbed and never sent without\nconsent` wrapping mid-sentence in the `Developer` card, in both a narrower and the full 1600px capture — the copy reads as a single deliberate sentence, not one designed to break there.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsDeveloperSection.qml`

**This is a copy-length fix, not a layout fix** — `Developer` and `Appearance` are both right-column cards at roughly half the page's content width; widening the label column generally would require touching `ExoSettingRow` (out of scope, affects every settings row on the page) or `SettingsPage.qml`'s column split (Task 11 already touches that file for an unrelated reason — do not conflate the two). The lower-risk fix is shortening the one hint that was clearly meant to fit on one line.

- [ ] **Step 1: Shorten the hint to fit the card width at 1600px without wrapping**

```qml
// app/quick/ExoSnap/Quick/SettingsDeveloperSection.qml
ExoSettingRow {
    label: qsTr("Crash reports")
    hint: qsTr("Privacy-scrubbed, never sent without consent")
    stacked: root.stacked
    Layout.fillWidth: true

    ExoSelect {
        // unchanged
    }
}
```

- [ ] **Step 2: Build, capture, confirm one line at 1600px. Check the same row at the 860px minimum width — a shorter sentence may still wrap there, which is acceptable (the old Widgets reference itself only guaranteed one line at its own tested width, not at every width) but should not look worse than before**

- [ ] **Step 3: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsDeveloperSection.qml
git commit -m "fix(quick): shorten the crash-reports hint so it reads as one line"
```

---

### Task 13: Tab hover highlight — widen the hit/highlight area

**Not independently visually confirmed in this plan** — a hover state cannot be captured by `--visual-test` (it renders one static state per process, and synthesizing a mouse-hover for the sole purpose of a screenshot is exactly the kind of input synthesis `AGENTS.md` reserves for an explicit ask). This task is based on the live report plus reading `ExoNavTab.qml` — confirm the fix visually together live, not by trusting this task's reasoning alone.

**Code observation:** `ExoNavTab.qml`'s `implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding` sizes the whole button — hit area AND hover-highlight background alike, since `background: Item { Rectangle { anchors.fill: parent; visible: root.hovered && !root.selected } }` fills exactly that. `leftPadding`/`rightPadding` are `ExoTheme.spacingMd`/`spacingSm` (compact) — tight enough that the highlight reads as hugging the label rather than filling a generous tab-shaped zone, matching the report.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/ExoNavTab.qml`

- [ ] **Step 1: Check `ExoTheme.spacingMd`/`spacingLg`'s actual pixel values before picking a new padding — do not guess a number**

```
grep -n "spacingMd\|spacingLg\|spacingXl" app/quick/ExoSnap/Quick/ExoTheme.qml
```

- [ ] **Step 2: Increase `leftPadding`/`rightPadding` one step (e.g. `spacingMd` -> `spacingLg` at the regular width class, keep `spacingSm` at compact — the compact rung exists specifically because 860px is already tight, per this file's own comment)**

```qml
leftPadding: root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg
rightPadding: root.compact ? ExoTheme.spacingSm : ExoTheme.spacingLg
```

- [ ] **Step 3: Build, run the app normally (not the harness — this needs a real hover), confirm live with the user that the highlight now reads as covering the tab rather than the label. This is a judgment call on "enough" padding, not a pass/fail measurement.**

- [ ] **Step 4: Confirm the wider tabs still fit five destinations plus the window controls at the 860px minimum width (the exact constraint this file's own comments describe) — if they don't, the compact rung's `spacingSm` floor may need to be reached at a wider breakpoint than today, which is a second, separate change to review before making it**

- [ ] **Step 5: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/ExoNavTab.qml
git commit -m "fix(quick): tab hover highlight covers the tab, not just its label"
```

### Task 14: Indent the revealed sub-rows in the microphone post-processing group, matching the Overlay content pattern

**This corrects a mistake in this plan's first draft.** The draft originally filed the `Recording overlay content` checkbox indent under "explicitly not changing," reasoning that `SettingsMicDspGroup.qml`'s stages use a structurally different container (full `ExoSettingRow`s, not a compact checkbox list) and so had nothing to be consistent WITH. That reasoning compared container shapes instead of actually looking at both renders side by side. A real capture of the mic post-processing panel open (`settings-mic-dsp-scroll.png`, this session, `--settings-visual-bottom` after seeding `mic_hpf_enabled`/`mic_gate_enabled`/`mic_agc_enabled = true` in the harness's `presets.toml`) shows `Cutoff frequency` starting at the exact same x as `High-pass filter` above it — zero indent — while `SettingsOverlaysSection.qml`'s `Elapsed time` DOES indent under `Recording overlay content`. Both are the same relationship: a row that only exists, and only matters, because a switch above it is on. The container each one happens to use (compact checkbox vs. a full label+control row) is not a reason to treat that relationship differently.

**Files:**
- Modify: `app/quick/ExoSnap/Quick/SettingsMicDspGroup.qml` (the three revealed rows: `Cutoff frequency`, `Gate threshold`, `AGC target loudness`)

- [ ] **Step 1: Add the same left margin the Overlay content pattern uses, to each of the three conditionally-visible rows**

```qml
// app/quick/ExoSnap/Quick/SettingsMicDspGroup.qml
ExoSettingRow {
    label: qsTr("Cutoff frequency")
    stacked: root.stacked
    visible: root.settings.micHpfEnabled
    Layout.fillWidth: true
    Layout.leftMargin: ExoTheme.spacingLg

    ExoNumberField {
        // unchanged
    }
}
```

Same one-line addition on the `Gate threshold` row (under `Noise gate`) and the `AGC target loudness` row (under `Automatic gain control`). `High-pass filter`, `Noise gate`, `Automatic gain control` and `RNNoise suppression` themselves are NOT indented — they are the top-level switches, exactly like `Recording overlay content`'s own segmented-control row stays unindented while only the checkboxes below it step in.

- [ ] **Step 2: Build, seed the three DSP stages on in the harness config, capture with `--settings-visual-bottom`, confirm the indent now reads consistently with the Overlays card**

Seed (same technique used this session):
```
sed -i -e 's/mic_hpf_enabled = false/mic_hpf_enabled = true/' -e 's/mic_gate_enabled = false/mic_gate_enabled = true/' -e 's/mic_agc_enabled = false/mic_agc_enabled = true/' <harness-config-dir>/presets.toml
```
Then temporarily set `SettingsMicDspGroup { visible: true }` in `SettingsAudioSection.qml` for the capture only (it is normally driven by the `Configure`/chevron control from Task 4) — build, capture, then revert the temporary `visible: true` before committing anything. Revert the seeded `presets.toml` values back to `false` afterward too, so the harness config stays at its baseline for the next person who uses it.

- [ ] **Step 3: qmllint, commit**

```bash
git add app/quick/ExoSnap/Quick/SettingsMicDspGroup.qml
git commit -m "fix(quick): indent mic post-processing's revealed rows, matching the overlay content pattern"
```

---

## Card header subtitles — surveyed, no change proposed

The user asked for a more unified convention across the ten section cards' header subtitles. Full survey of `title:`/`subtitle:` in every `Settings*Section.qml`:

| Kind | Cards | Example |
|---|---|---|
| Live-state summary (adapter-computed) | Format, Audio, Output, Updates | `"MKV · AV1 · Opus · 60 fps CFR"` |
| Static explanatory sentence | Hotkeys, Overlays, Webcam | `"Drawn over the recorded screen and always excluded from the recording."` |
| None | Appearance, Developer, Quality & timing, Notifications & presence | — |

This is not three arbitrary choices — it already resolves to one rule once stated: a card gets a live summary when it has a checkable resulting state worth surfacing before the user opens it; a card gets a static sentence when its scope or a non-obvious behavior needs saying before its rows make sense (Overlays' exclusion-from-recording, Hotkeys' background-active behavior, Webcam's redirect to the Record preview for position/size); a card gets nothing when its title is self-explanatory and every row already carries its own hint. The old Widgets design had none of the live-summary subtitles at all — they are a Quick-cutover addition, and a good one, not a source of the inconsistency.

**Recommendation: no code change.** The distribution already fits the rule above card by card; what was actually missing is the rule being written down anywhere, not the cards being wrong. If this reads right, the concrete next step is a short comment at the top of `SettingsPage.qml` (or wherever the twelve sections are enumerated) stating the rule, so a future new card is placed deliberately instead of by accident — say so and I will add a task for that comment. Padding the four subtitle-less cards with filler text purely for visual rhythm was considered and rejected: it contradicts this codebase's own "do not restate the obvious" convention (`AGENTS.md`), and every one of those four cards' rows already states what its subtitle would only repeat.

## Explicitly not changing

- **The Accent row's long hint text** ("Highlight colour for selection, active controls and the primary action..."). Unlike `Crash reports`, this reads as intentionally thorough copy explaining a two-axis design decision the file's own header comment describes at length — shortening it to force one line would lose real information. If the user still wants it shortened after seeing Task 12's result, that is a separate explicit ask, not inferred from "title+subtitle on one line."
- **The `Audio bitrate` stepper's 8kbps step size feeling "more like a dropdown."** This is a UX preference on an intentionally coarse, encoder-valid step size, not a defect — Task 5 fixes the actual bug (the value resetting), and the step-size question is left for the user to raise separately if they still want it after that fix lands.

## Self-Review

**Spec coverage** — every item from the chat messages maps to a task, the card-header-subtitle survey, or the "not changing" list:
toggle right-align (Task 1), overlay-content checkbox indent consistency (Task 14, corrected from an earlier wrong "no change" call), Minimal/Custom right-align (Task 1), audio card first-row merge-with-above (Task 2), mic device selectbox empty (Task 3), Configure button -> chevron (Task 4), number input reset-on-blur (Task 5, expanded to 8 fields from the Expert-mode capture), Alt+F9 default removal (Task 6), hotkey row redesign / Set-Change-clear (Task 7), hotkey capture text truncation (Task 8), stale conflict warning after clearing (Task 9), destination folder small/truncated text (Task 10), Developer/Appearance dangling (Task 11), Appearance/Accent right-align (Task 1), title+subtitle one line / Crash reports (Task 12), tab hover area (Task 13), card header subtitle consistency (its own section, no task recommended).

**Placeholder scan** — no TBD/"add error handling"/"similar to Task N" left in any step; every QML/C++ snippet is concrete, and steps that need a real file lookup before writing code (Tasks 3, 6, 9, 13) say exactly what to look up and why, rather than assuming an answer.

**Type/property consistency** — `showMergeOption` (Task 2) is introduced once and consumed once at the same name; `valueFromText` (Task 5) matches `SpinBox`'s real API rather than an invented signal name; `captureField` (Task 7's added `id`) is introduced and consumed within the same task.
