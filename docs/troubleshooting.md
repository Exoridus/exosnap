# Troubleshooting

Start with Diagnostics. A blocker prevents a start; an advisory does not. Unavailable means no measurement, not a healthy zero. [Known limitations](../KNOWN_LIMITATIONS.md) defines support, and [Privacy](../PRIVACY.md) explains diagnostic sharing.

## Recording will not start

Read the blocker's own reason. Confirm a source is selected, the output destination is reachable and writable, and the selected format is supported by the capture adapter's NVIDIA NVENC encoder. ExoSnap has no AMD, Intel or CPU encode fallback. A GPU listed in Hardware capabilities is not necessarily the adapter driving the captured display.

For an unavailable encoder, use the driver guidance in the diagnostic. An encoder-in-use error can require closing another recording/streaming application. A transient device error does not by itself prove the hardware is unsupported. After correcting the cause, retry; do not bypass the readiness gate.

For a missing runtime DLL at launch, install the Microsoft Visual C++ x64 Redistributable using the link in the [portable guide](../README-PORTABLE.md). Keep the entire portable folder together, including Qt, FFmpeg and plugins.

## Missing sound or a silent interval

Confirm the required application/system/microphone rows are enabled. Application audio contributes only for a specific window target. Check that the player is playing the intended audio track; multiple tracks are not necessarily mixed by every external player. An ExoSnap Edit playback mixes the available tracks for preview.

An endpoint lost during a recording becomes silence while the other sources and video continue. Reconnect the same device. The engine retries automatically; selecting a different source requires a new recording. Default-follow sources can reacquire the current Windows default after loss. A selected microphone that remains usable is not switched merely because Windows changes its default.

A connected but quiet endpoint is not a degraded device. A 44.1 kHz endpoint also does not inherently require a 44.1 kHz output file: capture/output conversion handles supported rates. Use the actual diagnostic rather than changing sample rate as a universal remedy. Another application's exclusive endpoint use must be released before reactivation can succeed.

For long-run drift, inspect the session report and clock-slaving status. Slaving cannot correct a device rate outside its fixed correction envelope. Disabling it is an explicit bit-exact workflow choice, not a synchronization fix.

## Black, frozen or interrupted picture

For legacy exclusive-fullscreen games, use display capture or set the game to borderless/windowed mode. ExoSnap does not inject into a game to capture its exclusive window. The **Record the monitor instead** fix requires confirmation because it records the whole monitor and removes the application-only audio route.

A fullscreen-shaped window with no new frames for ten seconds can raise a capture-stall caution, even when its content is legitimately paused. The notice says what was measured, not that exclusive fullscreen was proven. An ordinary static or minimized window may remain quiet without a notice.

For a display stall, wake or reconnect the selected display as indicated. The recording can hold its last picture while audio continues. A source-size change or incompatible HDR transition ends the recording so the file does not mix incompatible encoder/color states. Restart after the change. Moving a captured window to another display can change its pixel size through DPI scaling.

A saved source that cannot be resolved is not replaced automatically. Select it again. With identical displays or a panel moved onto an existing connector, confirm the live preview identifies the intended panel; saved identity is not a universal physical-panel guarantee.

## Stutter despite smooth source content

Separate real frame losses from intentional CFR coalescing and repeated frames from a quiet/slower source. Start with the live pipeline and frame-pacing information. Lower recording rate or quality/preset cost when capture, GPU processing, encoding or storage cannot keep up.

Use phase-correct pacing where supported for a high-refresh display recorded at a lower CFR. It selects frames; it does not interpolate motion or recover frames never captured. Optional **In-depth diagnostics** adds present and DPC/ISR evidence. It is not required for the ordinary capture measurements. Turning it on offers an administrator restart when needed; it does not elevate silently.

## Unexpected color or HDR playback

Limited-range BT.709 is the compatible SDR default. Full-range interpretation varies between players, so use the Full-to-Limited diagnostic fix for a playback-compatibility problem. A metadata flag alone cannot repair pixels encoded with the wrong conversion.

Native HDR10 needs HEVC or AV1, 10-bit and compatible playback. The in-app preview tone-maps HDR to SDR; it is not an HDR reference display. Its Edit preview uses a reference peak, not the editor monitor's measured peak. Keep source display/HDR configuration stable during a take. Changes to Windows SDR-content brightness can leave a short exposure transition before the next color-state reading.

## Storage, splits and interrupted sessions

An unreachable/unwritable destination blocks admission. Low disk can stop an active recording. MP4 needs space for the transient recording and remuxed output together, plus outstanding segment jobs. A retained edit master can use additional space after a single-file MP4 recording.

FAT32 has a per-file limit. ExoSnap warns but does not enforce a safe automatic split at that limit. Use NTFS/exFAT for long recordings or configure substantially smaller segments; keyframe-aligned size splitting is approximate.

After interruption, **Finish** attempts to save the original configured format. **Continue** starts independent slices, not one seamless repaired file. **Delete** is permanent and asks twice. **Decide later** leaves the offer for a later launch. Preserve the valuable partial recording when repair fails. A failed recovery-manifest write means that recording may not be offered automatically even when a partial file exists.

## Edit, export and markers

Trim is lossless and keyframe-bound, not arbitrary frame-accurate cutting. Nothing changes on disk until Export. A preview decode failure need not prevent a valid stream-copy export. Closing Edit discards an unexported recipe, but an already running export continues.

MKV and MP4 export use a new file or confirmed overwrite. A destination failure can offer **Choose another folder**; follow the actual offered recovery action rather than repeatedly retrying an unchanged invalid path. Markers use a sibling `<stem>.markers.json`, not container chapters. Keep that sidecar with the exported media. MKV and MP4 files with the same stem share that sidecar name.

## Update problems

Automatic checks are off by default. Choose the appropriate Stable/Preview channel and explicitly check. The application-driven Update action authorizes one pinned signed version. The manually launched updater separately asks to check, download and install. A canceled download changes nothing.

Stop recording/finalization before updating. A Scoop-managed installation must be updated through Scoop. A signature or hash failure is not a warning to bypass. Recheck/redownload only through the offered flow. Portable rollback can restore a backup, but a failed restoration must be investigated. An MSI failure is governed by Windows Installer; do not assume ExoSnap verified a rollback outcome it cannot observe.

## Share evidence

Use **Create support bundle** on Logs or Diagnostics. It writes a local ZIP and uploads nothing. Known path/user/machine/window-title forms are scrubbed, but review the archive before sharing arbitrary text that could be personal. Include version, capture kind, format, GPU/driver and a reproducible symptom. Crash dumps are a separate consent-controlled channel and can include module paths.
