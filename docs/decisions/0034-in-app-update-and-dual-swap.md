# ADR 0034: In-App Update Flow and Dual-Swap Updater

## Status

Proposed. Phase A (in-app check → notify → deep-link → "Update available · vX.Y") is small,
low-risk wiring over existing primitives and ships first. Phase B (the actual in-place download →
verify → swap → restart for **both** portable and MSI) is a separate, security- and
correctness-critical slice gated on this ADR. Builds on the existing `UpdateService`
(`RequestUpdateCheck`, `RequestDownloadAndVerify`, `packageReadyForInstall`, `HandoffToInstaller`,
signature verification), `NotificationManager` (`NotificationType::UpdateAvailable`,
`NotificationAction::OpenUpdate`), the Settings deep-link (`ConfigPage::scrollToSection("settings/updates")`),
and `UpdateSettingsPanel`/`UpdateUiState`. Relates to [[0017-crash-reporting-architecture]].

## Context

The update backend already checks for releases, downloads, and **verifies signatures**, and can hand
off to an external installer. The shipped UX deliberately stops at "notify + open releases page" — no
in-place updater (`UpdateUiState` has only UpToDate/Checking/Available/Error). The product design
wants the full loop: check (manual or auto) → notification (only when not user-initiated) →
clicking the notification lands in Settings → "Update to vX.Y" → progress with status labels →
install → restart.

Two hard constraints shape any design:

1. **A running `.exe` is locked.** Windows forbids overwriting/deleting the running image (rename is
   allowed). This applies to **both** distributions: portable ZIP *and* MSI (an MSI replacing the
   running `exosnap.exe` hits Windows Installer "files in use"). Therefore the actual file swap can
   never happen fully in-process — the app must exit and an **external agent** performs the swap and
   relaunch.
2. **We are unsigned** (SignPath reputation-gated; see release notes). An auto-launched installer
   trips SmartScreen/UAC. That is expected for the MSI path. The portable self-swap can avoid
   SmartScreen entirely because we verify the package signature/hash ourselves and can strip the
   Mark-of-the-Web before relaunch.

So "completely in-app" is achievable for everything *except* the swap instant: download + verify run
in-app with a real progress bar; the swap+restart is a brief external step.

## Decision

### Shared in-app frontend
One progress UI in the Settings "Updates" card, driven by `UpdateService`, used by both
distributions. New `UpdateUiState` values: `Available` (already exists), `Downloading`
(determinate %), `Verifying`, `Installing` (indeterminate), `Restarting`. Granular percentage lives
in the **download/verify** phase (fully in-app); the swap step shows a short indeterminate
"Updating — the app will restart…".

### Dual swap backends behind one frontend
| | MSI (`install_mode == Installed`) | Portable |
|---|---|---|
| Download + verify | in-app (shared UI) | in-app (shared UI) |
| Swap agent | `msiexec /qn` launched **elevated (one UAC)** from a thin helper; Windows MSI UI hidden | small **swapper sidecar** (`exosnap-updater.exe`) |
| Elevation | UAC (Program Files) | none if install dir is user-writable |
| SmartScreen | expected (unsigned installer) | avoided: self-verify + strip MOTW |
| Restart | helper relaunches `exosnap.exe` | swapper relaunches `exosnap.exe` |

`install_mode` is detected at runtime (presence of the MSI ProductCode registration / write-access to
the install dir). The frontend is identical; only the swap agent differs.

### Sidecar swapper (portable) mechanics
- App stages the verified new build in a temp dir, writes a swap manifest (target dir, version,
  rollback marker), launches `exosnap-updater.exe`, and exits.
- Swapper waits for the main process to exit, **atomically** replaces app files (rename old → backup,
  move new → live), relaunches `exosnap.exe`, and on a successful health-check (new process reports
  "started vX.Y") deletes the backup. On failure it restores the backup (**rollback**).
- **Updater-replaces-updater:** the swapper cannot overwrite its own running image, but the *main app*
  can — so the app replaces `exosnap-updater.exe` (renaming a running image is permitted; on next
  launch the renamed `*.old` is cleaned). The swapper is kept **minimal and version-stamped** so it is
  swapped only when its own version actually changes (rare).
- **Loop guard:** a persisted "applied version" stamp + manifest consumption ensures a completed
  update is never re-applied.

### Phasing
- **Phase A (now):** wire the existing primitives — manual "Check now" + auto-check toggle →
  `RequestUpdateCheck`; on an **auto** check that finds a release, enqueue an `UpdateAvailable`
  notification (`OpenUpdate` action); the notification/toast action deep-links to
  `settings/updates`; the card renders `Available · Update to vX.Y` whose action opens the releases
  page (interim hand-off). No swap. (Fix the stale `"update-view" → About` deep-link to
  `settings/updates`.)
- **Phase B:** the dual-swap (download/verify progress UI + msiexec-elevated and sidecar swapper),
  atomic staging, rollback, MOTW strip, loop guard, and the MSI `UpgradeCode` work below.

## Consequences

- **Bricking risk is real.** A faulty swap can leave a broken install. Mitigations are mandatory:
  atomic staging, keep-old-until-healthy rollback, post-start health-check, and a manual recovery
  path (re-download). The swapper must be tiny, dependency-free, and heavily tested.
- **MSI in-place upgrade requires a stable `UpgradeCode`** across versions (each release currently
  ships its own ProductCode). Must be verified/fixed in packaging before Phase B MSI works.
- **Two unavoidable non-in-app moments:** the UAC prompt (MSI only) and the brief window where the app
  is closed during the swap (both). The transition is kept short and branded; graceful SmartScreen/UAC
  handling is acceptable per product.
- **Portable can update without UAC or SmartScreen** when the install dir is user-writable and MOTW is
  stripped post-verification — the cleaner of the two paths.
- Phase A delivers the visible designed flow up to "Update to vX.Y" with negligible risk and is
  independent of the swap work.
