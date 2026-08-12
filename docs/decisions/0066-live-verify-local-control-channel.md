# 0066 — A local, opt-in control channel inside the shipping binary for release acceptance

- Status: accepted
- Date: 2026-08-12
- Related: ADR 0001 (visual test harness), ADR 0016 (on-screen overlay architecture),
  ADR 0034 (in-app update and dual swap), ADR 0055 (verification reinstall mode),
  ADR 0064 (Qt Quick production cutover)

## Context

ExoSnap's release gate (`docs/release-checklist.md` §5–§7a) is a long list of
checks that a person performs by clicking through a real build on real hardware.
Most of them are there for a good reason: CI runs on GPU-less runners and cannot
record anything, cannot compose an overlay, and cannot swap an installed
application.

But the list had drifted well past what genuinely needs a human. Walking it
produced three recurring failures:

- **Repetition without evidence.** A checklist records that somebody ticked a
  box. It does not record which binary was tested, in which environment, or what
  was observed. A Markdown checkbox and "PASS — looked fine" are the same
  artifact.
- **No resumability.** A 2–3 hour soak plus twenty interactive checks is longer
  than one sitting. An interrupted pass restarted from the top, so the expensive
  checks were the ones most often skipped.
- **Silent inheritance.** A fix produced a new binary, and the checks that had
  passed against the previous one were not rerun, because nothing tracked which
  bytes a PASS described.

The existing deterministic harnesses (`--auto-record`, `--auto-edit`,
`--visual-test`, `--hwnd-audit`, `--preview-smoke-test`, the A/V-sync analyzer,
the soak tooling) each answer one question well and are all launch-and-exit: they
cannot observe or drive a *running* application. Several remaining gate items are
exactly that shape — "start recording, pause it, confirm the state, move the
window across a monitor boundary, confirm the preview is still presenting".

## Decision

Compile a narrowly scoped **Live Verify control server** into the shipping
`exosnap.exe`, dormant unless explicitly armed, and build a thin client and a
resumable orchestrator on top of it.

### The server is in the shipping binary and inert by default

Official acceptance must exercise the executable users receive. A test-only build
proves things about a binary nobody installs. So the server is compiled in — and
its only activation is an explicit argv option carrying a run id:

```
exosnap.exe --live-verify-control <run-id>
```

Explicitly **not** activation triggers: a Debug configuration, an environment
variable, a settings key, or any "this looks like a developer machine"
heuristic. Each of those can be true on a user's machine without the user asking,
and the channel drives real recordings. A malformed option is fatal rather than
ignored, because a runner that believes it armed the channel and silently got a
normal application would report acceptance for a process it never drove.

Without the option: no pipe, no thread, no log line, no behaviour change.

### Transport: a native Windows named pipe

Candidates were `QLocalServer` and a native named pipe. The pipe wins on three
counts:

1. **No TCP, provably.** A named pipe has no port and no listening socket, so
   "unreachable from the network" is a property of the API rather than of a bind
   address that could be mistyped. `netstat` showing nothing is then real
   evidence.
2. **No new dependency in the package.** `QLocalServer` would add Qt6::Network to
   the shipped tree for one release-verification seam that is dormant in every
   user launch. (Qt's Windows implementation is a named pipe underneath anyway.)
3. **An explicit DACL.** The endpoint is created with a discretionary ACL built
   from the process token's own SID — the creating user and nobody else — plus
   `PIPE_REJECT_REMOTE_CLIENTS`.

The endpoint name embeds the run id (`\\.\pipe\ExoSnap.LiveVerify.<run-id>`), so
two verification runs cannot collide, and a normal second ExoSnap instance —
which opens no pipe at all — can never be mistaken for one.

### Protocol: NDJSON, versioned, with a mandatory handshake

One JSON object per line. A release-acceptance transcript is evidence, and
evidence a human cannot read is evidence nobody audits.

```json
{"protocol":1,"id":"42","command":"record.pause","params":{}}
{"protocol":1,"id":"42","ok":true,"result":{"stateText":"Paused"}}
{"protocol":1,"event":"record.stateChanged","data":{"stateText":"Paused"}}
```

The first command on a connection must be `system.hello`, carrying the run id the
process was launched with. It answers with the identity the runner needs to
refuse a process it did not mean to talk to: product version, full commit SHA,
build id, configuration, install mode, channel, executable path, executable
SHA-256 and PID. A wrong run id is fatal for the connection — the credential is
not retried against a live application.

### Security boundary: an allowlist of semantic intents

There is no generic invocation of anything. No `QObject` lookup, no method
invocation by name, no QML property write, no shell execution, no file access, no
process spawning, no `eval`. The entire reachable surface is the member functions
of one interface (`LiveVerifySource`); a command that is not a member cannot be
reached, and adding one is a code change with a review.

Every intent routes through the **same application entry points the QML surface
calls** (`RecordViewModelAdapter::requestStart()` and friends), and is refused
when the application says the control is unavailable. A check that drove
`RecordingCoordinator` directly would prove the engine works while saying nothing
about the product, and would keep passing after the button stopped being wired to
it.

One deliberate exception: `window.moveToScreen`. Placing a window on a screen is
not an intent the product exposes to users at all. It exists because the
cross-monitor Preview defect needs a boundary crossing and synthesising a mouse
drag is ruled out (`CLAUDE.md`). The interactive drag therefore stays a human
gate, precisely because programmatic placement is not the same thing.

Unknown commands, malformed JSON, oversized frames and wrong parameters all fail
closed with a stable error code, and none of them can take the application down.

### The control channel does not replace UI Automation

An IPC call is not a UI test. Where an acceptance requirement is "the visible
control is wired to the behaviour", the check uses Windows UI Automation to
activate the real control (by role and accessible name, never by screen
coordinate — UIA does not move the OS cursor and so cannot collide with the
developer's own pointer) and the control channel to observe the authoritative
state that follows. Neither half alone is the proof.

### The control channel does not replace the existing harnesses

The ladder is: unit/integration test → existing deterministic harness → control
channel → UI Automation → external media/artifact verification → human gate.
Nothing may use a weaker layer than the requirement needs. In practice that means
`--hwnd-audit`, `--auto-record`/`--auto-edit` and ffprobe are *invoked* by the
runner rather than reimplemented over the wire.

### Evidence and artifact binding

Every automated PASS carries machine evidence (snapshots, transcripts, hashes,
tool output) and the fingerprint of the artifact and the environment properties
it depended on. When either changes, the PASS becomes `STALE` rather than being
inherited by a binary nobody tested. Environment dependencies are declared per
check, so rearranging monitors invalidates the cross-monitor Preview check and
nothing about updater identity.

### Performance

When armed, the channel must not perturb what it measures. No per-frame events,
no polled hot state: the preview reports counters that already exist (the redraw
gate's publish/wake/render tallies), and events reuse signals the application
already emits.

## Consequences

- The shipping binary carries a control server. It is inert by default, its
  activation is a single explicit option, and its reachable surface is an
  allowlist — but this is a real production-binary control boundary and is
  treated as one: the isolation and rejection properties are covered by tests,
  not by inspection.
- Release acceptance becomes resumable and auditable. The list of genuinely human
  gates shrinks to subjective desktop composition, an interactive monitor drag,
  physical device manipulation and Secure Desktop prompts.
- A local Release dry run validates the infrastructure. It does **not** prove
  final release acceptance: after any product code change, official acceptance
  needs a newly published, immutable RC, and a previous RC's PASS does not apply
  to changed bytes.

## Explicitly not this

A future **ExoSnap Remote Recording Node** concept may involve LAN networking,
remote storage, remote encoding, discovery and pairing. This is not that, and
must not become its foundation:

- the Live Verify server is local-only and stays local-only,
- it is not a generic remote-control API, a scripting API, an automation SDK or a
  plugin framework,
- no TCP listener, no discovery, no media transport, no pairing.

A Remote Recording Node may later be *tested by* Live Verify. It must not be
built on top of the Live Verify protocol.
