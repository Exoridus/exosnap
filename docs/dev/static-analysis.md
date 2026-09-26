# Static analysis

`.clang-tidy`, `cmake/exosnap_warnings.cmake` and the quality scripts own the enabled rules. This guide explains how to run and extend them. Historical scan counts are not current baselines.

## Blocking and advisory paths

| Pass | Status | Entry point |
|---|---|---|
| Selected clang-tidy checks | Blocking | `scripts/run-clang-tidy-blocking.ps1`, the `clang-tidy` step of `cargo exo-dev verify` and of CI's `ci-build-debug` profile |
| cppcheck warning/performance/portability | Blocking locally | `scripts/check-quality.ps1`, the `cppcheck` step of `cargo exo-dev verify`; no CI profile runs it yet |
| Broad clang-tidy advisory set | Advisory | `advisory-checks.yml` |
| cppcheck unused-function analysis | Advisory | `advisory-checks.yml` |

clang-tidy compilation integration requires Ninja; the Visual Studio generator does not run `CMAKE_CXX_CLANG_TIDY`. Use the lint preset or the explicit compile-database runner rather than interpreting a Visual Studio build as a tidy pass.

The blocking set includes use-after-move, dangling handles, misleading indentation and the selected analyzer call/uninitialized/allocation checks. Read `.clang-tidy` and the blocking runner for exact patterns. Qt meta-object use and registered OS callbacks require special care when triaging apparent unused symbols; broad automated removal is unsafe.

MSVC `/W4 /WX` is supplemented by explicit unhandled-enumerator C4062 enablement. C4061 for switches with a `default` stays separate. Keep exhaustive policy switches genuinely exhaustive rather than adding a default that hides a new enum case.

## Add a blocking check only with two proofs

First run it over all relevant project translation units and triage every repository-owned finding. A blocking check must have a clean actionable baseline, not a tree-wide suppression that hides its subject.

Second add a deliberate violation under `scripts/tests/fixtures/lint-canaries` and prove `scripts/check-lint-canaries.ps1` rejects it. Zero findings can mean either clean code or a check that never executed. A canary proves the instrument runs; the tree scan proves the repository satisfies it. Neither replaces the other.

Record run-specific counts, commands and analysis with review evidence. Promote only the rule and durable rationale into configuration or this guide. Count distinct `(file, line, column, check)` sites rather than raw diagnostic lines, and compare runs only when their input sets and tool versions match.

## Missing tools and caches

A requested but missing analysis tool yields exit 3 from the quality runner. `cargo exo-dev verify` reports `TOOL_MISSING`; `--full` fails, `--fast` can report the gap and continue. Install the missing tool rather than interpreting absence as success.

clang-tidy caches by the translation unit's recorded input set. cppcheck uses its build-directory cache. Shared tool caches live under `%LOCALAPPDATA%\ExoSnap\tool-cache`, outside source/build trees. Delete that directory for a deliberate cold run. Full runs the whole tree; Fast scopes the pass to affected translation units where supported.

Compiler caching is distinct. Use `sccache --show-stats` after a build to inspect the active server. Size its cache for the number of build configurations and worktrees in use; a fixed historical hit rate is not a guarantee. A changed absolute source path can change cache identity. Do not enable path rewriting merely for hits until crash symbolication against the resulting PDB has been verified.

```powershell
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', '20G', 'User')
sccache --stop-server
```

That changes a machine preference, not repository policy. The next server reads it. Preserve embedded `/Z7` object debug information and the release linker's PDB generation; a faster cache that prevents diagnosing crashes is not an improvement.
