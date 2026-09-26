# Build and test

This is the normal contributor entry point. [CONTRIBUTING](../../CONTRIBUTING.md) owns change/review policy. [Architecture](../architecture/overview.md) owns module boundaries; [release checklist](../release-checklist.md) owns release acceptance.

## Environment and configuration

Use Windows 10/11 x64, Visual Studio 2022 Desktop development with C++, PowerShell 7, Git and the repository's CMake presets. The root declares CMake 3.27 as its minimum; the pinned Qt/toolchain can impose a newer practical requirement. Qt, FFmpeg and other dependency pins are build inputs, not independent versions to select casually. Use the matching developer shell for Ninja/MSVC.

The source language is C++20. NVIDIA NVENC is needed for real recording, not for most pure tests. Python, PowerShell and Rust are also needed by registered tool tests. Configuration defaults to requiring test tools so a missing tool cannot silently produce a smaller green suite.

```powershell
cmake --preset windows-x64-ninja-debug
cmake --build --preset windows-x64-ninja-debug-exosnap
cargo exo-dev test --filter recorder_core.
```

MSBuild presets remain available:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug-exosnap
```

Use a branch from the integrated default branch. Configure installs repository Git hooks and the local fast-forward-only pull policy when possible. These are convenience checks, not substitutes for reviewed changes and server-side protection.

## Iteration and final gate

`cargo exo-dev test` configures test process isolation: a throwaway configuration directory, offscreen Qt platform and matching Qt/plugin paths. It builds the selected tree first so the verdict describes current binaries.

```powershell
cargo exo-dev test
cargo exo-dev test --filter recorder_core.
cargo exo-dev test --exclude-label live
cargo exo-dev verify --fast
cargo exo-dev verify --full
```

A filter selects a registered test/binary prefix, not necessarily one internal GoogleTest case. Check the printed selection. Hardware exclusions reduce reach and must be reported as such.

`cargo exo-dev` is a Cargo alias defined in `.cargo/config.toml`; run it from the repository root. It builds `tools/exo-dev` when its sources changed and runs it. `--fast` is the scoped pre-commit contract: it may check more than a change strictly needs, never less. `--full` is the complete local gate, including missing-tool failures, whole-tree static checks, build/test and the Rust workspace. The git hooks call the same two contracts. `--dry-run` executes nothing and shows the plan a change would get; `--simulate-fail <step>` exercises the failure paths without a compiler.

Each CI job calls one named profile (`--profile ci-lint`, `ci-guardrails`, `ci-build-debug`, ...), so what a job blocks on is defined next to the local contract. A check that blocks in CI either also blocks before a push or declares why it cannot. Every run writes step logs and a receipt under the untracked workspace scratch directory's `verify` subdirectory. The receipt records the profile, HEAD, whether the tree was dirty, each check's status and whether it is still a PowerShell or Python script behind the Rust orchestration (`implementation: legacy`).

Build, test and receipt publication share a host lock per build directory. Independent trees do not block each other. Do not run a second unmanaged build into a tree the test runner is currently judging.

Every test run writes `<BuildDir>/Testing/last-run-receipt.json`, including a failing run. `reusable` is true only when build/source identity, phase declarations, census and evidence requirements hold. A clean printed case list does not override an unusable receipt. `-NoBuild` can refuse stale binaries (exit 3); `-AllowStale` deliberately weakens that claim. Exit 4 means no usable verdict, not a passing product. Retain raw build/CTest exit codes when diagnosing the runner.

## Documentation gate

```powershell
cargo exo-dev
exo-verify docs check --repo-root .
```

The `docs-superpowers-removed` step calls the built `exo-verify docs check` directly (`tools/exo-verify/src/docs.rs`). It rejects numbered decision references, removed document archives, broken relative Markdown links/fragments and private working-directory leaks in current public Markdown. It checks the working files, including new nonignored files, so stage/delete the intended files before final review. The changelog and temporary migration application records are deliberately excluded from current-state reference rules. Third-party legal notices retain their upstream provenance.

The checker is structural, not an editorial oracle. Review duplication, rationale, ownership and current behavior manually. Do not add a prose-style linter to enforce a writing preference.

## Analysis and harness builds

Use [static analysis](static-analysis.md) for blocking/advisory checks. For memory faults, use `windows-x64-asan` or `windows-x64-ninja-asan`. MSVC AddressSanitizer must be installed; the build stages its runtime DLLs and strips incompatible `/RTC1`. It is a diagnostic configuration, not the release build.

Most developer probes require `-DEXOSNAP_BUILD_PROBES=ON`. Capture instruments used directly by release gates can be unconditional targets. Check [probe reference](../../tools/probes/README.md) before adding another instrument.

Development capture/edit/visual switches are available in non-Release configurations. An exact Release needs `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON` for harness-only modes. Official release acceptance uses the opt-in production control channel, not a specially rebuilt product whose instrumentation changes its identity.

## Rust tooling development

`tools/` is a Cargo workspace with one lockfile. `exo-verify` owns the release scenario registry, candidate plan and report checks, and ships in the candidate bundle. `exo-dev` owns repository verification: change scope, gate order, hooks and CI profiles. It never ships.

```powershell
cd tools
cargo fmt --all --check
cargo clippy --workspace --locked --all-targets -- -D warnings
cargo test --workspace --locked
```

The lockfile is part of the tool input. A locked build must fail if dependencies drift. CI runs these commands directly rather than through `exo-dev`, so the orchestrator is never the only thing that certifies itself.

## What a successful local gate does not prove

A GPU-free unit test does not establish real capture/encode behavior, a generated screenshot does not establish native HWND ownership or overlay desktop composition, and a development build is not a signed downloadable release. Report exactly which layers ran. Native window checks, real playback, endpoint unplug and release installation belong to their named runbooks and acceptance scenarios.

## Known limits

- `install.rs` (`exo-verify`) and `clean-first-start-worker.ps1` define "a clean machine" differently. Reconciling the two definitions is unscoped work.
- `LiveVerifyChecks.ps1` carries eight `LV-*` checks with no `exo-verify` equivalent (`LV-OVL-001`, `LV-WIN-001`, `LV-NAV-002`, `LV-REC-002`, `LV-UIA-001`, `LV-THEME-001`, `LV-WIN-003`, and the single-monitor/SDR coverage inside `LV-PREV-001`/`LV-WIN-002`), and `scripts/lib/BuildArtifacts.psm1` stays in place because `scripts/live-verify.ps1` is still its only runner. `scripts/lib/DependencyIdentity.psm1` (ported into `exo-verify`'s `package::dependency_identity`) similarly stays in place pending a `build-release-artifacts.ps1` call-site swap nobody owns yet.
- `StepId::ScriptTests` still runs `scripts/tests/*.tests.ps1` directly rather than through Rust unit tests, because three of its suites (`check-docs-superpowers-removed.tests.ps1`'s coverage now lives in `exo-verify`'s own `docs::tests`, but `live-verify-response-shape.tests.ps1` and `synthetic-load.tests.ps1` do not have a Rust equivalent) would lose real coverage if the step were removed outright.
