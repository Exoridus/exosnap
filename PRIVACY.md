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
- **What is sent (allowlist).** If you choose to send a report, the following fields are
  included: OS build, GPU model and driver version, ExoSnap version, active encoder backend,
  container and codec, and the crash stack/minidump. No other data is included.
- **What is never sent.** Usernames, file paths (including your chosen output folder and
  recording filenames), and machine name are stripped before the report leaves the process.
  Breadcrumb logs are disabled (`enable_logs=0`). Recording content is never captured.
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

## Update channel

When the update check is enabled (opt-in), ExoSnap contacts the **public GitHub Releases API**
to compare the installed version against the latest release. No authentication token is used.
This request transmits your IP address to GitHub (GitHub Inc., USA) and the ExoSnap version
string. No other data is sent. Update channel downloads are hosted as GitHub Release assets.
No ExoSnap-operated server is involved.

## Both network features are off by default

Neither the update check nor crash reporting runs unless you opt in. Self-built binaries never
include the crash-reporting ingest key and never upload. You can use ExoSnap fully offline.

## Contact

Questions about privacy: <github@codexo.de>
