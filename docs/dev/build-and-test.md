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

The headless `quick.qml.recording_review` suite renders the controls with a decoded still fixture. It does not prove native video presentation. `quick.qml.recording_review_native` uses the Windows platform and Direct3D 11, opens a test window and belongs to the desktop phase. Coordinate desktop availability before running that selection locally. Its native assertions remain part of the full CI suite.

A filter selects a registered test/binary prefix, not necessarily one internal GoogleTest case. Check the printed selection. Hardware exclusions reduce reach and must be reported as such.

Use repeatable `--build-target <target>` arguments when a focused run needs only known CMake test targets, for example `cargo exo-dev test --filter '^quick\.qml\.record_source_picker$' --build-target record_source_picker_qml_tests`. Without explicit targets the runner retains the full-build default.

`--build-target auto` derives the targets from the selected tests instead: a selected test that runs a direct Ninja build-tree executable contributes that executable's output path, so the run builds those binaries and their library dependencies rather than every test binary. A selection whose test command is not such an executable falls back to the complete build and says why, and so does a non-Ninja tree. `auto` needs an already configured tree because the selection comes from CTest, and it cannot be combined with explicit targets. CTest omits an executable command before it exists. Direct GoogleTests and registered Qt Quick executables declare an `EXOSNAP_BUILD_TARGET` property, so `auto` can build them even in a newly configured tree. Script tests, wrappers and tests without an explicit target still fall back to the full build. A targeted receipt records its targets and proves the selected test executables current through the Ninja graph; `--no-build` with `auto` keeps that narrowed claim. Building an unrelated target does not make an existing test binary current. Targeted evidence requires direct build-tree test executables. Opaque script or external wrapper commands need the full-build default. Unsupported or stale targeted evidence is refused rather than described as a successful full build.

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

PR manager tests use injected GitHub responses and isolated Git repositories: `cargo test --locked --manifest-path tools/Cargo.toml -p exo-dev --lib pr:: --jobs 2`. They cover merge identity drift, expected-head submission, current branch refs, duplicate accepted updates, linked-worktree serialization and refusal paths without mutating GitHub. The [contributor workflow](../../CONTRIBUTING.md#opening-and-merging-a-pull-request) defines review prerequisites, branch-update commands and the limits of activation-time SHA guards. These tests do not establish hosted reviewer availability or server-side review enforcement.

### Developer-loop tiers

Commit and push are not release qualification: they check the change, not the product. Configure, build, ctest, cppcheck and the complete curated blocking clang-tidy set do not run in either local hook. The pull request owns the native build, test and complete analysis gates.

Both hooks run `clang-tidy-quick` for direct changed C/C++ sources when an existing Ninja compilation database is available. This uncached preflight uses the canonical blocking checks with two workers and no configure/build; header-only changes, missing databases and sources absent from the configured targets explicitly report SKIP/incomplete coverage. Pre-commit uses the exact staged file scope; pre-push uses its branch, remote and working scope. Real blocking findings in covered sources stop the hook. Use `cargo exo-dev lint clang-tidy --quick --base HEAD` explicitly; missing databases or uncovered direct sources fail that command with configuration guidance. A preflight does not replace the required header-aware blocking analyzer gate in CI.

The complete native gates run on the pull request, scoped to what it changes:

| Check | Local trigger | Now runs in | Why |
|---|---|---|---|
| CMake configure, `all_qmllint`, build | pre-commit, pre-push | `ci-build-debug`, `ci-build-release` | A Debug/Release compile is push-shaped cost, not commit-shaped; the PR gate already blocks on it before merge. |
| ctest suite | pre-commit, pre-push | `ci-build-debug`, `ci-build-release` | Same: needs the build it stood on, which is now a PR-CI concern. |
| Curated blocking clang-tidy set (rules and path scopes: [static analysis](static-analysis.md), `BLOCKING_RULES` in `tools/exo-dev/src/lint/canaries.rs`) | pre-commit (changed-since-base), pre-push (whole-tree) | `ci-build-debug`, scoped to what the pull request changes against its base | Each rule has a measured clean scope and a positive violation canary. A new violation fails within that scope. Re-scanning unrelated files on every local push added cost without new coverage. |
| cppcheck (`--enable=warning,performance,portability --error-exitcode=1`, whole `libs/`, `app/` and `apps/diagnostics_helper/`) | pre-commit, pre-push | `ci-lint` | Needs no compile database (`cargo exo-dev lint quality --only cppcheck`), so it belongs on the build-free lint leg, not duplicated against every local build. |

`StepId::Rust` runs `fmt`, `clippy` and Cargo tests over the `tools/` workspace locally. Pre-commit scopes this step to staged Rust-tool changes. Pre-push selects the complete Rust step when branch or working changes affect Rust tooling, shared Rust configuration, hooks or workflows. Known documentation and C++/QML-only changes can omit Rust verification. Unclassified paths, unresolved bases and failed scope queries run Rust conservatively. All other pre-push guardrails remain complete. Automation-policy checks always run, including when a new script is outside `tools/`.

The pre-push hook reads every branch update from Git's stdin. Its scope includes both the integration-base diff and each old-remote-to-new-local diff, including deletions, plus staged, unstaged and untracked working inputs. It verifies only pushed heads equal to the checked-out HEAD. Push another source tree from its own checkout so the executed tools and checked source agree. Tag-only, unchanged and deletion-only pushes retain their existing no-verification behavior. Malformed or unreadable ref input refuses the push.

Staged verification refuses partially staged paths whose working bytes differ from the index. It also refuses unstaged or untracked verification-tooling inputs that could change the executed Rust checks. Stage or separate those changes before committing. Unrelated unstaged native source edits remain allowed. This prevents a working-tree fix from certifying different staged bytes.

`cargo exo-dev verify --full` is not a release qualification and never was: it is the pre-push contract for what still runs locally. `cargo exo-dev verify --profile ci-build-debug --event <event>` (needs a Ninja-preset build tree) and the deep advisory passes in `.github/workflows/advisory-checks.yml` are the whole-tree/deep tier; they are not implied by a green push.

The Debug CI sequence is configure, early QML contracts, `all_qmllint`, the complete build, uncached direct-source clang-tidy preflight, complete header-aware blocking clang-tidy, then the complete CTest suite excluding the `live` label. The early stage builds the three QuickTest targets for shared controls, Edit timeline and Record source picker in the same build tree. Three exact contract registrations select nine representative internal functions and have a 120-second timeout each. They load test components and exercise creation, state restoration, keyboard interaction and settings/source wiring. Production module lint does not establish that filesystem-loaded QuickTest fixtures resolve their types and imports. A missing registration or failing contract stops the full build and analysis. The later build reuses unchanged outputs. All full QuickTest suites, navigation/AOT, About, updater, Debug/Release verification, packaging and required CI contexts retain their existing contracts.

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

## Local build footprint and cache reuse

A complete Debug Ninja tree measured 24.6 GB; the largest categories were PDBs 8.0 GB, `.ilk` files 6.36 GB, objects 5.3 GB, executables 2.0 GB and staged runtime DLLs 1.75 GB. Test executables still link incrementally. Disabling that was measured and not adopted: it removes 6.36 GB of `.ilk` and costs only 0.37 s on one test binary (7.03 s incremental against 7.40 s full on the 45 MB `record_source_picker_qml_tests`), but a change to a widely linked library then relinks about 270 test binaries in 48 s instead of 13 s. Reconsider it once the dependency replay below is clean, because a no-op build in such a tree relinks the same 270 binaries.

MSVC sccache 0.19.0 stores compiler output with the object.
On a hit, Ninja can receive the producer's `/showIncludes` paths.
A consumer header edit can leave its executable stale while the producer exists.
After the producer is removed, the missing paths can trigger recompilation on every no-op build.
The [upstream correction](https://github.com/mozilla/sccache/commit/e67c02eece3d58620dc54802d6f9d0dfd7516034)
takes the include lines from the consuming compile's preprocessor.
The [isolated probe](../../tools/exo-dev/examples/sccache_deps_replay.rs)
can qualify a new sccache version with `--verify-shared`.

The local Ninja presets keep sccache opt-in.
With `EXOSNAP_USE_SCCACHE=ON`, the managed MSVC C++ launcher adds the source and build directories to the sccache key via `SCCACHE_C_CUSTOM_CACHE_BUSTER`.
Different build trees, including Debug and Release, start cold and can then reuse their own entries.
They can use the same disk cache and server.
CMake rejects a bare sccache launcher override on local MSVC Ninja.
Hosted CI uses its configured cache and stable checkout layout.

`EXOSNAP_SCCACHE_ISOLATED=ON` also gives the build tree a dedicated disk cache and server port.
CMake verifies the server's reported cache directory since a client can silently connect to an existing server on the same port.
Use `EXOSNAP_SCCACHE_PORT` if the derived port collides.

For a build tree that previously used unscoped caching, use a fresh build directory and recompile before trusting test results.
Turning sccache off cannot repair recorded dependencies.
Reconfiguring with the scoped launcher changes Ninja's compile commands, so the next build recompiles its selected targets.
Local `cargo exo-dev test` still audits the recorded includes and can refuse a tree with foreign paths left by earlier builds.
Existing build trees and caches can be kept for inspection.

The Rust host tools build with line-table debug information only. Function names and line tables remain, so a panic backtrace keeps its file and line unless the frame was inlined; measured on `exo-dev` alone, a clean target directory went from 1318 MB to 875 MB.

## CI cache lifecycle

GitHub applies one repository-wide cache cap (10 GB at the time of writing) with LRU eviction. It scopes entries to the ref that wrote them. A pull request can restore its base branch's scope, so `next` is the shared compiler cache for development PRs and `main` is the shared scope for PRs that target the release branch.

| Scope | Writer | Readers |
|---|---|---|
| `refs/heads/next` | `cache-warm.yml`, the crash-capture push handler | PRs against `next` |
| `refs/heads/main` | the nightly advisory run (debug preset only), when `main` changed and weekly | PRs against `main` |
| `refs/pull/<N>/merge` | clang-tidy results and the crash-capture FetchContent tree; no new sccache objects | that PR's own runs |
| `refs/tags/v*` | no new sccache objects | that tag's run |

The `next` workflows set `SCCACHE_GHA_RW_MODE=READ_ONLY` for pull requests, tags and release-candidate builds. A read-only miss compiles normally, and sccache 0.19 reports the refused write as one write error, so those jobs show one write error per miss. The mode only declines to add a scope that dies with the PR or tag. Do not make PR builds read-write again without accounting for the closed scopes that then compete with the shared branch cache. The blocking clang-tidy result cache is separate, small and PR-scoped. The `main` branch carries only the cleanup backport, so its release workflow still uses the previous mode until the same change reaches it.

Manual `workflow_dispatch` is the exception to the table: `cache-warm.yml`, `advisory-checks.yml` and the crash-capture workflow keep their read-write default, so a dispatch on a tag writes that tag's scope. Treat a manual dispatch as a deliberate write.

`cache-cleanup.yml` deletes only scopes it can prove are unreachable: a closed PR's own scope on close, and weekly every pull request scope whose PR is no longer open. Before a ref's entries are deleted, the sweep reads that PR's current state again. A PR reopened between that read and the deletes can still lose its scope; the cache is disposable, so the cost is a rebuild. Branch and tag scopes are never deleted automatically: a branch without an open PR can be pushed again, and a tag workflow can be dispatched again, so removing those needs an explicit retention rule. It runs from the default branch, so the scheduled sweep uses the file on `main`. A change that only lands on `next` cannot fix the schedule. Manual dispatch defaults to a dry run, and the selection and protection rules are each self-tested against a fixture. A listing that fails for rate limiting or permissions stops the run before it deletes from that listing. The repository token allows about a thousand API requests per hour, so a run that exhausts the quota stops with a warning and leaves the rest to the next sweep; clearing a large backlog takes several sweeps.

The cache holds no sccache build directories, PDBs, Qt SDK trees or Cargo target directories. The one exception is the crash-capture workflow's `es-fetchcontent-*` entry, the sentry-native FetchContent build tree, which is about 0.8 GiB and is written to the branch or PR scope that runs it. It exists because rebuilding that tree dominates the crash-capture job. Do not add other build trees: the backend has no per-cache quota to divide, only the one LRU cap, and those artifacts are much larger than the compiler objects they would replace.

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

## Toolchain measurements

`cargo exo-dev toolchain-bench` compares compiler, linker and compiler-cache variants of one CMake target under identical settings. It is a developer tool and never runs in CI.

```powershell
cargo exo-dev toolchain-bench tools
cargo exo-dev toolchain-bench run --variant msvc-debug --variant clangcl-debug --scenario cold --scenario fulllink --reps 3
cargo exo-dev toolchain-bench probe --kind msvc-time --variant msvc-debug --file <source path suffix>
cargo exo-dev toolchain-bench rust --reps 3
```

A variant id is `<msvc|clangcl>[-lld|-link][-sccache]-<debug|release>`. Each variant owns the build tree `build/toolbench-<id>`, configured once from the matching Ninja preset and then reused. A missing tool skips its variants with a reason in the report. `--llvm-dir` or `EXO_LLVM_DIR` locates a portable LLVM distribution that provides `clang-cl` and `lld-link`.

`run` measures wall time and the peak committed memory of the whole build process tree for the scenarios `cold`, `warm`, `noop`, `touch-cpp`, `touch-header`, `relink` and `fulllink`, and writes `report.json` and `report.md` to `--out-dir`, which defaults to a new untracked directory inside the checkout. Memory does not include a compiler-cache server, which runs outside the measured process tree. A `touch` scenario changes only the modification time, so a content-addressed cache answers it with hits: it shows the best case, not an edit loop.

`probe` compiles chosen translation units once with MSVC `/Bt+ /d1reportTime`, clang `-ftime-trace` or clang loop-vectorization remarks, using the flags recorded in the variant's `compile_commands.json`. Remarks name candidate loops only. Confirm that a loop matters at run time before changing source.

clang-cl does not build the unmodified tree. The project's warning branch for clang passes `-Wall`, which clang-cl reads as `-Weverything`, and several dependencies need compiler-specific flags. Pass them with `--clang-cmake-arg` for a measurement. The runner adds a configure-time shim that restores GNU-style warnings without `-Werror` for clang-cl variants. Treat such trees as measurement trees, not as a supported build.

C++ Build Insights (`vcperf`) needs an elevated ETW session and is not scripted. Start and stop the trace yourself, build in between, and analyze with `vcperf /analyze /templates <trace>.etl /timetrace <out>.json`. The trace contains local paths and stays on the machine.

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
