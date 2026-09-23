# Verification and release trust boundaries

This document owns what each verifier can establish and what a release qualification means. Commands belong to the [release runbook](../dev/release-verify.md); the required product/release checks belong to the [release checklist](../release-checklist.md).

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

The typed .NET harness contains process lifetimes with argument-list invocation, concurrent stdout/stderr draining, deadlines and kill-on-close job objects. Elevated work runs in a separate worker and returns a result document. A standard process does not inspect elevated UI, and no test automates Secure Desktop.

Tier 0 is hermetic. Tier 1 exercises an ordinary desktop. Tier 2 uses a disposable OS for installation/registry/package state. Tier 3 needs declared physical hardware. Hyper-V GPU-partitioned guests can provide a clean console and GPU access, but are not proof of host-independent performance, physical HDR behavior, device clocks or unplug semantics. Sandbox remains a supported disposable transport where suitable.

A golden VM is never written by a campaign. A differencing disk contains its changes and is deleted after evidence collection. The recipe pins packages and records image/display/driver provenance. GPU-P guest PCI identity is not required to equal host PCI identity; partition provenance, guest adapter identity and staged driver package are separate checks. Actual capture/NVENC reachability still needs probes.

`Fail` means an observed product failure. Infrastructure error means the harness could not establish the observation. Unavailable, blocked, deferred, skipped and stale are not passes. Product outcome and environment-restore outcome are separate: a successful product test can still leave an unacceptable machine state. Operator attestation can state that an action occurred, but it cannot manufacture the verifying consequence or substitute for another person's visual judgment.

## Qualification and promotion

A qualification binds a campaign to an exact executable, packages, source commit, environment, harness/catalog/policy and evidence digests. Required IDs are re-derived from the source catalogue and release policy, not trusted from the record's own claim. Missing/duplicate required results, nonpassing required gates, any product/infrastructure failure, incomplete restore or missing evidence disqualifies.

The detached Ed25519 signature is checked before reading record fields. It authenticates the release-key holder's authorization; it does not cryptographically prove that physical observations occurred. Protecting that key and reviewing the evidence remain trust assumptions. A signed record cannot widen its own promotion difference budget.

The full release identity is compiled into runtime data and executable `ProductVersion` strings. Numeric Windows Installer version fields contain the base version only. Therefore a candidate and final built from the same source are different artifacts, and a final is not a renamed candidate ZIP.

The current promotion contract compares the install-tree file inventory and toolchain. Files outside its named executable set must be byte-identical. For `exosnap.exe`, `exosnap-updater.exe` and `crashpad_handler.exe`, differing files must provide matching PE-section inventories; only `.rdata` and `.rsrc` may differ. Other sections, including code and relocation sections, must match. Fixed-width identity fields reduce incidental layout movement.

This is stronger than "same commit" but weaker than exact-byte reuse. **Entire permitted sections** can differ, not only semantically identified version strings. The MSI's internal structure is covered by its own packaging/content assertions rather than exact candidate-to-final MSI replay. Source, runtime dependencies, build manifests, signer and release workflow remain part of the trust chain.

Release preparation/qualification authorizes no publication by itself. Creating version tags, attaching a signed qualification or promoting a final requires the maintainer's explicit release authorization. Repository-host rules and service settings are external configuration; a checkout cannot prove they are currently enabled.

## Implementation and tests

See [shared control](../../libs/control), [application control](../../app/live_verify), [typed verifier](../../tools/release-verify), [environment tool](../../tools/envctl), [VM recipe](../../tools/vm), [qualification policy](../../scripts/lib/ReleaseQualification.ps1), and [promotion comparison](../../scripts/check-release-promotion.ps1). Hostile-input, refusal and fake-provider tests are essential because a real device cannot reliably reproduce every dishonest-success or failed-restore case.
