# ExoSnap Release Checklist

The canonical steps to cut and publish an official ExoSnap release. The packaging gate itself is
automated (`scripts/build-release-artifacts.ps1` + `.github/workflows/release-candidate.yml`, ADR
0037); this document covers the human-gated steps around it and the live checks that CI cannot run on
a GPU-less runner.

> **Running the live checks.** `scripts/live-verify.ps1` (ADR 0066, usage in `docs/dev/live-verify.md`)
> executes the automatable part of §5–§7 against a prepared artifact, records evidence per check,
> binds every PASS to the exact binary and environment it was produced against, and survives an
> interruption without losing verified progress. It stops only for the bounded human gates that
> remain. This document stays the authority on *what* must be true; the runner records *whether it
> was proven and against which bytes*. Start with
> `pwsh scripts/live-verify.ps1 prepare` → `run` → `report`, and check the generated `report.md`
> into the RC's evidence rather than ticking boxes here from memory.

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
same gates as the final release. It differs in which tag it lands on, in being marked as a GitHub
**prerelease** (which the in-app update check reads from GitHub's own `prerelease` flag — so only
users on the **Preview** channel are ever offered it, and Stable users are unaffected), **and in
the embedded release version**: the full version (`0.9.0-rc4` vs `0.9.0`) is derived from the tag
and compiled into the binaries, so the RC and the final are **separate builds, not the same bytes**
— retagging RC artifacts as final is impossible (the pipeline's embedded-version gate would refuse
a binary whose `ProductVersion` does not match the tag).

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
      `update-manifest.json.sig` + `toolchain-manifest.json` next to the ZIP + MSI.
- [ ] **Run §5, §6 and §7 against this RC build.** Set the test install's update channel to
      **Preview** (Settings → Updates card → "Update channel" dropdown) before the updater checks —
      a Stable-channel client will not see a prerelease at all.
- [ ] **In-app RC→RC update offer.** Natural RC → RC discovery requires the **running** build to
      embed its full RC version — that is true from `v0.9.0-rc4` on (see the RC2/RC3 defect note
      below; rc2/rc3 misidentify as `0.9.0` and can never be offered a later `0.9.0`-family tag).
      Use the **newest already-published RC** (rc4 or later) as the baseline; today that is
      `v0.9.0-rc10`. Where no newer build exists yet to be offered, prove the full
      app→updater→install→relaunch path against the published artifacts of the RC under test with
      the **verification reinstall mode** (`--verify-update-reinstall`, §7a) instead, and leave the
      natural-discovery line open until the next candidate or the final tag publishes.
- [ ] **If anything fails:** fix it, and cut the next candidate (`rc_N+1`) from the new commit.
      A published RC is never re-used or overwritten; the pipeline refuses to re-upload into an
      already-published Release.
- [ ] **Once every check passes, push the final `vX.Y.Z` tag from the *same commit* the passing RC
      was built from** and continue with §4. Re-cut an RC if that commit moved.

> The RC prerelease stays on the releases page as a normal, visible prerelease. As of the
> SemVer prerelease-ordering fix (first shipped in `v0.9.0-rc2`), a running RC correctly detects a
> later RC or the eventual final release of the same `X.Y.Z` as a newer, available update (`rc1 <
> rc2 < ... < the final X.Y.Z`) — **but only if the running build itself already contains this fix**.
> `v0.9.0-rc1` predates it and still compares every `0.9.0`-family tag as equal, so a machine left on
> rc1 will not be offered rc2/rc3/the final release in-app; install by hand there (or delete the rc1
> prerelease once a newer one is out).
>
> **Known RC2/RC3 version-identity defect (fixed in rc4).** Natural RC2 → RC3 discovery failed
> because RC2 embedded the final base version `0.9.0` instead of `0.9.0-rc2` (`project(VERSION)`
> was the only version source and cannot carry a prerelease suffix), so SemVer correctly judged
> `0.9.0-rc3` as *older* than the claimed `0.9.0`. The official updater mechanics were validated
> separately using the released updater with a controlled lower `--current-version` argument:
> download, signature verification, package hash verification, portable swap, relaunch and cleanup
> all completed successfully. That controlled test does **not** count as a natural discovery test.
> RC2 and RC3 installs will never be offered rc4 or the final in-app (they believe they already run
> `0.9.0`); install rc4 by hand there. From rc4 on, the full release version is embedded
> (`EXOSNAP_RELEASE_VERSION`, ADR 0054) and RC → RC discovery works naturally.
>
> **§5's live checks need the *previous shipped version* to already contain the in-app swap-updater
> client** (the code that does the actual download/verify/staged-rename — not just the version
> *check*). `v0.8.1` does not: the swap updater (`26f7760`) landed six days after the `v0.8.1` tag,
> so its "Update to X" button unconditionally opens a browser tab and can never exercise §5's UAC/
> network-fail/close-refusal/temp-cleanup mechanics, no matter which RC it's pointed at. Before
> relying on "the previous shipped version" for §5, confirm it actually ships `exosnap-updater.exe`
> (portable ZIP) — if not, cut two RCs from the current cycle instead (rc_N as the swap-capable
> baseline, rc_N+1 as the target) to get a real swap test.

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
  4. uploads the ZIP, MSI, `.sha256` sidecars, `update-manifest.json`, `update-manifest.json.sig`,
     and `toolchain-manifest.json` (an informational record of the exact runner image, MSVC, CMake,
     Qt, WiX, and pinned FFmpeg prebuilt that produced the build — not signed, not part of the
     integrity gate);
  5. re-downloads the ZIP, MSI and manifest and re-hashes them, cross-checks the manifest's embedded
     SHA-256s against the shipped bytes, and re-verifies the signature against the embedded public key
     (`toolchain-manifest.json` is uploaded alongside but is informational only and is not part of this
     re-hash/signature check);
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
§3, by hand, from the previous shipped version to the RC build — but the last shipped version (0.8.1)
predates the swap updater (see the §3 note), so the baseline is the **newest already-published RC**
instead.

**Which baseline is valid.** The baseline must embed its own full RC version, which is true from
`v0.9.0-rc4` on (ADR 0054). rc1–rc3 embed the bare `0.9.0` and can never be offered a later
`0.9.0`-family build, so they cannot serve as the baseline for any check below that begins with an
update offer — the offer never appears. At the time of writing the newest published RC is
`v0.9.0-rc10`; substitute whatever the newest published RC actually is when running this. The test
install must be on the **Preview** update channel to see the RC at all:

- [ ] **Portable happy-path swap (newest published RC → the RC under test).** From a user-writable
      portable install of the baseline RC on the Preview channel, confirm the new RC is offered, then
      click Update; the dedicated updater downloads, verifies signature + hash, closes the app, does
      the staged-rename swap, verifies, and relaunches on the RC. No UAC. Backup is discarded on the
      healthy start. (`v0.8.1` does not ship the swap updater, so an 0.8.1 → RC run proves nothing
      about the swap path — a manual install-over comparison from 0.8.1 may still be done separately,
      but it is not this check.)
      - If this RC is the first candidate of the cycle and no newer build exists to be offered, the
        mechanics — download, signature, hash, staged swap, relaunch, cleanup — are proven with the
        **verification reinstall mode** (`--verify-update-reinstall`, §7a) instead, and this line
        stays open until a second RC or the final tag makes a natural offer possible.
- [ ] **MSI happy-path via UAC (newest published RC → the RC under test).** From the baseline RC's
      MSI-installed build on Preview, click Update; accept the **single** UAC prompt; `msiexec /qn`
      applies the upgrade and the app relaunches on the new RC.
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
prerelease from §3, in addition to the automated gates and the updater RC live-check above.

> **Since Wave D most of this section is driven by a runner rather than performed by hand.**
> `pwsh scripts/release-verify.ps1` (see `docs/dev/release-verify.md`) prepares the environment,
> drives ExoSnap through its own semantic automation, validates the output with ffprobe, and asks
> for a person only at a real physical, secure or visual boundary. Items below carry a
> `→ REL-…` reference to the scenario that covers them; run the scenario rather than repeating the
> steps by hand. Where the runner still needs a human it says so, prints exactly what to do, and
> then verifies the consequence itself — an operator answering "done" is never recorded as a pass.
>
> The long-term intent for this section is that only genuinely irreducible gates remain: UAC,
> physical unplug/replug, desktop composition a person has to look at, and hardware this machine
> does not have.

- [ ] **Window-capture recording with the `APP` audio row.** Record a specific application window
      with the `APP` row enabled; play the result back and confirm per-app audio is present and
      audibly correct.
- [ ] **System-audio recording on a real 44.1 kHz output device.** — `→ REL-AUD-FORMAT-001`
      (the runner verifies the endpoint really reads 44100 before it records, and ffprobes the result) Set a physical playback device to
      44.1 kHz, record with `SYS` enabled, and confirm audio is present in the output file.
- [ ] **Updater round-trip on the RC build.** — `→ REL-UPD-PORTABLE-001` (portable) and
      `→ REL-UPD-MSI-001` / `REL-UPD-MSI-DECLINE-001` (MSI; UAC stays yours) With the signed manifest and its detached `.sig`
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
      clip recorded with the Expert 4:4:4 chroma option and confirm it now plays decoded video too
      (it used to show a "Preview unavailable" placeholder). 4:4:4 has no hardware decoder on any
      vendor, so this clip is the software path's worst case — watch specifically for audio holes
      or stutter, not just for a picture appearing.
      Not automatable (real audio-clock pacing and a real decoder, no mock seam).
- [ ] **Audio-endpoint loss mid-recording degrades to silence and keeps recording (ADR 0046).** —
      `→ REL-AUD-DEGRADE-001` (you unplug; the runner asserts the degradation, the continued
      recording and the recovery) and `→ REL-AUD-SILENCE-001` (silence is not degradation)
      During a `SYS`-row recording, remove or switch the playback endpoint device: the recording
      does **not** stop. The affected source falls to honest silence, the engine reactivates the
      same source identity every 500 ms, and a standing notification plus Diagnostics/post-flight
      report surface the degraded state until the source returns (or the recording ends). Other
      sources keep recording normally; in a merged track only the dead source's contribution goes
      silent. Confirm this end-to-end on real hardware — unit/integration tests already cover the
      logic with fake sources, but the real endpoint-unplug path has no test-harness device seam.
- [ ] **Present-mode diagnostics are per-recording, not per-session.** — `→ REL-PRESENT-001`
      (unelevated posture) and `→ REL-PRESENT-002` (elevated, real presents; needs your UAC) After some normal desktop use
      (window switches, notifications), record one demonstrably stable window, stop, then record a
      second stable window. Neither recording may surface a false "captured source keeps changing
      present mode" notice — the warning must reflect only the current recording, not accumulated
      session history.
- [ ] **Trim keeps the keyframe at or before the cut.** In the Edit overlay, drag the start handle
      into the middle of a multi-GOP clip and Save; the exported file starts cleanly (no black or
      frozen lead-in, no overshoot past the end), with duration matching the trimmed range.
- [ ] **APP audio row arming across targets (settings rework).** On a display target, enable the
      receded `APP` row in Settings; switch the capture target to a specific window and record —
      per-app audio is present in the file. Switch back to a display target: the row stays
      configured (rendered receded), and a new recording carries no app track.
- [ ] **Quality scale reads truthfully.** The Default ladder shows tier names only, with no CQ
      number. In Expert, the CQ field's line underneath names the quantizer the selected codec is
      actually given, and it changes with the codec: `AV1 qindex 65 of 255`, `H.264 QP 19 of 51`,
      `HEVC QP 19 of 51` at CQ 19.
- [ ] **Five-tier quality + free frame rate record/playback.** Record one clip at the `Ultra` tier
      and one with an Expert free frame rate (e.g. 73 fps): both play back correctly and the
      container reports the chosen rate. Leaving Expert keeps the truth visible: the frame-rate
      combo grows a dynamic `73 fps (Custom)` entry showing the actually configured value — it never
      snaps to (or claims) a standard value like 60 fps while a custom rate is stored — and the
      "Current format" footer shows the same stored rate.
- [ ] **Reworked Settings page visual walkthrough.** One pass in both modes: rebalanced columns
      (left Container/Quality/Webcam/Notifications/Hotkeys, right Output/Audio/Updates/Appearance/
      Developer), uniform 46 px rows, webcam card with full-width live preview and key-color picker,
      "Split by time"/"Split by size" rows, Developer card visible without Expert.

### RC3 regression live checks

Named after the cycle the defects were **fixed** in, not the build under test: these run against
every subsequent candidate. Targeted regressions for defects fixed in the rc3 cycle (drop truthfulness, WGC frame copies,
webcam fps negotiation, audio-timing, VFR epoch clamping). Run against the RC build on real
hardware:

- [ ] **A. Solo-SYS silence + late audio start.** Monitor capture, only `SYS` enabled (`MIC`/`APP`
      off). Start recording, play **no** system audio for 10–15 s, then start a video/test tone with
      a visible cue; keep recording a while, stop. In the file: the initial silent stretch is
      preserved as real silence, the late audio is **not** pulled forward by the silent span, A/V
      sync is correct from the first audible sound on, and there is no doubled silence-fill stretch.
      Repeat once with a pause/resume inside the run.
- [ ] **B. WGC window capture: motion + resize.** Record a moving/scrolling window with several
      seconds of continuous motion: no tearing, no held frames that mutate after the fact, no stale
      frames or visible jumps backwards (WGC pool surfaces must be copied out, never encoded in
      place). Then resize the window sharply mid-recording: the recording ends with a clear,
      explicit error message (no corner content, no uninitialized/stale pixel borders), and the
      partial file survives per the existing recovery policy.
- [ ] **C. Webcam fps truth.** Pick a webcam mode with a clearly different frame rate than the
      default (e.g. 1080p60 instead of 1080p30) and record. The UI-selected mode is actually
      handed to Media Foundation — the engine no longer hard-codes 30 fps — and the displayed vs.
      actually negotiated rate do not contradict each other (if the camera lacks the exact mode,
      note the nearest native mode chosen). No redundant re-uploads of the same webcam sample at a
      higher CFR output rate.
- [ ] **D. High-refresh/VRR source → CFR 60 drop truthfulness.** On a 120/144/165 Hz display,
      record CFR 60 with phase-correct pacing. Normal source-frame selection and coalescing are
      **not** surfaced as real drops anywhere: no drop toast, Post-Flight Report Card reports no
      frame loss, Edit/Review matches Diagnostics, Pipeline Health stays healthy. The session
      report may show high `coalesced`/`cfr` pacing counters, but
      `frames_dropped.backpressure == 0` and `frames_dropped.processing_failure == 0`, and the
      recording plays with a stable CFR timeline.
- [ ] **E. VFR start after a static source.** Leave the desktop/window fully static for several
      seconds, start a **VFR** recording, create motion after a few seconds, stop. The container
      duration matches the real recording time (no artificially long lead-in from a stale
      pre-recording present timestamp), the first video PTS is at/near 0, the timeline is never
      negative, and audio and video stay aligned.
- [ ] **F. Processing-failure drop surfaces agree.** A real GPU processing failure is hard to
      provoke on healthy hardware — unit/integration tests remain the primary gate for the failure
      path itself. In the normal RC3 run, verify instead that **all surfaces show the same drop
      numbers** (live drop indicator, Diagnostics, Pipeline Health, Post-Flight Report Card,
      Edit/Review, toast, session report, soak metrics), and that a healthy recording shows
      `processing_failure == 0`, `backpressure == 0`, no real-drop toast, and no warning caused by
      benign pacing. Use an existing fault-injection path if one is available; do not build new
      debug infrastructure for this right before RC3.
- [x] **G. Historical rc2 → rc3 updater mechanics.** Natural discovery is a known failed/waived
      gate because rc2 embeds `0.9.0` and therefore cannot consider `0.9.0-rc3` newer. The released
      updater mechanics were exercised separately with the controlled lower `--current-version`
      injection documented in §3 (portable + MSI, UAC decline, network failure, close refusal and
      cleanup). That proves the mechanics only; it does **not** count as natural discovery. RC4's
      current acceptance gate is §7a below.

### §7a — RC acceptance live checks (version identity + verification reinstall)

The full embedded release version and the verification-reinstall mode landed in rc4
(ADR 0054/0055); every candidate from rc4 on is checked this way. `<rc>` below is the RC under test
(`0.9.0-rc10` at the time of writing). Run these against the **published** artifacts of that RC, not
against a local build:

**Identity**

- [ ] The published portable ZIP's `exosnap.exe` reports `ProductVersion == <rc>`
      (`(Get-Item exosnap.exe).VersionInfo.ProductVersion`), and the About page shows
      **Version `<rc>`** with no *Unofficial build* / *Dirty source tree* notice.
- [ ] `update-manifest.json` on the release carries `"version": "<rc>"`, and **Copy details**
      pastes the full commit SHA, build ID, install mode, channel and the executable SHA-256
      matching the published artifact hash.

**Natural discovery** (rc1–rc3 installs cannot prove this — they misidentify as `0.9.0`, see §3)

- [ ] A Preview-channel `<rc>` install is **not** offered `<rc>` again in normal mode
      (`✓ Up to date`).
- [ ] After the next candidate or the final `v0.9.0` publishes: the `<rc>` install **naturally**
      shows `Update available — <ver>` and `Update to <ver>` launches the updater; complete it end
      to end once for portable and once for MSI. A Stable-channel install never sees an rc.

**Portable (verification reinstall, `--verify-update-reinstall`)**

- [ ] Start `<rc>` portable with the flag: card shows `Verification reinstall available — <rc>` +
      `Reinstall <rc>`; app log and support bundle record the active mode.
- [ ] Full path runs: close → swap → verify → relaunch → cleanup; installed EXE hash equals the
      published `<rc>` hash; backup and temp directory removed afterwards.
- [ ] Interrupt the download once (kill network): old install intact, Retry works.
- [ ] Updater refuses to close during the swap-critical steps.
- [ ] Without the flag, the same `<rc>` is **not** offered (up to date); the flag does not survive a
      restart.

**MSI (verification reinstall)**

- [ ] Same-version reinstall over MSI: UAC decline leaves a retryable amber state; retry installs;
      `<rc>` relaunches; installed EXE matches the published hash.

**Guards**

- [ ] With a recording (or finalization) active, both the manual check and the Reinstall/Update
      action are blocked with the honest message; Scoop-managed installs still show the Scoop card
      and never launch the swap updater (also in verify mode).
- [ ] Clicking `Check for updates` with the card scrolled into view does not move the scroll
      position (regression check for the focus-steal fix).

### Long-duration soak (clock slaving)

Tooling and detailed reference: `docs/dev/soak-and-recovery-drills.md` §§1–2 (`exosnap-soak`,
`av-sync-check.py`). This checklist adopts one of that runbook's advisory numbers as an explicit
0.9 gate — see the note in that doc.

- [ ] **2–3 h soak recording, default profile (MKV + AV1 + Opus, CFR 60), monitor capture, `SYS` +
      `MIC` enabled as separate tracks, clock slaving at its default (on).** Continuous real system
      audio for the whole run (e.g. a music/video playlist). Machine must not sleep; displays stay
      on; display settings unchanged during the run.
- [ ] **A/V sync markers at start, middle and end.** Start one process for the planned wall-clock
      recording duration:
      `exosnap-soak --clapper --seconds <duration> --markers 3 --start-margin-seconds 10
      --end-margin-seconds 10`. It emits full-frame flash + beep markers at `+10 s`, the midpoint,
      and `-10 s` with no manual replay. Markers appear on the **primary monitor only**, so the soak
      must capture the primary monitor. With webcam PiP enabled, additionally clap hands in view
      near the markers for a `MIC`-track cue.
- [ ] **Analyze.** Run `python scripts/dev/av-sync-check.py <recorded-file> --max-drift-ms 20
      --expected-markers 3 --marker-times-seconds <start,mid,end>`
      (exit `0` = within budget, `2` = over budget, `3` = unmeasurable). The reported absolute
      offsets carry a device-dependent emission skew (flash via display capture vs. beep via SYS
      loopback, ~10–50 ms) that is not an ExoSnap error and is advisory only — **only start→end
      drift is the canonical pass/fail**, budgeted at ≤ 20 ms here. Record both segment drifts too;
      large opposing segments that cancel at the endpoint are a reliability finding, not a clean
      result.
- [ ] **Second, shorter soak (30–60 min) with a 44.1 kHz endpoint as the only audio source**
      (covers the 44.1 kHz gate and exercises the resampler drain path end-to-end).
- [ ] **Post-checks.** Compare audio vs. video stream durations (ffprobe) on every produced track —
      real stream durations must be plausible against the wall-clock recording time; spot-listen at
      start/middle/end plus a waveform scan for crackles/discontinuities. Confirm the session report
      written at stop — `%LOCALAPPDATA%\ExoSnap\logs\reports\session-<recording_session_id>.json`
      (newest of the last 10 kept) — shows sane `counters.av_drift_ms` / `counters.peak_av_drift_ms`
      / `counters.duration_skew_ms` and `counters.mux_failures` at 0, and every entry in `segments`
      marked `finalized`.

      Audio outages are judged by lost time, not by their count. A machine under real load misses
      capture buffers, and the engine answers each miss with exactly as much silence, so the track
      stays aligned with video and only that much audio is missing. Requiring
      `counters.audio_discontinuities == 0` would therefore fail the product for handling load
      correctly. Assert instead that `counters.audio_discontinuity_ms_total` stays under 0.1 % of
      the recording duration and that `counters.audio_discontinuity_ms_longest` stays at or below
      120 ms; the count remains informative, never a criterion. Additionally assert:
  - `audio.resampler_drain[*].undrained_frames == 0` (every track that drained),
  - `audio.degraded_occurred == false`,
  - `counters.frames_dropped.processing_failure == 0`,
  - `counters.frames_dropped.backpressure == 0`,
  - `counters.encoder_keyframe_prediction_mismatches == 0`.

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
