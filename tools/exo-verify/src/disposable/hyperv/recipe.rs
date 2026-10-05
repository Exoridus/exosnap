//! Base-image preparation around the same guest agent and sealed-image contract
//! used by disposable runs. Hyper-V cmdlets are narrow OS adapters; decisions,
//! retries, validation and resource ownership stay here.

use anyhow::{Context, Result, bail};
use base64::Engine as _;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

use super::{BaseManifest, GuestClient};
use exo_guest::protocol::Request;

const GUEST_AGENT: &str = r"C:\ExoSnapProvision\exo-guest.exe";
const GUEST_VERIFY: &str = r"C:\ExoSnapProvision\exo-verify.exe";
const GUEST_MANIFEST: &str = r"C:\ExoSnapProvision\provision-manifest.json";

#[derive(Debug, Clone, clap::Args)]
pub struct CreateArgs {
    #[arg(long)]
    pub root: PathBuf,
    #[arg(long, default_value = "ExoSnap-Verify")]
    pub name: String,
    #[arg(long)]
    pub iso: Option<PathBuf>,
    #[arg(long)]
    pub guest_agent: PathBuf,
    #[arg(long)]
    pub provision_manifest: PathBuf,
    #[arg(long)]
    pub host_driver_package: Option<PathBuf>,
    #[arg(long)]
    pub gpu_instance_path: Option<String>,
    #[arg(
        long,
        value_delimiter = ',',
        default_value = "create,install,gpu,driver,provision,seal"
    )]
    pub phase: Vec<Phase>,
    #[arg(long, default_value = "Default Switch")]
    pub provision_switch: String,
    #[arg(long)]
    pub dry_run: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, clap::ValueEnum)]
#[serde(rename_all = "kebab-case")]
pub enum Phase {
    Create,
    Install,
    Gpu,
    Driver,
    Provision,
    Seal,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum NetworkMode {
    Disconnected,
    HostOnly,
    Connected,
}

pub fn network_from_environment() -> Result<(NetworkMode, Option<String>)> {
    let mode = match std::env::var("EXO_VERIFY_HYPERV_NETWORK")
        .as_deref()
        .unwrap_or("disconnected")
    {
        "disconnected" => NetworkMode::Disconnected,
        "host-only" => NetworkMode::HostOnly,
        "connected" => NetworkMode::Connected,
        other => bail!(
            "unknown EXO_VERIFY_HYPERV_NETWORK {other:?}; use disconnected, host-only or connected"
        ),
    };
    let switch = std::env::var("EXO_VERIFY_HYPERV_SWITCH")
        .ok()
        .filter(|s| !s.trim().is_empty());
    if mode != NetworkMode::Disconnected && switch.is_none() {
        bail!("EXO_VERIFY_HYPERV_SWITCH must name the deliberately selected Hyper-V switch");
    }
    Ok((mode, switch))
}

pub fn configure_network(name: &str, mode: NetworkMode, switch: Option<&str>) -> Result<()> {
    if mode == NetworkMode::Disconnected {
        return Ok(());
    }
    let switch = switch.context("a connected network mode requires an explicit switch")?;
    let actual = ps(&format!(
        "(Get-VMSwitch -Name {} -ErrorAction Stop).SwitchType.ToString()",
        literal(switch)
    ))?;
    let expected = match mode {
        NetworkMode::HostOnly => "Internal",
        NetworkMode::Connected => "External",
        NetworkMode::Disconnected => unreachable!(),
    };
    if actual.as_str() != Some(expected) {
        bail!("switch {switch:?} has type {actual}, requested {expected}");
    }
    ps(&format!(
        "Add-VMNetworkAdapter -VMName {} -SwitchName {} -ErrorAction Stop",
        literal(name),
        literal(switch)
    ))?;
    Ok(())
}

pub fn configure_security(name: &str) -> Result<()> {
    let name = literal(name);
    for expression in [
        format!(
            "Set-VM -Name {name} -AutomaticCheckpointsEnabled $false -CheckpointType Disabled -ErrorAction Stop"
        ),
        format!("Set-VMKeyProtector -VMName {name} -NewLocalKeyProtector -ErrorAction Stop"),
        format!("Enable-VMTPM -VMName {name} -ErrorAction Stop"),
        format!(
            "Set-VMFirmware -VMName {name} -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows -ErrorAction Stop"
        ),
    ] {
        ps(&expression)?;
    }
    Ok(())
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct GpuConfig {
    pub instance_path: String,
    pub host_name: String,
    pub driver_version: String,
    pub driver_package: String,
    pub partition: BTreeMap<String, u64>,
}

pub fn default_partition() -> BTreeMap<String, u64> {
    ["VRAM", "Encode", "Decode", "Compute"]
        .into_iter()
        .flat_map(|resource| {
            [
                (format!("MinPartition{resource}"), 80_000_000),
                (format!("MaxPartition{resource}"), 100_000_000),
                (format!("OptimalPartition{resource}"), 100_000_000),
            ]
        })
        .collect()
}

pub fn validate_name(name: &str) -> Result<()> {
    if name.is_empty()
        || name.len() > 64
        || !name.as_bytes()[0].is_ascii_alphanumeric()
        || !name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
    {
        bail!(
            "VM name must start with an ASCII letter or digit and contain at most 64 ASCII letters, digits, '.', '_' or '-'"
        );
    }
    Ok(())
}

fn literal(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}
fn path_literal(path: &Path) -> String {
    literal(&path.to_string_lossy())
}

fn ps(expression: &str) -> Result<Value> {
    let command = format!(
        "[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); {expression} | ConvertTo-Json -Depth 8 -Compress"
    );
    let encoded = base64::engine::general_purpose::STANDARD.encode(
        command
            .encode_utf16()
            .flat_map(u16::to_le_bytes)
            .collect::<Vec<_>>(),
    );
    let output = crate::tools::run(
        Command::new("powershell.exe").args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-EncodedCommand",
            &encoded,
        ]),
        Duration::from_secs(120),
    )?;
    if !output.success() {
        bail!("Hyper-V OS adapter failed: {}", output.stderr.trim());
    }
    if output.stdout.trim().is_empty() {
        return Ok(Value::Null);
    }
    serde_json::from_str(output.stdout.trim()).context("Hyper-V OS adapter returned invalid JSON")
}

fn rows(value: Value) -> Vec<Value> {
    match value {
        Value::Array(rows) => rows,
        Value::Null => vec![],
        row => vec![row],
    }
}

fn vm(name: &str) -> Result<Value> {
    ps(&format!(
        "Get-VM -Name {} -ErrorAction Stop | Select-Object Name,Id,State",
        literal(name)
    ))
}

fn vm_id(name: &str) -> Result<String> {
    vm(name)?["Id"]
        .as_str()
        .map(str::to_string)
        .context("VM did not report its identity")
}

#[derive(Debug, Serialize)]
struct DirectResult {
    exit_code: i32,
    output: Vec<Value>,
}

fn decode_direct(reply: Value) -> Result<DirectResult> {
    let mut output = rows(reply);
    let exit_code = output
        .pop()
        .and_then(|value| value.as_i64())
        .and_then(|value| i32::try_from(value).ok())
        .context("guest bootstrap did not return a final native exit code")?;
    Ok(DirectResult { exit_code, output })
}

fn direct(name: &str, program: &str, argument: &str) -> Result<DirectResult> {
    // PowerShell Direct is needed only to start the native socket agent before
    // that transport exists. The account belongs to this disposable image.
    decode_direct(ps(&format!(
        "Invoke-Command -VMName {} -Credential ([pscredential]::new('exosnap',(ConvertTo-SecureString 'ExoSnapVerify!1' -AsPlainText -Force))) -ErrorAction Stop -ScriptBlock {{ & {} {}; $LASTEXITCODE }}",
        literal(name),
        literal(program),
        literal(argument)
    ))?)
}

fn copy_to_guest(name: &str, source: &Path, destination: &str) -> Result<()> {
    ps(&format!(
        "Copy-VMFile -Name {} -SourcePath {} -DestinationPath {} -CreateFullPath -FileSource Host -Force -ErrorAction Stop",
        literal(name),
        path_literal(source),
        literal(destination)
    ))?;
    Ok(())
}

fn wait_for_agent(name: &str) -> Result<GuestClient> {
    GuestClient::connect(&vm_id(name)?, Instant::now() + Duration::from_secs(600))
}

fn ready_sequence(
    state: u64,
    start: impl FnOnce() -> Result<()>,
    wait: impl FnOnce() -> Result<()>,
) -> Result<()> {
    match state {
        2 => {}
        3 => start()?,
        _ => bail!("VM must be running or off before a guest phase; state is {state}"),
    }
    wait()
}

fn ensure_guest_ready(name: &str, manifest: &Path) -> Result<()> {
    let state = vm(name)?["State"].as_u64().context("VM state missing")?;
    ready_sequence(
        state,
        || {
            ps(&format!(
                "Start-VM -Name {} -ErrorAction Stop",
                literal(name)
            ))?;
            Ok(())
        },
        || {
            wait_for_agent(name)?;
            let deadline = Instant::now() + Duration::from_secs(120);
            loop {
                match copy_to_guest(name, manifest, GUEST_MANIFEST) {
                    Ok(()) => return Ok(()),
                    Err(error) if Instant::now() >= deadline => {
                        return Err(error).context("guest file transfer did not become ready");
                    }
                    Err(_) => std::thread::sleep(Duration::from_secs(3)),
                }
            }
        },
    )
}

fn native_exit(reply: Value, what: &str) -> Result<i32> {
    reply["exitCode"]
        .as_i64()
        .and_then(|v| i32::try_from(v).ok())
        .with_context(|| format!("{what} returned no native exit code: {reply}"))
}

fn guest_exec(client: &mut GuestClient, program: &str, args: &[&str]) -> Result<Value> {
    client.call(Request::Exec {
        program: program.into(),
        args: args.iter().map(|v| (*v).into()).collect(),
        cwd: None,
        env: Default::default(),
        timeout_ms: 3_600_000,
    })
}

pub fn configure_partition(name: &str, gpu: &GpuConfig) -> Result<Value> {
    if gpu.partition.keys().ne(default_partition().keys()) {
        bail!("GPU partition must declare all four resource triples and no unknown fields");
    }
    let controllers = rows(ps(
        "Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop | Select-Object Name,PNPDeviceID,DriverVersion",
    )?);
    let matching: Vec<_> = controllers
        .iter()
        .filter(|row| {
            row["PNPDeviceID"].as_str().is_some_and(|p| {
                gpu.instance_path
                    .to_ascii_lowercase()
                    .contains(&p.replace('\\', "#").to_ascii_lowercase())
            })
        })
        .collect();
    if matching.len() != 1
        || matching[0]["DriverVersion"] != gpu.driver_version
        || matching[0]["Name"] != gpu.host_name
    {
        bail!(
            "the host GPU identity or active driver has changed since this image was qualified; rebuild and requalify the image"
        );
    }
    let quoted = literal(name);
    ps(&format!(
        "Set-VM -Name {quoted} -GuestControlledCacheTypes $true -LowMemoryMappedIoSpace 1073741824 -HighMemoryMappedIoSpace 34359738368 -ErrorAction Stop"
    ))?;
    let existing = rows(ps(&format!(
        "Get-VMGpuPartitionAdapter -VMName {quoted} -ErrorAction Stop | Select-Object InstancePath"
    ))?);
    if existing.len() > 1 {
        bail!("the VM has multiple GPU partitions; explicit recovery is required");
    }
    if existing.is_empty() {
        ps(&format!(
            "Add-VMGpuPartitionAdapter -VMName {quoted} -InstancePath {} -ErrorAction Stop",
            literal(&gpu.instance_path)
        ))?;
    } else if !existing[0]["InstancePath"]
        .as_str()
        .is_some_and(|p| p.eq_ignore_ascii_case(&gpu.instance_path))
    {
        bail!("the existing VM partition belongs to another host device");
    }
    let parameters = gpu
        .partition
        .iter()
        .map(|(key, value)| format!(" -{key} {value}"))
        .collect::<String>();
    ps(&format!(
        "Set-VMGpuPartitionAdapter -VMName {quoted}{parameters} -ErrorAction Stop"
    ))?;
    let actual = ps(&format!(
        "Get-VMGpuPartitionAdapter -VMName {quoted} -ErrorAction Stop | Select-Object InstancePath,{}",
        gpu.partition.keys().cloned().collect::<Vec<_>>().join(",")
    ))?;
    let differences = partition_differences(&gpu.partition, &actual);
    if !differences.is_empty() {
        bail!(
            "GPU partition read-back differs: {}",
            differences.join("; ")
        );
    }
    if !actual["InstancePath"]
        .as_str()
        .is_some_and(|p| p.eq_ignore_ascii_case(&gpu.instance_path))
    {
        bail!("GPU partition provenance differs from the explicitly selected host device");
    }
    Ok(actual)
}

fn partition_differences(requested: &BTreeMap<String, u64>, actual: &Value) -> Vec<String> {
    requested
        .iter()
        .filter_map(|(key, expected)| {
            let measured = actual.get(key).and_then(Value::as_u64);
            (measured != Some(*expected))
                .then(|| format!("{key}: requested {expected}, actual {measured:?}"))
        })
        .collect()
}

fn select_gpu(args: &CreateArgs) -> Result<GpuConfig> {
    let expected_path = args.gpu_instance_path.as_ref().context("--gpu-instance-path deliberately selects the host adapter; no device is selected automatically")?;
    let controllers = rows(ps(
        "Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop | Select-Object Name,PNPDeviceID,DriverVersion",
    )?);
    let matches: Vec<_> = controllers
        .iter()
        .filter(|row| {
            row["PNPDeviceID"].as_str().is_some_and(|p| {
                expected_path
                    .to_ascii_lowercase()
                    .contains(&p.replace('\\', "#").to_ascii_lowercase())
            })
        })
        .collect();
    if matches.len() != 1 {
        bail!(
            "the selected GPU partition path matches {} host adapters",
            matches.len()
        );
    }
    let host = matches[0];
    let version = host["DriverVersion"]
        .as_str()
        .context("host GPU driver version was not measured")?;
    let package = args.host_driver_package.as_ref().context(
        "--host-driver-package must identify the selected adapter's active DriverStore package",
    )?;
    let infs: Vec<_> = crate::package::walk_files(package)?
        .into_iter()
        .filter(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("inf")))
        .collect();
    let matches = infs
        .iter()
        .filter(|p| {
            std::fs::read_to_string(p)
                .ok()
                .and_then(|s| inf_driver_version(&s))
                .as_deref()
                == Some(version)
        })
        .count();
    if matches != 1 {
        bail!(
            "the selected driver package has {matches} INF files matching active driver {version}; require exactly one"
        );
    }
    Ok(GpuConfig {
        instance_path: expected_path.clone(),
        host_name: host["Name"]
            .as_str()
            .context("host adapter name missing")?
            .into(),
        driver_version: version.into(),
        driver_package: package
            .file_name()
            .context("driver package has no name")?
            .to_string_lossy()
            .into_owned(),
        partition: default_partition(),
    })
}

pub(super) fn inf_driver_version(text: &str) -> Option<String> {
    let pattern = regex::Regex::new(r"(?im)^\s*DriverVer\s*=\s*[^,]+,\s*([0-9][0-9.]*)\s*$")
        .expect("constant regex");
    pattern.captures(text).map(|c| c[1].to_string())
}

pub fn create(args: CreateArgs) -> Result<()> {
    validate_name(&args.name)?;
    if !args.root.is_absolute() || args.root.parent().is_none() {
        bail!("--root must name an absolute image directory below a volume root");
    }
    if args.phase.is_empty() {
        bail!("at least one image phase is required");
    }
    let manifest_text = std::fs::read_to_string(&args.provision_manifest)
        .context("read pinned guest provisioning manifest")?;
    let provisioning = exo_guest::provision::Manifest::parse(&manifest_text)?;
    let idd = provisioning
        .packages
        .iter()
        .find(|p| p.id == "idd")
        .context("IDD package missing")?;
    let exo_guest::provision::PackageSource::Download {
        hardware_id: Some(hardware_id),
        ..
    } = &idd.source
    else {
        bail!("IDD hardware identity is missing");
    };
    let display = super::facts::DisplayRequirement {
        width: provisioning.display.width,
        height: provisioning.display.height,
        refresh_hz: provisioning.display.refresh_rates[0],
        hardware_id: hardware_id.clone(),
    };
    if !args.guest_agent.is_file() {
        bail!("--guest-agent must name the built exo-guest.exe");
    }
    if args.dry_run {
        println!(
            "{}",
            serde_json::to_string_pretty(
                &json!({"name": args.name, "root": args.root, "phases": args.phase,
            "installationIso": args.iso, "guestAgent": args.guest_agent, "provisionManifest": args.provision_manifest,
            "network": "connected only during pinned provisioning", "sealedBase": args.root.join("base.json"),
            "operatorAction": "the Windows DVD boot prompt needs the operator in the VM console"})
            )?
        );
        return Ok(());
    }
    super::wmi::Hypervisor::connect().context("Hyper-V management is unavailable; enable Microsoft-Hyper-V and use an authorized Hyper-V Administrators token")?;
    if !super::availability().1 {
        bail!(
            "register the guest socket service first: cargo exo-verify disposable register-hyperv-service"
        );
    }
    if args.phase.contains(&Phase::Gpu) || args.phase.contains(&Phase::Driver) {
        select_gpu(&args)?;
    }
    let quoted = literal(&args.name);
    let disk = args.root.join("golden.vhdx");
    let state = args.root.join("image-owner.json");
    if args.phase.contains(&Phase::Create) {
        if disk.exists() || state.exists() {
            bail!("image already exists; choose a fresh --root or resume with --phase");
        }
        let iso =
            args.iso.as_ref().filter(|p| p.is_file()).context(
                "--iso must name the operator-verified Windows 11 x64 installation media",
            )?;
        let oscdimg = crate::tools::require("oscdimg")
            .context("install the Windows ADK Deployment Tools or set EXO_VERIFY_OSCDIMG")?;
        std::fs::create_dir_all(&args.root)?;
        let answer_dir = args.root.join("answer");
        std::fs::create_dir(&answer_dir)?;
        std::fs::write(
            answer_dir.join("autounattend.xml"),
            include_str!("../../../config/autounattend.xml"),
        )?;
        let answer_iso = args.root.join("unattend.iso");
        let output = crate::tools::run(
            Command::new(oscdimg)
                .args(["-n", "-m"])
                .arg(&answer_dir)
                .arg(&answer_iso),
            Duration::from_secs(120),
        )?;
        if !output.success() {
            bail!("answer ISO creation failed: {}", output.stderr);
        }
        ps(&format!(
            "New-VHD -Path {} -SizeBytes 64424509440 -Dynamic -ErrorAction Stop",
            path_literal(&disk)
        ))?;
        ps(&format!(
            "New-VM -Name {quoted} -Generation 2 -MemoryStartupBytes 8589934592 -VHDPath {} -ErrorAction Stop | Select-Object Name,Id",
            path_literal(&disk)
        ))?;
        std::fs::write(
            &state,
            serde_json::to_vec_pretty(
                &json!({"vmId": vm_id(&args.name)?, "name": args.name, "disk": disk}),
            )?,
        )?;
        ps(&format!(
            "Set-VMProcessor -VMName {quoted} -Count 4 -ErrorAction Stop"
        ))?;
        ps(&format!(
            "Set-VMMemory -VMName {quoted} -DynamicMemoryEnabled $false -ErrorAction Stop"
        ))?;
        configure_security(&args.name)?;
        ps(&format!(
            "Add-VMDvdDrive -VMName {quoted} -Path {} -ErrorAction Stop",
            path_literal(iso)
        ))?;
        ps(&format!(
            "Add-VMDvdDrive -VMName {quoted} -Path {} -ErrorAction Stop",
            path_literal(&answer_iso)
        ))?;
        ps(&format!(
            "Set-VMFirmware -VMName {quoted} -FirstBootDevice (Get-VMDvdDrive -VMName {quoted} -ControllerNumber 0 -ControllerLocation 1 -ErrorAction Stop) -ErrorAction Stop"
        ))?;
        let services = rows(ps(&format!(
            "Get-VMIntegrationService -VMName {quoted} -ErrorAction Stop | Select-Object Id,Name"
        ))?);
        let service = services
            .iter()
            .find(|s| {
                s["Id"].as_str().is_some_and(|id| {
                    id.to_ascii_uppercase()
                        .ends_with("6C09BB55-D683-4DA0-8931-C9BF705F6480")
                })
            })
            .context("guest file-transfer integration service is missing")?;
        ps(&format!(
            "Enable-VMIntegrationService -VMName {quoted} -Name {} -ErrorAction Stop",
            literal(
                service["Name"]
                    .as_str()
                    .context("guest service name absent")?
            )
        ))?;
    }
    let owner: Value = serde_json::from_slice(
        &std::fs::read(&state).context("image-owner.json is required to resume this image")?,
    )?;
    if owner["vmId"] != vm_id(&args.name)? {
        bail!("VM identity differs from this image's ownership receipt");
    }
    if args.phase.contains(&Phase::Install) {
        println!(
            "Open the VM console and confirm its DVD boot prompt; waiting up to 90 minutes for Windows installation."
        );
        ps(&format!("Start-VM -Name {quoted} -ErrorAction Stop"))?;
        let deadline = Instant::now() + Duration::from_secs(5400);
        loop {
            if copy_to_guest(&args.name, &args.guest_agent, GUEST_AGENT).is_ok()
                && let Ok(result) = direct(&args.name, GUEST_AGENT, "install")
            {
                std::fs::write(
                    args.root.join("agent-install.json"),
                    serde_json::to_vec_pretty(&result)?,
                )?;
                if result.exit_code == 0 {
                    break;
                }
            }
            if Instant::now() >= deadline {
                bail!("Windows installation did not expose guest file transfer within 90 minutes");
            }
            std::thread::sleep(Duration::from_secs(10));
        }
        ps(&format!(
            "Restart-VM -Name {quoted} -Force -ErrorAction Stop"
        ))?;
        wait_for_agent(&args.name)?;
    }
    let gpu_path = args.root.join("gpu.json");
    if args.phase.contains(&Phase::Gpu) {
        let gpu = select_gpu(&args)?;
        ps(&format!(
            "Stop-VM -Name {quoted} -TurnOff -Force -ErrorAction Stop"
        ))?;
        let actual = configure_partition(&args.name, &gpu)?;
        std::fs::write(&gpu_path, serde_json::to_vec_pretty(&gpu)?)?;
        std::fs::write(
            args.root.join("gpu-readback.json"),
            serde_json::to_vec_pretty(&actual)?,
        )?;
        ps(&format!("Start-VM -Name {quoted} -ErrorAction Stop"))?;
    }
    if args.phase.contains(&Phase::Driver) {
        ensure_guest_ready(&args.name, &args.provision_manifest)?;
        let gpu = select_gpu(&args)?;
        let package = args
            .host_driver_package
            .as_ref()
            .context("host driver package missing")?;
        for source in crate::package::walk_files(package)? {
            let relative = source.strip_prefix(package)?;
            copy_to_guest(
                &args.name,
                &source,
                &format!(
                    r"C:\HostDriverStore\FileRepository\{}\{}",
                    gpu.driver_package,
                    relative.display()
                ),
            )?;
        }
        let system = PathBuf::from(std::env::var_os("SystemRoot").context("SystemRoot missing")?)
            .join("System32");
        for entry in std::fs::read_dir(system)? {
            let file = entry?.path();
            let name = file
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .to_ascii_lowercase();
            if file.is_file() && name.starts_with("nv") && name.ends_with(".dll") {
                copy_to_guest(
                    &args.name,
                    &file,
                    &format!(r"C:\HostDriverStore\System32\{name}"),
                )?;
            }
        }
    }
    if args.phase.contains(&Phase::Provision) {
        ensure_guest_ready(&args.name, &args.provision_manifest)?;
        copy_to_guest(&args.name, &args.provision_manifest, GUEST_MANIFEST)?;
        ps(&format!(
            "Connect-VMNetworkAdapter -VMName {quoted} -SwitchName {} -ErrorAction Stop",
            literal(&args.provision_switch)
        ))?;
        let provision = (|| -> Result<()> {
            for pass in 0..2 {
                let mut client = wait_for_agent(&args.name)?;
                let id = vm_id(&args.name)?;
                let reply = guest_exec(
                    &mut client,
                    GUEST_AGENT,
                    &[
                        "provision",
                        "--manifest",
                        GUEST_MANIFEST,
                        "--apply",
                        "--disposable-vm-id",
                        &id,
                    ],
                )?;
                std::fs::write(
                    args.root.join(format!("provision-{pass}.json")),
                    serde_json::to_vec_pretty(&reply)?,
                )?;
                match native_exit(reply, "provisioning")? {
                    0 => return Ok(()),
                    2 if pass == 0 => {
                        client.call(Request::Reboot)?;
                        drop(client);
                        std::thread::sleep(Duration::from_secs(10));
                    }
                    code => bail!("provisioning exited {code} on pass {}", pass + 1),
                }
            }
            bail!("provisioning did not finish")
        })();
        let disconnected = ps(&format!(
            "Disconnect-VMNetworkAdapter -VMName {quoted} -ErrorAction Stop"
        ));
        if let Err(error) = provision {
            bail!(
                "provisioning failed: {error:#}{}",
                disconnected
                    .err()
                    .map(|e| format!("; disconnect failed: {e:#}"))
                    .unwrap_or_default()
            );
        }
        disconnected?;
    }
    if args.phase.contains(&Phase::Seal) {
        // Host ISO paths can disappear after installation and prevent boot.
        ps(&format!(
            "Get-VMDvdDrive -VMName {quoted} -ErrorAction Stop | Set-VMDvdDrive -Path $null -ErrorAction Stop"
        ))?;
        ensure_guest_ready(&args.name, &args.provision_manifest)?;
        copy_to_guest(&args.name, &std::env::current_exe()?, GUEST_VERIFY)?;
        let mut client = wait_for_agent(&args.name)?;
        let clean = guest_exec(&mut client, GUEST_VERIFY, &["disposable", "residue"])?;
        std::fs::write(
            args.root.join("clean-machine.json"),
            serde_json::to_vec_pretty(&clean)?,
        )?;
        if native_exit(clean, "clean-machine check")? != 0 {
            bail!(
                "the base image still contains ExoSnap state; inspect clean-machine.json and explicitly remove recognized residue"
            );
        }
        let measured = guest_exec(&mut client, GUEST_VERIFY, &["disposable", "guest-facts"])?;
        if measured["exitCode"].as_i64() != Some(0) {
            bail!("guest facts measurement failed: {measured}");
        }
        let facts: Value =
            serde_json::from_str(measured["stdout"].as_str().context("guest facts absent")?)?;
        let gpu = std::fs::read(&gpu_path)
            .ok()
            .map(|bytes| serde_json::from_slice::<GpuConfig>(&bytes))
            .transpose()?;
        let unmet = super::facts::unmet(&facts, gpu.as_ref(), Some(&display));
        std::fs::write(
            args.root.join("image-fingerprint.json"),
            serde_json::to_vec_pretty(&json!({
                "guest": facts, "gpu": gpu, "provisioningSha256": crate::bundle::sha256_bytes(manifest_text.as_bytes()), "display": display
            }))?,
        )?;
        if !unmet.is_empty() {
            bail!("image readiness is unproven: {}", unmet.join("; "));
        }
        let identity = guest_exec(&mut client, "cmd.exe", &["/d", "/c", "ver"])?;
        client.call(Request::Shutdown)?;
        drop(client);
        let deadline = Instant::now() + Duration::from_secs(120);
        while vm(&args.name)?["State"].as_u64() != Some(3) {
            if Instant::now() >= deadline {
                bail!("guest did not shut down for sealing");
            }
            std::thread::sleep(Duration::from_secs(2));
        }
        let (sha256, _) = crate::bundle::sha256_file(&disk)?;
        let mut permissions = std::fs::metadata(&disk)?.permissions();
        permissions.set_readonly(true);
        std::fs::set_permissions(&disk, permissions)?;
        let manifest = BaseManifest {
            image_id: args.name,
            windows_build: identity["stdout"]
                .as_str()
                .context("Windows build missing")?
                .trim()
                .into(),
            update_level: "recorded by Windows build".into(),
            guest_agent_version: env!("CARGO_PKG_VERSION").into(),
            driver_set: gpu
                .as_ref()
                .map(|g| format!("{} {}", g.driver_package, g.driver_version))
                .unwrap_or_else(|| "none".into()),
            provisioning_version: 1,
            vhdx: disk,
            sha256,
            memory_mb: 8192,
            processors: 4,
            gpu,
            display: Some(display),
        };
        std::fs::write(
            args.root.join("base.json"),
            serde_json::to_vec_pretty(&manifest)?,
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bootstrap_stdout_does_not_hide_the_native_exit_code() {
        let result = decode_direct(json!(["SUCCESS: task registered", "123", 0])).unwrap();
        assert_eq!(result.exit_code, 0);
        assert_eq!(
            result.output,
            vec![json!("SUCCESS: task registered"), json!("123")]
        );
        assert_eq!(decode_direct(json!(3010)).unwrap().exit_code, 3010);
        assert!(decode_direct(json!(["SUCCESS"])).is_err());
        assert!(decode_direct(Value::Null).is_err());
    }

    #[test]
    fn guest_readiness_follows_start_and_never_runs_after_failed_start() {
        let steps = std::cell::RefCell::new(Vec::new());
        ready_sequence(
            3,
            || {
                steps.borrow_mut().push("start");
                Ok(())
            },
            || {
                steps.borrow_mut().push("ready");
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(*steps.borrow(), ["start", "ready"]);
        steps.borrow_mut().clear();
        ready_sequence(
            2,
            || panic!("running VM must not be restarted"),
            || {
                steps.borrow_mut().push("ready");
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(*steps.borrow(), ["ready"]);
        assert!(ready_sequence(3, || bail!("start failed"), || panic!("must not wait")).is_err());
        assert!(
            ready_sequence(
                4,
                || panic!("unsupported state"),
                || panic!("unsupported state")
            )
            .is_err()
        );
    }
    #[test]
    fn names_cannot_escape_their_directory_or_inject_commands() {
        for name in ["", "..", "a/b", "a\\b", "C:\\evil", "-leading", "x';exit"] {
            assert!(validate_name(name).is_err(), "{name}");
        }
        validate_name("rc1-smoke.2").unwrap();
        assert_eq!(literal("a' $x `; b"), "'a'' $x `; b'");
    }
    #[test]
    fn every_gpu_resource_has_a_read_back_contract() {
        let requested = default_partition();
        assert_eq!(requested.len(), 12);
        let mut actual = serde_json::to_value(&requested).unwrap();
        assert!(partition_differences(&requested, &actual).is_empty());
        actual.as_object_mut().unwrap().remove("MinPartitionVRAM");
        actual["MaxPartitionEncode"] = json!(42);
        let differences = partition_differences(&requested, &actual);
        assert_eq!(differences.len(), 2);
        assert!(differences.iter().any(|v| v.contains("MinPartitionVRAM")));
    }
    #[test]
    fn driver_version_is_read_from_the_inf_directive() {
        assert_eq!(
            inf_driver_version("DriverVer = 04/12/2026, 32.0.16.1656\n"),
            Some("32.0.16.1656".into())
        );
        assert!(inf_driver_version("Provider=NVIDIA\n").is_none());
    }

    #[test]
    fn dry_run_validates_native_inputs_without_creating_the_image() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("image with spaces");
        let manifest = directory.path().join("manifest.json");
        std::fs::write(
            &manifest,
            include_str!("../../../../exo-guest/config/provision-manifest.json"),
        )
        .unwrap();
        let args = CreateArgs {
            root: root.clone(),
            name: "recipe-test".into(),
            iso: None,
            guest_agent: std::env::current_exe().unwrap(),
            provision_manifest: manifest,
            host_driver_package: None,
            gpu_instance_path: None,
            phase: vec![Phase::Create],
            provision_switch: "Default Switch".into(),
            dry_run: true,
        };
        create(args.clone()).unwrap();
        assert!(!root.exists());
        let invalid = CreateArgs {
            name: "../escape".into(),
            ..args
        };
        assert!(create(invalid).is_err());
        assert!(!root.exists());
    }
}
