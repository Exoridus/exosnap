# The release-verification guest

A Hyper-V virtual machine with a partition of the host GPU, built once from the recipe
in `tools/vm/`, and cloned per run onto a differencing disk that is deleted afterwards.

It exists because the install, update and packaging gates used to run on the
developer's own machine. Three of the four runner defects the rc19 campaign found were
consequences of that and of nothing else: an accept gate that removed the build the
decline gate needed, a killed soak whose recovery manifest made both MSI gates refuse
to start, and a declined update that left the updater holding its own executable.
Windows Sandbox removed those by starting clean every time, but it has no NVENC, no
persistent tool set, and no image anybody can reuse or reason about.

What the guest buys, in order of importance:

- A **deterministic starting state**. Every run begins on the same bytes.
- A **desktop nobody is sitting in**. Output Duplication, the fullscreen-present gates
  and the overlay gates need a real, unlocked console session; taking one over on the
  developer's machine is exactly the uncoordinated input the agent rules forbid.
- A **control channel that needs no network**. PowerShell Direct works with the
  adapter disconnected, so an install gate cannot silently reach the internet.
- A **GPU**, unlike Sandbox. NVENC, D3D11 and a real display driver.

What it does not buy: isolation from host load. The guest shares CPU and GPU with
whatever the developer is doing, so the soak class stays a quiet-machine run wherever
it executes.

## Sizes

| Thing | Size | Where |
|---|---|---|
| golden VHDX | 25 to 35 GB in use, 60 GB dynamic maximum | the image root, `D:` by default |
| answer ISO | under 1 MB | beside the golden image |
| per-run differencing disk | a few hundred MB to a few GB, deleted after the run | `runs/<runId>/` under the image root |
| Windows 11 installation ISO | about 6 GB | wherever it was downloaded; not tracked, not copied |

Memory 8 GB static, 4 vCPU, and a tenth of the GPU. None of the image, the ISO or the
run disks is tracked; the recipe is, and the image is reproducible from it.

## Building it, once

Every step here is elevated and every one of them is the developer's own action: an
agent may print these commands and may not run them. Run them in order.

**1. The Hyper-V feature and the group.** `Get-VM` existing is not enough; an account
outside `Hyper-V Administrators` gets Access denied from every management cmdlet.

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All
Add-LocalGroupMember -Group 'Hyper-V Administrators' -Member "$env:USERDOMAIN\$env:USERNAME"
```

Restart after the feature. Sign out and back in after the group: membership lives in
the logon token, so the shell that added it still does not have it.

**2. The Windows 11 ISO.** Download the x64 ISO from Microsoft. Nothing in the recipe
fetches it, and nothing checks its hash: it is Microsoft's file, named on the command
line, and the guest it produces is verified by what runs in it.

**3. Look at the plan before running it.** Unelevated, harmless, and the fastest way
to see what the next command is about to do:

```powershell
pwsh -NoProfile -File tools/vm/New-ReleaseVm.ps1 -IsoPath <Win11 x64 ISO> -DryRun
```

**4. Build the machine.** Elevated. About 30 to 60 minutes, most of it the unattended
Windows installation.

```powershell
Start-Process -FilePath pwsh -Verb RunAs -ArgumentList '-NoProfile','-File','tools/vm/New-ReleaseVm.ps1','-IsoPath','<Win11 x64 ISO>'
```

The phases run in this order and can be run one at a time with `-Phase`:

| Phase | What it does | Why it is separate |
|---|---|---|
| create | disk, machine, vTPM, Secure Boot, both DVD drives, checkpoints off | -- |
| install | boots once; `autounattend.xml` installs Windows and logs the console session on | ends with the machine stopped |
| gpu | MMIO window, `Add-VMGpuPartitionAdapter`, the partition triples | Hyper-V only attaches a partition to a stopped machine |
| driver | the host adapter's driver files, one `Copy-VMFile` per file | needs the machine running again |
| provision | `provision.ps1` over PowerShell Direct | needs a logged-on session |

**5. Pin the manifest.** See the next section. Provisioning stops on the first package
whose version or hash still reads `PIN-REQUIRED`, which is deliberate.

**6. Let the VB-CABLE restart happen.** `provision.ps1` exits 2 when it has staged a
driver that only appears after a reboot. `New-ReleaseVm.ps1` restarts the guest and
runs it again by itself; running provisioning by hand means doing that restart and
re-running the script, which skips every completed step.

**7. Run the two probes inside the guest.** This is the gate on the whole design, and
it is the developer's step because it is a real machine answering:

```powershell
# built on the host with -DEXOSNAP_BUILD_PROBES=ON, then copied into the guest
probe_gpup_nvenc.exe
probe_idd_duplication.exe
```

`probe_gpup_nvenc` must report `"ok":true` with a non-zero packet count: NVENC works
through the partition. `probe_idd_duplication` must report one output, the configured
mode, and frames arriving rather than only timeouts. Compare both against the same
probes run on the host. **Either one failing moves the gates that need it back to the
host** -- that is a supported outcome, not a broken image.

**8. Freeze the image.** Stop the machine, and treat `golden.vhdx` as read-only from
then on. Every run takes a differencing disk from it; a run that wrote into the parent
would end the property the whole design exists for.

## Pinning the manifest

`tools/vm/provision-manifest.psd1` describes every third-party package the guest runs.
Two kinds of pin, and both end in a SHA-256 comparison:

- `kind = 'winget'`: the package id plus an exact version. winget checks the installer
  against the hash in its own pinned manifest.
- `kind = 'download'`: a URL built from the version, plus the SHA-256 of the file at
  that URL. `provision.ps1` compares before it runs anything, and a mismatch deletes
  the download rather than leaving it on disk.

A pin that reads `PIN-REQUIRED` has not been recorded yet, and provisioning refuses to
proceed past it. Recording one is a one-time job done inside the guest, with the
network connected, while the image is being built:

```powershell
# a download
Invoke-WebRequest -Uri <url> -OutFile <file> -UseBasicParsing
Get-FileHash -Algorithm SHA256 <file>

# a winget package
winget show --id <package id> --versions
```

Put the version and the hash in the manifest on the host, commit them, copy the
manifest back into the guest, and continue. The image is not frozen until every pin is
a real value.

The pins are the recipe. A guest whose tool set drifts turns every disagreement
between two campaigns into an investigation of the image rather than of the product.

## Running one campaign

```powershell
pwsh -NoProfile -File tools/vm/Invoke-ReleaseVmRun.ps1 -RunId <id> -DryRun   # the plan
```

Elevated, for real:

```powershell
Start-Process -FilePath pwsh -Verb RunAs -ArgumentList '-NoProfile','-File','tools/vm/Invoke-ReleaseVmRun.ps1','-RunId','<id>','-ArtifactDirectory','<rc artifacts>','-HarnessDirectory','<published harness>'
```

The sequence: differencing disk from the golden image, a machine built around it with
the same GPU partition, the network mode this run asked for, start, wait for
PowerShell Direct, copy in the artifacts and the harness, run the campaign, copy the
evidence back into the repository's private working directory under
`release-verify/<runId>`, turn the machine off, delete it, delete the disk.

Network is `Disconnected` by default:

| `-Network` | Adapter | For |
|---|---|---|
| Disconnected | disconnected | everything by default. An install gate that can reach the internet is not measuring the artifact it was handed |
| HostOnly | an internal switch | a gate that needs IP between host and guest and nothing else |
| Connected | the external switch | gates whose subject is the network, such as a package manager fetching from a feed |

`-GuestCommand` is a string and defaults to the harness's `qualify` invocation. It is a
string on purpose: it names a program built elsewhere in the tree, and this script has
to be usable before that program exists.

## Host footprint after the cutover

The host keeps: the NVIDIA driver, `ffprobe`, `envctl`, Hyper-V.

The host loses, once Tier 2 runs in the guest: Intel PresentMon, VB-CABLE,
SoundVolumeView, and the Windows Sandbox feature. They live in the guest image
instead. The capability probe reports their absence as `UNAVAILABLE` on any host gate
that would have needed them; nothing fails for a missing tool, and nothing is green
because one was missing.

PresentMon on the host stays worth reinstalling for exactly one case: confirming a
present-diagnostics change or a driver major version against real hardware, when the
cross-check gate says the recorded confirmation no longer applies.

## What is not supported, and why

- **Checkpoints.** A virtual machine with a GPU partition cannot be checkpointed at
  all, so the recipe disables checkpoints in the create phase, before there is a
  partition -- a machine that already took an automatic checkpoint refuses the
  partition afterwards. The differencing disk is the replacement, and it is a better
  one: it resets the whole machine rather than a saved state.
- **Remote Desktop.** An RDP session is a different desktop, and Output Duplication
  does not work on it. The console session is the only session; it logs on
  automatically and stays logged on.
- **Live migration, saved state, and moving the image between machines.** The guest
  runs driver files copied from one specific host adapter. A different host means
  re-running the driver phase, and a host driver update means re-running it too.
- **Anything about real hardware.** Device clocks, physical unplug, a monitor
  powering off, an HDR panel, GPU thermals, colour judged by eye. Those are Tier 3 and
  stay on the developer's machine.
- **Secrets.** The guest's local administrator password is a constant of the recipe,
  in `autounattend.xml` and in `ReleaseVm.psm1`. The machine is disposable, local-only
  and has Remote Desktop off. Do not reuse the password anywhere, and do not give the
  guest a credential that means anything outside it.

## The pieces

| File | Runs on | What it is |
|---|---|---|
| `tools/vm/autounattend.xml` | Windows Setup | unattended install, local admin, console autologon, no OOBE |
| `tools/vm/provision-manifest.psd1` | -- | every package, pinned by version and SHA-256 |
| `tools/vm/provision.ps1` | the guest | idempotent, resumable provisioning; written for Windows PowerShell 5.1 because it installs PowerShell 7 |
| `tools/vm/ReleaseVm.psm1` | the host | the plans, as data, plus the commands they name |
| `tools/vm/New-ReleaseVm.ps1` | the host, elevated | builds the golden image |
| `tools/vm/Invoke-ReleaseVmRun.ps1` | the host, elevated | one campaign on a differencing clone |
| `tools/probes/probe_gpup_nvenc` | either | NVENC through the partition, as JSON |
| `tools/probes/probe_idd_duplication` | either | Output Duplication per output, as JSON |
| `scripts/tests/vm-recipe.tests.ps1` | anywhere | the refusals, the plans and the pins, without a hypervisor |

See ADR 0071 for the decision and what was rejected.
