//! The Chocolatey package rehearsal: pack, install, uninstall, restore.
//!
//! Chocolatey installs machine-wide and every step here ends in msiexec, so
//! this runs in-process inside a disposable guest that is already elevated
//! (the scenario that calls [`run_rehearsal`] requires [`Capability::Admin`]).
//!
//! Nothing under `packaging/chocolatey` is modified. The package is copied to
//! a scratch directory and only the copy is pointed at the local candidate
//! MSI, because the tracked checksum describes a file that does not exist
//! until the release is published.
//!
//! What is put back and what is not: the candidate MSI is reinstalled once
//! the rehearsal is done, whatever happened before that, because later gates
//! in the same campaign expect ExoSnap installed. The Visual C++
//! redistributable is not put back: `vcredist140` is a declared Chocolatey
//! dependency of the package, so installing it can upgrade the machine's
//! copy, and downgrading that to undo the change would be worse than the
//! change itself. Its version is recorded before and after so the verdict
//! can say what happened.
//!
//! [`Capability::Admin`]: crate::capability::Capability::Admin

use anyhow::{Context as _, Result};
use regex::Regex;
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use super::common::secs;
use super::install::{self, INSTALL_KEY};
use super::update::copy_tree;
use crate::bundle::sha256_file;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum StepKind {
    /// What the rehearsal asserts about the Chocolatey package.
    Product,
    /// Packing, resolving a source, putting the machine back. A failure here
    /// measured nothing about the package.
    Bootstrap,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StepResult {
    pub name: String,
    pub ok: bool,
    pub detail: String,
    pub kind: StepKind,
    pub assertions: Vec<String>,
    pub failed_assertions: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RehearsalResult {
    pub ok: bool,
    pub msi_path: String,
    pub msi_sha256: String,
    pub restore_ran: bool,
    pub restore_exit_code: Option<i32>,
    pub vcredist_before: String,
    pub vcredist_after: String,
    pub observations: Vec<String>,
    pub completed_utc: String,
    pub steps: Vec<StepResult>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub fatal: Option<String>,
}

/// Builds one step: records assertions, and whether any of them failed.
struct StepBuilder {
    name: String,
    kind: StepKind,
    ok: bool,
    detail: String,
    assertions: Vec<String>,
    failed_assertions: Vec<String>,
}

impl StepBuilder {
    fn new(name: impl Into<String>, kind: StepKind) -> Self {
        Self {
            name: name.into(),
            kind,
            ok: true,
            detail: String::new(),
            assertions: Vec::new(),
            failed_assertions: Vec::new(),
        }
    }

    fn detail(mut self, detail: impl Into<String>) -> Self {
        self.detail = detail.into();
        self
    }

    /// Records one assertion, passing or failing. Both outcomes matter: a
    /// step that passed three assertions and one that ran none look
    /// identical if only failures are kept.
    fn assert(&mut self, text: impl Into<String>, condition: bool) {
        let text = text.into();
        self.assertions.push(text.clone());
        if !condition {
            self.ok = false;
            self.failed_assertions.push(text);
        }
    }

    fn finish(self) -> StepResult {
        StepResult {
            name: self.name,
            ok: self.ok,
            detail: self.detail,
            kind: self.kind,
            assertions: self.assertions,
            failed_assertions: self.failed_assertions,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArpEntry {
    pub product_code: String,
    pub display_name: String,
    pub publisher: String,
    pub display_version: String,
    pub install_location: String,
}

/// One candidate row read from an Uninstall registry key, before ambiguity
/// between several ExoSnap entries is ruled out.
#[derive(Debug, Clone, PartialEq, Eq)]
struct ArpCandidate {
    product_code: String,
    display_name: String,
    publisher: String,
    display_version: String,
    install_location: String,
}

/// Picks the one ExoSnap Add/Remove Programs entry out of already
/// name-and-publisher-filtered candidates, refusing to guess between two.
///
/// `DisplayName` starting with `ExoSnap` is not by itself an identification;
/// it also matches an unrelated product named similarly. Two matches abort
/// the caller before any uninstall runs: picking one and removing it is how
/// an unrelated product disappears from a machine.
fn resolve_arp_entry(candidates: Vec<ArpCandidate>) -> Result<Option<ArpEntry>, String> {
    match candidates.len() {
        0 => Ok(None),
        1 => {
            let c = candidates.into_iter().next().unwrap();
            Ok(Some(ArpEntry {
                product_code: c.product_code,
                display_name: c.display_name,
                publisher: c.publisher,
                display_version: c.display_version,
                install_location: c.install_location,
            }))
        }
        n => Err(format!(
            "{n} ExoSnap products are registered ({}); refusing to guess which one to remove",
            candidates
                .iter()
                .map(|c| format!(
                    "{} {} {}",
                    c.display_name, c.display_version, c.product_code
                ))
                .collect::<Vec<_>>()
                .join(", ")
        )),
    }
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

const UNINSTALL_ROOTS: &[&str] = &[
    r"HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
    r"HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
];

/// The Add/Remove Programs entry for ExoSnap, or `None`. Both registry views
/// are searched, because a lookup that only knows the view it expects reports
/// a clean uninstall for an entry it never looked at.
fn arp_entry() -> Result<Option<ArpEntry>> {
    let mut candidates = Vec::new();
    for root in UNINSTALL_ROOTS {
        let listing = crate::tools::run(Command::new("reg.exe").args(["query", root]), secs(10.0))?;
        if !listing.success() {
            continue;
        }
        for line in listing.stdout.lines() {
            let line = line.trim();
            let Some(name) = line.rsplit('\\').next() else {
                continue;
            };
            if !is_product_code(name) {
                continue;
            }
            let subkey = format!("{root}\\{name}");
            let properties =
                crate::tools::run(Command::new("reg.exe").args(["query", &subkey]), secs(10.0))?;
            if !properties.success() {
                continue;
            }
            let display_name =
                install::parse_reg_value(&properties.stdout, "DisplayName").unwrap_or_default();
            let publisher =
                install::parse_reg_value(&properties.stdout, "Publisher").unwrap_or_default();
            if !display_name.starts_with("ExoSnap") || publisher != "Codexo" {
                continue;
            }
            candidates.push(ArpCandidate {
                product_code: name.to_string(),
                display_name,
                publisher,
                display_version: install::parse_reg_value(&properties.stdout, "DisplayVersion")
                    .unwrap_or_default(),
                install_location: install::parse_reg_value(&properties.stdout, "InstallLocation")
                    .unwrap_or_default(),
            });
        }
    }
    resolve_arp_entry(candidates).map_err(anyhow::Error::msg)
}

/// The differences between two directory-hash manifests, as readable lines. A
/// manifest says WHICH file changed, not only that something did.
fn describe_manifest_diff(
    before: &BTreeMap<String, String>,
    after: &BTreeMap<String, String>,
) -> Vec<String> {
    let mut differences = Vec::new();
    for (name, hash) in before {
        match after.get(name) {
            None => differences.push(format!("removed: {name}")),
            Some(after_hash) if after_hash != hash => differences.push(format!("changed: {name}")),
            _ => {}
        }
    }
    for name in after.keys() {
        if !before.contains_key(name) {
            differences.push(format!("added: {name}"));
        }
    }
    differences
}

/// The installed version of one Chocolatey package, or empty when it has none.
fn chocolatey_package_version(id: &str) -> String {
    let Ok(listed) = crate::tools::run(
        Command::new("choco").args(["list", "--exact", id, "--limit-output"]),
        secs(30.0),
    ) else {
        return String::new();
    };
    for line in listed.stdout.lines() {
        if let Some((name, version)) = line.split_once('|')
            && name == id
        {
            return version.to_string();
        }
    }
    String::new()
}

/// Runs one external command to completion and writes its argv and captured
/// output into `evidence_dir/log_name`. Returns its exit code.
fn run_logged(
    evidence_dir: &Path,
    log_name: &str,
    mut command: Command,
    timeout: Duration,
) -> Result<Option<i32>> {
    let description = format!("{command:?}");
    let out = crate::tools::run(&mut command, timeout)?;
    std::fs::write(
        evidence_dir.join(log_name),
        format!("{description}\n{}\n{}", out.stdout, out.stderr),
    )?;
    Ok(out.code())
}

enum CopyOutcome {
    Ready {
        package_directory: PathBuf,
        install_script: PathBuf,
    },
    Rejected(String),
}

/// Copies `packaging/chocolatey` and points the copy at a local MSI.
/// `Install-ChocolateyPackage` accepts a local path in `url64bit`, so a
/// rehearsal can install the package without the release being published,
/// which is the point: the checksum in the tracked file describes an MSI
/// that does not exist until the release is built.
fn stage_package_copy(
    source_directory: &Path,
    destination_directory: &Path,
    msi_path: &Path,
    sha256: &str,
) -> Result<CopyOutcome> {
    copy_tree(source_directory, destination_directory)
        .map_err(|stop| anyhow::anyhow!("could not copy the Chocolatey package: {stop:?}"))?;
    let install_script = destination_directory.join("tools/chocolateyinstall.ps1");
    if !install_script.is_file() {
        return Ok(CopyOutcome::Rejected(
            "the package copy carries no tools/chocolateyinstall.ps1".to_string(),
        ));
    }
    let text = std::fs::read_to_string(&install_script)
        .with_context(|| format!("read {}", install_script.display()))?;
    let url_pattern = Regex::new(r"(?m)^(\s*url64bit\s*=\s*)'[^']*'").unwrap();
    let checksum_pattern = Regex::new(r"(?m)^(\s*checksum64\s*=\s*)'[^']*'").unwrap();
    let url_matches = url_pattern.find_iter(&text).count();
    let checksum_matches = checksum_pattern.find_iter(&text).count();
    if url_matches != 1 || checksum_matches != 1 {
        return Ok(CopyOutcome::Rejected(format!(
            "chocolateyinstall.ps1 carries {url_matches} url64bit and {checksum_matches} checksum64 assignment(s); exactly one of each is required"
        )));
    }
    // The escaped MSI path is substituted for the quoted literal only; the
    // surrounding indentation and assignment shape are the file's own and
    // not this function's to decide.
    let escaped_msi_path = msi_path.display().to_string().replace('\'', "''");
    let text = url_pattern
        .replace(&text, |caps: &regex::Captures| {
            format!("{}'{escaped_msi_path}'", &caps[1])
        })
        .into_owned();
    let text = checksum_pattern
        .replace(&text, |caps: &regex::Captures| {
            format!("{}'{sha256}'", &caps[1])
        })
        .into_owned();
    std::fs::write(&install_script, text)
        .with_context(|| format!("write {}", install_script.display()))?;
    Ok(CopyOutcome::Ready {
        package_directory: destination_directory.to_path_buf(),
        install_script,
    })
}

/// Whether a directory holds at least one file, by the same tree-hash
/// definition [`install::msi_cycle`] uses to detect a clean or changed
/// install tree.
fn has_files(path: &Path) -> Result<bool> {
    Ok(!install::tree_hashes(path)?.is_empty())
}

/// Makes sure `user_config` exists and is populated before the rehearsal
/// measures whether uninstalling the package leaves it alone: a directory
/// that starts empty makes "unchanged" mean nothing.
fn ensure_user_config_populated(user_config: &Path) -> Result<()> {
    if !has_files(user_config)?
        && let Some(install_path) = install::reg_value("InstallPath")?
    {
        let exe = PathBuf::from(install_path.trim_end_matches(['\\', '/'])).join("exosnap.exe");
        if exe.is_file() {
            let mut command = Command::new(&exe);
            command.arg("--smoke-test");
            let _ = crate::tools::run(&mut command, secs(60.0));
        }
    }
    if !user_config.exists() {
        std::fs::create_dir_all(user_config)?;
    }
    // A fresh profile's short smoke start can exit before the application has
    // written anything. The rehearsal's subject is that uninstall leaves user
    // configuration alone, so there must be configuration to leave alone.
    if !has_files(user_config)? {
        std::fs::write(
            user_config.join("settings.ini"),
            b"[chocolatey-rehearsal]\nprobe = must survive an uninstall\n",
        )?;
    }
    Ok(())
}

const CHOCO_SOURCE_SUFFIX: &str = ";https://community.chocolatey.org/api/v2/";

struct Rehearsal<'a> {
    package_source: &'a Path,
    msi_path: &'a Path,
    msi_sha256: &'a str,
    evidence_dir: &'a Path,
    work_dir: PathBuf,
    steps: Vec<StepResult>,
    observations: Vec<String>,
    machine_touched: bool,
}

impl<'a> Rehearsal<'a> {
    fn push(&mut self, step: StepResult) -> bool {
        let ok = step.ok;
        self.steps.push(step);
        ok
    }

    fn installed_exe_path() -> PathBuf {
        PathBuf::from(r"C:\Program Files\ExoSnap\exosnap.exe")
    }

    fn install_directory() -> PathBuf {
        PathBuf::from(r"C:\Program Files\ExoSnap")
    }

    fn shortcut_path() -> Result<PathBuf> {
        Ok(PathBuf::from(std::env::var("ProgramData")?)
            .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk"))
    }

    fn choco_library() -> PathBuf {
        let root = std::env::var("ChocolateyInstall")
            .unwrap_or_else(|_| r"C:\ProgramData\chocolatey".to_string());
        PathBuf::from(root).join("lib/exosnap")
    }

    /// Ensures the guest has ExoSnap installed via the candidate MSI, and a
    /// populated user configuration directory, before the package rehearsal
    /// exercises its own install/uninstall cycle.
    fn bootstrap(&mut self, user_config: &Path) -> Result<StepResult> {
        let mut step = StepBuilder::new("bootstrap", StepKind::Bootstrap);
        if install::reg_value("installed")?.is_none() {
            self.machine_touched = true;
            let mut command = Command::new("msiexec.exe");
            command
                .arg("/i")
                .arg(self.msi_path)
                .args(["/qn", "/norestart", "/l*v"])
                .arg(self.evidence_dir.join("msiexec-bootstrap-verbose.log"));
            let code = run_logged(
                self.evidence_dir,
                "msiexec-bootstrap.log",
                command,
                secs(360.0),
            )?;
            step.assert(
                format!("msiexec /i exits 0 (was {code:?})"),
                code == Some(0),
            );
        } else {
            step = step.detail("ExoSnap was already installed");
        }
        ensure_user_config_populated(user_config)?;
        Ok(step.finish())
    }

    fn prepare(&mut self, user_config: &Path) -> Result<(StepResult, Option<PathBuf>)> {
        let mut step = StepBuilder::new("prepare", StepKind::Bootstrap);
        let config_before = install::tree_hashes(user_config)?;
        step.assert(
            format!("{} exists and is not empty", user_config.display()),
            !config_before.is_empty(),
        );
        let tracked_install = self.package_source.join("tools/chocolateyinstall.ps1");
        let tracked_before = sha256_file(&tracked_install)?.0;
        let package_directory = self.work_dir.join("package");
        let outcome = stage_package_copy(
            self.package_source,
            &package_directory,
            self.msi_path,
            self.msi_sha256,
        )?;
        let ready_directory = match &outcome {
            CopyOutcome::Ready {
                package_directory,
                install_script,
            } => {
                step.assert("the package copy could be rewritten", true);
                let rewritten = std::fs::read_to_string(install_script)?;
                step.assert(
                    "url64bit points at the local MSI",
                    rewritten.contains(&self.msi_path.display().to_string()),
                );
                step.assert(
                    "checksum64 is the local MSI sha256",
                    rewritten.contains(self.msi_sha256),
                );
                Some(package_directory.clone())
            }
            CopyOutcome::Rejected(detail) => {
                step.assert("the package copy could be rewritten", false);
                step = step.detail(detail.clone());
                None
            }
        };
        let tracked_after = sha256_file(&tracked_install)?.0;
        step.assert(
            "the tracked chocolateyinstall.ps1 was not modified",
            tracked_after == tracked_before,
        );
        Ok((step.finish(), ready_directory))
    }

    fn pack(&mut self, package_directory: Option<&Path>) -> Result<StepResult> {
        let mut step = StepBuilder::new("pack", StepKind::Bootstrap);
        let Some(package_directory) = package_directory else {
            step.assert("a package copy from prepare exists to pack", false);
            return Ok(step.finish());
        };
        let nuspec = package_directory.join("exosnap.nuspec");
        let mut command = Command::new("choco");
        command
            .arg("pack")
            .arg(&nuspec)
            .arg("--out")
            .arg(&self.work_dir);
        let code = run_logged(self.evidence_dir, "choco-pack.log", command, secs(300.0))?;
        step.assert(
            format!("choco pack exits 0 (was {code:?})"),
            code == Some(0),
        );
        let packages: Vec<PathBuf> = std::fs::read_dir(&self.work_dir)?
            .filter_map(|entry| entry.ok())
            .map(|entry| entry.path())
            .filter(|path| path.extension().is_some_and(|ext| ext == "nupkg"))
            .collect();
        step.assert(
            format!("exactly one .nupkg was produced (was {})", packages.len()),
            packages.len() == 1,
        );
        if let [package] = packages.as_slice()
            && let Some(name) = package.file_name()
        {
            step = step.detail(name.to_string_lossy().into_owned());
        }
        Ok(step.finish())
    }

    fn remove_existing(&mut self) -> Result<StepResult> {
        let mut step = StepBuilder::new("removeExisting", StepKind::Bootstrap);
        match arp_entry()? {
            None => step = step.detail("no ExoSnap was installed"),
            Some(existing) => {
                step = step.detail(format!(
                    "{} {} ({})",
                    existing.display_name, existing.display_version, existing.product_code
                ));
                self.machine_touched = true;
                let mut command = Command::new("msiexec.exe");
                command
                    .arg("/x")
                    .arg(&existing.product_code)
                    .args(["/qn", "/norestart", "/l*v"])
                    .arg(self.evidence_dir.join("msiexec-remove-verbose.log"));
                let code = run_logged(
                    self.evidence_dir,
                    "msiexec-remove.log",
                    command,
                    secs(360.0),
                )?;
                step.assert(
                    format!("msiexec /x exits 0 (was {code:?})"),
                    code == Some(0),
                );
                step.assert(
                    "the ARP entry is gone before the package install",
                    arp_entry()?.is_none(),
                );
            }
        }
        Ok(step.finish())
    }

    fn install(&mut self) -> Result<StepResult> {
        let mut step = StepBuilder::new("install", StepKind::Product);
        self.machine_touched = true;
        let source = format!("{}{CHOCO_SOURCE_SUFFIX}", self.work_dir.display());
        let mut command = Command::new("choco");
        command.args([
            "install",
            "exosnap",
            "--source",
            &source,
            "-y",
            "--no-progress",
        ]);
        let code = run_logged(self.evidence_dir, "choco-install.log", command, secs(600.0))?;
        step.assert(
            format!("choco install exits 0 (was {code:?})"),
            code == Some(0),
        );
        let installed_exe = Self::installed_exe_path();
        step.assert(
            format!("{} exists", installed_exe.display()),
            installed_exe.is_file(),
        );
        let installed = arp_entry()?;
        step.assert("an ExoSnap ARP entry exists", installed.is_some());
        if let Some(installed) = &installed {
            step = step.detail(format!(
                "{} {}",
                installed.display_name, installed.display_version
            ));
        }
        step.assert(
            format!("{INSTALL_KEY} reports an installed marker"),
            install::reg_value("installed")?.is_some(),
        );
        step.assert(
            format!("{INSTALL_KEY} carries an InstallPath"),
            install::reg_value("InstallPath")?.is_some_and(|value| !value.trim().is_empty()),
        );
        step.assert(
            "the start-menu shortcut exists",
            Self::shortcut_path()?.is_file(),
        );
        step.assert(
            "the Chocolatey lib directory exists",
            Self::choco_library().is_dir(),
        );
        Ok(step.finish())
    }

    fn uninstall(
        &mut self,
        user_config: &Path,
        config_before: &BTreeMap<String, String>,
    ) -> Result<StepResult> {
        let mut step = StepBuilder::new("uninstall", StepKind::Product);
        let mut command = Command::new("choco");
        command.args(["uninstall", "exosnap", "-y"]);
        let code = run_logged(
            self.evidence_dir,
            "choco-uninstall.log",
            command,
            secs(300.0),
        )?;
        step.assert(
            format!("choco uninstall exits 0 (was {code:?})"),
            code == Some(0),
        );
        let installed_exe = Self::installed_exe_path();
        step.assert(
            format!("{} is gone", installed_exe.display()),
            !installed_exe.exists(),
        );
        let install_directory = Self::install_directory();
        step.assert(
            format!("{} is gone", install_directory.display()),
            !install_directory.exists(),
        );
        step.assert("the ARP entry is gone", arp_entry()?.is_none());
        let shortcut = Self::shortcut_path()?;
        step.assert("the start-menu shortcut is gone", !shortcut.exists());
        step.assert(
            format!("{INSTALL_KEY} is gone"),
            !install::reg_key_exists(INSTALL_KEY)?,
        );
        step.assert(
            "the Chocolatey lib directory is gone",
            !Self::choco_library().exists(),
        );
        // The empty manufacturer folder and its registry key are recorded,
        // never required: WiX generates no RemoveFolder row for the
        // manufacturer folder, since it owns no component, so an uninstall
        // that leaves the empty parent behind is within what the package
        // promises even though this machine was measured clean.
        let vendor_directory = PathBuf::from(r"C:\Program Files\Codexo");
        let vendor_key = r"HKLM\SOFTWARE\Codexo";
        self.observations.push(format!(
            "{} after uninstall: {}",
            vendor_directory.display(),
            if vendor_directory.exists() {
                "still present (empty parent, not owned by the package)"
            } else {
                "gone"
            }
        ));
        self.observations.push(format!(
            "{vendor_key} after uninstall: {}",
            if install::reg_key_exists(vendor_key)? {
                "still present (empty parent key)"
            } else {
                "gone"
            }
        ));
        let config_after = install::tree_hashes(user_config)?;
        let differences = describe_manifest_diff(config_before, &config_after);
        let summary = if differences.is_empty() {
            "unchanged".to_string()
        } else {
            differences.join("; ")
        };
        step.assert(
            format!("{} is untouched ({summary})", user_config.display()),
            differences.is_empty(),
        );
        Ok(step.finish())
    }

    /// Reinstalls the candidate MSI so later gates in the same campaign find
    /// ExoSnap installed, whatever happened above. Runs unconditionally
    /// (reported, never skipped) so an absent `restore` step never reads as
    /// "the rehearsal stopped early".
    fn restore(&mut self) -> (StepResult, bool, Option<i32>) {
        let mut step = StepBuilder::new("restore", StepKind::Bootstrap);
        if !self.machine_touched {
            step =
                step.detail("nothing was installed or removed, so there was nothing to put back");
            return (step.finish(), true, Some(0));
        }
        let outcome: Result<(StepResult, Option<i32>)> = (|| {
            let mut command = Command::new("msiexec.exe");
            command
                .arg("/i")
                .arg(self.msi_path)
                .args(["/qn", "/norestart", "/l*v"])
                .arg(self.evidence_dir.join("msiexec-restore-verbose.log"));
            let code = run_logged(
                self.evidence_dir,
                "msiexec-restore.log",
                command,
                secs(360.0),
            )?;
            step.assert(
                format!("msiexec /i exits 0 (was {code:?})"),
                code == Some(0),
            );
            let restored = arp_entry()?;
            step.assert("the ExoSnap ARP entry is back", restored.is_some());
            step.assert(
                format!("{} is back", Self::installed_exe_path().display()),
                Self::installed_exe_path().is_file(),
            );
            // DisplayVersion is not compared against the release tag: an MSI
            // ProductVersion cannot carry a prerelease suffix, so a
            // release-candidate build legitimately reports the plain
            // three-part version here.
            step.assert(
                "the ARP entry names its install location",
                restored
                    .as_ref()
                    .is_some_and(|r| !r.install_location.trim().is_empty()),
            );
            if let Some(restored) = &restored {
                step = step.detail(format!(
                    "{} {}",
                    restored.display_name, restored.display_version
                ));
            }
            Ok((step.finish(), code))
        })();
        match outcome {
            Ok((step, code)) => (step, true, code),
            Err(error) => {
                let mut step = StepBuilder::new("restore", StepKind::Bootstrap);
                step.assert(
                    format!("the candidate MSI could not be reinstalled: {error}"),
                    false,
                );
                (step.finish(), true, None)
            }
        }
    }
}

/// Packs the candidate Chocolatey package against a local copy of the
/// candidate MSI, installs it, uninstalls it, and reinstalls the candidate
/// MSI so the machine ends in the state later gates expect.
///
/// Runs entirely in-process: the caller already holds the elevation this
/// needs (silent msiexec refuses to elevate on its own), so nothing here
/// spawns a further elevated child.
pub fn run_rehearsal(
    staging: &Path,
    package_source: &Path,
    msi_path: &Path,
    msi_sha256: &str,
    evidence_dir: &Path,
) -> Result<RehearsalResult> {
    std::fs::create_dir_all(staging)?;
    std::fs::create_dir_all(evidence_dir)?;
    let work_dir = staging.join("work");
    if work_dir.exists() {
        std::fs::remove_dir_all(&work_dir)?;
    }
    std::fs::create_dir_all(&work_dir)?;

    let user_config = PathBuf::from(std::env::var("LOCALAPPDATA")?).join("ExoSnap");
    let vcredist_before = chocolatey_package_version("vcredist140");

    let mut rehearsal = Rehearsal {
        package_source,
        msi_path,
        msi_sha256,
        evidence_dir,
        work_dir,
        steps: Vec::new(),
        observations: Vec::new(),
        machine_touched: false,
    };

    let run_body = (|| -> Result<()> {
        let bootstrap_step = rehearsal.bootstrap(&user_config)?;
        if !rehearsal.push(bootstrap_step) {
            return Ok(());
        }
        let config_before = install::tree_hashes(&user_config)?;
        let (prepare_step, package_directory) = rehearsal.prepare(&user_config)?;
        if !rehearsal.push(prepare_step) {
            return Ok(());
        }
        let pack_step = rehearsal.pack(package_directory.as_deref())?;
        if !rehearsal.push(pack_step) {
            return Ok(());
        }
        let remove_existing_step = rehearsal.remove_existing()?;
        if !rehearsal.push(remove_existing_step) {
            return Ok(());
        }
        let install_step = rehearsal.install()?;
        if !rehearsal.push(install_step) {
            return Ok(());
        }
        let uninstall_step = rehearsal.uninstall(&user_config, &config_before)?;
        rehearsal.push(uninstall_step);
        Ok(())
    })();
    if let Err(error) = run_body {
        let mut step = StepBuilder::new("worker", StepKind::Bootstrap);
        step.assert(
            format!("the worker completed without throwing: {error}"),
            false,
        );
        rehearsal.push(step.finish());
    }

    let (restore_step, restore_ran, restore_exit_code) = rehearsal.restore();
    rehearsal.push(restore_step);

    let vcredist_after = chocolatey_package_version("vcredist140");
    let _ = std::fs::remove_dir_all(&rehearsal.work_dir);
    let ok = rehearsal.steps.iter().all(|step| step.ok);

    Ok(RehearsalResult {
        ok,
        msi_path: msi_path.display().to_string(),
        msi_sha256: msi_sha256.to_string(),
        restore_ran,
        restore_exit_code,
        vcredist_before,
        vcredist_after,
        observations: rehearsal.observations,
        completed_utc: crate::model::now_rfc3339(),
        steps: rehearsal.steps,
        fatal: None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn candidate(display_name: &str, product_code: &str) -> ArpCandidate {
        ArpCandidate {
            product_code: product_code.to_string(),
            display_name: display_name.to_string(),
            publisher: "Codexo".to_string(),
            display_version: "1.2.3".to_string(),
            install_location: r"C:\Program Files\ExoSnap".to_string(),
        }
    }

    #[test]
    fn arp_resolution_accepts_none_or_one_and_refuses_two() {
        assert_eq!(resolve_arp_entry(vec![]).unwrap(), None);
        let one = resolve_arp_entry(vec![candidate("ExoSnap", "{1111}")])
            .unwrap()
            .unwrap();
        assert_eq!(one.product_code, "{1111}");
        assert_eq!(one.display_name, "ExoSnap");
        let error = resolve_arp_entry(vec![
            candidate("ExoSnap", "{1111}"),
            candidate("ExoSnap Beta", "{2222}"),
        ])
        .unwrap_err();
        assert!(error.contains("refusing to guess"));
        assert!(error.contains("{1111}"));
        assert!(error.contains("{2222}"));
    }

    #[test]
    fn product_code_pattern_matches_only_guid_shaped_keys() {
        assert!(is_product_code("{40078385-F676-4C61-9A9C-F9028599D6D3}"));
        assert!(!is_product_code("7-Zip"));
        assert!(!is_product_code("{missing-brace"));
    }

    #[test]
    fn manifest_diff_reports_added_removed_and_changed_files() {
        let before = BTreeMap::from([
            ("kept.json".to_string(), "hash-a".to_string()),
            ("removed.json".to_string(), "hash-b".to_string()),
            ("changed.json".to_string(), "hash-c".to_string()),
        ]);
        let after = BTreeMap::from([
            ("kept.json".to_string(), "hash-a".to_string()),
            ("changed.json".to_string(), "hash-d".to_string()),
            ("added.json".to_string(), "hash-e".to_string()),
        ]);
        let differences = describe_manifest_diff(&before, &after);
        assert!(differences.contains(&"removed: removed.json".to_string()));
        assert!(differences.contains(&"changed: changed.json".to_string()));
        assert!(differences.contains(&"added: added.json".to_string()));
        assert_eq!(differences.len(), 3);
        assert!(describe_manifest_diff(&before, &before).is_empty());
    }

    #[test]
    fn stage_package_copy_rewrites_url_and_checksum_exactly_once() {
        let source = tempfile::tempdir().unwrap();
        let tools_dir = source.path().join("tools");
        std::fs::create_dir_all(&tools_dir).unwrap();
        std::fs::write(
            tools_dir.join("chocolateyinstall.ps1"),
            "$packageArgs = @{\n  url64bit      = 'https://example.invalid/exosnap.msi'\n  checksum64    = 'DEADBEEF'\n  checksumType64 = 'sha256'\n}\n",
        )
        .unwrap();
        let destination = tempfile::tempdir().unwrap();
        let msi_path = PathBuf::from(r"C:\staging\exosnap.msi");
        let outcome =
            stage_package_copy(source.path(), destination.path(), &msi_path, "CAFEBABE").unwrap();
        let CopyOutcome::Ready { install_script, .. } = outcome else {
            panic!("expected the rewrite to succeed");
        };
        let rewritten = std::fs::read_to_string(install_script).unwrap();
        assert!(rewritten.contains(r"url64bit      = 'C:\staging\exosnap.msi'"));
        assert!(rewritten.contains("checksum64    = 'CAFEBABE'"));
    }

    #[test]
    fn stage_package_copy_rejects_a_script_with_no_url_assignment() {
        let source = tempfile::tempdir().unwrap();
        let tools_dir = source.path().join("tools");
        std::fs::create_dir_all(&tools_dir).unwrap();
        std::fs::write(tools_dir.join("chocolateyinstall.ps1"), "# nothing here\n").unwrap();
        let destination = tempfile::tempdir().unwrap();
        let outcome =
            stage_package_copy(source.path(), destination.path(), Path::new("msi"), "hash")
                .unwrap();
        assert!(matches!(outcome, CopyOutcome::Rejected(_)));
    }

    #[test]
    fn rehearsal_result_shape_matches_the_verdict_consumer() {
        let steps = vec![
            StepBuilder::new("prepare", StepKind::Bootstrap).finish(),
            StepBuilder::new("pack", StepKind::Bootstrap).finish(),
            StepBuilder::new("removeExisting", StepKind::Bootstrap).finish(),
            StepBuilder::new("install", StepKind::Product).finish(),
            StepBuilder::new("uninstall", StepKind::Product).finish(),
            StepBuilder::new("restore", StepKind::Bootstrap).finish(),
        ];
        let result = RehearsalResult {
            ok: true,
            msi_path: "msi".to_string(),
            msi_sha256: "hash".to_string(),
            restore_ran: true,
            restore_exit_code: Some(0),
            vcredist_before: String::new(),
            vcredist_after: String::new(),
            observations: vec![],
            completed_utc: "2026-01-01T00:00:00Z".to_string(),
            steps,
            fatal: None,
        };
        let document = serde_json::to_value(&result).unwrap();
        crate::scenarios::update::chocolatey_verdict(&document).unwrap();
        assert!(document["fatal"].is_null());
        assert_eq!(document["steps"][3]["kind"], "product");

        let mut broken = document.clone();
        broken["steps"][3]["ok"] = serde_json::json!(false);
        assert!(crate::scenarios::update::chocolatey_verdict(&broken).is_err());
    }
}
