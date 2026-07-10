# Notifications — design

Status: approved for planning
Date: 2026-07-10

## Why

Four complaints, one root cause.

Toasts pile up three deep, they always appear on the primary monitor, a toast that
carries a title and a single button is mostly empty space, and the Undo offered after a
preset switch expires while the user is still reading it.

The root cause is that **the notification hub is not a record of anything**. It keeps its
own list, and exactly one entry is ever written to it (`recovery-available`). Every other
notification exists only as a toast. A toast that is the sole carrier of its message
cannot be allowed to disappear, so three types were made permanent
(`kDismissMs_LowStorage`, `kDismissMs_UnexpectedStop`, `kDismissMs_RecoveryAvailable`
are all `0`), and `kMaxVisible = 3` let them stack. The permanence and the stacking are
symptoms; the empty hub is the disease.

The product spec already describes the intended behaviour ("the most recent is shown",
"auto-dismiss"). The code diverged from it.

## Model

**The hub is the record. The toast is a glance at it.**

Every notification is written to the hub. The toast becomes a transient view of the most
recent one, and losing it costs nothing.

### Timed vs. standing notifications

A notification is *timed* when it reports something that already finished, and *standing*
when it reports a condition that still holds.

| | Timed | Standing |
|---|---|---|
| Examples | recording saved, update available, frames dropped, settings repaired | low storage, unexpected stop, recovery available |
| Timer bar | yes | no |
| Auto-dismiss | yes | never |
| Stacking | no — a new timed toast replaces the current one | yes |
| Dismiss action | the `✕`, or waiting | always an explicit action, never only the `✕` |

This yields one rule with no exceptions: **a timer bar appears exactly when the
notification leaves on its own.** A standing notification has no bar because nothing is
counting down, and it must offer a way out because nothing else will remove it.

At most one timed toast is visible. Standing toasts stack above it.

### Placement

Toasts anchor to the bottom-right of the screen that currently hosts the ExoSnap window,
not to `primaryScreen()`. The anchor is recomputed when the window changes screen.

The DXGI preview overlays are out of scope and are not touched.

### Toast layout

The card grows to fit its content. There is no reserved space for an absent body and no
button strip for a single action.

- **No action** — title, optional body, `✕`. The card is not clickable.
- **One action** — the card *is* the action. No button; a `›` marks it. Clicking the card
  runs the action, `✕` dismisses.
- **Two actions** — named buttons in their own row.

This follows the platform convention: the notification body is the default action, extra
actions become buttons.

Consequently `Saved` offers **two** actions — open the destination folder and edit the
recording — rather than a lone `Edit` floating in an empty strip.

### Preset switching

The preset-switch toast is removed. Since the live configuration became the source of
truth, switching back is a matter of reselecting the previous preset from the same
combo box that performed the switch. A notification whose only purpose is to offer an
action already available two centimetres above it is noise.

The hub entry with Undo remains, because the hub is the record.

## Components

- `NotificationEvent` — gains the notion of a second action being ordinary rather than
  exceptional; `PresetSwitched` leaves the toast path.
- `NotificationManager` — replaces `kMaxVisible = 3` with the timed/standing split:
  one timed slot, unbounded standing slots. Every enqueued event is also handed to the
  hub. The dismiss interval table stays, but `0` now *means* standing rather than
  encoding a workaround.
- `NotificationHubPanel` — receives every notification, not just recovery.
- `NotificationToastWindow` — screen anchoring follows the app window; the card height is
  derived from present content; the one-action card becomes clickable.

## Testing

Widget tests, no live app:

- a timed toast replaces the previous timed toast, and never a standing one
- a standing toast never auto-dismisses, and always carries a non-`✕` action
- a timer bar is present if and only if the notification is timed
- every enqueued notification produces exactly one hub entry
- the toast anchors to the screen of the app window, including after a screen change
- card height for (title), (title+body), (title+1 action), (title+2 actions) — pinned via
  the `--visual-test` render harness, judged on rendered pixels

## Limits

Whether the redesigned toast *feels* right on a real multi-monitor desktop with mixed DPI
can only be judged by a human running the app. The tests pin geometry and behaviour, not
taste.
