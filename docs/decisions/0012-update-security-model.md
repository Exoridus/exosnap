# ADR 0012: Update Security Model

## Status

Accepted — implementation scheduled for 0.4.0 (see roadmap).

## Context

ExoSnap runs with user-level privileges and writes media files to user-chosen paths. An update
mechanism that pulls and executes code must be treated as a security surface, not just a
convenience feature. Without explicit constraints, auto-update implementations tend to:

- Fetch binaries without hash or signature verification.
- Store a server token in the client binary.
- Silently restart the application mid-recording.
- Offer no rollback when an update causes a regression.
- Apply to self-built binaries where no supply chain exists.

## Decision

### Signed manifest and package hash

Every update manifest is signed with a private key held exclusively by the ExoSnap release
pipeline. The client verifies the manifest signature before reading any content from it. Each
package listed in the manifest includes a cryptographic hash (SHA-256 minimum). The client
verifies the hash of the downloaded binary before executing it. An update that fails signature
or hash verification is rejected silently with a structured log entry; no partial binary is
retained.

### No GitHub token in the client

The client binary contains no API token, OAuth credential, or secret of any kind. Update checks
use only public, unauthenticated endpoints (e.g., public GitHub Releases API or a public manifest
URL). Any rate-limiting is handled by respecting standard HTTP retry-after headers.

### Downgrade and rollback protection

The manifest includes a minimum accepted version field. The client refuses to install a package
whose version is below this field (downgrade protection). If a user explicitly requests rollback
to a prior version, that action requires user confirmation and is only permitted if the target
version is above the minimum accepted version declared in the current manifest.

### No update during recording or finalization

Update download and installation are blocked while a recording session is active or while a
file is being finalized (muxer flush, moov write, recovery manifest commit). The update UI
shows a clear reason. The update resumes or retries after the session ends.

### No silent auto-restart

The application never silently restarts itself to apply an update. The user is notified that a
restart is needed and initiates it explicitly. If the application is closed and reopened normally,
a pending update may be applied at that point — but only after user confirmation on first launch
if the update was not explicitly approved.

### Portable vs. installed distinction

The update mechanism behaves differently depending on installation mode:

| Mode | Update behavior |
|---|---|
| Installed (installer) | Standard auto-update flow; installer applies update |
| Portable ZIP | Update check only — notifies user of a new version; does not modify files in place |

The portable ZIP is extracted to a user-chosen location that the application has no right to
modify unilaterally. Update notification links to the releases page.

### Updates off by default for self-built binaries

When the build does not define `EXOSNAP_OFFICIAL_BUILD` (or equivalent), the update check is
disabled at compile time. Self-built binaries do not phone home and do not receive update
notifications. This is enforced in the build system, not by a runtime flag.

### Stable and Preview channels

The update system supports two channels: `Stable` and `Preview`. The default is `Stable`. Users
may opt in to `Preview` in settings. Each channel has its own signed manifest. Channel switching
requires an explicit user action and a restart.

## Consequences

- No update-related secret is ever present in the client binary or configuration file.
- A compromised update server cannot push unsigned packages; the client rejects them.
- Users on portable builds receive only version notifications, not silent updates.
- Self-built developer builds have no update mechanism, reducing the attack surface for
  development environments.
- The 0.4.0 implementation slice must include the signing key infrastructure, manifest format
  specification, and installer integration before shipping the auto-updater.
