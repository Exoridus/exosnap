# Update, process security and crash reporting

This document owns the application/updater trust chain and local crash-capture boundary. [SECURITY.md](../../SECURITY.md) owns vulnerability reporting and support policy. [PRIVACY.md](../../PRIVACY.md) owns the public privacy statement. Release verification is covered separately in [verification boundaries](verification-boundaries.md).

## Network and consent boundary

Automatic update checks default off, including official builds. Manual checks are explicit user actions. Ordinary self-builds do not use the production update-check path and do not compile in the official crash-upload configuration. A development feed override is session-only, requires HTTPS and a host, and is refused by official builds. It does not relax signatures or package hashes.

Update checks use the public GitHub release feed without a token and compare versions locally. The fixed checker User-Agent is not the installed application version. Crash upload is a separate, consent-gated Sentry path. No account, analytics or recording upload is introduced by either feature. External server configuration is not proved by a source-code inspection; the [privacy review](../privacy-review.md) records that verification boundary.

## Distribution and update ownership

`libs/update` resolves one `DistributionContext`. `InstallMode` describes physical deployment: `Portable` is a replaceable application tree, and `Installed` is structurally owned by Windows Installer. `DistributionOwner` describes the distribution channel independently. Settings, About, update preparation, launch eligibility and live verification consume this context and its pure `ResolveUpdatePolicy` result.

| InstallMode | DistributionOwner | Update route |
|---|---|---|
| Portable | Direct | BuiltInPortableSwap |
| Installed | Direct | BuiltInMsi |
| Either | WinGet | ExternalPackageManager |
| Either | Chocolatey | ExternalPackageManager |
| Portable (or Installed defensively) | Scoop | ExternalPackageManager |
| Either | UnknownManaged | ExternalPackageManager, notify-only |

Stable owner tokens are `direct`, `winget`, `chocolatey`, `scoop` and `unknown`. An explicit unrecognized value stays `UnknownManaged`; absence is a separate legacy case.

For an MSI tree, the detector reads `installed`, `InstallPath` and `DistributionOwner` from the same open installation record. The authoritative product record is `HKLM\Software\ExoSnap`. Existing legacy/per-user record fallback remains path-matched and never mixes an owner's value from another record. A missing owner resolves to Direct for compatibility. Recognized MSI values are `direct`, `winget` and `chocolatey`; a present empty, malformed or unsupported value is externally managed.

The MSI public property `EXOSNAP_DISTRIBUTION_OWNER` accepts exactly `direct`, `winget` or `chocolatey`. An explicit recognized caller value wins. Without it, the installer preserves the existing persisted owner across repair and major upgrade, including an unknown retained value, rather than converting it to Direct. Only a fresh installation with neither value defaults to `direct`. Invalid explicit caller values fail installation before any write. Standard MSI AppSearch cannot distinguish a missing value from an existing empty string. A small embedded, read-only MSI action queries existence and the registry value type in the same HKLM64 product key. A present empty, incorrectly typed or otherwise unreadable marker resolves conservatively to `unknown` instead of the fresh-install default. The action modifies only MSI session properties and is not part of the application or staged updater runtime. The marker belongs to the same product registry component as `installed` and `InstallPath`.

WinGet supplies `EXOSNAP_DISTRIBUTION_OWNER=winget` through the manifest's custom installer switch. Chocolatey's official-MSI wrapper supplies `EXOSNAP_DISTRIBUTION_OWNER=chocolatey` in `silentArgs`. Source validators and release rendering require these switches. The app does not run `winget list` or inspect Chocolatey package directories at startup.

Scoop remains Portable with owner Scoop. The resolver prefers adjacent regular `scoop-install.json` / `scoop-manifest.json` files. Older Scoop versions use an adjacent `install.json` and `manifest.json` pair; those require recognizable install source/architecture and release asset metadata. Compatible path evidence includes `.../scoop/apps/exosnap/...` and relocated `.../apps/exosnap/current`. It does not depend on `%USERPROFILE%\scoop` and does not add an ExoSnap marker file.

Managed installations retain manual and enabled automatic release checks, version validation and release notes. They skip self-apply manifest/signature transaction preparation and never stage the updater, write an apply handoff, download an installation payload for self-apply, run the portable swap or execute ExoSnap's MSI apply path. The standalone updater independently resolves the target installation's ownership before download/apply, including app-handoff mode; its externally staged location is not ownership evidence.

The existing Settings card shows the manager and offers Copy command, without running, elevating or shelling any manager:

| Owner | Recommended command |
|---|---|
| WinGet | `winget upgrade --id Codexo.ExoSnap --exact` |
| Chocolatey | `choco upgrade exosnap` |
| Scoop | `scoop update exosnap` |
| UnknownManaged | No guessed command. Update with the package manager that installed ExoSnap. |

Ownership attribution is installer metadata, not a cryptographic guarantee. WinGet's caller-controlled `--override` can replace manifest switches. Installations predating this marker, or arriving without the owner switch, remain Direct through the legacy fallback. There is no reliable retroactive WinGet/Chocolatey discovery. The first future manager-driven upgrade passing its owner property establishes durable ownership. Ordinary later MSI maintenance without an explicit owner preserves it. There is no in-app ownership override.

## Signed release trust chain

The update manifest is verified with the embedded Ed25519 public key over its exact received bytes **before** parsing any manifest field. Detached signature and manifest must belong together. Package download is selected from the verified manifest and SHA-256 checked through a file handle that denies modification/replacement while the verified package is consumed.

The update applies only the offered target and refuses downgrade. Exact full-version equality is used for identity-sensitive operations; version precedence is for ordering, not for proving that two artifacts are the same release. Distribution ownership determines who applies an update; physical installation mode determines the built-in mechanism for a Direct installation.

The signature authenticates a release-key authorization, not arbitrary local metadata or a claim that no administrator could alter the installation. A writable handoff is not an authorization token.

## Versioned application handoff

`libs/update_handoff` owns the common document contract. For Direct self-updatable installations, the application resolves the offered release and stages its manifest/signature on the check worker. Applying atomically writes the handoff and starts `exosnap-updater.exe --apply-handoff <path>`. A failed manifest fetch does not turn a known newer release into "up to date"; apply remains refused with the actual reason.

The handoff includes its schema version, non-secret transaction ID, exact current/target versions, installation mode/path, parent PID, manifest/signature paths and the reinstall flag. The updater validates the document and installation context, re-verifies the signed manifest, matches the target exactly and then downloads the verified package. In app-handoff mode it **does not resolve a release feed again**. A release published after the offer cannot silently replace that offer.

The installation directory must be absolute, exist, contain the matching ExoSnap executable, and match the installed context where applicable. Unknown handoff versions and invalid required fields are rejected before installation. Extra fields do not redefine known ones. A handoff rejection is an observable product failure with intact installation, not a successful no-op.

The transaction ID correlates the application's child-launch snapshot, updater identity and state. It is not the automation run credential. Same-user replacement of handoff files remains possible; verification of the actual manifest bytes and the package handle protects the release trust chain. Do not describe that as general protection against a malicious local administrator or as proof every local path is immutable.

## Process and installation lifetime

The updater is staged outside the live installation with the runtime files it needs, so it does not prevent replacing itself. The staged set is the Quick runtime the updater process links, its QML import trees (the Controls module and the Basic style it uses) and the windows platform plugin; a missing entry fails staging before any update action instead of launching a half-deployed UI. The app's card enters Updater running on launch and Pending only after the marked close/handoff is accepted. A child that exits before handoff re-arms an actionable state instead of leaving a persisted pending fiction.

The bundled updater uses `QGuiApplication` and the `ExoSnap.Updater` QML module. There is no separately downloaded mandatory latest updater or bootstrap updater protocol. Portable update uses staged replacement: old installation to backup, verified new tree to live, installed-version/health checks, then approved relaunch and cleanup. Failure can restore the backup; if restoration itself fails, report the stranded/unknown state. Interrupted swaps are inspected and self-healed before another normal update proceeds.
The fresh release is extracted into a sibling `ExoSnap.new` tree. After the old process exits, same-volume directory renames move `ExoSnap` to `ExoSnap.old` and then `ExoSnap.new` to `ExoSnap`. Each rename is a metadata operation; the two-rename sequence is crash-recoverable, not a single filesystem transaction. Existing process-exit waits, instance checks, orphaned-swap repair and rollback remain required. No file-by-file overwrite or rename of a running executable is used. Backup deletion follows successful verification/relaunch.
Each directory rename retries access, sharing and lock violations with a short bounded backoff, because the exited application, its crash handler or a file scanner can hold a handle inside the tree for a moment after the process is gone.
Any other error, or a lock that outlasts the budget, fails the step with the last Windows error written to the updater's standard error.

MSI update invokes Windows Installer with the verified, locked package and the required UAC boundary. The updater itself remains `asInvoker`, not a generally elevated application. Windows Installer results and post-install verification do **not** establish the same rollback guarantee as a portable backup. `installState` can truthfully be `unknown`; the application must not promise that every MSI verification failure restored the previous version.

ExoSnap 0.10.1 installs under `C:\Program Files\ExoSnap` and publishes its product marker under `HKLM\Software\ExoSnap`. The immutable 0.10.0 updater re-reads the legacy `HKLM\Software\Codexo\ExoSnap` key after its MSI handoff and relaunches from the path it names, so an upgrade from 0.10.0 or older writes a one-time handoff into that legacy key that points at the new location. A fresh 0.10.1 install writes only the new marker; the runtime always prefers it, and accepts the legacy key only as a fallback while an old installation is still present. The handoff values are removed when the product is uninstalled.

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
