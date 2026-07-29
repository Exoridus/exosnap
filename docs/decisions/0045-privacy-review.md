# ADR 0045: Privacy review as a repeatable release step

## Status

**Accepted — 2026-07-12.** ExoSnap promises a telemetry-free product ("no analytics, no
telemetry, no account... by default ExoSnap makes no network connections"). Nothing previously
verified that promise was still true release over release, or that it stayed true as the code
changed. This ADR records the decisions behind a durable inventory document, three cheap
automated checks, and two real fixes an adversarial review of the current code surfaced.

**Amended 2026-07-29.** The crash dialog's unused prefilled GitHub-issue path was removed. The
inventory now has E1–E4 (crash upload, update check, update download, local-only support bundle);
the four runtime network call sites themselves are unchanged.

## Context

A review of every runtime network call site (`libs/update`, `libs/crash_capture`, and a project-
wide grep for socket/WinHTTP/Qt-network primitives) confirmed the app is fundamentally telemetry-
free: exactly four network call sites exist, all WinHTTP, all to GitHub or Sentry. But the review
found two places where the shipped code and the documented promise had drifted apart:

1. **Update-check consent drift.** `check_updates_on_start` defaulted to `true`
   (`AppSettingsStore.h`), and `MainWindow`'s startup path ran the check automatically with no
   first-run consent prompt anywhere in the codebase. An Official build therefore contacted
   `api.github.com` on first launch without an opt-in — directly contradicting `PRIVACY.md`
   ("opt-in and consent-gated") and `docs/product-spec.md` §13/§14.
2. **Minidump module paths.** A Crashpad minidump (uploaded on a hard crash, with consent) carries
   the full install path of `exosnap.exe` in its module list. `before_send` only scrubs the
   structured event, never the minidump binary, and does not run at all for the out-of-process
   hard-crash path. For a portable install run from `%USERPROFILE%`, the username segment of that
   path can appear in the uploaded minidump — `PRIVACY.md`'s "paths are stripped" claim was
   inaccurate for this specific channel.

Beyond those two concrete bugs, nothing enforced that the promise stayed true as the code
changed: no register of network call sites, no check that the crash-report tag allowlist (which
existed twice in the code, one copy a literal brace-list that could silently drift from the other)
matched what `PRIVACY.md` documented, and no requirement that a capture-target window title (a
potentially sensitive string — document titles, chat partner names) be neutralized before it
reaches a log a user might share.

## Decisions

### D1 — A durable, tracked egress inventory: `docs/privacy-review.md`

A new tracked document holds the egress table (E1–E4: crash upload, update check, update download,
the local-only support bundle), the local-never-transmitted store list,
and the release checklist, each item tagged **[CI]** or **[Live]**. `PRIVACY.md` stays the
user-facing plain-language statement; `docs/privacy-review.md` is the reviewer-facing artifact
with code references and test coverage — the two are cross-checked automatically (D2b) instead of
drifting independently.

### D2 — The crash-report tag allowlist becomes a single, header-level source of truth

`kAllowedTagKeys` moved from a `.cpp`-local array (with a second, literal, hand-repeated copy
inside `BeforeSendHook`) into `crash_scrubber.h` as `inline constexpr`, wrapped in
`// PRIVACY-ALLOWLIST-BEGIN/END` marker comments. `BeforeSendHook` now iterates `AllowedTagKeys()`
instead of its own literal list — one definition, for both the C++ code and the new doc-sync
script (D2b). A Golden-Set unit test (`test_crash_scrubber.cpp`) hardcodes an independent expected
key list so any change to the allowlist fails the test until the author consciously updates it —
the trigger to also update `PRIVACY.md` / product-spec §14.

**Honest CI reach.** The sentry-free Golden-Set + `IsAllowedTagKey`/`ScrubString` tests run on
every PR (the regular `build-test` job). The sentry-linked path (real `sentry_value_t` tag
filtering under `EXOSNAP_CRASH_CAPTURE_AVAILABLE`) is exercised by the existing crash_capture test
suite only inside `crash-capture-build.yml`, which previously built `--target exosnap` and ran no
tests at all. That workflow now also builds the test targets and runs the crash_capture suite
(`ctest -R crash.`) — but this job still only runs on push-to-`main`, the `crash-capture` PR
label, or manual dispatch, never an unlabeled PR. This ADR does not add a new sentry-native-linked
unit test beyond running the existing suite there; a dedicated `BeforeSendHook`-level test with a
hand-built `sentry_value_t` fixture is documented as a follow-up (see Deliberately not built).

### D2b — `validate-privacy-allowlist.ps1` (CI, build-free)

Parses the marker-delimited key list out of `crash_scrubber.h` and two matching
`<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN/END -->` markdown tables in `PRIVACY.md` and
`docs/product-spec.md` §14, then asserts all three sets are identical — bidirectionally, so a
documented field with no matching code key fails just as loudly as an undocumented code key. Runs
in the `lint` job (`ci.yml`) on every PR; no build required.

### D3 — `before_send` defensive backstop (not an active leak)

An earlier hypothesis assumed sentry-native attaches a `server_name`/`device` context that could
leak a hostname. Verified against the pinned sentry-native source (0.15.0,
`cmake/VendorSentry.cmake`): no such context or option exists in that version. `BeforeSendHook`
still strips `server_name` and `contexts.device` defensively — a one-line guard against a future
sentry-native version or app code adding either, not a fix for a real leak.

### D4 — `validate-network-egress.ps1` (CI, build-free): no new egress point unnoticed

A grep guard over tracked `app/`, `libs/`, `apps/` sources (excluding `third_party/` and `tests/`)
for network primitives (`WinHttpOpen`, `WinHttpConnect`, `WinHttpWebSocket`, `socket(`,
`WSAStartup`, `getaddrinfo`, `InternetOpen`, `curl_easy`, `QNetworkAccessManager`, `QTcpSocket`,
`QUdpSocket`, `QSslSocket`, `Qt6::Network`) and bare `http(s)://` literals, checked against a
file allowlist (the four known egress `.cpp`/`.h` pairs) and a host allowlist (GitHub, Sentry EU
ingest, plus the static SVG XML-namespace URI). Any hit outside both allowlists fails the build
with the exact file/line and a note to inventory it in `docs/privacy-review.md`.

**Deliberate deviation from the original primitive list:** a bare `connect(` was proposed as a
raw-socket signal, but Qt's `QObject::connect(...)` appears in roughly 80 tracked source files —
grepping for it would make this guard permanently noisy and therefore ignored, defeating its
purpose. `WinHttpConnect` (its own distinct, specific primitive) still catches the real WinHTTP
case. This is a build-time engineering tradeoff, not a product decision, and is documented in the
script's own header comment.

### D5 — Window-title neutralization at the log source (not only the support bundle)

The one-click support bundle (#194) already redacts a capture-target window title
(`RedactCaptureTargets`) when it finds one in a log file being packaged — but the underlying
`exosnap.log`/`engine.jsonl` still carried the raw title, because a user can also share those log
files manually, outside the bundle. `RecordingCoordinator::StartRecording`'s "start" log line and
`RecordPage`'s target-selection/start-request log lines now log a stable `[window]` placeholder
instead of the actual title whenever the capture target is a window
(`RecordViewModel::LogSafeTargetLabel`); monitor targets are unaffected (a display description is
a technical identifier, never personal). The bundle's `RedactCaptureTargets` pass stays as a
defense-in-depth backstop for any log line this fix does not cover and for log content already on
disk before an upgrade. UI-facing labels (the target picker, chrome status, notifications) are
unaffected — only what reaches the on-disk log changed.

### Update-check consent (blocker fix)

`check_updates_on_start` now defaults to `false` (`AppSettingsStore.h`/`.cpp`). A first launch —
Official build or self-build — never contacts `api.github.com` before the user explicitly turns
the automatic check on from the Settings update card (a manual "Check now" remains available and
needs no separate consent, since clicking it is itself the explicit action). This makes
`PRIVACY.md`'s "opt-in and consent-gated" claim and product-spec §13/§14 true again, rather than
weakening them to "opt-out."

### Update-check "version string" doc drift (doc-only fix)

`PRIVACY.md` and product-spec §14 claimed the update check sends "the ExoSnap version string."
The code sends no version at all — a fixed User-Agent (`ExoSnap-UpdateChecker/1.0`, a protocol
version) and only `Accept`/`X-GitHub-Api-Version` headers; the newest-release comparison is
client-side against the already-fetched releases JSON. Both docs are corrected rather than the
code changed — the actual egress is more minimal than previously documented, so this is a
doc-honesty fix, not a privacy fix.

### `os.*`/`gpu.*` allowlist drift (doc-only precision, not a new Sentry payload)

Only `encoder_backend`/`container`/`video_codec`/`audio_codec` are populated on the Sentry tag
path today (`SetEncoderContext`); the six other allowlisted keys (`os.*`, `gpu.*`, `app.version`)
are allowlisted (and would be scrubbed correctly if ever set) but are not populated by app code —
OS/GPU facts currently reach the user only via the crash dialog, not the Sentry upload.
`PRIVACY.md` and product-spec §14 are precise about this rather
than the app being changed to start sending more data on the Sentry path. This keeps the crash
network path unchanged (see "Deliberately not built" below) while still resolving the doc↔code
drift the review exists to find.

### Minidump module paths (doc precision; mitigation not shipped this slice)

`PRIVACY.md` and product-spec §14 now state precisely that the minidump binary channel carries
module paths (and can carry a username segment for a portable, non-standard-path install),
distinct from the "paths are stripped" guarantee that applies to the structured event. A code-side
mitigation (forcing a standard install path, or a Crashpad-side module-path redaction) is not
shipped in this slice — see Deliberately not built.

## Consequences

- `PersistedAppSettings::check_updates_on_start`'s default changed from `true` to `false` — a
  behavior change for anyone who previously relied on the (undocumented, consent-violating)
  auto-check. Pre-1.0, no migration; existing `settings.ini` files with an explicit `true` are
  unaffected (this only changes the default for a missing/fresh key).
- `kAllowedTagKeys` moved from `crash_scrubber.cpp` to `crash_scrubber.h`; any code including the
  header now sees the array (previously `.cpp`-local). No behavior change.
- `RecordingCoordinator` and `RecordPage` log a `[window]` placeholder instead of the real window
  title in three log call sites; UI-facing labels are unaffected. This is a support/debug-context
  tradeoff (a shared support log carries less detail for window captures) accepted deliberately
  for privacy, per product decision.
- Two new CI checks (`validate-privacy-allowlist.ps1`, `validate-network-egress.ps1`) run on every
  PR in the `lint` job — both are seconds, no build.
- `crash-capture-build.yml` now builds test targets and runs `ctest` (previously `--target
  exosnap` only, no tests at all) — slightly longer job, still gated to main-push/label/dispatch.

## Deliberately not built

- No telemetry, no analytics, no consent dashboard — the review only proves the existing
  telemetry-free behavior, it does not add any.
- No static taint/data-flow analysis — `validate-network-egress.ps1` is a deliberately blunt grep
  guard; it proves no new egress point appeared unnoticed, not what bytes flow. That question is
  answered by the field inventory, the scrubber tests, and the Live Sentry check.
- No automated Sentry event introspection in CI (no Official build / DSN on the runner) — stays a
  named Live check.
- No change to the update/crash network paths themselves beyond the consent-default fix above —
  they were already correct.
- No dedicated `BeforeSendHook`-level unit test with a hand-built `sentry_value_t` fixture exposing
  a new `ScrubEventInPlace` free function — the existing crash_capture suite now at least runs
  under the real sentry-native link via the `crash-capture-build.yml` change (D2), which is the
  proportionate fix for "the hook-level test never ran under ON"; a fixture-based hook test remains
  a documented, not-yet-built follow-up.
- No Crashpad-side minidump module-path redaction or forced standard-install-path enforcement —
  documented as a known limitation with a doc-level fix landed now; a code mitigation is a
  separate, larger follow-up if pursued.
- No `os.*`/`gpu.*` values newly sent on the Sentry tag path — the doc precision above resolves
  the drift without expanding what the crash-report channel actually transmits.
