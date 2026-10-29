# Static analysis

`.clang-tidy`, `cmake/exosnap_warnings.cmake` and Rust `exo-dev` own the enabled rules. This guide explains how to run and extend them. Historical scan counts are not current baselines.

## Blocking and advisory paths

| Pass | Status | Entry point |
|---|---|---|
| Selected clang-tidy checks | Blocking | `cargo exo-dev lint clang-tidy` and CI's `ci-build-debug` profile |
| cppcheck warning/performance/portability | Blocking | `cargo exo-dev lint quality --only cppcheck` and CI's `ci-lint` profile |
| Broad clang-tidy advisory set | Advisory | `advisory-checks.yml` |
| cppcheck unused-function analysis | Advisory | `advisory-checks.yml` |

clang-tidy compilation integration requires Ninja; the Visual Studio generator does not run `CMAKE_CXX_CLANG_TIDY`. Use the lint preset or the explicit compile-database runner rather than interpreting a Visual Studio build as a tidy pass.

The blocking set includes use-after-move, dangling handles, misleading indentation and the selected analyzer call/uninitialized/allocation checks. Read `.clang-tidy` and `tools/exo-dev/src/lint/canaries.rs` for exact patterns. Qt meta-object use and registered OS callbacks require special care when triaging apparent unused symbols; broad automated removal is unsafe.

Unused aliases, redundant declarations, unused using declarations and unused parameters are blocking after whole-scope triage and positive canaries. Required callback and virtual signatures retain their contracts; the unused-parameter check uses its conservative default configuration. Include-cleaner is blocking for `libs/engine/`, `libs/capability/` and `libs/update/` through the Rust rule metadata. Application and other layers remain advisory: Qt meta-object generation and public umbrella headers need contextual include review. The global `.clang-tidy` file leaves include-cleaner advisory because it cannot express this path-scoped verdict.

The first-party `exosnap::warnings` target applies `/W4 /WX /sdl /utf-8` in MSVC Debug and Release builds.
`/sdl` adds security diagnostics and code-generation protections. `/utf-8` fixes source and execution
character sets independently of the host code page. It does not replace UTF-8/UTF-16 conversions at
Windows API boundaries. Dependencies keep their own target policies rather than inheriting global flags.

Explicit unhandled-enumerator C4062 enablement supplements `/W4 /WX`. C4061 for switches with a
`default` stays separate. Keep exhaustive policy switches genuinely exhaustive rather than adding
a default that hides a new enum case.

## Add a blocking check only with two proofs

First run it over all relevant project translation units and triage every repository-owned finding. A blocking check must have a clean actionable baseline, not a tree-wide suppression that hides its subject.

Second add a deliberate violation under `tools/exo-dev/tests/fixtures/lint-canaries` and prove `cargo exo-dev lint canaries` rejects it. Zero findings can mean either clean code or a check that never executed. A canary proves the instrument runs; the tree scan proves the repository satisfies it. Neither replaces the other.

Record run-specific counts, commands and analysis with review evidence. Promote only the rule and durable rationale into configuration or this guide. Count distinct `(file, line, column, check)` sites rather than raw diagnostic lines, and compare runs only when their input sets and tool versions match.

Include-cleaner checks direct standard and project dependencies. Its Windows SDK provider exception preserves public umbrella headers instead of recommending internal SDK headers for Win32 declarations. Positive fixtures retain missing standard and project providers under the same configuration. The check analyzes the main translation unit, so its clean result does not by itself prove every header is self-contained.

## Manual include-cleaner repairs

`cargo exo-dev lint clang-tidy` is read-only and has no `--fix` or `--apply` option. Hooks and CI do not apply clang-tidy replacements. Debug CI runs configure, `all_qmllint`, build, blocking clang-tidy, then the complete CTest suite excluding `live`. The completed build supplies Ninja dependency data for changed-header scope. Blocking analysis failures stop before CTest.

For a narrow repair, first run the read-only gate against the configured and built Ninja tree. `--base HEAD` selects uncommitted changes against HEAD. Use another explicit revision when reviewing committed changes:

```powershell
cargo exo-dev lint clang-tidy --build-dir build/windows-x64-ninja-debug --base HEAD --jobs 1
```

The output reports the resolved `clang-tidy` executable. Use that exact path for a native one-off export, preserving compatibility with the compilation database's Visual Studio compiler. Select one first-party translation unit explicitly. Run from the repository root in the same compiler environment and finish each export/review before starting another. For example, substitute the reported executable path below:

```powershell
$tidy = Read-Host 'Exact clang-tidy executable path reported by exo-dev'
$fixes = Join-Path $env:TEMP ('exosnap-includes-' + [guid]::NewGuid().ToString() + '.yaml')
& $tidy -p build/windows-x64-ninja-debug '--checks=-*,misc-include-cleaner' '--warnings-as-errors=-*' '--extra-arg=-D_CRT_USE_BUILTIN_OFFSETOF' "--export-fixes=$fixes" libs/engine/src/edit_timeline_compositor.cpp
Get-Content -LiteralPath $fixes
```

This exports proposed replacements without changing source. A compiler error or failed analysis is not a valid repair proposal. Keep the repository configuration, including its Windows SDK provider exception. Inspect every replacement's `FilePath`, byte offset, length and replacement text against the current file. Apply only individually reviewed include edits to the explicitly selected project translation unit with an editor. Preserve existing dirty edits and reject proposals for headers, other translation units, SDK, vendor, generated or build files. If source changes after export, regenerate the proposal before applying it.

The Windows runner requests the UCRT builtin `offsetof` implementation for Clang. Its pointer-cast fallback is not a C++ constant expression in that frontend, so valid layout assertions can otherwise prevent analysis. This preserves the assertions and blocking rules; it does not change production compiler flags. Analysis cache identity includes this option.

Neither `--header-filter` nor a selected translation unit bounds native `--fix` writes. Do not pass `--fix`, `--fix-errors` or a directory of replacements to an automatic applier here. A bounded Rust apply subcommand that validates replacement paths, source identity and overlapping edits is not implemented. Until that exists, applying replacements is an explicit manual edit. Never stage or commit automatically.

Inspect `git diff` after editing, then rerun the read-only blocking gate with the same build directory and base. A fix-it proposal does not prove correctness or replace the blocking check.

## Advisory reports

Run `cargo exo-dev lint quality --only clang-tidy --report-path build/advisory/clang-tidy.txt` against a configured Ninja build. The runner selects actual repository translation units from `compile_commands.json`, excludes generated and vendored sources, and retains every batch's output. It writes raw text, normalized JSON and a Markdown summary beside the requested report path. The identity is `(repository-relative file, line, column, check)` with tracked path spelling and case-insensitive lookup on Windows. Reports record commit, dirty-tree status, tool version, translation-unit count, timing, raw and unique counts, and per-check/per-layer counts. Compare matching tool versions and input scopes.

The nightly/manual workflow uploads all three artifacts even when analysis fails. A missing tool, missing compilation database, compiler error or malformed diagnostic is an infrastructure failure, not an empty successful report. Advisory findings themselves do not fail the run.

Select a frontend compatible with the compilation database's MSVC standard library using `--clang-tidy <executable>` when the default LLVM installation is older. The advisory invocation removes compiler warning promotion only for measurement; production `/WX` and `-Werror` remain unchanged. The pinned PresentMon dependency carries a small build patch qualifying enum types hidden by descriptor member names and correcting include-path casing, so Clang can parse the same provider implementation.

Cognitive complexity remains advisory. The report ranks up to 20 diagnosed functions above clang-tidy's threshold of 25; device enumeration, native API setup and explicit state machines require contextual review. `bugprone-easily-swappable-parameters` has a separate public Engine header report. With `--base`, that report is restricted to changed public headers; a full scan reports findings located in public headers. Diagnostics located only at out-of-line definitions are outside this header-focused report. Neither diagnostic is a global blocking rule.

Both CI analysis workflows provision cppcheck through `cargo exo-dev setup cppcheck`, pinned to the Chocolatey package version in `tools/exo-dev/src/setup.rs`. Provisioning validates the executable's version and fails if installation or discovery fails. `cargo exo-dev lint quality --only cppcheck-unused-function --report-path build/advisory/cppcheck.txt` measures unused-function candidates without turning findings into failures. Qt slots and registered callbacks require review before removal.

## Windows compiler compatibility

Windows analysis supplies `_CRT_USE_BUILTIN_OFFSETOF=1` to select the UCRT builtin
`offsetof`, matching MSVC's predefined macro. Older Clang versions otherwise
reject valid constant-expression layout assertions. Clang 21 and later define
this macro themselves, as documented in the
[LLVM release notes](https://releases.llvm.org/21.1.0/tools/clang/docs/ReleaseNotes.html).
This argument contributes to the verdict identity and does not disable diagnostics
or change native build flags.

## Missing tools and caches

A requested but missing analysis tool yields exit 3 from the quality runner. `cargo exo-dev verify` reports `TOOL_MISSING`; `--full` fails, `--fast` can report the gap and continue. Install the missing tool rather than interpreting absence as success.

clang-tidy caches successful analysis output by the translation unit's complete recorded input set.
The key includes every compile-command variant, source/header content, analyzer configuration,
tool version and cache schema. Ninja records must have valid status, exact dependency counts,
all object variants and current object commands before they can support reuse.
Missing input coverage causes fresh analysis. Header membership and content are part of the global identity,
including repository vendor include directories, so a newly available conditional header cannot
reuse a verdict from the previous include search. Content hashing also covers optional or shadowing
headers not yet represented in an older Ninja graph. This conservatively invalidates all unit keys
when a repository header changes, rather than assuming the older graph proves include selection.
Dependency paths that ambiguously belong to
another checkout are rejected rather than remapped.
Checkout ownership and header consumers compare physically proven Windows path aliases
without changing recorded dependency paths.

Ninja omits installed MSVC/SDK headers from its MSVC dependency records.
Their content trees, compiler identity and include environment therefore contribute one global
fingerprint, rechecked before fresh results are cached. Use a matching Developer PowerShell for direct lint invocations.
Without an explicit INCLUDE environment or readable supported toolchain inputs, verdicts are
not reused. The CI job initializes that environment before building and analysing. Header scoping includes consumers with incomplete
coverage rather than reporting an empty successful scope. CMake/preset changes select the full tree,
but selection does not force fresh analysis of every unit. After configuring and building the changed
tree, adding a target with only a new source preserves cache keys for existing units whose compiler
commands and recorded inputs remain unchanged. Changed definitions, compiler options or include
paths change the affected units' keys. A stale or unconfigured Ninja graph cannot support reuse.
Repository header edits still invalidate the global identity, including when the same change adds
a target. A full selection with no cache hits must not be attributed to CMake selection alone.
Added, removed or renamed headers select the full tree and reload current dependency records.
The header-membership fingerprint invalidates earlier verdicts when include search changes,
while later runs with the same verified inputs can reuse the new results.
Scoping and cache-key construction share one initial ownership/header snapshot. Readable directory
links are traversed with cycle detection, and physical link targets contribute to membership identity.
Deleted registered worktrees are ignored; unreadable existing roots remain unavailable evidence.
Build-directory aliases and UNC roots retain their filesystem meaning during target matching.
The verification runner passes its imported MSVC environment to both analysis and identity hashing.
A disabled cache prints the unavailable evidence category.

Cached entries carry their schema, source, key, exit status and output digest.
Before storing any fresh result, the runner rechecks compilation-database bytes,
configuration, header membership, toolchain identity and the captured source/dependency keys.
Fresh work is checkpointed in batches of at least 16 units or four worker waves, whichever is larger.
Changed or unreadable evidence aborts the current batch before its cache writes. Earlier validated
batches remain reusable under their original keys; a changed input produces a different key.
Full toolchain revalidation is retained, including selected cache hits, rather than trusting an old
snapshot across analysis. This deliberately costs additional reads to detect persistent input drift.
Schema 4 rejects entries from producers that did not perform this validation.
Keep analysis inputs stable throughout the run. Build locks do not prevent editor changes.
Before/after checks detect persistent drift but cannot detect an edit and restoration during
analysis, or make sequential file reads immutable. Immutable input snapshots are not provided.
Old, malformed or damaged entries are misses. The digest detects corruption, not forgery by someone
already able to write a trusted cache scope. GitHub cache ref isolation remains the trust boundary.
CI restores and saves the verdict cache in separate steps. Validated diagnostics are saved
also when they make the analysis fail, and replaying a blocking finding still fails the check.
An interrupted analysis retains completed, validated checkpoints locally. Work in the unfinished
batch must run again. CI reuse still depends on its separate cache upload finishing, which a hard
job timeout does not guarantee. Cache upload failures do not replace the analysis verdict.
cppcheck keeps its separate build-directory cache.

Shared tool caches live under %LOCALAPPDATA%\ExoSnap\tool-cache, outside source/build trees.
Use a new --cache-dir for an isolated cold clang-tidy measurement.
Full runs the whole tree. Fast scopes the pass to affected translation units where supported.

### Measure blocking analysis costs

The --profile-path option to cargo exo-dev lint clang-tidy writes a JSON profile
with HEAD/dirty state, tool version, compilation-database SHA-256, selected-source count,
cache counts, fresh-worker count and pipeline wall times.
Each fresh translation unit records wall time and, on Windows when available, process CPU time
and peak working-set bytes. Missing resource measurements are null.
The verification runner also retains clang-tidy-profile.json beside its step logs.
An optional profile failure emits a warning and preserves the analysis diagnostics and verdict.
The Debug CI job uploads that profile on successful and failing runs when analysis produced it.
It sets EXO_DEV_CLANG_TIDY_PROFILE=public: the uploaded profile omits tool, compilation-database
and source paths, classifies scope without free-form details, and identifies fresh units by the
same queue positions as progress records. Versions, digests, counts, timings and resources remain.
Without that setting, local rich profiles retain their existing path fields.

Set EXO_DEV_CLANG_TIDY_PROGRESS=1 for incremental parent-process progress on stderr.
The Debug CI verification step enables it. Other invocations leave it off by default.
Each EXO_DEV_CLANG_TIDY_PROGRESS line contains escaped JSON for a phase start,
tool-version discovery, the input-ready counts/timings, or a translation-unit start/completion.
Translation units use one-based positions in the fresh-work queue. Completions include
completed/total counts and existing wall, CPU and working-set measurements.
These records contain no source paths, compiler commands or defines, and survive in the
GitHub Actions step log even when cancellation prevents a final profile. They do not constitute an analysis verdict.

Discovery, version detection, database parsing, configuration/toolchain identity, git scope, header expansion, Ninja extraction/parsing,
freshness checks, hashing, cache lookup, fresh subprocess execution, input revalidation, validation/storage and reporting
are distinct measurements. Scope/dependency totals include their detailed subphases, so do not sum
nested counters. Per-process CPU time is additive. Per-process memory peaks are not a simultaneous
aggregate memory peak. GitHub restore/save durations come from workflow logs, outside this profile.

Compare warm, source-change, header-change and cold-cache runs with the same toolchain and declared
input set. Preserve all compilation variants when making a representative sample.
Alternate worker counts and repeat measurements on an idle host before changing defaults.
A high hit rate alone does not identify which phase consumes wall time.

For a large selection of at least 1,000 current objects, dependency extraction reads the
whole Ninja dependency log once. Smaller selections retain batches of at most 100 objects.
The parser still admits only selected source/object variants with complete valid records.
Large freshness selections share the default Ninja command plan, with targeted fallback for
objects outside that plan. Counted dry-run frames must describe the entire scheduled plan.
Debug Rust builds optimize the sha2 dependency while retaining the same SHA-256 algorithm.
The threshold is conservative for the measured large workload, not a claim of universal optimality.

The ignored compare_ninja_dependency_strategies Rust test compares both extraction strategies
in seven alternating pairs and requires identical parsed dependency coverage.
Set EXO_DEV_DEPENDENCY_BUILD_DIR to a configured Ninja tree and EXO_DEV_DEPENDENCY_REPORT
to a JSON output path, then run cargo test --locked --manifest-path tools/Cargo.toml
-p exo-dev --lib compare_ninja_dependency_strategies -- --ignored --nocapture.
This measures extraction/parsing, not analysis correctness or reusable cache verdicts.

Compiler caching is distinct. Use `sccache --show-stats` after a build to inspect the active server. Size its cache for the number of build configurations and worktrees in use; a fixed historical hit rate is not a guarantee. A changed absolute source path can change cache identity. Do not enable path rewriting merely for hits until crash symbolication against the resulting PDB has been verified.

`SCCACHE_BASEDIRS` accepts absolute roots separated by `;` on Windows, as described in [sccache's configuration reference](https://github.com/mozilla/sccache/blob/main/docs/Configuration.md). Normalizing cache keys does not establish that the object is independent of its source root. A reused object can retain another checkout's `__FILE__` strings and embedded debug paths. Check observable paths, compiler arguments, generated inputs and symbolication before adopting cross-root normalization. Keep the default unchanged when those identities differ. Measure experiments with a separate server port and cache directory so active builds retain their configuration and statistics.

```powershell
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', '20G', 'User')
sccache --stop-server
```

That changes a machine preference, not repository policy. The next server reads it. Preserve embedded `/Z7` object debug information and the release linker's PDB generation; a faster cache that prevents diagnosing crashes is not an improvement.
