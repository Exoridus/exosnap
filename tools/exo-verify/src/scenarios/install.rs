//! MSI installation on a disposable Windows machine.

use anyhow::Result;
use serde_json::json;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use super::common::secs;
use crate::bundle::{FileRole, sha256_file};
use crate::capability::Capability;
use crate::context::Context;
use crate::control::{self, Client};
use crate::package::walk_files;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

/// The registry key an installed ExoSnap publishes its product state under.
/// Shared with the Chocolatey rehearsal, which installs and removes the same
/// product through a different channel and must recognize the same state.
pub(crate) const INSTALL_KEY: &str = r"HKLM\SOFTWARE\ExoSnap";
pub(crate) const USER_KEY: &str = r"HKCU\SOFTWARE\ExoSnap";

pub fn scenarios() -> Vec<Scenario> {
    vec![
        // The Setup scenario starts and ends with no product so the raw-MSI
        // scenario, which asserts a clean machine, can follow it.
        Scenario {
            id: "install.setup-cycle",
            revision: 1,
            title: "The offline Setup installs, repairs and uninstalls without its original file",
            class: ScenarioClass::Installer,
            contract: "on a clean disposable Windows machine, ExoSnap Setup installs silently with the default options (no desktop shortcut, one Installed Apps entry, Start Menu entry), repairs from Burn's cache after the original Setup.exe is deleted, and uninstalls while preserving the shared runtime and user data",
            lane: Lane::CiInstall,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
            ],
            timeout: secs(1800.0),
            run: setup_cycle,
        },
        Scenario {
            id: "install.setup-interactive",
            revision: 1,
            title: "The interactive Setup honors its checkboxes and Launch action",
            class: ScenarioClass::Installer,
            contract: "on a clean disposable Windows machine, the interactive ExoSnap Setup shows an unchecked desktop-shortcut option, creates the shortcut only when it is selected, launches the installed application from the success page, and its uninstall page shows an unchecked remove-local-data option that removes only local user data when selected",
            lane: Lane::CiInstall,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
            ],
            timeout: secs(1800.0),
            run: setup_interactive,
        },
        Scenario {
            id: "install.msi-cycle",
            revision: 1,
            title: "The MSI installs, starts, uninstalls and reinstalls cleanly",
            class: ScenarioClass::Installer,
            contract: "on a clean disposable Windows machine, the MSI installs the portable bytes, first and second starts are healthy, uninstall preserves user settings, and reinstall restores the product",
            lane: Lane::CiInstall,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
                Capability::MsvcRuntime,
            ],
            timeout: secs(900.0),
            run: msi_cycle,
        },
        Scenario {
            id: "install.msi-user-data-removal",
            revision: 1,
            title: "An explicit uninstall removes only the current user's local data",
            class: ScenarioClass::Installer,
            contract: "on a disposable Windows machine, an MSI uninstall with EXOSNAP_REMOVE_USER_DATA=1 removes the current user's %LOCALAPPDATA%\\ExoSnap and leaves recordings outside it byte-identical, then restores an installed product",
            lane: Lane::CiInstall,
            also: &[],
            tier: Tier::Required,
            requires: &[
                Capability::Windows,
                Capability::Admin,
                Capability::InteractiveDesktop,
                Capability::DisposableOs,
                Capability::MsvcRuntime,
            ],
            timeout: secs(900.0),
            run: msi_user_data_removal,
        },
    ]
}

/// One Add/Remove Programs row, from either registry view.
struct ArpRow {
    display_name: String,
    publisher: String,
    system_component: bool,
    uninstall_string: String,
}

fn is_product_code(name: &str) -> bool {
    let bytes = name.as_bytes();
    bytes.len() == 38
        && bytes[0] == b'{'
        && bytes[37] == b'}'
        && bytes[1..37]
            .iter()
            .all(|b| b.is_ascii_hexdigit() || *b == b'-')
}

/// Every Uninstall-registry row on the machine, both views.
fn arp_rows() -> Result<Vec<ArpRow>> {
    let roots = [
        r"HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
        r"HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
    ];
    let mut rows = Vec::new();
    for root in roots {
        let listing = crate::tools::run(Command::new("reg.exe").args(["query", root]), secs(20.0))?;
        if !listing.success() {
            continue;
        }
        for line in listing.stdout.lines() {
            let name = line.trim().rsplit('\\').next().unwrap_or_default();
            if !is_product_code(name) {
                continue;
            }
            let subkey = format!("{root}\\{name}");
            let props =
                crate::tools::run(Command::new("reg.exe").args(["query", &subkey]), secs(20.0))?;
            if !props.success() {
                continue;
            }
            let value = |key: &str| parse_reg_value(&props.stdout, key).unwrap_or_default();
            rows.push(ArpRow {
                display_name: value("DisplayName"),
                publisher: value("Publisher"),
                system_component: value("SystemComponent").trim() == "0x1",
                uninstall_string: value("UninstallString"),
            });
        }
    }
    Ok(rows)
}

fn exosnap_arp_rows() -> Result<Vec<ArpRow>> {
    Ok(arp_rows()?
        .into_iter()
        .filter(|row| row.display_name.starts_with("ExoSnap") && row.publisher == "Codexo")
        .collect())
}

/// The executable quoted at the start of an UninstallString.
fn quoted_exe(command: &str) -> Option<PathBuf> {
    let rest = command.trim();
    let start = rest.find('"')? + 1;
    let end = rest[start..].find('"')? + start;
    Some(PathBuf::from(&rest[start..end]))
}

fn run_setup(
    ctx: &mut Context,
    setup: &Path,
    verb: &str,
    display: &str,
    log_name: &str,
) -> Result<crate::tools::Output> {
    let log = ctx.scenario_dir.join(log_name);
    let mut command = Command::new(setup);
    command
        .arg(verb)
        .args([display, "/norestart", "/log"])
        .arg(&log);
    let out = crate::tools::run(&mut command, secs(1200.0))?;
    ctx.keep(&log);
    Ok(out)
}

fn burn_log(log: &Path) -> String {
    std::fs::read_to_string(log).unwrap_or_default()
}

fn setup_cycle(ctx: &mut Context) -> Step {
    let bundle = ctx.bundle()?;
    let version = bundle.inventory.product_version.clone();
    let commit = bundle.inventory.source_commit.clone();
    let setup_source = ctx.package(FileRole::Setup)?;

    // This scenario runs before the raw-MSI scenarios and must leave the
    // machine clean for them, so it both starts and ends with no product.
    let residue = super::clean::residue()?;
    ctx.evidence.put("residueBeforeSetup", json!(residue));
    infra_ensure!(
        residue.is_empty(),
        "the disposable machine is not clean: {}",
        residue.join("; ")
    );

    let work = ctx.scenario_dir.join("setup-copy");
    fs::create_dir_all(&work)?;
    let setup = work.join(
        setup_source
            .file_name()
            .ok_or_else(|| Stop::infra("Setup has no file name"))?,
    );
    fs::copy(&setup_source, &setup)?;

    let runtime_before = vc_runtime_version();
    ctx.evidence.put("vcRuntimeBefore", json!(runtime_before));

    let install = run_setup(ctx, &setup, "/install", "/quiet", "setup-install.log")?;
    product_ensure!(
        matches!(install.code(), Some(0 | 3010)),
        "Setup /install exited {:?}",
        install.code()
    );
    let install_log = burn_log(&ctx.scenario_dir.join("setup-install.log"));
    ctx.evidence.put(
        "setupInstallExited",
        json!(install.code().map(|code| code.to_string())),
    );

    let exe = PathBuf::from(r"C:\Program Files\ExoSnap\exosnap.exe");
    let shortcut = PathBuf::from(std::env::var("ProgramData")?)
        .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk");
    product_ensure!(exe.is_file(), "Setup did not install exosnap.exe");
    product_ensure!(
        reg_value("installed")?.is_some(),
        "Setup did not write the product registry marker"
    );
    product_ensure!(
        shortcut.is_file(),
        "Setup did not create the Start Menu shortcut"
    );
    product_ensure!(
        !desktop_shortcut()?.exists(),
        "a silent Setup created a desktop shortcut without the option"
    );
    let rows = exosnap_arp_rows()?;
    let visible: Vec<&ArpRow> = rows.iter().filter(|row| !row.system_component).collect();
    let hidden = rows.iter().filter(|row| row.system_component).count();
    product_ensure!(
        visible.len() == 1,
        "expected one Installed Apps entry, found {}: {:?}",
        visible.len(),
        visible.iter().map(|r| &r.display_name).collect::<Vec<_>>()
    );
    product_ensure!(
        hidden >= 1,
        "the chained MSI is not hidden behind the bundle's Installed Apps entry"
    );
    product_ensure!(
        install_log.contains("VCRedistX64"),
        "the Setup log does not report the runtime detection"
    );
    let runtime_after_install = vc_runtime_version();
    product_ensure!(
        runtime_sufficient(&runtime_after_install) && runtime_after_install >= runtime_before,
        "the runtime was downgraded or left below the floor: {runtime_before:?} -> {runtime_after_install:?}"
    );
    ctx.evidence.put(
        "vcRuntimeAfterInstall",
        json!(runtime_after_install.clone()),
    );

    run_start(ctx, &exe, &version, &commit)?;
    run_start(ctx, &exe, &version, &commit)?;

    let config = PathBuf::from(std::env::var("LOCALAPPDATA")?).join("ExoSnap");
    let recordings = PathBuf::from(std::env::var("USERPROFILE")?)
        .join("Videos")
        .join("ExoSnap");
    fs::create_dir_all(&config)?;
    fs::write(config.join("settings.ini"), b"setup cycle probe")?;
    fs::create_dir_all(&recordings)?;
    let recording = recordings.join("setup-cycle.mkv");
    fs::write(&recording, b"recording bytes must survive an uninstall")?;
    let (recording_hash, _) = sha256_file(&recording)?;

    // Deleting the original file is the point: repair and uninstall must come
    // from Burn's cache, not from the user's copy.
    fs::remove_file(&setup)?;
    let cached = quoted_exe(
        &exosnap_arp_rows()?
            .into_iter()
            .find(|row| !row.system_component)
            .map(|row| row.uninstall_string)
            .ok_or_else(|| Stop::fail("the bundle has no Installed Apps entry"))?,
    )
    .ok_or_else(|| Stop::fail("the bundle UninstallString carries no executable"))?;
    product_ensure!(
        cached.is_file(),
        "Burn did not cache the bundle for maintenance: {}",
        cached.display()
    );
    product_ensure!(
        !cached.starts_with(&work),
        "the cached bundle is the deleted original"
    );

    let repair = run_setup(ctx, &cached, "/repair", "/quiet", "setup-repair.log")?;
    product_ensure!(
        matches!(repair.code(), Some(0 | 3010)),
        "Setup /repair exited {:?}; log: {}",
        repair.code(),
        ctx.scenario_dir.join("setup-repair.log").display()
    );
    product_ensure!(
        exe.is_file() && reg_value("installed")?.is_some(),
        "repair did not leave the product installed"
    );

    // The quiet uninstall below removes Burn's cached bundle, so keep a copy
    // for the passive phase, which exercises the UI level, not the cache path.
    let passive_bundle = work.join("setup-passive-copy.exe");
    fs::copy(&cached, &passive_bundle)?;

    let uninstall = run_setup(ctx, &cached, "/uninstall", "/quiet", "setup-uninstall.log")?;
    product_ensure!(
        matches!(uninstall.code(), Some(0 | 3010)),
        "Setup /uninstall exited {:?}; log: {}",
        uninstall.code(),
        ctx.scenario_dir.join("setup-uninstall.log").display()
    );
    product_ensure!(
        !exe.exists()
            && reg_value("installed")?.is_none()
            && !shortcut.exists()
            && !desktop_shortcut()?.exists(),
        "uninstall left application files, registry or shortcuts"
    );
    product_ensure!(
        exosnap_arp_rows()?.is_empty(),
        "uninstall left an ExoSnap Installed Apps entry"
    );
    product_ensure!(
        config.join("settings.ini").is_file(),
        "uninstall removed local user data without the explicit option"
    );
    product_ensure!(
        sha256_file(&recording)?.0 == recording_hash,
        "uninstall modified a recording"
    );
    product_ensure!(
        runtime_sufficient(&vc_runtime_version()),
        "uninstall removed the shared Visual C++ runtime"
    );

    // Passive mode runs the same chain with a progress UI but no prompts, and
    // must not create the desktop shortcut either.
    let passive_install = run_setup(
        ctx,
        &passive_bundle,
        "/install",
        "/passive",
        "setup-passive-install.log",
    )?;
    product_ensure!(
        matches!(passive_install.code(), Some(0 | 3010)),
        "Setup /passive /install exited {:?}",
        passive_install.code()
    );
    product_ensure!(
        exe.is_file() && reg_value("installed")?.is_some() && !desktop_shortcut()?.exists(),
        "a passive install did not leave the expected product state"
    );
    let passive_uninstall = run_setup(
        ctx,
        &passive_bundle,
        "/uninstall",
        "/passive",
        "setup-passive-uninstall.log",
    )?;
    product_ensure!(
        matches!(passive_uninstall.code(), Some(0 | 3010)),
        "Setup /passive /uninstall exited {:?}",
        passive_uninstall.code()
    );
    product_ensure!(
        !exe.exists() && reg_value("installed")?.is_none(),
        "a passive uninstall left the product installed"
    );

    // Both uninstalls above deliberately preserved this user's data, and this
    // scenario runs before the MSI scenarios that assert a clean machine.
    // Remove the probes it created so the next scenario starts clean.
    fs::remove_dir_all(&config).ok();
    fs::remove_file(&recording).ok();

    // Nothing is installed now, so the next scenario starts from the asserted
    // clean state.
    Ok(())
}

/// The installed VC++ x64 runtime version tuple, when the redist key exists.
fn vc_runtime_version() -> Option<(u64, u64, u64, u64)> {
    let out = crate::tools::run(
        Command::new("reg.exe").args([
            "query",
            r"HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64",
        ]),
        secs(10.0),
    )
    .ok()?;
    if !out.success() {
        return None;
    }
    parse_vc_runtime_version(&out.stdout)
}

/// Reads the `Major`/`Minor`/`Bld`/`Rbld` DWORDs out of a `reg query` listing.
/// `reg.exe` renders DWORDs as hex (`0xe`), not decimal.
fn parse_vc_runtime_version(output: &str) -> Option<(u64, u64, u64, u64)> {
    let number = |name: &str| -> Option<u64> {
        let raw = parse_reg_value(output, name)?;
        let raw = raw.trim();
        let (digits, radix) = match raw.strip_prefix("0x").or_else(|| raw.strip_prefix("0X")) {
            Some(digits) => (digits, 16),
            None => (raw, 10),
        };
        u64::from_str_radix(digits, radix).ok()
    };
    Some((
        number("Major")?,
        number("Minor")?,
        number("Bld")?,
        number("Rbld")?,
    ))
}

fn runtime_sufficient(version: &Option<(u64, u64, u64, u64)>) -> bool {
    version.is_some_and(|(major, minor, build, _)| (major, minor, build) >= (14, 44, 35211))
}

/// One interactive Setup run for the UI Automation helper.
struct UiaRun<'a> {
    setup: &'a Path,
    mode: &'a str,
    log_name: &'a str,
    desktop_shortcut: bool,
    remove_user_data: bool,
    click_launch: bool,
}

/// The interactive Setup is driven by UI Automation inside the machine under
/// test; this runs the helper and keeps its transcript and page screenshots
/// beside the Burn log.
fn run_uia(ctx: &mut Context, script: &Path, run: &UiaRun<'_>) -> Result<crate::tools::Output> {
    let log = ctx.scenario_dir.join(run.log_name);
    let screenshots = ctx.scenario_dir.join("screenshots");
    fs::create_dir_all(&screenshots)?;
    let mut command = Command::new("powershell.exe");
    command
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
        ])
        .arg(script)
        .arg("-Setup")
        .arg(run.setup)
        .arg("-Mode")
        .arg(run.mode)
        .arg("-Log")
        .arg(&log)
        .arg("-ExpectedExe")
        .arg(r"C:\Program Files\ExoSnap\exosnap.exe")
        .arg("-ScreenshotDir")
        .arg(&screenshots);
    if run.desktop_shortcut {
        command.arg("-DesktopShortcut");
    }
    if run.remove_user_data {
        command.arg("-RemoveUserData");
    }
    if run.click_launch {
        command.arg("-ClickLaunch");
    }
    let out = crate::tools::run(&mut command, secs(1800.0))?;
    ctx.keep(&log);
    for entry in fs::read_dir(&screenshots)? {
        let path = entry?.path();
        if path.is_file() {
            ctx.keep(&path);
        }
    }
    Ok(out)
}

fn setup_interactive(ctx: &mut Context) -> Step {
    let residue = super::clean::residue()?;
    infra_ensure!(
        residue.is_empty(),
        "the disposable machine is not clean: {}",
        residue.join("; ")
    );
    let setup = ctx.package(FileRole::Setup)?;
    let script = ctx.scenario_dir.join("setup_ui.ps1");
    fs::write(&script, include_str!("setup_ui.ps1"))?;

    let exe = PathBuf::from(r"C:\Program Files\ExoSnap\exosnap.exe");
    let shortcut = PathBuf::from(std::env::var("ProgramData")?)
        .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk");
    let config = PathBuf::from(std::env::var("LOCALAPPDATA")?).join("ExoSnap");
    let recordings = PathBuf::from(std::env::var("USERPROFILE")?)
        .join("Videos")
        .join("ExoSnap");
    let desktop = desktop_shortcut()?;

    // Interactive install with the desktop shortcut chosen and the success
    // page's Launch action exercised.
    let install = run_uia(
        ctx,
        &script,
        &UiaRun {
            setup: &setup,
            mode: "install",
            log_name: "setup-ui-install.log",
            desktop_shortcut: true,
            remove_user_data: false,
            click_launch: true,
        },
    )?;
    product_ensure!(
        install.code() == Some(0),
        "the interactive install failed: {}",
        install.stderr.trim()
    );
    product_ensure!(exe.is_file(), "the interactive install left no exosnap.exe");
    product_ensure!(
        reg_value("installed")?.is_some(),
        "the interactive install wrote no product marker"
    );
    product_ensure!(shortcut.is_file(), "no Start Menu shortcut after install");
    product_ensure!(
        desktop.is_file(),
        "the selected desktop shortcut was not created"
    );
    let visible = exosnap_arp_rows()?
        .into_iter()
        .filter(|row| !row.system_component)
        .count();
    product_ensure!(
        visible == 1,
        "expected one Installed Apps entry, found {visible}"
    );
    for name in [
        "install-1-install-page.png",
        "install-2-license.png",
        "install-3-progress.png",
        "install-4-success.png",
    ] {
        product_ensure!(
            ctx.scenario_dir.join("screenshots").join(name).is_file(),
            "the interactive install captured no {name}"
        );
    }

    fs::create_dir_all(&config)?;
    fs::write(config.join("settings.ini"), b"interactive probe")?;
    fs::create_dir_all(&recordings)?;
    let recording = recordings.join("setup-interactive.mkv");
    fs::write(&recording, b"recording bytes must survive an uninstall")?;
    let (recording_hash, _) = sha256_file(&recording)?;

    // Interactive uninstall with the explicit local-data removal selected.
    let uninstall = run_uia(
        ctx,
        &script,
        &UiaRun {
            setup: &setup,
            mode: "uninstall",
            log_name: "setup-ui-uninstall.log",
            desktop_shortcut: false,
            remove_user_data: true,
            click_launch: false,
        },
    )?;
    product_ensure!(
        uninstall.code() == Some(0),
        "the interactive uninstall failed: {}",
        uninstall.stderr.trim()
    );
    product_ensure!(
        !exe.exists() && reg_value("installed")?.is_none(),
        "the interactive uninstall left the product installed"
    );
    product_ensure!(
        !shortcut.exists() && !desktop.exists(),
        "the interactive uninstall left shortcuts behind"
    );
    product_ensure!(
        exosnap_arp_rows()?.is_empty(),
        "the interactive uninstall left an Installed Apps entry"
    );
    product_ensure!(
        !config.exists(),
        "the selected local-data removal did not remove {}",
        config.display()
    );
    product_ensure!(
        sha256_file(&recording)?.0 == recording_hash,
        "the local-data removal touched a recording"
    );
    for name in [
        "uninstall-1-modify.png",
        "uninstall-2-progress.png",
        "uninstall-3-success.png",
    ] {
        product_ensure!(
            ctx.scenario_dir.join("screenshots").join(name).is_file(),
            "the interactive uninstall captured no {name}"
        );
    }
    Ok(())
}

/// Reads one value from [`INSTALL_KEY`], the product's own registry state.
pub(crate) fn reg_value(name: &str) -> Result<Option<String>> {
    let out = crate::tools::run(
        Command::new("reg.exe").args(["query", INSTALL_KEY, "/v", name]),
        secs(10.0),
    )?;
    if !out.success() {
        return Ok(None);
    }
    Ok(parse_reg_value(&out.stdout, name))
}

/// Whether a registry key exists at all, regardless of its values.
pub(crate) fn reg_key_exists(key: &str) -> Result<bool> {
    Ok(crate::tools::run(Command::new("reg.exe").args(["query", key]), secs(10.0))?.success())
}

/// Parses one named value out of `reg.exe query`'s `name  REG_TYPE  value` line format.
pub(crate) fn parse_reg_value(output: &str, name: &str) -> Option<String> {
    output.lines().find_map(|line| {
        let words: Vec<&str> = line.split_whitespace().collect();
        let kind = words.iter().position(|word| word.starts_with("REG_"))?;
        if kind != 1 || words[0] != name {
            return None;
        }
        let (_, after_name) = line.split_once(name)?;
        let (_, value) = after_name.split_once(words[kind])?;
        Some(value.trim().to_string())
    })
}

/// Relative path (lowercase, forward slashes) to sha256 for every file under
/// `root`, or an empty map when `root` does not exist. Shared by every
/// scenario that has to say whether a directory tree changed.
pub(crate) fn tree_hashes(root: &Path) -> Result<BTreeMap<String, String>> {
    let mut files = BTreeMap::new();
    if !root.is_dir() {
        return Ok(files);
    }
    for path in walk_files(root)? {
        let relative = path
            .strip_prefix(root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        files.insert(relative.to_ascii_lowercase(), sha256_file(&path)?.0);
    }
    Ok(files)
}

fn msi(ctx: &mut Context, verb: &str, path: &Path, log_name: &str) -> Step {
    let log = ctx.scenario_dir.join(log_name);
    let out = crate::tools::run(
        Command::new("msiexec.exe")
            .arg(verb)
            .arg(path)
            .args(["/qn", "/norestart", "/l*v"])
            .arg(&log),
        secs(360.0),
    )?;
    ctx.keep(&log);
    product_ensure!(
        out.code() == Some(0),
        "msiexec {verb} exited {:?}; log: {}",
        out.code(),
        log.display()
    );
    Ok(())
}

/// The all-users desktop shortcut the Setup option may create.
fn desktop_shortcut() -> Result<PathBuf> {
    Ok(PathBuf::from(std::env::var("PUBLIC")?).join(r"Desktop\ExoSnap.lnk"))
}

fn run_start(ctx: &mut Context, exe: &Path, version: &str, commit: &str) -> Step {
    let run_id = control::new_run_id("install");
    let mut child = ctx.spawn(Command::new(exe).args(["--live-verify-control", &run_id]))?;
    let mut client = match Client::connect("LiveVerify", &run_id, secs(60.0)) {
        Ok(client) => client,
        Err(error) => {
            return Err(match child.try_wait()? {
                Some(status) => Stop::fail(format!(
                    "installed app exited before control handshake: {status}"
                )),
                None => Stop::Infra(error),
            });
        }
    };
    let identity = client.identity.clone();
    let (hash, _) = sha256_file(exe)?;
    product_ensure!(
        identity["productVersion"] == version,
        "installed app reports a different version: {}",
        identity["productVersion"]
    );
    product_ensure!(
        identity["commit"]
            .as_str()
            .is_some_and(|s| s.eq_ignore_ascii_case(commit)),
        "installed app reports a different commit: {}",
        identity["commit"]
    );
    product_ensure!(
        identity["executableSha256"]
            .as_str()
            .is_some_and(|s| s.eq_ignore_ascii_case(&hash)),
        "installed app reports a different executable hash"
    );
    let state = client.call("ui.getState", json!({}))?;
    product_ensure!(
        state["blockingSurface"]
            .as_str()
            .is_none_or(|s| s.is_empty() || s == "none"),
        "a clean start opened a blocking surface: {}",
        state["blockingSurface"]
    );
    let settings = client.call("settings.snapshot", json!({}))?;
    for field in [
        "requested",
        "effective",
        "app",
        "constraints",
        "persistence",
    ] {
        product_ensure!(
            settings[field].as_object().is_some_and(|v| !v.is_empty()),
            "settings.snapshot has no {field} section"
        );
    }
    product_ensure!(
        settings["differences"]
            .as_array()
            .is_some_and(Vec::is_empty),
        "defaults differ from effective settings: {}",
        settings["differences"]
    );
    ctx.evidence.put("installedIdentity", identity);
    drop(client);
    #[cfg(windows)]
    {
        product_ensure!(
            crate::win::close_windows(child.id()) > 0,
            "installed app has no window to close"
        );
    }
    let status = crate::tools::wait(&mut child, secs(30.0))
        .map_err(|_| Stop::fail("installed app did not close within 30 s"))?;
    product_ensure!(status.success(), "installed app closed with {status}");
    Ok(())
}

fn msi_cycle(ctx: &mut Context) -> Step {
    let bundle = ctx.bundle()?;
    let version = bundle.inventory.product_version.clone();
    let commit = bundle.inventory.source_commit.clone();
    let installer = ctx.package(FileRole::Installer)?;
    let portable = ctx.product()?.root;
    let config = PathBuf::from(std::env::var("LOCALAPPDATA")?).join("ExoSnap");
    let shortcut = PathBuf::from(std::env::var("ProgramData")?)
        .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk");
    let residue = super::clean::residue()?;
    ctx.evidence.put("residueBeforeInstall", json!(residue));
    infra_ensure!(
        residue.is_empty(),
        "the disposable machine is not clean: {}",
        residue.join("; ")
    );
    msi(ctx, "/i", &installer, "install.log")?;
    let installed = reg_value("InstallPath")?
        .ok_or_else(|| Stop::fail("MSI succeeded but InstallPath is absent"))?;
    let root = PathBuf::from(installed.trim_end_matches(['\\', '/']));
    product_ensure!(
        root.is_dir(),
        "InstallPath does not exist: {}",
        root.display()
    );
    let expected = tree_hashes(&portable)?;
    let actual = tree_hashes(&root)?;
    product_ensure!(
        actual == expected,
        "installed file hashes differ from the portable package"
    );
    product_ensure!(
        reg_value("installed")?.is_some(),
        "MSI did not write its installed registry marker"
    );
    product_ensure!(
        shortcut.is_file(),
        "MSI did not create the Start Menu shortcut"
    );
    product_ensure!(
        !desktop_shortcut()?.exists(),
        "the raw MSI created a desktop shortcut without EXOSNAP_DESKTOP_SHORTCUT=1"
    );
    product_ensure!(
        !reg_key_exists(r"HKLM\SOFTWARE\Codexo\ExoSnap")?,
        "a fresh install wrote the legacy product key"
    );
    let exe = root.join("exosnap.exe");
    run_start(ctx, &exe, &version, &commit)?;
    run_start(ctx, &exe, &version, &commit)?;
    let config_before = tree_hashes(&config)?;
    msi(ctx, "/x", &installer, "uninstall.log")?;
    product_ensure!(
        !exe.exists() && reg_value("InstallPath")?.is_none() && !shortcut.exists(),
        "uninstall left application files, registry or shortcut"
    );
    product_ensure!(
        tree_hashes(&config)? == config_before,
        "uninstall changed user configuration"
    );
    msi(ctx, "/i", &installer, "reinstall.log")?;
    product_ensure!(
        tree_hashes(&root)? == expected,
        "reinstall did not restore the candidate bytes"
    );
    ctx.evidence.put("installedFiles", expected.len());
    ctx.evidence
        .put("preservedConfigFiles", config_before.len());
    Ok(())
}

/// Uninstall with the explicit local-data flag must remove exactly the current
/// user's `%LOCALAPPDATA%\ExoSnap` and must not reach recordings that live
/// outside it. The MSI is then reinstalled so later disposable-machine gates
/// still find an installed product.
fn msi_user_data_removal(ctx: &mut Context) -> Step {
    let bundle = ctx.bundle()?;
    let version = bundle.inventory.product_version.clone();
    let commit = bundle.inventory.source_commit.clone();
    let installer = ctx.package(FileRole::Installer)?;
    let config = PathBuf::from(std::env::var("LOCALAPPDATA")?).join("ExoSnap");
    let recordings = PathBuf::from(std::env::var("USERPROFILE")?)
        .join("Videos")
        .join("ExoSnap");

    if reg_value("installed")?.is_none() {
        msi(ctx, "/i", &installer, "removal-install.log")?;
    }
    fs::create_dir_all(&config)?;
    fs::write(config.join("settings.ini"), b"removal scenario probe")?;
    fs::create_dir_all(&recordings)?;
    let recording = recordings.join("removal-scenario.mkv");
    fs::write(&recording, b"recording bytes must survive an uninstall")?;
    let (recording_hash, _) = sha256_file(&recording)?;
    let config_files = tree_hashes(&config)?.len();

    let log = ctx.scenario_dir.join("removal-uninstall.log");
    let out = crate::tools::run(
        Command::new("msiexec.exe")
            .arg("/x")
            .arg(&installer)
            .args(["/qn", "/norestart", "EXOSNAP_REMOVE_USER_DATA=1", "/l*v"])
            .arg(&log),
        secs(360.0),
    )?;
    ctx.keep(&log);
    product_ensure!(
        out.code() == Some(0),
        "msiexec /x with EXOSNAP_REMOVE_USER_DATA=1 exited {:?}; log: {}",
        out.code(),
        log.display()
    );
    product_ensure!(
        !config.exists(),
        "the explicit uninstall left {} in place",
        config.display()
    );
    product_ensure!(
        sha256_file(&recording)?.0 == recording_hash,
        "the explicit uninstall modified a recording outside the local data folder"
    );

    msi(ctx, "/i", &installer, "removal-restore.log")?;
    let exe = PathBuf::from(r"C:\Program Files\ExoSnap\exosnap.exe");
    product_ensure!(
        exe.is_file() && reg_value("installed")?.is_some(),
        "the product was not restored after the removal scenario"
    );
    run_start(ctx, &exe, &version, &commit)?;
    ctx.evidence.put("removedConfigFiles", config_files);
    ctx.evidence.put("recordingSha256", recording_hash);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn install_scenario_is_restricted_to_disposable_machine() {
        let scenario = &scenarios()[0];
        assert_eq!(scenario.lane, Lane::CiInstall);
        assert!(scenario.requires.contains(&Capability::DisposableOs));
        assert!(!scenario.also.contains(&Lane::Quick));
    }

    #[test]
    fn registry_value_parser_handles_paths_and_dword_markers() {
        let output = "\n    InstallPath    REG_SZ    C:\\Program Files\\ExoSnap\\\n    installed    REG_DWORD    0x1\n";
        assert_eq!(
            parse_reg_value(output, "InstallPath"),
            Some(r"C:\Program Files\ExoSnap\".into())
        );
        assert_eq!(parse_reg_value(output, "installed"), Some("0x1".into()));
    }

    #[test]
    fn vc_runtime_dwords_are_read_as_the_hex_reg_renders() {
        let output = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64\n    Version    REG_SZ    v14.51.36247.00\n    Installed    REG_DWORD    0x1\n    Major    REG_DWORD    0xe\n    Minor    REG_DWORD    0x33\n    Bld    REG_DWORD    0x8d97\n    Rbld    REG_DWORD    0x0\n";
        assert_eq!(parse_vc_runtime_version(output), Some((14, 51, 36247, 0)));
        assert_eq!(parse_vc_runtime_version("no values here\n"), None);
    }
}
