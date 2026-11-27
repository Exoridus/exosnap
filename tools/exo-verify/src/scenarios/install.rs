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
    registry_key: String,
    display_version: String,
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
        r"HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
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
                registry_key: subkey,
                display_version: value("DisplayVersion"),
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

    let user_desktop = PathBuf::from(std::env::var("USERPROFILE")?).join(r"Desktop\ExoSnap.lnk");
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

    let exe = PathBuf::from(std::env::var("LOCALAPPDATA")?).join(r"Programs\ExoSnap\exosnap.exe");
    let shortcut = PathBuf::from(std::env::var("APPDATA")?)
        .join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk");
    product_ensure!(exe.is_file(), "Setup did not install exosnap.exe");
    product_ensure!(
        scoped_reg_value(USER_KEY, "installed")?.is_some(),
        "Setup did not write the product registry marker"
    );
    product_ensure!(
        shortcut.is_file(),
        "Setup did not create the Start Menu shortcut"
    );
    product_ensure!(
        !user_desktop.exists(),
        "a silent Setup created a desktop shortcut without the option"
    );
    product_ensure!(
        reg_value("installed")?.is_none(),
        "current-user Setup wrote machine registration"
    );
    let opposite = ctx.package(FileRole::SetupAllUsers)?;
    let conflict = run_setup(ctx, &opposite, "/install", "/quiet", "opposite-scope.log")?;
    product_ensure!(
        conflict.code() == Some(1638),
        "all-users Setup did not refuse the existing user registration"
    );
    product_ensure!(
        exe.is_file() && reg_value("installed")?.is_none(),
        "scope refusal changed the installation"
    );
    let rows = exosnap_arp_rows()?;
    let registrations: BTreeMap<_, _> = rows
        .iter()
        .map(|row| (&row.registry_key, &row.display_version))
        .collect();
    ctx.evidence.put("setupRegistrations", json!(registrations));
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

    // A newer-version foreign executable distinguishes forced replacement
    // from Windows Installer's usual equal-or-older version repair. Never start
    // the altered file; the hash of the original installed bytes is the oracle.
    let original_hash = sha256_file(&exe)?.0;
    let higher_version = PathBuf::from(std::env::var("SystemRoot")?).join(r"System32\cmd.exe");
    fs::copy(&higher_version, &exe)?;
    product_ensure!(
        sha256_file(&exe)?.0 != original_hash,
        "repair fixture did not change the installed executable"
    );
    let verified_msi = ctx.package(FileRole::Installer)?;
    let repair_log = ctx.scenario_dir.join("verified-msi-recache.log");
    let forced = crate::tools::run(
        Command::new("msiexec.exe")
            .arg("/i")
            .arg(&verified_msi)
            .args([
                "/qn",
                "/norestart",
                "EXOSNAP_REQUESTED_SCOPE=user",
                "ARPSYSTEMCOMPONENT=1",
                "EXOSNAP_SETUP_REPAIR=1",
                "REINSTALL=ALL",
                "REINSTALLMODE=vamus",
                "/l*v",
            ])
            .arg(&repair_log),
        secs(360.0),
    )?;
    ctx.keep(&repair_log);
    product_ensure!(
        matches!(forced.code(), Some(0 | 3010)) && sha256_file(&exe)?.0 == original_hash,
        "the verified release MSI did not restore the full executable; see {}",
        repair_log.display()
    );
    let visible_after_repair: Vec<_> = exosnap_arp_rows()?
        .into_iter()
        .filter(|r| !r.system_component)
        .collect();
    product_ensure!(
        visible_after_repair.len() == 1
            && quoted_exe(&visible_after_repair[0].uninstall_string).as_ref() == Some(&cached),
        "verified MSI repair changed the cached bundle's Installed Apps ownership"
    );
    fs::copy(&higher_version, &exe)?;
    let repair = run_setup(ctx, &cached, "/repair", "/quiet", "setup-repair.log")?;
    product_ensure!(
        matches!(repair.code(), Some(0 | 3010)),
        "Setup /repair exited {:?}; log: {}",
        repair.code(),
        ctx.scenario_dir.join("setup-repair.log").display()
    );
    product_ensure!(
        sha256_file(&exe)?.0 == original_hash && scoped_reg_value(USER_KEY, "installed")?.is_some(),
        "cached Setup repair did not restore the original executable"
    );

    // The quiet uninstall below removes Burn's cached bundle, and the passive
    // phase needs a full self-contained bundle to install from, so copy the
    // artifact again rather than relying on the cache.
    let passive_bundle = work.join("setup-passive.exe");
    fs::copy(&setup_source, &passive_bundle)?;

    let uninstall = run_setup(ctx, &cached, "/uninstall", "/quiet", "setup-uninstall.log")?;
    product_ensure!(
        matches!(uninstall.code(), Some(0 | 3010)),
        "Setup /uninstall exited {:?}; log: {}",
        uninstall.code(),
        ctx.scenario_dir.join("setup-uninstall.log").display()
    );
    product_ensure!(
        !exe.exists()
            && scoped_reg_value(USER_KEY, "installed")?.is_none()
            && !shortcut.exists()
            && !user_desktop.exists(),
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
        exe.is_file()
            && scoped_reg_value(USER_KEY, "installed")?.is_some()
            && !user_desktop.exists(),
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
        !exe.exists() && scoped_reg_value(USER_KEY, "installed")?.is_none(),
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

/// Drives one interactive Setup run, keeps its Burn log and page screenshots,
/// and turns a driver failure into a product failure with the log path.
#[cfg(windows)]
fn run_uia(ctx: &mut Context, run: &super::setup_ui::Run<'_>) -> Step {
    let result = super::setup_ui::drive(run)
        .map_err(|error| Stop::fail(format!("the interactive Setup failed: {error:#}")));
    ctx.keep(run.log);
    if let Ok(entries) = fs::read_dir(run.screenshots) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_file() {
                ctx.keep(&path);
            }
        }
    }
    result
}

#[cfg(not(windows))]
fn setup_interactive(_: &mut Context) -> Step {
    Err(Stop::unavailable("interactive Setup requires Windows"))
}

#[cfg(windows)]
fn setup_interactive(ctx: &mut Context) -> Step {
    let residue = super::clean::residue()?;
    infra_ensure!(
        residue.is_empty(),
        "the disposable machine is not clean: {}",
        residue.join("; ")
    );
    let setup = ctx.package(FileRole::SetupAllUsers)?;
    let screenshots = ctx.scenario_dir.join("screenshots");
    let install_log = ctx.scenario_dir.join("setup-ui-install.log");
    let uninstall_log = ctx.scenario_dir.join("setup-ui-uninstall.log");

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
    run_uia(
        ctx,
        &super::setup_ui::Run {
            setup: &setup,
            mode: super::setup_ui::Mode::Install,
            log: &install_log,
            expected_exe: &exe,
            screenshots: &screenshots,
            desktop_shortcut: true,
            remove_user_data: false,
            click_launch: true,
        },
    )?;
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
    run_uia(
        ctx,
        &super::setup_ui::Run {
            setup: &setup,
            mode: super::setup_ui::Mode::Uninstall,
            log: &uninstall_log,
            expected_exe: &exe,
            screenshots: &screenshots,
            desktop_shortcut: false,
            remove_user_data: true,
            click_launch: false,
        },
    )?;
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
    scoped_reg_value(INSTALL_KEY, name)
}

fn scoped_reg_value(key: &str, name: &str) -> Result<Option<String>> {
    let out = crate::tools::run(
        Command::new("reg.exe").args(["query", key, "/v", name]),
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
            .args([
                "/qn",
                "/norestart",
                "ALLUSERS=1",
                "EXOSNAP_REQUESTED_SCOPE=machine",
                "/l*v",
            ])
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

    #[cfg(windows)]
    fn set_msi_install_folder(command: &mut Command, folder: &str) -> Result<()> {
        use std::os::windows::process::CommandExt;

        // msiexec requires the quotes around the property value, after '='.
        anyhow::ensure!(!folder.contains('"'));
        command.raw_arg(format!(r#"INSTALLFOLDER="{folder}""#));
        Ok(())
    }

    #[cfg(windows)]
    fn smoke_msi(
        path: &Path,
        verb: &str,
        owner: Option<&str>,
        log: &Path,
    ) -> Result<crate::tools::Output> {
        let mut command = Command::new("msiexec.exe");
        command
            .arg(verb)
            .arg(path)
            .args([
                "/qn",
                "/norestart",
                "ALLUSERS=1",
                "EXOSNAP_REQUESTED_SCOPE=machine",
                "/l*v",
            ])
            .arg(log);
        if let Some(owner) = owner {
            command.arg(format!("EXOSNAP_DISTRIBUTION_OWNER={owner}"));
        }
        crate::tools::run(&mut command, secs(360.0))
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "runs only in the identity-bound disposable guest created by the MSI smoke host test"]
    fn msi_distribution_ownership_guest_smoke() -> Result<()> {
        let payload = PathBuf::from(crate::disposable::GUEST_ROOT).join("payload");
        let input: serde_json::Value =
            serde_json::from_slice(&fs::read(payload.join("msi-ownership-smoke.json"))?)?;
        let vm_id = input["vmId"]
            .as_str()
            .ok_or_else(|| anyhow::anyhow!("VM identity absent"))?;
        exo_guest::provision::assert_disposable_guest(vm_id)?;
        let out = PathBuf::from(crate::disposable::GUEST_ROOT).join("out");
        fs::create_dir_all(&out)?;
        let clean_before = super::super::clean::residue()?;
        assert!(
            clean_before.is_empty(),
            "the disposable image is not clean: {clean_before:?}"
        );
        assert!(!reg_key_exists(INSTALL_KEY)?);
        let install_root = PathBuf::from(
            std::env::var_os("ProgramFiles")
                .ok_or_else(|| anyhow::anyhow!("ProgramFiles unavailable"))?,
        )
        .join("ExoSnap");
        assert!(
            !install_root.exists(),
            "the disposable image contains an existing ExoSnap installation tree"
        );
        let packages = input["packages"]
            .as_array()
            .ok_or_else(|| anyhow::anyhow!("MSI inventory absent"))?;
        assert_eq!(packages.len(), 2);
        let paths: Vec<_> = packages
            .iter()
            .map(|p| PathBuf::from(p["path"].as_str().unwrap()))
            .collect();
        let codes: Vec<_> = packages
            .iter()
            .map(|p| p["productCode"].as_str().unwrap())
            .collect();
        assert!(codes.iter().all(|code| is_product_code(code)));
        assert_ne!(codes[0], codes[1]);
        assert_eq!(packages[0]["upgradeCode"], packages[1]["upgradeCode"]);
        for (package, path) in packages.iter().zip(&paths) {
            assert_eq!(sha256_file(path)?.0, package["sha256"].as_str().unwrap());
        }
        let minimum: Vec<u64> = crate::package::VC_REDIST_MIN_VERSION
            .trim_start_matches('v')
            .split('.')
            .map(str::parse)
            .collect::<std::result::Result<_, _>>()?;
        let runtime_ready =
            || vc_runtime_version().is_some_and(|(a, b, c, d)| vec![a, b, c, d] >= minimum);
        if !runtime_ready() {
            let runtime =
                PathBuf::from(input["runtime"]["path"].as_str().ok_or_else(|| {
                    anyhow::anyhow!("the guest needs the pinned Visual C++ runtime")
                })?);
            assert_eq!(sha256_file(&runtime)?.0, crate::package::VC_REDIST_SHA256);
            let installed = crate::tools::run(
                Command::new(runtime)
                    .args(["/install", "/quiet", "/norestart", "/log"])
                    .arg(out.join("runtime.log")),
                secs(360.0),
            )?;
            assert!(
                matches!(installed.code(), Some(0 | 3010)),
                "runtime install: {installed:?}"
            );
            assert!(
                runtime_ready(),
                "runtime install did not establish the MSI prerequisite"
            );
        }
        let mut results = Vec::new();
        let mut root = PathBuf::new();
        for (index, (name, package, caller, expected)) in [
            ("fresh-direct", 0, None, "direct"),
            ("upgrade-direct", 1, None, "direct"),
            ("explicit-winget", 0, Some("winget"), "winget"),
            ("preserve-winget", 1, None, "winget"),
            ("explicit-chocolatey", 0, Some("chocolatey"), "chocolatey"),
        ]
        .into_iter()
        .enumerate()
        {
            let log = out.join(format!("{name}.log"));
            let installed = smoke_msi(&paths[package], "/i", caller, &log)?;
            assert!(
                matches!(installed.code(), Some(0 | 3010)),
                "{name}: {installed:?}"
            );
            assert_eq!(
                reg_value("DistributionOwner")?.as_deref(),
                Some(expected),
                "{name}"
            );
            assert_eq!(reg_value("installed")?.as_deref(), Some("0x1"));
            root = PathBuf::from(
                reg_value("InstallPath")?.ok_or_else(|| anyhow::anyhow!("InstallPath absent"))?,
            );
            assert!(
                root.join("exosnap.exe").is_file() && root.join("exosnap-updater.exe").is_file()
            );
            let arp = exosnap_arp_rows()?;
            assert_eq!(arp.len(), 1, "{name}: stale product registration");
            assert!(
                arp[0]
                    .uninstall_string
                    .to_ascii_uppercase()
                    .contains(&codes[package].to_ascii_uppercase())
            );
            let mut previous = None;
            if index > 0 {
                let previous_code = codes[1 - package];
                assert!(!reg_key_exists(&format!(
                    r"HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{previous_code}"
                ))?);
                let text = crate::tools::read_diagnostic_log(&log, "msiexec")?;
                assert!(
                    text.contains("WIX_UPGRADE_DETECTED") && text.contains(previous_code),
                    "{name}: no real major upgrade observed"
                );
                previous = Some(previous_code);
            }
            results.push(json!({"scenario": name, "owner": expected, "exitCode": installed.code(),
                "productCode": codes[package], "removedProductCode": previous, "installPath": root}));
            fs::write(
                out.join("ownership.result.json"),
                serde_json::to_vec_pretty(&json!({
                    "vmId": vm_id, "sourceCommit": input["sourceCommit"], "scenarios": results,
                }))?,
            )?;
            println!("PASS {name}: {expected}");
        }
        let before_invalid = tree_hashes(&root)?;
        let invalid_log = out.join("invalid-owner.log");
        let invalid = smoke_msi(&paths[1], "/i", Some("invalid-owner"), &invalid_log)?;
        assert_eq!(invalid.code(), Some(1603));
        assert!(
            crate::tools::read_diagnostic_log(&invalid_log, "msiexec")?
                .contains("EXOSNAP_DISTRIBUTION_OWNER must be direct, winget or chocolatey.")
        );
        assert_eq!(
            reg_value("DistributionOwner")?.as_deref(),
            Some("chocolatey")
        );
        assert_eq!(tree_hashes(&root)?, before_invalid);
        let arp = exosnap_arp_rows()?;
        assert_eq!(arp.len(), 1);
        assert!(
            arp[0]
                .uninstall_string
                .to_ascii_uppercase()
                .contains(&codes[0].to_ascii_uppercase())
        );
        results.push(json!({"scenario": "invalid-owner", "exitCode": invalid.code(), "owner": "chocolatey", "treeUnchanged": true}));
        let removed = smoke_msi(&paths[0], "/x", None, &out.join("uninstall.log"))?;
        assert!(
            matches!(removed.code(), Some(0 | 3010)),
            "uninstall: {removed:?}"
        );
        assert!(!reg_key_exists(INSTALL_KEY)?);
        assert!(!root.exists());
        assert!(exosnap_arp_rows()?.is_empty());
        // Exercise a real change of ProductCode in the user context, retaining a
        // non-default destination. The second install deliberately supplies no path.
        let custom = out.join("user programs").join("ExoSnap");
        for (index, path) in paths.iter().enumerate() {
            let mut command = Command::new("msiexec.exe");
            command
                .arg("/i")
                .arg(path)
                .args(["/qn", "/norestart", "EXOSNAP_REQUESTED_SCOPE=user"]);
            if index == 0 {
                set_msi_install_folder(&mut command, custom.to_str().unwrap())?;
            }
            let log = out.join(format!("user-scope-{index}.log"));
            command.arg("/l*v").arg(&log);
            let result = crate::tools::run(&mut command, secs(360.0))?;
            assert!(
                matches!(result.code(), Some(0 | 3010)),
                "user install or upgrade: {result:?}"
            );
            assert!(custom.join("exosnap.exe").is_file());
            assert_eq!(
                scoped_reg_value(USER_KEY, "DistributionOwner")?.as_deref(),
                Some("direct")
            );
            assert!(reg_value("installed")?.is_none());
            assert_eq!(exosnap_arp_rows()?.len(), 1);
            if index == 0 {
                let original = tree_hashes(&custom)?;
                let wrong_product = crate::tools::run(
                    Command::new("msiexec.exe").arg("/i").arg(&paths[1]).args([
                        "/qn",
                        "/norestart",
                        "EXOSNAP_REQUESTED_SCOPE=user",
                        "ARPSYSTEMCOMPONENT=1",
                        "EXOSNAP_SETUP_REPAIR=1",
                        "REINSTALL=ALL",
                        "REINSTALLMODE=vamus",
                    ]),
                    secs(360.0),
                )?;
                assert!(
                    matches!(wrong_product.code(), Some(1603 | 1605)),
                    "repair must reject a different ProductCode: {wrong_product:?}"
                );
                assert_eq!(tree_hashes(&custom)?, original);
                assert_eq!(exosnap_arp_rows()?.len(), 1);
            }
        }
        let opposite = smoke_msi(&paths[0], "/i", None, &out.join("machine-conflict.log"))?;
        assert_eq!(opposite.code(), Some(1603));
        assert!(custom.join("exosnap.exe").is_file());
        let removed = crate::tools::run(
            Command::new("msiexec.exe")
                .arg("/x")
                .arg(&paths[1])
                .args(["/qn", "/norestart"]),
            secs(360.0),
        )?;
        assert!(matches!(removed.code(), Some(0 | 3010)));
        assert!(!custom.join("exosnap.exe").exists());
        assert!(scoped_reg_value(USER_KEY, "installed")?.is_none());
        assert!(exosnap_arp_rows()?.is_empty());
        let residue = super::super::clean::residue()?;
        assert!(residue.is_empty(), "uninstall residue: {residue:?}");
        results.push(json!({"scenario": "uninstall", "exitCode": removed.code(), "productMarkerRemoved": true, "residue": residue}));
        fs::write(
            out.join("ownership.result.json"),
            serde_json::to_vec_pretty(&json!({
                "vmId": vm_id, "sourceCommit": input["sourceCommit"], "packages": packages,
                "runtimeVersion": vc_runtime_version(), "scenarios": results, "passed": true,
            }))?,
        )?;
        println!("PASS invalid-owner, uninstall, clean-machine");
        Ok(())
    }

    #[cfg(windows)]
    fn installed_contexts(code: &str) -> Result<Vec<(u32, String)>> {
        #[link(name = "msi")]
        unsafe extern "system" {
            fn MsiGetProductInfoExW(
                code: *const u16,
                sid: *const u16,
                context: u32,
                property: *const u16,
                value: *mut u16,
                length: *mut u32,
            ) -> u32;
        }
        let wide = |text: &str| text.encode_utf16().chain([0]).collect::<Vec<_>>();
        let code = wide(code);
        let property = wide("VersionString");
        let mut installed = Vec::new();
        // USERMANAGED, USERUNMANAGED, MACHINE. A null SID selects the current user.
        for context in [1, 2, 4] {
            let mut value = [0u16; 64];
            let mut length = value.len() as u32;
            let status = unsafe {
                MsiGetProductInfoExW(
                    code.as_ptr(),
                    std::ptr::null(),
                    context,
                    property.as_ptr(),
                    value.as_mut_ptr(),
                    &mut length,
                )
            };
            if status == 1605 {
                continue;
            }
            anyhow::ensure!(status == 0, "query product context {context}: {status}");
            installed.push((context, String::from_utf16(&value[..length as usize])?));
        }
        Ok(installed)
    }

    #[cfg(windows)]
    fn build_rollback_fixture(output: &Path, out: &Path, version: &str) -> Result<PathBuf> {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let source = root.join("packaging/msi/Package.wxs");
        let production = fs::read_to_string(&source)?;
        anyhow::ensure!(production.matches("</Package>").count() == 1);
        anyhow::ensure!(production.contains("Schedule=\"afterInstallInitialize\""));
        let fixture_source = out.join("Rollback.wxs");
        fs::write(
            &fixture_source,
            production.replace(
                "</Package>",
                r#"<CustomAction Id="ForceFixtureRollback" Error="Deliberate rollback fixture failure." />
                   <InstallExecuteSequence>
                     <Custom Action="ForceFixtureRollback" After="RemoveExistingProducts" />
                   </InstallExecuteSequence>
                 </Package>"#,
            ),
        )?;
        let fields: Vec<_> = version.split('.').collect();
        let [major, minor, patch] = fields.as_slice() else {
            anyhow::bail!("expected a numeric three-field MSI ProductVersion, got {version}");
        };
        let major = major.parse::<u8>()?;
        let minor = minor.parse::<u8>()?;
        let patch = patch.parse::<u16>()?;
        let next = patch
            .checked_add(1)
            .ok_or_else(|| anyhow::anyhow!("no higher MSI patch version after {version}"))?;
        let higher = format!("{major}.{minor}.{next}");
        let fixture = out.join("rollback-fixture.msi");
        let wix = PathBuf::from(std::env::var("USERPROFILE")?).join(".dotnet/tools/wix.exe");
        let icon = root.join("app/assets/brand/exosnap-app.ico");
        let probe = root
            .join("build/windows-x64-ninja-release/packaging/msi")
            .join("exosnap-msi-owner-probe.dll");
        let floor = crate::package::VC_REDIST_MIN_VERSION.trim_start_matches('v');
        let built = crate::tools::run(
            Command::new(wix)
                .args(["build", "-arch", "x64", "-ext"])
                .arg("WixToolset.Util.wixext/4.0.5")
                .arg("-d")
                .arg(format!("ProductVersion={higher}"))
                .arg("-d")
                .arg(format!("VCRedistMinVersion={floor}"))
                .arg("-d")
                .arg(format!("AppIconPath={}", icon.display()))
                .arg("-d")
                .arg(format!("DistributionOwnerProbePath={}", probe.display()))
                .arg(&fixture_source)
                .arg(output.join("_harvest.wxs"))
                .arg("-o")
                .arg(&fixture),
            secs(360.0),
        )?;
        anyhow::ensure!(built.success(), "rollback fixture build: {built:?}");
        let receipt = json!({
            "productionWxsSha256": sha256_file(&source)?.0,
            "fixtureWxsSha256": sha256_file(&fixture_source)?.0,
            "fixtureMsiSha256": sha256_file(&fixture)?.0,
            "installedVersion": version,
            "attemptedVersion": higher,
            "injectedAction": "ForceFixtureRollback after RemoveExistingProducts",
        });
        let evidence = serde_json::to_vec_pretty(&receipt)?;
        fs::write(out.join("fixture.json"), evidence)?;
        Ok(fixture)
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "installs real packages on a disposable GitHub-hosted Windows runner"]
    fn hosted_installer_lifecycle() -> Result<()> {
        let runner = std::env::var("EXOSNAP_RUNNER_ENVIRONMENT")?;
        anyhow::ensure!(runner == "github-hosted");
        anyhow::ensure!(std::env::var("GITHUB_ACTIONS").as_deref() == Ok("true"));
        let output = PathBuf::from(std::env::var("EXOSNAP_INSTALLER_SMOKE")?);
        let report: serde_json::Value =
            serde_json::from_slice(&fs::read(output.join("package.json"))?)?;
        let artifact = |key: &str| -> Result<PathBuf> {
            let path = PathBuf::from(
                report[key]
                    .as_str()
                    .ok_or_else(|| anyhow::anyhow!("missing artifact: {key}"))?,
            );
            anyhow::ensure!(path.is_file() && path.parent() == Some(output.as_path()));
            Ok(path)
        };
        let installer = artifact("installer")?;
        let user_setup = artifact("setup")?;
        let machine_setup = artifact("setupAllUsers")?;
        let out = output.join("lifecycle");
        fs::create_dir(&out)?;
        let version = report["baseVersion"]
            .as_str()
            .ok_or_else(|| anyhow::anyhow!("package report has no baseVersion"))?;
        let rollback = build_rollback_fixture(&output, &out, version)?;
        let invoke = |program: &Path, args: &[&str], log: &str| -> Result<i32> {
            let mut command = Command::new(program);
            for &arg in args {
                if let Some(folder) = arg.strip_prefix("INSTALLFOLDER=") {
                    anyhow::ensure!(program == Path::new("msiexec.exe"));
                    set_msi_install_folder(&mut command, folder)?;
                } else {
                    command.arg(arg);
                }
            }
            command.arg(if program == Path::new("msiexec.exe") {
                "/l*v!"
            } else {
                "/log"
            });
            command.arg(out.join(format!("{log}.log")));
            let result = crate::tools::run(&mut command, secs(360.0))?;
            result
                .code()
                .ok_or_else(|| anyhow::anyhow!("{log}: process did not exit"))
        };
        let successful = |program: &Path, args: &[&str], log: &str| -> Result<()> {
            let code = invoke(program, args, log)?;
            anyhow::ensure!(matches!(code, 0 | 3010), "{log}: exit {code}");
            Ok(())
        };
        let msi = Path::new("msiexec.exe");
        let package = installer.to_str().unwrap();
        let local = PathBuf::from(std::env::var("LOCALAPPDATA")?);
        let program_files = PathBuf::from(std::env::var("ProgramFiles")?);
        let user_menu = PathBuf::from(std::env::var("APPDATA")?);
        let machine_menu = PathBuf::from(std::env::var("ProgramData")?);
        let mut results = Vec::new();
        for (scope, key, other_key, default, menu, setup, opposite) in [
            (
                "user",
                USER_KEY,
                INSTALL_KEY,
                local.join(r"Programs\ExoSnap"),
                user_menu,
                &user_setup,
                &machine_setup,
            ),
            (
                "machine",
                INSTALL_KEY,
                USER_KEY,
                program_files.join("ExoSnap"),
                machine_menu,
                &machine_setup,
                &user_setup,
            ),
        ] {
            let shortcut = menu.join(r"Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk");
            anyhow::ensure!(!reg_key_exists(key)? && !reg_key_exists(other_key)?);
            anyhow::ensure!(!reg_key_exists(r"HKLM\SOFTWARE\Codexo\ExoSnap")?);
            anyhow::ensure!(exosnap_arp_rows()?.is_empty());
            anyhow::ensure!(!default.exists() && !shortcut.exists());
            let custom = out.join(format!("{scope} custom")).join("ExoSnap");
            let request = format!("EXOSNAP_REQUESTED_SCOPE={scope}");
            let folder = format!("INSTALLFOLDER={}", custom.display());
            let log = |stage: &str| format!("{scope}-{stage}");
            successful(
                msi,
                &["/i", package, "/qn", "/norestart", &request, &folder],
                &log("install"),
            )?;
            let actual = scoped_reg_value(key, "InstallPath")?.unwrap();
            assert_eq!(Path::new(&actual), custom);
            let owner = scoped_reg_value(key, "DistributionOwner")?;
            assert_eq!(owner.as_deref(), Some("direct"));
            assert!(!reg_key_exists(other_key)? && shortcut.is_file());
            let rows = exosnap_arp_rows()?;
            assert_eq!(rows.len(), 1);
            assert!(!rows[0].system_component);
            let original = &rows[0];
            println!(
                "{scope} original ARP: {} version {}",
                original.registry_key, original.display_version
            );
            let original_arp = &rows[0].uninstall_string;
            let start = original_arp.find('{').unwrap();
            let code = &original_arp[start..start + 38];
            assert!(is_product_code(code));
            let expected_context = if scope == "user" { 2 } else { 4 };
            let expected = vec![(expected_context, version.to_string())];
            let contexts = installed_contexts(code)?;
            println!("{scope} original Windows Installer contexts: {contexts:?}");
            assert_eq!(contexts, expected);
            let expected_hive = if scope == "user" { "HKCU" } else { "HKLM" };
            assert_eq!(original.registry_key.split('\\').next(), Some(expected_hive));
            assert!(original.registry_key.ends_with(code));
            assert_eq!(original.display_version, version);
            let exe = custom.join("exosnap.exe");
            let hash = sha256_file(&exe)?.0;
            fs::remove_file(&exe)?;
            let repair = ["/fvamus", package, "/qn", "/norestart"];
            successful(msi, &repair, &log("repair"))?;
            assert_eq!(sha256_file(&exe)?.0, hash);
            fs::write(custom.join("unrelated.txt"), b"preserve")?;
            let upgrade = rollback.to_str().unwrap();
            let failed_upgrade = ["/i", upgrade, "/qn", "/norestart", &request];
            let status = invoke(msi, &failed_upgrade, &log("rollback"))?;
            assert_eq!(status, 1603);
            let rollback_log = out.join(format!("{scope}-rollback.log"));
            let trace = crate::tools::read_diagnostic_log(&rollback_log, "msiexec")?;
            let success = "RemoveExistingProducts. Return value 1.";
            let removed = trace.find(success).unwrap();
            assert!(trace[removed..].contains("Deliberate rollback fixture failure."));
            let upper_code = code.to_ascii_uppercase();
            let related = trace[..removed].lines().any(|line| {
                let identifies_product = line.to_ascii_uppercase().contains(&upper_code);
                identifies_product && line.contains("WIX_UPGRADE_DETECTED")
            });
            assert!(related, "missing original product in upgrade detection");
            let before = &trace[..removed];
            assert!(before.contains("Executing op: ProductUnregister("));
            assert!(before.lines().any(|line| {
                line.contains("Executing op: FileRemove(") && line.contains("FileName=exosnap.exe,")
            }));
            let failed = "ForceFixtureRollback. Return value 3.";
            let (_, after) = trace[removed..].split_once(failed).unwrap();
            let destination = format!("DestName={},", exe.display());
            assert!(after.lines().any(|line| {
                line.contains("Executing op: FileCopy(") && line.contains(&destination)
            }));
            let restored = exosnap_arp_rows()?;
            assert_eq!(restored.len(), 1);
            assert_eq!(restored[0].uninstall_string, *original_arp);
            assert!(!restored[0].system_component);
            println!(
                "{scope} restored ARP: {} version {}",
                restored[0].registry_key, restored[0].display_version
            );
            assert_eq!(restored[0].registry_key, original.registry_key);
            assert_eq!(restored[0].display_version, version);
            let contexts = installed_contexts(code)?;
            println!("{scope} restored Windows Installer contexts: {contexts:?}");
            assert_eq!(contexts, expected);
            let actual = scoped_reg_value(key, "InstallPath")?.unwrap();
            assert_eq!(Path::new(&actual), custom);
            let owner = scoped_reg_value(key, "DistributionOwner")?;
            assert_eq!(owner.as_deref(), Some("direct"));
            assert!(!reg_key_exists(other_key)? && shortcut.is_file());
            assert_eq!(sha256_file(&exe)?.0, hash);
            assert_eq!(fs::read(custom.join("unrelated.txt"))?, b"preserve");
            let uninstall = ["/x", package, "/qn", "/norestart"];
            successful(msi, &uninstall, &log("uninstall"))?;
            assert_eq!(fs::read(custom.join("unrelated.txt"))?, b"preserve");
            assert!(!exe.exists() && !shortcut.exists() && !reg_key_exists(key)?);
            assert!(exosnap_arp_rows()?.is_empty());

            let quiet_install = ["/install", "/quiet", "/norestart"];
            successful(setup, &quiet_install, &log("setup"))?;
            let exe = default.join("exosnap.exe");
            let hash = sha256_file(&exe)?.0;
            let actual = scoped_reg_value(key, "InstallPath")?.unwrap();
            assert_eq!(Path::new(&actual), default);
            assert!(shortcut.is_file() && !reg_key_exists(other_key)?);
            let rows = exosnap_arp_rows()?;
            assert_eq!(rows.iter().filter(|r| r.system_component).count(), 1);
            let visible: Vec<_> = rows.iter().filter(|r| !r.system_component).collect();
            assert_eq!(visible.len(), 1);
            let cached = quoted_exe(&visible[0].uninstall_string).unwrap();
            let cache_root = if scope == "user" {
                local.join("Package Cache")
            } else {
                PathBuf::from(std::env::var("ProgramData")?).join("Package Cache")
            };
            anyhow::ensure!(cached.starts_with(cache_root) && cached.is_file());
            let refused = invoke(opposite, &quiet_install, &log("conflict"))?;
            assert_eq!(refused, 1638);
            assert_eq!(sha256_file(&exe)?.0, hash);
            assert!(!reg_key_exists(other_key)?);
            fs::remove_file(&exe)?;
            let repair = ["/repair", "/quiet", "/norestart"];
            successful(&cached, &repair, &log("cached-repair"))?;
            assert_eq!(sha256_file(&exe)?.0, hash);
            let remove = ["/uninstall", "/quiet", "/norestart"];
            successful(&cached, &remove, &log("cached-remove"))?;
            assert!(!exe.exists() && !shortcut.exists() && !reg_key_exists(key)?);
            assert!(exosnap_arp_rows()?.is_empty());
            results.push(
                json!({"scope": scope, "msiCustomPath": custom, "setupPath": default,
                "repairedSha256": hash, "oppositeScopeExit": refused, "uninstalled": true,
                "rolledBackProductCode": code, "failedUpgradeExit": status}),
            );
        }
        let receipt = serde_json::to_vec_pretty(&results)?;
        fs::write(out.join("result.json"), receipt)?;
        Ok(())
    }

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
