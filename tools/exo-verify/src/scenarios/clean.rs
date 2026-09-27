//! Whether a machine holds any ExoSnap state. A capability check with no
//! product claim: the install lane and the disposable-guest seal share this one
//! definition of a clean machine.

use anyhow::{Context as _, Result};
use serde_json::json;
use std::path::PathBuf;
use std::process::Command;

use super::common::secs;
use super::install::{INSTALL_KEY, USER_KEY, parse_reg_value, reg_key_exists, reg_value};
use crate::capability::Capability;
use crate::context::Context;
use crate::infra_ensure;
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step};

const UNINSTALL_KEY: &str = r"HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall";

pub fn scenarios() -> Vec<Scenario> {
    vec![Scenario {
        id: "environment.clean-machine",
        revision: 1,
        title: "The machine holds no ExoSnap installation or state",
        class: ScenarioClass::Capability,
        contract: "no uninstall entry, product registry value, install directory, Start Menu shortcut, per-user or machine-wide ExoSnap state exists, so a first start on this machine is a real first start",
        lane: Lane::CiInstall,
        also: &[],
        tier: Tier::Required,
        requires: &[Capability::Windows, Capability::DisposableOs],
        timeout: secs(60.0),
        run: clean_machine,
    }]
}

/// The file-system roots ExoSnap state lives under.
pub struct Roots {
    pub local_app_data: PathBuf,
    pub program_data: PathBuf,
    pub program_files: PathBuf,
}

impl Roots {
    pub fn from_env() -> Result<Roots> {
        let var = |name: &str| {
            std::env::var_os(name)
                .map(PathBuf::from)
                .with_context(|| format!("{name} is not set"))
        };
        Ok(Roots {
            local_app_data: var("LOCALAPPDATA")?,
            program_data: var("ProgramData")?,
            program_files: var("ProgramFiles")?,
        })
    }
}

/// Every file-system location whose existence means ExoSnap state is present.
/// Recovery and update state are named separately although they live in the
/// configuration directory: they change what a first start looks like, so a
/// report has to say which one it found.
pub fn residue_paths(roots: &Roots) -> Vec<(&'static str, PathBuf)> {
    let config = roots.local_app_data.join("ExoSnap");
    vec![
        ("a per-user configuration directory", config.clone()),
        (
            "recovery state from an earlier recording",
            config.join("recovery"),
        ),
        (
            "update state from an earlier install",
            config.join("update"),
        ),
        (
            "machine-wide ExoSnap data",
            roots.program_data.join("ExoSnap"),
        ),
        (
            "the default install directory",
            roots.program_files.join(r"Codexo\ExoSnap"),
        ),
        (
            "the Start Menu shortcut",
            roots
                .program_data
                .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk"),
        ),
    ]
}

/// Uninstall keys whose `DisplayName` is exactly `ExoSnap`, from the output of
/// `reg.exe query <Uninstall> /s /v DisplayName`.
fn exosnap_uninstall_keys(output: &str) -> Vec<String> {
    let mut keys = Vec::new();
    let mut current: Option<&str> = None;
    for line in output.lines() {
        if line.starts_with("HKEY_") {
            current = Some(line.trim());
        } else if parse_reg_value(line, "DisplayName").as_deref() == Some("ExoSnap")
            && let Some(key) = current
        {
            keys.push(key.to_string());
        }
    }
    keys
}

fn installed_products() -> Result<Vec<String>> {
    let search = crate::tools::run(
        Command::new("reg.exe").args(["query", UNINSTALL_KEY, "/s", "/v", "DisplayName"]),
        secs(30.0),
    )?;
    // reg.exe exits non-zero when nothing matches.
    if !search.success() {
        return Ok(Vec::new());
    }
    let mut products = Vec::new();
    for key in exosnap_uninstall_keys(&search.stdout) {
        let version = crate::tools::run(
            Command::new("reg.exe").args(["query", &key, "/v", "DisplayVersion"]),
            secs(10.0),
        )?;
        let version = version
            .success()
            .then(|| parse_reg_value(&version.stdout, "DisplayVersion"))
            .flatten()
            .unwrap_or_else(|| "version not recorded".into());
        products.push(format!("an installed product (ExoSnap {version}, {key})"));
    }
    Ok(products)
}

/// Everything of an earlier ExoSnap still on this machine, one finding per
/// entry. Machine-wide state counts as much as the current user's: a first
/// start on a machine that already holds ExoSnap's ProgramData is not a first
/// start. An empty list means the machine is clean.
pub fn residue() -> Result<Vec<String>> {
    let mut found = installed_products()?;
    for (what, path) in residue_paths(&Roots::from_env()?) {
        if path.exists() {
            found.push(format!("{what} ({})", path.display()));
        }
    }
    for value in ["InstallPath", "installed"] {
        if reg_value(value)?.is_some() {
            found.push(format!("the product registry value {INSTALL_KEY}\\{value}"));
        }
    }
    if reg_key_exists(USER_KEY)? {
        found.push(format!("a per-user registry key ({USER_KEY})"));
    }
    Ok(found)
}

fn clean_machine(ctx: &mut Context) -> Step {
    let found = residue()?;
    ctx.evidence.put("residue", json!(found));
    infra_ensure!(
        found.is_empty(),
        "the machine is not clean: {}",
        found.join("; ")
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn residue_covers_user_machine_and_install_locations() {
        let roots = Roots {
            local_app_data: PathBuf::from(r"C:\Users\u\AppData\Local"),
            program_data: PathBuf::from(r"C:\ProgramData"),
            program_files: PathBuf::from(r"C:\Program Files"),
        };
        let paths: Vec<PathBuf> = residue_paths(&roots).into_iter().map(|(_, p)| p).collect();
        for expected in [
            r"C:\Users\u\AppData\Local\ExoSnap",
            r"C:\Users\u\AppData\Local\ExoSnap\recovery",
            r"C:\Users\u\AppData\Local\ExoSnap\update",
            r"C:\ProgramData\ExoSnap",
            r"C:\Program Files\Codexo\ExoSnap",
            r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk",
        ] {
            assert!(
                paths.contains(&PathBuf::from(expected)),
                "{expected} is not checked"
            );
        }
    }

    #[test]
    fn only_an_exact_exosnap_display_name_is_an_installed_product() {
        let output = "\r\nHKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{A}\r\n    DisplayName    REG_SZ    ExoSnap\r\n\r\nHKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{B}\r\n    DisplayName    REG_SZ    ExoSnap Helper\r\n\r\nHKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{C}\r\n    DisplayName    REG_SZ    Other\r\n\r\nEnd of search: 3 match(es) found.\r\n";
        assert_eq!(
            exosnap_uninstall_keys(output),
            vec![
                r"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{A}"
                    .to_string()
            ]
        );
        assert!(exosnap_uninstall_keys("").is_empty());
    }
}
