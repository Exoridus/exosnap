# Release verification

A release campaign binds checks to explicit candidate bytes, prepares declared environment state, invokes the real application and validates results with independent instruments. [Release checklist](../release-checklist.md) owns acceptance; [verification boundaries](../architecture/verification-boundaries.md) owns the trust model. [Live Verify](live-verify.md) documents the process protocol.

## Select regression qualification from the delta

Before repeating hardware qualification, record the baseline identity, implementation and environment delta, affected product contracts, selected lanes and remaining limits. Apply the [evidence reuse policy](../architecture/verification-boundaries.md#regression-qualification-and-evidence-reuse): select affected lanes, expand only when findings warrant it, and justify repeated manual operations. Keep reused baseline observations separate from results measured on the new binary.

The source registry in `tools/exo-verify/src/scenarios` is the executable catalog; inspect it with `exo-verify list`. The runner currently distinguishes `Required` (blocks without a result or explicit decision) and `Recommended` (reported, nonblocking). A release lane being bound to a bundle says which bytes a new observation measures; it does not make every scenario in that lane mandatory at every release.

| Qualification | Current registry / instrument | Selection triggers |
|---|---|---|
| Physical 44.1 kHz | `audio.endpoint-44100`, Recommended | WASAPI, format/rate negotiation, resampler, mixer, output conversion, clock slaving, rate-sensitive audio encoding or recovery |
| SYS/MIC endurance | `audio.mixed-clock-soak`, Recommended; longer clapper qualification in the release checklist | Audio/capture clocks, compensation, CFR scheduler, audio FIFO/threading, mix timeline, mux timestamps or pipeline scheduling |
| Endpoint loss/recovery | `audio.endpoint-degrade`, Recommended | Device discovery/recovery, source silence/timeline handling, default-device policy |
| HDR and refresh variants | `display.hdr-transaction`, `display.refresh-transaction`, Recommended | Capture/display/color/timing policy, driver or display changes |
| Webcam endurance | Physical endurance observation; `webcam.overlay-live` is only a short Recommended check | Camera lifecycle, frame generation, composition or scheduling |
| CPU/GPU/NVENC/storage pressure | Existing workload and pipeline evidence | Related scheduler, encoder, queue, mux, storage or resource-management changes |
| DPC cause and overlay presentation experiments | Focused investigation | Relevant provider/overlay/composition changes or concrete recording/presentation evidence |

`--only` selects the justified scenarios; avoid invoking an unfiltered hardware lane as a release ritual. Keep the baseline identity, scope rationale and retained limitations with the campaign. The current runner has no automatic source-diff classifier or `SKIPPED_REUSED_BASELINE` verdict: use the real unselected/missing status, and explain baseline reuse separately without inventing a measured PASS.

This selection does not alter an already frozen candidate plan. Several specialist checks, including `diagnostics.present-elevated`, `diagnostics.present-crosscheck` and `preview.cross-monitor`, currently remain Required in the registry. If the reviewed delta does not warrant repeating one, the maintainer records an explicit `exo-verify accept` decision against the new bundle, naming the prior evidence, unchanged contract and reason for reuse. A new observed failure requires `ACCEPTED_RISK`, not a reuse rationale. Recommended checks need no readiness override when unselected. Run all mandated candidate artifact gates against the exact files. Do not copy old hardware results into a new candidate's result set.

## Candidate and execution lanes

The `Build release candidate` workflow creates one official final-version build from a commit on `next`. Download `candidate-bundle/` and `candidate-plan.json` from the same Actions run. Keep them together with the separately signed disposable test-feed artifact. The bundle inventories the exact MSI, portable ZIP and verifier bytes; the plan freezes the source registry for that candidate.

```powershell
exo-verify bundle verify candidate-bundle
exo-verify plan verify --bundle candidate-bundle --plan candidate-plan.json
exo-verify list
exo-verify capabilities
exo-verify run --profile release-ci --lane release-ci-core --bundle candidate-bundle --out results/core
exo-verify run --profile release-ci --lane release-ci-install --bundle candidate-bundle --out results/install
exo-verify run --profile release-ci --lane release-ci-update --bundle candidate-bundle --out results/update
exo-verify run --profile release-gpu --bundle candidate-bundle --out results/gpu
# Set this list from the reviewed qualification scope; leave empty when none is selected.
if ($selectedHardwareScenarios) {
    exo-verify run --profile release-hardware --bundle candidate-bundle --only $selectedHardwareScenarios --out results/hardware
}
exo-verify report merge --bundle candidate-bundle --plan candidate-plan.json `
    --results results --out report
exo-verify status --bundle candidate-bundle --plan candidate-plan.json `
    --results results --report report/release-report.json
```

`run` selects where each lane runs from its scenarios' requirements (`--backend auto`, the default). A lane that needs a disposable OS runs itself on a runner attested with `--attest disposable-os`, otherwise inside Windows Sandbox or a Hyper-V VM. `--backend local|sandbox|hyperv` forces one for diagnostics and backend qualification. `--slot` names the qualification slot recorded with every result; it is derived from the environment when omitted. `exo-verify disposable probe` shows which backends this host can use.

The Hyper-V backend needs a sealed base image. Set `EXO_VERIFY_HYPERV_BASE` to its manifest, keep the VHDX read-only, and register the guest agent's socket service once per host from an elevated shell:

```powershell
exo-verify disposable register-hyperv-service
exo-verify disposable verify-base --manifest D:\ExoVerify\base\windows11.json
```

The base image is provisioned with `exo-guest install`, which starts the agent in the interactive session at every logon. Evidence of a Hyper-V run lands under the lane's output directory next to the VM description in `environment.json`.

Deterministic media regressions run without a candidate:

```powershell
exo-verify samples list
exo-verify samples run
exo-verify samples run media.* --out results/samples.result.json
```

`--update-reference` rewrites a sample's reviewed reference and prints the change. It is a maintainer action and never runs in CI.

Run installer and update lanes in a disposable guest. GPU and physical hardware lanes need declared capabilities and actual observations. `--only` narrows a diagnostic run; it does not remove a required scenario from the frozen plan. Keep results and media in a private untracked campaign directory. A missing lane is unavailable, never a pass.

The CI update lane uses the production embedded Ed25519 public key. A separate signing job reads the production private key only from its GitHub secret and uploads only the test manifest and detached signature. The key is never written to a file, log, result or artifact. The manifest describes the candidate package hashes and local test-feed URLs. It is served only by a loopback HTTPS server in the disposable update job, where a temporary hosts entry and certificate redirect `api.github.com`. No release or Stable feed receives that manifest. The official application rejects `--update-base-url`; no user setting or environment variable enables a feed override. The temporary network redirect changes discovery transport only. The product still performs release discovery, version comparison, Ed25519 signature verification and package SHA-256 verification.

The v0.9.0 Stable updater predates the injected UAC-decline fault.
The hosted update lane checks the accepting path against that baseline.
A real UAC refusal remains an external hardware-lane check.
The simulated decline is reserved for a later baseline that contains the fault seam.
The portable update from that baseline runs the v0.9.0 updater, which renames each directory exactly once.
A handle still open in the old tree after the application exits can therefore fail `update.portable` with "The current installation is in use and could not be moved."
One known holder: v0.9.0 always starts `crashpad_handler.exe` from the installation, the handler inherits the application's working directory and outlives it by up to about 50 ms.
When the application was started with its working directory inside the tree, as a launch from Explorer does, the single rename falls into that window.
The installation stays intact, and the updater offers Retry, which re-enters at the install step.
The bounded rename retry of later updaters cannot change that first hop.
The update scenarios record the evidence for such a failure as a timeline.
`console-<run id>.log` holds the old application's console and that of the updater it launches, with a receive time per line; the updater reports its failure only there.
`treeHolders` lists every process whose image or working directory lies in the old tree, and the application's children, with the time each ended relative to the application's exit.
It reads process state only and opens nothing in the tree, because any open file inside a directory blocks that directory's rename.
For the same reason `treeUsers`, the Restart Manager's list of processes using files in the tree, is taken only after the updater has reported the failure.

## Interpret both verdicts

| Product result | Meaning |
|---|---|
| PASS / FAIL | Measured correct / measured product failure |
| INFRA_ERROR | The instrument, parser, runner or required operation failed to establish product truth |
| UNAVAILABLE | Capability or precondition cannot be satisfied |
| SKIPPED | A scenario was not selected or run |

Exceptions escaping a scenario are infrastructure errors, not fabricated product failures. Unknown fields, malformed external JSON and absent tools are not zero-valued measurements.

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

Install/update tests should run in a disposable OS with copied artifacts and isolated configuration. A disposable backend reports completion only through the lane result the guest wrote; the launcher returning does not mean the guest completed. A missing or unreadable result is an infrastructure error for every selected scenario, never a partial PASS.

`EXOSNAP_UPDATE_FROM` / `EXOSNAP_UPDATE_FROM_MSI` identify an explicit older baseline for relevant gates. Ensure the target remains candidate-bound. MSI identity and hashes must be checked, not inferred from a neighboring filename.

A Chocolatey rehearsal can install/upgrade its `vcredist140` dependency. Do not attempt an unsafe system runtime downgrade to undo it. Record that footprint. The package source is not edited in place for a rehearsal; use a scratch copy with the tested MSI/hash.

## Operator protocol and faults

`--attest operator` declares that an operator is available for a named scenario. It does not attest that a result passed. The operator follows the scenario prompt and the independent measurement decides the result. Desktop input requires current coordination.

Real UAC cannot be clicked by an agent. The real decline scenario asks a human in an unelevated disposable guest to refuse the prompt and verifies the installed bytes and updater state. A separate `uacDeclined` fault seam only simulates refusal at the elevation call site. It cannot substitute for the real Secure Desktop observation.

Close a declined/failed updater before the next install gate; a running process holding its own file is not clean starting state. Human questions remain inside restoration scope even when the operator aborts or leaves.

## Field and media contracts

The diagnostics schema scenario verifies field existence across idle state, running pipeline and result. Empty collections prove their container exists, not every element's schema. Consumers must distinguish missing, unavailable and measured zero.

Frame analysis reports decode/count/timebase departures as values for the gate to judge. A baseline must contain no tested stimulus. Time alignment must not derive from the very feature under test. Use independent wall-clock, in-band marker or performance-counter evidence with declared uncertainties; at least two independent anchors must agree before a strong aligned verdict.

The engine logs video-epoch provenance. `frame_timestamp` is a measured source instant, `capture_observed` has acquisition uncertainty, and `session_start_floor` is only a bound. Select the correct session from an append-only log, never simply its last epoch.

Use the production control channel to record official artifacts. Harness-only `--auto-record` is not a release acceptance route. Keep stimulus duration/measurement windows distinct from synchronization; wait for actual state, events, device notification or bounded process completion.

## Qualification and evidence retention

Record each artifact hash, environment before/desired/applied/restored, independent observations, assertions, operator actions and evidence digests. A verdict whose evidence was not collected is not complete. `report merge`, `report verify` and `status` revalidate the bundle and source plan against lane results. A maintainer decision may accept an unavailable result or explicitly accept an observed product failure as a known risk. It never changes the measured verdict.

Every lane result names the attempt that produced it: run, attempt and job in GitHub Actions, a fresh identifier elsewhere.
A rerun is a new attempt, never a replacement.
The hosted lanes upload one artifact per attempt, and the report lists every attempt of a scenario in finishing order.
The effective verdict over the attempts of the planned revision is the most severe one.
A measured FAIL stays FAIL when a later attempt passes, and the entry is marked as having inconsistent attempts.
Only an ACCEPTED_RISK decision lets such a FAIL through.
An INFRA_ERROR or UNAVAILABLE attempt is resolved by a later measured verdict but stays in the history.
Attempts of another scenario revision are listed and never counted.
Supplying the same lane attempt twice is refused, because a copy is not independent evidence.

The hosted lanes upload their result JSON and focused diagnostic files.
Extracted package trees and copied executables are working data, not evidence.
Passing recordings are discarded unless explicitly requested.
Failures retain available logs, analyzer output, short media, dumps and screenshots.

`READY FOR APPROVAL` is a calculation, not release permission. The `Publish qualified release` workflow checks the frozen report and reuses the exact candidate MSI and ZIP bytes behind the `release` environment. Follow the [release checklist](../release-checklist.md#4-publication-boundary).

Prepare its qualification input after the final report is ready:

```powershell
exo-verify publication encode --bundle candidate-bundle --plan candidate-plan.json `
    --report report/release-report.json --decisions decisions.json `
    --results results --out publication-inputs.json
```

Omit `--decisions` when none were recorded. The output contains `qualification_sha256` and `qualification_base64` for workflow dispatch. Supply them together with `candidate_run` and `candidate_id`, using `next` at the same source revision as the candidate. The encoded document carries the frozen report, explicit decisions and external GPU/hardware result documents. Hosted results are downloaded independently from Actions, including all retained attempts; external input cannot replace those lanes. The input is bounded to the workflow dispatch size limit and never includes executables or signing keys. Preserve the underlying private evidence with the campaign; the workflow retains qualification records as private Actions artifacts.

Runner tests use fake environment/tools and hostile inputs to execute real gate logic. They test refusal, restoration, interrupted state, schema and evidence handling without changing the developer's machine. Their success does not claim the hardware or installer under test has been exercised.
