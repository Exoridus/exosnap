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
}
