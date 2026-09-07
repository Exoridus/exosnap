# Static analysis: how the blocking clang-tidy set was chosen

`.clang-tidy` carries the rules and the durable reasons for them. This file
carries the measurement they rest on, so that extending the blocking set is a
decision with evidence rather than a guess.

clang-tidy only runs under the Ninja generator; the Visual Studio generator
silently ignores `CMAKE_CXX_CLANG_TIDY`. The CI lint job and the
`windows-x64-ninja-lint` preset run it. `scripts/run-clang-tidy-blocking.ps1`
enforces the blocking subset and, with `-Base <ref>`, restricts the pass to the
translation units a change actually touches.

## The blocking set

```
bugprone-use-after-move
bugprone-dangling-handle
readability-misleading-indentation
clang-analyzer-core.CallAndMessage
clang-analyzer-core.uninitialized.*
clang-analyzer-cplusplus.NewDelete*
```

A check may fail a build only after a full pass over every project translation
unit reports zero findings in repository-owned files. The other five were
measured that way across 509 translation units. Do not extend the list without
repeating that pass and recording the result here.

`readability-misleading-indentation` qualified on the whole-tree pass recorded
below: zero findings across all 1012 tracked sources. It is the direct form of
the risk brace-less single statements are usually argued about -- Apple's
CVE-2014-1266, where a duplicated `goto fail;` sat outside a brace-less `if`,
ran unconditionally and skipped a TLS signature check. Braces are a proxy for
that; this check is the thing itself, and MSVC has no equivalent at any warning
level, so clang-tidy is the only place it can be caught in this build. It cost
no churn to adopt: the code was already clean.

`bugprone-use-after-move` was the one entry that did not start clean: three
findings on the qualifying pass, all resolved at the source rather than
suppressed wholesale. Two were loop-carried handles in `VideoThread::Run`, now
cleared explicitly after the move; the third was a deliberate moved-from
assertion in a test, which carries a `NOLINT` naming its reason.

## Candidates that did not qualify

Finding counts are from the same pass.

| Check | Findings | Why it stayed out |
|---|---|---|
| `bugprone-narrowing-conversions`, `cppcoreguidelines-narrowing-conversions` | 156 | `qsizetype` to `int` at Qt call sites, tree-wide |
| `bugprone-integer-division` | 5 | deliberate integer pixel math (4:2:0 chroma viewport, Matroska timescale) |
| `clang-diagnostic-switch` (enum exhaustiveness) | 2 | resolved at the compiler instead, see below |

Enum-switch exhaustiveness is now the compiler's job. The build uses `/W4 /WX`,
but MSVC keeps C4062 (unhandled enumerator, no default label) and C4061
(unhandled enumerator, default label present) off at every warning level; they
need an explicit `/w44062` / `/w44061`. `cmake/exosnap_warnings.cmake` raises
C4062, which makes /WX enforce it on every translation unit rather than only
where clang-tidy runs. It turned up exactly the two sites the clang-tidy pass had
found -- `ToString(PipelineBottleneck)` had no case for `Gpu` and reported the
newest bottleneck classification as "Unknown", and the updater's visual-proof
scenario builder had no case for `FailureCase::TargetVersionMismatch`.

C4061 stays off. A `switch` that carries a `default:` has already said what
happens to the enumerators it does not name.

## The advisory volume, measured

The whole-tree advisory pass had only ever been reported as a count of
diagnostic LINES. That number is not a count of code: a finding in a widely
included header is repeated once per translation unit that includes it, and a
macro expansion adds context lines of its own.

Measured properly -- one pass over every tracked `.cpp`/`.h` under `app/`,
`apps/`, `libs/`, `tests/` and `tools/` (1012 files, 29 min wall clock, clang-tidy
22.1.0, `--checks=-clang-analyzer-*`, the invocation `scripts/check-quality.ps1`
uses for its whole-tree form), counting each distinct (file, line, column, check)
tuple in a repository-owned file once:

| | sites |
|---|---|
| diagnostic lines | 20987 |
| **distinct sites** | **21486** |

Distinct sites exceed diagnostic lines because one diagnostic can name several
checks (`modernize-avoid-c-arrays` and `cppcoreguidelines-avoid-c-arrays` are the
same finding under two names), and each is a rule that would have to be answered
separately.

Four rules accounted for four fifths of it: `misc-include-cleaner` (9444),
`readability-braces-around-statements` (3958),
`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` (2862) and the
`avoid-c-arrays` pair (1120). The rest of this file records what happened to
each.

## Advisory checks

`misc-include-cleaner`, `misc-unused-using-decls`, `misc-unused-parameters`,
`misc-unused-alias-decls` and `readability-redundant-declaration` are enabled but
excluded from `WarningsAsErrors`. All five produce Qt meta-object and moc
false-positives. Promote one only after a human triage pass over its findings.

`cppcheck --enable=unusedFunction` is advisory for the same reason: its findings
are dominated by Qt slots reached through `QMetaObject` and by callbacks Windows
registers. It also needs a whole-program pass that cannot share the per-check
analysis the blocking gate does.

Both advisory passes run nightly in `.github/workflows/advisory-checks.yml` and
nowhere else. Neither can fail anything, and a finding list that moves on the
scale of weeks does not earn a place in a pre-commit hook.

## What runs where, and what it costs

| Pass | Blocking | Where |
|---|---|---|
| `run-clang-tidy-blocking.ps1` (the five checks above) | yes | `verify.ps1`, CI `build-test-debug` |
| `cppcheck --enable=warning,performance,portability` | yes | `verify.ps1` |
| broad clang-tidy (`.clang-tidy` minus the analyser) | no | `advisory-checks.yml`, nightly |
| `cppcheck --enable=unusedFunction` | no | `advisory-checks.yml`, nightly |

Two properties of the blocking passes are worth knowing before changing them.

**A missing tool is not a pass.** `check-quality.ps1` exits `3` when a tool it
was asked to run is not installed, `verify.ps1` reports that as `TOOL_MISSING`,
and `-Full` -- the contract that claims every local gate ran -- fails on it.
`-Fast` reports the gap and continues, so a machine that is still being set up
stays usable. Install what is missing: `winget install Cppcheck.Cppcheck`.

**Both passes replay results.** clang-tidy caches per translation unit, keyed on
that unit's whole recorded input set; cppcheck uses `--cppcheck-build-dir`, keyed
on its own version. Both caches live under `%LOCALAPPDATA%\ExoSnap\tool-cache`,
outside the repository and outside every build tree, because a fresh configure or
a `git clean` is exactly the moment a replay would have paid the most. Every
worktree of the repository shares them. Delete the directory to force a cold run.

`-Full` runs clang-tidy over the whole tree, `-Fast` over the translation units
the change reaches. The scoped pass is a subset, so `-Full` does not run both.

## Local machine settings this repository does not set

**Raise the sccache cache size.** sccache defaults to a 10 GB cap. Several
worktrees of this repository building Qt-heavy translation units evict each other
continuously at that size, so the cache stops paying for itself. This is a
machine setting, not a repository one -- nothing here changes it for you:

```powershell
[Environment]::SetEnvironmentVariable('SCCACHE_CACHE_SIZE', '50G', 'User')
sccache --stop-server    # the running server keeps the old cap until restarted
```

`sccache --show-stats` reports the cap in effect and the hit rate to judge it by.
