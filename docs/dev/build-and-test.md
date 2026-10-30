# Build and test

This is the normal contributor entry point. [CONTRIBUTING](../../CONTRIBUTING.md) owns change/review policy. [Architecture](../architecture/overview.md) owns module boundaries; [release checklist](../release-checklist.md) owns release acceptance.

## Environment and configuration

Use Windows 10/11 x64, Visual Studio 2022 Desktop development with C++, PowerShell 7, Git and the repository's CMake presets. The root declares CMake 3.27 as its minimum; the pinned Qt/toolchain can impose a newer practical requirement. Qt, FFmpeg and other dependency pins are build inputs, not independent versions to select casually. Use the matching developer shell for Ninja/MSVC.

The source language is C++20. NVIDIA NVENC is needed for real recording, not for most pure tests. Rust owns automation and protocol tests in the `tools/` workspace. PowerShell is the Windows command shell, not a separate script-test runtime.

The canonical Qt SDK version is in `.qt-version`; the current minimum is Qt 6.12.
Provisioning distinguishes base archives from modules: ShaderTools is a separate module. The Quick profile also requires the qttools archive for Qt LinguistTools (lupdate, lrelease and lcheck).
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

Create a local checkout with `git worktree add .worktrees/<name> -b <branch> origin/next`, following [the worktree policy](../../AGENTS.md#worktrees-and-storage). Use `git worktree list --porcelain` to establish ownership and state before storage cleanup. A missing checkout is an administrative-metadata case: inspect `git worktree prune --dry-run`. It does not reclaim a deleted checkout's former build files. For an inactive checkout that must move, prefer `git worktree move <old> <new>` and verify the resulting metadata before resuming work.

CMake/Ninja outputs remain private to their source checkout. Cargo's registry and Git download caches already live outside the checkout by default. The `cargo exo-dev` alias deliberately uses a separate `tools/target/exo-dev-host` so its running executable cannot collide with the Rust test build. Keep target directories isolated across concurrently active worktrees. Compare compatible artifact-cache reuse before changing target locations or Rust incremental settings.

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

Use repeatable `--build-target <target>` arguments when a focused run needs only known CMake test targets, for example `cargo exo-dev test --filter '^quick\.qml\.record_source_picker$' --build-target record_source_picker_qml_tests`. Without explicit targets the runner retains the full-build default. A targeted receipt records its targets and proves the selected test executables current through the Ninja graph. Building an unrelated target does not make an existing test binary current. Targeted evidence requires direct build-tree test executables. Opaque script or external wrapper commands need the full-build default. Unsupported or stale targeted evidence is refused rather than described as a successful full build.

`cargo exo-dev` is a Cargo alias defined in `.cargo/config.toml`; run it from the repository root, including the root of a git worktree created inside the checkout. It builds `tools/exo-dev` when its sources changed and runs it. `--fast` is the scoped pre-commit contract: it may check more than a change strictly needs, never less. `--full` is the complete local gate for what still runs locally: missing-tool failures, packaging/privacy/egress guards, source hygiene, format and the Rust workspace (`fmt`, `clippy`, `cargo test`). The git hooks call the same two contracts. `--dry-run` executes nothing and shows the plan a change would get; `--simulate-fail <step>` exercises the failure paths without a compiler.

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

`StepId::Rust` runs `fmt`, `clippy` and Cargo tests over the `tools/` workspace locally. Pre-commit scopes this step to staged Rust-tool changes. Pre-push selects the complete Rust step when branch or working changes affect Rust tooling, shared Rust configuration, hooks or workflows. Known documentation and C++/QML-only changes can omit Rust verification. Unclassified paths, unresolved bases and failed scope queries run Rust conservatively. All other pre-push guardrails remain complete. Automation-policy checks always run, including when a new script is outside `tools/`.

The pre-push hook reads every branch update from Git's stdin. Its scope includes both the integration-base diff and each old-remote-to-new-local diff, including deletions, plus staged, unstaged and untracked working inputs. It verifies only pushed heads equal to the checked-out HEAD. Push another source tree from its own checkout so the executed tools and checked source agree. Tag-only, unchanged and deletion-only pushes retain their existing no-verification behavior. Malformed or unreadable ref input refuses the push.

Staged verification refuses partially staged paths whose working bytes differ from the index. It also refuses unstaged or untracked verification-tooling inputs that could change the executed Rust checks. Stage or separate those changes before committing. Unrelated unstaged native source edits remain allowed. This prevents a working-tree fix from certifying different staged bytes.

`cargo exo-dev verify --full` is not a release qualification and never was: it is the pre-push contract for what still runs locally. `cargo exo-dev verify --profile ci-build-debug --event <event>` (needs a Ninja-preset build tree) and the deep advisory passes in `.github/workflows/advisory-checks.yml` are the whole-tree/deep tier; they are not implied by a green push.

The Debug CI sequence is configure, early QML contracts, `all_qmllint`, the complete build, blocking clang-tidy, then the complete CTest suite excluding the `live` label. The early stage builds the three QuickTest targets for shared controls, Edit timeline and Record source picker in the same build tree. Three exact contract registrations select nine representative internal functions and have a 120-second timeout each. They load test components and exercise creation, state restoration, keyboard interaction and settings/source wiring. Production module lint does not establish that filesystem-loaded QuickTest fixtures resolve their types and imports. A missing registration or failing contract stops the full build and analysis. The later build reuses unchanged outputs. All full QuickTest suites, navigation/AOT, About, updater, Debug/Release verification, packaging and required CI contexts retain their existing contracts.

Run `cargo exo-dev verify --profile qml-smoke` for this focused Debug gate locally. It configures the current source, builds its targets incrementally and runs the same bounded contracts with normal locks, isolation, census and receipts. It is an explicit deeper check. Ordinary hooks remain free of CMake/native builds. A build-free QML parser cannot establish the production C++ types, resource registrations and imports, so the staged hook does not claim native QML coverage.

QuickTest registrations retain a separate text log from the original execution, including Windows GUI executables whose stdout is unavailable. The runner clears selected logs before testing and retains their output with CTest evidence. Failure summaries identify internal Qt/QML and GoogleTest cases, locations and assertions where available. Receipts distinguish test failures, QML load errors, build failures, timeouts, abnormal exits and stale or invalid evidence. Early QML receipts and logs are retained separately before the full suite replaces its canonical last-run files.

Pull requests changing only canonical documentation skip both Windows product build legs.
The existing build-path filter keeps product sources, resources, test fixtures and build machinery eligible.
Cheap Rust, AV-sync and documentation checks retain their normal gates.
Cache warming on `next` also skips known uncompiled documentation alone. Shipped documentation,
unknown paths and mixed documentation/code changes remain eligible. Tags and manual CI runs retain full verification.
This scheduling policy does not weaken test receipts: changing any tracked working-tree input during a run still invalidates its source identity.

Build, test and receipt publication share a host lock per build directory. Independent trees do not block each other. Do not run a second unmanaged build into a tree the test runner is currently judging.

Every test run writes `<BuildDir>/Testing/last-run-receipt.json`, including a failing run. `reusable` is true only when build/source identity, phase declarations, census and evidence requirements hold. Targeted runs record requested build targets and the resolved test executable outputs actually built. A clean printed case list does not override an unusable receipt. `--no-build` can refuse stale binaries (exit 3); `--allow-stale` deliberately weakens that claim. Exit 4 means no usable verdict, not a passing product. Retain raw build/CTest exit codes and failure classification when diagnosing the runner. Missing diagnostic evidence does not replace a known timeout or abnormal-exit cause.

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

## Release package footprint

The canonical Rust `exo-verify package` operation writes
`package-size.json` and `package-size.md` alongside its `package.json`.
It measures the **validated, pruned** runtime staging tree used by both
packaging formats and reports the compressed Portable ZIP, MSI and offline
Setup sizes separately. The report lists component-category totals, the largest
shipped files, the separate Setup UI runtime and the Visual C++ redistributable
input where present.

Packaging-relevant PRs reuse the Release packaging-smoke operation. The report
is added to the CI step summary and uploaded with the existing package-smoke
evidence. Release-candidate jobs also display their report and upload it
separately from the immutable candidate bundle. Tests for the inventory live
in the Rust package module and run through the existing Rust workspace gate.

The Qt Widgets and Qt Labs Platform total is an **uncompressed candidate
footprint**, not a measured compressed saving or permission to remove
transitively imported libraries. ExoSnap's PE dependency audit, QML import
checks, package validation and Windows tray smoke remain authoritative for
correctness. Keep each report's JSON file for direct size comparisons against
future candidate reports of the same schema.

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

The German geometry fixtures also render the production audio resource card at 860, 1100 and 1600 logical pixels with long opaque IDs and unavailable reasons. Adapter fixtures check exact clip counters, redacted resource rows, shared-owner deduplication and independence from editable next-recording drafts. Runtime fixtures check source gap/overflow counters and track telemetry after mixer ownership transfer. These fixtures do not qualify physical device recovery, GPU capture or audible synchronization.

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

## MSVC STL hardening qualification

The production default does not define _MSVC_STL_HARDENING.
The macro is available starting with Visual Studio 2022 17.14, but compiler support alone
is not qualification for adoption. Hardened preconditions can terminate the process
with __fastfail, including an active recording. They are not recoverable C++ exceptions,
AddressSanitizer, general heap corruption detection or Release iterator debugging.

Qualification requires consistent settings for first-party translation units and the
static-library/header boundaries that share STL inline or template implementations.
The macro is independent of the STL ABI. Equal object sizes do not prove ODR consistency:
linking different inline definitions can select an unhardened implementation for both callers.
Prebuilt DLL implementation coverage is a separate question from that shared-definition risk.
Do not infer that Qt must be rebuilt solely from enabling this macro.

Run deliberate precondition violations in isolated child processes, with valid-operation
controls and an unhardened negative control. Keep Debug iterator checking separate from
proof that this macro is effective. Before adoption, validate Debug and Release, the pinned
CI toolchain, and representative recording paths with repeated measurements on a quiet host.
Microbenchmarks collected under competing compiler or browser load do not establish
recording latency headroom or acceptable end-to-end overhead.

The remaining adoption requirements are coherent shared-definition coverage, CI hardening
probes and representative runtime qualification. Keep the production default unchanged
until those requirements have evidence. Do not substitute _ITERATOR_DEBUG_LEVEL=1 in Release.
See [Microsoft's STL hardening documentation](https://github.com/microsoft/STL/wiki/STL-Hardening)
for the protected APIs and default settings.
