# ADR 0037: Packaging Robustness Gate

## Status

Accepted.

## Context

The v0.1.0 WinGet validation failure (STATUS_DLL_NOT_FOUND on clean machines) and the
0.8.0 ETW/PresentMon tdh.dll allowlist gap both demonstrate that packaging defects can
reach release artifacts silently. ADR 0036 introduced an auto-harvest mechanism to keep
the MSI and portable ZIP in sync. This ADR layers three additional defenses:

1. **MSI smoke test**: The content-assertion step (msiexec /a extraction) already proves
   all staging binaries are present in the MSI, but does not prove the MSI actually
   launches. A loader-stage missing-DLL failure (STATUS_DLL_NOT_FOUND / 0xC0000135)
   would still go undetected if the binary names matched but a file was silently corrupt.
   Adding a smoke launch from the extracted MSI tree catches this class of failure before
   the artifact ships.

2. **Release-gate workflow**: The release-candidate.yml workflow already triggers on
   `release/**` branches and `v*` tags, but the gate contract was implicit. Making it
   explicit (naming the step a "release gate", documenting which -Skip* flags are OFF)
   ensures no future maintainer accidentally adds -SkipSmoke or -SkipMsi to the CI call.

3. **Harvest-regression lint**: A cheap, build-free script (`validate-msi-harvest.ps1`)
   runs on every PR in the `lint` job alongside `validate-winget-manifest.ps1`. It
   asserts Package.wxs is metadata-only (no hand-maintained `<File Source=`) and
   references `StagingFiles`. This prevents a revert to the pre-ADR-0036 state.

## Decision

### A. MSI smoke (build-release-artifacts.ps1)

After the existing MSI content assertion (msiexec /a extraction), launch `exosnap.exe`
from the extracted MSI tree using the SmokeNative Win32 helpers (SetErrorMode +
dialog-sentinel). Only loader-stage failures are treated as hard failures:

- `STATUS_DLL_NOT_FOUND` (0xC0000135 / -1073741515 signed int32): **FAIL**
- Hard-error dialog matching the sentinel pattern: **FAIL**
- Any other non-zero exit (GPU unavailable, single-instance guard): `inconclusive`
  (the loader succeeded; GPU-less CI runners are safe)
- Process still alive after 8-second window: `launched` (pass)

The SmokeNative type is loaded once before the MSI section (shared with the portable ZIP
smoke in step 8) to avoid double-compilation in the same PS session. The extracted MSI
tree (`$msiExtractDir`) is reused from the content-assertion step — no second msiexec /a
call. Both the MSI smoke and the portable smoke are gated by the existing `-SkipSmoke`
switch.

### B. Release-gate workflow (release-candidate.yml)

The existing `release-candidate.yml` job is renamed to "Build and validate release
artifacts (release gate)" and its step comment is updated to enumerate every gate that
runs. The triggers remain unchanged: `push` to `release/**` and `push` of `v*` tags.
`workflow_dispatch` is kept for on-demand runs. Normal PRs are NOT affected.

The script call retains `-SkipConfigure` (cmake was configured in a prior step) but omits
`-SkipSmoke` and `-SkipMsi` so the full pipeline runs: install → audit → MSI → content
assertion → MSI smoke → ZIP smoke.

### C. Harvest-regression lint (scripts/validate-msi-harvest.ps1)

A new dependency-free PowerShell script (regex only, no WiX required) asserts:
- `packaging/msi/Package.wxs` does **not** contain `<File Source=` (no hand list)
- `packaging/msi/Package.wxs` **does** contain `<ComponentGroupRef Id="StagingFiles" />`

Wired into the `lint` job in `ci.yml` (runs on every PR, ~1 second).

### D. Advisory unused-code checks (Phase B)

Static analysis tooling is extended to surface stale code for human review. These are
ADVISORY ONLY — they must not fail the build or block PRs:

**clang-tidy** (`.clang-tidy`): Added to `Checks:` but excluded from `WarningsAsErrors`:
- `misc-include-cleaner` — unused includes (needs compile_commands.json)
- `misc-unused-using-decls` — stale `using` declarations
- `misc-unused-parameters` — unused function parameters
- `misc-unused-alias-decls` — stale type aliases
- `readability-redundant-declaration` — duplicate forward declarations

**cppcheck** (`scripts/check-quality.ps1`): A separate whole-program pass with
`--enable=unusedFunction` (no `--error-exitcode`) is added after the blocking pass. It
prints candidate counts to the console but never fails the script.

**CI** (`ci.yml`): A new `advisory-unused-checks` job runs the advisory checks on every
PR with `continue-on-error: true`. It configures with `windows-x64-ninja-debug`
(which now has `CMAKE_EXPORT_COMPILE_COMMANDS=ON`) and runs both clang-tidy and
cppcheck advisory passes. Findings are reported as GitHub Actions `::notice::` annotations
— visible in the job summary but never blocking.

**Qt false-positive caveat**: Qt slots invoked via QMetaObject::invokeMethod or the
connect() string form, moc-generated headers (Q_OBJECT), and callback factories are
expected to generate false-positives in all five clang-tidy checks and in cppcheck
unusedFunction. No finding should be acted on without a human triage pass.

### E. Updater smoke (build-release-artifacts.ps1) — 0.9.0

The swap-updater (`exosnap-updater.exe`, ADR 0034) is a shipped runtime component, so the packaging
gate now proves it loads. `exosnap-updater.exe` is added to the required-files presence list and is
covered automatically by the dumpbin import audit (which globs every staged `*.exe`/`*.dll`). Beyond
presence, a dedicated smoke reproduces the exact staging the app performs at update time
(`UpdaterStagingFileList` + the `[Paths] Plugins = plugins` qt.conf the app writes): it copies that
minimal runtime subset from the packaged tree into an isolated temp dir and launches
`exosnap-updater.exe --preview-state progress --preview-smoke`. The new `--preview-smoke` flag
auto-closes the window after ~2 s, so a clean exit proves the exe and its staged Qt runtime (Core/
Gui/Widgets + the windows platform plugin) load and render. `STATUS_DLL_NOT_FOUND`, a hard-error /
platform-plugin dialog (sentinel), or any non-zero exit fails the gate. Gated by the same
`-SkipSmoke` switch and reuses the shared `SmokeNative` helpers.

### F. Signed update manifest as a required release job — 0.9.0

The 0.9+ in-app updater only surfaces a release that carries an `update-manifest.json` asset
(verified against the embedded public key before any field is read); a release published without it
is invisible to in-app updates forever (ADR 0012 amendment). `sign-manifest.yml` is therefore wired
into `release-candidate.yml` as a required job (`needs: build`) that runs on a version-tag push for
official builds (the `EXOSNAP_UPDATE_PUBLIC_KEY_HEX` repo variable present). It binds the built
portable + MSI SHA-256 hashes (read from the sidecars as job outputs) and the canonical
GitHub-release asset URLs for the tag, and uploads the signed `update-manifest.json`. Publishing that
manifest as a GitHub release asset is a mandatory checklist step for every release from 0.9.0 on (see
`docs/release-checklist.md`).

### G. PR-CI packaging smoke (path-gated)

The full release gate in `release-candidate.yml` only runs on version tags and `release/**`
branches, so a harvest-fragment regression, a new runtime DLL `windeployqt` fails to stage, or a
leaked dev-tree file is invisible until the release tag is pushed — often long after the change
that caused it merged. `ci.yml` gains a `packaging-smoke` job that runs the fastest slice of the
release gate on every pull request that touches packaging-relevant paths:

- `cmake --install` into the same pruned staging tree the release script produces
- presence / absence / leak / exe-metadata validation of that tree
- the `dumpbin /dependents` static runtime-dependency audit
- portable ZIP creation + the isolated launch smoke (app exe + updater exe)

It runs `-SkipMsi` — the WiX/MSI build, MSI content assertion, and MSI smoke stay exclusive to the
release gate, since they require installing the WiX Toolset and add several minutes for a package
format the portable-ZIP path already exercises structurally (same staging tree, same harvest
source). It builds from the `windows-x64-ninja-release` preset instead of the canonical
VS-generator `windows-x64-release` preset the release gate uses: the VS/MSBuild generator ignores
`CMAKE_CXX_COMPILER_LAUNCHER`, so building with it on every PR would mean a from-scratch compile
each time, whereas the Ninja preset is sccache-cacheable and already used by `build-test`'s release
leg in the same workflow.

`build-release-artifacts.ps1` gained a `-Preset` parameter (default `windows-x64-release`, so the
release gate's invocation is unchanged) and now resolves the built `exosnap.exe` under either a
multi-config (Visual Studio, `app/Release/exosnap.exe`) or single-config (Ninja, `app/exosnap.exe`)
generator layout, since that is the only part of the script that assumed the VS-generator directory
shape.

A `changes` job (`dorny/paths-filter`) gates `packaging-smoke` on pull requests that touch
`packaging/**`, `scripts/build-release-artifacts.ps1`, `cmake/Vendor*`, the install-rule-bearing
CMake files (root, `app/`, `apps/updater/`, `third_party/`), or `THIRD_PARTY_NOTICES.md`. It does
not run on push triggers (main, tags) — those stay covered by the release gate.

## Consequences

- A missing-DLL defect in the MSI is now caught by three layers: dumpbin audit (import
  classification), content assertion (binary presence), and MSI smoke (actual load).
- The shipped `exosnap-updater.exe` is present in both artifacts (its install rule lands it flat in
  the CMake install tree, harvested into the MSI and zipped) and is proven to load via the updater
  smoke — the update feature can never silently ship non-functional in a packaged build.
- Every official release from 0.9.0 on produces a signed `update-manifest.json`; forgetting to
  publish it (which would make the release invisible to in-app updates) is now a wired, required CI
  job rather than a manual afterthought.
- A hand-maintained file-list regression in Package.wxs is caught in every PR in under
  1 second.
- The release-gate contract is explicit: no `-SkipSmoke` or `-SkipMsi` flags in CI.
- Advisory unused-code candidates surface for review without blocking any workflow.
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` is now set in the `windows-x64-ninja-debug` preset,
  enabling include-cleaner analysis for Ninja-preset users.
- Packaging drift (harvest gaps, missing runtime DLLs, leaked dev-tree files) surfaces on the
  pull request that introduces it, for the paths most likely to cause it, instead of only at the
  next release tag.
