# Troubleshooting

ExoSnap is diagnostics-first: most problems are already detected and explained in-app, on the
**Diagnostics** page, with a one-click fix where one exists. This guide maps common symptoms to the
card that reports them and what to do. When a problem is hard to pin down, the fastest way to get
help is to attach a **support bundle** (see the last entry).

The tone here mirrors the app's: calm, factual, one primary fix per problem.

## No sound in the recording

- **What ExoSnap shows.** The **Audio** diagnostics card flags an audio-format mismatch (for
  example, a 44.1 kHz endpoint against a 48 kHz session) and a source that stopped delivering after
  an endpoint was unplugged.
- **What to do.** Match the output sample rate to your device (Settings → Audio), or let the
  audio-format fix reconcile it. If a device was unplugged mid-session, reselect it. Confirm the
  source rows (APP / SYS / MIC) you expected are enabled; each enabled source becomes its own track
  unless merged with the row above.

## Black screen / black recording (or a frozen game)

- **What ExoSnap shows.** When a **window** capture target is a legacy exclusive-fullscreen game, the
  `rec.capture.exclusive_window` check fires pre-flight (a **Blocker** once capture has demonstrably
  produced no frames, otherwise a **Notice**). If a window switches to exclusive fullscreen *during*
  a recording, a standing notice reports that the video is frozen until frames return — the recording
  is not silently black.
- **What to do.** Use the **"Record the monitor instead"** fix (monitor capture can record exclusive
  fullscreen; you confirm a short summary — the whole monitor is recorded and the per-application
  audio row drops to System/Microphone). Or switch the game to **borderless / windowed fullscreen**
  to capture the window directly. For a specific display, confirm you selected the intended monitor.

## "Encoder unavailable" or the recording will not start

- **What ExoSnap shows.** An **Encoder** blocker names the required NVENC/driver minimum, with an
  **External** fix action that shows the exact version and a deep link (ExoSnap cannot install a
  driver for you).
- **What to do.** Update the GPU driver to at least the version shown, then re-run the check.
  Recording start stays blocked until the blocker clears — that is by design.

## Stutter / judder in an otherwise smooth game

- **What ExoSnap shows.** With the opt-in, elevation-gated **present/tearing/latency** provider
  enabled, Diagnostics correlates VRR-vs-CFR judder and encoder-bound frames, and names the kernel
  driver behind DPC/ISR latency spikes.
- **What to do.** Enable the provider (requires running as Administrator; the app offers a
  self-relaunch), then follow the named correlation. Frame pacing can be switched to phase-correct
  where Diagnostics recommends it.

## Recording looks too dark or washed out in VLC

- **What ExoSnap shows.** A color-range compatibility notice: some players (VLC) ignore the range
  flag and always expand limited→full, so a Full-range recording looks crushed there.
- **What to do.** Apply the **Full → Limited** color-range fix (the ecosystem default), or keep Full
  only for players you know honour the flag.

## An update will not install

- **What ExoSnap shows.** The updater surfaces explicit error states and restores the previous
  version automatically if verification fails at any step.
- **What to do.** Retry the update; for MSI installs, accept the single UAC prompt. Updates never run
  during an active recording or finalization. If it keeps failing, attach a support bundle.

## Recording stopped on its own (low disk / FAT32 4 GiB)

- **What ExoSnap shows.** The **Disk** card reports low free space and the FAT32 per-file 4 GiB
  limit. A low-disk hard stop ends the recording cleanly rather than corrupting it.
- **What to do.** Free space or record to a drive with more headroom; for very long recordings avoid
  FAT32 output targets (use NTFS/exFAT), or rely on split so no single segment approaches 4 GiB.

## "I want to share my logs" — create a support bundle

- **What ExoSnap shows.** A **Create support bundle** button on the **Logs** page (also reachable
  from the **Diagnostics** page).
- **What to do.** Click it, choose where to save the `.zip`, and share that file with support. It
  contains the rotated logs, the recent per-recording session reports, and your GPU/adapter/display
  facts — with paths, username, machine name and capture-target window titles scrubbed. Nothing is
  uploaded automatically; you decide where it goes. See `PRIVACY.md` for exactly what is and is not
  included.
