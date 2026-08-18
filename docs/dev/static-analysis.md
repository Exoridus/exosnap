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
clang-analyzer-core.CallAndMessage
clang-analyzer-core.uninitialized.*
clang-analyzer-cplusplus.NewDelete*
```

A check may fail a build only after a full pass over every project translation
unit reports zero findings in repository-owned files. These five were measured
that way across 509 translation units. Do not extend the list without repeating
that pass and recording the result here.

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
| `clang-diagnostic-switch` (enum exhaustiveness) | 2 | `RecordPage::canApplyPresetNow`, `VisualTestHarness` |

Enum-switch exhaustiveness is not covered by the compiler here. The build uses
`/W4 /WX`, but MSVC keeps C4062 (unhandled enumerator, no default label) and
C4061 (unhandled enumerator, default label present) off at every warning level;
they need an explicit `/w44062` / `/w44061`. Verified by compiling a probe with
the project's exact flags: silent at `/W4 /WX`, diagnosed once the warnings are
switched on. clang's own `-Wswitch` does run here as `clang-diagnostic-switch`,
which is why the two unhandled enumerators above are visible at all.

## Advisory checks

`misc-include-cleaner`, `misc-unused-using-decls`, `misc-unused-parameters`,
`misc-unused-alias-decls` and `readability-redundant-declaration` are enabled but
excluded from `WarningsAsErrors`. All five produce Qt meta-object and moc
false-positives. Promote one only after a human triage pass over its findings.
