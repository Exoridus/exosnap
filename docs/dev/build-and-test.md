# Build and test

This is the normal contributor entry point. [CONTRIBUTING](../../CONTRIBUTING.md) owns change/review policy. [Architecture](../architecture/overview.md) owns module boundaries; [release checklist](../release-checklist.md) owns release acceptance.

## Environment and configuration

Use Windows 10/11 x64, Visual Studio 2022 Desktop development with C++, PowerShell 7, Git and the repository's CMake presets. The root declares CMake 3.27 as its minimum; the pinned Qt/toolchain can impose a newer practical requirement. Qt, FFmpeg and other dependency pins are build inputs, not independent versions to select casually. Use the matching developer shell for Ninja/MSVC.

The source language is C++20. NVIDIA NVENC is needed for real recording, not for most pure tests. Rust owns automation and protocol tests in the `tools/` workspace. PowerShell is the Windows command shell, not a separate script-test runtime.

The canonical Qt SDK version is in `.qt-version`; the current minimum is Qt 6.12.
Provisioning distinguishes base archives from modules: ShaderTools is a separate module.
Use a fresh build directory when changing the SDK so cached CMake package paths
and deployment tools cannot retain a different Qt runtime.

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

The shipping main and updater modules use normal `qmlcachegen` compilation through
`qt_add_qml_module()`. The navigation lifecycle and updater window smoke tests carry
the `aot` label and run with `QV4_FAIL_ON_INVALID_AOT=1`, so invalid native AOT
assumptions fail qualification. Production launches retain Qt's bytecode fallback.
No `qmltc` compilation is used.

For an on-demand compilation report, run
`cargo exo-dev qml-aot-stats --build-dir build/windows-x64-ninja-release`.
This builds Qt's `all_aotstats` target, prints its compact module summary and retains
the full per-file/function report, including fallback reasons. Use `--report-path`
to copy that report into an evidence directory. Coverage is diagnostic, not a
percentage threshold for CI.

Qt 6.12 also generates `exosnap_qmlpreview` and `exosnap_updater_qmlpreview`
development targets. In a Debug developer shell, set
`$env:EXOSNAP_QML_PREVIEW = '1'`, then run
`cmake --build build/windows-x64-ninja-debug --target exosnap_qmlpreview`.
This opts into the preview services and starts the actual application with Qt's
resource mappings, retaining its C++ adapters and required initial properties.
Clear the environment variable after the session. Release binaries ignore it.
This is an interactive development session, not a qualification gate.
Coordinate desktop use before launching it.
Changing a component's C++ base type can require restarting the preview.

```powershell
cargo exo-dev test
cargo exo-dev test --filter recorder_core.
cargo exo-dev test --exclude-label live
cargo exo-dev verify --fast
cargo exo-dev verify --full
```

A filter selects a registered test/binary prefix, not necessarily one internal GoogleTest case. Check the printed selection. Hardware exclusions reduce reach and must be reported as such.

`cargo exo-dev` is a Cargo alias defined in `.cargo/config.toml`; run it from the repository root. It builds `tools/exo-dev` when its sources changed and runs it. `--fast` is the scoped pre-commit contract: it may check more than a change strictly needs, never less. `--full` is the complete local gate for what still runs locally: missing-tool failures, packaging/privacy/egress guards, source hygiene, format and the Rust workspace (`fmt`, `clippy`, `cargo test`). The git hooks call the same two contracts. `--dry-run` executes nothing and shows the plan a change would get; `--simulate-fail <step>` exercises the failure paths without a compiler.

Each CI job calls one named profile (`--profile ci-lint`, `ci-guardrails`, `ci-build-debug`, `ci-build-release`, ...), so what a job blocks on is defined next to the local contract. A check that blocks in CI either also blocks before a push or declares why it cannot, in `StepId::info` (`tools/exo-dev/src/step.rs`). Every run writes step logs and a receipt under the untracked workspace scratch directory's `verify` subdirectory. The receipt records the profile, HEAD, whether the tree was dirty, each check's status and evidence. Repository automation policy runs in both local modes and CI guardrails. The staged hook validates index contents so unstaged edits cannot conceal a prohibited script.

Verification output defaults to `--output auto`: CI profiles, CI environments and redirected output use `compact`. An interactive terminal uses `normal`.
`compact` prints each executed check's `RUN` and verdict with elapsed time. Full stdout and stderr remain in per-step logs, including successful steps.
Failures print at most 120 tail lines and 64 KiB, useful error context and the full log path. An unavailable tool remains `TOOL_MISSING`.
Compact process steps emit a heartbeat every 90 seconds while running, including when the child produces no output.
`--failure-tail-lines` limits the excerpt further. `--output normal` or `verbose` streams tool output. `verbose` also repeats the detailed final check table.
`--output silent` suppresses verification presentation while preserving logs, receipts and exit status.
CI uploads verification logs on failure. Advisory raw diagnostics, normalized sites and summaries remain artifacts on every run.
The independent Rust CI job invokes Cargo directly and retains its native console output so the orchestrator does not certify itself.

### Developer-loop tiers

Commit and push are not release qualification: they check the change, not the product. Configure, build, ctest, cppcheck and the curated blocking clang-tidy set do not run in either local hook. They ran there once, unconditionally, on every commit and push; that duplicated the pull request's own build/test/analysis legs at several minutes of cost per commit for coverage the PR gate already had. They now run once, on the pull request, scoped to what it actually changed:

| Check | Local trigger | Now runs in | Why |
|---|---|---|---|
| CMake configure, `all_qmllint`, build | pre-commit, pre-push | `ci-build-debug`, `ci-build-release` | A Debug/Release compile is push-shaped cost, not commit-shaped; the PR gate already blocks on it before merge. |
| ctest suite | pre-commit, pre-push | `ci-build-debug`, `ci-build-release` | Same: needs the build it stood on, which is now a PR-CI concern. |
| Curated blocking clang-tidy set (rules and path scopes: [static analysis](static-analysis.md), `BLOCKING_RULES` in `tools/exo-dev/src/lint/canaries.rs`) | pre-commit (changed-since-base), pre-push (whole-tree) | `ci-build-debug`, scoped to what the pull request changes against its base | Each rule has a measured clean scope and a positive violation canary. A new violation fails within that scope. Re-scanning unrelated files on every local push added cost without new coverage. |
| cppcheck (`--enable=warning,performance,portability --error-exitcode=1`, whole `libs/`+`app/`) | pre-commit, pre-push | `ci-lint` | Needs no compile database (`cargo exo-dev lint quality --only cppcheck`), so it belongs on the build-free lint leg, not duplicated against every local build. |

`StepId::Rust` runs `fmt`, `clippy` and Cargo tests over the `tools/` workspace locally. Pre-commit scopes this step to staged Rust-tool changes. Pre-push runs the complete Rust workspace. Automation-policy checks always run, including when a new script is outside `tools/`.

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

`libs/engine/tests/portable` builds the production mux queue, premux state, failure state, packed split request and callback-gate tests without Qt or capture hardware. On Linux with Clang, configure that directory with `-DEXOSNAP_TSAN=ON` to instrument these components with ThreadSanitizer. The `concurrency-sanitizers` workflow runs this narrow lane. It does not cover Windows capture workers, COM, drivers or Win32 stop events. Windows ASan and deterministic engine tests cover the corresponding memory and lifecycle paths; neither establishes absence of every data race.

```powershell
cmake -S libs/engine/tests/portable -B build/concurrency-portable
cmake --build build/concurrency-portable --config Debug
ctest --test-dir build/concurrency-portable -C Debug --output-on-failure
```

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

## Translation maintenance

English source text is canonical. German is the supported production localization.
Qt LinguistTools from the same Qt 6.12 SDK is required for application builds.
Both executables embed the generated catalogue; no loose QM file is needed when
the updater is staged externally.

The normal pipeline is English source -> TS -> QM -> QTranslator -> QML/C++.
Refresh source strings, edit the German catalogue in Qt Linguist, and build it:

~~~powershell
cmake --build --preset windows-x64-ninja-debug --target exosnap_lupdate
cmake --build --preset windows-x64-ninja-debug --target exosnap_lrelease
lcheck app/i18n/exosnap_de.ts
cargo exo-dev test --filter i18n.
~~~

exosnap_lrelease rejects unfinished translations. The translation tests check
catalogue structure, complete German plural forms and unchanged placeholders.
quick.qml.german_text_geometry_* runs the German resource at 100%, 125%, 150%
and 200%. The existing pseudo-localization tests remain independent and keep
their geometry stress role.

Use natural German around established technical vocabulary. Keep Encoder,
Decoder, Codec, Bitrate, B-Frames, Lookahead, Spatial AQ, Temporal AQ, Multipass,
Preset, HDR/HDR10, SDR, NVENC, AMF, QSV, oneVPL, CQ, VBR, CBR, CFR, VFR, GOP,
Keyframe, Frame Pacing, Capture, Pipeline, GPU, VRAM, FPS, A/V, PTS and DTS.
Codec/container names and units retain their established spelling.
Use Ausgabeauflösung, Bildrate, Aufnahme, Ausgabeordner, Encoder-Preset,
Keyframe-Intervall and Verworfene Frames consistently.

Translate ordinary user-facing labels, workflow/status copy, errors, warnings,
tooltips and accessibility strings. Preserve protocol/JSON/automation keys,
CLI flags, log field/event names, persistent IDs, enum/backend identifiers,
codec/container identifiers, file paths, commands and raw diagnostic evidence.
Qt-free policy sources use an extraction-only marker; adapters translate their
presentation fields with QTranslator, while stable tokens remain unchanged.

System, English and Deutsch persist as system, en and de. System selects German
only for a German system locale. Changes apply on restart. The running app's
effective language is passed to an app-launched updater as presentation data;
manual updater launches use System. A missing translation resource falls back
to English and never prevents an update. The offline WiX Setup is outside this
localization surface.