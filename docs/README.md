# ExoSnap documentation

`main` describes the current product and system. Pull requests, Git and releases describe how they changed.

## Find the authoritative document

| Question | Authority |
|---|---|
| What can a user do, and what are the defaults? | [Product specification](product-spec.md) |
| What is not supported or not broadly verified? | [Known limitations](../KNOWN_LIMITATIONS.md) |
| How is the current system constructed? | [Architecture overview](architecture/overview.md) |
| How do I build, test or debug it? | [Build and test](dev/build-and-test.md), [harnesses and tracing](dev/harness-and-tracing.md) |
| How do I verify and release it? | [Release checklist](release-checklist.md), [release verification](dev/release-verify.md) |
| What should happen next? | [Roadmap](roadmap.md) |
| Which accepted design is not implemented? | [Preview frame-rate cap](design/preview-frame-rate-cap.md) |
| How do I resolve a recording problem? | [Troubleshooting](troubleshooting.md) |
| What leaves the machine? | [Privacy policy](../PRIVACY.md), [privacy review](privacy-review.md) |

## Architecture

Read the overview first, then the subsystem being changed. These documents own constraints and rationale, not another copy of UI policy.

| Subsystem | Documents |
|---|---|
| Recording | [Capture and preview](architecture/capture-and-preview.md), [recording pipeline](architecture/recording-pipeline.md), [timing and CFR](architecture/timing-and-cfr.md) |
| Media | [Audio](architecture/audio.md), [encoding and containers](architecture/encoding-and-containers.md), [color management](architecture/color-management.md), [Edit and Export](architecture/edit-and-export.md) |
| Application | [Frontend](architecture/frontend.md), [configuration and persistence](architecture/configuration-and-persistence.md), [diagnostics](architecture/diagnostics.md) |
| Trust | [Updates and security](architecture/update-and-security.md), [verification boundaries](architecture/verification-boundaries.md) |

## Developer workflows

[Live Verify](dev/live-verify.md) documents the local application/updater control channel. [Release verification](dev/release-verify.md) explains campaigns and environment restoration. [Typed verification harness](dev/release-verify-harness.md) covers harness development; its [generated catalog](dev/release-verify-catalog.md) lists scenario declarations. [Verification guest](dev/release-verify-vm.md) describes disposable Windows runs.

[Soak and recovery drills](dev/soak-and-recovery-drills.md), [encoder-quality measurement](dev/encoder-quality-matrix.md) and [static analysis](dev/static-analysis.md) are focused runbooks. Repository-local READMEs describe the adjacent tool rather than duplicating these contracts.

## Maintaining this system

Before adding a document, ask whether a future contributor needs the information to understand or safely modify the current system. If not, leave it in the pull request, tests or private untracked working notes. If yes, update the existing authority above. Add a new document only for a subject with no suitable owner.

Product changes update the product specification in the same change. Architecture changes preserve current ownership, ordering, failure and security constraints. Developer guides describe commands that exist. Accepted future designs state `Status: Accepted, not implemented` or the precise remaining unimplemented scope. Once implemented, distribute their current contracts to the appropriate authorities and remove the design document.

Do not add numbered decision archives, completed implementation plans, duplicate product specifications, generated audit histories or links to private working material. A rejected approach earns space only when its rationale prevents a plausible correctness or safety regression. Preserve the lesson, not the sequence of attempts. `CHANGELOG.md` and legal component provenance have different, intentionally historical responsibilities.

Run the documentation checks described in [Build and test](dev/build-and-test.md) after changing navigation, moving documents or adding source references.
