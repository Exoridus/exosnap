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

MSVC `/W4 /WX` is supplemented by explicit unhandled-enumerator C4062 enablement. C4061 for switches with a `default` stays separate. Keep exhaustive policy switches genuinely exhaustive rather than adding a default that hides a new enum case.

## Add a blocking check only with two proofs

First run it over all relevant project translation units and triage every repository-owned finding. A blocking check must have a clean actionable baseline, not a tree-wide suppression that hides its subject.

Second add a deliberate violation under `tools/exo-dev/tests/fixtures/lint-canaries` and prove `cargo exo-dev lint canaries` rejects it. Zero findings can mean either clean code or a check that never executed. A canary proves the instrument runs; the tree scan proves the repository satisfies it. Neither replaces the other.

Record run-specific counts, commands and analysis with review evidence. Promote only the rule and durable rationale into configuration or this guide. Count distinct `(file, line, column, check)` sites rather than raw diagnostic lines, and compare runs only when their input sets and tool versions match.

Include-cleaner checks direct standard and project dependencies. Its Windows SDK provider exception preserves public umbrella headers instead of recommending internal SDK headers for Win32 declarations. Positive fixtures retain missing standard and project providers under the same configuration. The check analyzes the main translation unit, so its clean result does not by itself prove every header is self-contained.

## Advisory reports

Run `cargo exo-dev lint quality --only clang-tidy --report-path build/advisory/clang-tidy.txt` against a configured Ninja build. The runner selects actual repository translation units from `compile_commands.json`, excludes generated and vendored sources, and retains every batch's output. It writes raw text, normalized JSON and a Markdown summary beside the requested report path. The identity is `(repository-relative file, line, column, check)` with tracked path spelling and case-insensitive lookup on Windows. Reports record commit, dirty-tree status, tool version, translation-unit count, timing, raw and unique counts, and per-check/per-layer counts. Compare matching tool versions and input scopes.

The nightly/manual workflow uploads all three artifacts even when analysis fails. A missing tool, missing compilation database, compiler error or malformed diagnostic is an infrastructure failure, not an empty successful report. Advisory findings themselves do not fail the run.

Select a frontend compatible with the compilation database's MSVC standard library using `--clang-tidy <executable>` when the default LLVM installation is older. The advisory invocation removes compiler warning promotion only for measurement; production `/WX` and `-Werror` remain unchanged. The pinned PresentMon dependency carries a small build patch qualifying enum types hidden by descriptor member names and correcting include-path casing, so Clang can parse the same provider implementation.

Cognitive complexity remains advisory. The report ranks up to 20 diagnosed functions above clang-tidy's threshold of 25; device enumeration, native API setup and explicit state machines require contextual review. `bugprone-easily-swappable-parameters` has a separate public Engine header report. With `--base`, that report is restricted to changed public headers; a full scan reports findings located in public headers. Diagnostics located only at out-of-line definitions are outside this header-focused report. Neither diagnostic is a global blocking rule.

Both CI analysis workflows provision cppcheck through `cargo exo-dev setup cppcheck`, pinned to the Chocolatey package version in `tools/exo-dev/src/setup.rs`. Provisioning validates the executable's version and fails if installation or discovery fails. `cargo exo-dev lint quality --only cppcheck-unused-function --report-path build/advisory/cppcheck.txt` measures unused-function candidates without turning findings into failures. Qt slots and registered callbacks require review before removal.

## Missing tools and caches

A requested but missing analysis tool yields exit 3 from the quality runner. `cargo exo-dev verify` reports `TOOL_MISSING`; `--full` fails, `--fast` can report the gap and continue. Install the missing tool rather than interpreting absence as success.

clang-tidy caches by the translation unit's recorded input set. cppcheck uses its build-directory cache. Shared tool caches live under `%LOCALAPPDATA%\ExoSnap\tool-cache`, outside source/build trees. Delete that directory for a deliberate cold run. Full runs the whole tree; Fast scopes the pass to affected translation units where supported.

Compiler caching is distinct. Use `sccache --show-stats` after a build to inspect the active server. Size its cache for the number of build configurations and worktrees in use; a fixed historical hit rate is not a guarantee. A changed absolute source path can change cache identity. Do not enable path rewriting merely for hits until crash symbolication against the resulting PDB has been verified.

```powershell
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', '20G', 'User')
sccache --stop-server
```

That changes a machine preference, not repository policy. The next server reads it. Preserve embedded `/Z7` object debug information and the release linker's PDB generation; a faster cache that prevents diagnosing crashes is not an improvement.
