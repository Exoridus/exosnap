# Record View

## Purpose

Operational surface for capture target selection, readiness, and start/stop control.

## Structure

1. Preview surface (idle placeholder before recording).
2. Action panel:
   - `READY` / `RECORDING` state label
   - Hotkey badge (`ALT+F9`)
   - Primary CTA (`START`) or destructive CTA (`STOP`)
   - Timer (`00:00:00` idle, live amber timer while recording)
3. Capture target cards.
4. Readiness panel.
5. Existing MVP audio/destination/result panels.

`QUICK TOGGLES` are removed from this view.

## Action Panel Rules

- Idle:
  - `READY`
  - `START` button
  - Timer shown as `00:00:00`
- Recording:
  - `RECORDING`
  - `STOP` button
  - Timer uses live value; if live value is unavailable, show `--:--:--`
- `SIZE` / `EST` rows are no longer part of this panel.

## Preview Bottom Bar Rules

- Idle:
  - Bottom-left hidden
  - Bottom-right `AV1 · CQ 24`
- Recording:
  - Bottom-left `FRAME … ms · BITRATE … · DROP …`
  - Bottom-right `AV1 · CQ 24 · SIZE …`
- If a live value is unavailable, display `–` (never fake static values).

## Honesty Rules

- No static fake runtime values.
- Audio dB labels show `– dB` when no live data is available.
- Audio meters remain flat/dark when not live.

