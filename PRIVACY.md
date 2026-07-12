# Privacy Policy

**Effective date:** 2026-06-15

ExoSnap is a local, Windows-native screen/application/region recorder. This document
describes what data ExoSnap processes. It reflects the **current** state of the software and
will be updated if data processing ever changes.

> This is a plain-language statement of fact about the software's behaviour, not legal advice.

## Summary

ExoSnap collects **no** telemetry or analytics, has **no** account system, and never transmits
your recordings or personal data. By default it makes **no network connections**. As of 0.4.0,
two strictly **opt-in** features can contact external services, and only when you act: an
**update check** (public GitHub Releases) and **crash reporting** (consent-gated, to Sentry with
EU data residency). Both are detailed below. Everything else stays on your computer.

## Data stored locally (never transmitted)

ExoSnap stores the following on your machine only:

- Application settings — `%LOCALAPPDATA%\ExoSnap\settings.ini`
- Recording presets — `%LOCALAPPDATA%\ExoSnap\presets.ini`
- Recording history — `%LOCALAPPDATA%\ExoSnap\recording-history.json`
- A crash-recovery manifest while a recording is in progress — `%LOCALAPPDATA%\ExoSnap\`
- Diagnostic / startup logs — local log files
- Your recordings — saved to the output folder you choose

None of this leaves your device. You can delete any of it at any time.

## Crash reporting

ExoSnap 0.4.0 introduces opt-in crash reporting powered by **Sentry** (data processor) with
**EU data residency**. A Data Processing Agreement governs this relationship. Crash reporting
is subject to the following guarantees:

- **Opt-in and consent-gated.** Nothing is transmitted without your explicit consent. Upload is
  off by default. A crash-report dialog shows you exactly what would be sent before you decide.
- **Self-builds never upload.** Official builds compile in the Sentry ingest key
  (`EXOSNAP_OFFICIAL_BUILD`); self-built binaries do not include it and never phone home.
- **What is sent (allowlist).** If you choose to send a report, only the structured tag keys
  below can ever survive the scrubber (`crash_scrubber.h`, `kAllowedTagKeys`) — any other tag is
  dropped — plus the crash stack/minidump:

  <!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->
  | Tag key | What it carries |
  |---|---|
  | `os.name` | Windows edition name (e.g. "Windows 11") |
  | `os.version` | Windows build/version string |
  | `gpu.model` | GPU adapter name |
  | `gpu.vendor` | GPU vendor (e.g. "NVIDIA") |
  | `gpu.driver` | GPU driver version |
  | `app.version` | ExoSnap version |
  | `encoder_backend` | Active encoder backend (e.g. "nvenc") |
  | `container` | Output container (e.g. "mkv") |
  | `video_codec` | Selected video codec |
  | `audio_codec` | Selected audio codec |
  <!-- PRIVACY-ALLOWLIST-TABLE-END -->

  **Today, only `encoder_backend`, `container`, `video_codec`, and `audio_codec` are actually set**
  on the Sentry path (`SetEncoderContext`). `os.*`, `gpu.*`, and `app.version` are allowlisted (so
  they would pass the scrubber if a future change sets them) but are not populated by app code
  today — OS build and GPU model/driver currently reach you only through the local crash dialog
  and the opt-in Stage-0 GitHub issue (see docs/product-spec.md §13), not the Sentry upload. This
  table is kept in sync with the code by an automated check
  (`scripts/validate-privacy-allowlist.ps1`, see `docs/privacy-review.md`).
- **What is never sent.** Usernames, file paths (including your chosen output folder and
  recording filenames), and machine name are stripped from the **structured event** before it
  leaves the process. Breadcrumb logs are disabled (`enable_logs=0`). Recording content is never
  captured.
- **The minidump binary is a separate channel from the scrubbed event, and carries module
  paths.** A hard crash uploads (with consent) a Crashpad minidump out-of-process; the
  structured-event scrubber above does not run on it and cannot touch its binary contents. A
  minidump's module list includes the full install path of `exosnap.exe`. For a standard
  Program-Files-style install this is not personal; for a **portable install run from under
  `%USERPROFILE%`**, the username segment of that path can appear in the uploaded minidump. This
  is a real, narrower exception to "paths are stripped" — it applies only to the minidump binary,
  never to the structured event, the Stage-0 GitHub issue, or the crash dialog you see beforehand.
- **No persistent identifier.** No stable device-level identifier is generated or stored. At
  most a per-report random correlation id may be attached for de-duplication within a single
  crash submission.
- **IP address.** Sentry's servers are configured to not store IP addresses (org-level setting:
  Prevent Storing IP Addresses). Transmission to Sentry's EU ingest endpoint still involves an
  IP address in transit, as with any network request.
- **Org-level data hygiene.** The ExoSnap Sentry organisation has Require Data Scrubber,
  Require Default Scrubbers, and Prevent Storing IP Addresses enabled.

Local crash captures (minidumps in `%LOCALAPPDATA%\ExoSnap\crashes\`) are stored on your device
only, whether or not you consent to upload.

## Support bundle (local, user-initiated, never transmitted)

ExoSnap can package its diagnostics into a single `.zip` **support bundle** that you create
manually and share with support however you choose. It is a local file operation only:

- **No transmission.** The bundle is written to a location you pick (save dialog) and then
  revealed in the file manager. ExoSnap never uploads it anywhere — consistent with having no
  telemetry.
- **What it contains.** The rotated application and engine logs, the most recent per-recording
  session reports, and structured facts about your GPU, adapters, displays and current settings.
- **Scrubbing.** Every text entry is scrubbed before it is written: user paths, username and
  machine name are stripped. A capture-target **window title** is neutralized twice: the app no
  longer writes a window's title into `exosnap.log`/`engine.jsonl` in the first place (a stable
  `[window]` placeholder is logged instead — only "window vs monitor capture" is retained, never
  *which* window), and the bundle step additionally redacts any `target="…"` value it still finds
  to `[capture-target]` as a defense-in-depth backstop. Structured files include only a fixed
  allowlist of known-safe fields, never a raw dump of your settings file.
- **What it never contains.** Your recordings, raw settings/preset/history files, absolute paths,
  username, machine name, or crash dumps (crash reporting is a separate, consent-gated channel).

The per-recording **session report** written after each recording
(`%LOCALAPPDATA%\ExoSnap\logs\reports\`) follows the same principle: it holds byte counts,
codecs, and diagnostic counters plus a scrubbed output *file name*, and never an absolute path.

## Update channel

The automatic update check is **off by default** (opt-in) — first launch never contacts any
server. You can turn it on from the Settings update card, or run a manual "Check now" at any
time (a manual check is itself an explicit action, so it needs no separate toggle). When enabled,
ExoSnap contacts the **public GitHub Releases API** to compare the installed version against the
latest release. No authentication token is used. This request transmits your IP address to
GitHub (GitHub Inc., USA) and a fixed User-Agent header identifying the checker
(`ExoSnap-UpdateChecker/1.0` — a protocol version, not your installed app version). **No ExoSnap
version number is sent**; the newest-release comparison happens entirely client-side against the
already-fetched releases list. No other data is sent. Update channel downloads are hosted as
GitHub Release assets. No ExoSnap-operated server is involved.

## Both network features are off by default

Neither the update check nor crash reporting runs unless you opt in — including on first launch.
Self-built binaries never include the crash-reporting ingest key and never upload. You can use
ExoSnap fully offline.

## Contact

Questions about privacy: <github@codexo.de>
