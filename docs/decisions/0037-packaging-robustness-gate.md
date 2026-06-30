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

## Consequences

- A missing-DLL defect in the MSI is now caught by three layers: dumpbin audit (import
  classification), content assertion (binary presence), and MSI smoke (actual load).
- A hand-maintained file-list regression in Package.wxs is caught in every PR in under
  1 second.
- The release-gate contract is explicit: no `-SkipSmoke` or `-SkipMsi` flags in CI.
- Advisory unused-code candidates surface for review without blocking any workflow.
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` is now set in the `windows-x64-ninja-debug` preset,
  enabling include-cleaner analysis for Ninja-preset users.
