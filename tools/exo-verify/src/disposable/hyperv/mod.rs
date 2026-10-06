//! Hyper-V disposable VMs: a sealed base VHDX, one differencing disk and one
//! temporary VM per run, controlled through `exo-guest` over a Hyper-V socket.
//!
//! The host's own system volume is never a parent. The base image is
//! immutable infrastructure described by a manifest; a base whose bytes no
//! longer match that manifest is refused rather than booted.

pub mod facts;
pub mod recipe;
mod vhd;
pub mod watch;
mod wmi;

use anyhow::{Context as _, Result, bail};
use base64::Engine as _;
use exo_guest::protocol::{Envelope, PROTOCOL_VERSION, Request, Response};
use exo_guest::{frame, hvsock};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime};

use super::{BackendKind, DisposableWindows, GUEST_ROOT, GuestCommand, RunDir};

/// Names the base image manifest for the Hyper-V backend.
pub const BASE_ENV: &str = "EXO_VERIFY_HYPERV_BASE";

const SERVICES_KEY: &str =
    r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices";

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BaseManifest {
    pub image_id: String,
    pub windows_build: String,
    pub update_level: String,
    pub guest_agent_version: String,
    pub driver_set: String,
    pub provisioning_version: u32,
    /// Path of the base VHDX, relative to the manifest or absolute.
    pub vhdx: PathBuf,
    pub sha256: String,
    #[serde(default = "default_memory")]
    pub memory_mb: u64,
    #[serde(default = "default_processors")]
    pub processors: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gpu: Option<recipe::GpuConfig>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub display: Option<facts::DisplayRequirement>,
}

fn default_memory() -> u64 {
    8192
}
fn default_processors() -> u64 {
    4
}

#[derive(Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
struct VerifiedIdentity {
    len: u64,
    modified_ns: u128,
    sha256: String,
}

impl BaseManifest {
    pub fn load(path: &Path) -> Result<(BaseManifest, PathBuf)> {
        let text =
            std::fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?;
        let manifest: BaseManifest = serde_json::from_str(&text)
            .with_context(|| format!("parse base manifest {}", path.display()))?;
        let vhdx = if manifest.vhdx.is_absolute() {
            manifest.vhdx.clone()
        } else {
            path.parent().unwrap_or(Path::new(".")).join(&manifest.vhdx)
        };
        Ok((manifest, vhdx))
    }

    /// Proves the base VHDX still holds the sealed bytes. Hashing a whole
    /// image takes minutes, so a verified hash is cached next to it and
    /// reused only while size and modification time are unchanged. The
    /// image must also be read-only, which keeps a mistaken attach from
    /// writing into it.
    pub fn verify(&self, vhdx: &Path) -> Result<()> {
        let meta = std::fs::metadata(vhdx).with_context(|| format!("open {}", vhdx.display()))?;
        if !meta.permissions().readonly() {
            bail!(
                "base image {} is writable; seal it read-only",
                vhdx.display()
            );
        }
        let current = |sha256: String| VerifiedIdentity {
            len: meta.len(),
            modified_ns: meta
                .modified()
                .ok()
                .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                .map(|d| d.as_nanos())
                .unwrap_or(0),
            sha256,
        };
        let cache = vhdx.with_extension("vhdx.identity.json");
        let cached = std::fs::read_to_string(&cache)
            .ok()
            .and_then(|t| serde_json::from_str::<VerifiedIdentity>(&t).ok());
        let expected = current(self.sha256.to_ascii_lowercase());
        if cached.as_ref() == Some(&expected) {
            return Ok(());
        }
        let (sha, _) = crate::bundle::sha256_file(vhdx)?;
        if sha != expected.sha256 {
            bail!(
                "base image {} hashes to {sha}, manifest {} expects {}",
                vhdx.display(),
                self.image_id,
                self.sha256
            );
        }
        let _ = std::fs::write(&cache, serde_json::to_vec_pretty(&current(sha))?);
        Ok(())
    }
}

fn registry_key_exists(path: &str) -> bool {
    use windows::Win32::System::Registry::{
        HKEY, HKEY_LOCAL_MACHINE, KEY_READ, RegCloseKey, RegOpenKeyExW,
    };
    let mut key = HKEY::default();
    let opened = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            &windows::core::HSTRING::from(path),
            None,
            KEY_READ,
            &mut key,
        )
    };
    if opened.is_ok() {
        unsafe {
            let _ = RegCloseKey(key);
        }
        true
    } else {
        false
    }
}

fn service_key() -> String {
    format!(r"{SERVICES_KEY}\{{{}}}", exo_guest::SERVICE_ID)
}

/// (Hyper-V manageable, guest agent service registered on this host).
pub fn availability() -> (bool, bool) {
    if !registry_key_exists(SERVICES_KEY) {
        return (false, false);
    }
    let usable = wmi::available();
    (usable, usable && registry_key_exists(&service_key()))
}

/// Registers the guest agent's Hyper-V socket service on this host. Needs an
/// elevated token once per host.
pub fn register_service() -> Result<()> {
    use windows::Win32::System::Registry::{
        HKEY, HKEY_LOCAL_MACHINE, KEY_WRITE, REG_OPTION_NON_VOLATILE, REG_SZ, RegCloseKey,
        RegCreateKeyExW, RegSetValueExW,
    };
    let mut key = HKEY::default();
    unsafe {
        RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            &windows::core::HSTRING::from(service_key()),
            None,
            None,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            None,
            &mut key,
            None,
        )
        .ok()
        .context("create the service key (run elevated)")?;
        let name: Vec<u8> = "ExoSnap verification guest agent\0"
            .encode_utf16()
            .flat_map(u16::to_le_bytes)
            .collect();
        let set = RegSetValueExW(
            key,
            &windows::core::HSTRING::from("ElementName"),
            None,
            REG_SZ,
            Some(&name),
        );
        let _ = RegCloseKey(key);
        set.ok().context("set ElementName")?;
    }
    Ok(())
}

/// One connection to the guest agent.
pub struct GuestClient {
    stream: hvsock::HvStream,
    next: u64,
    deadline: Option<Instant>,
}

impl GuestClient {
    /// Connects and completes `hello`, retrying while the VM boots.
    pub fn connect(vm_id: &str, deadline: Instant) -> Result<GuestClient> {
        let vm = hvsock::parse_guid(vm_id)?;
        let service = hvsock::parse_guid(exo_guest::SERVICE_ID)?;
        let nonce = crate::control::new_run_id("guest");
        let mut last = None;
        while Instant::now() < deadline {
            match hvsock::HvStream::connect(vm, service, Duration::from_secs(10)) {
                Ok(stream) => {
                    stream.set_timeout(Duration::from_secs(120))?;
                    let mut client = GuestClient {
                        stream,
                        next: 0,
                        deadline: Some(deadline),
                    };
                    match client.call(Request::Hello {
                        protocol: PROTOCOL_VERSION,
                        nonce: nonce.clone(),
                    }) {
                        Ok(reply) if reply["nonce"] == nonce.as_str() => {
                            client.deadline = None;
                            return Ok(client);
                        }
                        Ok(reply) => last = Some(anyhow::anyhow!("unexpected hello reply {reply}")),
                        Err(e) => last = Some(e),
                    }
                }
                Err(e) => last = Some(e),
            }
            std::thread::sleep(Duration::from_secs(3));
        }
        Err(last
            .unwrap_or_else(|| anyhow::anyhow!("no attempt made"))
            .context("the guest agent did not answer before the deadline"))
    }

    pub fn call(&mut self, request: Request) -> Result<Value> {
        let timeout = match &request {
            Request::Exec { timeout_ms, .. } | Request::Wait { timeout_ms, .. } => {
                Duration::from_millis(*timeout_ms).saturating_add(Duration::from_secs(30))
            }
            _ => Duration::from_secs(120),
        };
        let timeout = self
            .deadline
            .map(|d| timeout.min(d.saturating_duration_since(Instant::now())))
            .unwrap_or(timeout);
        if timeout.is_zero() {
            bail!("guest request deadline expired");
        }
        self.stream.set_timeout(timeout)?;
        self.next += 1;
        let id = self.next;
        frame::write(&mut self.stream, &Envelope { id, request })?;
        let response: Response =
            frame::read(&mut self.stream)?.context("the guest agent closed the connection")?;
        if response.id != id {
            bail!("response {} answers request {id}", response.id);
        }
        if !response.ok {
            bail!("guest: {}", response.error.unwrap_or_default());
        }
        Ok(response.result)
    }

    fn push_file(&mut self, local: &Path, remote: &str) -> Result<()> {
        let bytes = std::fs::read(local)?;
        let chunks: Vec<&[u8]> = if bytes.is_empty() {
            vec![&[]]
        } else {
            bytes.chunks(16 * 1024 * 1024).collect()
        };
        for (i, chunk) in chunks.into_iter().enumerate() {
            self.call(Request::WriteFile {
                path: remote.into(),
                data: base64::engine::general_purpose::STANDARD.encode(chunk),
                append: i > 0,
            })?;
        }
        Ok(())
    }

    fn pull_file(&mut self, remote: &str, local: &Path) -> Result<()> {
        if let Some(parent) = local.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let mut out = Vec::new();
        loop {
            let reply = self.call(Request::ReadFile {
                path: remote.into(),
                offset: out.len() as u64,
                max_len: 16 * 1024 * 1024,
            })?;
            let data = base64::engine::general_purpose::STANDARD
                .decode(reply["data"].as_str().unwrap_or_default())?;
            let eof = reply["eof"].as_bool().unwrap_or(true) || data.is_empty();
            out.extend_from_slice(&data);
            if eof {
                break;
            }
        }
        std::fs::write(local, out)?;
        Ok(())
    }

    fn pull_tree(&mut self, remote: &str, local: &Path) -> Result<()> {
        let entries = self.call(Request::ListDir {
            path: remote.into(),
        })?;
        for entry in entries.as_array().cloned().unwrap_or_default() {
            let name = entry["name"].as_str().unwrap_or_default();
            if name.is_empty() || name.contains(['/', '\\']) || name == ".." {
                continue;
            }
            let remote_child = format!(r"{remote}\{name}");
            if entry["isDir"].as_bool().unwrap_or(false) {
                self.pull_tree(&remote_child, &local.join(name))?;
            } else {
                self.pull_file(&remote_child, &local.join(name))?;
            }
        }
        Ok(())
    }
}

pub struct HyperV {
    manifest: BaseManifest,
    base: PathBuf,
    hypervisor: Option<wmi::Hypervisor>,
    vm: Option<wmi::Vm>,
    disk: Option<PathBuf>,
    run: Option<PathBuf>,
    client: Option<GuestClient>,
    network: recipe::NetworkMode,
    network_switch: Option<String>,
}

impl HyperV {
    pub fn from_environment() -> Result<HyperV> {
        let path = std::env::var_os(BASE_ENV)
            .map(PathBuf::from)
            .with_context(|| format!("set {BASE_ENV} to the base image manifest"))?;
        let (manifest, base) = BaseManifest::load(&path)?;
        let (network, network_switch) = recipe::network_from_environment()?;
        Ok(HyperV {
            manifest,
            base,
            hypervisor: None,
            vm: None,
            disk: None,
            run: None,
            client: None,
            network,
            network_switch,
        })
    }

    fn hypervisor(&self) -> Result<&wmi::Hypervisor> {
        self.hypervisor.as_ref().context("the VM was not prepared")
    }

    fn client(&mut self) -> Result<&mut GuestClient> {
        self.client
            .as_mut()
            .context("the guest agent is not connected")
    }

    fn reconnect(&mut self, within: Duration) -> Result<()> {
        let id = self.vm.as_ref().context("no VM")?.id.clone();
        self.client = Some(GuestClient::connect(&id, Instant::now() + within)?);
        Ok(())
    }
}

impl DisposableWindows for HyperV {
    fn kind(&self) -> BackendKind {
        BackendKind::HyperV
    }

    fn prepare(&mut self, run: &RunDir) -> Result<()> {
        self.manifest.verify(&self.base)?;
        let disk = run.host.join("run.vhdx");
        vhd::create_differencing(&self.base, &disk)?;
        self.disk = Some(disk.clone());
        self.run = Some(run.host.clone());
        let hypervisor = wmi::Hypervisor::connect()?;
        let name = format!(
            "exo-verify-{}",
            run.host
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default()
        );
        let vm = hypervisor.define(&wmi::VmSpec {
            name: &name,
            memory_mb: self.manifest.memory_mb,
            processors: self.manifest.processors,
            vhdx: &disk.to_string_lossy(),
        })?;
        self.hypervisor = Some(hypervisor);
        self.vm = Some(vm);
        let vm = self.vm.as_ref().context("defined VM was lost")?;
        recipe::configure_security(&name)?;
        let gpu_readback = self
            .manifest
            .gpu
            .as_ref()
            .map(|gpu| recipe::configure_partition(&name, gpu))
            .transpose()?;
        recipe::configure_network(&name, self.network, self.network_switch.as_deref())?;
        std::fs::write(
            run.host.join("environment.json"),
            serde_json::to_vec_pretty(&serde_json::json!({
                "backend": "hyperv",
                "vmId": vm.id,
                "vmName": name,
                "base": self.manifest,
                "gpuReadback": gpu_readback,
                "network": self.network,
                "networkSwitch": self.network_switch,
            }))?,
        )?;
        Ok(())
    }

    fn start(&mut self, run: &RunDir) -> Result<()> {
        self.hypervisor()?
            .start(self.vm.as_ref().context("no VM")?)?;
        self.reconnect(Duration::from_secs(600))?;
        let deadline = Instant::now() + Duration::from_secs(600);
        loop {
            let gate = self.client()?.call(Request::GateReady)?;
            if gate["ready"].as_bool() == Some(true) {
                break;
            }
            if Instant::now() >= deadline {
                bail!("the guest never reached an interactive session: {gate}");
            }
            std::thread::sleep(Duration::from_secs(5));
        }
        if self.manifest.gpu.is_some() || self.manifest.display.is_some() {
            self.client()?.push_file(
                &run.payload().join("exo-verify.exe"),
                &format!(r"{GUEST_ROOT}\payload\exo-verify.exe"),
            )?;
            let receipt = self.client()?.call(Request::Exec {
                program: format!(r"{GUEST_ROOT}\payload\exo-verify.exe"),
                args: vec!["disposable".into(), "guest-facts".into()],
                cwd: None,
                env: Default::default(),
                timeout_ms: 60_000,
            })?;
            if receipt["exitCode"].as_i64() != Some(0) {
                bail!("guest readiness measurement failed: {receipt}");
            }
            let measured: Value = serde_json::from_str(
                receipt["stdout"]
                    .as_str()
                    .context("guest facts output absent")?,
            )?;
            std::fs::write(
                run.host.join("readiness.json"),
                serde_json::to_vec_pretty(&measured)?,
            )?;
            let unmet = facts::unmet(
                &measured,
                self.manifest.gpu.as_ref(),
                self.manifest.display.as_ref(),
            );
            if !unmet.is_empty() {
                bail!("guest readiness unproven: {}", unmet.join("; "));
            }
        }
        Ok(())
    }

    fn execute(&mut self, command: &GuestCommand) -> Result<i32> {
        let run = self.run.clone().context("not prepared")?;
        for entry in std::fs::read_dir(run.join("payload"))? {
            let path = entry?.path();
            if path.is_file() {
                let name = path.file_name().unwrap().to_string_lossy().into_owned();
                self.client()?
                    .push_file(&path, &format!(r"{GUEST_ROOT}\payload\{name}"))?;
            }
        }
        let spawned = self.client()?.call(Request::SpawnLogged {
            program: command.program.clone(),
            args: command.args.clone(),
            cwd: Some(GUEST_ROOT.into()),
            env: Default::default(),
            transcript: format!(r"{GUEST_ROOT}\transcript.log"),
        })?;
        let handle = spawned["handle"]
            .as_u64()
            .context("spawn returned no handle")?;
        let deadline = Instant::now() + command.timeout;
        loop {
            let waited = self.client()?.call(Request::Wait {
                handle,
                timeout_ms: 30_000,
            })?;
            if waited["exited"].as_bool() == Some(true) {
                self.client()?.call(Request::WriteFile {
                    path: format!(r"{GUEST_ROOT}\out\campaign.exit"),
                    data: base64::engine::general_purpose::STANDARD
                        .encode(serde_json::to_vec(&waited)?),
                    append: false,
                })?;
                return Ok(waited["exitCode"].as_i64().unwrap_or(-1) as i32);
            }
            if Instant::now() >= deadline {
                let _ = self.client()?.call(Request::Kill { handle });
                bail!(
                    "the lane did not finish within {} s",
                    command.timeout.as_secs()
                );
            }
        }
    }

    fn collect(&mut self, run: &RunDir) -> Result<()> {
        let client = self.client()?;
        client.pull_tree(&format!(r"{GUEST_ROOT}\out"), &run.out())?;
        client.pull_file(
            &format!(r"{GUEST_ROOT}\transcript.log"),
            &run.host.join("transcript.log"),
        )?;
        Ok(())
    }

    fn stop(&mut self) -> Result<()> {
        let Some(vm) = self.vm.as_ref() else {
            return Ok(());
        };
        if let Some(mut client) = self.client.take() {
            let _ = client.call(Request::Shutdown);
        }
        let hypervisor = self.hypervisor()?;
        let deadline = Instant::now() + Duration::from_secs(90);
        while Instant::now() < deadline {
            if hypervisor.is_off(vm).unwrap_or(false) {
                return Ok(());
            }
            std::thread::sleep(Duration::from_secs(2));
        }
        hypervisor.power_off(vm)
    }

    fn destroy(&mut self) -> Result<()> {
        self.client = None;
        let mut errors = Vec::new();
        if let (Some(vm), Some(hypervisor)) = (self.vm.take(), self.hypervisor.as_ref())
            && let Err(e) = hypervisor.destroy(&vm)
        {
            errors.push(format!("destroy VM {}: {e:#}", vm.id));
            self.vm = Some(vm);
        }
        if self.vm.is_none()
            && let Some(disk) = self.disk.take()
            && disk.exists()
            && let Err(e) = std::fs::remove_file(&disk)
        {
            errors.push(format!("delete {}: {e}", disk.display()));
            self.disk = Some(disk);
        }
        if errors.is_empty() {
            Ok(())
        } else {
            bail!("{}", errors.join("; "))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_writable_or_changed_base_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let vhdx = dir.path().join("base.vhdx");
        std::fs::write(&vhdx, b"sealed").unwrap();
        let manifest = BaseManifest {
            image_id: "test".into(),
            windows_build: "26100".into(),
            update_level: "2026-09".into(),
            guest_agent_version: "0.1.0".into(),
            driver_set: "none".into(),
            provisioning_version: 1,
            vhdx: vhdx.clone(),
            sha256: crate::bundle::sha256_bytes(b"sealed"),
            memory_mb: 4096,
            processors: 2,
            gpu: None,
            display: None,
        };
        assert!(manifest.verify(&vhdx).is_err(), "writable base accepted");
        let mut permissions = std::fs::metadata(&vhdx).unwrap().permissions();
        permissions.set_readonly(true);
        std::fs::set_permissions(&vhdx, permissions.clone()).unwrap();
        manifest.verify(&vhdx).unwrap();
        let tampered = BaseManifest {
            sha256: crate::bundle::sha256_bytes(b"other"),
            ..manifest
        };
        let _ = std::fs::remove_file(vhdx.with_extension("vhdx.identity.json"));
        assert!(tampered.verify(&vhdx).is_err());
        #[allow(clippy::permissions_set_readonly_false)]
        permissions.set_readonly(false);
        std::fs::set_permissions(&vhdx, permissions).unwrap();
    }
}
