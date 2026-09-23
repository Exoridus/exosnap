# ExoSnap release checklist

This document owns release acceptance and publication procedure. [Release verification](dev/release-verify.md) explains the runner. [Verification boundaries](architecture/verification-boundaries.md) explains what the evidence proves. A successful build is necessary, not sufficient, for release.

Creating/pushing a version tag, publishing a release, attaching a qualification record or submitting a package requires explicit maintainer authorization for that operation. Preparing or qualifying a candidate is not authorization to publish it. Published versions and their bytes are immutable.

## 1. Prepare the source

- Start from a reviewed, integrated commit. Confirm repository protection against the intended [rulesets](../.github/rulesets/README.md); local hooks are not server-side protection.
- Run `pwsh scripts/bump-version.ps1 -Version <x.y.z>` on a clean tree when changing the base version. It updates the coordinated version surfaces and resets release-dependent package hashes/identifiers to placeholders. Review the diff and refresh the versioned portable/support documentation.
- Run the full gate: `pwsh scripts/verify.ps1 -Full`. Check the actual test receipt and every required CI result. A missing tool, stale binary, skipped configuration or absent crash-capture build is not equivalent to running it successfully.
- Review current product behavior, limitations and privacy disclosures. Run the documentation check and privacy validators. Leave development narrative in the pull request.
- At the release cut only, preview `pwsh scripts/new-changelog.ps1`. Set `EXOSNAP_CHANGELOG_CUT=1` and use `-Version <x.y.z> -Apply` to perform the deliberate cut. Preview `pwsh scripts/render-release-notes.ps1 -Version <x.y.z>`. Ordinary feature branches do not edit the changelog.

## 2. Build and audit packages

The tag workflow and `scripts/build-release-artifacts.ps1` own the packaging gate. Review its report, not just whether an archive was produced. It must cover:

| Check | Required evidence |
|---|---|
| Identity | Full tag-derived version equals the executable ProductVersion strings and package/manifest identity; base version matches CMake |
| Install tree | Required executables, Qt/QML/plugins, FFmpeg, legal files and licenses present; development files, probes, secrets and unintended dependencies absent |
| Runtime imports | Required imports resolved against packaged files or the explicit Windows-system classification; never satisfy a missing package DLL from a developer PATH |
| MSI | Harvested contents match the validated staging tree, installer metadata is correct, smoke succeeds |
| Portable | Whole folder launches in the packaged layout, without source/build-tree dependencies |
| Updater | Separate updater and required runtime are packaged and load correctly |
| Reproducibility records | Package hashes, portable artifact inventory with executable section hashes, and toolchain manifest produced |

Qt deployment must include discovered QML imports as well as linked DLLs. The package inventory, not a hand-maintained generic Qt list, identifies exact deployed files. ExoSnap's own code remains C++20; resolve build-tool requirements against the pinned Qt/toolchain configuration.

## 3. Publish an immutable candidate

The maintainer explicitly pushes a candidate tag `vX.Y.Z-<suffix>` from the intended commit. The release workflow validates the tag/base identity, requires official update-key configuration, builds/audits the packages, signs the update manifest, creates a draft, uploads, re-downloads and verifies, then publishes as a prerelease. Failure must not expose a half-published update.

Confirm the release is a prerelease and contains the portable ZIP, MSI, SHA-256 sidecars, `update-manifest.json`, detached `.sig`, `artifact-manifest.json` and `toolchain-manifest.json`. Download the published artifacts for acceptance; local build-tree output is not a substitute.

Use Preview for candidate discovery. The baseline for a natural update must itself contain the current updater protocol and honestly embed its full version. Verify those properties of the baseline instead of naming a permanently fixed candidate in this checklist. Same-version verification reinstall tests mechanics but does not replace natural newer-version discovery.

If product code changes after acceptance, cut and qualify another immutable candidate. Do not overwrite the previous candidate's assets or transplant its PASS results onto changed bytes.

### 3a. Bind, qualify and promote

Bind the campaign to explicit published bytes and source identity:

```powershell
pwsh scripts/release-verify.ps1 prepare `
    -ExePath '<extracted candidate>\exosnap.exe' -Tag '<candidate tag>' `
    -SourceCommit '<full source commit>' `
    -PortableZip '<downloaded candidate ZIP>' -Msi '<downloaded candidate MSI>'
pwsh scripts/release-verify.ps1 list
pwsh scripts/release-verify.ps1 run
pwsh scripts/release-verify.ps1 report
pwsh scripts/release-verify.ps1 qualify
```

Select the release-relevant opt-in scenarios explicitly and include them with `-Required` when qualifying. The canonical [release policy](../scripts/lib/release-policy.json) and source catalog determine requirements; a record cannot reduce its own required set.

Qualification requires complete source/artifact/package/harness/catalog identity, exactly one passing verdict for every required gate, no recorded product failure or infrastructure error, complete evidence, and successful restoration of every mutated environment. Unknown, unavailable, deferred, stale or unattempted required work cannot become PASS. A failed requalification must not leave an old successful export usable.

Attaching a signed qualification record is a separate maintainer operation: `pwsh scripts/release-verify.ps1 qualify -RunId <id> -Publish`. Supply the signing seed through the approved environment secret mechanism, not a committed file or shell transcript. Public-key matching is checked before signing when configured. The upload includes the record's detached signature.

Only then may the maintainer push the final tag from the exact qualified commit. The publish lock verifies the record signature **before parsing**, re-derives eligibility against the source catalog/release policy, and checks candidate identity and package hashes. An unsigned or self-asserted `QUALIFIED` field is insufficient.

### Promotion comparison and limits

Candidate and final binaries are different because their full versions are compiled in. The promotion contract `exosnap.release-promotion/2` permits this rebuild while comparing the final portable tree and toolchain against the qualified candidate.

All files must match byte-for-byte except the declared executables, `exosnap.exe`, `exosnap-updater.exe` and `crashpad_handler.exe`. Changed declared executables require matching section inventories; all sections outside `.rdata` and `.rsrc` must match. The record may not widen that budget. Missing inventories, moved source, different toolchain or additional/removed files are refusals.

This does **not** prove literal byte identity of the final binaries. Constants/strings in the permitted sections can differ. MSI internal structure is not compared as a PE tree; its content is covered by its packaging assertions. The artifact/toolchain manifests themselves are not individually signed update assets. Preserve these limits instead of calling the process a byte-for-byte promotion or assuming every final executable is unconstrained.

## 4. Publish and inspect the final release

The authorized final tag triggers the workflow. Do not create a competing manual Release. The workflow validates packaging versions, official identity, signed update metadata and qualified promotion, then publishes only after the downloadable bytes pass the post-upload checks.

Verify the published manifest version, package URLs/hashes and detached signature match what clients receive. Confirm both distribution forms and their legal files. A checksum alone verifies integrity, not publisher identity. State the actual Authenticode status; an Ed25519 update signature does not remove SmartScreen warnings.

The standalone signing workflow is not a way around qualification. Attaching replacement update metadata to a published release changes what clients install and requires the same explicit authority and asset checks. Prefer a new candidate when correcting candidate artifacts.

## 5. Update and installation acceptance

Use a disposable install tree or OS. Never use the maintainer's working installation as scratch state. Test against a pinned intended candidate, not whichever newer release happens to appear during a run.

| Path | Acceptance |
|---|---|
| Portable natural update | Newer candidate is offered on Preview; signature/hash checked; app closes; staged swap verifies and relaunches; installed bytes match intended artifact; backup/temp cleanup completes |
| MSI update | Correct installed context, explicit UAC, correct version/product identity and retained user settings; independently inspect installer result |
| Declined elevation | Installation remains intact; retryable refusal, not a successful update |
| Download failure/cancel | Installed version untouched; failure and intentional cancellation remain distinct; retries are offered only where they can change the outcome |
| Critical close guard | Installing, verifying and launching cannot be interrupted through ordinary window-close routes |
| Restore failure | Failure status names whether the portable tree is intact, restored, stranded or unknown; no blanket MSI rollback guarantee |
| Managed install | Scoop never uses the swap updater |
| Clean installation | No dependency on a development Qt/FFmpeg/PATH; first launch uses defaults; required runtime prerequisite is satisfied by the chosen distribution |

### 5a. Verification reinstall and channel guards

Launch the official candidate with `--verify-update-reinstall`. Confirm an exact same-full-version reinstall is visibly labeled, goes through production manifest/signature/hash and installation checks, and installs the published bytes. The flag is not persisted. Without it the identical version is not offered. It never allows a downgrade or relaxes cryptographic checks.

Check recording/preparation/finalization guards, channel-switch invalidation and an in-flight old-channel result being discarded. Check app-handoff identity against the updater's transaction/target. A manual updater starts at rest and requires separate check/download/install actions. Test natural candidate-to-newer-candidate or final discovery separately when such a release exists.

## 6. Privacy acceptance

Complete [Privacy review](privacy-review.md): source allowlist/egress checks, linked crash-hook tests where applicable, explicit update request inspection, one-shot/remembered consent, structured event inspection, separate native minidump inspection and local support-bundle review. Verify service-side policy settings through the service account. A GPU-free CI result or synthetic crash test does not prove actual delivery contents.

## 7. Recording and UI acceptance

Use [the scenario catalog](dev/release-verify-catalog.md) to select the strongest available verifier. Source/tests prove deterministic contracts; real files and hardware establish the remaining boundaries.

| Area | Required checks for relevant release changes |
|---|---|
| Capture | Display, window and region; motion, quiet source, resize, reconnect, GPU failure handling; display/HDR transitions finalize rather than corrupt; exclusive-window and stall notices distinguish evidence from cause |
| Audio routing | APP only on a window, SYS and MIC, separate and merged tracks, live mute, 44.1 kHz endpoint with converted output; real playback and track counts |
| Audio outages | Lost endpoint versus connected silence; full and partial merged-source recovery; every track preserves the intended elapsed timeline, including reopen duration; no silent switch to an unrelated fixed device |
| Pacing | High-refresh to lower CFR coalescing is benign; ring eviction, processing and backpressure losses agree on all surfaces; VFR static start establishes a valid epoch |
| Webcam | Actual selected device/mode, negotiated rate, live mirror/opacity/chroma/PiP parity, continued movement over a still desktop, loss/reconnect and unavailable-MF behavior |
| Color | SDR range and tags, HEVC/AV1 10-bit, native HDR10/tone-map, container plus bitstream metadata, actual playback; no false HDR interpretation of SDR FP16 surfaces |
| Edit/export | Real decoded video/audio, hardware/software decode, 4:4:4 and HDR preview, seek/scrub/trim, closed-session resource release, immutable running export, destination errors and marker-sidecar lifecycle |
| Interaction | Minimum size, Dark/Light × accents, keyboard focus/activation and modal guards, native chrome, cross-monitor preview debt, notifications and capture-excluded overlays |
| Diagnostics | Correct attribution/availability/reset, optional elevated present and DPC/ISR measurement, independent present cross-check, report/ledger agreement |
| Recovery | Controlled interrupted recording/finalize/remux, original partial preservation, atomic publication, missing/empty entry filtering and manifest-write failure notification |

A scene-graph screenshot does not prove desktop composition of a capture-excluded overlay. A programmatic move does not prove the native interactive move loop. An operator confirmation alone does not prove a machine-observable postcondition. Record what each check actually reached.

### Long-duration audio/capture gate

Run a 2–3 hour default-profile monitor recording with SYS and MIC as separate tracks, slaving on, sustained audio, no sleep and stable display settings. Run a shorter 30–60 minute 44.1 kHz endpoint session as well. The [soak runbook](dev/soak-and-recovery-drills.md) explains stimulus and analysis.

Use a scheduled clapper signal with enough markers for the analyzer's reference-quality test. The acceptance budget is **20 ms fitted drift**, not merely a small start/end difference. Reference uncertainty must meet the analyzer's budget fraction and residual/nonlinearity checks. An unqualified reference is unmeasurable, not PASS; `--unqualified-reference` must not be used for acceptance. Absolute offset includes setup-dependent emission skew and is reported separately.

Inspect packet-span durations for each stream, listen at the beginning/middle/end and review session diagnostics. Audio-discontinuity duration must remain below 0.1% of recording duration, with longest gap at most 120 ms. Count alone is advisory. Require no source degradation, no mux failures, no processing/backpressure losses, no undrained resampler frames and no unexpected keyframe-prediction mismatches. Inspect ring-eviction losses as real frame loss too. Opposing segment drifts or other reliability findings cannot be hidden by cancellation at the endpoints.

## 8. Package-manager publication

Publish downstream only after the release is qualified, promoted and available. Follow [WinGet](../packaging/winget/README.md), [Chocolatey](../packaging/chocolatey/README.md) and [Scoop](../packaging/scoop/README.md). The [publication policy](../packaging/publication-policy.json) records each channel's intended hold/publish state; `scripts/check-feed-drift.ps1` is an advisory comparison, not an upload command.

Fill release hashes from the published bytes/sidecars. Read each new MSI ProductCode from that MSI; never reuse the previous build's generated code. Preserve the permanent UpgradeCode. Run each full package validator, including manifest/hash checks where required. Version-placeholder checks alone do not authorize submission.

## 9. Closeout

Retain signed qualification, artifact/toolchain identities, scenario evidence and package publication results with the release records. Do not copy the campaign transcript into durable docs. Update only actual changed behavior, boundaries and procedures. Recheck server-side protection against the tracked intended rulesets; source files cannot prove the server currently enforces them.
