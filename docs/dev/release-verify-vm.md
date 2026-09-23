# Disposable release-verification guest

The `tools/vm` recipe builds a Windows Hyper-V base image and runs campaigns on disposable differencing disks. The base is read-only after sealing. Its purpose is reproducible install/update state and a desktop nobody is actively using, not isolation from the host's CPU/GPU load.

[Verification boundaries](../architecture/verification-boundaries.md) defines what a guest proves. Physical device clocks, real monitor power/unplug, HDR-panel appearance and thermal behavior still require appropriate hardware checks.

## Prerequisites

Use a host with Hyper-V, suitable GPU partitioning and enough disk/memory for the declared image. The recipe defaults to a dynamically growing disk, 8 GB static memory and four virtual CPUs; inspect the plan/manifest before relying on sizes or adjusting them. GPU partition settings use Hyper-V's units and must be read back, not interpreted as an assumed percentage.

Enable Hyper-V and authorize VM management once, with explicit administrator coordination:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All -NoRestart
Add-LocalGroupMember -Group 'Hyper-V Administrators' -Member "$env:USERDOMAIN\$env:USERNAME"
```

Restart after enabling the feature. New group membership needs a new logon token. An ordinary process carrying the required group access can manage the recipe; an unauthorized process must not treat Access denied as missing hardware. UAC remains the operator's Secure Desktop action.

Supply a Windows 11 x64 ISO yourself. Use compatible retail media including the inbox App Installer package expected by provisioning. The recipe does not authenticate Microsoft's ISO for you. Validate your installation media through its distribution source before using it as a trusted base.

## Build, provision, probe and seal

```powershell
pwsh -NoProfile -File tools/vm/New-ReleaseVm.ps1 -IsoPath '<Windows ISO>' -DryRun
pwsh -NoProfile -File tools/vm/New-ReleaseVm.ps1 -IsoPath '<Windows ISO>'
```

Review the dry-run plan first. The phases are create, install, gpu, driver, provision and seal, selectable individually with `-Phase`. Installation requires the one-time console DVD-boot confirmation; the script never opens a console or synthesizes that input. Subsequent runs boot the installed disk.

Provisioning is idempotent. If a staged driver needs a reboot, the wrapper restarts/retries; a manual provisioning run must honor the same outcome. Driver staging selects the host adapter's active DriverStore package, not simply the newest directory, because a rollback can leave newer unused files behind.

`tools/vm/provision-manifest.psd1` is the exact package-version/hash authority. Winget entries pin a version and rely on that manifest's installer hash; downloads pin a SHA-256 checked before execution. `PIN-REQUIRED` is a refusal sentinel. An unversioned vendor URL can change: retaining the exact checked archive may be necessary to rebuild the same image later.

Before sealing, run the real GPU/capture probes in the guest and retain results:

```powershell
probe_gpup_nvenc.exe
probe_idd_duplication.exe
```

Nonzero encoded packets and actual frames from the intended output matter; an adapter name or open API handle does not prove usable NVENC/duplication. Compare against declared host expectations. Failure makes the relevant scenario unavailable on that image, not a reason to mark it passed or silently use a different path.

Seal removes only explicitly listed recipe-created artifacts, then applies the first-start residue assertion. Unknown leftovers are named and stop sealing; do not silently clean arbitrary matching paths. Stop the guest and preserve the sealed parent. Store Windows, driver/package pins, display profile and monitor mode in `image-fingerprint.json`.

## Display/GPU identity

`displayProfile` describes what the recipe installs; `qualifiedDisplayProfile` describes what the scenario is qualified against. They are separate facts. Current pins select the MTT profile; consult the manifest rather than carrying an experimental driver comparison into a support claim.

A GPU-partitioned guest can expose a Microsoft paravirtual vendor/device identity. Do not require it to have the host's PCI IDs. Establish partition provenance on the host, the presented adapter identity in the guest, and the staged driver package/version independently. Basic rendering fallback is not the intended partition. Capability still requires a real probe.

Display readiness is per attached display path, resolution and refresh. Adapter-wide WMI mode fields can describe a different path. An interactive console receipt proves the desktop/session that a capture campaign needs; PowerShell Direct responding proves only that the guest OS is reachable.

A changed host driver, base image, virtual-display profile or package pin requires appropriate requalification. Copying driver files once does not make a future host change compatible automatically. Fingerprint unknowns are not matches.

## Run a campaign

```powershell
pwsh -NoProfile -File tools/vm/Invoke-ReleaseVmRun.ps1 -RunId '<run-id>' -DryRun
$artifacts = (Resolve-Path '<candidate artifact directory>').Path
$harness = (Resolve-Path '<published harness directory>').Path
pwsh -NoProfile -File tools/vm/Invoke-ReleaseVmRun.ps1 `
    -RunId '<run-id>' -ArtifactDirectory $artifacts -HarnessDirectory $harness `
    -GuestCommand 'C:\ExoSnapRun\harness\campaign.cmd'
```

Provide `campaign.cmd` or another complete command deliberately. Execution has no safe implicit artifact/campaign path. The command must use copied candidate files under `C:\ExoSnapRun\artifacts`, write evidence beneath `C:\ExoSnapRun\out` and return the real campaign exit code. A recipe dry run or successful probe does not establish that the full campaign ran.

For a capture campaign, declare `-RequireInteractiveGuest`; for binding-sensitive work use `-ProveGpuBinding`, which also requires interactive evidence. Verify the resulting readiness/agent receipt and actual launch session. Merely starting a program through session-0 PowerShell Direct is not equivalent to running it on the console desktop.

The runner creates a differencing disk/VM, configures the declared partition/network, waits for transport/readiness, copies inputs, runs the supplied command, collects evidence and removes its own VM/disk. Cleanup runs on failures too and is ownership-gated; a colliding pre-existing name must never be deleted. `-KeepDisk` intentionally preserves failure state and requires later manual cleanup.

| Network selection | Use |
|---|---|
| Disconnected (default) | No guest network; PowerShell Direct still provides host control |
| HostOnly | Internal-switch communication explicitly needed by the scenario |
| Connected | External network only for a scenario whose subject requires it |

No campaign receives reusable production credentials. The recipe's local administrator credentials belong only to this disposable, non-RDP guest and must not be reused elsewhere.

## Limits and recovery

Do not use checkpoints, live migration or saved state as a substitute for the differencing-disk model on a GPU-partitioned VM. Do not substitute an RDP desktop for the qualified interactive console. Do not infer general capture support from the availability of a virtual adapter.

The guest shares host resources, so endurance/performance runs still need a quiet, controlled host. Keep physical hardware gates on equipment that can establish their subject. A virtual cable or virtual display validates a software path, not every user's device.

Run `scripts/tests/vm-recipe.tests.ps1` after recipe changes. It validates plans, pins, refusals and cleanup logic without creating a real VM; image provisioning and GPU/capture reach require the separate live probe evidence.
