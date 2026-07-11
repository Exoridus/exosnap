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

## 3. Publish the GitHub release

- [ ] Create the `vX.Y.Z` tag and GitHub Release; upload the portable ZIP, MSI, and their `.sha256`
      sidecars.
- [ ] **Publish `update-manifest.json` AND `update-manifest.json.sig` as release assets. MANDATORY
      for every release from 0.9.0 on.** The signature is detached: the `.sig` asset holds the
      ed25519 signature over the exact bytes of `update-manifest.json`, and the in-app update
      checker only surfaces a release that carries **both** assets (the signature is verified
      against the embedded public key before any manifest field is read); a release published
      without them is **invisible to in-app updates forever**. `release-candidate.yml` runs
      `sign-manifest.yml` as a required job on the version-tag push (official builds); the job
      attaches both files to the GitHub Release for the tag automatically once the Release exists.
      Verify both assets are on the release page; if the job ran before the Release existed,
      re-run `sign-manifest.yml` via workflow_dispatch with the final URLs + SHAs.

## 4. Updater RC live-check (manual, on real hardware)

CI runs on GPU-less runners and cannot exercise a real swap. Before announcing an RC, verify these by
hand from the previous shipped version (currently 0.8.1) to the RC build:

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

## 5. 0.9 release gate — manual live verifications

0.9 is **not** tagged or released until these manual checks pass, on real hardware, in addition to
the automated gates and the updater RC live-check above:

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

## 6. Downstream package managers

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
- [ ] **Scoop.** `packaging/scoop/exosnap.json` carries an `autoupdate`/`checkver` block, so the
      *published* bucket entry (`Exoridus/scoop-exosnap`) refreshes its own version/URL/hash once
      `scoop update` runs against the new GitHub Release — no manual bucket edit needed. Still keep
      this in-repo template's `version`, `architecture.64bit.url`, and `hash` current so a
      first-time copy into the bucket (`packaging/scoop/README.md`) starts from the right values.

---

## Known limitations to address before MSI-heavy adoption (0.9.0)

Two MSI-path copy/behavior decisions ship as known limitations in 0.9.0. They are acceptable for the
portable-first 0.9.0 audience but should be fixed before MSI adoption grows:

1. **MSI verify-failure reuses the portable "previous version was restored" copy — untruthful for
   MSI.** The post-install verification-failure card (case B3 / red) says the previous version was
   restored. That is accurate for the portable staged-rename path (which really does restore its
   backup), but the MSI path has no portable-style backup to restore — `msiexec` manages its own
   rollback. The copy should be MSI-specific ("Windows Installer rolled back to the previous version")
   rather than reusing the portable string.
2. **`msiexec` exit 3010 maps to a C2 failure card — should be a "restart Windows to finish"
   terminal.** Exit code 3010 (`ERROR_SUCCESS_REBOOT_REQUIRED`) means the upgrade succeeded but needs
   a reboot to complete; today it is rendered as a generic C2 failure. It should get its own terminal
   state that tells the user the update installed and Windows needs a restart to finish — not an error.
