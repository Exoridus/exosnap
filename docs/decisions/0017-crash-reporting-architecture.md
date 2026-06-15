# ADR 0017: Crash Reporting Architecture

## Status

Accepted — implementation in 0.4.0 (Crash Stage 0: local capture + optional upload).

## Context

ExoSnap 0.3.0 has no crash capture. When the process terminates unexpectedly during a
recording session, the only user-visible signal is the recovery overlay at next launch
(ADR 0015). There is no way to diagnose the root cause.

Goals for 0.4.0 Crash Stage 0:

1. Capture a minidump and structured metadata on crash — out-of-process (Crashpad), so
   the handler survives a stack-overflow or heap-corruption in the main process.
2. Privacy-first: no data leaves the device until the user explicitly opts in.
3. Self-build isolation: developer builds never phone home.
4. Recovery coordination: the crash dialog and the recovery overlay must not both
   surface at the same startup for the same recording session.

## Decision

### Backend: sentry-native 0.15.0 with Crashpad

sentry-native is fetched via CMake `FetchContent` with `GIT_SUBMODULES_RECURSE TRUE`.
This initializes Crashpad and mini_chromium as vendored submodules without requiring
GN, depot_tools, or any additional toolchain. Spike verdict (ADR 0017 § Spike):
integration is clean on MSVC 2022 + Ninja.

### License gate

| Component | License | Status |
|-----------|---------|--------|
| sentry-native | MIT | Permissive |
| Crashpad | Apache-2.0 | Permissive |
| mini_chromium | BSD-3-Clause | Permissive |

No GPL contamination. All licenses are compatible with ExoSnap's GPL-3.0-or-later
and with closed-source commercial use. Licenses staged in `licenses/` at install.

### EXOSNAP_OFFICIAL_BUILD gate

When the CMake option `EXOSNAP_OFFICIAL_BUILD` is OFF (default; all self-builds
and CI unless explicitly set):

- `sentry_options_set_dsn(options, "")` — empty DSN.
- No network traffic. Only local minidumps under `%LOCALAPPDATA%\ExoSnap\crashes\`.
- `EXOSNAP_ENABLE_CRASH_CAPTURE` may still be ON if the developer wants local dumps.

When `EXOSNAP_OFFICIAL_BUILD` is ON (release pipeline only):

- EU/de DSN compiled in: `https://e57ff3eff29ad472e673830d7f2fee21@o4511566018576384.ingest.de.sentry.io/4511566053900368`
- DSN is write-only (not a secret). If leaked, it only allows crash event ingestion
  into the ExoSnap project — no read access to other events.
- Upload still gated by `require_user_consent=1` until the user opts in.

### Consent model

`sentry_options_set_require_user_consent(1)` is always set. Until `GiveUserConsent()`
is called, nothing is uploaded. `RevokeUserConsent()` resets to unset (revocable at
any time). The consent UI (crash dialog) is built in a later slice.

### Privacy / scrubbing (before_send hook)

A `before_send` hook runs before any Sentry event is queued for upload. It:

- Strips %USERPROFILE%, %USERNAME%, machine name from all string fields.
- Strips absolute Windows paths (C:\... and UNC \\...) with `[path]` placeholder.
- Enforces an allowlist of permitted tag keys (see crash_scrubber.h).
- Attaches a random per-report correlation ID (not persistent; not tied to install).
- Removes `user` and `breadcrumbs` fields entirely.

The hook runs for structured Sentry events. The minidump itself contains stack frames
with module offsets; PII within the crashing thread's stack is possible but acceptable
under the consent gate (the user sees what is sent before approving).

### No persistent install ID

ExoSnap never generates or stores a persistent machine or install identifier. The
per-report correlation ID is a random UUID generated at hook time. Sentry's automatic
device fingerprinting is not used (no persistent device context is attached).

### Crash directory

`%LOCALAPPDATA%\ExoSnap\crashes\` resolved via `SHGetKnownFolderPath(FOLDERID_LocalAppData)`.
When `EXOSNAP_CONFIG_DIR` env var is set (test/CI isolation), the crash dir is
`$EXOSNAP_CONFIG_DIR\crashes`.

### crashpad_handler.exe deployment

The Crashpad handler is a standalone executable that must be co-located with
`exosnap.exe` at runtime. It is:

- Built by the sentry-native FetchContent dependency.
- Copied next to `exosnap.exe` by a POST_BUILD command on the `exosnap` target.
- Installed alongside `exosnap.exe` (flat layout, same as Qt DLLs and FFmpeg).
- Found at runtime by `ResolveHandlerExePath()` via `GetModuleFileNameW`.

### Recovery coordination (ADR 0015 integration)

The crash handler runs out-of-process after the main process has died. The
recovery manifest (RecoveryManifestStore) is the single source of truth for
unfinished recordings.

Coordination seam:

1. After a crash is processed (first launch after crash), the crash dialog writes
   `%LOCALAPPDATA%\ExoSnap\crashes\dump_handled.json` (the "dump_handled" sentinel)
   via `WriteDumpHandledSentinel()`.
2. The recovery overlay checks `ReadAndClearDumpHandledSentinel()` at startup.
   If the sentinel is present, the overlay suppresses the "crash recovery" banner
   and defers to the crash dialog for that session's entry.
3. The crash dialog still reads `RecoveryManifestStore` to correlate the crash
   with a specific recording artefact and offers "Finish / Continue / Delete".

This avoids double-reporting the same session in both the recovery overlay
and a separate crash dialog.

### UI-agnostic engine

All code in `libs/crash_capture/` has no Qt dependency. The `sentry.h` include
is conditional on `EXOSNAP_CRASH_CAPTURE_AVAILABLE`. The library compiles as a
clean stub when sentry-native is not fetched.

## Consequences

- `EXOSNAP_ENABLE_CRASH_CAPTURE` OFF (default) = zero build-time cost.
- `EXOSNAP_ENABLE_CRASH_CAPTURE` ON = network access at configure time to clone
  sentry-native (~5 MB) + Crashpad (recursive submodules, ~20 MB).
- Release builds set `EXOSNAP_OFFICIAL_BUILD=ON` to compile in the DSN.
- The crash dialog / reporter UI is a separate slice (built by another builder).
  Required seams: `crash_capture.h` public API, `dump_handled.json` sentinel.
- PDB symbol upload to Sentry requires `sentry-cli` in the release pipeline
  (known limitation: MSVC 2022 PDB recognition in sentry-cli #895).
- Crashpad does not support Xbox / WinGDK (irrelevant for ExoSnap).
- MinGW is not supported by Crashpad (irrelevant; ExoSnap uses MSVC).

## Spike verdict

Performed 2026-06-15. sentry-native `FetchContent` + `GIT_SUBMODULES_RECURSE TRUE`
cleanly initializes Crashpad and mini_chromium on MSVC 2022 + Ninja.
No GN/depot_tools required. Integration is approved. See cmake/VendorSentry.cmake.
