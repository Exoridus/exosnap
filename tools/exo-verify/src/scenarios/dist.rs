//! Distribution integrity: the candidate's packages, judged as bytes and as
//! a first launch on a machine that has nothing but the package.

use serde_json::json;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use super::common::secs;
use crate::bundle::{FileRole, installer_name, portable_name, sha256_file};
use crate::capability::Capability;
use crate::context::{Context, extract_zip};
use crate::package::{portable_dir_name, validate_tree, walk_files};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "dist.bundle-identity",
            revision: 1,
            title: "Candidate bundle names one version, one commit and both packages",
            claim: "the bundle's packages carry the candidate version in their names and the portable archive has exactly one top-level package directory",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[],
            timeout: secs(60.0),
            run: bundle_identity,
        },
        Scenario {
            id: "dist.portable-contents",
            revision: 1,
            title: "Portable package is complete, clean and self-contained",
            claim: "the portable tree carries every runtime file and license, no development or user data, the candidate's embedded version, and no unresolved imports",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: secs(180.0),
            run: portable_contents,
        },
        Scenario {
            id: "dist.msi-matches-portable",
            revision: 1,
            title: "MSI installs byte-identical files to the portable package",
            claim: "every file of the portable package is inside the MSI with the same bytes, and the MSI carries no other binaries",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: secs(300.0),
            run: msi_matches_portable,
        },
        Scenario {
            id: "dist.portable-first-launch",
            revision: 1,
            title: "Portable package starts on a bare environment and reports the candidate identity",
            claim: "the extracted package starts with only system directories on PATH, reports the candidate's version, commit and executable hash, and leaves the user's real configuration untouched",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: portable_first_launch,
        },
        Scenario {
            id: "dist.updater-staged-launch",
            revision: 1,
            title: "The updater runs from the file subset the application stages",
            claim: "exosnap-updater.exe loads and renders from exactly the runtime subset the application copies before a handoff",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(60.0),
            run: updater_staged_launch,
        },
        Scenario {
            id: "dist.embedded-update-key",
            revision: 1,
            title: "Both executables embed the official update verification key",
            claim: "exosnap.exe and exosnap-updater.exe carry the configured official Ed25519 public key, so signed manifests verify in the field",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[],
            timeout: secs(60.0),
            run: embedded_update_key,
        },
    ]
}

fn bundle_identity(ctx: &mut Context) -> Step {
    let bundle = ctx.bundle()?;
    let version = bundle.inventory.product_version.clone();
    let files: BTreeMap<String, String> = bundle.inventory.packages();
    ctx.evidence.put("packages", json!(files));
    product_ensure!(
        files.contains_key(&installer_name(&version)),
        "no {} in the bundle",
        installer_name(&version)
    );
    product_ensure!(
        files.contains_key(&portable_name(&version)),
        "no {} in the bundle",
        portable_name(&version)
    );
    let top = portable_dir_name(&version);
    let zip = zip::ZipArchive::new(std::fs::File::open(ctx.package(FileRole::Portable)?)?)?;
    let strays: Vec<String> = zip
        .file_names()
        .filter(|n| !n.starts_with(&format!("{top}/")))
        .map(String::from)
        .collect();
    product_ensure!(
        strays.is_empty(),
        "portable entries outside {top}/: {}",
        strays.join(", ")
    );
    product_ensure!(
        zip.len() > 50,
        "the portable archive has only {} entries",
        zip.len()
    );
    Ok(())
}

/// Extracts the portable package once per scenario and returns its package root.
fn extracted_portable(ctx: &mut Context) -> Step<PathBuf> {
    let version = ctx.bundle()?.inventory.product_version.clone();
    let target = ctx.scenario_dir.join("portable");
    extract_zip(&ctx.package(FileRole::Portable)?, &target)?;
    let root = target.join(portable_dir_name(&version));
    product_ensure!(
        root.is_dir(),
        "the portable archive does not unpack to {}",
        portable_dir_name(&version)
    );
    Ok(root)
}

fn portable_contents(ctx: &mut Context) -> Step {
    let version = ctx.bundle()?.inventory.product_version.clone();
    let root = extracted_portable(ctx)?;
    let leaks = vec![
        r"C:\Users\".to_string(),
        ".workspace".into(),
        ".claude".into(),
        r"D:\a\".into(),
    ];
    let problems = validate_tree(&root, &version, &leaks)?;
    ctx.evidence.put("problems", json!(problems));
    product_ensure!(problems.is_empty(), "{}", problems.join("; "));
    let audit = crate::pe::audit_tree(&root)?;
    ctx.evidence.put(
        "imports",
        json!({ "binaries": audit.binaries, "shipped": audit.shipped, "windows": audit.system, "msvcRuntime": audit.msvc }),
    );
    product_ensure!(
        audit.unresolved.is_empty(),
        "unresolved runtime imports: {}",
        audit.unresolved.join(", ")
    );
    Ok(())
}

fn msi_root(extracted: &Path) -> Step<PathBuf> {
    let exe = walk_files(extracted)?
        .into_iter()
        .find(|p| {
            p.file_name()
                .is_some_and(|n| n.eq_ignore_ascii_case("exosnap.exe"))
        })
        .ok_or_else(|| Stop::fail("the MSI contains no exosnap.exe"))?;
    Ok(exe.parent().unwrap().to_path_buf())
}

fn tree_hashes(root: &Path) -> Step<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    for f in walk_files(root)? {
        let rel = f
            .strip_prefix(root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/")
            .to_ascii_lowercase();
        out.insert(rel, sha256_file(&f)?.0);
    }
    Ok(out)
}

fn msi_matches_portable(ctx: &mut Context) -> Step {
    let portable = extracted_portable(ctx)?;
    let extracted = ctx.scenario_dir.join("msi");
    crate::package::msi_extract(&ctx.package(FileRole::Installer)?, &extracted)?;
    // An administrative image also carries a copy of the MSI itself.
    let msi_root = msi_root(&extracted)?;
    let expected = tree_hashes(&portable)?;
    let actual = tree_hashes(&msi_root)?;
    let missing: Vec<&String> = expected
        .keys()
        .filter(|k| !actual.contains_key(*k))
        .collect();
    let differing: Vec<&String> = expected
        .iter()
        .filter(|(k, v)| actual.get(*k).is_some_and(|a| a != *v))
        .map(|(k, _)| k)
        .collect();
    let extra: Vec<&String> = actual
        .keys()
        .filter(|k| !expected.contains_key(*k) && (k.ends_with(".dll") || k.ends_with(".exe")))
        .collect();
    ctx.evidence.put("files", expected.len());
    product_ensure!(
        missing.is_empty(),
        "files of the portable package missing from the MSI: {missing:?}"
    );
    product_ensure!(
        differing.is_empty(),
        "files whose MSI bytes differ from the portable package: {differing:?}"
    );
    product_ensure!(
        extra.is_empty(),
        "binaries in the MSI but not in the portable package: {extra:?}"
    );
    Ok(())
}

/// The environment of a machine with nothing but the package: system
/// directories only, and none of the variables that let Qt find a developer's
/// installation. QML imports are resolved by the engine, not the DLL loader,
/// so a stray Qt on PATH can hide a missing module.
pub fn bare_environment(command: &mut Command) {
    let system_root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into());
    command.env(
        "PATH",
        format!(r"{system_root}\system32;{system_root};{system_root}\System32\Wbem"),
    );
    for var in [
        "QML_IMPORT_PATH",
        "QML2_IMPORT_PATH",
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QT_QUICK_CONTROLS_STYLE",
        "QT_DIR",
        "Qt6_DIR",
        "QT_QPA_PLATFORM",
    ] {
        command.env_remove(var);
    }
}

/// Makes a missing DLL an immediate exit code instead of a modal dialog that
/// would sit on screen until the scenario times out. Children inherit it.
pub fn suppress_error_dialogs() {
    #[cfg(windows)]
    unsafe {
        use windows::Win32::System::Diagnostics::Debug::{
            SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX, SEM_NOOPENFILEERRORBOX, SetErrorMode,
        };
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
}

pub const STATUS_DLL_NOT_FOUND: i32 = 0xC000_0135_u32 as i32;

fn dir_snapshot(dir: &Path) -> BTreeMap<String, (u64, std::time::SystemTime)> {
    let mut out = BTreeMap::new();
    if let Ok(files) = walk_files(dir) {
        for f in files {
            if let Ok(m) = std::fs::metadata(&f) {
                out.insert(
                    f.display().to_string(),
                    (m.len(), m.modified().unwrap_or(std::time::UNIX_EPOCH)),
                );
            }
        }
    }
    out
}

fn portable_first_launch(ctx: &mut Context) -> Step {
    suppress_error_dialogs();
    let product = ctx.product()?;
    let real_config =
        PathBuf::from(std::env::var("LOCALAPPDATA").unwrap_or_default()).join("ExoSnap");
    let before = dir_snapshot(&real_config);
    let run_id = crate::control::new_run_id("exov");
    let config = ctx.scenario_dir.join("config");
    std::fs::create_dir_all(&config)?;
    let mut command = Command::new(&product.exe);
    command
        .arg("--live-verify-control")
        .arg(&run_id)
        .current_dir(std::env::temp_dir())
        .env("EXOSNAP_CONFIG_DIR", &config)
        .env("EXOSNAP_OUTPUT_DIR", ctx.scenario_dir.join("output"));
    bare_environment(&mut command);
    let mut child = ctx.spawn(&mut command)?;
    let client = match crate::control::Client::connect("LiveVerify", &run_id, secs(60.0)) {
        Ok(c) => c,
        Err(e) => {
            return Err(
                match child.try_wait().ok().flatten().and_then(|s| s.code()) {
                    Some(STATUS_DLL_NOT_FOUND) => Stop::fail(
                        "exosnap.exe could not load a required DLL (STATUS_DLL_NOT_FOUND)",
                    ),
                    Some(code) => Stop::fail(format!(
                        "exosnap.exe exited with {code:#x} before its control endpoint came up"
                    )),
                    None => Stop::Infra(e),
                },
            );
        }
    };
    let identity = client.identity.clone();
    ctx.evidence.put("identity", identity.clone());
    let (exe_sha, _) = sha256_file(&product.exe)?;
    if let Ok(bundle) = ctx.bundle() {
        let inv = &bundle.inventory;
        product_ensure!(
            identity["productVersion"] == inv.product_version.as_str(),
            "the product reports version {}, the candidate is {}",
            identity["productVersion"],
            inv.product_version
        );
        product_ensure!(
            identity["commit"]
                .as_str()
                .is_some_and(|c| c.eq_ignore_ascii_case(&inv.source_commit)),
            "the product reports commit {}, the candidate was built from {}",
            identity["commit"],
            inv.source_commit
        );
        product_ensure!(
            identity["officialBuild"] == true,
            "the candidate is not an official build"
        );
        product_ensure!(
            identity["dirtySourceTree"] != true,
            "the candidate reports a dirty source tree"
        );
    }
    product_ensure!(
        identity["executableSha256"]
            .as_str()
            .is_some_and(|s| s.eq_ignore_ascii_case(&exe_sha)),
        "the running executable hashes to {}, the packaged file to {exe_sha}",
        identity["executableSha256"]
    );
    drop(client);
    let _ = child.kill();
    let _ = child.wait();
    let after = dir_snapshot(&real_config);
    product_ensure!(
        before == after,
        "the user's real configuration under {} changed",
        real_config.display()
    );
    ctx.evidence
        .put("isolatedConfigFiles", walk_files(&config)?.len());
    Ok(())
}

/// Must match the application's staging list: the updater never runs in place.
const UPDATER_STAGING: &[&str] = &[
    "exosnap-updater.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "plugins/platforms/qwindows.dll",
];

fn updater_staged_launch(ctx: &mut Context) -> Step {
    suppress_error_dialogs();
    let product = ctx.product()?;
    let stage = ctx.scenario_dir.join("updater-stage");
    for rel in UPDATER_STAGING {
        let src = product.root.join(rel);
        product_ensure!(
            src.is_file(),
            "the package lacks {rel}, which the application stages for its updater"
        );
        let dst = stage.join(rel);
        std::fs::create_dir_all(dst.parent().unwrap())?;
        std::fs::copy(&src, &dst)?;
    }
    std::fs::write(stage.join("qt.conf"), "[Paths]\nPlugins = plugins\n")?;
    let mut command = Command::new(stage.join("exosnap-updater.exe"));
    command
        .args(["--preview-state", "progress", "--preview-smoke"])
        .current_dir(&stage);
    bare_environment(&mut command);
    let mut child = ctx.spawn(&mut command)?;
    let status = crate::tools::wait(&mut child, Duration::from_secs(20))
        .map_err(|_| Stop::fail("the staged updater did not close itself after its preview"))?;
    match status.code() {
        Some(0) => Ok(()),
        Some(STATUS_DLL_NOT_FOUND) => Err(Stop::fail(
            "the staged updater could not load a DLL (STATUS_DLL_NOT_FOUND)",
        )),
        other => Err(Stop::fail(format!(
            "the staged updater exited with {other:?}"
        ))),
    }
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|w| w == needle)
}

fn embedded_update_key(ctx: &mut Context) -> Step {
    let Ok(hex_key) = std::env::var("EXO_VERIFY_UPDATE_PUBLIC_KEY_HEX") else {
        return Err(Stop::unavailable(
            "EXO_VERIFY_UPDATE_PUBLIC_KEY_HEX (the official public key) is not configured",
        ));
    };
    let key = hex::decode(hex_key.trim())
        .map_err(|_| Stop::infra("EXO_VERIFY_UPDATE_PUBLIC_KEY_HEX is not hex"))?;
    infra_ensure!(
        key.len() == 32 && key.iter().any(|b| *b != 0),
        "the configured public key is not a real 32-byte key"
    );
    let product = ctx.product()?;
    for exe in [&product.exe, &product.updater] {
        let bytes = std::fs::read(exe)?;
        product_ensure!(
            contains(&bytes, &key),
            "{} does not embed the official update key",
            exe.display()
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_search_finds_only_the_exact_bytes() {
        let key = [7u8; 32];
        let mut image = vec![0u8; 1000];
        image[500..532].copy_from_slice(&key);
        assert!(contains(&image, &key));
        image[510] = 8;
        assert!(!contains(&image, &key));
    }
}
