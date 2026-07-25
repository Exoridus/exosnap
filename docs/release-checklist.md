# ExoSnap Release Checklist

The canonical steps to cut and publish an official ExoSnap release. The packaging gate itself is
automated (`scripts/build-release-artifacts.ps1` + `.github/workflows/release-candidate.yml`, ADR
0037); this document covers the human-gated steps around it and the live checks that CI cannot run on
a GPU-less runner.

## 1. Pre-cut

- [ ] Bump `project(exosnap VERSION x.y.z)` in the root `CMakeLists.txt` (single source of truth).
- [ ] Refresh `KNOWN_LIMITATIONS.md` to the new version and fold in any newly-shipped boundaries
      (the release script asserts the doc names the canonical version).
- [ ] Full test suite green: `pwsh scripts/run-tests.ps1`.

## 2. Build + validate artifacts (packaging gate)

- [ ] Run `pwsh scripts/build-release-artifacts.ps1` (or let `release-candidate.yml` run it on the
      version-tag push). It must exit 0. The gate includes: install-tree presence/absence/leak/metadata
      audit, dumpbin runtime-dependency audit, MSI harvest + build + content assertion, MSI smoke,
      portable ZIP smoke, and the **updater smoke** (staged `exosnap-updater.exe` load check).
- [ ] Confirm both the portable ZIP and the MSI contain `exosnap-updater.exe` (validation report
      required-files section) — without it the in-app updater is non-functional in packaged builds.

## 3. Cut a release candidate (prerelease)

The live checks in §5, §6 and §7 have to run against **real official artifacts** — in particular the
updater round-trip in §7 needs a genuinely published GitHub Release to download from. Publishing the
final `vX.Y.Z` tag first and testing afterwards is not an option: that tag is the release. So cut an
RC first. It is built by the same pipeline, from the same commit, with the same signing key and the
same gates as the final release; it differs only in which tag it lands on and in being marked as a
GitHub **prerelease**, which the in-app update check reads from GitHub's own `prerelease` flag — so
only users on the **Preview** channel are ever offered it, and Stable users are unaffected.

- [ ] **Push a release-candidate tag yourself**, e.g. `v0.9.0-rc1` (`vX.Y.Z-<suffix>`, `X.Y.Z`
      matching `CMakeLists.txt` — both are validated in seconds, before the build starts). This
      workflow cannot create the tag itself: the repository's "Block version tags" ruleset
      (`refs/tags/v*`) blocks tag creation/update/deletion for the GitHub Actions token, and only
      bypasses for a human pushing over their own credentials. Pushing the tag is what triggers the
      pipeline — same as the final tag in §4, just with an RC suffix.
- [ ] **Let the pipeline run and confirm it went green.** It performs exactly the steps listed in §4
      — fail-closed official build, signed manifest, draft Release, upload, re-download + re-hash +
      signature re-verification — and then publishes the Release **as a prerelease**. A failure
      leaves a hidden draft, same as for a final release.
- [ ] **Confirm the RC page shows the "Pre-release" badge** and carries `update-manifest.json` +
      `update-manifest.json.sig` next to the ZIP + MSI.
- [ ] **Run §5, §6 and §7 against this RC build.** Set the test install's update channel to
      **Preview** (Settings → updates card, Stable/Preview toggle) before the updater checks — a
      Stable-channel client will not see a prerelease at all.
- [ ] **If anything fails:** fix it, and cut the next candidate (`v0.9.0-rc2`) from the new commit.
      A published RC is never re-used or overwritten; the pipeline refuses to re-upload into an
      already-published Release.
- [ ] **Once every check passes, push the final `vX.Y.Z` tag from the *same commit* the passing RC
      was built from** and continue with §4. Re-cut an RC if that commit moved.

> The RC prerelease stays on the releases page as a normal, visible prerelease. Note that an
> RC and its eventual final release compare as the *same* version number (`0.9.0-rc1` → `0.9.0`),
> so a machine left on the RC build will not be offered the final release as an in-app update —
> install the final build there by hand (or delete the RC prerelease once the release is out).

## 4. Publish the GitHub release

For an **official** version tag (`vX.Y.Z` with the `EXOSNAP_UPDATE_PUBLIC_KEY_HEX` repository
variable and the `EXOSNAP_UPDATE_SIGNING_KEY` secret provisioned), `release-candidate.yml` now owns
this deterministically — there is no manual asset upload and no `sign-manifest.yml` re-run dance:

- [ ] **Push the `vX.Y.Z` tag** (this is the *only* manual step, and it triggers everything below),
      from the same commit as the RC that passed §3. Do **not** hand-create the GitHub Release first
      — the workflow creates it. The build job hard-fails if the update key is missing on a `v*`
      tag, so a version tag can never produce an unofficial artifact.
- [ ] **Let the pipeline run and confirm it went green.** On the tag push the workflow, in order:
  1. builds + validates the portable ZIP, MSI, and their `.sha256` sidecars (packaging gate);
  2. generates `update-manifest.json`, signs it (detached ed25519 `.sig`), and **verifies in CI**
     that the signing key is the private half of the embedded public key and that the signature
     verifies;
  3. creates a **draft** GitHub Release for the tag;
  4. uploads the ZIP, MSI, `.sha256` sidecars, `update-manifest.json`, and `update-manifest.json.sig`;
  5. re-downloads those assets and re-hashes them, cross-checks the manifest's embedded SHA-256s
     against the shipped bytes, and re-verifies the signature against the embedded public key;
  6. only then **un-drafts (publishes)** the Release.
      If any step fails the Release stays a hidden draft, so it is never visible to users or the
      in-app updater in a half-published state.
- [ ] **Spot-check the published release page**: both `update-manifest.json` AND
      `update-manifest.json.sig` are present alongside the ZIP + MSI. The detached `.sig` holds the
      ed25519 signature over the exact bytes of `update-manifest.json`; the in-app update checker
      only surfaces a release that carries **both** assets (signature verified against the embedded
      public key before any manifest field is read), so a release missing either is **invisible to
      in-app updates forever**. **MANDATORY for every release from 0.9.0 on.**
- [ ] (Optional) Edit the release notes on GitHub after publication.

> **Manual escape hatch.** `sign-manifest.yml` still exposes a standalone `workflow_dispatch` that
> attaches a freshly signed manifest to an **already-existing** Release (supply the final URLs +
> SHAs). It is only needed if the automated `publish-release` job is unavailable (e.g. re-signing an
> old release); the normal path above requires no re-run.

## 5. Updater RC live-check (manual, on real hardware)

CI runs on GPU-less runners and cannot exercise a real swap. Run these against the RC prerelease from
§3, by hand, from the previous shipped version (currently 0.8.1) to the RC build — the test install
must be on the **Preview** update channel to see the RC at all:

- [ ] **Portable happy-path swap (0.8.1 → RC).** From a user-writable portable install, click Update;
      the dedicated updater downloads, verifies, closes the app, does the staged-rename swap, verifies,
      and relaunches on the RC. No UAC. Backup is discarded on the healthy start.
- [ ] **MSI happy-path via UAC.** From the MSI-installed build, click Update; accept the single UAC
      prompt; `msiexec /qn` applies the upgrade and the app relaunches on the RC.
- [ ] **UAC-decline (case C1).** MSI path, decline the UAC prompt: the current version stays intact
      and the failure card is amber/retryable, naming the current version as safe to run.
- [ ] **Unplugged-network download failure (case A1).** Disconnect the network mid-download: the
      updater surfaces a download failure (amber), the current version is untouched, and Retry resumes
      cleanly once the network is back.
- [ ] **Mid-swap close refusal (0.9.0+).** While the updater is in its Install/Verify/Launch phase,
      try to close the updater window (Alt+F4, taskbar "Close window"): the window refuses to close so
      the in-place staged rename cannot be torn apart mid-swap. The disabled close button alone was
      insufficient before 0.9.0 (a raw `WM_CLOSE` still quit the app).
- [ ] **Temp-download cleanup after a successful update.** After a healthy update completes, confirm
      `%TEMP%\ExoSnapUpdate\<version>\` is gone — the downloaded manifest, `.sig`, and ZIP/MSI are
      removed on the success path (they used to accumulate one full copy per version).

## 6. Privacy review (every release)

ExoSnap promises a telemetry-free product; this is the repeatable step that keeps that promise
provable rather than assumed. Full detail and rationale: `docs/privacy-review.md` (inventory +
checklist) and ADR 0045. The **[CI]** items below are already enforced on every PR (`lint` job) —
this step is a reminder they must be green for the commit being released, not a re-run. The
**[Live]** items are not automatable (no GPU/Official-build/Sentry DSN on CI runners) and must be
walked by hand.

- [ ] **[CI]** `scripts/validate-privacy-allowlist.ps1` green — the crash-report tag allowlist
      matches `PRIVACY.md` and `docs/product-spec.md` §14.
- [ ] **[CI]** `scripts/validate-network-egress.ps1` green — no network call site outside the
      known GitHub/Sentry allowlist.
- [ ] **[CI]** Crash-scrubber tests green (Golden-Set + `IsAllowedTagKey`/`ScrubString`) and, if
      this release touches the crash-capture strand, the sentry-linked suite in
      `crash-capture-build.yml` (push-to-main / `crash-capture` label / dispatch).
- [ ] **[Live]** **Sentry reality check.** On a real Official build, give consent and trigger a
      test event; in the Sentry EU UI confirm no hostname/path/username and exactly the
      allowlisted tags + stack arrived. Couple this to any release that touches the crash-capture
      or Official-build path.
- [ ] **[Live]** **Minidump module-path check.** Provoke a real hard crash with consent active;
      inspect the uploaded minidump's module list for a username segment in the `exosnap.exe`
      path (relevant for portable/non-standard installs). This is the only real check of the
      minidump binary channel (a test event does not produce one).
- [ ] **[Live]** **Update-check network trace.** A proxy capture of a real update check shows only
      the expected `GET api.github.com/.../releases` — no user data in the query.
- [ ] **[Manual/Doc]** `PRIVACY.md`'s `Effective date` and `docs/product-spec.md` §14 have been
      walked against `docs/privacy-review.md`'s inventory for this release; bump `Effective date`
      if any field or recipient changed.

## 7. 0.9 release gate — manual live verifications

0.9 is **not** tagged or released until these manual checks pass, on real hardware, against the RC
prerelease from §3, in addition to the automated gates and the updater RC live-check above:

- [ ] **Window-capture recording with the `APP` audio row.** Record a specific application window
      with the `APP` row enabled; play the result back and confirm per-app audio is present and
      audibly correct.
- [ ] **System-audio recording on a real 44.1 kHz output device.** Set a physical playback device to
      44.1 kHz, record with `SYS` enabled, and confirm audio is present in the output file.
- [ ] **Updater round-trip on the RC build.** With the signed manifest and its detached `.sig`
      published alongside the RC release, confirm the in-app update check finds the release,
      verifies it, and installs it end to end.
- [ ] **Edit overlay walkthrough.** Open a completed recording in the Edit overlay and click through
      it once: the trim is applied only on Save (not while dragging the handles), scrubbing pauses
      playback for the drag and resumes only if it was playing before, the playhead follows
      playback, and a marker JSON sidecar is written only when at least one marker survives the
      trim.
- [ ] **Edit overlay real decoded playback.** In the same walkthrough, confirm the player actually
      shows decoded video (not a placeholder) and that audio is present and stays in sync with the
      picture through a play/pause/scrub cycle, for a clip that has an audio track. Also open a
      clip recorded with the Expert 4:4:4 chroma option and confirm it shows the documented
      "Preview unavailable" placeholder instead of a decoded frame (trim/markers/export still work).
      Not automatable (real audio-clock pacing and a real decoder, no mock seam).
- [ ] **Audio-endpoint loss mid-recording degrades to silence and keeps recording (ADR 0046).**
      During a `SYS`-row recording, remove or switch the playback endpoint device: the recording
      does **not** stop. The affected source falls to honest silence, the engine reactivates the
      same source identity every 500 ms, and a standing notification plus Diagnostics/post-flight
      report surface the degraded state until the source returns (or the recording ends). Other
      sources keep recording normally; in a merged track only the dead source's contribution goes
      silent. Confirm this end-to-end on real hardware — unit/integration tests already cover the
      logic with fake sources, but the real endpoint-unplug path has no test-harness device seam.
- [ ] **Present-mode diagnostics are per-recording, not per-session.** After some normal desktop use
      (window switches, notifications), record one demonstrably stable window, stop, then record a
      second stable window. Neither recording may surface a false "captured source keeps changing
      present mode" notice — the warning must reflect only the current recording, not accumulated
      session history.
- [ ] **Trim keeps the keyframe at or before the cut.** In the Edit overlay, drag the start handle
      into the middle of a multi-GOP clip and Save; the exported file starts cleanly (no black or
      frozen lead-in, no overshoot past the end), with duration matching the trimmed range.

## 8. Downstream package managers

WinGet and Chocolatey each pin an exact version, download URL, and SHA-256 for the release inside
tracked files; both are easy to forget because nothing fails locally if they go stale. Update them
by hand, every release:

- [ ] **WinGet.** `packaging/winget/manifests/c/Codexo/ExoSnap/` must contain exactly one version
      directory (`scripts/validate-winget-manifest.ps1` enforces this) — `git mv` the existing
      `<old-version>/` directory to `<new-version>/` rather than adding a second one, then update:
  - `Codexo.ExoSnap.yaml` — `PackageVersion`.
  - `Codexo.ExoSnap.installer.yaml` — `PackageVersion`, `ReleaseDate`, `InstallerUrl` (the version
    segment of the path), `InstallerSha256` (uppercase, from the published `.msi.sha256`
    sidecar), and `ProductCode` in both the top-level installer entry and
    `AppsAndFeaturesEntries` — WiX auto-generates a **new** `ProductCode` on every MSI build (see
    `packaging/msi/Package.wxs`), so this must be read out of the freshly built MSI, never copied
    from the previous release. `UpgradeCode` is permanent (ADR 0034) and must **not** change.
  - `Codexo.ExoSnap.locale.en-US.yaml` — `PackageVersion`, `ReleaseNotesUrl`, and the version
    number/release-specific text inside `Description` if it names one.
  - Run `scripts/validate-winget-manifest.ps1` before submitting per `packaging/winget/README.md`.
- [ ] **Chocolatey.** `packaging/chocolatey/tools/chocolateyinstall.ps1` — `url64bit` (version in
      the path) and `checksum64` (lowercase SHA-256, from the same `.msi.sha256`). Also
      `packaging/chocolatey/exosnap.nuspec` — `<version>`, the `@vX.Y.Z` tag in `<iconUrl>`,
      `<releaseNotes>`, and any version-specific line in `<description>`.
  - Run `scripts/validate-chocolatey-package.ps1 -Version <x.y.z> -ManifestPath
    .workspace/release/<x.y.z>/artifact-manifest.json -RequireManifest` before publishing —
    it proves the CMake version, nuspec `<version>`/`<iconUrl>`/`<releaseNotes>`, and
    `chocolateyinstall.ps1` `url64bit` all agree, flags any other stale version reference left
    in `packaging/chocolatey/`, and checks `checksum64` against the manifest's `msiSha256`.
    **`-RequireManifest` is mandatory for a real submission** — without it, a missing manifest
    silently skips the checksum check instead of failing, which would let an unverified
    `checksum64` through. Static checks only — it does not run `choco pack` or install/
    uninstall the package.
- [ ] **Scoop.** `packaging/scoop/exosnap.json` carries an `autoupdate`/`checkver` block, so the
      *published* bucket entry (`Exoridus/scoop-exosnap`) refreshes its own version/URL/hash once
      `scoop update` runs against the new GitHub Release — no manual bucket edit needed. Still keep
      this in-repo template's `version`, `architecture.64bit.url`, and `hash` current so a
      first-time copy into the bucket (`packaging/scoop/README.md`) starts from the right values.
