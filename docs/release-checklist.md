# ExoSnap release checklist

This document owns release acceptance and publication procedure. [Release verification](dev/release-verify.md) explains the runner. [Verification boundaries](architecture/verification-boundaries.md) explains what the evidence proves. A successful build is necessary, not sufficient, for release.

Creating/pushing a version tag, publishing a release or submitting a package requires explicit maintainer authorization for that operation. Preparing or verifying a candidate does not authorize publication. Published versions and their bytes are immutable.

## 1. Prepare the source

- Start from a reviewed, integrated commit. Confirm repository protection against the intended [rulesets](../.github/rulesets/README.md); local hooks are not server-side protection.
- Run `pwsh scripts/bump-version.ps1 -Version <x.y.z>` on a clean tree when changing the base version. It updates the coordinated version surfaces and resets release-dependent package hashes/identifiers to placeholders. Review the diff and refresh the versioned portable/support documentation.
- Run the full gate: `pwsh scripts/verify.ps1 -Full`. Check the actual test receipt and every required CI result. A missing tool, stale binary, skipped configuration or absent crash-capture build is not equivalent to running it successfully.
- Review current product behavior, limitations and privacy disclosures. Run the documentation check and privacy validators. Leave development narrative in the pull request.
- At the release cut only, preview `pwsh scripts/new-changelog.ps1`. Set `EXOSNAP_CHANGELOG_CUT=1` and use `-Version <x.y.z> -Apply` to perform the deliberate cut. Preview `pwsh scripts/render-release-notes.ps1 -Version <x.y.z>`. Ordinary feature branches do not edit the changelog.

## 2. Build and audit a candidate

Run `Build release candidate` from `next` with the full source commit and a unique candidate ID. The workflow verifies that `main` is an ancestor and the commit is on `next`, compiles the final `X.Y.Z` identity once with the official update key, and uses `exo-verify package` to build MSI and portable ZIP from one CMake install tree. It uploads `candidate-bundle/` and `candidate-plan.json` as a private Actions artifact. A separate job signs disposable update-feed metadata without exposing the signing key to the candidate build job.

Review the package report and bundle, not just whether archives were produced. The gate covers:

| Check | Required evidence |
|---|---|
| Identity | Final version equals the executable ProductVersion strings, package names and CMake base version; bundle binds the source commit and every file hash |
| Install tree | Required executables, Qt/QML/plugins, FFmpeg, legal files and licenses present; development files, probes, secrets and unintended dependencies absent |
| Runtime imports | Required imports resolved against packaged files or the explicit Windows-system classification; never satisfy a missing package DLL from a developer PATH |
| MSI | Harvested contents match the validated staging tree, installer metadata is correct, smoke succeeds |
| Portable | Whole folder launches in the packaged layout, without source/build-tree dependencies |
| Updater | Separate updater and required runtime are packaged and load correctly |
| Candidate record | `bundle.json` inventories exact package bytes, a source commit, candidate ID and toolchain inputs; `candidate-plan.json` freezes the source registry |

Qt deployment must include discovered QML imports as well as linked DLLs. The package inventory, not a hand-maintained generic Qt list, identifies exact deployed files. ExoSnap's own code remains C++20; resolve build-tool requirements against the pinned Qt/toolchain configuration.

## 3. Verify the frozen candidate

Download the candidate bundle and plan from the same Actions run. Use the candidate's own `exo-verify.exe` against its official package bytes. The bundle hash binds each release lane's result to the exact MSI, ZIP and runtime. The plan fixes every required scenario, revision and lane from that source commit. Run the CI core, disposable install and update, GPU and hardware lanes where their declared capabilities exist. [Release verification](dev/release-verify.md) gives the commands and guest preparation.

For a local result set, rederive the report and check it against the original bundle and source registry:

```powershell
exo-verify bundle verify <candidate-bundle>
exo-verify plan verify --bundle <candidate-bundle> --plan <candidate-plan.json>
exo-verify report merge --bundle <candidate-bundle> --plan <candidate-plan.json> `
    --results <result-directory> --out <report-directory>
exo-verify status --bundle <candidate-bundle> --plan <candidate-plan.json> `
    --results <result-directory> --report <report-directory>/release-report.json
```

An explicit `exo-verify accept` decision can cover an unavailable or failed required scenario, with its reason and author visible in the report. `ACCEPTED_RISK` applies only to an observed product failure. A decision never changes the recorded verdict. Review lane evidence and environment restoration before acting on `READY FOR APPROVAL`; that phrase is a readiness calculation, not release permission.

If source or package bytes change, build another candidate and rerun the affected acceptance. Never transplant a result to a different bundle hash.

## 4. Publication boundary

There is currently no enabled publish workflow. The candidate workflow creates no version tag and publishes no GitHub Release. Until the publish path verifies the frozen report and reuses the candidate's exact MSI and ZIP bytes behind the `release` environment, do not push a release tag or submit a package-manager version.

At publication, independently re-download and hash every public asset, verify the signed production update manifest against the embedded public key, and confirm that the tag names the exact source commit and final version already compiled into the candidate. State the actual Authenticode status; an Ed25519 update signature does not remove SmartScreen warnings.

## 5. Update and installation acceptance

Use a disposable install tree or OS. Never use the maintainer's working installation as scratch state. Test against a pinned intended candidate, not whichever newer release happens to appear during a run.

| Path | Acceptance |
|---|---|
| Portable natural update | Newer candidate is offered by the signed disposable feed; signature/hash checked; app closes; staged swap verifies and relaunches; installed bytes match intended artifact; backup/temp cleanup completes |
| MSI update | Correct installed context, explicit UAC, correct version/product identity and retained user settings; independently inspect installer result |
| Declined elevation | Installation remains intact; retryable refusal, not a successful update |
| Download failure/cancel | Installed version untouched; failure and intentional cancellation remain distinct; retries are offered only where they can change the outcome |
| Critical close guard | Installing, verifying and launching cannot be interrupted through ordinary window-close routes |
| Restore failure | Failure status names whether the portable tree is intact, restored, stranded or unknown; no blanket MSI rollback guarantee |
| Managed install | Scoop never uses the swap updater |
| Clean installation | No dependency on a development Qt/FFmpeg/PATH; first launch uses defaults; required runtime prerequisite is satisfied by the chosen distribution |

### 5a. Verification reinstall and channel guards

Launch the official candidate with `--verify-update-reinstall` where this separate reinstall behavior is in scope. Confirm an exact same-full-version reinstall is visibly labeled, goes through production manifest/signature/hash and installation checks, and installs the candidate bytes. The flag is not persisted. Without it the identical version is not offered. It never allows a downgrade or relaxes cryptographic checks.

Check recording/preparation/finalization guards, channel-switch invalidation and an in-flight old-channel result being discarded. Check app-handoff identity against the updater's transaction/target. A manual updater starts at rest and requires separate check/download/install actions. Test natural candidate-to-newer-candidate or final discovery separately when such a release exists.

## 6. Privacy acceptance

Complete [Privacy review](privacy-review.md): source allowlist/egress checks, linked crash-hook tests where applicable, explicit update request inspection, one-shot/remembered consent, structured event inspection, separate native minidump inspection and local support-bundle review. Verify service-side policy settings through the service account. A GPU-free CI result or synthetic crash test does not prove actual delivery contents.

## 7. Recording and UI acceptance

Use `exo-verify list` to inspect the source registry and lane requirements. Source/tests prove deterministic contracts; real files and hardware establish the remaining boundaries.

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

Publish downstream only after the approved release is available. Follow [WinGet](../packaging/winget/README.md), [Chocolatey](../packaging/chocolatey/README.md) and [Scoop](../packaging/scoop/README.md). The [publication policy](../packaging/publication-policy.json) records each channel's intended hold/publish state; `scripts/check-feed-drift.ps1` is an advisory comparison, not an upload command.

Fill release hashes from the published bytes/sidecars. Read each new MSI ProductCode from that MSI; never reuse the previous build's generated code. Preserve the permanent UpgradeCode. Run each full package validator, including manifest/hash checks where required. Version-placeholder checks alone do not authorize submission.

## 9. Closeout

Retain the bundle, frozen plan, lane results, explicit decisions, verified report, toolchain facts and package publication results with the release records. Do not copy the campaign transcript into durable docs. Update only actual changed behavior, boundaries and procedures. Recheck server-side protection against the tracked intended rulesets; source files cannot prove the server currently enforces them.
