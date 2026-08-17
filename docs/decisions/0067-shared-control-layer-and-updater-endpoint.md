# 0067 — One control layer, two endpoints: the updater becomes observable

- Status: accepted
- Date: 2026-08-17
- Related: ADR 0012 (update security model), ADR 0034 (in-app update and dual swap),
  ADR 0055 (verification reinstall mode), ADR 0066 (Live Verify local control channel)

## Context

ADR 0066 put a local, opt-in control channel inside `exosnap.exe` so release
acceptance could drive and observe a *running* application instead of ticking
Markdown checkboxes. It left one process out, and it is the one whose
correctness cannot be established by looking at the application at all:
`exosnap-updater.exe`.

The updater has its own window, its own state machine, its own worker thread and
a twelve-case failure matrix with rollback semantics. Nothing about it was
machine-readable:

- It exited `0` after a *failed* update, because "Close" goes to
  `QCoreApplication::quit()` and a quit is a normal exit. The application dutifully
  logged a number that meant nothing.
- `FailureCase`, `RetryEntryStep` and the "which installation is live" answer
  existed as code and as comments in the failure matrix, but only ever reached
  the outside world folded into a sentence of English UI copy.
- There was no endpoint, so the most valuable sentence an update test can say —
  *the existing installation is unharmed* — could not be asserted at all. The
  portable rows of `docs/release-checklist.md` §7a ("full path runs", "interrupt
  the download once", "retry works") were hand-ticked for exactly that reason.

Meanwhile the semantic core in `app/live_verify/` was already UI-free and already
carried the property that prevents false success: `Settle::Asynchronous`, and a
`stateRevision` that hangs on the equality of a flat value type rather than on
signal invocations.

## Decision

**Lift the process-independent half of the control channel into `libs/control`
and give the updater its own endpoint on top of it.** Specifically:

- `libs/control` owns the NDJSON envelope and error taxonomy (`control/protocol.h`),
  the command-policy mechanics (`control/command_policy.h`), the connection and
  dispatch state machine (`control/session.h`), the named-pipe transport with its
  per-user DACL, `PIPE_REJECT_REMOTE_CLIENTS` and dispatch timeout
  (`control/control_server.h`), and the argv gate (`control/options.h`).
- Each process keeps what only it can answer: its state type, its command table
  with its preconditions, its intents and its identity. `app/live_verify/` keeps
  `AutomationState`, `LiveVerifyCommandPolicy`, `LiveVerifySource` and its
  dispatcher; the updater adds `UpdaterControlSource`, `UpdaterCommandPolicy` and
  `UpdaterControlDispatcher`.
- The endpoint name gains a **role**: `\\.\pipe\ExoSnap.<role>.<run-id>`. The
  application's role is `LiveVerify`, so its endpoint name is unchanged; the
  updater's is `Updater`. One runner can therefore hold both endpoints of the
  same run id, which is what an end-to-end update flow needs.
- The updater's gate is one argv option carrying a run id:
  `--automation-control <run-id>`. No environment variable, no Debug-build
  autostart. Without it there is no pipe, no thread and no log line.

**Make the updater's state a product value type, not an automation-only view.**
`update/update_flow_state.h` (in `libs/update`, so both processes share the
vocabulary) carries `mode`, `phase`, `failureCase`, `retryEntryStep`,
`installState`, the versions and the byte counters. The window renders from it
and the channel publishes it, written by the same controller events. There is
deliberately no second automation state: a second model is a second thing that
can be wrong.

## Consequences

### What this buys

`installState` is the point. It distinguishes `intact` / `restored` /
`strandedInBackup` — and `unknown`, which is not a gap in the model but the only
truthful answer after a Windows Installer run that reported success and then
failed verification. This process reads a registry path and a version string; it
never asks Windows Installer for a rollback outcome, so claiming `intact` there
would be a guess. A recovery check can now assert the sentence it actually cares
about instead of settling for `failed`.

Exit codes became meaningful (`0` applied, `1` failed, `2` usage, `3` up to date,
`4` reboot required, `5` closed without an outcome), so a release script can fail
on a failed update.

### Deliberate absences

- **No command arms a handoff.** A handoff is a start argument by definition. A
  channel that could set one afterwards would let an external caller decide what
  an elevated `msiexec` installs — the `--package` shape rejected in the audit,
  reintroduced through the back door.
- **`updater.cancel` refuses where cancellation is not observed.** The engine
  honours cancellation in the download loop and in the bounded `msiexec` wait,
  and nowhere else. During the staged rename, the post-install verification and
  the relaunch health check the command answers `blocked` — the same three phases
  in which the window disables its own close control. A command that answered
  "ok" and did nothing would be undetectable from the client side.
- **`stateRevision` excludes download progress.** Progress arrives at roughly
  12 Hz. A revision that advanced on every tick would turn "wait until the
  revision advances" into "wait up to 80 ms", which is a sleep with extra steps.
  The byte counters are still published in every snapshot at full frequency; they
  are simply not what the counter measures.
- **Elevation is unchanged.** The updater stays `asInvoker`. The only elevation
  in the product remains `ShellExecuteExW(runas, msiexec.exe, …)` in the MSI
  apply step, with a self-produced path to a hash-verified, write-locked package.
  A blanket-elevated updater would put the manual start behind a UAC wall, turn
  `--install-dir` from a data-loss risk into a system risk, and make automation
  impossible — the Secure Desktop is not scriptable.

### Risk taken

The lift touches files compiled into the shipping executable. The mitigation is
that the existing protocol, policy and server tests moved with it **unchanged**
and stayed green: that, rather than review, is the evidence the extraction was
behaviour-neutral.
