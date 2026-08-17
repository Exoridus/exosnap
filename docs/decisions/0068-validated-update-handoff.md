# 0068 — The App → Updater handoff is a versioned document, not a search

- Status: accepted
- Date: 2026-08-17
- Related: ADR 0012 (update security model), ADR 0034 (in-app update and dual swap),
  ADR 0055 (verification reinstall mode), ADR 0066 (Live Verify local control channel),
  ADR 0067 (shared control layer and updater endpoint)

## Context

ExoSnap started `exosnap-updater.exe` with seven search arguments:

```text
--channel --install-mode --install-dir --app-pid --current-version
--target-version --base-url
```

Every one of them described the same operation, and together they were an
unversioned second contract for it. Three concrete consequences:

1. **The child re-resolved the release.** `--channel` and `--base-url` exist so
   the updater can fetch the releases feed and pick a release *again*. A release
   published between the application's check and the updater's start would win.
   `--target-version` was added to compensate: the app pinned the tag it had
   offered and the updater refused anything else. That closed the hole by
   detecting the second resolution rather than by removing it.
2. **`--install-dir` was an unvalidated instruction.** It named the directory a
   staged rename would replace, and nothing checked that it was an ExoSnap
   installation at all.
3. **Nothing correlated the two processes.** Parent and child could be shown to
   agree on a version string; there was no identity that said *this is one
   operation*, so any evidence tying them together was inferred from timing.

Adding an eighth argument for a transaction id would have made all three worse.

## Decision

**Replace the search arguments with one versioned handoff document, and make the
updater re-establish the trust chain over it.**

```text
exosnap-updater.exe --apply-handoff <path>
```

### The document

`libs/update_handoff` owns the schema; both processes link it, so producer and
consumer cannot drift.

```json
{
  "handoffVersion": 1,
  "updateTransactionId": "u-0123456789abcdef",
  "targetVersion": "0.9.0-rc9",
  "currentVersion": "0.9.0-rc1",
  "manifestPath": "…/update-transactions/u-…/update-manifest.json",
  "manifestSignaturePath": "…/update-transactions/u-…/update-manifest.json.sig",
  "installMode": "portable",
  "installDir": "…/ExoSnap",
  "appPid": 4321,
  "verifyReinstall": false
}
```

The application prepares it on the **check** worker: the check that resolves a
release also downloads that release's manifest and detached signature into a
per-transaction directory. Release resolution is now unambiguously the parent's
job, which is what lets the child stop doing it. `update.apply` then writes the
document atomically (`QSaveFile`: temporary sibling, then rename) and launches
the updater with two options at most — the handoff, and `--automation-control`
when this process is itself being driven.

### The document is untrusted

It carries **no signature of its own**, on purpose. It is written into a
user-writable directory and no decision may rest on one of its fields alone; it
*points at* the release trust chain that already exists. The updater, which is
the process that performs the destructive action, re-establishes trust itself:

```text
read the exact manifest bytes  (once, into memory)
  → ed25519 verify against the pinned public key
    → ONLY THEN parse a single field
      → manifest.version == handoff.targetVersion, byte for byte
        → download the package named by the VERIFIED manifest
          → deny-write lock + SHA-256 through that handle
            → apply
```

Tampering therefore cannot install anything: point `manifestPath` elsewhere and
the signature fails; change `targetVersion` and the version gate refuses; change
`installDir` and the install-context check refuses.

### Install-context validation

`installDir` is no longer an instruction. It must be absolute, exist, contain
`exosnap.exe`, and that executable's `ProductVersion` string must equal
`currentVersion` **exactly**. In installed mode it must additionally match the
directory Windows Installer recorded, when there is one. This binds the named
directory to the operation instead of trusting it.

### Correlation, not authorisation

`updateTransactionId` is opaque, non-secret and authorises nothing. It is not the
automation run id and does not replace it:

```text
automation run id      → control-session identity (part of the pipe name)
updateTransactionId    → product-operation correlation
```

It is published in `updater.getState`, in the updater's `system.hello` identity
and in the application's `updaterLaunch` snapshot, so an observer can state that
the transaction the application started is the transaction the updater ran.

### Versioning

`handoffVersion` is validated exactly and an unknown value is a **hard reject** —
never a best-effort read. Unknown *additional* keys are ignored, which is the
forward-compatible half: a future writer may add fields, but may not redefine
these without bumping the version.

### A refusal is a product outcome

A document that cannot be accepted produces `FailureCase::HandoffRejected`
(phase `failed`, `installState: intact`, no retry offered, non-zero exit), not a
usage error. It is observable on the automation endpoint like every other
terminal state, because "the updater refused the handoff" is exactly the thing an
acceptance run has to be able to assert.

### What was removed

The seven search arguments are **gone**, not deprecated — ExoSnap is pre-1.0 and
two production paths into one swap is the defect, not the migration cost.
`--channel`, `--base-url` and `--preview-state` survive for the **manual** mode
only, which still resolves the channel itself because nobody told it anything.
A handoff run reads no feed at all and therefore reports no channel.

## Consequences

- The updater cannot install a release the user was not offered, because it never
  looks for one.
- The dev feed override (`--update-base-url`) is now application-only. It still
  cannot be used in an official build.
- The application performs two extra sub-kilobyte GETs per check that finds an
  update, on the check worker thread. A failure there does **not** hide the
  update — a release that exists and is newer must never be reported as "up to
  date" — it is recorded and `update.apply` refuses with that reason.
- Transaction directories live under `%LOCALAPPDATA%\…\update-transactions\`.
  Preparing a new one removes every older one except the directory the last
  launched updater was handed; the updater removes its own on success. Bounded
  without a time heuristic.
- A remaining TOCTOU boundary, stated rather than papered over: the handoff, the
  manifest and the signature live in a user-writable directory, so a local
  process running as the same user can replace them between the write and the
  read. That is not a privilege boundary — the signature over the manifest bytes
  the updater actually reads is what makes the substitution useless, and the
  package lock closes the verify→consume window as before.
