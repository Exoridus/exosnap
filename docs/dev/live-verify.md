# Live Verify — the resumable release-acceptance runner

Architecture decision and rationale: **ADR 0066**. This document is how to use
it.

Live Verify does not restate the release gate. `docs/release-checklist.md` and
`docs/privacy-review.md` remain the authority on *what* has to be true; the
runner's catalog references them and records *whether it was proven, against
which bytes, in which environment, and with what evidence*.

---

## Three layers

```
scripts/live-verify.ps1            orchestrator: process lifecycle, check state,
                                   evidence, resume/retry, bounded human gates
        |
scripts/lib/LiveVerifyClient.psm1  thin client: connect, hello, query, command,
scripts/live-verify-client.ps1     wait — machine-readable JSON, useful exit codes
        |
        |  local named pipe, current user only, no TCP
        v
exosnap.exe                        LiveVerifyControlServer — dormant unless armed,
                                   semantic application intents only
```

---

## Quick start (local dry run)

```powershell
pwsh scripts/live-verify.ps1 prepare -Artifact local   # bind a run to a built exe
pwsh scripts/live-verify.ps1 list                      # what the catalog covers
pwsh scripts/live-verify.ps1 run                       # execute everything runnable
pwsh scripts/live-verify.ps1 status                    # per-check state
pwsh scripts/live-verify.ps1 report                    # report.md + report.json + junit.xml
```

Interrupt it at any point (Ctrl-C, a reboot, a crash) and continue:

```powershell
pwsh scripts/live-verify.ps1 resume
```

Other verbs:

```powershell
pwsh scripts/live-verify.ps1 retry LV-REC-001
pwsh scripts/live-verify.ps1 skip  LV-WIN-003 -Reason "single-monitor machine today"
pwsh scripts/live-verify.ps1 note  LV-PREV-001 -Text "browser playing video on display 2"
pwsh scripts/live-verify.ps1 run   -Only LV-OVL-001,LV-OVL-002
pwsh scripts/live-verify.ps1 run   -NonInteractive     # human gates become MANUAL_REQUIRED
```

**Close ExoSnap first.** A verification launch is a *normal* launch — the
single-instance guard is part of what is being accepted — so a second instance
would exit immediately and activate the running window, taking focus off whatever
you are doing. The runner refuses to start in that situation and says so.

---

## Run directory

```
.workspace/live-verify/<run-id>/
├── run.json                    run id, creation time, format version
├── state.json                  the authoritative per-check state
├── environment.json            captured once per run
├── artifact-fingerprint.json   exe path, SHA-256, product/file version, size
├── report.md / report.json / junit.xml
├── checks/<check-id>/…         per-check evidence
├── logs/process.json           PID, exe path + hash, handshake identity
├── media/ analysis/ screenshots/ updater/
```

`.workspace/` is untracked, so evidence is never committed.

### Check states

| State | Meaning |
|---|---|
| `PENDING` | never attempted in this run |
| `RUNNING` | persisted **before** the check executes; a leftover means the runner died mid-check |
| `PASS` | proven, with evidence, against the recorded artifact + environment |
| `FAIL` | disproven |
| `BLOCKED` | the environment cannot satisfy it (one monitor, no HDR display, no ffprobe, no RC) |
| `MANUAL_REQUIRED` | waiting for a human gate |
| `SKIPPED` | deliberately not run, with a recorded reason (a reason is mandatory) |
| `UNVERIFIED` | attempted, outcome unknown — including "interrupted" |
| `STALE` | passed once, against an artifact or environment that has since changed |

Interruption is never converted to `PASS`. `resume` turns a stranded `RUNNING`
into `UNVERIFIED` and says which checks that happened to.

### Artifact binding

A PASS applies to the exact bytes it was produced against. `resume` re-hashes the
executable and re-reads the environment; anything a changed fingerprint
invalidates becomes `STALE` and is rerun. Environment dependencies are declared
per check (`EnvironmentKeys`), so rearranging monitors invalidates the
cross-monitor Preview check and leaves updater identity alone.

---

## The control channel

### Arming it

```
exosnap.exe --live-verify-control <run-id>
```

Nothing else arms it. Not a Debug build, not an environment variable, not a
settings key. A missing or malformed run id makes the process exit 2 rather than
start normally.

Run ids are 8–64 characters of `[A-Za-z0-9._-]`; the runner mints a GUID, which
doubles as the connection credential. The endpoint is
`\\.\pipe\ExoSnap.LiveVerify.<run-id>`, ACL'd to the creating user, with
`PIPE_REJECT_REMOTE_CLIENTS`.

Live Verify mode is **not** a harness mode: no config isolation, no
single-instance suppression, no tray suppression. Set `EXOSNAP_CONFIG_DIR` and
`EXOSNAP_OUTPUT_DIR` yourself when a check needs an isolated profile, so which
profile is in force stays the check's decision.

### Client

```powershell
pwsh scripts/live-verify-client.ps1 hello        -RunId <run-id>
pwsh scripts/live-verify-client.ps1 capabilities -RunId <run-id>
pwsh scripts/live-verify-client.ps1 query record -RunId <run-id>
pwsh scripts/live-verify-client.ps1 command record.pause -RunId <run-id>
pwsh scripts/live-verify-client.ps1 command window.moveToScreen -RunId <run-id> -Params '{"screen":"27GL850"}'
pwsh scripts/live-verify-client.ps1 wait record.stateChanged -Where stateText=Paused -TimeoutSeconds 10 -RunId <run-id>
```

`query <domain>` is sugar for that domain's snapshot command (`system`, `app`,
`window`, `preview`, `record`, `result`, `overlay`, `editor`, `diagnostics`).
Anything else is passed through verbatim, so the CLI cannot drift from the server
allowlist.

Exit codes — distinct because "refused" and "never answered" are different
acceptance outcomes:

| Code | Meaning |
|---|---|
| 0 | success |
| 2 | usage error |
| 3 | could not connect / handshake refused / protocol mismatch |
| 4 | the command was answered with `ok:false` |
| 5 | timed out waiting for a response or an event |

There are **no hidden retries**. A silent reconnect would turn "the application
restarted under us" — the single most important thing an updater check has to
notice — into a green result.

### Commands

| Command | Acceptance purpose |
|---|---|
| `system.hello` | Handshake. Proves the process is the artifact under acceptance (version, commit, build id, exe SHA-256, PID, install mode, channel). |
| `system.capabilities` | The exact command/event surface this build answers; a client can never be told about something that would then be rejected. |
| `system.snapshot` | Screens, topology, DPR, refresh — the environment a cross-monitor or DPI check keys off. |
| `app.snapshot` | Appearance, accent, current navigation index, Expert mode, window visibility. |
| `window.snapshot` | Geometry, screen, and the native facts no pixel instrument can see: style, ex-style, non-client inset, child HWND count, display affinity. |
| `window.moveToScreen` | The programmatic half of the cross-monitor Preview check. The only window mutation exposed. The screen name is `QScreen::name()` — on Windows the monitor's friendly name, not `\\.\DISPLAYn`; read it from `system.snapshot`. |
| `preview.snapshot` | Live preview state plus the redraw gate's counters (`publishSignals`, `wakeups`, `renderPasses`, `owed`). `owed` is the whole cross-monitor question. |
| `record.snapshot` | Transport state and what the UI would allow (`canStart`/`canPause`/…), so a check can wait on authoritative state instead of sleeping. |
| `record.selectTarget` | Selects a monitor/window target through the same path a source-picker click takes. |
| `record.start` / `pause` / `resume` / `stop` / `split` / `captureFrame` | The real transport intents, refused exactly where the UI refuses them. |
| `record.result` | Typed result of the finished recording: paths, container/codecs, duration, marker count, error phase. |
| `overlay.snapshot` | Per capture-excluded overlay: visibility, `WS_EX_LAYERED`, display affinity, geometry. The only observable part of a structurally unobservable surface. |
| `editor.snapshot` | Edit-surface state (open, clip, trim, position, export running). Read-only — the Edit → Export path is already covered deterministically by `--auto-edit`. |
| `diagnostics.snapshot` | Verdict, blocker/notice counts, elevation — recording start is blocked by diagnostic blockers, so a check needs to know. |

### Events

`app.ready`, `record.stateChanged`, `record.resultReady`, `window.screenChanged`.

All four reuse signals the application already emits; none introduces a second
idea of the state it reports. **There are no synchronization sleeps** anywhere in
the client or the runner: a command is followed by a wait on an authoritative
event or state, and a timeout is a failure boundary, not a synchronization
primitive.

---

## The catalog

`pwsh scripts/live-verify.ps1 list` prints it. Each entry names the strongest
verifier that can actually prove it:

```
FULL_AUTO        a deterministic test or script, no running application
EXTERNAL_TOOL    an existing harness or analyzer (--hwnd-audit, --auto-record, ffprobe)
CONTROL_CHANNEL  observation/intent against the running application
UI_AUTOMATION    the real visible control, via UIA
SEMI_AUTO        machine work plus one bounded human confirmation
MANUAL_VISUAL    a human judging composition or appearance
MANUAL_PHYSICAL  a human moving real hardware or a real pointer
```

Nothing may claim a lower layer than it can deliver, and nothing may use a weaker
layer than the requirement needs.

### Human gates that remain, and why

| Check | Why software cannot close it |
|---|---|
| `LV-WIN-003` interactive cross-monitor drag | Programmatic placement does not reproduce the modal move loop, the per-pixel `WM_MOVING` sequence, or the moment the pointer stops — which is the failure. Synthesising a drag is ruled out by `CLAUDE.md`. |
| `LV-OVL-002` overlay composition on the desktop | `WDA_EXCLUDEFROMCAPTURE` defeats screenshots, screen recording and `PrintWindow` by design, and `grabWindow()` renders the scene graph, which shows correct alpha even when the window composes wrongly. |
| `LV-THEME-001` Light appearance | "Washed out" and "enough depth" are judgements. The numeric contrast floor is already `theme_contrast_tests`; judge the current tokens, not an older screenshot. |

Each is one bounded prompt, asked once, with the machine-observable half verified
by the runner before and after. Reply `done` and nothing else.

---

## What a local dry run does not prove

A local Release run validates the infrastructure and the product behaviour it can
reach. It is **not** release acceptance:

- an RC's updater, install-mode and signed-manifest checks need a genuinely
  published, immutable RC (`docs/release-checklist.md` §3–§5, §7a),
- after any product code change, acceptance needs a *new* RC — a published RC is
  never re-used, re-uploaded or overwritten,
- a previous RC's PASS does not apply to changed bytes; the runner marks those
  `STALE` on its own.

---

## Tests

| Suite | What it pins |
|---|---|
| `live_verify.live_verify_protocol_tests` | argv gate, wire format, handshake, allowlist, parameter validation — no pipe, no window, no GPU |
| `live_verify.live_verify_server_tests` | the transport: no endpoint before start, none after exit, malformed/hostile clients survive, per-connection handshake, events, reconnect, oversized frames, destruction under a live client |
| `live_verify.runner_state` | the runner's own promise: interruption is never success, a PASS is artifact- and environment-bound, a corrupt state file is reported rather than reset |

```powershell
pwsh scripts/run-tests.ps1 -Filter 'live_verify'
```
