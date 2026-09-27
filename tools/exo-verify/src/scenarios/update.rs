//! Candidate-bound update journeys on a disposable Windows installation.

use serde_json::Value;
use serde_json::json;
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use super::chocolatey_worker;
use crate::bundle::{FileRole, sha256_file};
use crate::capability::Capability;
use crate::context::Context;
use crate::control::{self, Client};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "update.portable",
            revision: 3,
            title: "Portable update installs and relaunches the candidate",
            class: ScenarioClass::Installer,
            contract: "the old portable build accepts the signed candidate offer, installs the bound candidate bytes and relaunches the candidate from the updated tree",
            lane: Lane::CiUpdate,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
            ],
            timeout: std::time::Duration::from_secs(900),
            run: portable,
        },
        Scenario {
            id: "update.msi-decline",
            revision: 2,
            title: "Simulated declined MSI elevation leaves the installation intact",
            class: ScenarioClass::Installer,
            contract: "the updater's uacDeclined fault seam reports a declined elevation and preserves the old installation",
            lane: Lane::Nightly,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
            ],
            timeout: std::time::Duration::from_secs(900),
            run: msi_decline,
        },
        Scenario {
            id: "update.msi-decline-real",
            revision: 1,
            title: "Operator declines the real MSI UAC prompt",
            class: ScenarioClass::Installer,
            contract: "in a disposable guest, an unelevated old MSI install reports uacDeclined after a human refuses the real UAC prompt and leaves the old installation intact",
            lane: Lane::Hardware,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
                Capability::Operator,
            ],
            timeout: Duration::from_secs(900),
            run: msi_decline_real,
        },
        Scenario {
            id: "update.msi-accept",
            revision: 3,
            title: "MSI update installs and relaunches the candidate",
            class: ScenarioClass::Installer,
            contract: "accepting the signed candidate offer installs the bound candidate into the MSI installation, the old process ends, and the installed candidate executable runs again",
            lane: Lane::CiUpdate,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
            ],
            timeout: std::time::Duration::from_secs(900),
            run: msi_accept,
        },
        Scenario {
            id: "package.chocolatey-rehearsal",
            revision: 1,
            title: "Chocolatey package install and uninstall rehearsal",
            class: ScenarioClass::Installer,
            contract: "a disposable guest installs and uninstalls the candidate Chocolatey package while preserving user configuration",
            lane: Lane::CiUpdate,
            also: &[],
            tier: Tier::Recommended,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::DisposableOs,
            ],
            timeout: Duration::from_secs(900),
            run: chocolatey_rehearsal,
        },
    ]
}

fn ready(ctx: &Context) -> Step {
    ctx.require(Capability::DisposableOs)?;
    if std::env::var("EXO_VERIFY_UPDATE_FEED_READY").as_deref() != Ok("1") {
        return Err(Stop::unavailable(
            "the disposable guest must trust the local api.github.com test certificate and redirect api.github.com to its candidate feed; set EXO_VERIFY_UPDATE_FEED_READY=1 only after both are active",
        ));
    }
    let probe = crate::tools::run(
        Command::new("pwsh").args([
            "-NoProfile", "-NonInteractive", "-Command",
            "$ErrorActionPreference='Stop'; (Invoke-WebRequest -Uri 'https://api.github.com/repos/Exoridus/exosnap/releases' -NoProxy -TimeoutSec 15).Content",
        ]),
        Duration::from_secs(30),
    )?;
    infra_ensure!(
        probe.success(),
        "the disposable guest cannot read the trusted local HTTPS feed: {}",
        probe.stderr.trim()
    );
    let releases: Value = serde_json::from_str(&probe.stdout)?;
    let version = &ctx.bundle()?.inventory.product_version;
    infra_ensure!(
        releases.as_array().is_some_and(|items| items
            .iter()
            .any(|release| release["tag_name"] == format!("v{version}"))),
        "the api.github.com route did not serve the bound candidate {version}"
    );
    Ok(())
}

fn old_path(variable: &str) -> Step<PathBuf> {
    let value = std::env::var_os(variable).ok_or_else(|| {
        Stop::unavailable(format!("set {variable} to the previous official release"))
    })?;
    let path = PathBuf::from(value);
    if !path.is_file() {
        return Err(Stop::unavailable(format!(
            "{variable} does not name a file"
        )));
    }
    Ok(path)
}

pub(super) fn copy_tree(from: &Path, to: &Path) -> Step {
    std::fs::create_dir_all(to)?;
    for entry in std::fs::read_dir(from)? {
        let entry = entry?;
        let source = entry.path();
        let target = to.join(entry.file_name());
        if source.is_dir() {
            copy_tree(&source, &target)?;
        } else if source.is_file() {
            std::fs::copy(source, target)?;
        }
    }
    Ok(())
}

fn portable(ctx: &mut Context) -> Step {
    ready(ctx)?;
    let previous = old_path("EXOSNAP_UPDATE_FROM")?;
    let root = ctx
        .scenario_dir
        .join(format!("old-portable-{}", control::new_run_id("copy")));
    copy_tree(
        previous
            .parent()
            .ok_or_else(|| Stop::infra("previous portable executable has no parent"))?,
        &root,
    )?;
    let exe = root.join("exosnap.exe");
    infra_ensure!(
        exe.is_file() && root.join("exosnap-updater.exe").is_file(),
        "previous portable tree is incomplete"
    );
    drive_update(ctx, &exe, false, None)
}

fn installed_exe() -> Step<Option<PathBuf>> {
    let out = crate::tools::run(
        Command::new("reg.exe").args([
            "query",
            r"HKLM\SOFTWARE\Codexo\ExoSnap",
            "/v",
            "InstallPath",
        ]),
        Duration::from_secs(10),
    )?;
    if !out.success() {
        return Ok(None);
    }
    let path = out.stdout.lines().find_map(|line| {
        let (_, value) = line.split_once("REG_SZ")?;
        line.contains("InstallPath")
            .then_some(PathBuf::from(value.trim()).join("exosnap.exe"))
    });
    Ok(path.filter(|p| p.is_file()))
}

fn install_base(ctx: &mut Context) -> Step<PathBuf> {
    let base = old_path("EXOSNAP_UPDATE_FROM_MSI")?;
    let base_hash = sha256_file(&base)?.0;
    let marker = ctx.run_dir.join("update-base.json");
    if let Some(path) = installed_exe()? {
        let saved: Value = serde_json::from_slice(&std::fs::read(&marker).map_err(|_| {
            Stop::unavailable(
                "an existing ExoSnap installation has no baseline record from this disposable run",
            )
        })?)?;
        infra_ensure!(
            saved["msiSha256"] == base_hash && saved["exeSha256"] == sha256_file(&path)?.0,
            "the installed ExoSnap does not match this run's previous MSI baseline"
        );
        return Ok(path);
    }
    let log = ctx.scenario_dir.join("base-install.log");
    let result = crate::tools::run(
        Command::new("msiexec.exe")
            .arg("/i")
            .arg(base)
            .args(["/qn", "/norestart", "/l*v"])
            .arg(&log),
        Duration::from_secs(360),
    )?;
    ctx.keep(&log);
    infra_ensure!(
        result.code() == Some(0),
        "the previous MSI could not be installed: {:?}; {}",
        result.code(),
        log.display()
    );
    let exe = installed_exe()?
        .ok_or_else(|| Stop::infra("previous MSI installed but registered no executable"))?;
    std::fs::write(
        marker,
        serde_json::to_vec(&json!({"msiSha256": base_hash, "exeSha256": sha256_file(&exe)?.0}))?,
    )?;
    Ok(exe)
}

fn msi_decline(ctx: &mut Context) -> Step {
    ready(ctx)?;
    let exe = install_base(ctx)?;
    drive_update(ctx, &exe, true, Some(DeclineKind::Simulated))
}

fn msi_accept(ctx: &mut Context) -> Step {
    ready(ctx)?;
    let exe = install_base(ctx)?;
    drive_update(ctx, &exe, true, None)
}

fn msi_decline_real(ctx: &mut Context) -> Step {
    ctx.require(Capability::Operator)?;
    if ctx.has(Capability::Admin) {
        return Err(Stop::unavailable(
            "the real UAC decline gate must run from an unelevated interactive session inside the disposable guest",
        ));
    }
    ready(ctx)?;
    let exe = installed_exe()?.ok_or_else(|| Stop::unavailable("install the previous official MSI in the disposable guest before starting the unelevated operator run"))?;
    let previous = old_path("EXOSNAP_UPDATE_FROM")?;
    infra_ensure!(
        sha256_file(&exe)?.0 == sha256_file(&previous)?.0,
        "the guest's MSI-installed executable is not the provided previous official release"
    );
    if !ctx.ask("This gate will open a real Windows UAC prompt in the disposable guest. Is its desktop free, and will you select No on that prompt?")? {
        return Err(Stop::unavailable("operator did not confirm the disposable guest is ready for a real UAC refusal"));
    }
    drive_update(ctx, &exe, true, Some(DeclineKind::Operator))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DeclineKind {
    Simulated,
    Operator,
}

fn chocolatey_rehearsal(ctx: &mut Context) -> Step {
    ctx.require(Capability::DisposableOs)?;
    let checkout = std::env::var_os("EXO_VERIFY_CHECKOUT")
        .map(PathBuf::from)
        .ok_or_else(|| {
            Stop::unavailable(
                "set EXO_VERIFY_CHECKOUT to an exact-commit checkout inside the disposable guest",
            )
        })?;
    let expected_commit = ctx.bundle()?.inventory.source_commit.clone();
    let revision = crate::tools::run(
        Command::new("git")
            .arg("-C")
            .arg(&checkout)
            .args(["rev-parse", "HEAD"]),
        Duration::from_secs(10),
    )?;
    infra_ensure!(
        revision.success()
            && revision
                .stdout
                .trim()
                .eq_ignore_ascii_case(&expected_commit),
        "Chocolatey source checkout is not the candidate commit {expected_commit}"
    );
    let status = crate::tools::run(
        Command::new("git").arg("-C").arg(&checkout).args([
            "status",
            "--porcelain",
            "--",
            "packaging/chocolatey",
        ]),
        Duration::from_secs(10),
    )?;
    infra_ensure!(
        status.success() && status.stdout.trim().is_empty(),
        "Chocolatey package source differs from the candidate commit"
    );
    let package_source = checkout.join("packaging/chocolatey");
    if !package_source.join("exosnap.nuspec").is_file() {
        return Err(Stop::unavailable(
            "the candidate checkout has no packaging/chocolatey/exosnap.nuspec",
        ));
    }
    let nonce = control::new_run_id("choco");
    let staging = ctx.scenario_dir.join(format!("choco-staging-{nonce}"));
    let installer = ctx.package(FileRole::Installer)?;
    let (installer_hash, _) = sha256_file(&installer)?;
    let declared_hash = &ctx
        .bundle()?
        .inventory
        .file(FileRole::Installer)
        .ok_or_else(|| Stop::infra("candidate bundle lists no MSI"))?
        .sha256;
    infra_ensure!(
        &installer_hash == declared_hash,
        "candidate MSI changed before Chocolatey rehearsal"
    );
    let evidence = ctx.scenario_dir.join(format!("choco-evidence-{nonce}"));
    let result_path = ctx.scenario_dir.join(format!("choco-result-{nonce}.json"));
    let outcome = chocolatey_worker::run_rehearsal(
        &staging,
        &package_source,
        &installer,
        &installer_hash,
        &evidence,
    );
    let result = match outcome {
        Ok(result) => result,
        Err(error) => chocolatey_worker::RehearsalResult {
            ok: false,
            msi_path: installer.display().to_string(),
            msi_sha256: installer_hash,
            restore_ran: false,
            restore_exit_code: None,
            vcredist_before: String::new(),
            vcredist_after: String::new(),
            observations: vec![],
            completed_utc: crate::model::now_rfc3339(),
            steps: vec![],
            fatal: Some(error.to_string()),
        },
    };
    let document = serde_json::to_value(&result)?;
    std::fs::write(&result_path, serde_json::to_vec_pretty(&document)?)?;
    ctx.keep(&result_path);
    ctx.keep(&evidence);
    ctx.evidence
        .put("chocolateySteps", document["steps"].clone());
    chocolatey_verdict(&document)?;
    Ok(())
}

pub(super) fn chocolatey_verdict(document: &Value) -> Step {
    const REQUIRED: &[&str] = &[
        "prepare",
        "pack",
        "removeExisting",
        "install",
        "uninstall",
        "restore",
    ];
    let steps = document["steps"]
        .as_array()
        .ok_or_else(|| Stop::infra("Chocolatey result has no steps array"))?;
    for step in steps {
        let name = step["name"]
            .as_str()
            .ok_or_else(|| Stop::infra("Chocolatey result has an unnamed step"))?;
        let kind = step["kind"]
            .as_str()
            .ok_or_else(|| Stop::infra(format!("Chocolatey step {name} has no kind")))?;
        let ok = step["ok"]
            .as_bool()
            .ok_or_else(|| Stop::infra(format!("Chocolatey step {name} has no ok flag")))?;
        if !ok {
            let detail = step["detail"].as_str().unwrap_or_default();
            return match kind {
                "product" => Err(Stop::fail(format!("Chocolatey {name} failed: {detail}"))),
                "bootstrap" => Err(Stop::infra(format!(
                    "Chocolatey {name} setup failed: {detail}"
                ))),
                _ => Err(Stop::infra(format!(
                    "Chocolatey {name} has unknown kind {kind}"
                ))),
            };
        }
        infra_ensure!(
            matches!(kind, "product" | "bootstrap"),
            "Chocolatey {name} has unknown kind {kind}"
        );
    }
    for name in REQUIRED {
        infra_ensure!(
            steps
                .iter()
                .any(|step| step["name"] == *name && step["ok"] == true),
            "Chocolatey worker never completed required step {name}"
        );
    }
    infra_ensure!(
        document["fatal"].as_str().is_none_or(str::is_empty),
        "Chocolatey worker reported a fatal error: {}",
        document["fatal"]
    );
    Ok(())
}

fn start_old(ctx: &Context, exe: &Path, config: &Path, fault: bool) -> Step<(Child, Client)> {
    let run_id = control::new_run_id("updv");
    let mut command = old_command(
        exe,
        config,
        &ctx.scenario_dir.join("output"),
        &run_id,
        fault,
    );
    command.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = ctx.spawn(&mut command)?;
    capture_console(
        &mut child,
        &ctx.scenario_dir.join(format!("console-{run_id}.log")),
    )?;
    let client = match Client::connect("LiveVerify", &run_id, Duration::from_secs(60)) {
        Ok(client) => client,
        Err(error) => {
            return Err(match child.try_wait()? {
                Some(status) => Stop::fail(format!(
                    "previous app exited before control handshake: {status}"
                )),
                None => Stop::Infra(error),
            });
        }
    };
    Ok((child, client))
}

/// Copies the old app's console, and that of the updater it launches and
/// which inherits it, into `log` with a receive time per line, and still
/// forwards every line to this process's stderr. The old updater reports its
/// failures only there, so the time of that line is the time of the failure.
fn capture_console(child: &mut Child, log: &Path) -> Step {
    let file = Arc::new(Mutex::new(std::fs::File::create(log)?));
    fn pump(
        stream: impl Read + Send + 'static,
        name: &'static str,
        file: Arc<Mutex<std::fs::File>>,
    ) {
        std::thread::spawn(move || {
            for line in BufReader::new(stream).lines() {
                let Ok(line) = line else { break };
                eprintln!("{line}");
                if let Ok(mut file) = file.lock() {
                    let _ = writeln!(file, "{} {name} {line}", crate::model::now_rfc3339());
                }
            }
        });
    }
    if let Some(stdout) = child.stdout.take() {
        pump(stdout, "stdout", Arc::clone(&file));
    }
    if let Some(stderr) = child.stderr.take() {
        pump(stderr, "stderr", file);
    }
    Ok(())
}

fn old_command(exe: &Path, config: &Path, output: &Path, run_id: &str, fault: bool) -> Command {
    let mut command = Command::new(exe);
    command.args(["--live-verify-control", run_id]);
    command
        .env("EXOSNAP_CONFIG_DIR", config)
        .env("EXOSNAP_OUTPUT_DIR", output)
        .env_remove("EXOSNAP_UPDATER_FAULT");
    if fault {
        command.env("EXOSNAP_UPDATER_FAULT", "uacDeclined");
    }
    command
}

fn update_offer(client: &mut Client, version: &str) -> Step {
    client
        .request("update.check", json!({}), Duration::from_secs(15))?
        .map_err(|refusal| Stop::fail(format!("update.check refused: {refusal}")))?;
    let until = Instant::now() + Duration::from_secs(90);
    loop {
        let state = client.call("update.getState", json!({}))?;
        if state["updateAvailable"] == true {
            product_ensure!(
                state["availableVersion"] == version,
                "update offered {}, expected bound candidate {version}",
                state["availableVersion"]
            );
            return Ok(());
        }
        if state["checking"] == false && state["state"] == "error" {
            return Err(Stop::fail(format!("update check failed: {state}")));
        }
        product_ensure!(
            Instant::now() < until,
            "candidate update was not offered within 90 s: {state}"
        );
        std::thread::sleep(Duration::from_millis(250));
    }
}

fn drive_update(
    ctx: &mut Context,
    exe: &Path,
    installed: bool,
    decline_kind: Option<DeclineKind>,
) -> Step {
    let decline = decline_kind.is_some();
    let version = {
        let bundle = ctx.bundle()?;
        bundle.inventory.product_version.clone()
    };
    let candidate_exe = ctx.product()?.exe;
    let candidate_hash = sha256_file(&candidate_exe)?.0;
    let before_hash = sha256_file(exe)?.0;
    infra_ensure!(
        before_hash != candidate_hash,
        "previous executable is already the candidate"
    );
    let config = ctx
        .scenario_dir
        .join(format!("old-config-{}", control::new_run_id("config")));
    std::fs::create_dir_all(&config)?;
    let (mut first, mut first_client) = start_old(ctx, exe, &config, false)?;
    infra_ensure!(
        first_client.identity["officialBuild"] == true,
        "the previous executable does not identify as an official release"
    );
    infra_ensure!(
        first_client.identity["executableSha256"]
            .as_str()
            .is_some_and(|hash| hash.eq_ignore_ascii_case(&before_hash)),
        "the previous app's running executable identity does not match its staged bytes"
    );
    let selected = first_client
        .request(
            "settings.set",
            json!({"key":"app.updateChannel", "value":"Stable"}),
            Duration::from_secs(15),
        )?
        .map_err(|refusal| Stop::infra(format!("select Stable channel: {refusal}")))?;
    infra_ensure!(
        selected["values"]["app.updateChannel"] == "Stable",
        "previous app did not retain the Stable update channel: {selected}"
    );
    drop(first_client);
    #[cfg(windows)]
    infra_ensure!(
        crate::win::close_windows(first.id()) > 0,
        "the previous app has no window to close after selecting Stable"
    );
    crate::tools::wait(&mut first, Duration::from_secs(20)).map_err(|error| {
        Stop::infra(format!(
            "previous app did not close after selecting Stable: {error}"
        ))
    })?;
    let (mut old, mut client) = start_old(
        ctx,
        exe,
        &config,
        decline_kind == Some(DeclineKind::Simulated),
    )?;
    // The swap moves this tree right after the old app exits. When it fails
    // as "in use", this names the processes that still held it at that moment.
    #[cfg(windows)]
    let holder_watch = exe
        .parent()
        .map(|tree| crate::holders::HolderWatch::start(tree, old.id()));
    update_offer(&mut client, &version)?;
    if decline_kind == Some(DeclineKind::Operator) {
        ctx.announce("Decline the Windows UAC prompt on the disposable guest's Secure Desktop. The runner will observe the updater state afterwards.");
    }
    client
        .request("update.apply", json!({}), Duration::from_secs(20))?
        .map_err(|refusal| Stop::fail(format!("update.apply refused: {refusal}")))?;
    let state = client.call("update.getState", json!({}))?;
    let launch = &state["updaterLaunch"];
    let run_id = launch["controlRunId"].as_str().ok_or_else(|| {
        Stop::fail(format!(
            "update.apply launched no controlled updater: {launch}"
        ))
    })?;
    product_ensure!(
        launch["targetVersion"] == version,
        "updater target is {}, expected {version}",
        launch["targetVersion"]
    );
    let transaction = launch["updateTransactionId"]
        .as_str()
        .ok_or_else(|| Stop::fail("update.apply did not report a transaction id"))?
        .to_owned();
    let mut updater = Client::connect("Updater", run_id, Duration::from_secs(30))?;
    let until = Instant::now() + Duration::from_secs(if decline { 90 } else { 480 });
    let mut final_state = Value::Null;
    while Instant::now() < until {
        match updater.call("updater.getState", json!({})) {
            Ok(state) => {
                final_state = state;
                if decline && final_state["failureCase"] == "uacDeclined" {
                    break;
                }
                if !decline
                    && matches!(
                        final_state["phase"].as_str(),
                        Some("completed" | "restartPending" | "rebootRequired" | "failed")
                    )
                {
                    break;
                }
            }
            Err(error) if !decline => {
                ctx.evidence.put("updaterEndpointEnded", error.to_string());
                break;
            }
            Err(error) => {
                return Err(Stop::infra(format!(
                    "updater endpoint vanished before decline state: {error}"
                )));
            }
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    if final_state["phase"] == "failed" {
        ctx.evidence
            .put("failureObservedAt", crate::model::now_rfc3339());
        // Only now: the query opens files in the tree, which would itself
        // block the very move it is meant to explain.
        #[cfg(windows)]
        if let Some(tree) = exe.parent() {
            ctx.evidence
                .put("treeUsers", crate::holders::restart_manager_users(tree));
        }
    }
    #[cfg(windows)]
    if let Some(watch) = holder_watch {
        ctx.evidence.put("treeHolders", watch.finish());
    }
    ctx.evidence.put("updaterState", final_state.clone());
    if installed {
        product_ensure!(
            final_state["installMode"] == "installed",
            "the updater did not run in MSI installation mode: {final_state}"
        );
    }
    product_ensure!(
        final_state["updateTransactionId"] == transaction,
        "updater transaction does not match the application's launch: {final_state}"
    );
    if decline {
        declined_state(&final_state)?;
        let after_hash = sha256_file(exe)?.0;
        product_ensure!(
            after_hash == before_hash,
            "declined MSI update changed the installed executable"
        );
        updater
            .request("updater.close", json!({}), Duration::from_secs(10))?
            .map_err(|refusal| Stop::fail(format!("declined updater refused close: {refusal}")))?;
        let pid = launch["pid"]
            .as_u64()
            .ok_or_else(|| Stop::fail("updater launch has no process id"))?;
        let wait_command = format!(
            "$p=Get-Process -Id {pid} -ErrorAction SilentlyContinue; if($p -and -not $p.WaitForExit(30000)){{exit 1}}"
        );
        let waited = crate::tools::run(
            Command::new("pwsh").args(["-NoProfile", "-NonInteractive", "-Command", &wait_command]),
            Duration::from_secs(35),
        )?;
        product_ensure!(
            waited.success(),
            "declined updater did not exit after close"
        );
    } else {
        accepted_state(&final_state)?;
        let completed_at = Instant::now();
        product_ensure!(
            old.try_wait()?.is_some(),
            "updater reported relaunch while the old app was still running"
        );
        let target = if installed {
            installed_exe()?.ok_or_else(|| Stop::fail("installed executable disappeared"))?
        } else {
            exe.to_path_buf()
        };
        let until = Instant::now() + Duration::from_secs(180);
        while sha256_file(&target)
            .map(|(hash, _)| hash != candidate_hash)
            .unwrap_or(true)
            && Instant::now() < until
        {
            std::thread::sleep(Duration::from_millis(500));
        }
        product_ensure!(
            sha256_file(&target)?.0 == candidate_hash,
            "updater did not install candidate executable bytes"
        );
        ctx.evidence
            .put("updatedExecutableSha256", candidate_hash.clone());
        let relaunched = wait_relaunch(&target, old.id(), completed_at + RELAUNCH_WINDOW)?;
        ctx.evidence.put("relaunchedPid", relaunched);
        let (running_hash, _) = sha256_file(&target)?;
        product_ensure!(
            running_hash == candidate_hash,
            "the relaunched executable {} is not the candidate",
            target.display()
        );
        #[cfg(windows)]
        terminate(relaunched);
    }
    if decline {
        drop(client);
        let _ = old.kill();
        let _ = old.wait();
    }
    Ok(())
}

/// How long after the updater reports completion the relaunched application
/// has to be running.
const RELAUNCH_WINDOW: Duration = Duration::from_secs(60);

fn comparable_path(path: &Path) -> String {
    let text = path.to_string_lossy().replace('/', "\\");
    text.strip_prefix(r"\\?\")
        .unwrap_or(&text)
        .to_ascii_lowercase()
}

/// The process, other than `old_pid`, whose image is `target`.
fn relaunched_pid(
    processes: &[(u32, Option<PathBuf>)],
    old_pid: u32,
    target: &Path,
) -> Option<u32> {
    let target = comparable_path(target);
    processes
        .iter()
        .filter(|(pid, _)| *pid != old_pid)
        .find(|(_, image)| {
            image
                .as_deref()
                .is_some_and(|image| comparable_path(image) == target)
        })
        .map(|(pid, _)| *pid)
}

/// Waits for the relaunched application. The relaunch is not armed for
/// automation, so it is found by enumerating processes, not by an endpoint.
fn wait_relaunch(target: &Path, old_pid: u32, deadline: Instant) -> Step<u32> {
    let target = std::fs::canonicalize(target).unwrap_or_else(|_| target.to_path_buf());
    loop {
        #[cfg(windows)]
        let processes = exosnap_processes();
        #[cfg(not(windows))]
        let processes: Vec<(u32, Option<PathBuf>)> = Vec::new();
        if let Some(pid) = relaunched_pid(&processes, old_pid, &target) {
            return Ok(pid);
        }
        if Instant::now() >= deadline {
            return Err(Stop::fail(format!(
                "no exosnap.exe from {} was running {} s after the updater reported completion",
                target.display(),
                RELAUNCH_WINDOW.as_secs()
            )));
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

/// Every running `exosnap.exe` with its image path when it can be read.
#[cfg(windows)]
fn exosnap_processes() -> Vec<(u32, Option<PathBuf>)> {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW,
        TH32CS_SNAPPROCESS,
    };
    use windows::Win32::System::Threading::{
        OpenProcess, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION,
        QueryFullProcessImageNameW,
    };
    use windows::core::PWSTR;
    let mut out = Vec::new();
    let Ok(snapshot) = (unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) }) else {
        return out;
    };
    let mut entry = PROCESSENTRY32W {
        dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32,
        ..Default::default()
    };
    let mut more = unsafe { Process32FirstW(snapshot, &mut entry) }.is_ok();
    while more {
        let end = entry
            .szExeFile
            .iter()
            .position(|unit| *unit == 0)
            .unwrap_or(entry.szExeFile.len());
        if String::from_utf16_lossy(&entry.szExeFile[..end]).eq_ignore_ascii_case("exosnap.exe") {
            let pid = entry.th32ProcessID;
            let image = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) }
                .ok()
                .and_then(|process| {
                    let mut buffer = [0u16; 1024];
                    let mut length = buffer.len() as u32;
                    let read = unsafe {
                        QueryFullProcessImageNameW(
                            process,
                            PROCESS_NAME_WIN32,
                            PWSTR(buffer.as_mut_ptr()),
                            &mut length,
                        )
                    };
                    unsafe {
                        let _ = CloseHandle(process);
                    }
                    read.ok().map(|_| {
                        PathBuf::from(String::from_utf16_lossy(&buffer[..length as usize]))
                    })
                });
            out.push((pid, image));
        }
        more = unsafe { Process32NextW(snapshot, &mut entry) }.is_ok();
    }
    unsafe {
        let _ = CloseHandle(snapshot);
    }
    out
}

/// Ends the relaunched candidate, which no job of this run owns.
#[cfg(windows)]
fn terminate(pid: u32) {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Threading::{OpenProcess, PROCESS_TERMINATE, TerminateProcess};
    if let Ok(process) = unsafe { OpenProcess(PROCESS_TERMINATE, false, pid) } {
        unsafe {
            let _ = TerminateProcess(process, 1);
            let _ = CloseHandle(process);
        }
    }
}

fn declined_state(state: &Value) -> Step {
    if state["failureCase"] != "uacDeclined" || state["installState"] != "intact" {
        return Err(Stop::fail(format!(
            "declined updater state is not safe: {state}"
        )));
    }
    Ok(())
}

fn accepted_state(state: &Value) -> Step {
    product_ensure!(
        state["phase"] == "completed" && state["failureCase"].is_null(),
        "updater did not confirm a successful relaunch: {state}"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn decline_requires_failure_case_and_safe_installation() {
        declined_state(
            &json!({"phase":"failed", "failureCase":"uacDeclined", "installState":"intact"}),
        )
        .unwrap();
        assert!(
            declined_state(
                &json!({"phase":"failed", "failureCase":"downloadFailed", "installState":"intact"})
            )
            .is_err()
        );
        assert!(declined_state(&json!({"phase":"failed", "failureCase":"uacDeclined", "installState":"strandedInBackup"})).is_err());
    }

    #[test]
    fn accept_requires_successful_relaunch() {
        accepted_state(&json!({"phase":"completed", "failureCase":null})).unwrap();
        assert!(
            accepted_state(&json!({"phase":"restartPending", "failureCase":"launchFailed"}))
                .is_err()
        );
        assert!(accepted_state(&json!({"phase":"failed", "failureCase":"launchFailed"})).is_err());
        assert!(accepted_state(&json!({"phase":"rebootRequired", "failureCase":null})).is_err());
    }

    #[test]
    fn the_relaunch_is_a_new_process_running_the_updated_executable() {
        let target = Path::new(r"C:\Program Files\Codexo\ExoSnap\exosnap.exe");
        let processes = vec![
            (
                10,
                Some(PathBuf::from(
                    r"C:\Program Files\Codexo\ExoSnap\exosnap.exe",
                )),
            ),
            (11, Some(PathBuf::from(r"C:\Other\exosnap.exe"))),
            (12, None),
            (
                13,
                Some(PathBuf::from(
                    r"c:\program files\codexo\exosnap\EXOSNAP.EXE",
                )),
            ),
        ];
        assert_eq!(relaunched_pid(&processes, 10, target), Some(13));
        assert_eq!(relaunched_pid(&processes[..3], 10, target), None);
        assert_eq!(
            relaunched_pid(
                &processes,
                99,
                Path::new(r"\\?\C:\Program Files\Codexo\ExoSnap\exosnap.exe")
            ),
            Some(10)
        );
    }

    #[test]
    fn chocolatey_worker_verdict_preserves_product_and_bootstrap_failures() {
        let passed = json!({"fatal":"", "steps":[
            {"name":"prepare","kind":"bootstrap","ok":true},
            {"name":"pack","kind":"bootstrap","ok":true},
            {"name":"removeExisting","kind":"product","ok":true},
            {"name":"install","kind":"product","ok":true},
            {"name":"uninstall","kind":"product","ok":true},
            {"name":"restore","kind":"bootstrap","ok":true}
        ]});
        chocolatey_verdict(&passed).unwrap();
        let mut broken = passed.clone();
        broken["steps"][3]["ok"] = json!(false);
        assert!(matches!(chocolatey_verdict(&broken), Err(Stop::Fail(_))));
        broken["steps"][3]["kind"] = json!("bootstrap");
        assert!(matches!(chocolatey_verdict(&broken), Err(Stop::Infra(_))));
        assert!(matches!(
            chocolatey_verdict(&json!({"steps":[]})),
            Err(Stop::Infra(_))
        ));
    }

    #[test]
    fn real_uac_decline_requires_operator_and_has_no_injected_fault() {
        let real = scenarios()
            .into_iter()
            .find(|scenario| scenario.id == "update.msi-decline-real")
            .unwrap();
        assert_eq!(real.lane, Lane::Hardware);
        assert!(real.requires.contains(&Capability::Operator));
        assert!(real.requires.contains(&Capability::DisposableOs));
        assert!(!real.requires.contains(&Capability::Admin));
        let command = old_command(
            Path::new("exosnap.exe"),
            Path::new("config"),
            Path::new("output"),
            "run",
            false,
        );
        let fault = command
            .get_envs()
            .find(|(name, _)| name == &"EXOSNAP_UPDATER_FAULT");
        assert!(matches!(fault, Some((_, None))));
        let simulated = old_command(
            Path::new("exosnap.exe"),
            Path::new("config"),
            Path::new("output"),
            "run",
            true,
        );
        let injected = simulated
            .get_envs()
            .find(|(name, _)| name == &"EXOSNAP_UPDATER_FAULT");
        assert_eq!(
            injected
                .and_then(|(_, value)| value)
                .and_then(|value| value.to_str()),
            Some("uacDeclined")
        );
    }
}
