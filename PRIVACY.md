# Privacy Policy

**Effective date:** 2026-06-13

ExoSnap is a local, Windows-native screen/application/region recorder. This document
describes what data ExoSnap processes. It reflects the **current** state of the software and
will be updated if data processing ever changes.

> This is a plain-language statement of fact about the software's behaviour, not legal advice.

## Summary

ExoSnap does **not** collect, transmit, or share any personal data. It has no telemetry, no
analytics, no account system, and makes **no network connections** during normal operation.
Everything stays on your computer.

## Data stored locally (never transmitted)

ExoSnap stores the following on your machine only:

- Application settings — `%LOCALAPPDATA%\ExoSnap\settings.ini`
- Recording presets — `%LOCALAPPDATA%\ExoSnap\presets.ini`
- Recording history — `%LOCALAPPDATA%\ExoSnap\recording-history.json`
- A crash-recovery manifest while a recording is in progress — `%LOCALAPPDATA%\ExoSnap\`
- Diagnostic / startup logs — local log files
- Your recordings — saved to the output folder you choose

None of this leaves your device. You can delete any of it at any time.

## Network features (planned — not present in current releases)

Current ExoSnap releases make **no** network connections. Future versions may add the following
features. When they ship, this policy will be updated, and the features will be **off by default**
(opt-in):

- **Update check** — would contact a server (for example the GitHub Releases API) to compare the
  installed version against the latest release. Any such request necessarily transmits your **IP
  address** to that server (and, where GitHub is the host, to GitHub Inc. in the USA), together
  with the current application version. No other data would be sent.
- **Crash reporting** — would, **only with your explicit consent**, send crash diagnostics (stack
  traces and limited system information, with sensitive paths scrubbed) to help diagnose defects.

Neither feature exists today.

## Contact

Questions about privacy: <github@codexo.de>
