//! Disposable Windows environments a lane can run inside.
//!
//! Scenarios never name a backend. They declare capabilities, and the traits a
//! backend guarantees (a discardable OS, reboot, real UAC) decide where a lane
//! runs. Everything else a scenario needs is probed inside the environment by
//! the same `exo-verify` binary, so an environment that lacks it reports
//! UNAVAILABLE instead of failing the product.
//!
//! A backend runs one whole lane: it copies this executable and the candidate
//! bundle in, runs `exo-verify run` for that lane inside, and carries the lane
//! result and its evidence back out. The host never reinterprets guest verdicts.

#[cfg(windows)]
pub mod hyperv;
pub mod sandbox;

use anyhow::{Context as _, Result, bail};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::capability::{Capability, CapabilitySet};
use crate::model::{
    Identity, LaneResult, RESULT_SCHEMA_VERSION, ScenarioResult, Verdict, current_attempt,
    now_rfc3339, runner_version,
};
use crate::scenario::{Lane, Scenario};

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum BackendKind {
    Local,
    Sandbox,
    HyperV,
}

impl BackendKind {
    pub const ALL: [BackendKind; 3] = [
        BackendKind::Local,
        BackendKind::Sandbox,
        BackendKind::HyperV,
    ];

    pub fn name(self) -> &'static str {
        match self {
            BackendKind::Local => "local",
            BackendKind::Sandbox => "sandbox",
            BackendKind::HyperV => "hyperv",
        }
    }

    pub fn parse(name: &str) -> Option<BackendKind> {
        BackendKind::ALL.into_iter().find(|k| k.name() == name)
    }

    /// Environment traits the backend guarantees to every lane it runs.
    /// Hardware facts (GPU, displays, audio) are not traits; the guest
    /// probes them itself.
    pub fn guarantees(self) -> &'static [Capability] {
        match self {
            BackendKind::Local => &[],
            BackendKind::Sandbox => &[
                Capability::DisposableOs,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::Network,
            ],
            BackendKind::HyperV => &[
                Capability::DisposableOs,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::Reboot,
                Capability::RealUac,
            ],
        }
    }

    /// The host capability that makes this backend usable at all.
    fn host_requirement(self) -> Option<Capability> {
        match self {
            BackendKind::Local => None,
            BackendKind::Sandbox => Some(Capability::WindowsSandbox),
            BackendKind::HyperV => Some(Capability::HyperVSockets),
        }
    }
}

/// Requirements a backend, not the machine's hardware, has to satisfy.
fn is_trait(c: Capability) -> bool {
    matches!(
        c,
        Capability::DisposableOs | Capability::Reboot | Capability::RealUac
    )
}

/// Picks where a set of scenarios runs: here when this machine already offers
/// every environment trait they need, otherwise the cheapest disposable
/// backend that guarantees them. `None` means no backend on this host can
/// qualify them, which makes them UNAVAILABLE, never FAIL.
pub fn select(requires: &[Capability], host: &CapabilitySet) -> Option<BackendKind> {
    let traits: Vec<Capability> = requires.iter().copied().filter(|c| is_trait(*c)).collect();
    if traits.iter().all(|c| host.has(*c)) {
        return Some(BackendKind::Local);
    }
    [BackendKind::Sandbox, BackendKind::HyperV]
        .into_iter()
        .filter(|k| k.host_requirement().is_none_or(|c| host.has(c)))
        .find(|k| traits.iter().all(|c| k.guarantees().contains(c)))
}

/// Union of the requirements of every scenario a lane will run.
pub fn lane_requirements(scenarios: &[&Scenario]) -> Vec<Capability> {
    let mut all: Vec<Capability> = scenarios
        .iter()
        .flat_map(|s| s.requires.iter().copied())
        .collect();
    all.sort();
    all.dedup();
    all
}

/// The qualification slot a result belongs to: which kind of machine or
/// environment produced it. Results are `scenario x slot`, so a PASS on a
/// sandbox never stands in for one on an HDR display.
pub fn default_slot(backend: BackendKind, caps: &CapabilitySet) -> String {
    match backend {
        BackendKind::Sandbox => "windows11-sandbox".into(),
        BackendKind::HyperV => "windows11-hyperv-clean".into(),
        BackendKind::Local => {
            let vendor = if caps.has(Capability::NvidiaGpu) {
                "nvidia"
            } else {
                "local"
            };
            let range = if caps.has(Capability::HdrDisplay) {
                "hdr"
            } else {
                "sdr"
            };
            format!("windows11-{vendor}-{range}")
        }
    }
}

/// One process to run inside the environment.
pub struct GuestCommand {
    pub program: String,
    pub args: Vec<String>,
    pub timeout: Duration,
}

/// A host directory shared with, or copied into, the environment.
pub struct RunDir {
    pub host: PathBuf,
}

impl RunDir {
    pub fn payload(&self) -> PathBuf {
        self.host.join("payload")
    }
    pub fn out(&self) -> PathBuf {
        self.host.join("out")
    }
}

/// Where the run directory appears inside the guest.
pub const GUEST_ROOT: &str = r"C:\ExoVerify";

/// One disposable environment. Implementations must make `destroy`
/// idempotent and safe after any partial failure: it is always called.
pub trait DisposableWindows {
    fn kind(&self) -> BackendKind;
    /// Creates the environment without starting it.
    fn prepare(&mut self, run: &RunDir) -> Result<()>;
    /// Boots and waits until commands can run in the interactive session.
    fn start(&mut self, run: &RunDir) -> Result<()>;
    /// Runs one command and returns its exit code. Output goes to files in
    /// the run directory, because not every backend returns it.
    fn execute(&mut self, command: &GuestCommand) -> Result<i32>;
    /// Makes the guest's `out` directory available under `run.out()`.
    fn collect(&mut self, run: &RunDir) -> Result<()>;
    fn stop(&mut self) -> Result<()>;
    fn destroy(&mut self) -> Result<()>;
}

pub struct LaneRequest<'a> {
    pub lane: Lane,
    pub profile: &'a str,
    pub bundle: Option<&'a Path>,
    pub only: &'a [String],
    pub skip: &'a [String],
    pub attest: &'a [String],
    pub keep_media: bool,
    pub scenarios: Vec<&'a Scenario>,
    pub slot: Option<&'a str>,
}

/// Runs one lane inside a fresh environment and returns the guest's own lane
/// result. Any failure to obtain that result becomes INFRA_ERROR for every
/// selected scenario; the environment is destroyed in every case.
pub fn run_lane(
    backend: &mut dyn DisposableWindows,
    request: &LaneRequest,
    host_out: &Path,
) -> LaneResult {
    let started_at = now_rfc3339();
    let run = RunDir {
        host: host_out.join(format!("{}-{}", backend.kind().name(), request.lane.name())),
    };
    let outcome = drive(backend, request, &run);
    let destroyed = backend.destroy();
    let mut result = match (outcome, destroyed) {
        (Ok(result), Ok(())) => result,
        (Ok(_), Err(e)) => synthetic_result(
            request,
            &started_at,
            Verdict::InfraError,
            &format!("disposable environment could not be destroyed: {e:#}"),
        ),
        (Err(e), _) => synthetic_result(
            request,
            &started_at,
            Verdict::InfraError,
            &format!("disposable environment: {e:#}"),
        ),
    };
    result.backend = Some(backend.kind().name().to_string());
    result
}

fn drive(
    backend: &mut dyn DisposableWindows,
    request: &LaneRequest,
    run: &RunDir,
) -> Result<LaneResult> {
    if run.host.exists() {
        std::fs::remove_dir_all(&run.host)
            .with_context(|| format!("clear {}", run.host.display()))?;
    }
    std::fs::create_dir_all(run.payload())?;
    std::fs::create_dir_all(run.out())?;
    let exe = std::env::current_exe()?;
    std::fs::copy(&exe, run.payload().join("exo-verify.exe")).context("stage exo-verify.exe")?;
    let guest = |relative: &str| format!(r"{GUEST_ROOT}\{relative}");
    let mut args = vec![
        "run".to_string(),
        "--profile".into(),
        request.profile.into(),
        "--lane".into(),
        request.lane.name().into(),
        "--out".into(),
        guest("out"),
        "--backend".into(),
        "local".into(),
        "--environment".into(),
        backend.kind().name().into(),
    ];
    if let Some(bundle) = request.bundle {
        let staged = run.payload().join("bundle.zip");
        if bundle.is_dir() {
            crate::bundle::pack(&crate::bundle::open(bundle)?, &staged)?;
        } else {
            std::fs::copy(bundle, &staged).context("stage bundle archive")?;
        }
        args.extend(["--bundle".into(), guest(r"payload\bundle.zip")]);
    }
    for c in backend.kind().guarantees() {
        if c.is_attested() {
            args.extend(["--attest".into(), c.name().into()]);
        }
    }
    for a in request.attest {
        args.extend(["--attest".into(), a.clone()]);
    }
    for o in request.only {
        args.extend(["--only".into(), o.clone()]);
    }
    for s in request.skip {
        args.extend(["--skip".into(), s.clone()]);
    }
    if let Some(slot) = request.slot {
        args.extend(["--slot".into(), slot.into()]);
    }
    if request.keep_media {
        args.push("--keep-media".into());
    }
    let budget: Duration = request.scenarios.iter().map(|s| s.timeout).sum();
    backend.prepare(run)?;
    backend.start(run)?;
    let code = backend.execute(&GuestCommand {
        program: guest(r"payload\exo-verify.exe"),
        args,
        timeout: budget + Duration::from_secs(600),
    })?;
    backend.collect(run)?;
    let _ = backend.stop();
    let file = run
        .out()
        .join(format!("{}.result.json", request.lane.name()));
    let text = std::fs::read_to_string(&file).with_context(|| {
        format!(
            "guest exo-verify exited {code} without a lane result at {}",
            file.display()
        )
    })?;
    let result: LaneResult =
        serde_json::from_str(&text).with_context(|| format!("parse {}", file.display()))?;
    if result.lane != request.lane.name() {
        bail!("guest produced a result for lane {}", result.lane);
    }
    Ok(result)
}

/// A lane result for scenarios that never reached an environment.
pub fn synthetic_result(
    request: &LaneRequest,
    started_at: &str,
    verdict: Verdict,
    detail: &str,
) -> LaneResult {
    LaneResult {
        schema_version: RESULT_SCHEMA_VERSION,
        lane: request.lane.name().into(),
        profile: request.profile.into(),
        runner_version: runner_version(),
        started_at: started_at.into(),
        finished_at: now_rfc3339(),
        attempt: current_attempt(),
        identity: Identity::default(),
        environment: BTreeMap::new(),
        tools: BTreeMap::new(),
        capabilities: vec![],
        slot: request.slot.map(str::to_string),
        backend: None,
        scenarios: request
            .scenarios
            .iter()
            .map(|s| ScenarioResult {
                id: s.id.into(),
                scenario_revision: s.revision,
                verdict,
                detail: detail.to_string(),
                duration_ms: 0,
                missing_capabilities: vec![],
                evidence: BTreeMap::new(),
                artifacts: vec![],
            })
            .collect(),
    }
}

/// Host-side backend availability, for `capabilities` and selection.
#[cfg(windows)]
pub fn probe_into(set: &mut CapabilitySet) {
    let sandbox = sandbox::available();
    set.facts.insert(
        "windowsSandbox".into(),
        serde_json::json!({"available": sandbox}),
    );
    if sandbox {
        set.insert(Capability::WindowsSandbox);
    }
    let (hyperv, sockets) = hyperv::availability();
    set.facts.insert(
        "hyperV".into(),
        serde_json::json!({"available": hyperv, "guestServiceRegistered": sockets}),
    );
    if hyperv {
        set.insert(Capability::HyperV);
        if sockets {
            set.insert(Capability::HyperVSockets);
        }
    }
}

/// Opens the backend of `kind` from its host-side configuration.
pub fn open(kind: BackendKind) -> Result<Box<dyn DisposableWindows>> {
    match kind {
        BackendKind::Local => bail!("the local backend does not run in a disposable environment"),
        BackendKind::Sandbox => Ok(Box::new(sandbox::Sandbox::new())),
        #[cfg(windows)]
        BackendKind::HyperV => Ok(Box::new(hyperv::HyperV::from_environment()?)),
        #[cfg(not(windows))]
        BackendKind::HyperV => bail!("Hyper-V needs a Windows host"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn host(names: &[&str]) -> CapabilitySet {
        CapabilitySet::from_names(names)
    }

    #[test]
    fn scenarios_without_environment_traits_run_here() {
        assert_eq!(
            select(&[Capability::Windows, Capability::Nvenc], &host(&[])),
            Some(BackendKind::Local)
        );
    }

    #[test]
    fn an_attested_disposable_host_runs_installer_lanes_itself() {
        assert_eq!(
            select(&[Capability::DisposableOs], &host(&["disposable-os"])),
            Some(BackendKind::Local)
        );
    }

    #[test]
    fn a_clean_os_prefers_the_sandbox() {
        assert_eq!(
            select(
                &[Capability::DisposableOs, Capability::Admin],
                &host(&["windows-sandbox", "hyper-v", "hyper-v-sockets"])
            ),
            Some(BackendKind::Sandbox)
        );
    }

    #[test]
    fn reboot_and_real_uac_need_hyper_v() {
        let both = host(&["windows-sandbox", "hyper-v", "hyper-v-sockets"]);
        assert_eq!(
            select(&[Capability::DisposableOs, Capability::Reboot], &both),
            Some(BackendKind::HyperV)
        );
        assert_eq!(
            select(&[Capability::RealUac], &host(&["windows-sandbox"])),
            None
        );
    }

    #[test]
    fn hyper_v_without_the_guest_service_is_not_usable() {
        assert_eq!(select(&[Capability::Reboot], &host(&["hyper-v"])), None);
    }

    #[test]
    fn slots_name_the_environment() {
        assert_eq!(
            default_slot(BackendKind::Local, &host(&["nvidia-gpu", "hdr-display"])),
            "windows11-nvidia-hdr"
        );
        assert_eq!(
            default_slot(BackendKind::Local, &host(&[])),
            "windows11-local-sdr"
        );
        assert_eq!(
            default_slot(BackendKind::Sandbox, &host(&[])),
            "windows11-sandbox"
        );
    }
}
