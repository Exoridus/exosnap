# ADR 0071: Tier 2 verification runs in a Hyper-V GPU-partitioned guest

- Status: Accepted
- Date: 2026-09-08
- Supersedes: nothing
- Related: ADR 0066 (Live Verify control channel), ADR 0069 (transactional environment
  orchestration), ADR 0070 (the release-verify harness is a typed C# program)

## Context

The install, update and packaging gates -- MSI decline and accept, the Chocolatey
rehearsal, clean first start, upgrade from an older build -- assert things about a
machine, not about a window. They install software, write to the registry, create a
user configuration directory, and take a machine-wide single-instance mutex. Running
them on the developer's own machine means the machine is a shared mutable fixture, and
the rc19 campaign is the evidence: three of its four runner defects were consequences
of that and of nothing else. The accept gate removed the installed build the decline
gate needed, so the two could only run in one order and only once. A killed soak left
a recovery manifest under the user's local application data and both MSI gates refused
to start. A declined update left the updater running and holding `exosnap-updater.exe`,
so the next launch could not stage its own over it, and that was reported as an MSI
failure.

Windows Sandbox (`scripts/lib/ReleaseSandbox.ps1`) fixed the starting state and is
still the cheapest way to run an install gate. Three things it cannot do:

- **No GPU.** Sandbox's vGPU is off in this repository on purpose and would not give
  NVENC anyway, so no capture, encode, present or overlay gate can run in it.
- **No persistence.** Every tool the gates need has to be mapped in or downloaded on
  every run, inside a machine whose whole value is a known starting state.
- **No image.** There is nothing to inspect, version, or reason about; "a clean
  Windows" is whatever the host's Sandbox feature happens to produce.

Meanwhile the gates that do need a GPU and a desktop -- present mode, fullscreen
exclusive, the capture-excluded overlays, the toasts -- run on the developer's machine
by taking its desktop, which is the uncoordinated input the agent rules forbid.

## Decision

**Tier 2 runs in a Hyper-V guest with a partition of the host GPU, built once from a
recipe in the repository and cloned per run.**

The recipe is `tools/vm/` and the image is not tracked. `New-ReleaseVm.ps1` builds a
generation 2 Windows 11 machine, installs it unattended, attaches a GPU partition,
copies the host adapter's driver files in, and provisions it.
`Invoke-ReleaseVmRun.ps1` takes a differencing disk from that image per run, runs one
campaign over PowerShell Direct, copies the evidence out, and deletes the disk.

Four properties follow, and each of them answers one of the failures above:

- **The starting state is bytes, not a hope.** The golden image is never written to.
  An install gate cannot see what a previous gate installed, because the previous gate
  ran on a disk that no longer exists.
- **The desktop belongs to nobody.** The console session logs on automatically and
  stays logged on, so Output Duplication and the overlay gates have a real session
  without anyone taking the developer's.
- **The control channel needs no network.** PowerShell Direct works with the adapter
  disconnected, so `Disconnected` is the default network mode rather than a mode
  somebody remembers to ask for.
- **The tool set is pinned.** Every third-party package is fixed by version and
  SHA-256, and provisioning refuses to install one whose pin has not been recorded.

### Plans are data

Both host scripts build a plan -- an ordered list of steps, each a command name plus
its parameter table -- and only then execute it. `-DryRun` prints the plan and runs
nothing.

This is the part that is load-bearing rather than tidy. A dry run that describes what
the real run would do is a second implementation with its own bugs, and it drifts
silently, because nothing compares the two. Here the printed plan and the executed
plan are the same object. It is also what makes a virtual machine recipe testable at
all: `scripts/tests/vm-recipe.tests.ps1` reads plans and refusals on a machine with no
Hyper-V feature enabled, from a shell that is not elevated and whose account is not in
`Hyper-V Administrators`.

### The two probes come before the gates

`probe_gpup_nvenc` and `probe_idd_duplication` (both under `tools/probes`, both built
with `-DEXOSNAP_BUILD_PROBES=ON`) answer the two questions the design rests on: does
NVENC work through a GPU partition, and can the virtual monitor be duplicated. They
are run in the guest and on the host, and the host answer is the reference.

Either failing **moves the gates that need it back to the host**. That is a supported
outcome recorded here so it is not later read as a broken image: the guest is a
transport for gates, not a claim that every gate can live in it.

## Consequences

- Hyper-V and membership in `Hyper-V Administrators` become prerequisites for running
  Tier 2 locally. Both are one-time elevated steps, and the scripts refuse with the
  exact command rather than with a denied cmdlet.
- Checkpoints are unsupported for the lifetime of this design: a machine with a GPU
  partition cannot be checkpointed. The recipe disables them before the partition
  exists, because a machine that already took an automatic checkpoint refuses the
  partition afterwards. The differencing disk replaces them and resets more.
- The image is bound to one host adapter. It carries driver files copied from the
  host's DriverStore, so a driver update on the host means re-running the driver
  phase, and the image does not travel to another machine.
- After the cutover, Intel PresentMon, VB-CABLE, SoundVolumeView and the Windows
  Sandbox feature leave the host and live in the guest image. Host gates that would
  have used them report `UNAVAILABLE`; nothing fails for a missing tool and nothing is
  green because one was missing.
- Windows Sandbox stays as the fallback transport. It is cheaper for a bare install
  gate and needs no image, and removing a working path because a better one exists
  would leave nothing to fall back to while the image is being rebuilt.
- Tier 3 is unchanged and stays on the developer's machine: real device clocks,
  physical unplug, a monitor powering off, an HDR panel, GPU thermals, and colour
  judged by eye.

## Alternatives rejected

**A cloud runner.** No budget, and the gates that matter most need a GPU and a
desktop, which is the expensive end of every hosted offering.

**Discrete Device Assignment instead of a GPU partition.** DDA hands the whole adapter
to the guest, which means the host loses its display for the duration. On a machine
the developer is using, that is not a verification transport.

**Keeping Sandbox and adding a second machine for the GPU gates.** Two transports with
two starting states, two tool sets and two ways for a gate to be wrong. The guest does
both, and Sandbox remains available for the case it is genuinely cheaper.

**Nested virtualization inside an existing VM.** Adds a layer whose failure modes are
harder to tell apart from the product's, for no property this design needs.
