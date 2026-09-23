# Update, process security and crash reporting

This document owns the application/updater trust chain and local crash-capture boundary. [SECURITY.md](../../SECURITY.md) owns vulnerability reporting and support policy. [PRIVACY.md](../../PRIVACY.md) owns the public privacy statement. Release verification is covered separately in [verification boundaries](verification-boundaries.md).

## Network and consent boundary

Automatic update checks default off, including official builds. Manual checks are explicit user actions. Ordinary self-builds do not use the production update-check path and do not compile in the official crash-upload configuration. A development feed override is session-only, requires HTTPS and a host, and is refused by official builds. It does not relax signatures or package hashes.

Update checks use the public GitHub release feed without a token and compare versions locally. The fixed checker User-Agent is not the installed application version. Crash upload is a separate, consent-gated Sentry path. No account, analytics or recording upload is introduced by either feature. External server configuration is not proved by a source-code inspection; the [privacy review](../privacy-review.md) records that verification boundary.

## Signed release trust chain

The update manifest is verified with the embedded Ed25519 public key over its exact received bytes **before** parsing any manifest field. Detached signature and manifest must belong together. Package download is selected from the verified manifest and SHA-256 checked through a file handle that denies modification/replacement while the verified package is consumed.

The update applies only the offered target and refuses downgrade. Exact full-version equality is used for identity-sensitive operations; version precedence is for ordering, not for proving that two artifacts are the same release. The current installation mode determines whether update means portable swap, MSI execution or notify-only Scoop behavior.

The signature authenticates a release-key authorization, not arbitrary local metadata or a claim that no administrator could alter the installation. A writable handoff is not an authorization token.

## Versioned application handoff

`libs/update_handoff` owns the common document contract. The application resolves the offered release and stages its manifest/signature on the check worker. Applying atomically writes the handoff and starts `exosnap-updater.exe --apply-handoff <path>`. A failed manifest fetch does not turn a known newer release into "up to date"; apply remains refused with the actual reason.

The handoff includes its schema version, non-secret transaction ID, exact current/target versions, installation mode/path, parent PID, manifest/signature paths and the reinstall flag. The updater validates the document and installation context, re-verifies the signed manifest, matches the target exactly and then downloads the verified package. In app-handoff mode it **does not resolve a release feed again**. A release published after the offer cannot silently replace that offer.

The installation directory must be absolute, exist, contain the matching ExoSnap executable, and match the installed context where applicable. Unknown handoff versions and invalid required fields are rejected before installation. Extra fields do not redefine known ones. A handoff rejection is an observable product failure with intact installation, not a successful no-op.

The transaction ID correlates the application's child-launch snapshot, updater identity and state. It is not the automation run credential. Same-user replacement of handoff files remains possible; verification of the actual manifest bytes and the package handle protects the release trust chain. Do not describe that as general protection against a malicious local administrator or as proof every local path is immutable.

## Process and installation lifetime

The updater is staged outside the live installation with the runtime files it needs, so it does not prevent replacing itself. The app's card enters Updater running on launch and Pending only after the marked close/handoff is accepted. A child that exits before handoff re-arms an actionable state instead of leaving a persisted pending fiction.

Portable update uses staged replacement: old installation to backup, verified new tree to live, installed-version/health checks, then approved relaunch and cleanup. Failure can restore the backup; if restoration itself fails, report the stranded/unknown state. Interrupted swaps are inspected and self-healed before another normal update proceeds.

MSI update invokes Windows Installer with the verified, locked package and the required UAC boundary. The updater itself remains `asInvoker`, not a generally elevated application. Windows Installer results and post-install verification do **not** establish the same rollback guarantee as a portable backup. `installState` can truthfully be `unknown`; the application must not promise that every MSI verification failure restored the previous version.

The only accepted cancellation phases are those whose worker observes cancellation, as declared by the current command policy. Download cancellation leaves the installation untouched. Installing, verifying and launching refuse close/cancel instead of acknowledging an interruption that cannot safely happen. Retry availability is mode-aware; an immutable bad handoff manifest cannot be fixed by endlessly retrying the same child operation.

Success removes downloaded package/manifest/signature and the completed transaction's temporary state. Failure can retain data needed for diagnosis or retry. Transaction cleanup must not remove the directory still used by a launched updater.

## Manual start and verification reinstall

A manually started updater discovers the on-disk context and remains idle until asked to check. Check, download and install are separate confirmations in manual mode. This is different from the app-handoff flow, where the user's Update action already authorized that specific operation.

`--verify-update-reinstall` is a per-launch application option that can additionally offer an exactly matching installed full version. It persists nothing, never permits a downgrade, never bypasses guards/signatures/hashes and never writes a normal applied-version loop stamp. The updater requires the verified target to match the reinstall identity. It exists to test the current candidate's own production path, not to spoof an older version or provide an implicit repair mode for every installation.

## Bootstrap and local control

DLL search policy is established before Qt/plugin loading. PATH is not a trusted runtime dependency location. Build and install trees must stage the libraries their application actually loads.

The optional local automation endpoint is dormant unless explicitly armed by its argv run ID. Per-user DACL plus rejection of remote named-pipe clients constrains transport. The allowlist exposes product operations, not arbitrary methods, file reads, shell commands or Windows administration. A same-user process with the credential can drive the allowed real actions; it is not a sandbox against that user. Full control-channel invariants are in [verification boundaries](verification-boundaries.md#product-control-channel).

## Crash capture and reporting

Official crash capture uses an out-of-process Crashpad handler. A local fallback exists for builds without that component. A missing clean-exit marker identifies an interrupted previous session, not necessarily a known exception or culprit module. Read previous-session context before starting a new sidecar and do not substitute current machine facts as crash evidence.

Crash reporting is offered on next launch, separately from recording recovery. Ask every time is the default. Remembered Send automatically and Never send are explicit policies. One-shot Send releases/flushes the pending report and resets consent; closing a dialog does not commit a policy. Never send does not disable local recovery.

The structured Sentry event passes a tag allowlist and scrubber. Native minidump bytes are a **different channel**: their module list can contain installation paths, including a username for a portable installation beneath a user profile. The structured scrubber does not sanitize the binary dump. No raw recording is a crash-report input.

Release PDBs enable server-side symbolication and are archived outside the runtime package. Their collection is distinct from successful upload to the crash service. Do not claim automated symbol publication unless the release workflow actually performs it.

## Implementation and tests

See [update checker](../../libs/update/src/update_checker.cpp), [package verifier](../../libs/update/include/update/package_verifier.h), [handoff](../../libs/update_handoff/include/update_handoff/handoff.h), [updater worker](../../apps/updater/UpdaterWorker.cpp), [update service](../../app/services/UpdateService.cpp), [bootstrap](../../app/bootstrap/ProductionBootstrap.cpp), and [crash scrubber](../../libs/crash_capture/include/crash_capture/crash_scrubber.h). Negative tests and live packaged-artifact checks are both necessary; one cannot substitute for the other.
