//! Pinned, resumable provisioning of an explicitly identified disposable guest.

#[cfg(any(windows, test))]
use std::collections::BTreeMap;
use std::collections::BTreeSet;
#[cfg(any(windows, test))]
use std::io::{Read, Write};
#[cfg(any(windows, test))]
use std::path::Path;
use std::path::PathBuf;
#[cfg(windows)]
use std::process::Command;

use anyhow::{Context as _, Result, bail, ensure};
use clap::Args;
use serde::{Deserialize, Serialize};
#[cfg(any(windows, test))]
use sha2::{Digest, Sha256};

#[cfg(windows)]
mod windows;

const DEFAULT_MANIFEST: &str = include_str!("../config/provision-manifest.json");
const STEPS: &[&str] = &[
    "power",
    "console",
    "hostdriver",
    "vcredist",
    "ffmpeg",
    "presentmon",
    "soundvolumeview",
    "vbcable",
    "idd",
    "uac",
];

#[derive(Clone, Debug, Args)]
pub struct ProvisionArgs {
    /// JSON package pins. The embedded manifest is used when omitted.
    #[arg(long)]
    pub manifest: Option<PathBuf>,
    /// Apply changes. Without this flag, print the validated plan only.
    #[arg(long, requires = "disposable_vm_id")]
    pub apply: bool,
    /// Hyper-V VM identifier, checked against the guest's own KVP registry.
    #[arg(long)]
    pub disposable_vm_id: Option<String>,
    #[arg(long, value_delimiter = ',')]
    pub only: Vec<String>,
    /// Reapply completed steps from the same manifest.
    #[arg(long)]
    pub force: bool,
    #[arg(long)]
    pub enable_hdr: bool,
    #[arg(long)]
    pub state_path: Option<PathBuf>,
    #[arg(long)]
    pub list_steps: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Manifest {
    pub schema: u32,
    pub display_profile: String,
    pub qualified_display_profile: String,
    pub packages: Vec<Package>,
    pub display: Display,
    pub paths: Paths,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Package {
    pub id: String,
    pub title: String,
    pub version: String,
    pub reason: String,
    #[serde(flatten)]
    pub source: PackageSource,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum PackageSource {
    Winget {
        #[serde(rename = "wingetId")]
        winget_id: String,
    },
    Download {
        #[serde(rename = "urlTemplate")]
        url_template: String,
        sha256: String,
        layout: Layout,
        #[serde(rename = "fileName", default)]
        file_name: Option<String>,
        #[serde(default)]
        installer: Option<String>,
        #[serde(rename = "rebootRequired", default)]
        reboot_required: bool,
        #[serde(rename = "hardwareId", default)]
        hardware_id: Option<String>,
        #[serde(rename = "signerThumbprint", default)]
        signer_thumbprint: Option<String>,
        #[serde(rename = "signerSubject", default)]
        signer_subject: Option<String>,
    },
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum Layout {
    File,
    Zip,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Display {
    pub width: u32,
    pub height: u32,
    pub refresh_rates: Vec<u32>,
    pub hdr: bool,
    pub config_directory: PathBuf,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Paths {
    pub tools: PathBuf,
    pub staging: PathBuf,
    pub host_driver_staging: PathBuf,
}

impl Manifest {
    pub fn parse(text: &str) -> Result<Self> {
        let manifest: Self = serde_json::from_str(text).context("invalid provisioning manifest")?;
        manifest.validate()?;
        Ok(manifest)
    }

    fn validate(&self) -> Result<()> {
        ensure!(self.schema == 1, "unsupported provisioning manifest schema");
        ensure!(
            self.display_profile == "mtt" && self.qualified_display_profile == "mtt",
            "unqualified virtual display profile"
        );
        ensure!(
            self.display.width > 0
                && self.display.height > 0
                && !self.display.refresh_rates.is_empty()
                && self.display.refresh_rates.iter().all(|rate| *rate > 0),
            "invalid virtual display mode list"
        );
        let mut ids = BTreeSet::new();
        for package in &self.packages {
            ensure!(ids.insert(&package.id), "duplicate package {}", package.id);
            ensure!(
                safe_component(&package.id)
                    && safe_component(&package.version)
                    && package.version != "PIN-REQUIRED",
                "invalid or missing version pin for {}",
                package.id
            );
            match &package.source {
                PackageSource::Winget { winget_id } => {
                    ensure!(!winget_id.is_empty(), "missing winget id")
                }
                PackageSource::Download {
                    url_template,
                    sha256,
                    file_name,
                    installer,
                    ..
                } => {
                    ensure!(
                        url_template.starts_with("https://")
                            && !url_template.contains(['\r', '\n']),
                        "downloads must use HTTPS"
                    );
                    ensure!(
                        sha256.len() == 64 && sha256.bytes().all(|byte| byte.is_ascii_hexdigit()),
                        "missing SHA-256 pin for {}",
                        package.id
                    );
                    for name in [file_name, installer].into_iter().flatten() {
                        ensure!(safe_component(name), "invalid package filename {name}");
                    }
                }
            }
        }
        for id in [
            "vcredist",
            "ffmpeg",
            "presentmon",
            "soundvolumeview",
            "vbcable",
            "idd",
            "nefcon",
        ] {
            self.package(id)?;
        }
        ensure!(
            matches!(self.package("idd")?.source, PackageSource::Download { ref hardware_id, ref signer_thumbprint, .. } if hardware_id.as_deref() == Some(r"Root\MttVDD") && signer_thumbprint.as_ref().is_some_and(|pin| pin.len() == 40 && pin.bytes().all(|byte| byte.is_ascii_hexdigit()))),
            "display driver requires its hardware id and publisher pin"
        );
        Ok(())
    }

    fn package(&self, id: &str) -> Result<&Package> {
        self.packages
            .iter()
            .find(|package| package.id == id)
            .with_context(|| format!("manifest has no {id} package"))
    }
}

fn safe_component(value: &str) -> bool {
    !value.is_empty()
        && value != "."
        && value != ".."
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"-_.+".contains(&byte))
}

#[cfg(any(windows, test))]
#[derive(Debug, Default, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct ResumeState {
    fingerprint: String,
    vm_id: String,
    reboot_boot: Option<String>,
    steps: BTreeMap<String, StepReceipt>,
}

#[cfg(any(windows, test))]
#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct StepReceipt {
    completed: bool,
    detail: String,
}

fn selected_steps(only: &[String]) -> Result<Vec<&'static str>> {
    for step in only {
        ensure!(
            STEPS.contains(&step.as_str()),
            "unknown provisioning step {step}"
        );
    }
    Ok(STEPS
        .iter()
        .copied()
        .filter(|step| only.is_empty() || only.iter().any(|only| only == step))
        .collect())
}

/// Returns 2 when a successful driver step requires a guest reboot, otherwise 0.
pub fn run(args: &ProvisionArgs) -> Result<u8> {
    if args.list_steps {
        println!("{}", STEPS.join("\n"));
        return Ok(0);
    }
    let text = match &args.manifest {
        Some(path) => {
            std::fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?
        }
        None => DEFAULT_MANIFEST.into(),
    };
    let manifest = Manifest::parse(&text)?;
    let selected = selected_steps(&args.only)?;
    if !args.apply {
        println!(
            "{}",
            serde_json::to_string_pretty(
                &serde_json::json!({"mode": "plan", "steps": selected, "manifest": manifest, "hdr": args.enable_hdr || manifest.display.hdr})
            )?
        );
        return Ok(0);
    }
    let vm_id = args
        .disposable_vm_id
        .as_deref()
        .context("--apply requires --disposable-vm-id")?;
    assert_disposable_guest(vm_id)?;
    #[cfg(not(windows))]
    bail!("guest provisioning applies only on Windows");
    #[cfg(windows)]
    {
        let fingerprint = hex::encode(Sha256::digest(serde_json::to_vec(&(
            manifest.clone(),
            args.enable_hdr,
        ))?));
        let path = args
            .state_path
            .clone()
            .unwrap_or_else(|| manifest.paths.staging.join("provision-state.json"));
        let mut state = load_state(&path, &fingerprint, vm_id)?;
        let boot = windows::boot_identity()?;
        if state
            .reboot_boot
            .as_ref()
            .is_some_and(|previous| previous != &boot)
        {
            state.reboot_boot = None;
        }
        for step in selected {
            if !args.force
                && state
                    .steps
                    .get(step)
                    .is_some_and(|receipt| receipt.completed)
            {
                println!("{step}: already complete");
                continue;
            }
            match apply_step(step, &manifest, args.enable_hdr, vm_id) {
                Ok((detail, reboot)) => {
                    if reboot {
                        state.reboot_boot = Some(boot.clone());
                    }
                    println!("{step}: {detail}");
                    state.steps.insert(
                        step.into(),
                        StepReceipt {
                            completed: true,
                            detail,
                        },
                    );
                    save_state(&path, &state)?;
                }
                Err(error) => {
                    state.steps.insert(
                        step.into(),
                        StepReceipt {
                            completed: false,
                            detail: format!("{error:#}"),
                        },
                    );
                    save_state(&path, &state)?;
                    return Err(error).with_context(|| format!("provisioning stopped at {step}"));
                }
            }
        }
        save_state(&path, &state)?;
        if state.reboot_boot.is_some() {
            println!("A guest reboot is required. Resume provisioning after restarting the guest.");
            return Ok(2);
        }
        Ok(0)
    }
}

#[cfg(any(windows, test))]
fn load_state(path: &Path, fingerprint: &str, vm_id: &str) -> Result<ResumeState> {
    if !path.exists() {
        return Ok(ResumeState {
            fingerprint: fingerprint.into(),
            vm_id: vm_id.into(),
            ..ResumeState::default()
        });
    }
    let state: ResumeState = serde_json::from_slice(&std::fs::read(path)?)
        .context("corrupt provisioning state; preserve and inspect it before retrying")?;
    ensure!(
        state.fingerprint == fingerprint && state.vm_id.eq_ignore_ascii_case(vm_id),
        "provisioning state belongs to different pins, HDR configuration or VM; use a new state path"
    );
    Ok(state)
}

#[cfg(any(windows, test))]
fn save_state(path: &Path, state: &ResumeState) -> Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("json.partial");
    let mut file = std::fs::File::create(&temporary)?;
    file.write_all(&serde_json::to_vec_pretty(state)?)?;
    file.sync_all()?;
    drop(file);
    #[cfg(windows)]
    windows::replace_file(&temporary, path)?;
    #[cfg(not(windows))]
    std::fs::rename(temporary, path)?;
    Ok(())
}

pub fn assert_disposable_guest(vm_id: &str) -> Result<()> {
    ensure!(
        valid_vm_id(vm_id),
        "disposable VM identifier must be a GUID"
    );
    #[cfg(windows)]
    return windows::assert_guest(vm_id);
    #[cfg(not(windows))]
    bail!("disposable guest mutation requires Windows Hyper-V guest identity");
}

fn valid_vm_id(value: &str) -> bool {
    let value = value.trim_matches(['{', '}']);
    value.len() == 36
        && value.bytes().enumerate().all(|(index, byte)| {
            if [8, 13, 18, 23].contains(&index) {
                byte == b'-'
            } else {
                byte.is_ascii_hexdigit()
            }
        })
}

pub fn attach_console(vm_id: &str) -> Result<()> {
    assert_disposable_guest(vm_id)?;
    #[cfg(windows)]
    windows::attach_console()?;
    Ok(())
}

#[cfg(windows)]
fn checked_command(program: &Path, args: &[String], accepted: &[i32]) -> Result<i32> {
    let output = Command::new(program)
        .args(args)
        .output()
        .with_context(|| format!("run {}", program.display()))?;
    let code = output
        .status
        .code()
        .context("tool terminated without an exit code")?;
    ensure!(
        accepted.contains(&code),
        "{} exited {code}: {}{}",
        program.display(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(code)
}

#[cfg(any(windows, test))]
fn sha256_file(path: &Path) -> Result<String> {
    let mut file = std::fs::File::open(path)?;
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let length = file.read(&mut buffer)?;
        if length == 0 {
            break;
        }
        hash.update(&buffer[..length]);
    }
    Ok(hex::encode(hash.finalize()))
}

#[cfg(windows)]
fn verified_download(package: &Package, staging: &Path) -> Result<PathBuf> {
    verified_download_with(package, staging, |url, partial| {
        checked_command(
            Path::new("curl.exe"),
            &[
                "--fail".into(),
                "--location".into(),
                "--proto".into(),
                "=https".into(),
                "--proto-redir".into(),
                "=https".into(),
                "--retry".into(),
                "1".into(),
                "--output".into(),
                partial.to_string_lossy().into_owned(),
                url.into(),
            ],
            &[0],
        )?;
        Ok(())
    })
}

#[cfg(any(windows, test))]
fn verified_download_with(
    package: &Package,
    staging: &Path,
    mut fetch: impl FnMut(&str, &Path) -> Result<()>,
) -> Result<PathBuf> {
    let PackageSource::Download {
        url_template,
        sha256,
        ..
    } = &package.source
    else {
        bail!("{} is not a download", package.id);
    };
    let url = url_template.replace("{version}", &package.version);
    let name = url
        .rsplit('/')
        .next()
        .context("download URL has no filename")?;
    ensure!(safe_component(name), "invalid download filename");
    let directory = staging.join("downloads").join(&package.id);
    std::fs::create_dir_all(&directory)?;
    let target = directory.join(name);
    if target.exists() {
        if sha256_file(&target)?.eq_ignore_ascii_case(sha256) {
            return Ok(target);
        }
        std::fs::remove_file(&target)?;
    }
    let partial = target.with_extension("partial");
    for _ in 0..2 {
        if partial.exists() {
            std::fs::remove_file(&partial)?;
        }
        fetch(&url, &partial)?;
        if sha256_file(&partial)?.eq_ignore_ascii_case(sha256) {
            std::fs::rename(&partial, &target)?;
            return Ok(target);
        }
        std::fs::remove_file(&partial)?;
    }
    bail!("{} repeatedly failed its SHA-256 pin", package.id)
}
#[cfg(windows)]
fn extract_package(package: &Package, manifest: &Manifest) -> Result<PathBuf> {
    let archive = verified_download(package, &manifest.paths.staging)?;
    let PackageSource::Download {
        sha256,
        layout: Layout::Zip,
        ..
    } = &package.source
    else {
        bail!("{} is not an archive", package.id);
    };
    let destination = manifest
        .paths
        .staging
        .join("extracted")
        .join(&package.id)
        .join(sha256);
    extract_zip(&archive, &destination)?;
    Ok(destination)
}

#[cfg(any(windows, test))]
fn extract_zip(archive: &Path, destination: &Path) -> Result<()> {
    std::fs::create_dir_all(destination)?;
    let canonical = std::fs::canonicalize(destination)?;
    let mut zip = zip::ZipArchive::new(std::fs::File::open(archive)?)?;
    for index in 0..zip.len() {
        let mut entry = zip.by_index(index)?;
        let relative = entry
            .enclosed_name()
            .context("archive entry escapes extraction directory")?;
        ensure!(!entry.is_symlink(), "archive symlinks are not allowed");
        let target = destination.join(relative);
        let parent = if entry.is_dir() {
            target.as_path()
        } else {
            target.parent().context("archive entry has no parent")?
        };
        let ancestor = parent
            .ancestors()
            .find(|path| path.exists())
            .context("archive target has no existing ancestor")?;
        ensure!(
            std::fs::canonicalize(ancestor)?.starts_with(&canonical),
            "archive target follows an external link"
        );
        std::fs::create_dir_all(parent)?;
        ensure!(
            std::fs::canonicalize(parent)?.starts_with(&canonical),
            "archive target follows an external link"
        );
        if entry.is_dir() {
            continue;
        }
        if let Ok(metadata) = std::fs::symlink_metadata(&target) {
            ensure!(
                !metadata.file_type().is_symlink(),
                "archive file target is a link"
            );
        }
        let mut file = std::fs::File::create(target)?;
        std::io::copy(&mut entry, &mut file)?;
    }
    Ok(())
}

#[cfg(any(windows, test))]
fn files_under(root: &Path) -> Result<Vec<PathBuf>> {
    let mut result = Vec::new();
    for entry in std::fs::read_dir(root)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        ensure!(
            !kind.is_symlink(),
            "provisioning input must not contain links: {}",
            entry.path().display()
        );
        if kind.is_dir() {
            result.extend(files_under(&entry.path())?);
        } else if kind.is_file() {
            result.push(entry.path());
        }
    }
    result.sort();
    Ok(result)
}

#[cfg(windows)]
fn find_unique(root: &Path, predicate: impl Fn(&Path) -> bool) -> Result<PathBuf> {
    let matches: Vec<_> = files_under(root)?
        .into_iter()
        .filter(|path| predicate(path))
        .collect();
    ensure!(
        matches.len() == 1,
        "expected exactly one matching file under {}, found {}",
        root.display(),
        matches.len()
    );
    Ok(matches.into_iter().next().unwrap())
}

#[cfg(any(windows, test))]
fn copy_if_changed(source: &Path, destination: &Path) -> Result<bool> {
    if destination.is_file() && sha256_file(source)? == sha256_file(destination)? {
        return Ok(false);
    }
    if let Some(parent) = destination.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::copy(source, destination)
        .with_context(|| format!("copy {} to {}", source.display(), destination.display()))?;
    Ok(true)
}

#[cfg(any(windows, test))]
fn display_xml(display: &Display, enable_hdr: bool) -> String {
    let rates = display
        .refresh_rates
        .iter()
        .map(|rate| format!("        <refresh_rate>{rate}</refresh_rate>\n"))
        .collect::<String>();
    format!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<vdd_settings>\n  <monitors><count>1</count></monitors>\n  <resolutions>\n    <resolution>\n      <width>{}</width>\n      <height>{}</height>\n{}    </resolution>\n  </resolutions>\n  <colour><HDRPlus>{}</HDRPlus></colour>\n</vdd_settings>\n",
        display.width,
        display.height,
        rates,
        enable_hdr || display.hdr
    )
}

#[cfg(windows)]
fn apply_step(
    step: &str,
    manifest: &Manifest,
    enable_hdr: bool,
    vm_id: &str,
) -> Result<(String, bool)> {
    match step {
        "power" => {
            for args in [
                vec!["/change", "standby-timeout-ac", "0"],
                vec!["/change", "monitor-timeout-ac", "0"],
                vec!["/change", "hibernate-timeout-ac", "0"],
                vec!["/hibernate", "off"],
                vec!["/setactive", "SCHEME_MIN"],
            ] {
                checked_command(
                    Path::new("powercfg.exe"),
                    &args.iter().map(|arg| (*arg).into()).collect::<Vec<_>>(),
                    &[0],
                )?;
            }
            Ok((
                "sleep and display-off disabled; high performance active".into(),
                false,
            ))
        }
        "console" => {
            let exe = std::env::current_exe()?;
            let action = format!(
                "\"{}\" attach-console --disposable-vm-id {vm_id}",
                exe.display()
            );
            let user = std::env::var("USERNAME")
                .context("provisioning needs the intended interactive account")?;
            checked_command(
                Path::new("schtasks.exe"),
                &[
                    "/Create".into(),
                    "/F".into(),
                    "/TN".into(),
                    "ExoSnapAttachConsole".into(),
                    "/SC".into(),
                    "ONLOGON".into(),
                    "/RU".into(),
                    user,
                    "/IT".into(),
                    "/RL".into(),
                    "HIGHEST".into(),
                    "/TR".into(),
                    action,
                ],
                &[0],
            )?;
            let detail = match windows::attach_console() {
                Ok(()) => "interactive session attached to console".into(),
                Err(error) => {
                    format!("logon task installed; current console unavailable: {error:#}")
                }
            };
            Ok((detail, false))
        }
        "hostdriver" => {
            let staging = &manifest.paths.host_driver_staging;
            ensure!(
                staging.is_dir(),
                "host GPU driver files must be staged before provisioning"
            );
            let system =
                PathBuf::from(std::env::var_os("SystemRoot").context("SystemRoot unavailable")?)
                    .join("System32");
            let repository = staging.join("FileRepository");
            let mut count = 0;
            if repository.is_dir() {
                for source in files_under(&repository)? {
                    count += usize::from(copy_if_changed(
                        &source,
                        &system
                            .join("HostDriverStore/FileRepository")
                            .join(source.strip_prefix(&repository)?),
                    )?);
                }
            }
            let direct = staging.join("System32");
            if direct.is_dir() {
                for source in files_under(&direct)? {
                    ensure!(
                        source.parent() == Some(direct.as_path()),
                        "System32 staging must contain direct files only"
                    );
                    count += usize::from(copy_if_changed(
                        &source,
                        &system.join(source.file_name().unwrap()),
                    )?);
                }
            }
            ensure!(
                repository.is_dir() || direct.is_dir(),
                "host driver staging is empty"
            );
            Ok((format!("{count} host GPU driver files updated"), false))
        }
        "vcredist" | "ffmpeg" => {
            let package = manifest.package(step)?;
            let PackageSource::Winget { winget_id } = &package.source else {
                bail!("{step} requires a winget pin");
            };
            let tool = windows::winget()?;
            checked_command(
                &tool,
                &[
                    "install".into(),
                    "--id".into(),
                    winget_id.clone(),
                    "--version".into(),
                    package.version.clone(),
                    "--exact".into(),
                    "--silent".into(),
                    "--accept-package-agreements".into(),
                    "--accept-source-agreements".into(),
                    "--disable-interactivity".into(),
                ],
                &[0, -1978335189],
            )?;
            Ok((
                format!("{} {} installed", package.title, package.version),
                false,
            ))
        }
        "presentmon" | "soundvolumeview" => {
            let package = manifest.package(step)?;
            let PackageSource::Download {
                file_name: Some(file_name),
                layout,
                ..
            } = &package.source
            else {
                bail!("{step} needs a destination filename");
            };
            let source = if *layout == Layout::File {
                verified_download(package, &manifest.paths.staging)?
            } else {
                find_unique(&extract_package(package, manifest)?, |path| {
                    path.file_name()
                        .is_some_and(|name| name == file_name.as_str())
                })?
            };
            let target = manifest.paths.tools.join(file_name);
            copy_if_changed(&source, &target)?;
            Ok((
                format!("{} installed at {}", package.title, target.display()),
                false,
            ))
        }
        "vbcable" => {
            let package = manifest.package(step)?;
            let PackageSource::Download {
                installer: Some(installer),
                ..
            } = &package.source
            else {
                bail!("VB-CABLE installer missing from manifest");
            };
            let source = find_unique(&extract_package(package, manifest)?, |path| {
                path.file_name()
                    .is_some_and(|name| name == installer.as_str())
            })?;
            checked_command(&source, &["-i".into(), "-h".into()], &[0, 3010])?;
            Ok((
                "VB-CABLE installed; endpoint requires guest reboot".into(),
                true,
            ))
        }
        "idd" => {
            let package = manifest.package(step)?;
            let PackageSource::Download {
                hardware_id: Some(hardware_id),
                signer_thumbprint: Some(pin),
                ..
            } = &package.source
            else {
                bail!("display driver hardware id/publisher pin unavailable");
            };
            let root = extract_package(package, manifest)?;
            let inf = find_unique(&root, |path| {
                path.extension()
                    .is_some_and(|extension| extension.eq_ignore_ascii_case("inf"))
            })?;
            windows::trust_catalog(&inf.with_file_name("mttvdd.cat"), pin)?;
            std::fs::create_dir_all(&manifest.display.config_directory)?;
            std::fs::write(
                manifest.display.config_directory.join("vdd_settings.xml"),
                display_xml(&manifest.display, enable_hdr),
            )?;
            if windows::display_installed(hardware_id)? {
                return Ok((
                    "pinned virtual display already installed; mode configuration updated".into(),
                    false,
                ));
            }
            let nefcon = manifest.package("nefcon")?;
            let root = extract_package(nefcon, manifest)?.join("x64");
            let executable = find_unique(&root, |path| {
                path.file_name()
                    .is_some_and(|name| name.eq_ignore_ascii_case("nefconw.exe"))
            })?;
            let code = checked_command(
                &executable,
                &[
                    "install".into(),
                    inf.to_string_lossy().into_owned(),
                    hardware_id.clone(),
                    "--no-duplicates".into(),
                    "--remove-duplicates".into(),
                ],
                &[0, 3010],
            )?;
            Ok(("pinned virtual display installed".into(), code == 3010))
        }
        "uac" => Ok((
            format!(
                "ConsentPromptBehaviorAdmin remains {}",
                windows::uac_setting()?
            ),
            false,
        )),
        _ => bail!("unknown provisioning step {step}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_pins_are_complete_and_only_steps_are_closed() {
        let manifest = Manifest::parse(DEFAULT_MANIFEST).unwrap();
        assert_eq!(manifest.packages.len(), 7);
        assert!(manifest.packages.iter().all(|package| package.id != "pwsh"));
        assert!(selected_steps(&["idd".into(), "uac".into()]).unwrap() == ["idd", "uac"]);
        assert!(selected_steps(&["arbitrary".into()]).is_err());
        assert!(
            Manifest::parse(&DEFAULT_MANIFEST.replace("14.51.36247.0", "PIN-REQUIRED")).is_err()
        );
    }

    #[test]
    fn resume_identity_prevents_skipping_different_pins_or_another_vm() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("state.json");
        let mut state = load_state(&path, "pins", "guest").unwrap();
        state.steps.insert(
            "power".into(),
            StepReceipt {
                completed: true,
                detail: "done".into(),
            },
        );
        state.reboot_boot = Some("boot-1".into());
        save_state(&path, &state).unwrap();
        assert!(load_state(&path, "pins", "guest").unwrap().steps["power"].completed);
        assert_eq!(
            load_state(&path, "pins", "guest")
                .unwrap()
                .reboot_boot
                .as_deref(),
            Some("boot-1")
        );
        assert!(load_state(&path, "different-pins", "guest").is_err());
        assert!(load_state(&path, "pins", "different-guest").is_err());
        std::fs::write(&path, "{").unwrap();
        assert!(load_state(&path, "pins", "guest").is_err());
    }

    #[test]
    fn driver_copy_preserves_identical_loaded_bytes_and_updates_different_bytes() {
        let directory = tempfile::tempdir().unwrap();
        let source = directory.path().join("source.dll");
        let target = directory.path().join("target.dll");
        std::fs::write(&source, "driver").unwrap();
        assert!(copy_if_changed(&source, &target).unwrap());
        assert!(!copy_if_changed(&source, &target).unwrap());
        std::fs::write(&source, "new driver").unwrap();
        assert!(copy_if_changed(&source, &target).unwrap());
    }

    #[test]
    fn archive_traversal_fails_before_writing_outside_destination() {
        let directory = tempfile::tempdir().unwrap();
        let archive = directory.path().join("archive.zip");
        let mut writer = zip::ZipWriter::new(std::fs::File::create(&archive).unwrap());
        writer
            .start_file("../escaped.txt", zip::write::SimpleFileOptions::default())
            .unwrap();
        writer.write_all(b"untrusted").unwrap();
        writer.finish().unwrap();
        assert!(extract_zip(&archive, &directory.path().join("output")).is_err());
        assert!(!directory.path().join("escaped.txt").exists());
    }

    #[test]
    fn virtual_display_modes_and_hdr_are_explicit() {
        let manifest = Manifest::parse(DEFAULT_MANIFEST).unwrap();
        let xml = display_xml(&manifest.display, false);
        assert!(xml.contains("<width>2560</width>"));
        assert!(xml.contains("<refresh_rate>144</refresh_rate>"));
        assert!(xml.contains("<HDRPlus>false</HDRPlus>"));
        assert!(display_xml(&manifest.display, true).contains("<HDRPlus>true</HDRPlus>"));
    }

    #[test]
    fn mutation_identifier_cannot_be_a_shell_expression() {
        assert!(valid_vm_id("{00112233-4455-6677-8899-aabbccddeeff}"));
        for invalid in ["", "guest", "00112233-4455-6677-8899-aabbccddee;f", ".."] {
            assert!(!valid_vm_id(invalid));
        }
    }

    #[test]
    fn downloaded_bytes_are_pinned_before_the_cache_can_expose_them() {
        let directory = tempfile::tempdir().unwrap();
        let mut package = Manifest::parse(DEFAULT_MANIFEST)
            .unwrap()
            .package("presentmon")
            .unwrap()
            .clone();
        if let PackageSource::Download { sha256, .. } = &mut package.source {
            *sha256 = hex::encode(Sha256::digest(b"trusted bytes"));
        }
        let mut attempts = 0;
        let result = verified_download_with(&package, directory.path(), |_, path| {
            attempts += 1;
            std::fs::write(path, b"tampered bytes")?;
            Ok(())
        });
        assert!(result.is_err());
        assert_eq!(attempts, 2);
        assert!(files_under(directory.path()).unwrap().is_empty());
        let path = verified_download_with(&package, directory.path(), |_, path| {
            std::fs::write(path, b"trusted bytes")?;
            Ok(())
        })
        .unwrap();
        let cached = verified_download_with(&package, directory.path(), |_, _| {
            panic!("verified cached download must not be fetched again")
        })
        .unwrap();
        assert_eq!(path, cached);
        std::fs::write(&path, b"interrupted bytes").unwrap();
        assert!(
            verified_download_with(&package, directory.path(), |_, path| {
                std::fs::write(path, b"trusted bytes")?;
                Ok(())
            })
            .is_ok()
        );
    }
}
