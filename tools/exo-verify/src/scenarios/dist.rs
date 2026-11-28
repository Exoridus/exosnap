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
use crate::context::{App, Context, extract_zip};
use crate::package::{
    LAUNCHER_NAME, PORTABLE_CRT_FILES, RECORDER_NAME, RUNTIME_DIR, portable_dir_name,
    validate_tree, verify_msi_runtime_parity, verify_portable_config, verify_portable_crt,
    verify_staged_diagnostics, walk_files,
};
use crate::plan::Tier;
use crate::scenario::{Lane, Scenario, ScenarioClass, Step, Stop};
use crate::{infra_ensure, product_ensure};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            id: "dist.bundle-identity",
            revision: 1,
            title: "Candidate bundle names one version, one commit and both packages",
            class: ScenarioClass::Contract,
            contract: "the bundle's packages carry the candidate version in their names and the portable archive has exactly one top-level package directory",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[],
            timeout: secs(60.0),
            run: bundle_identity,
        },
        Scenario {
            id: "dist.portable-contents",
            revision: 4,
            title: "Portable package is complete, clean and self-contained",
            class: ScenarioClass::Contract,
            contract: "the portable tree carries the launcher, the default portable.toml, pinned app-local Visual C++ runtime, official signed diagnostics pair, required runtime files and licenses, the candidate's embedded version, and no unresolved imports or development or user data",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: secs(180.0),
            run: portable_contents,
        },
        Scenario {
            id: "dist.msi-matches-portable",
            revision: 4,
            title: "MSI installs byte-identical files to the portable package",
            class: ScenarioClass::Contract,
            contract: "the shared portable and MSI files match by path and bytes, and only the ZIP carries portable.toml and the pinned app-local Visual C++ runtime",
            lane: Lane::CiCore,
            also: &[],
            tier: Tier::Required,
            requires: &[Capability::Windows],
            timeout: secs(300.0),
            run: msi_matches_portable,
        },
        Scenario {
            id: "dist.portable-first-launch",
            revision: 3,
            title: "Portable package starts on a bare environment and reports the candidate identity",
            class: ScenarioClass::Contract,
            contract: "the extracted package contains its launcher and the recorder under runtime starts with only system directories on PATH, reports the candidate identity, creates the main window, quits cleanly, and leaves the user's real configuration untouched",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(120.0),
            run: portable_first_launch,
        },
        Scenario {
            id: "dist.updater-staged-launch",
            revision: 3,
            title: "The updater runs from the file subset the application stages",
            class: ScenarioClass::Contract,
            contract: "exosnap-updater.exe loads and renders from the runtime subset the application copies before a handoff, including its app-local Visual C++ runtime when present",
            lane: Lane::CiCore,
            also: &[Lane::Quick],
            tier: Tier::Required,
            requires: &[Capability::InteractiveDesktop],
            timeout: secs(60.0),
            run: updater_staged_launch,
        },
        Scenario {
            id: "dist.embedded-update-key",
            revision: 2,
            title: "The updater embeds the official update verification key",
            class: ScenarioClass::Contract,
            contract: "exosnap-updater.exe carries the configured official Ed25519 public key used to verify signed manifests",
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
    verify_portable_config(&root).map_err(|err| Stop::fail(format!("{err:#}")))?;
    let leaks = vec![
        r"C:\Users\".to_string(),
        ".workspace".into(),
        ".claude".into(),
        r"D:\a\".into(),
    ];
    let problems = validate_tree(&root, &version, &leaks)?;
    ctx.evidence.put("problems", json!(problems));
    product_ensure!(problems.is_empty(), "{}", problems.join("; "));
    if let Ok(key) = std::env::var("EXO_VERIFY_UPDATE_PUBLIC_KEY_HEX") {
        let public = crate::manifest::public_key_from_hex(&key).map_err(Stop::infra)?;
        verify_staged_diagnostics(&root, &public).map_err(|err| Stop::fail(format!("{err:#}")))?;
    } else if root
        .join(RUNTIME_DIR)
        .join(crate::diagnostics_pair::MANIFEST_NAME)
        .is_file()
    {
        return Err(Stop::unavailable(
            "EXO_VERIFY_UPDATE_PUBLIC_KEY_HEX is needed to verify the signed diagnostics pair",
        ));
    }
    let audit = verify_portable_crt(&root).map_err(|error| Stop::fail(format!("{error:#}")))?;
    ctx.evidence.put(
        "imports",
        json!({ "binaries": audit.binaries, "shipped": audit.shipped, "windows": audit.system, "msvcRuntime": audit.msvc }),
    );
    Ok(())
}

fn msi_matches_portable(ctx: &mut Context) -> Step {
    let portable = extracted_portable(ctx)?;
    let extracted = ctx.scenario_dir.join("msi");
    crate::package::msi_extract(&ctx.package(FileRole::Installer)?, &extracted)?;
    let files = verify_msi_runtime_parity(&portable, &extracted)
        .map_err(|err| Stop::fail(format!("{err:#}")))?;
    ctx.evidence.put("files", files);
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
    if ctx.bundle().is_ok() {
        product_ensure!(
            product.root.join(LAUNCHER_NAME).is_file()
                && product.exe == product.root.join(RUNTIME_DIR).join(RECORDER_NAME),
            "the candidate has no launcher and recorder at the expected paths"
        );
    }
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
    let mut client = match crate::control::Client::connect("LiveVerify", &run_id, secs(60.0)) {
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
    // The endpoint answers before QML has loaded; a UI that failed to load
    // only shows as a main window that never gets a native handle.
    client
        .request("ui.getState", json!({}), secs(30.0))?
        .map_err(|r| Stop::fail(format!("ui.getState was refused: {r}")))?;
    let windows = client
        .poll("windows.snapshot", json!({}), secs(30.0), |snapshot| {
            snapshot["windows"].as_array().is_some_and(|windows| {
                windows
                    .iter()
                    .any(|w| w["role"] == "main" && w["nativeWindowCreated"] == true)
            })
        })?
        .ok_or_else(|| Stop::fail("the main window got no native handle within 30 s"))?;
    ctx.evidence.put("windows", windows);
    let app = App {
        client,
        child,
        output: ctx.scenario_dir.join("output"),
        config: config.clone(),
        run_id,
    };
    app.close_gracefully(secs(30.0))?;
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
/// The updater is a Qt Quick application, so this mirrors both halves of
/// `UpdaterStagingFileList()` -- the Quick runtime DLLs and the QML import trees
/// (staged recursively, because a module is qmldir + plugin + its own runtime
/// files). No .qmltypes file is expected: that is tooling metadata which the
/// official deploy tree does not carry and the engine does not read.
const UPDATER_STAGING_FILES: &[&str] = &[
    "exosnap-updater.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Qml.dll",
    "Qt6QmlMeta.dll",
    "Qt6QmlModels.dll",
    "Qt6QmlWorkerScript.dll",
    "Qt6Network.dll",
    "Qt6OpenGL.dll",
    "Qt6Quick.dll",
    "Qt6QuickControls2.dll",
    "Qt6QuickControls2Basic.dll",
    "Qt6QuickControls2BasicStyleImpl.dll",
    "Qt6QuickControls2Impl.dll",
    "Qt6QuickTemplates2.dll",
    "Qt6QuickLayouts.dll",
    "Qt6QuickShapes.dll",
    "Qt6Svg.dll",
    "plugins/platforms/qwindows.dll",
    "qml/QtQuick/Controls/qmldir",
    "qml/QtQuick/Controls/qtquickcontrols2plugin.dll",
];

const UPDATER_STAGING_DIRS: &[&str] = &[
    "qml/QtQml",
    "qml/QtQuick/Controls/Basic",
    "qml/QtQuick/Controls/impl",
    "qml/QtQuick/Layouts",
    "qml/QtQuick/Shapes",
    "qml/QtQuick/Templates",
    "qml/QtQuick/Window",
];

/// Copies the updater's runtime subset out of `product_root` into `stage`, as
/// the application does before a handoff, and returns the staged updater.
pub(super) fn stage_updater(product_root: &Path, stage: &Path) -> Step<PathBuf> {
    let nested = product_root.join(RUNTIME_DIR);
    let source_root = if nested.join("exosnap-updater.exe").is_file() {
        &nested
    } else {
        product_root
    };
    for rel in UPDATER_STAGING_FILES {
        let src = source_root.join(rel);
        product_ensure!(
            src.is_file(),
            "the package lacks {rel}, which the application stages for its updater"
        );
        let dst = stage.join(rel);
        std::fs::create_dir_all(dst.parent().unwrap())?;
        std::fs::copy(&src, &dst)?;
    }
    for (name, _) in PORTABLE_CRT_FILES {
        let src = source_root.join(name);
        if src.exists() {
            std::fs::copy(src, stage.join(name))?;
        }
    }
    for rel_dir in UPDATER_STAGING_DIRS {
        let src_root = source_root.join(rel_dir);
        product_ensure!(
            src_root.is_dir(),
            "the package lacks {rel_dir}, which the application stages for its updater"
        );
        for file in walk_files(&src_root)? {
            let rel = file.strip_prefix(&src_root).unwrap_or(&file);
            let dst = stage.join(rel_dir).join(rel);
            std::fs::create_dir_all(dst.parent().unwrap())?;
            std::fs::copy(&file, &dst)?;
        }
    }
    std::fs::write(stage.join("qt.conf"), "[Paths]\nPlugins = plugins\n")?;
    Ok(stage.join("exosnap-updater.exe"))
}

fn updater_staged_launch(ctx: &mut Context) -> Step {
    suppress_error_dialogs();
    let product = ctx.product()?;
    let stage = ctx.scenario_dir.join("updater-stage");
    let mut command = Command::new(stage_updater(&product.root, &stage)?);
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

pub(super) fn contains(haystack: &[u8], needle: &[u8]) -> bool {
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
    let updater = &ctx.product()?.updater;
    let bytes = std::fs::read(updater)?;
    product_ensure!(
        contains(&bytes, &key),
        "{} does not embed the official update key",
        updater.display()
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stages_the_nested_updater_and_copies_existing_app_local_runtime() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("package");
        let runtime = root.join(RUNTIME_DIR);
        let stage = temp.path().join("updater");
        for rel in UPDATER_STAGING_FILES {
            let path = runtime.join(rel);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, rel).unwrap();
        }
        for rel in UPDATER_STAGING_DIRS {
            std::fs::create_dir_all(runtime.join(rel)).unwrap();
        }
        let present = PORTABLE_CRT_FILES[0].0;
        std::fs::write(runtime.join(present), b"fixture MSVC runtime").unwrap();
        let updater = stage_updater(&root, &stage).unwrap();
        assert_eq!(updater, stage.join("exosnap-updater.exe"));
        assert_eq!(
            std::fs::read(stage.join(present)).unwrap(),
            b"fixture MSVC runtime"
        );
        assert!(!stage.join(PORTABLE_CRT_FILES[1].0).exists());
        assert!(!stage.join(RUNTIME_DIR).exists());
    }

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
