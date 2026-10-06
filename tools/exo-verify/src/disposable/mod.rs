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
/// selected scenario. A guest whose evidence could not be collected is retained
/// for recovery; successful collection permits cleanup even after a failed run.
pub fn run_lane(
    backend: &mut dyn DisposableWindows,
    request: &LaneRequest,
    host_out: &Path,
) -> LaneResult {
    let started_at = now_rfc3339();
    let run = RunDir {
        host: host_out.join(format!("{}-{}", backend.kind().name(), request.lane.name())),
    };
    let mut retain = false;
    let outcome = drive(backend, request, &run, &mut retain);
    let destroyed = if retain { Ok(()) } else { backend.destroy() };
    let mut result = match (outcome, destroyed) {
        (Ok(result), Ok(())) => result,
        (Ok(_), Err(e)) => synthetic_result(
            request,
            &started_at,
            Verdict::InfraError,
            &format!("disposable environment could not be destroyed: {e:#}"),
        ),
        (Err(e), cleanup) => synthetic_result(
            request,
            &started_at,
            Verdict::InfraError,
            &format!(
                "disposable environment: {e:#}{}",
                cleanup
                    .err()
                    .map(|e| format!("; cleanup: {e:#}"))
                    .unwrap_or_default()
            ),
        ),
    };
    result.backend = Some(backend.kind().name().to_string());
    result
}

fn drive(
    backend: &mut dyn DisposableWindows,
    request: &LaneRequest,
    run: &RunDir,
    retain: &mut bool,
) -> Result<LaneResult> {
    if run.host.exists() {
        bail!(
            "run directory {} already exists; preserve or recover its evidence before choosing a fresh output directory",
            run.host.display()
        );
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
    let code = execute_and_collect(
        backend,
        run,
        &GuestCommand {
            program: guest(r"payload\exo-verify.exe"),
            args,
            timeout: budget + Duration::from_secs(600),
        },
        retain,
    )?;
    backend.stop().context("stop the disposable environment")?;
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

fn execute_and_collect(
    backend: &mut dyn DisposableWindows,
    run: &RunDir,
    command: &GuestCommand,
    retain: &mut bool,
) -> Result<i32> {
    let execution = backend.execute(command);
    if let Err(collection) = backend.collect(run) {
        *retain = true;
        bail!(
            "evidence collection failed: {collection:#}; the guest and its disk are retained for recovery at {}{}",
            run.host.display(),
            execution
                .err()
                .map(|e| format!("; execution: {e:#}"))
                .unwrap_or_default()
        );
    }
    execution
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

    #[cfg(windows)]
    #[test]
    #[ignore = "requires a sealed Hyper-V base, explicit MSI smoke artifacts and a fresh output directory"]
    fn msi_distribution_ownership_system_smoke() -> Result<()> {
        let artifacts = std::env::var_os("EXO_VERIFY_MSI_SMOKE_ARTIFACTS")
            .context("set EXO_VERIFY_MSI_SMOKE_ARTIFACTS to the smoke artifact inventory")?;
        let mut input: serde_json::Value = serde_json::from_slice(&std::fs::read(artifacts)?)?;
        let packages = input["packages"].as_array().context("packages absent")?;
        anyhow::ensure!(
            packages.len() == 2,
            "two complete MSI packages are required"
        );
        anyhow::ensure!(
            packages[0]["productCode"] != packages[1]["productCode"]
                && packages[0]["upgradeCode"] == packages[1]["upgradeCode"],
            "the packages must exercise a major upgrade within one product family"
        );
        let run = RunDir {
            host: std::env::var_os("EXO_VERIFY_MSI_SMOKE_OUT")
                .map(PathBuf::from)
                .context("set EXO_VERIFY_MSI_SMOKE_OUT to a fresh absolute output directory")?,
        };
        anyhow::ensure!(
            run.host.is_absolute() && !run.host.exists(),
            "output must be fresh and absolute"
        );
        let base = std::env::var_os(hyperv::BASE_ENV).context("sealed Hyper-V base absent")?;
        let (manifest, _) = hyperv::BaseManifest::load(Path::new(&base))?;
        anyhow::ensure!(
            manifest.gpu.is_none() && manifest.display.is_none(),
            "the MSI ownership smoke requires a base without GPU or display qualification"
        );
        let mut backend = open(BackendKind::HyperV)?;
        std::fs::create_dir_all(run.payload())?;
        std::fs::create_dir_all(run.out())?;
        for (index, package) in input["packages"]
            .as_array_mut()
            .unwrap()
            .iter_mut()
            .enumerate()
        {
            let path = PathBuf::from(package["path"].as_str().context("MSI path absent")?);
            let hash = crate::bundle::sha256_file(&path)?.0;
            anyhow::ensure!(
                package["sha256"].as_str() == Some(hash.as_str()),
                "MSI artifact hash mismatch"
            );
            let name = format!("ownership-{index}.msi");
            std::fs::copy(path, run.payload().join(&name))?;
            package["path"] = serde_json::json!(format!(r"{GUEST_ROOT}\payload\{name}"));
        }
        if let Some(runtime) = input.get_mut("runtime") {
            let path = PathBuf::from(runtime["path"].as_str().context("runtime path absent")?);
            let hash = crate::bundle::sha256_file(&path)?.0;
            anyhow::ensure!(
                hash == crate::package::VC_REDIST_SHA256,
                "runtime does not match the repository pin"
            );
            std::fs::copy(path, run.payload().join("vc_redist.x64.exe"))?;
            runtime["path"] = serde_json::json!(format!(r"{GUEST_ROOT}\payload\vc_redist.x64.exe"));
            runtime["sha256"] = serde_json::json!(hash);
        }
        std::fs::copy(
            std::env::current_exe()?,
            run.payload().join("msi-ownership-smoke.exe"),
        )?;
        let mut retain = false;
        let outcome = (|| -> Result<i32> {
            backend.prepare(&run)?;
            let environment: serde_json::Value =
                serde_json::from_slice(&std::fs::read(run.host.join("environment.json"))?)?;
            input["vmId"] = environment["vmId"].clone();
            std::fs::write(
                run.payload().join("msi-ownership-smoke.json"),
                serde_json::to_vec_pretty(&input)?,
            )?;
            backend.start(&run)?;
            execute_and_collect(
                backend.as_mut(),
                &run,
                &GuestCommand {
                    program: format!(r"{GUEST_ROOT}\payload\msi-ownership-smoke.exe"),
                    args: vec![
                        "--ignored".into(),
                        "--exact".into(),
                        "scenarios::install::tests::msi_distribution_ownership_guest_smoke".into(),
                        "--nocapture".into(),
                    ],
                    timeout: Duration::from_secs(1200),
                },
                &mut retain,
            )
        })();
        if !retain {
            let stopped = backend.stop();
            let destroyed = backend.destroy();
            std::fs::write(
                run.host.join("cleanup.json"),
                serde_json::to_vec_pretty(&serde_json::json!({
                    "stopped": stopped.is_ok(), "destroyed": destroyed.is_ok(),
                    "stopError": stopped.as_ref().err().map(|e| format!("{e:#}")),
                    "destroyError": destroyed.as_ref().err().map(|e| format!("{e:#}")),
                }))?,
            )?;
            stopped?;
            destroyed?;
        }
        anyhow::ensure!(
            outcome? == 0,
            "the guest smoke failed; inspect its collected result and MSI logs"
        );
        Ok(())
    }

    #[derive(Default)]
    struct Lifecycle {
        fail_execute: bool,
        fail_collect: bool,
        fail_destroy: bool,
        result: Option<LaneResult>,
        events: Vec<&'static str>,
    }

    impl DisposableWindows for Lifecycle {
        fn kind(&self) -> BackendKind {
            BackendKind::HyperV
        }
        fn prepare(&mut self, _: &RunDir) -> Result<()> {
            self.events.push("prepare");
            Ok(())
        }
        fn start(&mut self, _: &RunDir) -> Result<()> {
            self.events.push("start");
            Ok(())
        }
        fn execute(&mut self, _: &GuestCommand) -> Result<i32> {
            self.events.push("execute");
            if self.fail_execute {
                bail!("campaign failed")
            }
            Ok(0)
        }
        fn collect(&mut self, run: &RunDir) -> Result<()> {
            self.events.push("collect");
            if self.fail_collect {
                bail!("transport failed")
            }
            if let Some(result) = &self.result {
                std::fs::write(
                    run.out().join(format!("{}.result.json", result.lane)),
                    serde_json::to_vec(result)?,
                )?;
            }
            Ok(())
        }
        fn stop(&mut self) -> Result<()> {
            self.events.push("stop");
            Ok(())
        }
        fn destroy(&mut self) -> Result<()> {
            self.events.push("destroy");
            if self.fail_destroy {
                bail!("cleanup failed");
            }
            Ok(())
        }
    }

    #[test]
    fn evidence_is_rescued_after_failure_and_failed_rescue_preserves_guest() {
        for fail_execute in [false, true] {
            for fail_collect in [false, true] {
                let mut backend = Lifecycle {
                    fail_execute,
                    fail_collect,
                    events: vec![],
                    ..Default::default()
                };
                let mut retain = false;
                let result = execute_and_collect(
                    &mut backend,
                    &RunDir {
                        host: "unused".into(),
                    },
                    &GuestCommand {
                        program: "unused".into(),
                        args: vec![],
                        timeout: Duration::ZERO,
                    },
                    &mut retain,
                );
                assert_eq!(result.is_err(), fail_execute || fail_collect);
                assert_eq!(retain, fail_collect);
                assert_eq!(backend.events, ["execute", "collect"]);
                if fail_execute && fail_collect {
                    let detail = result.unwrap_err().to_string();
                    assert!(detail.contains("campaign failed"));
                    assert!(detail.contains("transport failed"));
                    assert!(detail.contains("retained for recovery"));
                }
            }
        }
    }

    #[test]
    fn lane_cleanup_is_ordered_and_failed_collection_retains_the_environment() {
        let scenarios = crate::scenarios::registry();
        let request = LaneRequest {
            lane: Lane::CiInstall,
            profile: "release",
            bundle: None,
            only: &[],
            skip: &[],
            attest: &[],
            keep_media: false,
            scenarios: vec![&scenarios[0]],
            slot: None,
        };
        for (execute, collect, destroy) in [
            (false, false, false),
            (true, false, false),
            (false, true, false),
            (true, true, false),
            (false, false, true),
            (true, false, true),
        ] {
            let directory = tempfile::tempdir().unwrap();
            let mut backend = Lifecycle {
                fail_execute: execute,
                fail_collect: collect,
                fail_destroy: destroy,
                result: Some(synthetic_result(
                    &request,
                    &now_rfc3339(),
                    Verdict::Pass,
                    "fixture campaign",
                )),
                ..Default::default()
            };
            let result = run_lane(&mut backend, &request, directory.path());
            assert_eq!(
                result.scenarios[0].verdict,
                if execute || collect || destroy {
                    Verdict::InfraError
                } else {
                    Verdict::Pass
                }
            );
            assert_eq!(
                &backend.events[..4],
                ["prepare", "start", "execute", "collect"]
            );
            assert_eq!(backend.events.contains(&"destroy"), !collect);
            if !execute && !collect {
                assert_eq!(&backend.events[4..], ["stop", "destroy"]);
            }
            if execute && destroy {
                assert!(result.scenarios[0].detail.contains("campaign failed"));
                assert!(result.scenarios[0].detail.contains("cleanup failed"));
            }
        }
    }

    #[test]
    fn a_colliding_run_directory_is_never_cleared() {
        let directory = tempfile::tempdir().unwrap();
        let existing = directory
            .path()
            .join(format!("hyperv-{}", Lane::CiInstall.name()));
        std::fs::create_dir(&existing).unwrap();
        std::fs::write(existing.join("evidence"), "preserve").unwrap();
        let scenarios = crate::scenarios::registry();
        let request = LaneRequest {
            lane: Lane::CiInstall,
            profile: "release",
            bundle: None,
            only: &[],
            skip: &[],
            attest: &[],
            keep_media: false,
            scenarios: vec![&scenarios[0]],
            slot: None,
        };
        let mut backend = Lifecycle::default();
        let result = run_lane(&mut backend, &request, directory.path());
        assert_eq!(result.scenarios[0].verdict, Verdict::InfraError);
        assert!(!backend.events.contains(&"prepare"));
        assert_eq!(
            std::fs::read_to_string(existing.join("evidence")).unwrap(),
            "preserve"
        );
    }

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
