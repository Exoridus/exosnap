# ExoSnap agent instructions

## Project

ExoSnap is a Windows-native recording application: a high-performance C++ engine
with a Qt 6 / Qt Quick user interface (ADR 0064). `Qt6::Widgets` remains linked
for `QSystemTrayIcon` alone, and the separate updater executable keeps its own
Widgets UI; neither is a second frontend.

Business and product policy stays in C++. QML owns presentation, layout and
interaction only.

## Product behavior

`docs/product-spec.md` is authoritative for user-visible product behavior. Read
it before changing behavior, defaults, navigation, terminology, UX, or product
policy, and update it in the same change when that behavior moves. Do not
duplicate product decisions into this file.

## Working context

Inspect the code and tests relevant to the task first. Do not read large
repository documents by default.

`.workspace/` is private working context, not repository authority. Plans,
research, reviews, live-verify evidence and release artifacts belong there.
Agents may consult it when relevant, but it is never a source of truth and must
never be referenced from committed source or public documentation. If a decision
becomes a durable contract, promote the conclusion into `docs/` instead.

## Architecture

- Keep the recording engine independent from UI concerns, and capture, encode,
  mux, diagnostics, telemetry and UI separate from each other.
- The UI submits editable source rows; the engine returns resolved tracks. Track
  resolution is never duplicated in the UI.
- Switching containers reconciles the selected audio codec to one the new
  container allows. That reconciliation is engine logic.
- A hotkey that starts recording while the window is visible activates the Record
  view; while minimized it does not restore the window.
- Prefer explicit state machines for the recording session lifecycle, and
  structured models over ad hoc UI-bound state.
- Every live metric has a source, a meaning, an update cadence, a UI consumer and
  a log consumer.
- Probe a capability in isolation before integrating it. Do not optimize
  speculatively; add profiling hooks and measure.

## Driving the running application

The developer works on the same machine and may be doing anything else on it at
the same moment. The failure mode this guards against is *uncoordinated* input:
taking focus while a controller is in use breaks controller-input recognition,
and moving the OS cursor while the developer is moving it causes mis-clicks.

- Never synthesize mouse or keyboard input, and never take window focus, without
  asking in the same turn and being told the developer is not using the machine
  right now. An earlier "go ahead" does not carry forward.
- UAC and Secure Desktop prompts cannot be scripted at all. Describe what the
  prompt will ask and what each answer does, then wait — the developer cannot
  have another window open while answering one.
- Physical and system-level changes (HDR, refresh rate, unplugging an endpoint)
  stay the developer's own action, even inside an otherwise automated flow.
- Prefer structural automation (UI Automation invoke patterns, accessible names)
  over coordinate-based synthesis: it does not move the real cursor.
- Starting the app once to confirm it does not crash is always allowed;
  `--smoke-test` is the cheaper form of the same check.
- Judge pixels with `--visual-test` and behavior with the adapter and QML tests
  before reaching for a live run, and say so when nothing else can verify a
  change. Know what a fixture cannot reach: the Edit surface's decode path needs
  real media (`--auto-edit`).
- The five capture-excluded overlays are structurally unobservable —
  `WDA_EXCLUDEFROMCAPTURE` defeats screenshots, screen recording and
  `PrintWindow`, and the harness only grabs their scene graph. How they reach the
  desktop can only be confirmed by the developer looking at the screen. Their
  `[overlay]` log lines exist for that reason.
- `--auto-record` is the same class of exception as `--visual-test`: argv- or
  environment-configured, never input synthesis. Its output goes to a scratch
  directory (`EXOSNAP_OUTPUT_DIR`, else the system temp directory) and is never
  committed.

## Source hygiene

Prefer self-explanatory code. Add comments only for non-obvious correctness,
safety, invariants, lifecycle or ordering constraints, compatibility workarounds,
or intentional deviations from normal practice. Explain why the obvious
implementation would be wrong, not what the code visibly does.

API documentation (Doxygen, QDoc, JSDoc) is concise and caller-facing. Document
only behavior, contracts, constraints, important side effects, and non-obvious
edge cases. Do not restate names, types or signatures.

Never put development provenance in source or API documentation: task IDs,
commits, issues, pull requests, branches or worktrees, conversation or agent
history, private workspace references, or machine-specific paths. Keep the
durable technical rationale, drop how it was discovered.

Developer-facing source documentation is English and uses ASCII punctuation;
non-ASCII characters are allowed only when technically meaningful. The rule bans
typographic variants (em dash, en dash, curly quotes, ellipsis), not characters
as such: a unit, a symbol or an arrow that is the correct notation stays.

Repository specifics:

- `.workspace/` is private planning context. Agents may read it; committed source
  and public documentation must never point at it.
- Review findings are tracked as `QCR-###`. They belong in `.workspace/`, not in a
  source comment - keep the constraint the finding produced, drop the number.
- Diagnostic identifiers shipped as product data (`ART-001`, `ENV-001`, ...) are
  code, not tracker references, and are left alone.
- `scripts/check-source-hygiene.ps1` enforces the mechanical half; `scripts/verify.ps1`
  runs it. Its default scope is the work in front of you. The branch-wide sweep
  (`check-source-hygiene.ps1 -All`) currently reports a backlog in older comments.

## Iteration

- A normal slice targets one subsystem and 30-60 minutes. Do not broaden scope
  without a blocking technical need, and do not silently expand the MVP.
- Record minor polish findings for consolidated review instead of fixing
  everything on the way past.
- Run parallel workers only when their file ownership is disjoint. Never let two
  workers modify the same shared integration file.
- While implementing, validate with the smallest sufficient check: build the
  affected target, run the focused tests for that subsystem. Complete validation
  happens once, at the final gate.
- Budget roughly 5-15 targeted tests and 2-4 visual scenarios per slice. No
  exhaustive matrix unless the feature is inherently high-risk.

## Running tests

`scripts/run-tests.ps1` is the entry point — it sets the throwaway
`EXOSNAP_CONFIG_DIR`, `QT_QPA_PLATFORM=offscreen`, `QT_PLUGIN_PATH` and Qt on
PATH, and prints a compact summary plus the exact failing gtest cases.

```
pwsh scripts/run-tests.ps1                        # whole suite
pwsh scripts/run-tests.ps1 -Filter recorder_core. # one binary (not one case)
pwsh scripts/run-tests.ps1 -ExcludeLabel live     # skip real hardware queries
```

## Diagnostics and verification tooling

- Harness modes and tracing (`--hwnd-audit`, `--window-trace`,
  `EXOSNAP_PREVIEW_TRACE`, AddressSanitizer): `docs/dev/harness-and-tracing.md`.
- The Live Verify control channel, the updater automation channel and the
  cross-process update handoff: `docs/dev/live-verify.md` (ADR 0066, ADR 0067,
  ADR 0068).
- The release acceptance campaign: `docs/dev/release-verify.md`.

## Final validation

Once, after the branch is complete: format check, `git diff --check`, full Debug
build, full CTest, the static quality checks, and a Release build. Do not re-run
`check-quality.ps1` after an identical configure/build/test sequence unless it
contributes a check that cannot be invoked separately.

Green automated tests and clean deterministic visual scenarios are sufficient for
an implementation wave. Physical hardware checks and broad visual review are
deferred to consolidated review rounds. One or two documented minor limitations
do not block merge when the core behavior is correct.

## Reporting

Keep final reports concise: what changed, what validated it, and what remains
limited. Mention specification or ADR updates when the change required them.
