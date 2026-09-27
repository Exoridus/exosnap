# Verification and release trust boundaries

This document owns what each verifier can establish and what release readiness means. Commands belong to the [release runbook](../dev/release-verify.md); the required product/release checks belong to the [release checklist](../release-checklist.md).

## Different owners of truth

| Subject | Appropriate evidence |
|---|---|
| Pure policy, parser or state-machine result | Deterministic unit/contract test |
| Application intent and its settled state | Product control channel |
| A visible control reaching the intended action | UI Automation plus product-state observation |
| What a media/package file contains | Independent analyzer and exact artifact identity |
| Windows environment value | Read-back from the mechanism that owns it |
| Physical display, cable or device behavior | Declared hardware and an observable live consequence |
| Secure Desktop consent | A person acts; software observes the outcome |
| Capture-excluded desktop appearance | A person judges the real desktop; a scene-graph grab is insufficient |

A test must use a verifier strong enough for the claim. A source-level invariant is not a GPU result, a screenshot is not an HWND audit, and a control-channel command is not proof its visible button is wired.

A proxy observation needs a stated gap. `WindowFromPoint` reports the window a hit test finds, not where real input is delivered: it can name the window beneath a click-through overlay while an actual click stays with the overlay. A same-process probe of cross-process behavior can pass for the same reason. Where a proxy cannot see the failure, the oracle also asserts the structural precondition the real mechanism needs, such as the window-style pair for click-through. Oracles derive expectations from the state Windows holds (a window's region, its styles) rather than re-deriving a layout from QML that can drift from it. A scenario over several subjects judges all of them before failing, so that one subject's failure cannot hide another's.

## Product control channel

The shipping executable includes a dormant local control server because acceptance must exercise the same bytes users receive. Only `--live-verify-control <run-id>` arms the application endpoint; the updater uses `--automation-control <run-id>`. A normal launch creates neither endpoint nor its worker. A malformed request fails rather than silently starting an uncontrolled application.

`libs/control` owns transport, envelope, handshake and dispatch policy mechanics. Process-specific owners keep their command tables, state and actions. Native named pipes use the creating user's DACL and `PIPE_REJECT_REMOTE_CLIENTS`. A named pipe by itself is not inherently local-only; those restrictions are part of the contract. Endpoints include a role and run ID so app and updater can share one campaign credential without colliding.

The first request is `system.hello`, which binds the connection to run ID, process/executable identity, full version, hash and build data. Wrong credentials, malformed/oversized frames and unsupported parameters fail closed. A connection selects its protocol version and cannot switch it halfway through a transcript.

Commands are a static allowlist of semantic product operations. They pass through the same admission and action owners as the UI. No generic QObject lookup, method invocation, QML property write, shell execution, arbitrary file access or Windows administration is exposed. Product operations can naturally write recordings/settings or perform an approved update; "no generic file access" must not be misread as "no effects on disk".

Protocol 2 adds observable `stateRevision`, asynchronous `settled` meaning and structured refusal requirements/actual state. An accepted async command is not a completed operation. Revisions change on actionable state, not every progress tick or audio meter sample. Wait for real state, event or bounded external observation rather than an arbitrary sleep. An exiting server does not wait indefinitely for its client to drain pipe output.

The updater publishes its **product** state, including failure case and installation state, rather than maintaining a second automation-only success model. There is no command to arm a new handoff after startup. Such a command would let the control client redefine what the installer acts on.

## Environment orchestration

Environment mutation is outside the shipped product, in test-only `tools/envctl` and verification adapters. The capability catalogue distinguishes read-only, supported/restorable mutation, test-only mutation, operator-owned, physical, secure and unavailable properties. Readable does not mean writable. Audio device format/default-role properties remain operator-owned in envctl even where a separately named test tool can change them.

A mutation snapshots the exact original value, durably journals before changing anything, applies only the needed delta, independently reads back, runs the scenario, restores the original and reads back again. Setter success alone is not evidence. Restore means original, not defaults.

One machine has one outstanding environment journal shared across campaigns. A new campaign must not snapshot an earlier campaign's unresolved mutation as its clean baseline. A failed begin can also leave failed rollback. Unknown/unreadable state is not clean. Missing original devices remain restore-pending; no different device is substituted. The guard process accelerates recovery when an owner dies but cannot promise instant restoration after power loss.

Device aliases bind to stable identifiers. Display journals use monitor device paths, not boot-scoped adapter LUIDs. Ambiguous and unbound aliases are explicit refusals. Refresh read-back uses Windows' reported integer mode values; do not replace an exact restore contract with an arbitrary tolerance around a nominal datasheet rate.

## Campaign isolation and outcomes

The Rust verifier contains process lifetimes with argument-list invocation, output capture and deadlines. Elevated work runs in a separate process with a result-file boundary. A standard process does not inspect elevated UI, and no test automates Secure Desktop.

Tier 0 is hermetic. Tier 1 exercises an ordinary desktop. Tier 2 uses a disposable OS for installation/registry/package state. Tier 3 needs declared physical hardware. Hyper-V GPU-partitioned guests can provide a clean console and GPU access, but are not proof of host-independent performance, physical HDR behavior, device clocks or unplug semantics. Sandbox remains a supported disposable transport where suitable.

Every scenario has one class and states the current contract it protects. A check without a contract does not exist; a historical bug is not a contract.

| Class | Statement | Absent capability means |
|---|---|---|
| regression | Known input compared against a stable oracle (`tests/samples`) | Unavailable |
| contract | A user-visible workflow keeps a durable product invariant | Unavailable |
| capability | Whether this machine can qualify other scenarios; no product claim | Unavailable, never Fail |
| hardware | A contract only physical hardware can demonstrate | Unavailable |
| installer | Installing, updating or removing the product mutates the OS | Unavailable |

A scenario is judged at the cheapest layer that can prove it: unit test, deterministic sample, local product run, Windows Sandbox, Hyper-V, physical hardware. A result holds for `scenario x slot`, where the slot names the kind of machine or environment (`windows11-nvidia-hdr`, `windows11-sandbox`, `windows11-hyperv-clean`). A PASS in one slot never stands in for another.

Scenarios declare capabilities, never backends. `exo-verify` runs a lane where it is, unless the lane needs an environment trait this machine lacks (`disposable-os`, `reboot`, `real-uac`). Then it selects the cheapest disposable backend that guarantees those traits, and reports UNAVAILABLE when none does. A backend copies the same `exo-verify` binary and the candidate bundle into the environment and runs the lane there. The guest probes its own hardware facts and writes its own lane result; the host only transports it and never reinterprets a verdict. A failure to obtain that result is an infrastructure error for every selected scenario, and the environment is destroyed in every case.

Windows Sandbox is driven through its `wsb` command line. `wsb exec` returns only an exit code, so the run directory is mapped into the sandbox and all output travels through it. Hyper-V runs use Hyper-V WMI v2 for the VM lifecycle and the Virtual Disk API for a per-run differencing disk. The host talks to `exo-guest`, a small agent baked into the sealed base image, over a Hyper-V socket. That channel has no network stack, is reachable only from the parent partition, and accepts only a fixed set of process and file operations. It is not remote administration. The base image is immutable, read-only infrastructure described by a manifest (image id, Windows build, update level, agent version, driver set, provisioning version, VHDX SHA-256), and a base whose bytes no longer match its manifest is refused.

A golden VM is never written by a campaign. A differencing disk contains its changes and is deleted after evidence collection. The recipe pins packages and records image/display/driver provenance. GPU-P guest PCI identity is not required to equal host PCI identity; partition provenance, guest adapter identity and staged driver package are separate checks. Actual capture/NVENC reachability still needs probes.

`Fail` means an observed product failure. Infrastructure error means the harness could not establish the observation. Unavailable, blocked, deferred, skipped and stale are not passes. Product outcome and environment-restore outcome are separate: a successful product test can still leave an unacceptable machine state. Operator attestation can state that an action occurred, but it cannot manufacture the verifying consequence or substitute for another person's visual judgment.

## Candidate readiness and publication

The official candidate build compiles the final release identity once. Its bundle inventories the exact executable, MSI, portable ZIP and verifier bytes. The frozen plan binds the source registry and required scenario revisions to that bundle. Lane results carry the bundle hash, and report verification re-derives required results from the plan and source registry. Missing, duplicated or mismatched evidence cannot establish readiness. A maintainer may explicitly accept an unavailable scenario or an observed product failure, with a reason and author visible in the report. An infrastructure error is not a product judgment.

The production Ed25519 update signature authenticates the update-key holder and binds the downloadable package hashes. It does not prove physical observations. The disposable test feed is signed in a separate CI job whose runner never executes candidate-controlled build code with the signing secret. The candidate workflow does not publish a release.

Publication remains blocked until a reviewed workflow independently verifies the frozen report and reuses the exact candidate MSI and ZIP bytes behind the `release` environment. Release preparation and a ready report authorize no tag, GitHub Release or package submission by themselves. Repository-host rules and service settings are external configuration; a checkout cannot prove they are currently enabled.

## Implementation and tests

See [shared control](../../libs/control), [application control](../../app/live_verify), [verifier](../../tools/exo-verify), [guest agent](../../tools/exo-guest), [samples](../../tests/samples), [environment tool](../../tools/envctl), and [VM recipe](../../tools/vm). Hostile-input, refusal and fake-provider tests are essential because a real device cannot reliably reproduce every dishonest-success or failed-restore case.
