# ExoSnap contributor and agent rules

## Orientation and ownership

ExoSnap is a Windows-native C++20 recording engine with a Qt 6 / Qt Quick frontend. C++ owns product policy; QML owns presentation and interaction. Qt Widgets is limited to native integration such as the tray and OS dialogs; the updater and Setup use Qt Quick.

Act as a responsible long-term maintainer: fix root causes and directly affected callers, edge cases, and regressions, not just symptoms. Reuse existing abstractions, preserve architecture and performance constraints, and implement the smallest complete, maintainable change. Do not broaden product scope or refactor unrelated code.

Read task-relevant source and tests. Consult the [product specification](docs/product-spec.md) for behavior and terminology, the [architecture overview](docs/architecture/overview.md) for subsystem boundaries, and the [documentation index](docs/README.md) for other authoritative docs. Load only what the task needs. Update the canonical product/architecture document when changing its contract; do not reproduce product policy here.

Keep capture, encoding, muxing, diagnostics, and UI responsibilities separate. UI submits editable source rows; C++ resolves tracks and container/codec policy. Prefer explicit lifecycle state machines and structured models. Every reported metric needs a real source, cadence, meaning, and consumer; unavailable data is not fabricated zero. Measure before optimizing.

## Documentation and source

- `main` documents the current system; PRs/Git preserve development history. Durable docs explain current decisions and non-obvious safety/ownership contracts, not private work logs. Accepted unimplemented designs must say so. Keep audits, plans, agent notes, and evidence untracked, never referenced in committed files. Do not create decision/plan archives or duplicate canonical documents.
- Prefer self-explanatory code. Comments/JSDoc explain non-obvious invariants, ownership, compatibility, or caller-facing constraints. Do not embed task IDs, PR/commit history, agent narratives, private paths, or machine-specific paths in source/API docs.
- Developer prose and comments are English with ASCII punctuation; write readable Markdown with semantic line breaks. Wrap user-facing Qt strings for translation. English is the fallback; German localization uses proper ä/ö/ü/ß.

## Development environment and diagnostics

- Primary environment: Windows 11 x64, PowerShell 7 (`pwsh`), CMake, and Rust. Use `rg` with explicit roots/globs. Do not assume GNU shell semantics or invoke WSL for repository work.
- Shared automation belongs in Rust: `cargo exo-dev`, `exo-verify`, and disposable-guest `exo-guest`. Do not add standalone PowerShell/Python/Bash/batch/C# helper automation. PowerShell/Bash may provide minimal shell or hook glue; CMake configures builds. Documented exact-path external packaging adapters are exceptions.
- Keep tool output compact. Redirect verbose output to untracked logs; report exit status, relevant failure excerpts, and log path. Never conceal errors or infer success from omitted output. Inspect installed media tools (`ffprobe`, `ffmpeg`, `mpv`, VLC) before declaring them missing.
- Desktop interaction requires same-turn coordination when it could take focus or synthesize input; earlier permission is insufficient. Never automate UAC/Secure Desktop or physical power actions. For restorable environment setters, use the documented `exosnap-envctl` snapshot/transaction and restore sequence; never auto-bind an unbound device.
- Prefer deterministic adapter/QML, `--visual-test`, and safe launch checks before live interaction. A simulated/offscreen test does not prove real capture, audio output, native drag, D3D11 rendering, or capture exclusion. Use production binaries/endpoints for release acceptance. See [harnesses](docs/dev/harness-and-tracing.md), [live verification](docs/dev/live-verify.md), and [release verification](docs/dev/release-verify.md).

## Validation and change discipline

- Keep tasks within their subsystem unless a real dependency requires integration. Assign disjoint ownership to parallel workers. Do not claim planned features or unobserved hardware behavior as working.
- Iterate with targeted tests. Before completion run the full **applicable** final-tree verification once (format, diff, builds, Debug/tests/static checks, Release requirements as applicable); avoid repeating unchanged expensive suites. Use `cargo exo-dev test` and `cargo exo-dev verify --fast` / `--full` according to [build and test](docs/dev/build-and-test.md).
- Test receipts (`Testing/last-run-receipt.json`) must match the current tree and be marked reusable. Builds share host locks. Disclose `--no-build`, `--allow-stale`, missing hardware, or any other limitation instead of presenting stale results as proof.

## Pull requests, reviews, and releases

- Use a branch from current `origin/next` and open PRs against `next`. Use `cargo exo-dev pr open` and `cargo exo-dev pr merge` with their policy checks. For dependent changes, prefer supported native stacks only if every level retains required CI/review and the repository's PR authorization controls; set `--base next`. Squash per PR; avoid stacking unrelated changes.
- Merging **each exact PR** requires explicit approval. `pr merge --confirm` is only a mechanical guard. Before auto-merge, require independent review of the exact head and resolve findings; `neutral`, `skipped`, or empty checks are not approval. Use the reviewed `--expected-head`, monitor queued heads, and requalify changed ones. Never bypass the PR helper/approval via `gh stack merge`.
- For selective branch refreshes, use `cargo exo-dev pr update-branch` with the actual base/head SHAs and the documented acceptance-update coordination for drafts or uncertain CI. Do not sweep all PRs after a `next` push or erase uncertain update intent. Consult [CONTRIBUTING](CONTRIBUTING.md#opening-and-merging-a-pull-request) for the full command and race contract.
- Tag pushes, GitHub releases, and package-manager submissions each need explicit approval of that exact operation in the current interaction or approval at the release environment gate. Green checks alone never authorize publication.
- Do not change branch protection, merge queues, required CI, or review gates as part of normal work. Keep PR/workstream state and validation evidence in private untracked notes.

## Worktrees and storage

- Use `.worktrees/<short-name>/` for linked checkouts and `.workspace/` for untracked notes/evidence. Each checkout owns its writable CMake/Ninja `build/` and Cargo `tools/target/`; share compatible artifact caches, not mutable build graphs or generated QML.
- Retain active and open-PR worktrees and useful warm builds. After merge, retire the worktree once remote commits, clean state, inactivity, locks, and evidence retention are verified. Remove through Git, never forced removal or broad recursive cleaning. Do not delete an unmerged worktree without an explicit preserve/discard decision.

## Visual evidence for Qt Quick PRs

- For visible changes, maintain the PR's cumulative **Visual evidence** section with current captures for each affected surface/state/theme/size. Cover Dark and Light, relevant focus/error states, and responsive sizes when affected. Replace only invalidated evidence; technical-only changes need no recapture.
- Use revision-matched `cargo exo-dev screenshot` fixtures (inspect `list`, capture targeted `shot` or `sweep`), and inspect the actual images/clips and manifest. PNG by default; short GIF only for motion where stills are insufficient. Do not claim software captures prove native hardware. Never upload personal desktops, window titles, or secrets without explicit authorization.
- Attach evidence with identifying state/theme/size and honest source revision to the Draft PR, not just a local path or expiring artifact. Follow [harness capture/upload/cleanup](docs/dev/harness-and-tracing.md#deterministic-visual-capture), use scoped temp output, and delete only disposable files after successful upload. If evidence cannot be attached, keep the PR Draft and disclose the limitation. Explain when visual evidence genuinely does not apply.

## Commits and reporting

Use English Conventional Commits (`type(scope): summary`), with `!` for breaking changes. Do not prepend the PR number or edit CHANGELOG for ordinary PRs. [CONTRIBUTING](CONTRIBUTING.md) owns details. Report what changed, exact checks run, results, and remaining limitations concisely and accurately.
