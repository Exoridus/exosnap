# The release-verification guest

A Hyper-V virtual machine with a partition of the host GPU, built once from the recipe in `tools/vm/`, and cloned per run onto a differencing disk that is deleted afterwards.

It exists because the install, update and packaging gates used to run on the developer's own machine. Three of the four runner defects the rc19 campaign found were consequences of that and of nothing else: an accept gate that removed the build the decline gate needed, a killed soak whose recovery manifest made both MSI gates refuse to start, and a declined update that left the updater holding its own executable. Windows Sandbox removed those by starting clean every time, but it has no NVENC, no persistent tool set, and no image anybody can reuse or reason about.

What the guest buys, in order of importance:

- A **deterministic starting state**. Every run begins on the same bytes.
- A **desktop nobody is sitting in**. Output Duplication, the fullscreen-present gates and the overlay gates need a real, unlocked console session; taking one over on the developer's machine is exactly the uncoordinated input the agent rules forbid.
- A **control channel that needs no network**. PowerShell Direct works with the adapter disconnected, so an install gate cannot silently reach the internet.
- A **GPU**, unlike Sandbox. NVENC, D3D11 and a real display driver.

What it does not buy: isolation from host load. The guest shares CPU and GPU with whatever the developer is doing, so the soak class stays a quiet-machine run wherever it executes.

## Sizes

| Thing | Size | Where |
|---|---|---|
| golden VHDX | 25 to 35 GB in use, 60 GB dynamic maximum | the image root, `D:` by default |
| answer ISO | under 1 MB | beside the golden image |
| per-run differencing disk | a few hundred MB to a few GB, deleted after the run | `runs/<runId>/` under the image root |
| Windows 11 installation ISO | about 6 GB | wherever it was downloaded; not tracked, not copied |

Memory 8 GB static, 4 vCPU, and a GPU partition. The partition triples are in Hyper-V's own units, which are documented as a range and not as a proportion of the adapter, and the platform may normalise a requested value, so a run reads the applied configuration back from `Get-VMGpuPartitionAdapter` and stops if it is not what was asked for. The display driver staged into the guest is the DriverStore package whose INF declares the version the host adapter is actually running, not the newest directory in the store: an update leaves the previous package behind and a rollback leaves the newer one.

None of the image, the ISO or the run disks is tracked. The recipe is, and the image is reproducible from it.

## Building it, once

Enabling the Hyper-V feature and adding the group need elevation once. After that, membership in `Hyper-V Administrators` is what the VM build, the freeze and the campaign commands rely on, and an ordinary shell carrying that membership is enough: the group exists precisely so virtual machines can be managed without elevation. An elevated shell works too, because its token carries Administrators, which has the same access. Starting an elevated PowerShell shows UAC on the secure desktop. **Yes** runs the named command as administrator and **No** cancels it without starting the script. An agent cannot answer that prompt, which is the reason to do the one-time setup and then work unelevated.

**1. The Hyper-V feature and the group.** `Get-VM` existing is not enough. An account outside `Hyper-V Administrators` gets Access denied from every management cmdlet.

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All -NoRestart
Add-LocalGroupMember -Group 'Hyper-V Administrators' -Member "$env:USERDOMAIN\$env:USERNAME"
```

Restart after the feature. Sign out and back in after the group: membership lives in the logon token, so the shell that added it still does not have it.

**2. The Windows 11 ISO.** Download the x64 ISO from Microsoft. Nothing in the recipe fetches it, and nothing checks its hash: it is Microsoft's file, named on the command line, and the guest it produces is verified by what runs in it. Use standard retail media that includes the inbox Microsoft App Installer package. Provisioning registers that package explicitly for the new local account before it invokes pinned winget packages. It refuses rather than downloading an unpinned Store bootstrap if the package is absent.

**3. Look at the plan before running it.** Unelevated, harmless, and the fastest way to see what the next command is about to do:

```powershell
pwsh -NoProfile -File tools/vm/New-ReleaseVm.ps1 -IsoPath <Win11 x64 ISO> -DryRun
```

**4. Build the machine.** About 30 to 60 minutes, most of it the unattended Windows installation. Once the group membership is in the logon token this runs unelevated:

```powershell
pwsh -NoProfile -File tools/vm/New-ReleaseVm.ps1 -IsoPath <Win11 x64 ISO>
```

Without that membership, the same script run from an elevated shell does the same work. The dry run prints that command when it applies.

The phases run in this order and can be run one at a time with `-Phase`:

The install phase pauses before starting the VM. Open its console in Hyper-V Manager, then press Enter in the recipe's terminal and immediately press a key in the VM console when the DVD boot prompt appears. This one-time confirmation is required by the standard Windows installation ISO. `autounattend.xml` applies only after Setup starts. The recipe never opens a console or synthesizes input. Subsequent campaign VMs boot from the installed disk and need no DVD confirmation.

| Phase | What it does | Why it is separate |
|---|---|---|
| create | disk, machine, vTPM, Secure Boot, both DVD drives, checkpoints off | n/a |
| install | waits for console coordination, boots once; `autounattend.xml` installs Windows and logs the console session on | ends with the machine stopped |
| gpu | MMIO window, `Add-VMGpuPartitionAdapter`, the partition triples | Hyper-V only attaches a partition to a stopped machine |
| driver | the host adapter's driver files, one `Copy-VMFile` per file | needs the machine running again |
| provision | `provision.ps1` over PowerShell Direct | needs a logged-on session |
| seal | removes what bringing the image up wrote, then refuses to freeze an image that still holds state of its own | runs last, after the probes of step 7 have left their output |

**5. Review the manifest.** The tracked manifest already contains the exact versions and hashes used by the recipe. See the next section before intentionally updating a tool.

**6. Let the VB-CABLE restart happen.** `provision.ps1` exits 2 when it has staged a driver that only appears after a reboot. `New-ReleaseVm.ps1` restarts the guest and runs it again by itself. Running provisioning by hand means doing that restart and re-running the script, which skips every completed step.

**7. Run the two probes inside the guest.** This is the gate on the whole design, and it is the developer's step because it is a real machine answering:

```powershell
# built on the host with -DEXOSNAP_BUILD_PROBES=ON, then copied into the guest
probe_gpup_nvenc.exe
probe_idd_duplication.exe
```

`probe_gpup_nvenc` must report `"ok":true` with a non-zero packet count: NVENC works through the partition. `probe_idd_duplication` must report one output, the configured mode, and frames arriving rather than only timeouts. Compare both against the same probes run on the host. **Either one failing moves the gates that need it back to the host**: that is a supported outcome, not a broken image.

**8. Freeze the image.** The `seal` phase clears what the probes wrote, refuses the freeze if anything else of the product's is still there, and stops the machine. Treat `golden.vhdx` as read-only from then on. Every run takes a differencing disk from it. A run that wrote into the parent would end the property the whole design exists for.

## Pinned provisioning

`tools/vm/provision-manifest.psd1` describes every third-party package the guest runs. Two kinds of pin, and both end in a SHA-256 comparison:

- `kind = 'winget'`: the package id plus an exact version. winget checks the installer against the hash in its own pinned manifest.
- `kind = 'download'`: a URL built from the version, plus the SHA-256 of the file at that URL. `provision.ps1` compares before it runs anything, and a mismatch deletes the download rather than leaving it on disk.

The tracked recipe currently pins Visual C++ runtime 14.51.36247.0, PowerShell 7.6.5.0, FFmpeg 9.0.1, PresentMon 2.5.1, SoundVolumeView 2.53, VB-CABLE pack 45, Virtual Display Driver 25.7.23, and NefCon 1.14.0. NefCon is required because the signed VDD archive contains the driver but no utility that can create its root-enumerated `Root\MttVDD` device.

`PIN-REQUIRED` remains a fail-closed sentinel for a newly added or deliberately upgraded package. Resolve it on the host without running the downloaded file:

```powershell
# a download
Invoke-WebRequest -Uri <url> -OutFile <file> -UseBasicParsing
Get-FileHash -Algorithm SHA256 <file>

# a winget package
winget show --id <package id> --versions
```

Put the version and hash in the manifest, run `scripts/tests/vm-recipe.tests.ps1`, and copy the manifest into the guest through the normal build phase. Do not freeze an image while any sentinel remains. SoundVolumeView's vendor URL is not versioned. Its hash still fails closed if the vendor replaces the bytes, but rebuilding that exact old image then requires an independently retained copy of the pinned archive.

The pins are the recipe. A guest whose tool set drifts turns every disagreement between two campaigns into an investigation of the image rather than of the product.

## Which image a campaign ran on

Two facts decide what an image is. `displayProfile` in the provisioning manifest says which virtual display driver the image is built with. `qualifiedDisplayProfile` says which one the capture work was qualified on. They are the MTT driver today, and they stay two fields even while they agree, because a scenario qualified on one driver must not quietly run on the other.

The qualified profile named SudoVDA until 2026-09-13, on the strength of the 4K120 Graphics Capture runs. The matched control that was run for exactly that question does not support singling it out: 4K120 captured 112.332 FPS with MTT against 112.716 with SudoVDA at the same GPU utilisation, and unattended Desktop Duplication was demonstrated on both once the capture path settles.

What SudoVDA offers is a dynamic monitor lifecycle for the test machine, which is a harness capability and not a capture-quality claim. The image that ran it was ReviOS-derived, which would put a non-stock Windows underneath every release gate. The recording application has never been qualified on SudoVDA.

Naming it therefore claimed something nobody measured, while making every scenario that requires the qualified profile unrunnable. Moving to it later is an image rebuild on stock Windows with its package pinned the way every package here is, plus qualifying the application on it.

A run measures this rather than trusting it. The guest is asked which virtual display driver it actually runs, by the root-enumerated device that driver binds to, and the answer is compared with the profile the scenario was qualified on. The comparison has three outcomes, not two: **qualified**, **not-qualified**, and **unverifiable** for a profile whose device identity the recipe has not recorded. Unverifiable means the scenario is unrunnable on that image, not failing on it. Nothing substitutes a device identity that was never measured, because a claim of qualification is exactly what an unpinned profile cannot support.

## Sealing an image

The last phase of a build is `seal`, and it exists because bringing an image up leaves state in it. The probes of step 7 and the experiments that drive them write under the product's own name (`C:\ProgramData\ExoSnap\DxgiDuplicationExperiment` and `C:\ExoSnap-DxgiTest`), and an image frozen with that in it is not the clean machine the first-start gate is premised on. That is not hypothetical: it is how the image frozen on 2026-09-11 reached a campaign, which reported an environment precondition instead of a product verdict.

Two steps, in this order, and the order is the point:

1. **Clear**, by exact path. Only what the recipe knows it created, listed in `BringUpArtifact`. A parent directory goes when clearing emptied it; a parent that still holds something is left alone. Nothing searches for residue: deleting what a sweep happened to match would turn an unexplained leftover into a silently clean image.
2. **Assert**, with the first-start gate's own residue definition, read out of the worker rather than restated. Anything left stops the freeze and is named. Recognise it and add it to `BringUpArtifact`; do not recognise it and find out what put it there.

An unknown leftover is never cleaned automatically. It is the one thing worth finding out about, and each one otherwise costs a full machine build to discover from the other end.

Beside the golden image sits `image-fingerprint.json`: the display profile, the monitor mode the gates assert against, a digest over every package pin, the Windows build, and the host GPU driver version the guest driver was staged from. The host driver is in there because the guest driver is copied from the host: changing it on the host changes the guest without anything in the guest being rebuilt, and the image has to be requalified.

A run can pin the image it needs. The check is the first step in the plan, before a differencing disk is created, and it names every drifted field at once. A fact the image never recorded counts as drift: an older image simply does not carry a field a later build of the recipe compares, and the absence of a record is not evidence that the two agree.

## What binds a run to the GPU it ran on

A partitioned guest shares no PCI identity with its host, and expecting it to is the mistake this recipe made first. What the guest binds to is the paravirtual device: it reports Microsoft's vendor id and a Microsoft inbox driver version, because the vendor's kernel-mode driver never leaves the host. A rule that required the host's vendor and device ids to appear in the guest refuses every correct campaign and accepts none.

Three independent assertions replace that one, and none of them compares PCI ids across the two operating systems:

| Assertion | Where both sides are measured | What it establishes |
|---|---|---|
| partition provenance | the host alone | the partition was created from the physical adapter the host measured. Hyper-V reports the partition's `InstancePath` as that adapter's PnP path with the separators rewritten, so this is the one place a PCI comparison belongs |
| the adapter | host and guest | the guest is on a paravirtual device that presents the host adapter. The GPU-P device carries the host GPU's friendly name; the Basic Render Driver, which is the same vendor and the fallback worth catching, does not |
| the driver | host and guest | the DriverStore package staged into the guest is the package the host selected, at the version the host adapter is running. The package is copied in verbatim, so both operating systems can be asked the same question, and a mismatch here is the classic GPU-P failure |

A run asks for all of this with `-ProveGpuBinding`. The host adapter is measured before anything is partitioned, the guest is held to it once the interactive agent has written its receipt, and the identity the run was measured under lands beside its evidence as `campaign-binding.json`: host adapter and package, the guest's receipt, and the display-profile verdict. Nothing is proven for a run that does not declare it: an install-and-uninstall scenario needs none of this and is not refused for it.

Capability (that Direct3D and NVENC are actually reachable through the partition) is not among these. It needs a probe running inside the guest rather than a fact Windows reports about a device, and this recipe does not claim it.

Display readiness is measured per display path, not per adapter. `Win32_VideoController` answers with a mode per adapter, and on a guest carrying an indirect display driver beside the synthetic one both adapters can report a mode that no display path is actually in, so a gate asking for 2560x1440 at 144 Hz is told it has one while the virtual monitor runs 60 Hz and the primary display runs 1024x768. `EnumDisplayDevices` and `EnumDisplaySettings` read the display device database rather than the calling session's desktop, so they answer from session 0, and one attached path has to be in the requested mode with its own resolution and its own refresh rate.

## Running one campaign

```powershell
pwsh -NoProfile -File tools/vm/Invoke-ReleaseVmRun.ps1 -RunId <id> -DryRun   # the plan
```

For real, in a shell with Hyper-V access (replace the artifact paths and run id):

```powershell
$artifacts = (Resolve-Path 'D:\rc-artifacts').Path
$harness = (Resolve-Path 'build/release-verify/publish').Path
$guestCommand = 'C:\ExoSnapRun\harness\campaign.cmd'
& tools/vm/Invoke-ReleaseVmRun.ps1 -RunId '<id>' -ArtifactDirectory $artifacts `
    -HarnessDirectory $harness -GuestCommand $guestCommand
```

`-GuestCommand` is required for execution. Supply `campaign.cmd` in the harness directory yourself, or pass the complete command for the campaign being tested. The wrapper must use the copied artifacts under `C:\ExoSnapRun\artifacts`, write evidence beneath `C:\ExoSnapRun\out`, and return its campaign exit code. This recipe does not invent an executable path or a campaign CLI contract. A dry run without `-GuestCommand` prints the missing-input precondition and the remaining infrastructure plan.

The campaign runner remains a separate integration dependency. Use the full harness CLI only after that implementation is available. This recipe's dry runs and probes do not establish that a Tier 2 campaign has run successfully.

The sequence: differencing disk from the golden image, a machine built around it with the same GPU partition, the network mode this run asked for, start, wait for PowerShell Direct, copy in the artifacts and the harness, run the campaign, copy the evidence back into the repository's private working directory under `release-verify/<runId>`, turn the machine off, delete it, delete the disk. VM and disk cleanup also runs when boot, transport, campaign or evidence collection throws. Use `-KeepDisk` only when preserving a failed guest for diagnosis is intentional. Cleanup is ownership-gated: a VM or disk is removed only if that run completed the step that created it, so a name or path collision cannot delete a pre-existing resource.

Network is `Disconnected` by default:

| `-Network` | Adapter | For |
|---|---|---|
| Disconnected | disconnected | everything by default. An install gate that can reach the internet is not measuring the artifact it was handed |
| HostOnly | an internal switch | a gate that needs IP between host and guest and nothing else |
| Connected | the external switch | gates whose subject is the network, such as a package manager fetching from a feed |

`-GuestCommand` is a string and defaults to the harness's `qualify` invocation. It is a string on purpose: it names a program built elsewhere in the tree, and this script has to be usable before that program exists.

## Host footprint after the cutover

The host keeps: the NVIDIA driver, `ffprobe`, `envctl`, Hyper-V.

The host loses, once Tier 2 runs in the guest: Intel PresentMon, VB-CABLE, SoundVolumeView, and the Windows Sandbox feature. They live in the guest image instead. The capability probe reports their absence as `UNAVAILABLE` on any host gate that would have needed them. Nothing fails for a missing tool, and nothing is green because one was missing.

PresentMon on the host stays worth reinstalling for exactly one case: confirming a present-diagnostics change or a driver major version against real hardware, when the cross-check gate says the recorded confirmation no longer applies.

## What is not supported, and why

- **Checkpoints.** A virtual machine with a GPU partition cannot be checkpointed at all, so the recipe disables checkpoints in the create phase, before there is a partition. A machine that already took an automatic checkpoint refuses the partition afterwards. The differencing disk is the replacement, and it is a better one: it resets the whole machine rather than a saved state.
- **Remote Desktop.** An RDP session is a different desktop, and Output Duplication does not work on it. The console session is the only session; it logs on automatically and stays logged on.
- **Live migration, saved state, and moving the image between machines.** The guest runs driver files copied from one specific host adapter. A different host means re-running the driver phase, and a host driver update means re-running it too.
- **Anything about real hardware.** Device clocks, physical unplug, a monitor powering off, an HDR panel, GPU thermals, colour judged by eye. Those are Tier 3 and stay on the developer's machine.
- **Secrets.** The guest's local administrator password is a constant of the recipe, in `autounattend.xml` and in `ReleaseVm.psm1`. The machine is disposable, local-only and has Remote Desktop off. Do not reuse the password anywhere, and do not give the guest a credential that means anything outside it.

## The pieces

| File | Runs on | What it is |
|---|---|---|
| `tools/vm/autounattend.xml` | Windows Setup | unattended install, local admin, console autologon, no OOBE |
| `tools/vm/provision-manifest.psd1` | n/a | every package, pinned by version and SHA-256 |
| `tools/vm/provision.ps1` | the guest | idempotent, resumable provisioning; written for Windows PowerShell 5.1 because it installs PowerShell 7 |
| `tools/vm/ReleaseVm.psm1` | the host | the plans, as data, plus the commands they name |
| `tools/vm/New-ReleaseVm.ps1` | the host, with Hyper-V access | builds the golden image |
| `tools/vm/Invoke-ReleaseVmRun.ps1` | the host, with Hyper-V access | one campaign on a differencing clone |
| `tools/probes/probe_gpup_nvenc` | either | NVENC through the partition, as JSON |
| `tools/probes/probe_idd_duplication` | either | Output Duplication per output, as JSON |
| `scripts/tests/vm-recipe.tests.ps1` | anywhere | the refusals, the plans and the pins, without a hypervisor |

See ADR 0071 for the decision and what was rejected.
