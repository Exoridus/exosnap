# Live Verify

Live Verify observes and drives a real ExoSnap process through application-owned semantic intents. It is not the product's general automation API and cannot administer Windows. [Verification boundaries](../architecture/verification-boundaries.md) owns the trust model. [Release verification](release-verify.md) owns candidate campaigns; the [release checklist](../release-checklist.md) owns acceptance requirements.

## Local runner

Close any existing ExoSnap instance before preparing a run. A verification launch retains normal single-instance behavior; launching a second instance can activate the existing window instead of testing the chosen executable.

```powershell
pwsh scripts/live-verify.ps1 prepare -Artifact local
pwsh scripts/live-verify.ps1 list
pwsh scripts/live-verify.ps1 run
pwsh scripts/live-verify.ps1 status
pwsh scripts/live-verify.ps1 report
pwsh scripts/live-verify.ps1 resume
```

Use `retry <check-id>` for an explicit retry, `skip <check-id> -Reason <reason>` for a documented omission, `note <check-id> -Text <text>` for evidence context, and `run -Only <ids>` for a selection. `-NonInteractive` leaves human gates unresolved; it does not approve them.

Each private run directory contains identity, state, environment, artifact fingerprint, per-check evidence, process records and generated Markdown/JSON/JUnit reports. Keep those outputs untracked. The reported path is the actual run path; do not copy a prior campaign's evidence directory into a new identity.

| State | Meaning |
|---|---|
| PENDING / RUNNING | Not attempted / attempt persisted before execution |
| PASS / FAIL | Proven / disproven by the check's evidence |
| BLOCKED / MANUAL_REQUIRED | Preconditions missing / human boundary remains |
| SKIPPED | Deliberately omitted, with a reason |
| UNVERIFIED | Attempted but no trustworthy outcome, including interrupted RUNNING |
| STALE | Evidence applies to different artifact bytes or relevant environment |

Resume rehashes the executable and checks declared environment dependencies. A display change invalidates display-dependent evidence, not unrelated updater identity. Interruptions never become successful results.

## Arming and connection identity

```powershell
exosnap.exe --live-verify-control <run-id>
exosnap-updater.exe --automation-control <run-id>
```

Run IDs are 8–64 characters from `[A-Za-z0-9._-]`. Malformed explicit activation exits with usage failure rather than starting normally. Normal launches create no endpoint. Use a fresh unpredictable run ID for each campaign.

The native pipe roles are `ExoSnap.LiveVerify.<run-id>` and `ExoSnap.Updater.<run-id>`. Both have a creating-user DACL and remote-client rejection. The common `libs/control` implementation owns framing, handshake, limits and teardown; each process owns its command allowlist. An update transaction ID correlates an operation but is not the pipe credential.

Live Verify is not a harness-isolation switch. Set `EXOSNAP_CONFIG_DIR` and `EXOSNAP_OUTPUT_DIR` explicitly when a run must not touch a user's profile/output. Do not disable the single-instance, close or recording guards to make a check pass.

```powershell
pwsh scripts/live-verify-client.ps1 hello -RunId <run-id>
pwsh scripts/live-verify-client.ps1 capabilities -RunId <run-id>
pwsh scripts/live-verify-client.ps1 state -RunId <run-id>
pwsh scripts/live-verify-client.ps1 describe -RunId <run-id>
pwsh scripts/live-verify-client.ps1 query record -RunId <run-id>
pwsh scripts/live-verify-client.ps1 command record.pause -RunId <run-id>
pwsh scripts/live-verify-client.ps1 wait record.stateChanged -Where stateText=Paused -TimeoutSeconds 10 -RunId <run-id>
```

The client exits 0 on success, 2 for usage, 3 for connection/handshake/protocol failure, 4 for an answered refusal and 5 for timeout. It does not silently reconnect/retry: process replacement can be the very fact an update test is observing.

## Protocol and discovery

Protocols 1 and 2 are supported; a connection negotiates one and cannot switch dialect halfway through its transcript. Use `system.capabilities` for the actual command/event surface and protocol-2 `ipc.describe` for parameters, errors, idempotence and settling behavior. Do not hardcode a historical command count as a support boundary.

Protocol 2 adds a monotonic `stateRevision` when published product state changes, `settled` on mutations, structured refusal `requires`/`actual`, and optional `includeState`. Progress bytes, meters and preview frames do not advance the state revision. `ok:true` can mean accepted asynchronously, not completed.

`invalid_state` means the operation has no valid current context. `blocked` means a product rule refuses an otherwise applicable operation. `operation_failed` means an admitted operation did not achieve its postcondition. Protocol 1 maps the applicable refusal cases to its older envelope; do not parse human text to reconstruct protocol-2 fields.

## Application surfaces

| Surface | Use |
|---|---|
| `system.hello`, `system.capabilities`, `system.snapshot` | Process identity, exact surface and display/environment facts |
| `app.snapshot`, `ui.getState` | Appearance/navigation and named product state, blocking surface, edit session versus visibility, available actions |
| `window.snapshot`, `window.moveToScreen` | Native style/geometry/affinity and application-owned placement; use a screen name returned by snapshot |
| `preview.snapshot` | Publication/render counters and outstanding-frame debt |
| `record.snapshot`, `record.selectTarget`, `record.start/pause/resume/stop/split/captureFrame`, `record.result` | Real source/transport/result flow with the same admission guards as UI |
| `ui.navigate`, `ui.reveal`, `ui.scrollHome`, `ui.scrollEnd` | Guarded navigation and actual visibility/scroll postconditions |
| `edit.open`, `edit.playPause/seek/setTrimIn/setTrimOut/timelineHome/timelineEnd/close` | Existing edit session operations; only open creates a session |
| `sourcePicker.open/close`, `notificationHub.open/close`, `notification.clearAll` | Application popups and hub state |
| `diagnostics.snapshot`, `diagnostics.setInDepth` | Measured diagnostic state and the session-scoped in-depth switch |
| `settings.*`, `pipeline.*`, `notifications.*`, other discovered surfaces | Use the schema/allowlist returned by this build; fixture paths must match emitted data |

Reveal/scroll commands require the target page to be the current page, even if another page's object remains resident. A syntactically valid target name is not proof that it reached the viewport. Navigation goes through the same guard as tabs and Ctrl+1…5.

The endpoint does not expose arbitrary shell execution, registry writes, Windows display/audio setters, arbitrary URL opening or destructive recovery/crash actions. Their UI state can be observable without granting mutation authority.

Events include application ready, recording state/result, screen change and protocol-2 UI state changes. Prefer a synchronous settled response, then an event/revision wait, then bounded polling of a field that has no event. Fixed measurement intervals and recording duration are legitimate; an arbitrary sleep in place of a postcondition is not.

## Update handoff and updater automation

`update.getState`, `update.check` and `update.apply` invoke the Settings card's real paths. Apply is accepted only when the card offers update/reinstall. Its response and state expose the child PID, staged executable/hash, pinned target and updater endpoint. Attach to that reported child; never discover an arbitrary updater process by name.

Cross-process checks require an installed-layout tree. A Debug build's suffixed Qt DLLs and sibling target directories are not the updater's runtime layout. Use a validated install tree or release package. A missing staged runtime file is an honest launch failure, not a reason to teach the product a special build-tree layout.

In app-handoff mode the updater validates one versioned handoff and independently verifies the supplied manifest signature before trusting its fields. It installs the pinned version or nothing, never resolves the feed again. Malformed/unknown handoff, wrong current install or target mismatch is a visible product refusal. A development HTTPS feed override belongs only to the app, is refused in official builds and does not change keys or hashes.

Updater commands are `updater.getState`, `.check`, `.download`, `.apply`, `.retry`, `.cancel` and `.close`. Follow `availableActions` and `retryEntryStep`. Actions are asynchronous; wait for an actual phase/revision result. Download byte progress is separate from revision. No command can arm/change a handoff after startup.

Cancel is honored only during download. Checking and waiting for the parent do not have cancellation-aware operations, and install/verify/launch are critical sections. A canceled download reports cancelled, no failure case, intact install and process exit 5. Updater process outcomes distinguish applied (0), failure (1), usage (2), manual up-to-date (3), reboot required (4) and canceled/no outcome (5).

`installState` can be intact, restored, strandedInBackup or unknown. In particular, MSI verification failure does not prove Windows Installer rollback. An attached client must not prevent process exit; server teardown does not wait indefinitely for a client to drain unread pipe data.

## Human and evidence limits

UI Automation can establish that a capture-excluded window exists and contains text. It cannot establish the color/alpha the desktop actually composes. A scene-graph grab has the same limit. A native interactive cross-monitor drag differs from programmatic placement. UAC is a Secure Desktop decision, never a scripted click.

The runner should prepare machine-observable state, ask one bounded human question only at the irreducible boundary, and verify the observable consequence afterward. A local dry run validates reachable behavior/infrastructure, not official candidate acceptance.

Tests live in the protocol/server/runner-state suites:

```powershell
pwsh scripts/run-tests.ps1 -Filter live_verify
```

They cover activation, framing, hostile clients, allowed intents, asynchronous state, shutdown and artifact-bound result handling. Real cross-process package installation and real hardware remain separate acceptance layers.
