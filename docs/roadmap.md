# ExoSnap roadmap

This is future direction, not a support matrix or a release schedule. [Product specification](product-spec.md) describes implemented behavior; [Known limitations](../KNOWN_LIMITATIONS.md) states its boundaries. Work below requires validation before becoming a shipped promise.

## First priority: reliability and release confidence

Broaden real-hardware coverage of capture transitions, HDR, high-refresh recording, audio endpoint recovery and long-running synchronization. Extend the player/editor compatibility matrix, especially HEVC, 10-bit and 4:4:4 files. Preserve independent media inspection and explicit product-versus-harness verdicts.

Complete candidate-bound verification where a scenario still cannot prove which installed bytes it exercised. Continue hardening disposable-environment provisioning, interactive-console execution, clean installation, upgrade and restoration. A gate body existing is not evidence that its preconditions can be satisfied or that a release passed it.

Give the offline Setup a dark native title bar and ExoSnap-styled buttons once WiX supports them. WixStandardBootstrapperApplication exposes no DWM option, and thmutil's graphic buttons lose the default-button bit and draw no disabled state, so both wait on an upstream change or a reviewed native extension rather than a custom bootstrapper application.

Strengthen display restoration against identity ambiguity. The current matcher prefers an exact connector path before panel metadata; it cannot guarantee that a different physical panel on that connector will be rejected. Any stronger promise needs matcher changes and regression tests, not just different wording.

Maintain repeatable privacy review and symbol delivery for official crash reports. Authenticode signing and its operational integration remain release work; signed update manifests are not a substitute for executable publisher signing. Keep signing status explicit in release documentation.

## Product work

| Direction | Boundary |
|---|---|
| Localization qualification | German ships through Qt translations with English fallback. Broaden native screen-reader and real-device qualification, and maintain translation coverage as product copy changes. |
| Cold navigation responsiveness | Explore lightweight page shells and asynchronous section construction without layout jumps, blank pages or a second notion of selected state. |
| Capture-format rollover | Consider ending one segment and rebuilding capture/color/encoder state when HDR or source dimensions change. Existing mux-only split does not rebuild those resources. Current behavior remains an explicit stop. |
| Replay and richer editing | Replay buffering and richer edit operations are capability gaps, not controls that should be advertised as working. Lossless keyframe trim remains the current editing boundary. |

## Deferred to v0.11 and later

These are explicitly postponed, not silently implied by any current surface. v0.10 keeps the existing behavior.

### Update lifecycle and distribution (v0.11+)

Evaluate whether updater distribution, temporary runtime staging and installation ownership can be simplified without changing the verified release trust chain. Preserve distinct portable, Windows Installer and package-manager responsibilities. Validate renamed/removed runtime files, locked files, interrupted updates and restoration. A separately downloadable bootstrapper is an option to evaluate, not a shipped feature. The current staged updater and install-mode-specific apply logic remain unchanged in v0.10.

### Region workflow (v0.11+)

Evaluate draw-on-start, explicit reuse of the previous region and optional saved areas against the current reusable-region workflow. Define hotkey, countdown, cancel/restore and source-preview behavior before changing the default. Preserve physical-coordinate, display-identity, DPI and capture-exclusion correctness. v0.10 keeps the existing region selection and recording-start semantics.

### Optional presentation tracing integration (post-v0.10)

Evaluate a PresentMon Service client or another narrowly scoped collector only if it provides sufficient attribution value. Keep the main UI unelevated. Define installation consent, privilege boundaries, service/API compatibility, update ownership, unavailable behavior and bounded resource use before shipping a dependency. Core recording diagnostics must remain useful without that component. A service moves privileged work to another process; it does not make system rights unnecessary.

### Overlay presentation policy (post-v0.10)

Measure the effects of overlay visibility and repaint cadence separately across relevant Windows, driver, display and game configurations. Evaluate low-distraction/default policies from recording and gameplay impact, not from overlay frame rate alone. Do not promise VRR, Independent Flip or DWM neutrality solely because an overlay is capture-excluded or redraws infrequently. The overlays already render with Qt Quick; their migration is not pending work.

## Hardware reach and media capability

AMD AMF and Intel oneVPL/QSV are future hardware backends. Each requires native surface integration, capability and diagnostic mappings, failure behavior, and a real hardware matrix. Generalize backend/codec boundaries where needed rather than copying per-codec assumptions into each vendor implementation.

An optional SVT-AV1 software path remains exploratory. No software video encoder is currently available. Bundling software AVC/HEVC is not planned; any external, user-provided encoder integration needs a separately accepted execution, support and distribution design. This roadmap is not legal clearance for distributing codec implementations.

AV1-in-MP4 and PCM-in-MP4 require a concrete sample-entry and player/editor matrix before they become selectable. FLAC remains an MKV feature. HLG, 4:2:2, 10-bit 4:4:4 and surround audio are outside current support and have no committed delivery date. Do not infer planned support from an enum, upstream codec feature or disabled placeholder.

## Changes that must earn their cost

Encoder tuning beyond the current pipeline, including B-frames, lookahead, adaptive quantization and deeper overlap, needs measured quality-per-bitrate and latency gains using the [quality workflow](dev/encoder-quality-matrix.md). A higher preset number is not itself evidence of a better recording experience.

GPU-native editor decode transport is a possible optimization if measured readback/upload cost justifies sharing the decoder and renderer device. The current hardware decode path deliberately reads back to the same plane representation as software decode.

A supported general recording CLI or external automation service is distinct from the existing development harness and opt-in local verification endpoint. It needs its own user-facing authorization and lifetime contract. Do not turn release-test convenience into an undocumented administration API.
