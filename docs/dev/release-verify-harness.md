# Typed release-verification harness

`tools/release-verify` contains `ExoSnap.Verify`, a typed Windows verification program. It owns scenario execution, external-tool contracts, evidence and local qualification records. It does not tag or publish releases. [Campaign operation](release-verify.md) and [release acceptance](../release-checklist.md) are separate authorities.

## Build and test

Run from `tools/release-verify` so `global.json` selects the pinned SDK and Microsoft.Testing.Platform configuration:

```powershell
dotnet restore ExoSnap.Verify.slnx --locked-mode
dotnet build ExoSnap.Verify.slnx --no-restore -warnaserror
dotnet test ExoSnap.Verify.slnx --no-restore
dotnet publish ExoSnap.Verify/ExoSnap.Verify.csproj -p:PublishProfile=win-x64
```

The self-contained publish is suitable for a release machine without a matching installed runtime. Full verification and CI include the harness tests. A changed NuGet dependency must update the deliberate lock/pin, not be accepted by a floating restore.

| Area | Ownership |
|---|---|
| Models / JSON contracts | Typed outcomes, capabilities, descriptors, run state, qualification and evidence serialization |
| Engine / Catalog | Selection, dependency ordering, campaign binding, result persistence and eligibility |
| Adapters / Processes / LiveVerify | ffprobe, envctl, PresentMon parsing, process lifetime and product protocol |
| Gates / Analysis | Scenario bodies and shared media/timebase analysis |
| Windows / Worker | Native APIs, job objects, COM and separate elevated execution |
| Tests / Fixtures | Pure gate tests, tool-contract fixtures, adversarial child processes and targeted platform smoke |

Build output stays untracked. Runtime source names and catalog IDs are product/test contracts, not places for development-provenance identifiers.

## CLI

| Command | Contract |
|---|---|
| `capabilities [--out <path>]` | Read-only machine capability document, unknown when unmeasured |
| `list [--json]` | Current descriptors and available bodies |
| `catalog [--out <path>] [--check]` | Deterministic Markdown rendering, or exact checked-copy comparison |
| `prepare --exe <path> [--rc <tag>] [--commit <sha>] [--package <path>]...` | Explicit artifact/package binding and machine measurement |
| `run [--id <id>] [--class <class>] [--include-opt-in]` | Execute selected scenarios and record distinct verdicts |
| `report` | Report recorded outcomes without inventing missing results |
| `qualify [--required <id>]...` | Revalidate bindings/evidence and export the local eligibility record |
| `qualify --dry-run [--rc <tag>] [--include-opt-in] [--class <class>] [--id <id>]` | Capability/selection preview only; never a successful qualification |

Unknown scenario IDs are errors, not empty selections. Run and qualify revalidate executable hash and prepared catalog identity. Changed bytes/catalog require a new valid binding; old results must not be silently attributed to them.

## Generated catalog

The [catalog page](release-verify-catalog.md) is generated from `Catalog/ReleaseCatalog.cs` and `CatalogStatusPage.cs`. Requirement Source fields point to current documents; changing prose in the generated page alone is not sufficient.

```powershell
cd tools/release-verify
dotnet run --project ExoSnap.Verify -- catalog --out ../../docs/dev/release-verify-catalog.md
```

`CatalogStatusPageTests.TheCommittedCatalogPageMatchesTheCatalog` enforces byte parity. The explicit fixture-writing environment switch can regenerate approved fixtures during a deliberate change, but never hides an unexplained failure. The catalog version is derived from serialized descriptors, so even a documentation-source change invalidates an older prepared catalog. This is intentional evidence binding, not a version to manually preserve.

All current catalog declarations have bodies. That says nothing about successful execution: a body can require an unavailable instrument, candidate binding, elevated worker, physical action or visual judgment. A future declaration without a body must remain nonpassing. The PowerShell wrapper and publisher use their own source catalog/policy; do not assume this catalog's count or default required set replaces them.

## Outcome and qualification rules

Pass means the scenario proved the expected product behavior. Fail means the product did not satisfy it. InfrastructureError means the measurement could not be carried out correctly. Blocked/Unavailable, Deferred, Skipped and Stale remain separate nonpassing outcomes. Exceptions are caught at the engine boundary as infrastructure failures; individual gates should not convert parser/process failures into product defects.

Required scenarios need exactly one passing verdict. Any recorded product failure or infrastructure error disqualifies even when the scenario was opt-in. Evidence gaps on a nominal PASS and unrestored environment state also block. Requalification removes an obsolete exported record before evaluating a new result.

A local qualification record still needs to satisfy the final publisher's signed-record schema, canonical release policy, candidate/package identity and promotion contract. Exercise that integration explicitly. The PowerShell publication checker is not obligated to accept a smaller required set merely because a typed run called itself qualified.

## Capabilities and adapters

Requirements name measured capability keys, not device model names. A missing/unknown value satisfies nothing. `display.hdr` means currently active HDR, not panel marketing capability. Device aliases must be bound unambiguously. Tool overrides include `EXOSNAP_FFPROBE`, `EXOSNAP_PRESENTMON` and `EXOSNAP_SOUNDVOLUMEVIEW`; retain executable identity when parsing version-dependent output.

`IFfprobe`, `IEnvctl`, `ILiveVerifySession` and `IPresentMon` isolate external mechanisms. Gates test through those contracts. ffprobe stream duration is measured from packet spans where needed, not assumed from optional duration tags. PresentMon CSV columns are matched by header; absent optional columns remain null rather than shifting positional interpretation.

Process execution preserves exit code, stdout/stderr, deadlines, cancellation and descendant cleanup. Paths with spaces, ampersands or non-ASCII characters must be passed as arguments, not shell-concatenated commands. Bounded output prevents a child that floods a stream from exhausting the harness. Malformed/empty JSON and locale-incompatible numeric output are contract failures, not silently coerced values.

## Windows and isolation

Hermetic tests need no device/UI/registry. Desktop scenarios need a real application context. Disposable OS scenarios own install/update/registry footprint. HardwareLab scenarios declare real capture/encoder/HDR/clock requirements. Tier labels summarize those boundaries rather than granting capabilities.

Application sessions use isolated configuration and job-object ownership. Offscreen mode avoids taking focus for tests that need no visible desktop, but cannot satisfy native-window shutdown, capture or overlay requirements. A shutdown request that cannot be sent is `NotRequestable`, not a timed-out close and not success.

Environment mutations always restore in `finally`, with independent readback. Elevation uses a separate worker/result document; no cross-integrity UI inspection. UI Automation can read existence/text, while actual capture-excluded color/alpha remains an operator judgment. Neither a synthetic CSV fixture nor a stub tool establishes real ETW delivery.

## Test strategy

Test gate decisions against fakes, adapters against declared captured/synthetic fixtures, and narrowly scoped platform behavior against the actual mechanism. Label synthetic evidence as synthetic. Use the hostile fixture child instead of a shell whose quoting rules would become the subject of the test.

A skipped smoke because a local executable/tool is absent is a statement about environment reach. Keep it visible. A full green unit suite does not mean every scenario ran on a real release candidate or that the final publish lock accepted its record.
