# ExoSnap contributor and agent rules

## Product and architecture

ExoSnap is a Windows-native C++ recording engine with a Qt 6 / Qt Quick frontend. Business/product policy stays in C++; QML owns presentation, layout and interaction. Qt Widgets remains for the main application's native tray integration and the separate updater, not a second product frontend.

Read relevant source/tests first. [Product specification](docs/product-spec.md) owns behavior, defaults, navigation and terminology; update it in the same change when behavior moves. [Architecture overview](docs/architecture/overview.md) maps subsystem constraints. Do not duplicate product policy in agent instructions.

Keep capture, encode, mux, diagnostics and UI responsibilities separate. UI submits editable source rows; the engine resolves tracks. Container/codec reconciliation is shared C++ policy, not QML logic. A hotkey start activates Record only when the window is already visible. Prefer explicit lifecycle state machines and structured models. Every metric needs a source, meaning, cadence and consumer; an unmeasured value is unavailable, never a fabricated zero. Measure before optimizing.

## Durable documentation

`main` documents the current system. Pull requests and Git preserve development history. [Documentation index](docs/README.md) defines the authorities: product behavior, thematic architecture, developer workflows, accepted unimplemented designs, and future-only roadmap.

Before documenting a decision, ask whether a future contributor needs it to understand or safely modify the current system, then find the existing canonical owner. Create a document only when no owner fits. Preserve non-obvious ownership, lifetime, failure and security lessons without narrating prior attempts. A plausible rejected approach needs rationale only when it could otherwise be reintroduced unsafely.

Private implementation plans, research, audits, evidence and agent working notes stay untracked. They are not authority and must never be linked from committed source or public docs. Do not create numbered decision archives, completed implementation-plan archives or generated review-history directories. Implemented designs are absorbed into product/architecture/dev authorities and removed. Accepted future designs state their exact status.

The documentation gate checks structure/references; editorial review checks truth and ownership. Do not build brittle prose-style linters. CHANGELOG and legal dependency provenance retain their distinct historical responsibilities.

## Command environment

Primary development uses Windows 11 x64 and PowerShell 7. Run commands through `pwsh` with Windows paths/quoting. Use `rg` / `rg --files`, then native PowerShell operations. Pass explicit roots and `--glob` rather than relying on shell wildcard expansion by native commands. The presence of an individual Coreutils command does not imply a GNU shell; do not invoke the Windows WSL launcher for repository work.

Media tools include ffprobe, ffmpeg, ffplay, MPV (`mpv`) and VLC where installed. Check `Get-Command` and resolve an installed tool before declaring it absent from a long-lived process's PATH. Prefer noninteractive ffprobe for inspection. Opening a GUI player follows the same desktop-coordination rules as driving the app.

## Driving applications and Windows

The developer may be using the same machine. Never synthesize mouse/keyboard input or take focus without same-turn coordination and confirmation that the machine is free. Earlier permission does not carry forward. Prefer semantic application control or structural UI Automation over coordinate input, but do not treat either as permission to disrupt the desktop.

UAC and Secure Desktop are never scripted. Explain the requested action and leave the decision to the operator. Physical unplug/repower remains physical. For supported restorable environment setters, inspect `exosnap-envctl snapshot` and use its declared transaction mechanism; pair begin with restore and leave the journal clean. An unbound alias requires deliberate binding, not automatic device selection. Human/read-only classifications must not be bypassed with an undocumented API.

A simple launch/smoke check is allowed when it does not seize input. Use adapter/QML tests and `--visual-test` for deterministic behavior/composition before a live run. Know their limits: Edit decode needs real media; capture-excluded overlays cannot be judged from screenshots/PrintWindow; native interactive drag is not programmatic movement. State what remains unobserved.

`--auto-record` and bare/visual harness modes are argument-configured, not input synthesis. Keep configuration/output isolated and scratch artifacts untracked. Release acceptance uses the production control endpoint against official bytes, not a separately instrumented replacement binary. See [Harnesses](docs/dev/harness-and-tracing.md), [Live Verify](docs/dev/live-verify.md) and [Release verification](docs/dev/release-verify.md).

## Release and merge authority

Preparing or testing a release does not authorize publication. Creating or pushing a version tag, publishing a GitHub release and submitting a package-manager version each require the user's explicit approval of that exact operation in the current interaction, or the maintainer's approval at the `release` environment gate of the release workflow. A green verification report is evidence, not permission.

Use `scripts/open-pr.ps1` and `scripts/merge-pr.ps1` for their validation rules. `merge-pr.ps1 -Confirm` is a mechanical guard, not authorization. Pass it only after approval to merge that exact pull request. Work in a branch and preserve Git history.

## Source hygiene and language

Prefer self-explanatory code. Comments explain non-obvious correctness, safety, invariants, ownership/ordering, compatibility workarounds or intentional deviations, not what a line visibly does. API documentation is concise and caller-facing: behavior, constraints, side effects and edge cases, not a restatement of names/types.

No task/audit IDs, commits, issues, pull-request numbers, branch/worktree names, private paths, agent/session narrative or machine-specific paths in source/API documentation. Replace a decision-number pointer with the invariant itself. Link a current architecture section only when larger context is genuinely necessary. Shipped diagnostic/scenario identifiers are actual data contracts, not review provenance.

Developer prose and code comments are English with ASCII punctuation. Preserve technically meaningful notation, names/diacritics and localized strings; identifiers remain ASCII. Rewrite a sentence instead of substituting double hyphens for an em dash. Prefer a second sentence to a prose semicolon. Commands/operators retain required syntax.

Markdown follows the same writing rules. Use fences for literal commands/files/logs/code, not a box around prose. Use a table/list when the set itself is a lookup reference; ordinary explanations stay prose. Write long prose lines with breaks at meaningful boundaries, not a fixed column. Correct text being changed without an unrelated tree-wide style sweep.

Wrap user-visible text for Qt translation as it is written. Current shipped UI is English; German localization is planned. German text uses natural ä/ö/ü/ß, not ASCII transliteration. A font/encoding defect is fixed at that layer. Developer docs remain English regardless of locales.

## Change discipline and validation

Keep work scoped to a subsystem unless a technical dependency requires integration. Record incidental polish separately instead of silently broadening scope. Parallel workers need disjoint file ownership. Do not silently expand product scope or mark a planned control as working.

Use the smallest sufficient build/tests during iteration. Before completion run the full required gate once for the finished tree, including format, diff whitespace, Debug/tests/static checks and Release requirements. Avoid re-running an identical expensive sequence without new evidence to gain. Hardware/visual verification follows the release risk and declared acceptance boundary, not an assumed universal green from fixtures.

`scripts/run-tests.ps1` is the test entry point. It builds the selected tree, isolates Qt/configuration and publishes `Testing/last-run-receipt.json`. Read `reusable`; stale binaries or incomplete test census cannot prove current code. Build and test use a shared host lock per build directory. `-NoBuild`/`-AllowStale` weaken the claim and must be disclosed.

```powershell
pwsh scripts/run-tests.ps1
pwsh scripts/run-tests.ps1 -Filter recorder_core.
pwsh scripts/run-tests.ps1 -ExcludeLabel live
pwsh scripts/verify.ps1 -Fast
pwsh scripts/verify.ps1 -Full
```

[Build and test](docs/dev/build-and-test.md) covers tools, receipts and documentation checks. Existing probes answer hardware questions before another probe is added. The disposable guest is test infrastructure, not proof of real physical hardware.

## Commits and reporting

[CONTRIBUTING](CONTRIBUTING.md) owns commit/review policy. Use an English Conventional Commits subject, `type(scope): summary`, with `!` for a breaking change. Do not pre-append the pull request's own number; the merge helper adds it once. Rationale, evidence and breaking-change guidance belong in the pull request.

Do not edit CHANGELOG on an ordinary branch. The release-cut tool derives entries from merged subjects. Keep final reports concise and factual: changed behavior/docs, exact checks run, outcomes and remaining limits. Do not claim tests, hardware observations or publication that did not occur.
