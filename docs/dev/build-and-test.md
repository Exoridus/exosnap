# Build and test

This is the normal contributor entry point. [CONTRIBUTING](../../CONTRIBUTING.md) owns change/review policy. [Architecture](../architecture/overview.md) owns module boundaries; [release checklist](../release-checklist.md) owns release acceptance.

## Environment and configuration

Use Windows 10/11 x64, Visual Studio 2022 Desktop development with C++, PowerShell 7, Git and the repository's CMake presets. The root declares CMake 3.27 as its minimum; the pinned Qt/toolchain can impose a newer practical requirement. Qt, FFmpeg and other dependency pins are build inputs, not independent versions to select casually. Use the matching developer shell for Ninja/MSVC.

The source language is C++20. NVIDIA NVENC is needed for real recording, not for most pure tests. Python, PowerShell and Rust are also needed by registered tool tests. Configuration defaults to requiring test tools so a missing tool cannot silently produce a smaller green suite.

```powershell
cmake --preset windows-x64-ninja-debug
cmake --build --preset windows-x64-ninja-debug-exosnap
pwsh scripts/run-tests.ps1 -Filter recorder_core.
```

MSBuild presets remain available:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug-exosnap
```

Use a branch from the integrated default branch. Configure installs repository Git hooks and the local fast-forward-only pull policy when possible. These are convenience checks, not substitutes for reviewed changes and server-side protection.

## Iteration and final gate

`run-tests.ps1` configures test process isolation: a throwaway configuration directory, offscreen Qt platform and matching Qt/plugin paths. It builds the selected tree first so the verdict describes current binaries.

```powershell
pwsh scripts/run-tests.ps1
pwsh scripts/run-tests.ps1 -Filter recorder_core.
pwsh scripts/run-tests.ps1 -ExcludeLabel live
pwsh scripts/verify.ps1 -Fast
pwsh scripts/verify.ps1 -Full
```

A filter selects a registered test/binary prefix, not necessarily one internal GoogleTest case. Check the printed selection. Hardware exclusions reduce reach and must be reported as such. `-Fast` is scoped iteration; `-Full` is the complete local gate, including missing-tool failures, whole-tree static checks, build/test and the Rust verifier.

Build, test and receipt publication share a host lock per build directory. Independent trees do not block each other. Do not run a second unmanaged build into a tree the test runner is currently judging.

Every test run writes `<BuildDir>/Testing/last-run-receipt.json`, including a failing run. `reusable` is true only when build/source identity, phase declarations, census and evidence requirements hold. A clean printed case list does not override an unusable receipt. `-NoBuild` can refuse stale binaries (exit 3); `-AllowStale` deliberately weakens that claim. Exit 4 means no usable verdict, not a passing product. Retain raw build/CTest exit codes when diagnosing the runner.

## Documentation gate

```powershell
python scripts/check-documentation.py
python -m unittest discover -s scripts/tests -p documentation_checks_test.py
```

The repository gate invokes the same checker through `check-docs-superpowers-removed.ps1`. It rejects numbered decision references, removed document archives, broken relative Markdown links/fragments and private working-directory leaks in current public Markdown. It checks the working files, including new nonignored files, so stage/delete the intended files before final review. The changelog and temporary migration application records are deliberately excluded from current-state reference rules. Third-party legal notices retain their upstream provenance.

The checker is structural, not an editorial oracle. Review duplication, rationale, ownership and current behavior manually. Do not add a prose-style linter to enforce a writing preference.

## Analysis and harness builds

Use [static analysis](static-analysis.md) for blocking/advisory checks. For memory faults, use `windows-x64-asan` or `windows-x64-ninja-asan`. MSVC AddressSanitizer must be installed; the build stages its runtime DLLs and strips incompatible `/RTC1`. It is a diagnostic configuration, not the release build.

Most developer probes require `-DEXOSNAP_BUILD_PROBES=ON`. Capture instruments used directly by release gates can be unconditional targets. Check [probe reference](../../tools/probes/README.md) before adding another instrument.

Development capture/edit/visual switches are available in non-Release configurations. An exact Release needs `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON` for harness-only modes. Official release acceptance uses the opt-in production control channel, not a specially rebuilt product whose instrumentation changes its identity.

## Verifier development

The Rust verifier owns the scenario registry, candidate plan and report checks:

```powershell
cargo fmt --all --check --manifest-path tools/exo-verify/Cargo.toml
cargo clippy --locked --all-targets --manifest-path tools/exo-verify/Cargo.toml -- -D warnings
cargo test --locked --manifest-path tools/exo-verify/Cargo.toml
```

The lockfile is part of the verifier input. A locked build must fail if dependencies drift.

## What a successful local gate does not prove

A GPU-free unit test does not establish real capture/encode behavior, a generated screenshot does not establish native HWND ownership or overlay desktop composition, and a development build is not a signed downloadable release. Report exactly which layers ran. Native window checks, real playback, endpoint unplug and release installation belong to their named runbooks and acceptance scenarios.
