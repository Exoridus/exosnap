//! MSI installation on a disposable Windows machine.

use serde_json::json;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

use super::common::secs;
use crate::bundle::{FileRole, sha256_file};
use crate::capability::Capability;
use crate::context::Context;
use crate::control::{self, Client};
use crate::package::walk_files;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

const INSTALL_KEY: &str = r"HKLM\SOFTWARE\Codexo\ExoSnap";
const USER_KEY: &str = r"HKCU\SOFTWARE\Codexo\ExoSnap";

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "install.msi-cycle",
        revision: 1,
        title: "The MSI installs, starts, uninstalls and reinstalls cleanly",
        claim: "on a clean disposable Windows machine, the MSI installs the portable bytes, first and second starts are healthy, uninstall preserves user settings, and reinstall restores the product",
        lane: Lane::CiInstall,
        also: &[],
        tier: Tier::Required,
        requires: &[
            Capability::Windows,
            Capability::Admin,
            Capability::InteractiveDesktop,
            Capability::DisposableOs,
        ],
        timeout: secs(900.0),
        run: msi_cycle,
    }]
}

fn reg_value(name: &str) -> Step<Option<String>> {
    let out = crate::tools::run(
        Command::new("reg.exe").args(["query", INSTALL_KEY, "/v", name]),
        secs(10.0),
    )?;
    if !out.success() {
        return Ok(None);
    }
    Ok(parse_reg_value(&out.stdout, name))
}

fn reg_key_exists(key: &str) -> Step<bool> {
    Ok(crate::tools::run(Command::new("reg.exe").args(["query", key]), secs(10.0))?.success())
}

fn parse_reg_value(output: &str, name: &str) -> Option<String> {
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

fn tree_hashes(root: &Path) -> Step<BTreeMap<String, String>> {
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
    let program_data = PathBuf::from(std::env::var("ProgramData")?).join("ExoSnap");
    let default_install = PathBuf::from(std::env::var("ProgramFiles")?).join(r"Codexo\ExoSnap");
    let shortcut = PathBuf::from(std::env::var("ProgramData")?)
        .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk");
    infra_ensure!(
        reg_value("InstallPath")?.is_none()
            && reg_value("installed")?.is_none()
            && !default_install.exists()
            && !shortcut.exists()
            && !config.exists()
            && !program_data.exists()
            && !reg_key_exists(USER_KEY)?,
        "the disposable machine is not clean: installation or ExoSnap state already exists"
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
        let output = "\n    InstallPath    REG_SZ    C:\\Program Files\\Codexo\\ExoSnap\\\n    installed    REG_DWORD    0x1\n";
        assert_eq!(
            parse_reg_value(output, "InstallPath"),
            Some(r"C:\Program Files\Codexo\ExoSnap\".into())
        );
        assert_eq!(parse_reg_value(output, "installed"), Some("0x1".into()));
    }
}
