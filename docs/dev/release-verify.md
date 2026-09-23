# Release verification

A release campaign binds checks to explicit candidate bytes, prepares declared environment state, invokes the real application and validates results with independent instruments. [Release checklist](../release-checklist.md) owns acceptance; [verification boundaries](../architecture/verification-boundaries.md) owns the trust model. [Live Verify](live-verify.md) documents the process protocol.

## Select the execution engine

```powershell
pwsh scripts/release-verify.ps1 run
pwsh scripts/release-verify.ps1 run -Engine DotNet
```

PowerShell is the wrapper's default. `-Engine DotNet` selects the typed `ExoSnap.Verify` implementation and builds/publishes it when necessary. It forwards prepare, run, list, qualify and report. Wrapper recover/resume/retry/status have no equivalent in that selection and are refused rather than emulated.

The implementations have distinct catalogs and capabilities. The [typed catalog](release-verify-catalog.md) contains concrete bodies for its declarations, but a body can still report unavailable/deferred. Do not infer candidate qualification from body presence, or assume the two catalogs have identical required sets. The final publisher's source catalog and [release policy](../../scripts/lib/release-policy.json) remain decisive.

## Prepare and run

Use the exact published candidate, not a build tree with convenient neighboring tools:

```powershell
pwsh scripts/release-verify.ps1 prepare `
    -ExePath '<candidate install>\exosnap.exe' -Tag '<candidate tag>' `
    -SourceCommit '<full commit>' `
    -PortableZip '<published portable ZIP>' -Msi '<published MSI>'
pwsh scripts/release-verify.ps1 list
pwsh scripts/release-verify.ps1 run
pwsh scripts/release-verify.ps1 run -Only REL-CAP-001
pwsh scripts/release-verify.ps1 run -IncludeClass display,audio-physical
pwsh scripts/release-verify.ps1 status
pwsh scripts/release-verify.ps1 report
pwsh scripts/release-verify.ps1 qualify
```

The runner reports a private untracked campaign directory holding identity, state, evidence and reports. Missing commit/package identity can permit a diagnostic run but cannot support release qualification. A default run excludes opt-in long/disruptive classes; selection and qualification requirements are separate decisions.

For PowerShell, `resume` refingerprints and continues, `retry -Only <id>` explicitly reattempts a finding, and `recover` restores dirty environment state without running product checks. An existing FAIL is not silently erased by rerunning a sweep. `-NonInteractive` defers questions rather than approving them.

## Interpret both verdicts

| Product result | Meaning |
|---|---|
| PASS / FAIL | Measured correct / measured product failure |
| INFRA_ERROR | The instrument, parser, runner or required operation failed to establish product truth |
| UNVERIFIED / STALE | Attempt without usable outcome / evidence bound to changed inputs |
| UNAVAILABLE / BLOCKED | Capability/precondition cannot be satisfied |
| DEFERRED / SKIPPED | Human/decision postponement / not selected |

The typed engine uses its corresponding named enum outcomes. Exceptions escaping a gate are infrastructure errors, not fabricated product failures. Unknown fields, malformed external JSON and absent tools are not zero-valued measurements.

Environment restoration is reported separately: not applicable, restored, restore pending, pending because the original device is unavailable, or failed. Product PASS with failed restore is not a releasable campaign. Report/JUnit must expose both.

## Environment preparation

Use `exosnap-envctl describe`, `snapshot`, `resolve-aliases` and `list-modes` before deciding what a machine supports. Bind scenario aliases deliberately using the tool's `bind-alias` interface. A fresh unbound profile is not permission to choose the first device; two matching devices are ambiguous, not interchangeable.

The capability taxonomy distinguishes application-owned state, safely mutable environment state, read-only environment facts, human-only environment actions, physical actions and Secure Desktop. The application control endpoint never gains a Windows-administration command merely because a scenario needs it.

Every safe environment change follows this contract:

1. Read the exact original state and durably journal it before mutation.
2. Validate the requested change, apply only the delta and read the result back.
3. Compare actual with requested before running the product assertion.
4. In a `finally` path, restore the exact original, read back and close only when verified.

Use one machine-wide journal across campaigns; `EXOSNAP_ENV_JOURNAL` must agree between runner and tool when overriding its location. A failed begin can leave a failed rollback. Unknown state is not clean. No new mutating scenario runs over an unresolved journal. If the original device disappears, restoration stays pending for that device; no substitute is chosen. A guard process helps recover from owner death but does not replace journal recovery after an OS/power failure.

Refresh values must use the tool's enumerated/current Windows vocabulary. A nominal 60 Hz request may read back as 59; success from the setter is insufficient. Do not weaken exact restoration to a tolerance. Select another supported same-resolution/depth/orientation mode or record the refused precondition.

## Tools, elevated operations and disposable systems

| Mechanism | Purpose / selection |
|---|---|
| ffprobe | Independent streams, tags and packet spans; pin the executable with the relevant tool override |
| PresentMon | Independent present-path evidence; `EXOSNAP_PRESENTMON` |
| SoundVolumeView | Explicit external endpoint/default/format operation where envctl deliberately has no supported writer; `EXOSNAP_SOUNDVOLUMEVIEW` |
| VB-CABLE or a named quiet endpoint | Silence-versus-loss scenario; `EXOSNAP_SILENT_AUDIO_ENDPOINT` |
| pnputil / explicit visibility tool | Deliberate device loss in a disposable or coordinated hardware test |
| Windows Sandbox | Disposable installer/update/package-manager execution |
| Hyper-V guest | Pinned disposable OS with declared GPU/console capabilities; [guest guide](release-verify-vm.md) |
| UI Automation | Native window/text/control existence, not a pixel-color oracle |

External tools are test mechanisms, not shipped runtime dependencies. Pin their versions for reproducible evidence, never silently install or guess a similarly named program. A missing tool reports the exact precondition and cannot pass its scenario.

The audio device-format/default-role properties can be human-only in envctl because their setter is not a supported public Windows contract. An explicitly selected external tool can perform the test operation outside that boundary; envctl still independently reads it back, and the runner restores what was changed. This does not add an undocumented API to the product or envctl.

Install/update tests should run in a disposable OS with copied artifacts and isolated configuration. Sandbox workers report completion with a marker/result document; the launcher returning does not mean the guest completed. Missing marker, unreadable result or a step never reached is unverified/infrastructure failure, never a partial PASS.

`EXOSNAP_UPDATE_FROM` / `EXOSNAP_UPDATE_FROM_MSI` identify an explicit older baseline for relevant gates. Ensure the target remains candidate-bound. `EXOSNAP_RELEASE_MSI` can explicitly select the intended installer. MSI identity and hashes must be checked, not inferred from a neighboring filename.

A Chocolatey rehearsal can install/upgrade its `vcredist140` dependency. Do not attempt an unsafe system runtime downgrade to undo it. Record that footprint. The package source is not edited in place for a rehearsal; use a scratch copy with the tested MSI/hash.

## Operator protocol and faults

Prompts distinguish **Enter to start the runner's action** from **Enter after completing the operator action**. Detail/help, skip and abort remain explicit. `-Attest <id>` says an action has already occurred and skips the action prompt; the independent Verify step still decides. It cannot attest someone else's visual judgment.

Real UAC cannot be clicked by an agent. An elevated worker is a separate process with a result-file boundary, not cross-integrity UI inspection. A deliberate updater `uacDeclined` fault seam only simulates refusal at the elevation call site. It can never bypass verification or make an install succeed and is not evidence that a real Secure Desktop interaction occurred.

Close a declined/failed updater before the next install gate; a running process holding its own file is not clean starting state. Human questions remain inside restoration scope even when the operator aborts or leaves.

## Field and media contracts

`REL-SCHEMA-001` verifies field existence across idle state, running pipeline and result. Empty collections prove their container exists, not every element's schema. Its consumers must distinguish missing, unavailable and measured zero. Keep declared consumers aligned with current scenario IDs.

Frame analysis reports decode/count/timebase departures as values for the gate to judge. A baseline must contain no tested stimulus. Time alignment must not derive from the very feature under test. Use independent wall-clock, in-band marker or performance-counter evidence with declared uncertainties; at least two independent anchors must agree before a strong aligned verdict.

The engine logs video-epoch provenance. `frame_timestamp` is a measured source instant, `capture_observed` has acquisition uncertainty, and `session_start_floor` is only a bound. Select the correct session from an append-only log, never simply its last epoch.

Use the production control channel to record official artifacts. Harness-only `--auto-record` is not a release acceptance route. Keep stimulus duration/measurement windows distinct from synchronization; wait for actual state, events, device notification or bounded process completion.

## Qualification and evidence retention

Record each artifact hash, environment before/desired/applied/restored, independent observations, assertions, operator actions and evidence digests. A verdict whose evidence was not collected is not complete. Qualification revalidates source/catalog/artifact bindings and required results; the publisher independently re-derives that decision and verifies its signature.

`qualify -Publish` is a separately authorized maintainer operation, not the normal end of an agent run. A printed local verdict does not establish that the record passes the publisher's current catalog/policy/signature requirements. Follow [release checklist](../release-checklist.md#3a-bind-qualify-and-promote) for the complete boundary.

Runner tests use fake environment/tools and hostile inputs to execute real gate logic. They test refusal, restoration, interrupted state, schema and evidence handling without changing the developer's machine. Their success does not claim the hardware or installer under test has been exercised.
