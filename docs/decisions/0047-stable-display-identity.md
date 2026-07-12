# ADR 0047: Stable display identity for saved capture targets

## Status

**Accepted — 2026-07-12.** Supersedes the "stable identity deferred" position recorded in
ADR 0005, and amends the description-based `display_key` / `region_display_key` persistence
described in ADR 0003.

## Context

A saved Display or Region capture target used to be remembered by the GDI device name
(`\\.\DISPLAYn`) — persisted, via `RecordViewModel::TargetLabelFromCaptureTarget`, as a
description string such as `"Desktop - Display 6"`. Windows reassigns GDI names and their trailing
numbers on any topology change (monitor unplug/replug, KVM/EDID renegotiation, mode-set, reboot in
a different port order). Two silent failures followed:

1. **Silent mis-hit.** The saved number matched a *different* physical monitor after a reshuffle —
   the user recorded the wrong screen with no signal.
2. **Silent loss.** The number matched nothing — the selection went blank and had to be re-picked
   wordlessly.

Region rectangles compounded the problem: they persisted as absolute virtual-screen pixels, so a
layout change left them on the wrong pixels or off-screen.

The runtime capture machinery (DXGI/WGC hub keying, preview, HDR) already re-resolves the live GDI
name at capture start (`DxgiOdCaptureSrc::Reopen`); it is *not* affected by this problem and is left
untouched. Stable identity is purely a persistence/restore concern.

## Decision

### Composite identity, ranked matcher

Persist a structured `StableDisplayId` (device path, EDID vendor/product, serial, friendly name,
GDI name, sequence hint) and resolve it at restore with a pure ranked matcher
(`ResolveStableDisplay`), first hit wins:

1. `device_path` exact (`DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath`) — same connector.
2. `{edid_vendor, edid_product, serial}` exact, serial non-empty — same panel at another port.
3. `{edid_vendor, edid_product}` matching exactly one connected display — a unique single monitor of
   its model. Two or more identical monitors without a serial are **not** disambiguated.
4. `gdi_name` exact, **only** when the saved identity never carried a device path (a degraded save,
   e.g. DisplayConfig unavailable under RDP) — never worse than the old match, and never a silent
   mismatch for a rich identity.

No match → **UNRESOLVED**: the selection stays empty and a calm Display notice is raised. The matcher
never guesses among ambiguous twins.

Alternatives considered and rejected as the *primary* key: AdapterLUID + output index (LUID is
boot-unique, not persistent; index is topology-order — the very thing we fix); a raw SetupAPI EDID
enumeration (most new native code, over-engineered for this scope). The device path (primary) is
enriched by EDID (best-effort) so we get port stability *and* panel-follow where a serial exists.

### Serial is provisional

`serial` is read best-effort from `QScreen::serialNumber()` (historically unreliable on Windows'
QPA). A once-per-process gate log records vendor/product/serial occupancy so a live verify can decide
whether ranking stage 2 carries real data. When no serial is available, stages 1/3/4 carry the match;
the twins-after-cable-swap case is documented as the honest remaining boundary. A native EDID read
over the persisted device path is the documented escalation if the gate log shows the panel-follow
stage is worth it.

### Region is anchor-relative and physical

The region is persisted as normalized [0,1] fractions of its **anchor display's physical
`MONITORINFOEXW.rcMonitor`** (not `QScreen::geometry()`, which is DPI-scaled/logical), plus the
anchor's `StableDisplayId`. A capture region (`recorder_core::CaptureRegion`) is in physical
virtual-screen pixels, so normalizing against the logical geometry would break the round-trip on any
scaled display. On restore the rectangle is recomputed from the current anchor geometry and clamped;
a missing anchor skips the region rather than dropping a stale rectangle onto the wrong pixels. This
is deliberately proportional (not pixel-exact) so a resolution change carries the rectangle.

### Purity of the save path

`RecordPage::currentCapturePolicy()` is called on every dirty/live-config check via
`MainWindow::captureLiveConfig()`. Enumeration (`QueryDisplayConfig` + N × `DisplayConfigGetDeviceInfo`
+ Qt join) must never run there. The `StableDisplayId` is resolved once at selection/region-change
time and cached in the view model; the save getter only reads the cache. A save-time mapping failure
writes at least the GDI name so the preference is not silently lost (matcher stage 4 carries it).

### Notice lifecycle

The "Saved display not found" Display notice is a projection of an `unresolved` flag. It clears when
the user manually re-selects (a manual pick is resolved and sticky) or when the target display
returns and the user has not re-chosen (`RecordPage::onDisplaysChanged` re-resolves the cached
identity and restores the selection). A sticky bit distinguishes "restore the declared wish" from
"do not override a deliberate manual choice".

### Persistence format (breaking, pre-1.0, no migration)

`kPresetSchemaVersion` → 25. The `[capture]` string keys `display_key`/`region_display_key` are
replaced by `[capture.display_id]`/`[capture.region_display_id]` sub-tables plus `region_*_norm`
fields. Per the pre-1.0 policy, older files simply lack the sub-tables: field-wise repair leaves the
identity empty ("no preference"), the saved display target is dropped once and rewritten in the new
stable form on the next save. Not reported as an error.

## Consequences

- Saved monitor targets survive unplug/replug, driver restarts, and port-order reboots without a
  silent mis-hit or silent loss.
- One honest remaining boundary: identical monitors with no EDID serial, cable-swapped between ports,
  resolve to UNRESOLVED + notice rather than a guess (see `KNOWN_LIMITATIONS.md`).
- Region restore is proportional, not pixel-exact, by design.
- The engine stays UI-agnostic: the impure enumerator and the notice live in the app layer; the
  matcher and region math are pure and unit-tested.
