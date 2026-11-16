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

`cargo exo-dev lint clang-tidy` is read-only and has no `--fix` or `--apply` option. Hooks and CI do not apply clang-tidy replacements. Debug CI runs configure, focused QML contracts, `all_qmllint`, the complete build, blocking clang-tidy, then the complete CTest suite excluding `live`. The completed build supplies Ninja dependency data for changed-header scope. Blocking analysis failures stop before CTest.

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

The nightly/manual workflow uploads all three artifacts even when analysis fails. On Monday to Saturday the schedule skips a `main` commit that already has a successful advisory run. The Sunday run and every manual dispatch analyse unconditionally. A missing tool, missing compilation database, compiler error or malformed diagnostic is an infrastructure failure, not an empty successful report. Advisory findings themselves do not fail the run.

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

clang-tidy caches successful analysis output in two levels, like a direct-mode compiler cache.
A unit's lookup key covers every compile-command variant, the source content and a global salt.
The salt covers analyzer flags, tool version, cache schema, every `.clang-tidy` that can configure a project unit,
repository header membership, include lookup directory membership and the toolchain identity.
The lookup key selects stored [dependency manifests](#dependency-manifests).
A manifest lists every file clang opened for the unit with its content digest and physical target.
A verdict replays only through a manifest whose files all still match.
A header content edit therefore re-analyzes exactly the units that opened that header,
while every other unit keeps its verdict. Ninja's records are no longer part of any key.

Header contents leave the global identity, but which files exist does not.
A `__has_include` probe, a failed lookup or a shadowing file in an earlier search directory
can change the analysis without opening a file that was opened before.
Repository header membership covers names and directory/file links in
`app`, `apps`, `libs`, `tools`, `tests` and `third_party`, including untracked headers.
Lookup membership covers every name and link below the directories an include lookup searches:
every `-I`, `/I`, `-external:I`, `-isystem`, `-imsvc`, `-iquote`, `-idirafter` and
`--include-directory` directory of a project unit, and the directories of the project sources.
That includes generated and fetched build-tree headers and external installations such as Qt.
Any extension counts, except compiler and linker outputs such as `.obj`, `.pdb` and `.exe`,
because a manifest that opened or probed one is refused.
Installed MSVC/SDK include trees, the compiler identity and the include environment remain one global
content fingerprint. Without an explicit INCLUDE environment or readable supported toolchain inputs,
verdicts are not reused. Use a matching Developer PowerShell for direct lint invocations.
The CI job initializes that environment before building and analysing.
Every `.clang-tidy` in a directory between a project translation unit and the repository root
belongs to the global configuration identity, so adding, editing or removing one invalidates earlier verdicts.

Any membership change invalidates every lookup key, as a header content change did before.
Added, removed or renamed files below a lookup directory and a new search directory, such as a new target's
generated include directory, therefore re-analyze the full selection once.
A unit whose command adds a response file, an environment include option, `/X`, `-nostdinc`,
a sysroot, a VFS overlay or another lookup option outside the search directories is not keyed.
A manifest is refused, and an existing one rejected, when an opened file lies outside every covered
root or may belong to another registered checkout. Covered roots are the lookup directories,
the toolchain roots and the clang-tidy installation, whose resource headers its version identifies.
A manifest is also refused when an opened file contains an include or `__has_include` lookup that could
probe outside the covered roots: an absolute name outside them, or a parent-relative name that is not
found beside its includer and leaves the roots from any directory it is searched in.
A macro that wraps `__has_include`, directly or through another such macro, is followed to its invocations
in every opened file, and each invocation is checked like the builtin probe.
An include name computed from a macro is refused unless the file belongs to the toolchain identity.
The exception is a computed `#include` in a branch no run can enter: the first branch of `#ifdef NAME` or a later branch of `#ifndef NAME`, where `NAME` starts with an upper-case letter, is named by no compile command, analysis flag, `.clang-tidy` file (`ExtraArgs` included), `CL` or `_CL_` value, or other opened file, is not defined earlier in its own file, and is defined later only when `#pragma once` rules out a second entry.
A `#define` that mentions `NAME` anywhere, including its parameters and body, counts as a definition.
Such a line is decided by the manifest's own files, the command and the keyed configuration, so a later definition of `NAME` changes one of those and misses.
Clang's MSVC-target predefined macros are underscore-prefixed, which is why an upper-case name starts out undefined.
Any other condition, a comment or raw string the scanner cannot follow, alternative directive spellings (`%:` digraph or `??=` trigraph), or unbalanced conditionals keep the refusal.
A probe for a build artifact name is refused, because lookup membership leaves those names out.
Checkout ownership and header consumers compare physically proven Windows path aliases
without changing recorded paths. Readable directory links are traversed with cycle detection,
and physical link targets contribute to membership.
Deleted registered worktrees are ignored; unreadable existing roots disable reuse.
Build-directory aliases and UNC roots retain their filesystem meaning during target matching.
The verification runner passes its imported MSVC environment to both analysis and identity hashing.
A disabled cache prints the unavailable evidence category.

A scoped run selects changed translation units, the canary units for analysis configuration
changes and the units below a changed nested `.clang-tidy`. A changed header anywhere, or any other
changed file below a lookup directory, is an include input. Resource files such as QML, images, JSON
and Markdown are not, so editing them selects nothing. Ninja's records cannot bound its effect,
because they miss includes only clang takes, such as headers behind `#ifdef __clang__`.
With a usable cache, such a change selects every unit, and only those whose manifests no longer match
run again. Without one, the scope falls back to Ninja's recorded consumers, and to the full tree when
none is recorded. Header scoping includes consumers with incomplete Ninja coverage rather than
reporting an empty successful scope. CMake/preset changes select the full tree, but selection does
not force fresh analysis. After configuring and building the changed tree, adding a target with only
a new source preserves lookup keys for existing units whose commands remain unchanged.
Changed definitions, compiler options or include paths change the affected units' keys.
A full selection with no cache hits must not be attributed to CMake selection alone.
Scoping and cache-key construction share one initial ownership/header snapshot.

Verdict entries carry their schema, source, verdict key, exit status and output digest.
Before storing any fresh result, the runner rechecks compilation-database bytes, configuration,
both memberships, toolchain identity, every lookup key and every file a lookup or manifest has read.
A file first read for a manifest must also be older than the analysis start.
Fresh work runs in one continuous worker pool. A worker starts the next queued unit as soon as its
previous one ends, so a slow unit does not hold back the rest of the queue.
Completed results are checkpointed after at least 16 units or four worker waves, whichever is larger,
while the workers continue. A checkpoint stores only results that completed before its revalidation began.
Changed or unreadable evidence aborts the run before that checkpoint's cache writes, stops queued units
from starting and discards every result not yet checkpointed. Earlier checkpoints remain reusable
under their original keys. A changed input produces a different key.
Full toolchain revalidation is retained, including selected cache hits, rather than trusting an old
snapshot across analysis. This deliberately costs additional reads to detect persistent input drift.
Schema 6 rejects verdicts and manifests produced under earlier include-admission rules, including previous direct-cache and schema-5 entries. The first run after the upgrade is cold once.
Keep analysis inputs stable throughout the run. Build locks do not prevent editor changes.
Before/after checks detect persistent drift but cannot detect an edit and restoration during
analysis, or make sequential file reads immutable. A copy that preserves modification times
also evades the age check. Immutable input snapshots are not provided.
Old, malformed or damaged entries and manifests are misses. The digests detect corruption, not forgery by someone
already able to write a trusted cache scope. GitHub cache ref isolation remains the trust boundary.
CI restores and saves the verdict cache in separate steps. Validated diagnostics are saved
also when they make the analysis fail, and replaying a blocking finding still fails the check.
An interrupted analysis retains completed, validated checkpoints locally. Results not yet
checkpointed must run again. CI reuse still depends on its separate cache upload finishing, which a hard
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
A manifest-refused record names the unit, the reason and the one file or lookup that caused it,
so a refusal in a CI environment can be diagnosed from the log. Apart from that, these records
contain no source paths, compiler commands or defines, and survive in the
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

### Dependency manifests

Blocking clang-tidy runs ask clang to report every file its preprocessor opens
(`-Xclang --show-includes -Xclang -sys-header-deps`). This is a by-product of the analysis, not a second parse.
The runner removes those notes before it validates, caches or reports output.
Verdicts are therefore byte-identical to a run without the flags.

After checkpoint revalidation, a fresh unit's manifest is written to
`<lookup key shard>/<lookup key>/<verdict key>.deps.json`, and its verdict to `<verdict key shard>/<verdict key>.txt`.
The manifest lists the source, every file clang opened for any of the unit's compile commands and every forced include,
each with its SHA-256, its root (`repository`, `build` or `absolute`), a root-relative or absolute path,
how it was reached (`source`, `include` or `forced`), and the physical target when a link or junction resolves elsewhere.
Short 8.3 spellings do not count as a different target.
It also records its own schema, the lookup and verdict keys, cache schema, tool version, observation flags,
roots, compile-command count and command-identity digest.
The verdict key is the SHA-256 of the lookup key and every file's root, path, digest and target.
One lookup key can hold manifests for several header states, such as a pull request and `next`.

A lookup reads each manifest under the unit's lookup key. It rejects the manifest when any recorded field
differs from the current run, when the verdict key does not match the listed files, or when a listed file
is missing, changed, retargeted, uncovered or owned by another checkout. The first matching manifest's verdict replays.
A manifest is written only when complete. A noted file that is missing, unreadable, uncovered, a build artifact or modified after analysis began,
an include lookup that could probe outside the covered roots,
an unresolvable forced include, a response file, an ambiguous relative note or a unit without include notes
leaves the fresh verdict uncached and is counted by reason.
A probe for a missing file, such as `__has_include`, opens nothing. Membership covers it instead.

The profile's `dependency_manifests` object counts lookup candidates, rejected candidates by reason,
recorded, incomplete and unkeyed units, and compares clang's files with each unit's Ninja record.
`clang_only` counts files clang opened that Ninja did not record.
Installed MSVC/SDK headers are expected there under `absolute`. `ninja_only` counts recorded files clang did not open.
Local profiles also list the distinct project files on either side. Public profiles keep only the counts.
The ignored manifest_keys_replay_every_unit_whose_opened_files_are_unchanged Rust test runs this
path end to end with cmake, Ninja, the MSVC toolchain and Visual Studio clang-tidy.

Compiler caching is distinct. Use `sccache --show-stats` after a build to inspect the active server. Size its cache for the number of build configurations and worktrees in use; a fixed historical hit rate is not a guarantee. A changed absolute source path can change cache identity. Do not enable path rewriting merely for hits until crash symbolication against the resulting PDB has been verified.

`SCCACHE_BASEDIRS` accepts absolute roots separated by `;` on Windows, as described in [sccache's configuration reference](https://github.com/mozilla/sccache/blob/main/docs/Configuration.md). Normalizing cache keys does not establish that the object is independent of its source root. A reused object can retain another checkout's `__FILE__` strings and embedded debug paths. Check observable paths, compiler arguments, generated inputs and symbolication before adopting cross-root normalization. Keep the default unchanged when those identities differ. Measure experiments with a separate server port and cache directory so active builds retain their configuration and statistics.

```powershell
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', '20G', 'User')
sccache --stop-server
```

That changes a machine preference, not repository policy. The next server reads it. Preserve embedded `/Z7` object debug information and the release linker's PDB generation; a faster cache that prevents diagnosing crashes is not an improvement.
