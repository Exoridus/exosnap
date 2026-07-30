# What's-new overlay redesign — design

Status: approved for planning
Date: 2026-07-30

## Why

Live-verify of the RC5 update round-trip surfaced several small but real gaps in the
"What's new" surface (`app/ui/dialogs/WhatsNewOverlay.cpp`, `app/services/WhatsNewPayload.*`,
the Settings → Updates card):

- The Settings-card link reads "What's new in vX.Y" — reads as a heading, not an action.
- The Updates-card "Update to X" button uses a dim, muted accent tint
  (`QPushButton#updatesActionButton[updatesCta="true"]`) instead of the app's existing
  solid primary-button pattern (`QPushButton[role="primary"]`), so it doesn't read as the
  primary action it is.
- The overlay shows one collapsible section per version (newest expanded, older
  collapsed behind a click) for the notes in the update gap `(installed, target]`. That
  matches today's product-spec text, but a plain, always-expanded, single scrolling
  document is easier to skim and matches every other read-only text surface in the app.
- The overlay's footer already has a flat, accent-colored "All releases" link and an
  accent-colored primary "Close"/"Got it" button — both close to right, visually
  adjacent. There's no external-link affordance on the releases link, and it isn't
  anchored to the opposite corner from Close.
- The pre-update entry point ("What's new in vX.Y" on the Settings card) only shows the
  gap notes from the last check, not the reference list of everything shipped on the
  current channel.

None of this needs new infrastructure: `libs/update`'s `CollectReleaseNotes` already
parses the *entire* GitHub `/releases` JSON array that the update check fetches (see its
doc comment — "no extra network call is needed"); it just bounds the result to the gap.
Getting the full channel history is a bounding change, not a new data source.

## Scope

Two independent-but-related fixes, done together because they touch the same files:

1. Settings-card copy/styling (link rename, primary-button color) — cosmetic, no logic
   change.
2. Overlay content and footer redesign — the real work: a new engine-side query, a
   rendering rewrite (collapsible sections → single `QTextBrowser`), a footer layout
   change, and a checkbox polarity flip.

## Settings card

- `ConfigPage.cpp`'s `"What's new in %1"` (`updates_whats_new_link_`, line ~6315)
  becomes `"See what's new in %1"`. Text-only change.
- `exosnap_dark.qss`'s `QPushButton#updatesActionButton[updatesCta="true"]` rule is
  restyled to the same tokens as the existing `QPushButton[role="primary"]` rule:
  `background: ${accent}`, `color: ${accent-ink}`, hover `background/border-color:
  ${accent-hover}`. No C++ change — `updatesCta` stays the gating property, only its QSS
  colors change.

## Engine: all-channel release notes

`libs/update` gets a new pure function alongside the existing `CollectReleaseNotes`:

```cpp
// Collect the release notes for every non-draft release on `channel`, newest first, from
// the same already-fetched /releases JSON. Unlike CollectReleaseNotes this has no lower
// bound — it is the full reference list for the channel, not a gap. Same channel rule
// (Stable excludes prereleases, Preview includes them). Bounded by whatever the fetch's
// per_page returns (currently 30, unpaginated) — acceptable for now (14 real releases
// today); revisit if the channel's release count approaches that cap.
std::vector<ReleaseNote> CollectAllReleaseNotesForChannel(std::string_view releases_json,
                                                          UpdateChannel channel);
```

`UpdateCheckResult` gets a new field, `all_channel_notes`, populated **unconditionally**
on every successful check — not gated by `update_available` like `gap_notes` is, because
the pre-update "See what's new" link must work even when the app is already up to date.
`UpdateService` exposes it as `LastAllChannelNotes()`, mirroring the existing
`LastGapNotes()`.

`gap_notes` / `LastGapNotes()` are unchanged and keep their existing meaning and call
site (the post-update auto-show payload written by `LaunchUpdater()`).

## Overlay: content source per entry point

- **Pre-update** (Settings-card link, `post_update_mode=false`): switches from
  `LastGapNotes()` to `LastAllChannelNotes()` — the reference list for the active
  channel, not just the pending gap.
- **Post-update** (one-time auto-show, `post_update_mode=true`): **unchanged** — still
  `LastGapNotes()` via the persisted `whats-new-pending.json` payload. Showing the whole
  channel history right after an update would bury "what changed for you" under
  everything else ever shipped; the gap is what's relevant here.

Both entry points render through the same rewritten overlay (below); the only
content-source difference is which note list is passed in, exactly as today.

## Overlay: rendering rewrite

`WhatsNewOverlay::buildCard()`'s per-version loop (checkable `QPushButton` header +
toggleable `QLabel` body, inside a `QScrollArea`) is replaced with a single read-only
`QTextBrowser` (`objectName: whatsNewNotesBrowser`):

- One HTML document is assembled from `notes_`: for each note, a small non-interactive
  version heading (`v0.9.0-rc5`) followed by its Markdown body (rendered via
  `QTextDocument::setMarkdown`, same as today), newest first, separated by a rule between
  versions. Set once via `setHtml()`.
- `setOpenExternalLinks(true)` so links inside release-note bodies still open in the
  browser, matching today's `QLabel` behavior.
- The `QScrollArea` wrapper is removed — `QTextBrowser` scrolls itself. Word-wrap is its
  default behavior.
- New QSS: `QTextBrowser#whatsNewNotesBrowser { background: transparent; border: none;
  color: ${text1}; font-size: 13px; }` (matches today's `whatsNewBody` text tokens). The
  now-unused `whatsNewHeader`/`whatsNewBody` rules are deleted.

## Footer layout

- The "All releases" link (`whatsNewAllReleasesBtn`) moves to the far left of the footer
  row (added *before* the stretch instead of after) and gains the existing
  `external-link` Lucide icon (`ui::theme::lucideIcon("external-link", ...)`) to its
  left, signaling it opens outside the app. Its QSS (flat, accent-colored, underline on
  hover) is unchanged.
- `Close`/`Got it` stays pinned to the far right (already primary-styled via
  `whatsNewCloseBtn`'s existing QSS — no change there). The button text keeps its
  existing per-mode distinction: `"Got it"` for the one-time post-update acknowledgment,
  `"Close"` for the pre-update reference lookup.
- In post-update mode, the suppress checkbox moves to its own row **above** the
  link/Close row, instead of sharing the row with the other two controls.

## Checkbox polarity flip

The post-update checkbox's label changes from "Don't show this after updates" (default
unchecked = shown) to **"Show release notes after updates"** with **default checked**.
The underlying persisted setting, `whats_new_suppressed`, is unchanged in meaning and
storage — only the checkbox's displayed/checked state and the signal payload invert:
the checkbox shows `!whats_new_suppressed`, and toggling it emits
`suppressToggled(!checked)` so `MainWindow`'s existing
`persisted_settings_.whats_new_suppressed = suppressed` assignment keeps working
unmodified.

## Testing

- `libs/update`: new unit tests for `CollectAllReleaseNotesForChannel` — Preview includes
  prereleases, Stable excludes them, drafts are skipped, malformed JSON yields an empty
  vector (mirrors `CollectReleaseNotes`'s existing test shape).
- `UpdateChecker`/`UpdateService`: a test asserting `all_channel_notes` is populated even
  when `update_available` is false (the behavior that differs from `gap_notes`).
- `WhatsNewOverlay`: existing widget tests (if any target the removed collapsible
  headers) are updated for the `QTextBrowser`; a new test asserts the checkbox polarity
  flip (checking "Show release notes after updates" emits `suppressToggled(false)`, not
  `true`).
- Visual proof via `--visual-test` for both overlay variants (pre-update, no checkbox;
  post-update, with checkbox), current theme.

## Docs

`docs/product-spec.md`'s "What's new (shipped)" paragraph (~lines 1114–1127) is rewritten
to describe: gap notes for the post-update auto-show only; the full channel history
(Preview includes RCs, Stable does not) for the pre-update link; the always-expanded
single-document layout (no more collapse/expand); the renamed link; the inverted,
default-checked suppress checkbox; and the footer's external-link icon + left/right
anchoring.

## Out of scope

- Pagination beyond the existing single-page (`per_page=30`) fetch.
- Changing when the Settings-card link is visible (stays "available state only").
- Any change to the update-check/download/install mechanics themselves.
