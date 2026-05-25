# ExoSnap Console Reference Pack

This directory is the **authoritative visual reference package** for rebuilding the Qt 6.9 Widgets UI with high fidelity to the chosen Claude Design result.

## How to use this pack

Unzip this archive at the **repository root** so that these files land at:

`docs/design/exosnap-console-reference/`

### Visual authority order

1. `target-screens/*.png`  
   Final target compositions for the key product pages. These define macro layout, density, hierarchy, panel composition, and intended overall appearance.

2. `boards/*.png`  
   Chosen direction boards, chrome/titlebar direction, and the design-system/token board. These define system-level language and cross-screen consistency.

3. `brand-lock/01-logo-style-board-current-lock.png`  
   Current logo/style lock. The brand is **not** open for ideation.

4. `source/claude-design/*`  
   Implementation aids only. Use these to recover layout logic, tokens, styling intent, and component anatomy.  
   **If a source file and screenshot disagree, the screenshot wins.**

5. `implementation-context/*`  
   Current Qt implementation notes / Codex reports. These describe recent implementation status, but they do not supersede visual references.

## Critical fidelity rules

The next UI pass must **reconstruct**, not merely “take inspiration from,” the references.

A result is not acceptable if it:
- keeps the old single-column/form-stack layouts while only changing colors,
- omits the chosen operational chrome/titlebar direction,
- flattens the Record page into a generic Qt settings form,
- simplifies the Video and Audio target pages into materially different compositions,
- replaces the reference sidebar hierarchy with a generic text list,
- treats screenshots as moodboards rather than target outcomes.

## Mandatory comparison scope

Any implementation/review agent should explicitly compare the current Qt app against:

- `target-screens/01-record-interactive-click-start.png`
- `target-screens/02-video-settings-system.png`
- `target-screens/03-audio-sources-tracks.png`
- `boards/04-titlebar-variant-b-operational-56px-recommended.png`
- `boards/06-tokens-surfaces-controls-qt-notes.png`

## Intended next step

Before further implementation, produce a **Design Fidelity Reconstruction Brief** that maps:

- reference layout → current Qt layout,
- exact mismatches,
- required structural rebuilds,
- reusable widgets/components,
- exact vs. approximate Qt feasibility,
- ordered implementation plan.

Recommended reviewer/model for that brief:
- **Claude Opus — max**

Then hand the resulting bounded implementation brief to:
- **Codex — xhigh**
