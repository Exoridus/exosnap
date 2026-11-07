# ExoSnap contributor and agent rules

## Product and architecture

ExoSnap is a Windows-native C++ recording engine with a Qt 6 / Qt Quick frontend. Business/product policy stays in C++; QML owns presentation, layout and interaction. Qt Widgets remains for the main application's native tray integration and for native OS surfaces such as file dialogs, not a second product frontend. The standalone updater and offline Setup render with Qt Quick.

Read relevant source/tests first. [Product specification](docs/product-spec.md) owns behavior, defaults, navigation and terminology; update it in the same change when behavior moves. [Architecture overview](docs/architecture/overview.md) maps subsystem constraints. Do not duplicate product policy in agent instructions.

Keep capture, encode, mux, diagnostics and UI responsibilities separate. UI submits editable source rows; the engine resolves tracks. Container/codec reconciliation is shared C++ policy, not QML logic. A hotkey start activates Record only when the window is already visible. Prefer explicit lifecycle state machines and structured models. Every metric needs a source, meaning, cadence and consumer; an unmeasured value is unavailable, never a fabricated zero. Measure before optimizing.

## Durable documentation

`main` documents the current system. Pull requests and Git preserve development history. [Documentation index](docs/README.md) defines the authorities: product behavior, thematic architecture, developer workflows, accepted unimplemented designs, and future-only roadmap.

Before documenting a decision, ask whether a future contributor needs it to understand or safely modify the current system, then find the existing canonical owner. Create a document only when no owner fits. Preserve non-obvious ownership, lifetime, failure and security lessons without narrating prior attempts. A plausible rejected approach needs rationale only when it could otherwise be reintroduced unsafely.

Private implementation plans, research, audits, evidence and agent working notes stay untracked. They are not authority and must never be linked from committed source or public docs. Do not create numbered decision archives, completed implementation-plan archives or generated review-history directories. Implemented designs are absorbed into product/architecture/dev authorities and removed. Accepted future designs state their exact status.

The documentation gate checks structure/references; editorial review checks truth and ownership. Do not build brittle prose-style linters. CHANGELOG and legal dependency provenance retain their distinct historical responsibilities.

## Command environment

Primary development uses Windows 11 x64 and PowerShell 7. Run commands through `pwsh` with Windows paths/quoting. Use `rg` / `rg --files`, then native PowerShell operations. Pass explicit roots and `--glob` rather than relying on shell wildcard expansion by native commands. The presence of an individual Coreutils command does not imply a GNU shell; do not invoke the Windows WSL launcher for repository work.

Reusable automation lives in Rust: `exo-dev` for repository checks and release tooling, `exo-verify` for product verification, and `exo-guest` inside disposable guests. Do not add standalone PowerShell, Python, Bash/shell, batch/CMD or C# helper scripts or tools. PowerShell and Bash remain command shells with minimal workflow glue and Git-hook launchers. CMake configures builds, not general automation. External package-format adapters require an exact-path policy exception and contain no ExoSnap business logic. `cargo exo-dev check automation-policy` enforces the tracked-source contract.

Media tools include ffprobe, ffmpeg, ffplay, MPV (`mpv`) and VLC where installed. Check `Get-Command` and resolve an installed tool before declaring it absent from a long-lived process's PATH. Prefer noninteractive ffprobe for inspection. Opening a GUI player follows the same desktop-coordination rules as driving the app.

## Tool output and context usage

Keep tool output minimal by default. Use a tool's compact or quiet mode where available; `exo-dev verify --output compact` explicitly selects compact verification output. Redirect verbose build, test and analysis output to untracked log files before it reaches the conversation. Return only the exit status, concise result, log path and relevant failure excerpts. Preserve full logs and verification receipts for review.

Do not dump complete logs, large files, directory trees or repeated progress output into the conversation. Read targeted ranges or filtered matches with bounded output, and expand only when needed to diagnose a specific problem or when requested. Tool response limits are a backstop, not a replacement for limiting output at its source. Apply this policy to delegated agents as well; never hide failures or treat omitted output as evidence of success.

## Driving applications and Windows

The developer may be using the same machine. Never synthesize mouse/keyboard input or take focus without same-turn coordination and confirmation that the machine is free. Earlier permission does not carry forward. Prefer semantic application control or structural UI Automation over coordinate input, but do not treat either as permission to disrupt the desktop.

UAC and Secure Desktop are never scripted. Explain the requested action and leave the decision to the operator. Physical unplug/repower remains physical. For supported restorable environment setters, inspect `exosnap-envctl snapshot` and use its declared transaction mechanism; pair begin with restore and leave the journal clean. An unbound alias requires deliberate binding, not automatic device selection. Human/read-only classifications must not be bypassed with an undocumented API.

A simple launch/smoke check is allowed when it does not seize input. Use adapter/QML tests and `--visual-test` for deterministic behavior/composition before a live run. Know their limits: Edit decode needs real media; capture-excluded overlays cannot be judged from screenshots/PrintWindow; native interactive drag is not programmatic movement. State what remains unobserved.

`--auto-record` and bare/visual harness modes are argument-configured, not input synthesis. Keep configuration/output isolated and scratch artifacts untracked. Release acceptance uses the production control endpoint against official bytes, not a separately instrumented replacement binary. See [Harnesses](docs/dev/harness-and-tracing.md), [Live Verify](docs/dev/live-verify.md) and [Release verification](docs/dev/release-verify.md).

## Release and merge authority

Preparing or testing a release does not authorize publication. Creating or pushing a version tag, publishing a GitHub release and submitting a package-manager version each require the user's explicit approval of that exact operation in the current interaction, or the maintainer's approval at the `release` environment gate of the release workflow. A green verification report is evidence, not permission.

Use `cargo exo-dev pr open` and `cargo exo-dev pr merge` for their validation rules. `pr merge --confirm` is a mechanical guard, not authorization. Pass it only after approval to merge that exact pull request. Work in a branch and preserve Git history.

Before enabling auto-merge, obtain an independent review of the exact current head and resolve its findings. Prefer the installed hosted Claude reviewer when available. An empty check suite or advisory `neutral`/`skipped` conclusion is not a passed review. If hosted review is unavailable, the coordinator obtains an independent agent review outside the authoring context and verifies its head. Confirmed ordinary and native-stack merges require `--expected-head` from that review, using the top member for a stack. This binds identity without certifying review. The PR helper does not publish a trusted review gate, and the ruleset does not enforce this policy. Until that changes, the coordinator monitors the queued head, cancels auto-merge if it changes, and requalifies before enabling it again. Local assertions and author comments cannot substitute for independent evidence.

Use `cargo exo-dev pr update-branch` for a selected candidate, with explicit expected head and actual base-ref SHAs on confirmation. Its one-shot merge update preserves the actual target branch and suppresses repeated requests for an in-flight, uncertain or accepted head across linked worktrees, including after the base advances. Coordinate `--acceptance-update` before changing a draft or a head with active/unknown CI. Do not turn `next` pushes into an indiscriminate update sweep, retarget dependent PRs, or remove an uncertain intent just to retry. After a new actual head is observed or an uncertainty is explicitly resolved, reassess the dependency, review and CI state. [CONTRIBUTING](CONTRIBUTING.md#opening-and-merging-a-pull-request) owns the command contract and remaining races.

## Multi-workstream pull requests and stacked integration

For a substantial multi-agent or multi-subsystem change, choose the PR dependency graph before coding. Independent workstreams use separate worktrees and branches from the same current `origin/next` base, independent PRs against `next`, and concurrent CI. Do not stack unrelated fixes just to group a sprint. Give shared source, generated assets and build outputs a single owner; group closely related small corrections into one coherent PR with logical commits.

For genuinely dependent workstreams, prefer GitHub's native stacked pull requests when the installed tools and repository policy support them. Inspect `gh stack --help` first; the supported lifecycle includes `gh stack init`, `gh stack link`, `gh stack submit` and `gh stack merge`, not guessed commands. Explicitly set `--base next` when initializing or linking a new stack: the repository default is `main`, but development PRs target `next`. Retain narrow per-level diffs, review each actual head, and require passing CI for all levels and the integrated result. Squash per PR remains the default. If an ancestor was squash-merged separately, reconcile descendants onto `next` without replaying already merged commits; rewrite a published branch only after inspecting its old SHA and with explicit `--force-with-lease=<ref>:<expected-sha>`.

Keep `cargo exo-dev pr open`, `cargo exo-dev pr merge`, their policy checks, and the exact-PR merge authorization in the preceding section authoritative. Do not substitute `gh stack merge` if it bypasses those guards or the maintainer's approval for each exact PR; instead use a compatible integration path or request the required authorization/tooling change. Preserve the existing per-PR Windows CI, `sccache`, and separate PR-policy checks; run the required checks for every relevant stack level and avoid repeating an identical expensive local matrix without changed evidence.

Keep workstream ownership, dependencies and validation receipts in private untracked notes, not in committed product documentation. Do not enable merge queues, modify branch protection or weaken required checks as part of ordinary implementation.

## Source hygiene and language

Prefer self-explanatory code. Comments explain non-obvious correctness, safety, invariants, ownership/ordering, compatibility workarounds or intentional deviations, not what a line visibly does. API documentation is concise and caller-facing: behavior, constraints, side effects and edge cases, not a restatement of names/types.

No task/audit IDs, commits, issues, pull-request numbers, branch/worktree names, private paths, agent/session narrative or machine-specific paths in source/API documentation. Replace a decision-number pointer with the invariant itself. Link a current architecture section only when larger context is genuinely necessary. Shipped diagnostic/scenario identifiers are actual data contracts, not review provenance.

Developer prose and code comments are English with ASCII punctuation. Preserve technically meaningful notation, names/diacritics and localized strings; identifiers remain ASCII. Rewrite a sentence instead of substituting double hyphens for an em dash. Prefer a second sentence to a prose semicolon. Commands/operators retain required syntax.

Markdown follows the same writing rules. Use fences for literal commands/files/logs/code, not a box around prose. Use a table/list when the set itself is a lookup reference; ordinary explanations stay prose. Write long prose lines with breaks at meaningful boundaries, not a fixed column. Correct text being changed without an unrelated tree-wide style sweep.

Wrap user-visible text for Qt translation as it is written. English source text is the canonical fallback for German localization. German text uses natural ä/ö/ü/ß, not ASCII transliteration. A font/encoding defect is fixed at that layer. Developer docs remain English regardless of locales.

## Worktrees and storage

Create local linked Git worktrees under `<repository-root>/.worktrees/<short-descriptive-name>/`, with short, stable, collision-free names. `.workspace` holds untracked evidence, research and scratch data, not source checkouts. Each checkout owns its `build/` CMake/Ninja outputs and its `tools/target/` Cargo outputs under the existing Cargo configuration. Share compatible artifact caches, not mutable CMake caches, Ninja graphs, compilation databases, generated QML or test receipts.

Keep active worktrees and useful incremental outputs in place while an agent, developer process, IDE, preview or test may use them. An open PR normally retains its source checkout and useful Debug build. Reclaim other artifacts only after checking their continued use and evidence requirements. A successful push is not a cleanup trigger.

Cleanup is part of completing a workstream. Once the work is finished and its pull request is merged, retire the linked worktree and its disposable build and Cargo outputs as soon as the safety checks below pass. Do not wait for a separate cleanup request or retain completed, merged worktrees as an artifact cache. Preserve required evidence outside the worktree first. If cleanup is blocked by active use, local changes, a lock or commits not confirmed on the intended remote, report the specific blocker and finish cleanup when it is resolved. Removing the source checkout of a closed, unmerged workstream requires an explicit decision to discard or preserve that work.

Before retiring a merged or closed worktree, verify desired commits on the intended remote, a clean working tree, inactivity and no lock. Retain required evidence outside it, remove it through Git, and verify metadata and refs afterward. Use `git worktree prune --dry-run` for missing administrative entries only. Do not use forced removal, branch deletion, `git clean -fdx` or broad recursive deletion as generic cleanup strategies.

Migrate a noncanonical worktree only when inactive and safe, preferably with `git worktree move`. Never silently relocate the primary checkout. Technical exceptions such as isolated cloud filesystems, Windows path limits or incompatible tooling belong in private session notes with their reason. Independently managed clones retain their own ownership.

## Visual evidence for UI pull requests

Every pull request that visibly adds or changes a Qt Quick screen, control, layout, interaction state or theme must maintain a cumulative collection of relevant, currently valid screenshots and clips in a structured **Visual evidence** section of its description. Keep one current evidence entry per relevant combination of surface, state, theme and logical window size. Different relevant states coexist. Label any intentional before/after baseline separately from current-state evidence.

Add evidence for a new visible state. When a documented state visibly changes, replace only its affected captures and remove their superseded references from the PR. Retain evidence for unchanged states without recapturing or uploading it again. A purely technical revision needs no new capture when it causes no relevant visible change. Do not append a new comment or image collection for every commit. Historical local captures may remain untracked.

Preserve **both Dark and Light** coverage for changed Qt Quick surfaces. Capture material states such as normal, focused, disabled, open or error when affected, and check regular and narrow logical window sizes when responsive layout changes. A targeted crop is sufficient for a small local UI correction if it retains enough context to identify the change. Use short GIF, MP4 or WebM clips when a still cannot convey animation or moving UI, adding new animations and replacing changed clips. Do not create a full-application sweep without a reason.

Use the built-in `cargo exo-dev screenshot` fixtures from the PR revision being evidenced, not a design mock or an unrelated build. Unchanged evidence may come from an earlier PR revision and need not match the latest commit. Preserve its actual capture source and manifest, and do not relabel it as captured from the latest head. If a change leaves its validity uncertain, verify it again or recapture the affected state.

Run `cargo exo-dev screenshot list` to find the closest named fixture and use `shot` or a narrowly scoped `sweep`; [Harnesses](docs/dev/harness-and-tracing.md#deterministic-visual-capture) owns the commands, manifests, limits and per-shot logs. Inspect the actual images, play clips and check capture metadata, not just process exit status. Disclose missing scenarios, warnings or test cards, and do not claim that software/offscreen captures prove native D3D11 rendering, capture exclusion, audio output or live device behavior. Never upload actual desktop captures, personal window titles or secrets without explicit authorization and review.

Embed or attach the relevant images and clips in that description section with clear **surface, state, appearance and size** labels and their actual capture revision so reviewers can inspect them without a developer checkout. A local `.workspace` path and an expiring Actions artifact alone are not durable PR references. Capture after committing and pushing relevant checked changes to a Draft PR. Evidence does not block the push or CI, and uploading it needs no source commit. Use a unique PR/run directory under system temp with `--out-dir`. After successful upload and visual review, copy required provenance into the PR and automatically remove only this run's disposable files once no longer needed, without a separate cleanup request. Preserve files needed for review or retry after incomplete uploads. Do not require a permanent local media archive. Follow the [cleanup contract](docs/dev/harness-and-tracing.md#temporary-capture-cleanup). Do not add generated captures to the source tree. Use GitHub CLI 2.99 or newer with `gh pr edit --attach`, following the [selective upload workflow](docs/dev/harness-and-tracing.md#attaching-screenshots-to-a-pull-request). Check the installed version and upgrade an outdated CLI before declaring uploads unavailable. If tooling cannot attach required media, state the limitation and leave the PR in Draft until the visual evidence is accessible; do not silently replace it with an unevaluated capture claim. If a change provably has no visible UI impact, explicitly state why visual evidence is not applicable.

## Change discipline and validation

Keep work scoped to a subsystem unless a technical dependency requires integration. Record incidental polish separately instead of silently broadening scope. Parallel workers need disjoint file ownership. Do not silently expand product scope or mark a planned control as working.

Use the smallest sufficient build/tests during iteration. Before completion run the full required gate once for the finished tree, including format, diff whitespace, Debug/tests/static checks and Release requirements. Avoid re-running an identical expensive sequence without new evidence to gain. Hardware/visual verification follows the release risk and declared acceptance boundary, not an assumed universal green from fixtures.

`cargo exo-dev test` is the test entry point. It builds the selected tree, isolates Qt/configuration and publishes `Testing/last-run-receipt.json`. Read `reusable`; stale binaries or incomplete test census cannot prove current code. Build and test use a shared host lock per build directory. `--no-build`/`--allow-stale` weaken the claim and must be disclosed.

```powershell
cargo exo-dev test
cargo exo-dev test --filter recorder_core.
cargo exo-dev test --exclude-label live
cargo exo-dev verify --fast
cargo exo-dev verify --full
```

[Build and test](docs/dev/build-and-test.md) covers tools, receipts and documentation checks. Existing probes answer hardware questions before another probe is added. The disposable guest is test infrastructure, not proof of real physical hardware.

## Commits and reporting

[CONTRIBUTING](CONTRIBUTING.md) owns commit/review policy. Use an English Conventional Commits subject, `type(scope): summary`, with `!` for a breaking change. Do not pre-append the pull request's own number; the merge helper adds it once. Rationale, evidence and breaking-change guidance belong in the pull request.

Do not edit CHANGELOG on an ordinary branch. The release-cut tool derives entries from merged subjects. Keep final reports concise and factual: changed behavior/docs, exact checks run, outcomes and remaining limits. Do not claim tests, hardware observations or publication that did not occur.
