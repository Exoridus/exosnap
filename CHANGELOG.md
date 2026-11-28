# Changelog

All notable changes to ExoSnap are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). ExoSnap is pre-v1 preview software: a minor version may break behaviour, and such a change is marked **BREAKING**.

The release preparation assembles this file with `cargo exo-dev release changelog`. See [CONTRIBUTING.md](CONTRIBUTING.md) for the release cut exception.

Releases up to and including 0.9.0 predate this file; their notes are on the [releases page](https://github.com/Exoridus/exosnap/releases).

## [Unreleased]

## [0.11.0] - 2026-11-28

### Changed

- **keep Recording Clip creation unavailable in the release.** Existing clips can still be opened as media. ([#606](https://github.com/Exoridus/exosnap/pull/606))
- **BREAKING: integrate recording reliability and encoder input contracts.** ([#549](https://github.com/Exoridus/exosnap/pull/549))
- **BREAKING: guard pull request integration.** ([#511](https://github.com/Exoridus/exosnap/pull/511))
- **drive the interactive Setup from Rust UI Automation.** ([#460](https://github.com/Exoridus/exosnap/pull/460))

### Added

- **ship a launcher and preserve portable data.** ([#606](https://github.com/Exoridus/exosnap/pull/606))
- **isolate privileged tracing in a protected companion.** ([#603](https://github.com/Exoridus/exosnap/pull/603))
- **persist projects with atomic saves and recovery.** ([#602](https://github.com/Exoridus/exosnap/pull/602))
- **add toolchain-bench measurement runner.** ([#572](https://github.com/Exoridus/exosnap/pull/572))
- **integrate SDL3 input and native controller views.** ([#551](https://github.com/Exoridus/exosnap/pull/551))
- **integrate scene menu, export feedback and inspector controls.** ([#550](https://github.com/Exoridus/exosnap/pull/550))
- **make visual scenes the single preset authority.** ([#516](https://github.com/Exoridus/exosnap/pull/516))
- **integrate host-owned overlay providers and composition.** ([#515](https://github.com/Exoridus/exosnap/pull/515))
- **add passive visual runtime and gamepad sampler.** ([#513](https://github.com/Exoridus/exosnap/pull/513))
- **add typed visual contracts and migration codecs.** ([#512](https://github.com/Exoridus/exosnap/pull/512))
- **add native DualSense 3D pilot.** ([#506](https://github.com/Exoridus/exosnap/pull/506))
- **normalize controls and mask sticks on the GPU.** ([#505](https://github.com/Exoridus/exosnap/pull/505))
- **integrate native 2D wireframe controller artwork.** ([#504](https://github.com/Exoridus/exosnap/pull/504))
- **add gain envelopes and direct timeline editing.** ([#502](https://github.com/Exoridus/exosnap/pull/502))
- **animate video clips with source-time keyframes.** ([#501](https://github.com/Exoridus/exosnap/pull/501))
- **apply validated timeline actions directly with undo.** ([#500](https://github.com/Exoridus/exosnap/pull/500))
- **add tabbed sources and inline recording review.** ([#499](https://github.com/Exoridus/exosnap/pull/499))
- **report pruned release footprint.** ([#494](https://github.com/Exoridus/exosnap/pull/494))
- **implement compact native track routing.** ([#486](https://github.com/Exoridus/exosnap/pull/486))
- **restore settings cards and editable overlays.** ([#491](https://github.com/Exoridus/exosnap/pull/491))
- **add compact setup, updater repair and preview fixes.** ([#485](https://github.com/Exoridus/exosnap/pull/485))
- **integrate controller overlays with GPU preview and recording.** ([#483](https://github.com/Exoridus/exosnap/pull/483))
- **implement precise timeline editing and media workflows.** ([#479](https://github.com/Exoridus/exosnap/pull/479))
- **per-stream audio, junctions and planned export.** ([#475](https://github.com/Exoridus/exosnap/pull/475))
- **integrate v1.3 runtime, multitrack audio and workspace.** ([#473](https://github.com/Exoridus/exosnap/pull/473))
- **composite the webcam on the GPU and rework its settings and shortcuts.** ([#471](https://github.com/Exoridus/exosnap/pull/471))
- **return to the ready transport after a successful recording.** ([#469](https://github.com/Exoridus/exosnap/pull/469))
- **add a dedicated multi-clip editing workspace.** ([#467](https://github.com/Exoridus/exosnap/pull/467))
- **add AV1 VBR Auto Lookahead.** ([#465](https://github.com/Exoridus/exosnap/pull/465))
- **add reorder-safe expert encoder controls.** ([#464](https://github.com/Exoridus/exosnap/pull/464))
- **localize app and updater in German.** ([#463](https://github.com/Exoridus/exosnap/pull/463))
- **respect distribution ownership when applying updates.** ([#462](https://github.com/Exoridus/exosnap/pull/462))
- **coalesce HUD telemetry and compact quick controls.** ([#461](https://github.com/Exoridus/exosnap/pull/461))

### Fixed

- **pin the FFmpeg verification download.** ([#607](https://github.com/Exoridus/exosnap/pull/607))
- **deliver automatic MP4 segments.** ([#599](https://github.com/Exoridus/exosnap/pull/599))
- **reuse validated picker capture items.** ([#598](https://github.com/Exoridus/exosnap/pull/598))
- **compact source picker controls for short window lists.** ([#596](https://github.com/Exoridus/exosnap/pull/596))
- **keep Qt test artifacts out of compiler include roots.** ([#595](https://github.com/Exoridus/exosnap/pull/595))
- **preserve Unicode in diagnostics and recovery.** ([#593](https://github.com/Exoridus/exosnap/pull/593))
- **integrate v0.11 audit and review corrections.** ([#591](https://github.com/Exoridus/exosnap/pull/591))
- **scope local MSVC sccache keys by build tree.** ([#590](https://github.com/Exoridus/exosnap/pull/590))
- **stabilize hardware soaks and CI validation.** ([#575](https://github.com/Exoridus/exosnap/pull/575))
- **safely cache computed includes in unreachable branches.** ([#574](https://github.com/Exoridus/exosnap/pull/574))
- **key clang-tidy verdicts on observed inputs.** ([#570](https://github.com/Exoridus/exosnap/pull/570))
- **surface PresentMon consumer loss as non-attributed evidence.** ([#571](https://github.com/Exoridus/exosnap/pull/571))
- **remove clang-tidy batch barriers and skip unchanged advisory runs.** ([#568](https://github.com/Exoridus/exosnap/pull/568))
- **harden initial tidy warmup and no-profile reporting.** ([#566](https://github.com/Exoridus/exosnap/pull/566))
- **seed blocking clang-tidy cache on next.** ([#565](https://github.com/Exoridus/exosnap/pull/565))
- **consolidate ten engine policy test binaries.** ([#564](https://github.com/Exoridus/exosnap/pull/564))
- **give a local sccache its own cache and server per worktree.** ([#561](https://github.com/Exoridus/exosnap/pull/561))
- **prevent unsafe shared local sccache by default.** ([#559](https://github.com/Exoridus/exosnap/pull/559))
- **consolidate five audio DSP test binaries into one.** ([#557](https://github.com/Exoridus/exosnap/pull/557))
- **optimize local build reuse and worktree storage.** ([#555](https://github.com/Exoridus/exosnap/pull/555))
- **restore recording runs and define the idle matrix.** ([#543](https://github.com/Exoridus/exosnap/pull/543))
- **optimize cache lifecycle and compiler reuse.** ([#553](https://github.com/Exoridus/exosnap/pull/553))
- **protect session changes when closing.** ([#544](https://github.com/Exoridus/exosnap/pull/544))
- **align gamepad actions and preview contracts.** ([#537](https://github.com/Exoridus/exosnap/pull/537))
- **integrate Stage A caching and SDR demand-aware rendering.** ([#531](https://github.com/Exoridus/exosnap/pull/531))
- **retain completed recordings and qualify export handoff.** ([#523](https://github.com/Exoridus/exosnap/pull/523))
- **use generic artwork for Steam Controller 2026.** ([#510](https://github.com/Exoridus/exosnap/pull/510))
- **speed up verification and support safe stack merges.** ([#493](https://github.com/Exoridus/exosnap/pull/493))
- **polish audio routing and record interactions.** ([#490](https://github.com/Exoridus/exosnap/pull/490))
- **load marker sidecars on the metadata worker.** ([#487](https://github.com/Exoridus/exosnap/pull/487))
- **harden cache publication, webcam reconnect and queue peaks.** ([#481](https://github.com/Exoridus/exosnap/pull/481))
- **harden audio, HDR, editing and recording pipelines.** ([#480](https://github.com/Exoridus/exosnap/pull/480))
- **make clang-tidy cache reuse reliable.** ([#478](https://github.com/Exoridus/exosnap/pull/478))
- **take clang-tidy from the toolchain that compiled the tree.** ([#472](https://github.com/Exoridus/exosnap/pull/472))
- **keep the exo-dev alias working in nested worktrees.** ([#470](https://github.com/Exoridus/exosnap/pull/470))
- **pass the Chocolatey API key the way choco consumes it.** ([#456](https://github.com/Exoridus/exosnap/pull/456))

### Documentation

- **streamline guidance and add engineering ownership.** ([#573](https://github.com/Exoridus/exosnap/pull/573))
- **plan demand-aware rendering and Stage A cache.** ([#519](https://github.com/Exoridus/exosnap/pull/519))
- **define incremental visual evidence updates.** ([#520](https://github.com/Exoridus/exosnap/pull/520))
- **require screenshots for visual UI pull requests.** ([#496](https://github.com/Exoridus/exosnap/pull/496))
- **document native stacked PR workflow.** ([#489](https://github.com/Exoridus/exosnap/pull/489))
- **use reliable release and license badges.** ([#477](https://github.com/Exoridus/exosnap/pull/477))
