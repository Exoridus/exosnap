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
- [ ] **Publish `update-manifest.json` as a release asset. MANDATORY for every release from 0.9.0 on.**
      The in-app update checker only surfaces a release that carries this signed manifest (verified
      against the embedded public key before any field is read); a release published without it is
      **invisible to in-app updates forever**. `release-candidate.yml` runs `sign-manifest.yml` as a
      required job on the version-tag push (official builds) and produces the signed manifest; download
      it from that run and attach it to the release. The signed asset URLs point at the canonical
      release download paths for the tag, so publish it alongside the ZIP + MSI under the same tag.

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

## 5. Downstream package managers

- [ ] Update WinGet / Scoop manifests (Chocolatey when applicable) per the published SHAs and URLs.

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
