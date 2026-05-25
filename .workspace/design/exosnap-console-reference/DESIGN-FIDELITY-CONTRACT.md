# ExoSnap Design Fidelity Contract

## Purpose

This document exists to prevent “directional similarity” from being mistaken for **design fidelity**.

The Claude Design references are not moodboards. They are the target visual system to be matched as closely as practical in Qt Widgets.

## Fidelity hierarchy

### Tier 1 — Final target screens
- `target-screens/01-record-interactive-click-start.png`
- `target-screens/02-video-settings-system.png`
- `target-screens/03-audio-sources-tracks.png`

These are the strongest references for page layout, density, major component composition, and value hierarchy.

### Tier 2 — Chosen system boards
- Console direction
- operational chrome/titlebar recommendation
- tokens/surfaces/controls board

These define the cross-screen system: color, type, panel rhythm, status language, iconography direction, and chrome.

### Tier 3 — Source files
The HTML/JSX/CSS files are implementation references. They are useful for:
- spacing clues,
- grouping,
- exact labels,
- panel composition,
- fonts,
- state styles,
- operational chrome anatomy.

They are **not** the visual authority if they diverge from screenshots.

## Mandatory implementation expectations

An implementation pass that claims to “match the Claude design” must address:

1. Operational chrome / custom titlebar direction
2. Sidebar brand/nav/footer hierarchy
3. Record page macro-layout
4. Video page two-column settings system
5. Audio page source-row + resulting-tracks + encoding composition
6. Typography hierarchy
7. Technical value styling / monospaced data treatment
8. Surface and border rhythm
9. Status pills / selected-state language
10. Density and relative proportions

## What may be approximated

Qt Widgets may approximate:
- subtle background textures,
- scanline-like preview fill,
- minute antialiasing differences,
- exact browser/CSS typography rendering.

Such deviations must be **explicitly documented**.

## What must not be casually approximated away

Do **not** omit or radically simplify:
- operational chrome,
- multi-column page compositions,
- prominent preview/action composition on Record,
- structured source rows on Audio,
- effective-output side rail on Video,
- status footer/sidebar rhythm,
- active/selected states.

## Prompt language to use

Recommended phrasing for implementation agents:

> Reconstruct the provided Claude Design references as faithfully as practical in Qt Widgets. The PNG target screens are normative. Do not preserve the current Qt layouts when they conflict with the references. Do not perform cosmetic-only styling passes where a structural rebuild is required. If exact replication is infeasible, implement the closest practical equivalent and document the delta.

