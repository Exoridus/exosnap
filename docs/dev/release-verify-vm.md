# Disposable release-verification guest

`exo-verify disposable create-base` builds and seals Windows Hyper-V images.
`exo-verify run --backend hyperv` uses the existing disposable backend: one read-only base,
one differencing disk and one owned VM per lane. `exo-guest` runs on the guest console
and carries commands, logs and evidence over Hyper-V sockets without guest networking.

[Verification boundaries](../architecture/verification-boundaries.md) defines what a guest proves.
Physical device clocks, monitor unplug/power, HDR-panel appearance and thermals still require hardware evidence.

## Host preparation

Enable Hyper-V and authorize management with administrator coordination:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All -NoRestart
Add-LocalGroupMember -Group 'Hyper-V Administrators' -Member "$env:USERDOMAIN\$env:USERNAME"
cargo exo-verify disposable register-hyperv-service
```

Restart after enabling Hyper-V and log on again after changing group membership.
UAC remains the operator's Secure Desktop decision. No command synthesizes console input.
Install the Windows ADK Deployment Tools for `oscdimg`, or set `EXO_VERIFY_OSCDIMG` to its executable.
The host uses this maintained ISO writer to put the unattended answer file on a second DVD.
Supply and authenticate the Windows 11 x64 installation ISO through its distribution source.

## Build and provision

```powershell
cargo build --manifest-path tools/Cargo.toml -p exo-guest --release
cargo exo-verify disposable create-base --root 'D:\ExoSnapImages\base' `
    --guest-agent tools/target/release/exo-guest.exe `
    --provision-manifest tools/exo-guest/config/provision-manifest.json `
    --iso '<Windows 11 x64 ISO>' --phase create --dry-run
```

The dry run validates the native manifest and prints phase inputs without creating resources.
Remove `--dry-run` to create the VM. Resume the same image with `--phase install`, then
`--phase gpu,driver,provision,seal`, carrying the same root, agent and manifest arguments.
The Windows DVD boot prompt still requires a one-time key press by the operator in the VM console.
Windows Setup then uses the unattended answer file, creates the disposable local administrator
and logs on automatically. The host registers the native agent and restarts the guest.
The fixed account in the answer file belongs only to these disposable images and must never be reused.

The GPU and driver phases require `--gpu-instance-path` naming the intended
`Get-VMHostPartitionableGpu` instance and `--host-driver-package` naming its active DriverStore package.
Selection is deliberate. The package must contain exactly one INF matching the measured active driver version.
All four GPU resource triples are read back, together with physical partition provenance.
The numbers are Hyper-V units, not assumed percentages.

Rust owns phase order, deadlines, package validation, retries, identity checks and cleanup.
Narrow PowerShell invocations call Windows Hyper-V cmdlets and serialize their results.
They contain no reusable workflow program. In particular, Windows exposes creation of its opaque
local VM key protector through `Set-VMKeyProtector -NewLocalKeyProtector`; the native WMI
`SetKeyProtector` method only consumes already-created protector bytes.
PowerShell Direct is used only to register the native agent before its socket transport exists.

[The JSON provisioning manifest](../../tools/exo-guest/config/provision-manifest.json) owns exact package pins,
SHA-256 values, the MTT display profile and modes. `exo-guest provision` prints a validated plan by default.
Applying it requires `--apply --disposable-vm-id <id>`; the agent compares that identity against
Hyper-V's guest registry before any mutation. Downloads are checked before execution,
installed steps are recorded atomically, and a requested reboot remains pending until the boot identity changes.
The guest does not need PowerShell 7 installed. The host uses Windows' inbox PowerShell for its OS adapters.
The host connects networking only for provisioning
and attempts disconnection on failures too. One requested reboot is allowed before the second pass.

## Seal and identity

Sealing calls `exo-verify disposable residue`, the same clean-machine definition used by the install lane.
It checks installed products, current and legacy per-user registry state, installation paths,
shortcuts and per-user/machine state. Residue stops sealing. Review and remove recognized probe
artifacts deliberately; sealing never deletes unknown user data to obtain a clean result.
The MSI lane already checks first and second starts, default settings, official identity,
normal shutdown, uninstall preservation and reinstall using that shared definition.

Guest facts measure the console session, account, window station and desktop; each attached display path;
DXGI adapter identity; and staged driver INF versions. A GPU-P guest may expose Microsoft's vendor ID.
Its PCI IDs are not required to match the host. The presented adapter name and the copied active driver
package establish the guest binding. Missing measurements fail readiness.

`image-fingerprint.json` retains measured guest facts, GPU identity, package-manifest hash and required mode.
After clean-state/readiness checks and guest shutdown, `base.json` records the VHDX hash and configuration,
and the VHDX becomes read-only. Every run verifies those bytes and rechecks the host GPU/driver identity.
Changed drivers, package pins or virtual-display profiles require rebuilding and requalification.

Run `probe_gpup_nvenc.exe` and `probe_idd_duplication.exe` in the guest before using it for capture acceptance.
Encoded packets and captured frames are evidence; a correctly named adapter alone is not.

## Run and observe

```powershell
$env:EXO_VERIFY_HYPERV_BASE = 'D:\ExoSnapImages\base\base.json'
cargo exo-verify run --profile release-ci --lane release-ci-install --backend hyperv `
    --bundle '<candidate bundle>' --out '<fresh evidence directory>'
```

The runner stages the official bundle and native verifier, proves readiness, runs the selected lane,
collects evidence and destroys its own VM/disk. A pre-existing output directory is refused.
Execution failure still attempts evidence collection. Failed collection retains the VM and disk
for recovery and reports an infrastructure error; it cannot erase the only remaining evidence.
`environment.json` identifies the VM and base. Cleanup failures are reported independently.

Networking defaults to disconnected. To select it explicitly, set `EXO_VERIFY_HYPERV_NETWORK`
to `host-only` or `connected` and `EXO_VERIFY_HYPERV_SWITCH` to the intended switch name.
Host-only requires an Internal switch; connected requires an External switch.
No campaign receives reusable production credentials.

```powershell
cargo exo-verify disposable watch --vm-id '<environment.json VM id>' `
    --exit-file 'C:\ExoVerify\out\campaign.exit' `
    --log-file 'C:\ExoVerify\transcript.log' --timeout-seconds 3600
```

The watch is read-only and can use a second socket connection while the campaign runs.
It preserves log byte order and bounds each read, the overall deadline and consecutive transport failures.
Exit 0 means the exit file appeared, 2 means the deadline expired, and 3 means repeated transport failure.
The file's content is printed, not substituted for the watch exit code. Stopping the watch leaves the campaign running.

## Validation limits

```powershell
cargo test --manifest-path tools/Cargo.toml -p exo-verify disposable::
cargo test --manifest-path tools/Cargo.toml -p exo-guest
```

These tests exercise protocol framing, session/readiness refusals, GPU and display identity,
provisioning pins and guards, response contracts, bounded observation and failure/collection/cleanup ordering.
They do not prove a particular Windows ISO, host GPU driver or installed virtual display works.
Real image creation, provisioning and GPU/capture probes require separate VM evidence.
Do not substitute checkpoints, RDP or saved state for the declared console/differencing-disk model.
The guest shares host resources; endurance/performance runs still need a quiet host.
