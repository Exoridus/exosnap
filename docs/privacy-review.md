# Privacy review

A durable, tracked inventory of every place ExoSnap's code can touch the network, plus the
review checklist that keeps `PRIVACY.md` and `docs/product-spec.md` §14 honest release over
release. See ADR 0045 for the decisions behind this document and its automated checks.

This is **not** a new privacy feature. ExoSnap remains telemetry-free; nothing here changes what
data the app processes. It is a **procedure** — a place the egress inventory lives, plus a
handful of automated checks that make "nothing new phones home" and "the docs match the code"
provable instead of a reviewer's hope.

## Egress inventory (E1–E5)

There are exactly four runtime network call sites in the app, all over WinHTTP, all to GitHub or
Sentry. `scripts/validate-network-egress.ps1` (CI, every PR) fails if a fifth appears without this
table being updated and the script's allowlist being consciously extended.

| # | Purpose | Gate · Consent | Fields sent | Recipient / host |
|---|---|---|---|---|
| E1 | Crash report upload (Sentry, Stage 1) | Official build only (DSN compiled in under `EXOSNAP_OFFICIAL_BUILD`); **consent-gated** — nothing uploads before `GiveUserConsent()` | Allowlisted tags only (see table below) + crash stack + minidump. Minidump binary carries module paths — see "Minidump module paths" below. | `ingest.de.sentry.io` (EU) |
| E2 | Stage-0 GitHub issue (assisted, prefilled URL) | Always available (no Official-build gate); user must click "File an issue" and review/submit in the browser | `app_version`, `os`, `gpu`, `encoder`, `exception`, `correlation_id` — allowlisted fields only (`CrashIssueData`) | `github.com/Exoridus/exosnap/issues/new` |
| E3 | Update check | **Opt-in** — `check_updates_on_start` defaults to `false` (ADR 0045); self-built binaries are additionally blocked at compile time (`IsUpdateCheckEnabled()`) regardless of the setting | No request body, no auth token, no app version. Fixed User-Agent `ExoSnap-UpdateChecker/1.0` (protocol version, not app version) + `Accept`/`X-GitHub-Api-Version` headers. Version comparison is client-side against the fetched releases JSON. | `api.github.com` |
| E4 | Update package download | Same opt-in gate as E3 (only reached after E3 finds a newer release and the user clicks Update) | No body, no token. Fixed User-Agent `ExoSnap-Updater/1.0`. | GitHub Release asset URLs (`objects.githubusercontent.com` / `github.com`) |
| E5 | Support bundle (Thema "diagnostics support channel", #194) | User-initiated, local file operation — **no transmission** | N/A — the bundle is a `.zip` written to a location the user picks; ExoSnap never uploads it | None — this is the "local, not egress" entry, kept in the table so "no network path" is explicit rather than merely absent |

**E3 baseline note (ADR 0045).** Before this slice, `check_updates_on_start` defaulted to `true`
with no first-run consent step — an Official build silently contacted `api.github.com` on first
launch, contradicting `PRIVACY.md`'s "opt-in" claim. The default is now `false`; this table's E3
row is code-true as written. See ADR 0045 for the full before/after.

### What is sent (crash-report tag allowlist)

The `kAllowedTagKeys` array in `libs/crash_capture/include/crash_capture/crash_scrubber.h` is the
single source of truth for which structured tags can ever leave the process on the Sentry path
(E1). `scripts/validate-privacy-allowlist.ps1` (CI, every PR) fails if this list drifts from the
mapping tables in `PRIVACY.md` or `docs/product-spec.md` §14 in either direction.

| Tag key | What it carries | Populated today? |
|---|---|---|
| `os.name` | Windows edition name | No — not yet set via `SetTag`/`SetEncoderContext` |
| `os.version` | Windows build/version string | No |
| `gpu.model` | GPU adapter name | No |
| `gpu.vendor` | GPU vendor | No |
| `gpu.driver` | GPU driver version | No |
| `app.version` | ExoSnap version | No |
| `encoder_backend` | Active encoder backend | Yes (`SetEncoderContext`) |
| `container` | Output container | Yes |
| `video_codec` | Selected video codec | Yes |
| `audio_codec` | Selected audio codec | Yes |

OS/GPU facts reach the user today only via the Stage-0 GitHub issue (E2) and the local crash
dialog, not the Sentry tag path — a documented, deliberate doc↔code precision (not a leak: less
is sent than the allowlist permits, never more).

### Minidump module paths (E1 detail)

A hard crash uploads (with consent) a Crashpad minidump out-of-process. The `before_send` scrubber
runs only on the structured event, never on the minidump binary — it cannot strip the
`MINIDUMP_MODULE_LIST`, which carries the full install path of `exosnap.exe`. For a standard
Program-Files-style install this is not personal; for a **portable install run from under
`%USERPROFILE%`**, the username segment of that path can appear in the uploaded minidump. This is
a real, narrower exception to "paths are stripped" (see `PRIVACY.md`) — it applies only to the
minidump binary, never to the structured event, the Stage-0 issue, or the crash dialog shown
beforehand. No code mitigation ships in this slice (see Offene Frage 1 / ADR 0045); the doc
precision above is the fix that landed. A forced standard-install-path mitigation remains a
possible follow-up, tracked as a known limitation rather than silently promised.

### `before_send` defensive backstop

`BeforeSendHook` (`libs/crash_capture/src/crash_capture.cpp`) additionally strips `server_name` and
`contexts.device` from every event before it can be sent. This is **not** closing an active leak:
the pinned sentry-native version (0.15.0, see `cmake/VendorSentry.cmake`) does not set either field
on init. It is a cheap guard against a future sentry-native version (or future app code) adding
either without `before_send` being updated to catch it.

### Local, never-transmitted stores

Confirmed by code search: no `WinHttpOpen`/socket call exists outside `libs/update` and
`libs/crash_capture` (enforced by `scripts/validate-network-egress.ps1`).

- Application settings — `%LOCALAPPDATA%\ExoSnap\settings.ini`
- Recording presets — `%LOCALAPPDATA%\ExoSnap\presets.ini`
- Recording history — `%LOCALAPPDATA%\ExoSnap\recording-history.json`
- Crash-recovery manifest — `%LOCALAPPDATA%\ExoSnap\`
- Logs — `exosnap.log`, `engine.jsonl` (rotated) and per-recording `reports/session-*.json`
- Local crash captures — `%LOCALAPPDATA%\ExoSnap\crashes\*.dmp`
- Recordings — the output folder the user chooses
- Support bundle `.zip` — written to a location the user picks (E5 above)

## Window-title logging (capture-target privacy)

A capture-target **window title** (a WGC-capture app/window name — potentially a document title,
a private tab title, a chat partner's name) is neutralized **at the log source**, not only when a
support bundle is later assembled:

- `RecordingCoordinator::StartRecording` and `RecordPage`'s target-selection/start-request log
  lines now log `[window]` instead of the actual title whenever the capture target is a window
  (`RecordViewModel::LogSafeTargetLabel`). Monitor targets are unaffected — a display description
  ("Desktop - Display 1") is a technical identifier, never personal.
- The one-click support bundle (`app/diagnostics/SupportBundle.cpp`, `RedactCaptureTargets`)
  additionally redacts any `target="…"` value it still finds to `[capture-target]`, as a
  defense-in-depth backstop for any log line this slice's source-level fix does not cover and for
  historical log content already on disk before an upgrade.
- The UI-facing labels a user actually sees (the target picker list, the recording chrome status,
  notifications) are **unaffected** — only what reaches the on-disk log changed.

## Review checklist (repeat every release)

Each item is tagged **[CI]** (automatic, part of the `lint` job on every PR) or **[Live]** (a
named manual check on real hardware / a real Official build / a real Sentry event — CI has no GPU
and no Sentry DSN, so these cannot be automated). See `docs/release-checklist.md` for where this
plugs into the release process.

- **[CI]** `scripts/validate-privacy-allowlist.ps1` green — the crash-report tag allowlist matches
  `PRIVACY.md` and `docs/product-spec.md` §14, in both directions.
- **[CI]** `scripts/validate-network-egress.ps1` green — no network primitive or disallowed host
  literal outside the four known call sites (E1/E3/E4 above).
- **[CI]** Crash-scrubber tests green — the allowlist Golden-Set test (`test_crash_scrubber.cpp`)
  and the sentry-free `IsAllowedTagKey`/`ScrubString` suite, on every PR (`build-test` job,
  `ci.yml`). The sentry-linked `before_send` path (tag filtering + the defensive backstop) is
  exercised by the crash_capture test suite **only** when `crash-capture-build.yml` runs — push to
  `main`, the `crash-capture` PR label, or manual dispatch, never an unlabeled PR. This is an
  intentional, documented CI-reach boundary (see ADR 0045), not a gap that was closed silently.
- **[CI]** Support-bundle scrubber coverage green (`test_support_bundle.cpp`), including the
  window-title fixture (`NoPersonalDataOrWindowTitleSurvives`).
- **[Live]** **Sentry reality check (event).** On a real Official build, give consent, trigger
  `SendTestEvent`; in the Sentry EU UI, confirm no hostname/`server_name`, no path/username in the
  event, and exactly the allowlisted tags + stack arrived.
- **[Live]** **Minidump module-path check (hard crash).** Provoke a real hard crash with consent
  active; inspect the uploaded minidump's module list (Sentry UI or the local `.dmp`) for a
  username segment in the `exosnap.exe` path (relevant for non-standard/portable installs).
  `SendTestEvent` does not produce a minidump — this is the only real check of the binary channel.
- **[Live]** **Update-check network trace.** A proxy/Fiddler capture of a real update check shows
  only the expected `GET api.github.com/.../releases` with the fixed User-Agent — no user data in
  the query.
- **[Manual/Doc]** `PRIVACY.md`'s `Effective date` and `docs/product-spec.md` §14 have been walked
  against this inventory for the release; bump `Effective date` if any field or recipient changed.

Ruhig, nicht alarmistisch: this is a per-release checkbox ritual, not a continuous monitor. The CI
checks are silent unless something actually drifted.

## What this deliberately does not do

- No new telemetry, analytics, or consent dashboard — the review only *proves* the existing
  telemetry-free behavior.
- No static taint/data-flow analysis — `validate-network-egress.ps1` is a deliberately blunt grep
  guard: it proves *no new egress point appeared unnoticed*, not *what bytes flow*. The "what is
  sent" question is answered by the field inventory above plus the crash-scrubber tests plus the
  Live Sentry check.
- No automated Sentry event introspection in CI — the CI runner has no Official build / DSN; the
  event-level check stays a named Live check.
- No change to the update/crash network paths themselves — they were already correct; this
  document and its checks encapsulate them, they do not rebuild them.
