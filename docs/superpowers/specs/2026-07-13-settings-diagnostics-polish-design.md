# Settings & Diagnostics Polish — Design (v0.9)

**Status:** Approved direction, pre-implementation. Scope is a visual/UX polish pass over
existing surfaces. **No new top-level pages** (Tracks / Overlay-HUD / Pipeline were discussed and
deferred to v0.10+; captured separately). Model staffing per complexity (Fable for mechanical,
Sonnet/Opus for logic-bearing slices); one central build, git as ground-truth.

## 1. Goal

Usability first, then aesthetics. Remove UI that reads as broken or inconsistent, consolidate
per-option guidance, and make the Diagnostics simple view feel finished. Every change is
visual/structural except where noted (info consolidation and the diagnostics tile set touch
product-visible behaviour → spec updates flagged).

## 2. Divergences from design canon (deliberate, owner-approved)

The canon is `.workspace/design-canon/` (`suite-*.jsx` + screenshots). Three changes knowingly
diverge from it; recorded here so the divergence is intentional, not drift:

- **Card-header glyph chips.** Canon uses a filled/bordered box
  (`suite-settings.jsx:40`: `28×28, radius 8, background var(--ac-dim), border 1px var(--ac-b2)`),
  and the current build matches it. We remove the box (plain glyph) because it reads as an active
  button. → **diverges from canon.**
- **Diagnostics simple view.** Canon is deliberately compact-centred with intentional empty space
  ("The empty calm IS the feature", `suite-diag2.jsx:264-265`), exactly four tiles. We fill the
  height and add tiles. → **diverges from canon + from product-spec §11** ("exactly four wide
  readiness tiles"); spec must be updated.
- **Info-i placement/selectivity.** We keep the canon's after-label placement but make it
  **selective** (only rows with a real tradeoff) and pull all inline helper text into the popover.
  This is within canon spirit but changes which rows carry an icon.

The **preset toolbar** change is *toward* canon (the current build lost the canon's slim-toolbar
frame): canon wraps it in `background var(--surf), border 1px var(--line), radius 12` slim padding
(`suite-settings.jsx:251-265`) — a quiet member of the card family.

## 3. Decisions

### 3.1 Settings — chrome / look

- **Card-header glyphs:** drop `background` + `border` from `QLabel#cardGlyphChip`
  (`app/ui/theme/exosnap_dark.qss:1114-1119`); keep the accent-tinted glyph, bump icon 15→18px
  (`app/pages/ConfigPage.cpp:507-510`). Replace the too-generic **Presence "activity"** glyph with a
  more specific one (bell/broadcast) in `cardGlyphPathFor` (`ConfigPage.cpp:438-439`).
- **Preset toolbar:** wrap the preset row in the canon slim-toolbar container (surf bg, 1px line,
  radius 12, slim padding). Keep our functions (Save as new / Reset / overflow / Export / Import),
  restyle to canon pills: the primary action as **ghost** (transparent, 1px border, pill), the
  secondary/overflow items as **quiet** (borderless icon+text pill).
- **Info-i rework:** keep the icon **after the label** (canon), but only on rows with a genuine
  A/B tradeoff (Container, Video/Audio codec, Quality, Frame rate, Frame timing, Colour range, Bit
  depth, Chroma, HDR handling, Encoder preset, Frame pacing, Keyframe interval). Simple boolean
  rows lose the icon. **All inline helper/subtitle text moves into the popover** (owner preference:
  "lieber im info-i als drunter" — keeps the expert view uniform and allows longer text). Absorbed
  lines include: "VFR is available for MKV/WebM…" (→ Frame timing), "Open editor when finished: On
  — jumps to Edit…" (→ that row), "Position and size are configured in the Record preview." (→
  Webcam). **Never two info-i in a row** — audio source rows drop to one.
- **Expert-mode banner:** shorten to a short sentence + info-i carrying the detail (first instance
  of the pattern above).

### 3.2 Settings — structure / content

- **Cogwheels → inline** (remove all three `SettingsPopoverRow` gears in the audio-expert section,
  `ConfigPage.cpp:4103-4239`):
  - *Audio clock slaving* (empty popover) → plain inline toggle.
  - *Brickwall limiter* → inline toggle; when on, the ceiling spin appears inline (right-aligned).
  - *Microphone post-processing* → inline **disclosure** (chevron) expanding the four stage rows
    (HPF / Gate / AGC / RNNoise) within the card.
  - `SettingsPopoverRow` has no other users → remove the component + its tests
    (`app/ui/widgets/SettingsPopoverRow.*`, `test_config_page.cpp:1796-1845`).
- **Output card:**
  - Remove the redundant "Output: Native · 60 fps · …" line (not canon). Keep "Current format" in
    Quality & timing — it is the compatibility preview (`compat_ok_label_`, toggles exclusively with
    the caution callout, `ConfigPage.cpp:1167-1173`, `:2687-2708`).
  - Keep the destination-folder input + Browse, and the filename input (owner instruction).
  - Fix the filename-pattern label→input spacing: the `fn_row` wrapper's `QToolButton`
    (`tokens_toggle_`) inflates the row height vs the plain-label rows (`ConfigPage.cpp:1486-1504`);
    flatten it to label height.
  - Fix the tokens-disclosure empty space: `ChipFlowWidget::minimumSizeHint()` sums all chip heights
    as if stacked (`ConfigPage.cpp:177-189`) — return a flow-based height (same fix class as the
    webcam card).
  - "Saves as <path>" — middle-ellipsize to a single line.
- **Webcam card:** reduce the wasted vertical space in the card (shorten height/spacing). Do **not**
  collapse the preview on off — the 1-source-N-consumer capture hubs let the webcam preview run even
  during recording, so the box stays; only the empty padding shrinks.
- **Hotkeys card (embedded):** remove dead buttons — an unbound row shows only **Set**; **Clear** /
  **Reset** appear only once a binding exists (`hotkeys` rows).
- **Presence card → rename "Notifications & overlays"** (current name is imprecise for
  overlays + notifications + tray). Swap its header icon accordingly.
- **Expert-view structure** (answer to a–d): **not** more cards (a); collapsible only for genuinely
  deep clusters (b, e.g. the mic-DSP disclosure above); **yes relevance-gating (c)** — show/enable
  controls only when the active codec/container/display supports them (HDR only on an HDR display —
  already partly spec'd §6; 4:4:4 only when codec+GPU support it; 10-bit only when the codec carries
  it), so the expert view shrinks to what's relevant instead of listing inapplicable rows; **yes
  light regroup (d)** — move Frame pacing (timing) into Quality & timing, keep format-identity rows
  in Container & codecs.

### 3.3 Diagnostics — simple view redesign

Replace the centred-float composition with a **top-anchored readiness dashboard** that fills the
height: verdict hero as a header band (icon + "Ready to record" + subline + Run-check / last-check),
then a responsive tile grid with **more than four tiles** — Readiness, Encoder, Disk, Display, plus
Audio and Capture target (and, space permitting, last session) — with the tip chip integrated. Keep
the calm, non-alarmist ethos; blockers always visible. Pixel iteration via the `--visual-test`
harness (`diagnostics`, `diagnostics-blocked`, `diagnostics-issues`, `diagnostics-post`).
**Product-spec §11 update required** (drop "exactly four").

### 3.4 Logs

- Give log entries row separators / subtle zebra (current `QPlainTextEdit` → custom rendering or a
  ListView/delegate). Long-standing open item.
- Restyle the Startup table to the card visual language (it currently reads as a spreadsheet grid).

### 3.5 Hotkey-unavailable notification + Rebind landing

- Finish the working-tree WIP: the unregisterable binding is dropped and persisted via the
  `<unset>` sentinel so it fires once, not every launch (`GlobalHotkeyService` save/load,
  `MainWindow::showEvent`). Verify + commit; keep the tests.
- **Add a landing affordance** to `ConfigPage::scrollToSection` (`ConfigPage.cpp:5826-5850`): after
  `ensureWidgetVisible`, briefly pulse an accent border (~1.2 s) on the target panel and focus its
  primary control, so the Rebind deep-link gives visible feedback even when the card is already in
  view (today it silently no-ops).

## 4. Slices (reviewable PRs, own branches, automerge after green gate)

1. **Hotkey notification finish + Rebind landing** (§3.5). Small, self-contained, WIP present. First.
2. **Settings chrome** (§3.1 + Output-card fixes, webcam spacing, hotkey-button cleanup, audio
   1×info-i). Mostly QSS + layout. Fable-suitable.
3. **Cogwheels → inline** (§3.2 first bullet) + remove `SettingsPopoverRow`. UI-only, medium.
4. **Expert-view relevance-gating + regroup + Presence rename** (§3.2 last bullets). Logic-bearing;
   Sonnet/Opus.
5. **Diagnostics simple-view redesign** (§3.3). Design-heavy, pixel iteration; Opus.
6. **Logs zebra + Startup table** (§3.4). Medium.

## 5. Testing

- Widget tests per surface (`test_config_page`, diagnostics/logs tests); update popover tests when
  removing `SettingsPopoverRow`; add a test that Rebind's landing pulse/focus fires.
- Full build + `ctest` after each slice (per project rule: `--target exosnap` does not build tests).
- Judge pixels via `--visual-test` at 1440×960; never drive the live app.

## 6. Spec / ADR impact

- **product-spec.md §11:** replace "exactly four wide readiness tiles" with the new tile set.
- **product-spec.md §12:** the info-hint model stays accurate (hover popovers on info-i); note
  selective placement.
- **Presence → Notifications & overlays:** update any spec reference to the card name.
- No ADR needed (no new product decision; the deferred pages get their own specs later).
