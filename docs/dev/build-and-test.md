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

`cargo exo-dev` is a Cargo alias defined in `.cargo/config.toml`; run it from the repository root. It builds `tools/exo-dev` when its sources changed and runs it. `--fast` is the scoped pre-commit contract: it may check more than a change strictly needs, never less. `--full` is the complete local gate for what still runs locally: missing-tool failures, packaging/privacy/egress guards, source hygiene, format and the Rust workspace (`fmt`, `clippy`, `cargo test`). The git hooks call the same two contracts. `--dry-run` executes nothing and shows the plan a change would get; `--simulate-fail <step>` exercises the failure paths without a compiler.

Each CI job calls one named profile (`--profile ci-lint`, `ci-guardrails`, `ci-build-debug`, `ci-build-release`, ...), so what a job blocks on is defined next to the local contract. A check that blocks in CI either also blocks before a push or declares why it cannot, in `StepId::info` (`tools/exo-dev/src/step.rs`). Every run writes step logs and a receipt under the untracked workspace scratch directory's `verify` subdirectory. The receipt records the profile, HEAD, whether the tree was dirty, each check's status and whether it is still a PowerShell or Python script behind the Rust orchestration (`implementation: legacy`).

### Developer-loop tiers

Commit and push are not release qualification: they check the change, not the product. Configure, build, ctest, cppcheck and the curated blocking clang-tidy set do not run in either local hook. They ran there once, unconditionally, on every commit and push; that duplicated the pull request's own build/test/analysis legs at several minutes of cost per commit for coverage the PR gate already had. They now run once, on the pull request, scoped to what it actually changed:

| Check | Local trigger | Now runs in | Why |
|---|---|---|---|
| CMake configure, `all_qmllint`, build | pre-commit, pre-push | `ci-build-debug`, `ci-build-release` | A Debug/Release compile is push-shaped cost, not commit-shaped; the PR gate already blocks on it before merge. |
| ctest suite | pre-commit, pre-push | `ci-build-debug`, `ci-build-release` | Same: needs the build it stood on, which is now a PR-CI concern. |
| Curated blocking clang-tidy set (`bugprone-use-after-move` and its four siblings, see [static analysis](static-analysis.md)) | pre-commit (changed-since-base), pre-push (whole-tree) | `ci-build-debug`, scoped to what the pull request changes against its base | The curated set was already clean tree-wide before promotion; a new violation is caught wherever it is introduced, on the PR that introduces it. Re-scanning unrelated files on every local push added cost without new coverage. |
| cppcheck (`--enable=warning,performance,portability --error-exitcode=1`, whole `libs/`+`app/`) | pre-commit, pre-push | `ci-lint` | Needs no compile database (`cargo exo-dev lint quality --only cppcheck`), so it belongs on the build-free lint leg, not duplicated against every local build. |

`StepId::Rust` (`fmt`/`clippy`/`cargo test` over the `tools/` workspace) and `StepId::ScriptTests` stay local: pre-commit skips `rust` when the staged change touches nothing under `tools/`, and both steps are cheap relative to a C++ configure/build. Warm, typical measurements on this tree (a change that does not touch `tools/`): pre-commit well under 15 s; pre-push, which is not scope-narrowed and always runs the Rust workspace and script suites, around 55-60 s. A change to `tools/` itself pays the full `fmt`+`clippy`+`cargo test` cost locally in both hooks, by design: `exo-dev` is what enforces the developer loop, so its own commits still prove themselves before they can weaken that enforcement.

`cargo exo-dev verify --full` is not a release qualification and never was: it is the pre-push contract for what still runs locally. `cargo exo-dev verify --profile ci-build-debug --event <event>` (needs a Ninja-preset build tree) and the deep advisory passes in `.github/workflows/advisory-checks.yml` are the whole-tree/deep tier; they are not implied by a green push.

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
- `StepId::ScriptTests` still runs the remaining `scripts/tests/*.tests.ps1` suites directly. They guard `LiveVerifyClient.psm1` and the VM recipe, which stay until the Rust Hyper-V backend has proven parity with the PowerShell one.
