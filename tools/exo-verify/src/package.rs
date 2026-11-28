//! Packaging: the CMake install tree becomes the portable ZIP and the MSI.
//!
//! Both packages are produced from the same pruned staging tree. The release
//! lanes check that their runtime paths and bytes match and work on a machine.

use anyhow::{Context, Result, bail, ensure};
use ed25519_dalek::VerifyingKey;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use crate::{bundle::sha256_file, diagnostics_pair, manifest, pe};

mod dependency_identity;
mod footprint;

pub const PLATFORM: &str = "windows-x64";

#[derive(clap::Args)]
pub struct PackageArgs {
    /// Configured and built CMake Release tree.
    #[arg(
        long,
        required_unless_present = "prepared_dir",
        conflicts_with = "prepared_dir"
    )]
    pub build_dir: Option<PathBuf>,
    /// Export the unsigned package inputs for signing and packaging in later jobs.
    #[arg(long, conflicts_with_all = ["prepared_dir", "diagnostics_pair_dir", "launch_smoke"])]
    pub prepare_only: bool,
    /// Read the prepared package inputs without accessing a build directory.
    #[arg(long, requires = "diagnostics_pair_dir", conflicts_with = "vc_redist")]
    pub prepared_dir: Option<PathBuf>,
    /// Output directory (must be empty or absent).
    #[arg(long)]
    pub out: PathBuf,
    /// The version compiled into the binaries. Defaults to the prepared version or project version with `-dev`.
    #[arg(long)]
    pub version: Option<String>,
    #[arg(long, default_value = ".")]
    pub repo_root: PathBuf,
    /// Skip the MSI (requires the WiX CLI otherwise).
    #[arg(long)]
    pub skip_msi: bool,
    /// Directory containing the detached diagnostics pair manifest and signature.
    #[arg(long, requires = "diagnostics_public_key_hex")]
    pub diagnostics_pair_dir: Option<PathBuf>,
    /// Official Ed25519 public key embedded in the product, hex.
    #[arg(long, requires = "diagnostics_pair_dir")]
    pub diagnostics_public_key_hex: Option<String>,
    /// The pinned Microsoft Visual C++ x64 redistributable to embed in the
    /// offline Setup bootstrapper. Its SHA-256 is verified against the pin in
    /// this module before anything is built.
    #[arg(long)]
    pub vc_redist: Option<PathBuf>,
    /// Start the staged exosnap.exe with --smoke-test in an environment that
    /// hides every developer Qt, so a package that only runs with the
    /// build machine's Qt on PATH fails here.
    #[arg(long)]
    pub launch_smoke: bool,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackageResult {
    pub version: String,
    pub base_version: String,
    pub portable: PathBuf,
    pub installer: Option<PathBuf>,
    pub setup: Option<PathBuf>,
    pub staging: PathBuf,
    pub file_count: usize,
    pub dependency_audit: BTreeMap<String, usize>,
    /// Upstream identity of every third-party component in the package.
    pub dependencies: Vec<dependency_identity::DependencyEntry>,
}

/// The exact Microsoft Visual C++ 2015-2022 x64 redistributable embedded in
/// the offline Setup bootstrapper. The link is Microsoft's supported stable
/// alias; the digest pins the bytes so a silently updated alias fails the
/// build instead of changing what users install. Refreshing the pin is a
/// deliberate release change.
pub const VC_REDIST_URL: &str = "https://aka.ms/vs/17/release/vc_redist.x64.exe";
pub const VC_REDIST_SHA256: &str =
    "cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b";
/// The runtime floor the product's toolset requires. Burn skips the embedded
/// redistributable only when the installed runtime meets or exceeds it, and the
/// raw MSI's file-version search refuses to install below it; both read this
/// one value.
pub const VC_REDIST_MIN_VERSION: &str = "v14.44.35211.0";

/// The floor without Burn's `v` prefix, which is the form the Windows
/// Installer's `FileSearch/@MinVersion` expects.
fn vc_redist_floor() -> &'static str {
    VC_REDIST_MIN_VERSION.trim_start_matches(['v', 'V'])
}

pub fn portable_dir_name(version: &str) -> String {
    format!("ExoSnap-{version}-{PLATFORM}-portable")
}

pub fn project_version(repo_root: &Path) -> Result<String> {
    let text = fs::read_to_string(repo_root.join("CMakeLists.txt"))?;
    let re =
        regex::Regex::new(r"project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)").unwrap();
    Ok(re
        .captures(&text)
        .context("could not parse project(exosnap VERSION x.y.z) from CMakeLists.txt")?[1]
        .to_string())
}

fn external_path(path: &Path) -> PathBuf {
    let Some(text) = path.to_str() else {
        return path.to_path_buf();
    };
    if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
        PathBuf::from(format!(r"\\{rest}"))
    } else if let Some(rest) = text.strip_prefix(r"\\?\") {
        PathBuf::from(rest)
    } else {
        path.to_path_buf()
    }
}

/// Files and directories every runtime tree must carry. Qt Quick modules are
/// load-bearing: without them the process starts and dies at QML load, which
/// no compile or link step can catch.
pub(crate) const LAUNCHER_NAME: &str = "ExoSnap.exe";
pub(crate) const RUNTIME_DIR: &str = "runtime";
pub(crate) const RECORDER_NAME: &str = "exosnap.exe";
pub(crate) const PORTABLE_CONFIG_NAME: &str = "portable.toml";
pub(crate) const PORTABLE_CONFIG_DEFAULT: &str = "data_path = \"./data\"\n";

const REQUIRED_ROOT_FILES: &[&str] = &[
    LAUNCHER_NAME,
    "licenses/LICENSE",
    "licenses/THIRD_PARTY_NOTICES.md",
    "docs/KNOWN_LIMITATIONS.md",
    "docs/README-PORTABLE.md",
];

const REQUIRED_RUNTIME_FILES: &[&str] = &[
    "exosnap.exe",
    "exosnap-updater.exe",
    "SDL3.dll",
    "qt.conf",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6Svg.dll",
    "Qt6Xml.dll",
    "Qt6Qml.dll",
    "Qt6QmlModels.dll",
    "Qt6Network.dll",
    "Qt6Quick.dll",
    "Qt6QuickControls2.dll",
    "Qt6QuickTemplates2.dll",
    "Qt6QuickLayouts.dll",
    "Qt6QuickShapes.dll",
    "Qt6QuickDialogs2.dll",
    "Qt6QuickEffects.dll",
    "Qt6Quick3D.dll",
    "Qt6Quick3DRuntimeRender.dll",
    "Qt6Quick3DUtils.dll",
    // A QML module directory without its qmldir cannot be resolved by the engine.
    "qml/QtQuick/qmldir",
    "qml/QtQuick/Controls/qmldir",
    "qml/QtQml/qmldir",
];

const REQUIRED_DIRS: &[&str] = &[
    "plugins/platforms",
    "qml/QtQuick",
    "qml/QtQuick/Controls",
    "qml/QtQuick/Controls/Basic",
    "qml/QtQuick/Controls/impl",
    "qml/QtQuick/Dialogs",
    "qml/QtQuick/Shapes",
    "qml/QtQml",
    // The countdown overlay's MultiEffect; a missing module fails inside a
    // capture-excluded overlay, where nobody can observe it.
    "qml/QtQuick/Effects",
    // The production Controller3D scene loads this module for the native gamepad view.
    "qml/QtQuick3D",
];

/// Kept in step with THIRD_PARTY_NOTICES.md: the package never ships a
/// component the notices do not describe, or the reverse.
const REQUIRED_LICENSES: &[&str] = &[
    "spdlog.txt",
    "nlohmann_json.txt",
    "tomlplusplus.txt",
    "opus.txt",
    "flac.txt",
    "rnnoise.txt",
    "libebml.txt",
    "libmatroska.txt",
    "qt.txt",
    "ibm-plex-mono.txt",
    "hanken-grotesk.txt",
    "ffmpeg.txt",
    "presentmon.txt",
    "miniz.txt",
    "monocypher.txt",
    "sdl3.txt",
    "gamepad-artwork.txt",
    "gamepad-al2009man.txt",
    "gamepad-al2009man-notice.txt",
    "controller-models-larf.txt",
    "controller-models-3dco-plus.txt",
];

const CRASH_CAPTURE_LICENSES: &[&str] = &["sentry-native.txt", "crashpad.txt", "mini_chromium.txt"];

const FORBIDDEN_EXTENSIONS: &[&str] = &[
    "pdb", "ilk", "exp", "lib", "obj", "pch", "h", "hpp", "cpp", "c", "cmake", "pc", "suo", "user",
    "vcxproj", "sln", "log", "tmp",
];

const FORBIDDEN_NAMES: &[&str] = &[
    "recording-history.json",
    "settings.ini",
    "presets.ini",
    "presets.toml",
];

/// Executables no current tree can produce; their presence means a stale
/// build directory was packaged.
const FORBIDDEN_EXECUTABLES: &[&str] = &[
    "exosnap_quick_spike.exe",
    "exosnap_widgets_legacy.exe",
    "exosnap-diagnostics.exe",
];
pub(crate) const DIAGNOSTICS_HELPER: &str = "exosnap-helper.exe";
const DIAGNOSTICS_ARTIFACTS: &[&str] = &[
    DIAGNOSTICS_HELPER,
    diagnostics_pair::MANIFEST_NAME,
    diagnostics_pair::SIGNATURE_NAME,
];
// The x64 release files in the MSVC 14.44.35211.0 redistributable toolchain.
pub(crate) const PORTABLE_CRT_FILES: &[(&str, &str)] = &[
    (
        "msvcp140.dll",
        "0f885b509a685d2bbfa652fed26b5fb31d88fbdab0a978c641d1c7b8aa460aa9",
    ),
    (
        "msvcp140_1.dll",
        "bfad5aef4c63a669e3c140655cdfdf395b6c979b400a447bd5dcb65ed8826c3d",
    ),
    (
        "msvcp140_2.dll",
        "3ea06f0ee098b4823cb79599df3780e7f23cce52c19aac31d2a0d47efe33a5e9",
    ),
    (
        "msvcp140_atomic_wait.dll",
        "640b2aefced484d0368eea5bdd06addd0658a3a70a49256e560d6923b404a479",
    ),
    (
        "vcruntime140.dll",
        "d5e4d9a3e835fa679450145d6a7d94e36573a509317111904d9b3712c30d9066",
    ),
    (
        "vcruntime140_1.dll",
        "1f2d41c4aa5db0bc33ebf7b66d72943a817d7ce6cbe880502a9403823633093f",
    ),
];
const PREPARED_INVENTORY: &str = "package-inputs.json";
const SETUP_RUNTIME: &str = "setup-runtime";
const SETUP_BOOTSTRAPPER: &str = "exosnap-setup-ba.dll";
const MSI_OWNER_PROBE: &str = "exosnap-msi-owner-probe.dll";
const SETUP_REDIST: &str = "vc_redist.x64.exe";

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct PreparedInventory {
    version: String,
    files: BTreeMap<String, String>,
}

struct SetupInputs {
    bootstrapper: PathBuf,
    runtime: PathBuf,
    vc_redist: PathBuf,
}

struct InstallerInputs {
    owner_probe: PathBuf,
    setup: Option<SetupInputs>,
}

const FORBIDDEN_DIRS: &[&str] = &[
    ".git",
    ".github",
    ".workspace",
    ".claude",
    // Upstream Crashpad's own install rules drop a second handler and the WER
    // shim here; the application resolves only the root handler. The
    // redistributable belongs to Burn and the package managers, never to the
    // application tree.
    "bin",
    "include",
    "lib",
    "src",
    "tests",
    "CMakeFiles",
    "Testing",
    "plugins/qmltooling",
    "qml/QtTest",
    // ExoSnap selects the Basic style explicitly; every other Controls style
    // and the native-style module are dead weight plus attack surface.
    "qml/QtQuick/Controls/Fusion",
    "qml/QtQuick/Controls/Imagine",
    "qml/QtQuick/Controls/Material",
    "qml/QtQuick/Controls/Universal",
    "qml/QtQuick/Controls/FluentWinUI3",
    "qml/QtQuick/Controls/Windows",
    "qml/QtQuick/NativeStyle",
];

/// File-name fragments no shipping runtime tree may contain, wherever a Qt
/// deployment decides to place them. This is the wildcard half of
/// [`FORBIDDEN_DIRS`]: a future Qt layout that flattens a style plugin next to
/// the executables is caught here instead of shipping silently.
const FORBIDDEN_NAME_FRAGMENTS: &[&str] = &[
    "gameinputredist",
    "gameinputrawinputproxy",
    "vc_redist",
    "qmldbg_",
    "qtquickcontrols2fusion",
    "qtquickcontrols2imagine",
    "qtquickcontrols2material",
    "qtquickcontrols2universal",
    "qtquickcontrols2fluentwinui3",
    "qtquickcontrols2windowsstyle",
    "qtquickcontrols2nativestyle",
];

pub fn walk_files(root: &Path) -> Result<Vec<PathBuf>> {
    let mut out = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for entry in fs::read_dir(&dir)
            .with_context(|| format!("read {}", dir.display()))?
            .flatten()
        {
            let path = entry.path();
            if entry.file_type()?.is_dir() {
                stack.push(path);
            } else {
                out.push(path);
            }
        }
    }
    out.sort();
    Ok(out)
}

fn relative(root: &Path, path: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .to_string_lossy()
        .replace('\\', "/")
}

fn diagnostics_artifact(path: &Path) -> Option<&'static str> {
    let name = path.file_name()?;
    DIAGNOSTICS_ARTIFACTS
        .iter()
        .copied()
        .find(|leaf| name.eq_ignore_ascii_case(leaf))
}

fn stage_diagnostics_helper(staging: &Path, helper: &Path) -> Result<()> {
    for path in walk_files(staging)? {
        ensure!(
            diagnostics_artifact(&path).is_none(),
            "unexpected diagnostics artifact in the CMake install tree: {}",
            relative(staging, &path)
        );
    }
    let staged_helper = staging.join(RUNTIME_DIR).join(DIAGNOSTICS_HELPER);
    fs::copy(helper, &staged_helper).with_context(|| {
        format!(
            "stage the Release diagnostics helper from {}",
            helper.display()
        )
    })?;
    Ok(())
}

fn stage_diagnostics_pair(staging: &Path, pair_dir: &Path, public: &VerifyingKey) -> Result<()> {
    let runtime = staging.join(RUNTIME_DIR);
    for leaf in [
        diagnostics_pair::MANIFEST_NAME,
        diagnostics_pair::SIGNATURE_NAME,
    ] {
        let source = pair_dir.join(leaf);
        let destination = runtime.join(leaf);
        ensure!(
            !destination.exists(),
            "unexpected diagnostics artifact: {leaf}"
        );
        fs::copy(&source, destination).with_context(|| format!("stage {}", source.display()))?;
    }
    verify_staged_diagnostics(staging, public)
}

pub fn verify_staged_diagnostics(root: &Path, public: &VerifyingKey) -> Result<()> {
    let runtime = root.join(RUNTIME_DIR);
    diagnostics_pair::verify_in_directory(
        &runtime,
        &runtime.join(RECORDER_NAME),
        &runtime.join(DIAGNOSTICS_HELPER),
        public,
    )
}

/// Structural validation of a runtime tree. Returns every problem found; an
/// empty list means the tree is shippable as far as its contents can tell.
pub fn validate_tree(root: &Path, version: &str, leak_patterns: &[String]) -> Result<Vec<String>> {
    let mut errors = Vec::new();
    let runtime = root.join(RUNTIME_DIR);
    for f in REQUIRED_ROOT_FILES {
        if !root.join(f).is_file() {
            errors.push(format!("missing required file: {f}"));
        }
    }
    for f in REQUIRED_RUNTIME_FILES
        .iter()
        .copied()
        .chain([DIAGNOSTICS_HELPER])
    {
        if !runtime.join(f).is_file() {
            errors.push(format!("missing required file: {RUNTIME_DIR}/{f}"));
        }
    }
    if runtime.join(diagnostics_pair::MANIFEST_NAME).is_file()
        != runtime.join(diagnostics_pair::SIGNATURE_NAME).is_file()
    {
        errors.push("incomplete diagnostics pair in package".into());
    }
    for d in REQUIRED_DIRS {
        if !runtime.join(d).is_dir() {
            errors.push(format!("missing required directory: {RUNTIME_DIR}/{d}"));
        }
    }
    for lic in REQUIRED_LICENSES {
        if !root.join("licenses").join(lic).is_file() {
            errors.push(format!("missing third-party license: licenses/{lic}"));
        }
    }
    if runtime.join("crashpad_handler.exe").is_file() {
        for lic in CRASH_CAPTURE_LICENSES {
            if !root.join("licenses").join(lic).is_file() {
                errors.push(format!(
                    "crash capture ships but its license is missing: licenses/{lic}"
                ));
            }
        }
    }
    for d in FORBIDDEN_DIRS {
        if root.join(d).is_dir() || runtime.join(d).is_dir() {
            errors.push(format!("forbidden directory in package: {d}/"));
        }
    }
    for file in walk_files(root)? {
        let rel = relative(root, &file);
        let name = file
            .file_name()
            .unwrap()
            .to_string_lossy()
            .to_ascii_lowercase();
        let ext = file
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        if FORBIDDEN_EXTENSIONS.contains(&ext.as_str()) {
            errors.push(format!("development file in package: {rel}"));
        }
        if FORBIDDEN_NAMES.contains(&name.as_str()) {
            errors.push(format!("user-data file in package: {rel}"));
        }
        if FORBIDDEN_EXECUTABLES.contains(&name.as_str()) {
            errors.push(format!("superseded executable in package: {rel}"));
        }
        if let Some(leaf) = diagnostics_artifact(&file)
            && file != runtime.join(leaf)
        {
            errors.push(format!("misplaced diagnostics artifact: {rel}"));
        }
        if is_binary(&name) && !file.starts_with(&runtime) && rel != LAUNCHER_NAME {
            errors.push(format!("binary outside {RUNTIME_DIR}/: {rel}"));
        }
        if let Some(fragment) = FORBIDDEN_NAME_FRAGMENTS
            .iter()
            .find(|fragment| name.contains(**fragment))
        {
            errors.push(format!(
                "forbidden runtime payload in package ({fragment}): {rel}"
            ));
        }
        if name == "qt6quicktest.dll" || name == "qt6test.dll" {
            errors.push(format!("Qt test framework in package: {rel}"));
        }
        if is_debug_qt(&name) {
            errors.push(format!("debug Qt DLL in package: {rel}"));
        }
        if is_debug_msvc(&name) {
            errors.push(format!("debug MSVC runtime in package: {rel}"));
        }
        if matches!(ext.as_str(), "md" | "txt" | "json" | "ini" | "conf")
            && let Ok(text) = fs::read_to_string(&file)
        {
            for pattern in leak_patterns.iter().filter(|p| !p.is_empty()) {
                if text.to_lowercase().contains(&pattern.to_lowercase()) {
                    errors.push(format!("path or identity leak '{pattern}' in {rel}"));
                }
            }
        }
    }
    let base = version.split('-').next().unwrap_or(version);
    #[cfg(windows)]
    for exe in [
        LAUNCHER_NAME,
        "runtime/exosnap.exe",
        "runtime/exosnap-updater.exe",
    ] {
        let path = root.join(exe);
        if !path.is_file() {
            continue;
        }
        match crate::win::version_strings(&path) {
            Ok(info) => {
                let get = |k: &str| info.get(k).map(String::as_str).unwrap_or("");
                if get("ProductVersion") != version {
                    errors.push(format!(
                        "{exe} carries ProductVersion '{}', not '{version}'; the build tree was configured for another release identity",
                        get("ProductVersion")
                    ));
                }
                if get("FileVersion") != format!("{base}.0") {
                    errors.push(format!(
                        "{exe} FileVersion '{}' is not '{base}.0'",
                        get("FileVersion")
                    ));
                }
                if get("ProductName") != "ExoSnap" {
                    errors.push(format!(
                        "{exe} ProductName '{}' is not 'ExoSnap'",
                        get("ProductName")
                    ));
                }
            }
            Err(e) => errors.push(format!("{exe}: {e:#}")),
        }
    }
    for document in ["docs/KNOWN_LIMITATIONS.md", "docs/README-PORTABLE.md"] {
        if let Ok(text) = fs::read_to_string(root.join(document))
            && !text.contains(base)
        {
            errors.push(format!("{document} does not name version {base}"));
        }
    }
    Ok(errors)
}

/// Qt debug builds append `d` to the module name; `Quick3D` is a release exception.
fn is_debug_qt(lower_name: &str) -> bool {
    let lower_name = lower_name.to_ascii_lowercase();
    lower_name.starts_with("qt6")
        && lower_name.ends_with("d.dll")
        && !lower_name.ends_with("3d.dll")
}

fn run_checked(command: &mut Command, what: &str, timeout: Duration) -> Result<()> {
    let out = crate::tools::run(command, timeout).with_context(|| what.to_string())?;
    if !out.status.success() {
        let tail: Vec<&str> = out
            .stdout
            .lines()
            .chain(out.stderr.lines())
            .rev()
            .take(40)
            .collect();
        bail!(
            "{what} failed ({}):\n{}",
            out.status,
            tail.into_iter().rev().collect::<Vec<_>>().join("\n")
        );
    }
    Ok(())
}

fn remove_if_exists(path: &Path) -> Result<()> {
    if path.is_dir() {
        fs::remove_dir_all(path)?;
    } else if path.is_file() {
        fs::remove_file(path)?;
    }
    Ok(())
}

/// Writes `root` as a ZIP whose single top-level directory is `top`.
pub fn write_zip(root: &Path, top: &str, archive: &Path) -> Result<()> {
    let file = fs::File::create(archive)?;
    let mut zip = zip::ZipWriter::new(file);
    let options = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    for path in walk_files(root)? {
        zip.start_file(format!("{top}/{}", relative(root, &path)), options)?;
        let mut source = fs::File::open(&path)?;
        std::io::copy(&mut source, &mut zip)?;
    }
    zip.finish()?.flush()?;
    Ok(())
}

fn wix_id(path: &str) -> String {
    let mut id: String = path
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '_' || c == '.' {
                c
            } else {
                '_'
            }
        })
        .collect();
    if id.starts_with(|c: char| c.is_ascii_digit()) {
        id.insert(0, '_');
    }
    id
}

fn xml_attr(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('"', "&quot;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

/// A WiX v4 fragment listing every staged file, referenced from Package.wxs
/// as the `StagingFiles` component group. Generated because WiX v4 has no
/// harvest command and no `<Files>` glob.
pub fn harvest_fragment(root: &Path) -> Result<String> {
    let files = walk_files(root)?;
    let mut by_dir: BTreeMap<String, Vec<PathBuf>> = BTreeMap::new();
    let mut dirs: BTreeSet<String> = BTreeSet::new();
    for f in &files {
        let rel = relative(root, f);
        let dir = rel
            .rsplit_once('/')
            .map(|(d, _)| d.to_string())
            .unwrap_or_default();
        let mut d = dir.clone();
        while !d.is_empty() {
            dirs.insert(d.clone());
            d = d
                .rsplit_once('/')
                .map(|(p, _)| p.to_string())
                .unwrap_or_default();
        }
        by_dir.entry(dir).or_default().push(f.clone());
    }
    fn children<'a>(parent: &str, dirs: &'a BTreeSet<String>) -> Vec<&'a String> {
        dirs.iter()
            .filter(|d| {
                let p = d.rsplit_once('/').map(|(p, _)| p).unwrap_or("");
                p == parent
            })
            .collect()
    }
    fn emit(parent: &str, dirs: &BTreeSet<String>, depth: usize, out: &mut Vec<String>) {
        for child in children(parent, dirs) {
            let name = child.rsplit('/').next().unwrap();
            let indent = "      ".to_string() + &"  ".repeat(depth);
            out.push(format!(
                "{indent}<Directory Id=\"dir_{}\" Name=\"{}\">",
                wix_id(child),
                xml_attr(name)
            ));
            emit(child, dirs, depth + 1, out);
            out.push(format!("{indent}</Directory>"));
        }
    }
    let mut lines = vec![
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>".to_string(),
        "<!-- Generated by exo-verify package from the staging tree. -->".to_string(),
        "<Wix xmlns=\"http://wixtoolset.org/schemas/v4/wxs\">".to_string(),
        "  <Fragment>".to_string(),
    ];
    if !dirs.is_empty() {
        lines.push("    <DirectoryRef Id=\"INSTALLFOLDER\">".into());
        emit("", &dirs, 0, &mut lines);
        lines.push("    </DirectoryRef>".into());
    }
    let mut groups = Vec::new();
    for (dir, files) in &by_dir {
        let (dir_id, suffix) = if dir.is_empty() {
            ("INSTALLFOLDER".to_string(), "root".to_string())
        } else {
            (format!("dir_{}", wix_id(dir)), wix_id(dir))
        };
        let group = format!("StagingFiles_{suffix}");
        lines.push(format!(
            "    <ComponentGroup Id=\"{group}\" Directory=\"{dir_id}\">"
        ));
        for f in files {
            lines.push(format!(
                "      <Component Id=\"comp_{}\" Guid=\"*\">",
                wix_id(&relative(root, f))
            ));
            lines.push(format!(
                "        <File Source=\"{}\" KeyPath=\"yes\" />",
                xml_attr(&f.display().to_string())
            ));
            lines.push("      </Component>".into());
        }
        lines.push("    </ComponentGroup>".into());
        groups.push(group);
    }
    lines.push("    <ComponentGroup Id=\"StagingFiles\">".into());
    for g in groups {
        lines.push(format!("      <ComponentGroupRef Id=\"{g}\" />"));
    }
    lines.push("    </ComponentGroup>".into());
    lines.push("  </Fragment>".into());
    lines.push("</Wix>".into());
    Ok(lines.join("\n") + "\n")
}

/// Administrative extraction of an MSI (no install, no registry, no elevation).
pub fn msi_extract(msi: &Path, target: &Path) -> Result<()> {
    fs::create_dir_all(target)?;
    // Windows Installer interprets a relative TARGETDIR as a network location.
    let target = if target.is_absolute() {
        target.to_path_buf()
    } else {
        std::env::current_dir()?.join(target)
    };
    let out = crate::tools::run(
        Command::new("msiexec.exe")
            .arg("/a")
            .arg(msi)
            .arg("/qn")
            .arg(format!("TARGETDIR={}", target.display())),
        Duration::from_secs(600),
    )?;
    ensure!(
        out.status.success(),
        "msiexec /a failed with {}",
        out.status
    );
    Ok(())
}

fn is_binary(path: &str) -> bool {
    path.ends_with(".exe") || path.ends_with(".dll")
}

fn package_runtime_hashes(root: &Path) -> Result<BTreeMap<String, String>> {
    let mut hashes = BTreeMap::new();
    let runtime = root.join(RUNTIME_DIR);
    for file in walk_files(root)? {
        let rel = relative(root, &file);
        if let Some(leaf) = diagnostics_artifact(&file) {
            ensure!(
                file == runtime.join(leaf),
                "misplaced diagnostics artifact: {rel}"
            );
        }
        if file
            .file_name()
            .is_some_and(|name| name.eq_ignore_ascii_case(RECORDER_NAME))
        {
            ensure!(
                rel == LAUNCHER_NAME || file == runtime.join(RECORDER_NAME),
                "misplaced recorder or launcher: {rel}"
            );
        }
        ensure!(
            hashes
                .insert(rel.to_ascii_lowercase(), sha256_file(&file)?.0)
                .is_none(),
            "duplicate package file: {rel}"
        );
    }
    for rel in [
        LAUNCHER_NAME.to_ascii_lowercase(),
        format!("{RUNTIME_DIR}/{RECORDER_NAME}"),
        format!("{RUNTIME_DIR}/{DIAGNOSTICS_HELPER}"),
    ] {
        ensure!(hashes.contains_key(&rel), "missing package file: {rel}");
    }
    ensure!(
        hashes.contains_key(&format!(
            "{RUNTIME_DIR}/{}",
            diagnostics_pair::MANIFEST_NAME
        )) == hashes.contains_key(&format!(
            "{RUNTIME_DIR}/{}",
            diagnostics_pair::SIGNATURE_NAME
        )),
        "incomplete diagnostics pair"
    );
    Ok(hashes)
}

fn is_msvc_runtime(path: &str) -> bool {
    let leaf = path.rsplit('/').next().unwrap_or(path);
    pe::MSVC_RUNTIME_DLLS.contains(&leaf)
}

fn reject_app_local_crt(hashes: &BTreeMap<String, String>) -> Result<()> {
    if let Some(path) = hashes.keys().find(|path| is_msvc_runtime(path)) {
        bail!("unexpected app-local MSVC runtime outside portable ZIP: {path}");
    }
    Ok(())
}

pub(crate) fn installed_runtime_hashes(root: &Path) -> Result<BTreeMap<String, String>> {
    let hashes = package_runtime_hashes(root)?;
    ensure!(
        !hashes.contains_key(PORTABLE_CONFIG_NAME),
        "the MSI contains {PORTABLE_CONFIG_NAME}"
    );
    reject_app_local_crt(&hashes)?;
    Ok(hashes)
}

pub(crate) fn verify_portable_config(root: &Path) -> Result<()> {
    let config = fs::read(root.join(PORTABLE_CONFIG_NAME))
        .with_context(|| format!("read packaged {PORTABLE_CONFIG_NAME}"))?;
    ensure!(
        config == PORTABLE_CONFIG_DEFAULT.as_bytes(),
        "packaged {PORTABLE_CONFIG_NAME} must select ./data"
    );
    ensure!(
        !root.join("data").exists(),
        "portable ZIP contains user data"
    );
    Ok(())
}

pub(crate) fn portable_runtime_hashes(root: &Path) -> Result<BTreeMap<String, String>> {
    let mut hashes = package_runtime_hashes(root)?;
    if hashes.remove(PORTABLE_CONFIG_NAME).is_some() {
        verify_portable_config(root)?;
    }
    if hashes.keys().any(|path| is_msvc_runtime(path)) {
        for (name, expected) in PORTABLE_CRT_FILES {
            let path = format!("{RUNTIME_DIR}/{name}");
            ensure!(
                hashes.remove(&path).as_deref() == Some(*expected),
                "missing or mismatched pinned MSVC runtime: {path}"
            );
        }
        reject_app_local_crt(&hashes)?;
    }
    Ok(hashes)
}

fn verify_crt_directory(directory: &Path) -> Result<()> {
    for (name, expected) in PORTABLE_CRT_FILES {
        let path = directory.join(name);
        let (actual, _) = sha256_file(&path)
            .with_context(|| format!("read pinned MSVC runtime {}", path.display()))?;
        ensure!(
            actual == *expected,
            "MSVC runtime {} differs from the pinned x64 {VC_REDIST_MIN_VERSION} file",
            path.display()
        );
    }
    Ok(())
}

pub(crate) fn verify_portable_crt(root: &Path) -> Result<pe::Audit> {
    verify_crt_directory(&root.join(RUNTIME_DIR))?;
    validate_launcher_imports(&root.join(LAUNCHER_NAME))?;
    let audit = pe::audit_tree(root)?;
    ensure!(
        audit.msvc == 0,
        "portable ZIP still has {} imports requiring the shared MSVC runtime",
        audit.msvc
    );
    ensure!(
        audit.unresolved.is_empty(),
        "unresolved portable runtime imports: {}",
        audit.unresolved.join(", ")
    );
    Ok(audit)
}

fn stage_portable_crt(staging: &Path, source: &Path) -> Result<pe::Audit> {
    let runtime = staging.join(RUNTIME_DIR);
    for (name, _) in PORTABLE_CRT_FILES {
        fs::copy(source.join(name), runtime.join(name)).with_context(|| {
            format!("stage the pinned x64 {VC_REDIST_MIN_VERSION} MSVC runtime file {name}")
        })?;
    }
    verify_portable_crt(staging)
}

/// Compares shared runtime paths and bytes in the MSI administrative image and portable package.
pub fn verify_msi_runtime_parity(portable: &Path, extracted: &Path) -> Result<usize> {
    let msi_files = walk_files(extracted)?;
    let mut launchers = msi_files
        .iter()
        .filter(|path| path.file_name().is_some_and(|name| name == LAUNCHER_NAME));
    let msi_root = launchers
        .next()
        .and_then(|exe| exe.parent())
        .context("the MSI contains no ExoSnap.exe launcher")?;
    ensure!(
        launchers.next().is_none(),
        "the MSI contains multiple ExoSnap.exe launchers"
    );
    let expected = portable_runtime_hashes(portable)?;
    let actual = installed_runtime_hashes(msi_root)?;
    for file in msi_files.iter().filter(|file| !file.starts_with(msi_root)) {
        let rel = relative(extracted, file);
        ensure!(
            diagnostics_artifact(file).is_none() && !is_binary(&rel.to_ascii_lowercase()),
            "unexpected MSI payload outside the app directory: {rel}"
        );
    }

    let missing: Vec<_> = expected
        .keys()
        .filter(|key| !actual.contains_key(*key))
        .collect();
    let differing: Vec<_> = expected
        .iter()
        .filter(|(key, hash)| actual.get(*key).is_some_and(|actual| actual != *hash))
        .map(|(key, _)| key)
        .collect();
    let extra: Vec<_> = actual
        .keys()
        .filter(|key| {
            !expected.contains_key(*key)
                && (is_binary(key) || diagnostics_artifact(Path::new(key)).is_some())
        })
        .collect();
    ensure!(
        missing.is_empty(),
        "portable files missing from the MSI: {missing:?}"
    );
    ensure!(
        differing.is_empty(),
        "portable files with different MSI bytes: {differing:?}"
    );
    ensure!(
        extra.is_empty(),
        "extra runtime files in the MSI: {extra:?}"
    );
    Ok(expected.len())
}

/// Builds the offline Setup bootstrapper: the pinned VC++ x64 redistributable
/// followed by the MSI, as one WiX Burn bundle. The redistributable is
/// embedded, never downloaded at install time, and is `Permanent` so
/// uninstalling ExoSnap can never remove a shared system runtime.
fn build_setup(
    repo_root: &Path,
    out: &Path,
    base: &str,
    msi: &Path,
    inputs: &SetupInputs,
) -> Result<PathBuf> {
    let wix = crate::tools::require("wix")?;
    let (hash, size) = validate_setup_inputs(inputs)?;
    let icon = repo_root.join("app/assets/brand/exosnap-app.ico");
    ensure!(icon.is_file(), "product icon missing: {}", icon.display());
    let payloads = out.join("setup-payloads.wxs");
    fs::write(
        &payloads,
        setup_payload_xml(&inputs.runtime, &walk_files(&inputs.runtime)?),
    )?;
    let bundle = out.join(format!("ExoSnap-{base}-Setup.exe"));
    println!("==> {}", bundle.display());
    println!(
        "  embedding {VC_REDIST_URL} ({size} bytes, {hash}); minimum runtime {VC_REDIST_MIN_VERSION}"
    );
    run_checked(
        Command::new(wix)
            .args([
                "build",
                "-arch",
                "x64",
                "-ext",
                "WixToolset.Util.wixext/4.0.5",
                "-ext",
                "WixToolset.Bal.wixext/4.0.5",
                "-o",
            ])
            .arg(&bundle)
            .arg("-d")
            .arg(format!("BundleVersion={base}.0"))
            .arg("-d")
            .arg(format!("VCRedistPath={}", inputs.vc_redist.display()))
            .arg("-d")
            .arg(format!("VCRedistMinVersion={VC_REDIST_MIN_VERSION}"))
            .arg("-d")
            .arg(format!("MsiPath={}", msi.display()))
            .arg("-d")
            .arg(format!("IconPath={}", icon.display()))
            .arg("-d")
            .arg(format!(
                "BootstrapperPath={}",
                inputs.bootstrapper.display()
            ))
            .arg(&payloads)
            .arg(repo_root.join("packaging/burn/Setup.wxs")),
        "wix build (Setup)",
        Duration::from_secs(900),
    )?;
    Ok(bundle)
}

fn validate_setup_inputs(inputs: &SetupInputs) -> Result<(String, u64)> {
    let (hash, size) = sha256_file(&inputs.vc_redist)?;
    ensure!(
        hash == VC_REDIST_SHA256,
        "the Visual C++ redistributable at {} hashes to {hash}, not the pinned {VC_REDIST_SHA256} from {VC_REDIST_URL}",
        inputs.vc_redist.display()
    );
    let shim_imports = pe::imports_of(&inputs.bootstrapper)?;
    ensure!(
        shim_imports
            .iter()
            .all(|name| !pe::MSVC_RUNTIME_DLLS.contains(&name.to_ascii_lowercase().as_str())),
        "the native Setup loader must use the static CRT before the prerequisite is installed"
    );
    validate_setup_runtime(&inputs.runtime)?;
    Ok((hash, size))
}

fn prepare_setup_runtime(
    repo_root: &Path,
    staging: &Path,
    runtime: &Path,
    out: &Path,
) -> Result<()> {
    fs::create_dir_all(out)?;
    for file in walk_files(runtime)? {
        let rel = relative(runtime, &file);
        let extension = file.extension().and_then(|e| e.to_str()).unwrap_or("");
        if FORBIDDEN_EXTENSIONS.contains(&extension) || rel == "exosnap-setup-preview.exe" {
            continue;
        }
        let name = file
            .file_name()
            .unwrap()
            .to_string_lossy()
            .to_ascii_lowercase();
        ensure!(!is_debug_qt(&name), "debug Qt in Setup payload: {rel}");
        ensure!(
            !is_debug_msvc(&name),
            "debug MSVC runtime in Setup payload: {rel}"
        );
        let destination = out.join(&rel);
        fs::create_dir_all(destination.parent().unwrap())?;
        fs::copy(file, destination)?;
    }
    fs::copy(repo_root.join("LICENSE"), out.join("LICENSE"))?;
    fs::copy(
        staging.join("licenses/THIRD_PARTY_NOTICES.md"),
        out.join("THIRD_PARTY_NOTICES.md"),
    )?;
    copy_tree(&staging.join("licenses"), &out.join("licenses"))?;
    fs::copy(
        repo_root.join("packaging/burn/wix-license.txt"),
        out.join("licenses/wix.txt"),
    )?;
    Ok(())
}

fn setup_build_file(build_dir: &Path, name: &str) -> Result<PathBuf> {
    let mut candidates = vec![
        build_dir.join("apps/setup").join(name),
        build_dir.join("apps/setup/Release").join(name),
    ];
    if let Some(runtime_file) = name.strip_prefix("runtime/") {
        candidates.push(
            build_dir
                .join("apps/setup/runtime/Release")
                .join(runtime_file),
        );
    }
    candidates.into_iter().find(|path| path.is_file())
        .with_context(|| format!("Setup build output missing: {name}; build exosnap_setup_ba and exosnap_setup_ui in Release"))
}

fn validate_setup_runtime(root: &Path) -> Result<()> {
    let audit = pe::audit_tree(root)?;
    ensure!(
        audit.msvc == 0 && audit.unresolved.is_empty(),
        "Setup UI needs a complete private runtime before the VC chain runs: {} unresolved MSVC imports; {:?}",
        audit.msvc,
        audit.unresolved
    );
    for file in walk_files(root)? {
        if !file
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| e.eq_ignore_ascii_case("dll"))
        {
            continue;
        }
        for import in pe::imports_of(&file)? {
            if pe::MSVC_RUNTIME_DLLS.contains(&import.to_ascii_lowercase().as_str()) {
                ensure!(
                    root.join(&import).is_file(),
                    "Setup private runtime must be at payload root: {import}"
                );
            }
        }
    }
    Ok(())
}

fn is_debug_msvc(name: &str) -> bool {
    name == "ucrtbased.dll"
        || (["vcruntime", "msvcp", "concrt", "vcomp"]
            .iter()
            .any(|prefix| name.starts_with(prefix))
            && (name.ends_with("d.dll") || name.contains("d_")))
}

fn setup_payload_xml(root: &Path, files: &[PathBuf]) -> String {
    fn escaped(text: &str) -> String {
        text.replace('&', "&amp;")
            .replace('<', "&lt;")
            .replace('"', "&quot;")
    }
    let mut xml = String::from(
        "<Wix xmlns=\"http://wixtoolset.org/schemas/v4/wxs\"><Fragment><PayloadGroup Id=\"ExoSnapSetupUiPayloads\">\n",
    );
    for (index, file) in files.iter().enumerate() {
        xml.push_str(&format!(
            "<Payload Id=\"SetupUi{index}\" Name=\"{}\" SourceFile=\"{}\" Compressed=\"yes\" />\n",
            escaped(&relative(root, file).replace('/', "\\")),
            escaped(&file.to_string_lossy())
        ));
    }
    xml.push_str("</PayloadGroup></Fragment></Wix>\n");
    xml
}

fn ownership_probe_path(build_dir: &Path) -> Result<PathBuf> {
    let directory = build_dir.join("packaging/msi");
    [
        directory.join(MSI_OWNER_PROBE),
        directory.join("Release").join(MSI_OWNER_PROBE),
    ]
    .into_iter()
    .find(|path| path.is_file())
    .with_context(|| {
        format!(
            "MSI ownership probe is missing from {}. Build exosnap first",
            directory.display()
        )
    })
}

fn prepare_runtime(build_dir: &Path, staging: &Path) -> Result<()> {
    println!("==> cmake --install into {}", staging.display());
    run_checked(
        Command::new("cmake")
            .arg("--install")
            .arg(build_dir)
            .args(["--config", "Release", "--prefix"])
            .arg(staging),
        "cmake --install",
        Duration::from_secs(900),
    )?;
    stage_diagnostics_helper(
        staging,
        &build_dir
            .join("apps/diagnostics/Release")
            .join(DIAGNOSTICS_HELPER),
    )?;
    // Vendored static dependencies install headers and import libraries the
    // product never loads; they carry no switch to turn that off.
    for dev in ["lib", "include", "bin"] {
        remove_if_exists(&staging.join(dev))?;
    }
    // Software OpenGL, translations, network plugins, development-only QML
    // tooling and the unattached Controls styles are not used. The compiler
    // runtime is added only to the ZIP after the MSI has been assembled.
    let runtime = staging.join(RUNTIME_DIR);
    for unused in [
        "opengl32sw.dll",
        "translations",
        "plugins/bearer",
        "plugins/tls",
        "plugins/qmltooling",
        "qml/QtQuick/Controls/Fusion",
        "qml/QtQuick/Controls/Imagine",
        "qml/QtQuick/Controls/Material",
        "qml/QtQuick/Controls/Universal",
        "qml/QtQuick/Controls/FluentWinUI3",
        "qml/QtQuick/Controls/Windows",
        "qml/QtQuick/NativeStyle",
        "bin",
    ] {
        remove_if_exists(&runtime.join(unused))?;
    }
    for entry in fs::read_dir(&runtime)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
        if name.starts_with("vc_redist") && name.ends_with(".exe") {
            remove_if_exists(&entry.path())?;
        }
    }
    Ok(())
}

fn validate_runtime(repo_root: &Path, staging: &Path, version: &str) -> Result<pe::Audit> {
    let mut leaks = vec![
        r"C:\Users\".to_string(),
        ".workspace".into(),
        ".claude".into(),
        repo_root.display().to_string(),
    ];
    if let Ok(o) = Command::new("git")
        .args(["config", "user.email"])
        .current_dir(repo_root)
        .output()
    {
        leaks.push(String::from_utf8_lossy(&o.stdout).trim().to_string());
    }
    println!("==> validating the install tree");
    let mut errors = validate_tree(staging, version, &leaks)?;
    let launcher = staging.join(LAUNCHER_NAME);
    if launcher.is_file()
        && let Err(error) = validate_launcher_imports(&launcher)
    {
        errors.push(format!("{error:#}"));
    }
    errors.extend(walk_files(staging)?.iter().filter_map(|file| {
        let path = relative(staging, file);
        is_msvc_runtime(&path.to_ascii_lowercase())
            .then(|| format!("unexpected MSVC runtime in common staging: {path}"))
    }));
    println!("==> auditing static runtime dependencies");
    let audit = pe::audit_tree(staging)?;
    errors.extend(
        audit
            .unresolved
            .iter()
            .map(|u| format!("unresolved runtime dependency: {u}")),
    );
    if !errors.is_empty() {
        for e in &errors {
            eprintln!("  [FAIL] {e}");
        }
        bail!(
            "{} packaging problem(s); nothing was packaged",
            errors.len()
        );
    }
    println!(
        "  {} binaries: {} shipped, {} Windows, {} MSVC-runtime imports, 0 unresolved",
        audit.binaries, audit.shipped, audit.system, audit.msvc
    );
    Ok(audit)
}

fn validate_launcher_imports(launcher: &Path) -> Result<()> {
    for import in pe::imports_of(launcher)? {
        let name = import.to_ascii_lowercase();
        ensure!(
            pe::WINDOWS_SYSTEM_DLLS.contains(&name.as_str())
                || name.starts_with("api-ms-win-")
                || name.starts_with("ext-ms-"),
            "{LAUNCHER_NAME} depends on a DLL outside Windows: {import}"
        );
    }
    Ok(())
}

fn copy_tree(source: &Path, target: &Path) -> Result<()> {
    for file in walk_files(source)? {
        let destination = target.join(file.strip_prefix(source)?);
        fs::create_dir_all(destination.parent().unwrap())?;
        fs::copy(&file, &destination)
            .with_context(|| format!("copy {} to {}", file.display(), destination.display()))?;
    }
    Ok(())
}

fn prepared_hashes(root: &Path) -> Result<BTreeMap<String, String>> {
    let mut hashes = BTreeMap::new();
    for file in walk_files(root)? {
        let rel = relative(root, &file);
        if rel == PREPARED_INVENTORY {
            continue;
        }
        ensure!(
            fs::symlink_metadata(&file)?.is_file(),
            "prepared input is not a regular file: {rel}"
        );
        ensure!(
            hashes
                .insert(rel.to_ascii_lowercase(), sha256_file(&file)?.0)
                .is_none(),
            "duplicate prepared input: {rel}"
        );
    }
    Ok(hashes)
}

fn load_prepared_inventory(root: &Path) -> Result<PreparedInventory> {
    let path = root.join(PREPARED_INVENTORY);
    let inventory: PreparedInventory = serde_json::from_slice(
        &fs::read(&path).with_context(|| format!("read {}", path.display()))?,
    )
    .with_context(|| format!("parse {}", path.display()))?;
    let actual = prepared_hashes(root)?;
    if actual != inventory.files {
        let changed = inventory
            .files
            .iter()
            .find(|(name, hash)| actual.get(*name) != Some(*hash));
        let unexpected = actual
            .keys()
            .find(|name| !inventory.files.contains_key(*name));
        bail!(
            "prepared package input inventory mismatch (changed or missing: {:?}, unexpected: {:?})",
            changed.map(|(name, _)| name),
            unexpected
        );
    }
    Ok(inventory)
}

fn stage_prepared_runtime(root: &Path, staging: &Path) -> Result<()> {
    let runtime = root.join("runtime");
    let expected_helper = runtime.join(RUNTIME_DIR).join(DIAGNOSTICS_HELPER);
    for file in walk_files(&runtime)? {
        if diagnostics_artifact(&file).is_some() {
            ensure!(
                file == expected_helper,
                "unexpected prepared diagnostics artifact: {}",
                relative(&runtime, &file)
            );
        }
    }
    copy_tree(&runtime, staging)
}

fn installer_inputs_from_build(
    repo_root: &Path,
    build_dir: &Path,
    staging: &Path,
    out: &Path,
    vc_redist: Option<&Path>,
) -> Result<InstallerInputs> {
    let owner_probe = ownership_probe_path(build_dir)?;
    let setup = if let Some(vc_redist) = vc_redist {
        let bootstrapper = setup_build_file(build_dir, SETUP_BOOTSTRAPPER)?;
        let ui = setup_build_file(build_dir, "runtime/exosnap-setup-ui.dll")?;
        let runtime = out.join(SETUP_RUNTIME);
        prepare_setup_runtime(repo_root, staging, ui.parent().unwrap(), &runtime)?;
        Some(SetupInputs {
            bootstrapper,
            runtime,
            vc_redist: vc_redist.to_path_buf(),
        })
    } else {
        None
    };
    Ok(InstallerInputs { owner_probe, setup })
}

fn installer_inputs_from_prepared(root: &Path, out: &Path) -> Result<InstallerInputs> {
    let owner_probe = root.join(MSI_OWNER_PROBE);
    ensure!(
        owner_probe.is_file(),
        "prepared inputs lack the MSI ownership probe"
    );
    let vc_redist = root.join(SETUP_REDIST);
    let bootstrapper = root.join(SETUP_BOOTSTRAPPER);
    let prepared_runtime = root.join(SETUP_RUNTIME);
    let setup = if vc_redist.is_file() {
        ensure!(
            bootstrapper.is_file(),
            "prepared inputs lack the Setup bootstrapper"
        );
        let runtime = out.join(SETUP_RUNTIME);
        copy_tree(&prepared_runtime, &runtime)?;
        Some(SetupInputs {
            bootstrapper,
            runtime,
            vc_redist,
        })
    } else {
        ensure!(
            !bootstrapper.exists() && !prepared_runtime.exists(),
            "prepared inputs lack the Setup redistributable"
        );
        None
    };
    Ok(InstallerInputs { owner_probe, setup })
}

fn package_version(base: &str, version: &str) -> Result<()> {
    let re = regex::Regex::new(r"^([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?$").unwrap();
    let caps = re
        .captures(version)
        .with_context(|| format!("'{version}' is not X.Y.Z or X.Y.Z-label"))?;
    ensure!(
        &caps[1] == base,
        "version '{version}' has base '{}', but CMakeLists.txt declares {base}",
        &caps[1]
    );
    Ok(())
}

fn empty_output(path: &Path, prepared: Option<&Path>) -> Result<PathBuf> {
    if let Some(prepared) = prepared {
        let prepared = fs::canonicalize(prepared)?;
        let absolute = std::path::absolute(path)?;
        if let Some(ancestor) = absolute.ancestors().find(|path| path.exists()) {
            ensure!(
                !fs::canonicalize(ancestor)?.starts_with(&prepared),
                "the package output must be outside the prepared inputs"
            );
        }
    }
    if path.exists() {
        ensure!(
            fs::read_dir(path)?.next().is_none(),
            "{} is not empty",
            path.display()
        );
    }
    fs::create_dir_all(path)?;
    Ok(external_path(&fs::canonicalize(path)?))
}

fn export_prepared(
    args: &PackageArgs,
    repo_root: &Path,
    build_dir: &Path,
    out: &Path,
    version: String,
) -> Result<()> {
    let staging = out.join("runtime");
    prepare_runtime(build_dir, &staging)?;
    validate_runtime(repo_root, &staging, &version)?;
    if !args.skip_msi {
        let inputs = installer_inputs_from_build(
            repo_root,
            build_dir,
            &staging,
            out,
            args.vc_redist.as_deref(),
        )?;
        fs::copy(inputs.owner_probe, out.join(MSI_OWNER_PROBE))?;
        if let Some(setup) = inputs.setup {
            validate_setup_inputs(&setup)?;
            verify_crt_directory(&setup.runtime)?;
            fs::copy(setup.bootstrapper, out.join(SETUP_BOOTSTRAPPER))?;
            fs::copy(setup.vc_redist, out.join(SETUP_REDIST))?;
        }
    }
    let inventory = PreparedInventory {
        version,
        files: prepared_hashes(out)?,
    };
    fs::write(
        out.join(PREPARED_INVENTORY),
        serde_json::to_vec_pretty(&inventory)?,
    )?;
    println!(
        "prepared {} files in {}",
        inventory.files.len(),
        out.display()
    );
    Ok(())
}

pub fn run(args: &PackageArgs) -> Result<Option<PackageResult>> {
    let repo_root = external_path(&fs::canonicalize(&args.repo_root)?);
    let inputs_root = args
        .prepared_dir
        .as_ref()
        .map(|path| fs::canonicalize(path).map(|path| external_path(&path)))
        .transpose()?;
    let inventory = inputs_root
        .as_deref()
        .map(load_prepared_inventory)
        .transpose()?;
    let base = project_version(&repo_root)?;
    let version = args
        .version
        .clone()
        .or_else(|| {
            inventory
                .as_ref()
                .map(|inventory| inventory.version.clone())
        })
        .unwrap_or_else(|| format!("{base}-dev"));
    package_version(&base, &version)?;
    if let Some(inventory) = inventory {
        ensure!(
            inventory.version == version,
            "prepared package version '{}' does not match '{version}'",
            inventory.version
        );
    }
    let out = empty_output(&args.out, inputs_root.as_deref())?;
    let build_dir = args.build_dir.as_deref();
    if args.prepare_only {
        export_prepared(
            args,
            &repo_root,
            build_dir.context("missing build directory")?,
            &out,
            version,
        )?;
        return Ok(None);
    }

    let top = portable_dir_name(&version);
    let staging = out.join("staging").join(&top);
    if let Some(prepared) = &inputs_root {
        stage_prepared_runtime(prepared, &staging)?;
    } else {
        prepare_runtime(build_dir.context("missing build directory")?, &staging)?;
    }
    if let Some(pair_dir) = &args.diagnostics_pair_dir {
        let public = manifest::public_key_from_hex(
            args.diagnostics_public_key_hex
                .as_deref()
                .context("missing diagnostics public key")?,
        )?;
        stage_diagnostics_pair(&staging, pair_dir, &public)?;
    }
    let common_audit = validate_runtime(&repo_root, &staging, &version)?;

    let installer_inputs = if args.skip_msi {
        None
    } else if let Some(root) = &inputs_root {
        Some(installer_inputs_from_prepared(root, &out)?)
    } else {
        Some(installer_inputs_from_build(
            &repo_root,
            build_dir.context("missing build directory")?,
            &staging,
            &out,
            args.vc_redist.as_deref(),
        )?)
    };
    let installer = if let Some(inputs) = &installer_inputs {
        let wix = crate::tools::require("wix")?;
        let msi = out.join(format!("ExoSnap-{version}-{PLATFORM}.msi"));
        let fragment = out.join("_harvest.wxs");
        fs::write(&fragment, harvest_fragment(&staging)?)?;
        println!("==> {}", msi.display());
        // The Windows Installer ProductVersion is numeric; the full identity
        // lives in the file name and the executables' ProductVersion string.
        run_checked(
            Command::new(wix)
                .args([
                    "build",
                    "-arch",
                    "x64",
                    "-ext",
                    "WixToolset.Util.wixext/4.0.5",
                    "-o",
                ])
                .arg(&msi)
                .arg("-d")
                .arg(format!("ProductVersion={base}"))
                .arg("-d")
                .arg(format!("VCRedistMinVersion={}", vc_redist_floor()))
                .arg("-d")
                .arg(format!(
                    "AppIconPath={}",
                    repo_root.join("app/assets/brand/exosnap-app.ico").display()
                ))
                .arg("-d")
                .arg(format!(
                    "DistributionOwnerProbePath={}",
                    inputs.owner_probe.display()
                ))
                .arg(repo_root.join("packaging/msi/Package.wxs"))
                .arg(&fragment),
            "wix build",
            Duration::from_secs(900),
        )?;
        let extracted = out.join("msi-extract");
        msi_extract(&msi, &extracted)?;
        verify_msi_runtime_parity(&staging, &extracted)?;
        fs::remove_dir_all(&extracted).ok();
        Some(msi)
    } else {
        None
    };

    let setup_inputs = installer_inputs
        .as_ref()
        .and_then(|inputs| inputs.setup.as_ref());
    let setup = match (&installer, setup_inputs) {
        (Some(msi), Some(inputs)) => Some(build_setup(&repo_root, &out, &base, msi, inputs)?),
        _ => None,
    };

    let audit = if let Some(inputs) = setup_inputs {
        stage_portable_crt(&staging, &inputs.runtime)?
    } else {
        common_audit
    };
    fs::write(staging.join(PORTABLE_CONFIG_NAME), PORTABLE_CONFIG_DEFAULT)?;
    let portable = out.join(format!("{top}.zip"));
    println!("==> {}", portable.display());
    write_zip(&staging, &top, &portable)?;
    if args.launch_smoke {
        println!("==> launch smoke");
        launch_smoke(&staging)?;
    }

    let file_count = walk_files(&staging)?.len();
    let result = PackageResult {
        version,
        base_version: base,
        portable,
        installer,
        setup,
        staging,
        file_count,
        dependency_audit: BTreeMap::from([
            ("binaries".into(), audit.binaries),
            ("shipped".into(), audit.shipped),
            ("windows".into(), audit.system),
            ("msvcRuntime".into(), audit.msvc),
        ]),
        dependencies: dependency_identity::derive(&repo_root)?.entries,
    };
    fs::write(
        out.join("package.json"),
        serde_json::to_vec_pretty(&result)?,
    )?;
    footprint::write(
        &out,
        &result,
        setup_inputs
            .map(|inputs| inputs.vc_redist.as_path())
            .or(args.vc_redist.as_deref()),
    )?;
    Ok(Some(result))
}

/// Runs the staged application's own smoke test with only Windows on PATH,
/// no Qt location variables, an isolated configuration and temp directory,
/// and hard-error dialogs suppressed, so a missing DLL exits instead of
/// waiting on a modal box.
fn launch_smoke(staging: &Path) -> Result<()> {
    let exe = staging.join(RUNTIME_DIR).join(RECORDER_NAME);
    ensure!(exe.is_file(), "{} is missing", exe.display());
    let scratch = std::env::temp_dir().join(crate::control::new_run_id("exosnap-smoke"));
    let (config, temp) = (scratch.join("config"), scratch.join("temp"));
    fs::create_dir_all(&config)?;
    fs::create_dir_all(&temp)?;
    let root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into());
    let path = [
        format!(r"{root}\system32"),
        root.clone(),
        format!(r"{root}\System32\Wbem"),
    ]
    .join(";");
    let mut command = Command::new(&exe);
    command
        .arg("--smoke-test")
        .current_dir(&temp)
        .env("PATH", path)
        .env("EXOSNAP_CONFIG_DIR", &config)
        .env("TEMP", &temp)
        .env("TMP", &temp);
    for name in [
        "QML_IMPORT_PATH",
        "QML2_IMPORT_PATH",
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QT_QUICK_CONTROLS_STYLE",
        "QT_DIR",
        "Qt6_DIR",
    ] {
        command.env_remove(name);
    }
    #[cfg(windows)]
    let previous = unsafe {
        use windows::Win32::System::Diagnostics::Debug::{
            SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX, SEM_NOOPENFILEERRORBOX, SetErrorMode,
        };
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)
    };
    let result = crate::tools::run(&mut command, Duration::from_secs(120));
    #[cfg(windows)]
    unsafe {
        windows::Win32::System::Diagnostics::Debug::SetErrorMode(previous);
    }
    let _ = fs::remove_dir_all(&scratch);
    let out = result?;
    ensure!(
        out.success(),
        "exosnap.exe --smoke-test exited {:?} in a clean environment: {}",
        out.code(),
        out.stderr.trim()
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::Parser;

    #[test]
    fn package_cli_can_prepare_and_consume_inputs_without_a_build_tree() {
        let prepare = [
            "exo-verify",
            "package",
            "--prepare-only",
            "--build-dir",
            "build",
            "--out",
            "inputs",
        ];
        assert!(crate::Cli::try_parse_from(prepare).is_ok());
        let consume = [
            "exo-verify",
            "package",
            "--prepared-dir",
            "inputs",
            "--out",
            "packages",
        ];
        let signed = [
            "--diagnostics-pair-dir",
            "pair",
            "--diagnostics-public-key-hex",
            "pin",
        ];
        assert!(crate::Cli::try_parse_from(consume).is_err());
        assert!(crate::Cli::try_parse_from(consume.into_iter().chain(signed)).is_ok());
        assert!(
            crate::Cli::try_parse_from(
                consume
                    .into_iter()
                    .chain(signed)
                    .chain(["--build-dir", "build"])
            )
            .is_err()
        );
        assert!(crate::Cli::try_parse_from(prepare.into_iter().chain(signed)).is_err());
    }

    #[test]
    fn package_cli_requires_diagnostics_directory_and_pin_together() {
        let local = [
            "exo-verify",
            "package",
            "--build-dir",
            "build",
            "--out",
            "out",
        ];
        assert!(crate::Cli::try_parse_from(local).is_ok());
        let pair = ["--diagnostics-pair-dir", "pair"];
        let pin = ["--diagnostics-public-key-hex", "pin"];
        for flag in [pair, pin] {
            let error = crate::Cli::try_parse_from(local.into_iter().chain(flag))
                .err()
                .expect("the other flag is required");
            assert_eq!(
                error.kind(),
                clap::error::ErrorKind::MissingRequiredArgument
            );
        }
        assert!(crate::Cli::try_parse_from(local.into_iter().chain(pair).chain(pin)).is_ok());
    }

    #[test]
    fn external_tools_receive_normal_windows_paths() {
        assert_eq!(
            external_path(Path::new(r"\\?\C:\work\package")),
            PathBuf::from(r"C:\work\package")
        );
        assert_eq!(
            external_path(Path::new(r"\\?\UNC\server\share\package")),
            PathBuf::from(r"\\server\share\package")
        );
    }

    #[test]
    fn debug_qt_names_are_recognised() {
        assert!(is_debug_qt("qt6cored.dll"));
        assert!(is_debug_qt("qt6quickd.dll"));
        assert!(is_debug_qt("qt6quick3dd.dll"));
        assert!(!is_debug_qt("qt6quick3d.dll"));
        assert!(!is_debug_qt("Qt6Quick3D.dll"));
        assert!(is_debug_qt("Qt6Quick3Dd.dll"));
        assert!(is_debug_qt("Qt6Cored.dll"));
        assert!(!is_debug_qt("qt6quickshapes.dll"));
        assert!(!is_debug_qt("qt6core.dll"));
    }

    #[test]
    fn an_empty_tree_reports_every_missing_piece() {
        let dir = tempfile::tempdir().unwrap();
        let errors = validate_tree(dir.path(), "0.10.0", &[]).unwrap();
        assert!(errors.iter().any(|e| e.contains("exosnap.exe")));
        assert!(errors.iter().any(|e| e.contains("qml/QtQuick/Effects")));
        assert!(
            errors
                .iter()
                .any(|e| e.contains("qml/QtQuick/Controls/Basic"))
        );
        assert!(errors.iter().any(|e| e.contains("licenses/ffmpeg.txt")));
    }

    #[test]
    fn retired_gameinput_payloads_are_rejected_even_beside_sdl() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join("SDL3.dll"), []).unwrap();
        fs::write(dir.path().join("GameInputRedist.dll"), []).unwrap();
        fs::create_dir(dir.path().join("nested")).unwrap();
        fs::write(dir.path().join("nested/GAMEINPUTRAWINPUTPROXY.EXE"), []).unwrap();
        let errors = validate_tree(dir.path(), "0.10.0", &[]).unwrap();
        for name in ["GameInputRedist.dll", "GAMEINPUTRAWINPUTPROXY.EXE"] {
            assert!(
                errors.iter().any(
                    |error| error.contains("forbidden runtime payload") && error.contains(name)
                ),
                "retired runtime was accepted: {name}"
            );
        }
    }

    #[test]
    fn portable_config_requires_the_default_marker_and_no_user_data() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        let marker = root.join(PORTABLE_CONFIG_NAME);
        assert!(verify_portable_config(root).is_err());
        fs::write(&marker, "data_path = \"./other\"\n").unwrap();
        assert!(verify_portable_config(root).is_err());
        fs::write(marker, PORTABLE_CONFIG_DEFAULT).unwrap();
        verify_portable_config(root).unwrap();
        fs::create_dir(root.join("data")).unwrap();
        assert!(verify_portable_config(root).is_err());
    }

    #[test]
    fn portable_tree_allows_diagnostics_payload_only_beside_the_recorder() {
        let dir = tempfile::tempdir().unwrap();
        let runtime = dir.path().join(RUNTIME_DIR);
        fs::create_dir(&runtime).unwrap();
        fs::write(runtime.join(DIAGNOSTICS_HELPER), b"helper").unwrap();
        let errors = validate_tree(dir.path(), "0.11.0", &[]).unwrap();
        assert!(
            errors
                .iter()
                .all(|error| !error.contains(DIAGNOSTICS_HELPER)),
            "the recorder's sibling helper was rejected"
        );
        let helper = dir.path().join("nested/EXOSNAP-HELPER.EXE");
        fs::create_dir_all(helper.parent().unwrap()).unwrap();
        fs::write(&helper, []).unwrap();
        fs::write(dir.path().join("nested/DIAGNOSTICS-PAIR.TXT.SIG"), b"sig").unwrap();
        let errors = validate_tree(dir.path(), "0.11.0", &[]).unwrap();
        for leaf in ["EXOSNAP-HELPER.EXE", "DIAGNOSTICS-PAIR.TXT.SIG"] {
            assert!(errors.iter().any(|error| error.contains(leaf)), "{leaf}");
        }
    }

    #[test]
    fn stages_the_signed_pair_with_the_portable_helper_and_rejects_contamination() {
        let dir = tempfile::tempdir().unwrap();
        let pair = dir.path().join("pair");
        let helper = dir.path().join(DIAGNOSTICS_HELPER);
        let staging = dir.path().join("staging");
        let runtime = staging.join("runtime");
        fs::create_dir_all(&runtime).unwrap();
        fs::write(staging.join("ExoSnap.exe"), b"launcher").unwrap();
        let recorder = runtime.join("exosnap.exe");
        fs::write(&recorder, b"recorder").unwrap();
        fs::write(&helper, b"helper").unwrap();
        let key = ed25519_dalek::SigningKey::from_bytes(&[7; 32]);
        diagnostics_pair::sign_to_directory(&pair, &recorder, &helper, &key).unwrap();
        let public = key.verifying_key();

        stage_diagnostics_helper(&staging, &helper).unwrap();
        stage_diagnostics_pair(&staging, &pair, &public).unwrap();
        let archive = dir.path().join("portable.zip");
        write_zip(&staging, "portable", &archive).unwrap();
        let extracted = dir.path().join("extracted");
        crate::context::extract_zip(&archive, &extracted).unwrap();
        verify_staged_diagnostics(&extracted.join("portable"), &public).unwrap();

        let xml = harvest_fragment(&staging).unwrap();
        assert_eq!(xml.matches(DIAGNOSTICS_HELPER).count(), 1);
        assert!(xml.contains(diagnostics_pair::MANIFEST_NAME));
        assert!(xml.contains(diagnostics_pair::SIGNATURE_NAME));

        for leaf in DIAGNOSTICS_ARTIFACTS {
            fs::remove_file(runtime.join(leaf)).unwrap();
        }
        fs::create_dir(staging.join("nested")).unwrap();
        fs::write(staging.join("nested/DIAGNOSTICS-PAIR.TXT"), []).unwrap();
        let error = stage_diagnostics_helper(&staging, &helper).unwrap_err();
        assert!(
            error
                .to_string()
                .contains("unexpected diagnostics artifact")
        );
    }

    #[test]
    fn staging_rejects_a_malformed_or_mismatched_signed_pair() {
        let dir = tempfile::tempdir().unwrap();
        let pair = dir.path().join("pair");
        let helper = dir.path().join(DIAGNOSTICS_HELPER);
        fs::write(&helper, b"helper").unwrap();
        let key = ed25519_dalek::SigningKey::from_bytes(&[7; 32]);
        let public = key.verifying_key();
        for (folder, expected_error) in [
            ("mismatched", "does not match recorder and helper"),
            ("malformed", "signature is not 64 bytes"),
        ] {
            let staging = dir.path().join(folder);
            let runtime = staging.join(RUNTIME_DIR);
            fs::create_dir_all(&runtime).unwrap();
            let recorder = runtime.join(RECORDER_NAME);
            fs::write(&recorder, b"recorder").unwrap();
            diagnostics_pair::sign_to_directory(&pair, &recorder, &helper, &key).unwrap();
            if folder == "mismatched" {
                fs::write(&recorder, b"changed recorder").unwrap();
            } else {
                fs::write(pair.join(diagnostics_pair::SIGNATURE_NAME), b"x").unwrap();
            }
            stage_diagnostics_helper(&staging, &helper).unwrap();
            let error = stage_diagnostics_pair(&staging, &pair, &public).unwrap_err();
            assert!(error.to_string().contains(expected_error), "{error:#}");
        }
    }

    #[test]
    fn prepared_inputs_relocate_and_stage_the_signed_runtime_and_setup_inputs() {
        let dir = tempfile::tempdir().unwrap();
        let prepare = dir.path().join("prepare");
        let package = prepare.join("runtime");
        let runtime = package.join(RUNTIME_DIR);
        fs::create_dir_all(&runtime).unwrap();
        fs::write(package.join(LAUNCHER_NAME), b"launcher").unwrap();
        let recorder = runtime.join(RECORDER_NAME);
        let helper = runtime.join(DIAGNOSTICS_HELPER);
        fs::write(&recorder, b"recorder").unwrap();
        fs::write(&helper, b"helper").unwrap();
        for leaf in [MSI_OWNER_PROBE, SETUP_BOOTSTRAPPER, SETUP_REDIST] {
            fs::write(prepare.join(leaf), leaf).unwrap();
        }
        let setup_ui = format!("{SETUP_RUNTIME}/exosnap-setup-ui.dll");
        fs::create_dir(prepare.join(SETUP_RUNTIME)).unwrap();
        fs::write(prepare.join(&setup_ui), b"setup ui").unwrap();
        let inventory = PreparedInventory {
            version: "0.11.0".into(),
            files: prepared_hashes(&prepare).unwrap(),
        };
        fs::write(
            prepare.join(PREPARED_INVENTORY),
            serde_json::to_vec(&inventory).unwrap(),
        )
        .unwrap();

        let key = ed25519_dalek::SigningKey::from_bytes(&[7; 32]);
        let pair = dir.path().join("pair");
        diagnostics_pair::sign_to_directory(&pair, &recorder, &helper, &key).unwrap();
        let downloaded = dir.path().join("downloaded");
        fs::rename(&prepare, &downloaded).unwrap();
        let loaded = load_prepared_inventory(&downloaded).unwrap();
        assert_eq!(loaded.version, "0.11.0");

        let inside = downloaded.join("bad-output");
        let alias = external_path(&fs::canonicalize(&downloaded).unwrap())
            .join("..")
            .join("downloaded");
        for prepared in [&alias, &downloaded] {
            let error = empty_output(&inside, Some(prepared)).unwrap_err();
            assert_eq!(
                error.to_string(),
                "the package output must be outside the prepared inputs"
            );
            assert!(!inside.exists());
        }
        let out = dir.path().join("packages");
        let stage = out.join("staging");
        stage_prepared_runtime(&downloaded, &stage).unwrap();
        stage_diagnostics_pair(&stage, &pair, &key.verifying_key()).unwrap();
        assert_eq!(
            fs::read(stage.join(RUNTIME_DIR).join(DIAGNOSTICS_HELPER)).unwrap(),
            b"helper"
        );
        let installer = installer_inputs_from_prepared(&downloaded, &out).unwrap();
        assert_eq!(
            fs::read(installer.owner_probe).unwrap(),
            MSI_OWNER_PROBE.as_bytes()
        );
        let setup = installer.setup.unwrap();
        assert_eq!(
            fs::read(setup.bootstrapper).unwrap(),
            SETUP_BOOTSTRAPPER.as_bytes()
        );
        assert_eq!(fs::read(setup.vc_redist).unwrap(), SETUP_REDIST.as_bytes());
        assert_eq!(fs::read(out.join(&setup_ui)).unwrap(), b"setup ui");
        assert_eq!(prepared_hashes(&downloaded).unwrap(), loaded.files);

        fs::write(downloaded.join(&setup_ui), b"changed").unwrap();
        let error = load_prepared_inventory(&downloaded).err().unwrap();
        assert!(error.to_string().contains(&setup_ui));
        fs::write(downloaded.join(&setup_ui), b"setup ui").unwrap();
        fs::write(downloaded.join("unexpected.dll"), []).unwrap();
        assert!(load_prepared_inventory(&downloaded).is_err());
    }

    #[test]
    fn msi_parity_requires_the_same_helper_path_and_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let portable = dir.path().join("portable");
        let extracted = dir.path().join("msi-extract");
        let program_files = extracted.join("Program Files");
        let app = program_files.join("ExoSnap");
        let misplaced_dir = program_files.join("ExoSnap Diagnostics");
        let portable_runtime = portable.join(RUNTIME_DIR);
        let installed_runtime = app.join(RUNTIME_DIR);
        fs::create_dir_all(&portable_runtime).unwrap();
        fs::create_dir_all(&installed_runtime).unwrap();
        fs::create_dir_all(&misplaced_dir).unwrap();
        for root in [&portable, &app] {
            fs::write(root.join(LAUNCHER_NAME), b"launcher").unwrap();
        }
        for leaf in [
            RECORDER_NAME,
            diagnostics_pair::MANIFEST_NAME,
            diagnostics_pair::SIGNATURE_NAME,
        ] {
            fs::write(portable_runtime.join(leaf), leaf).unwrap();
            fs::write(installed_runtime.join(leaf), leaf).unwrap();
        }
        let source = portable_runtime.join(DIAGNOSTICS_HELPER);
        fs::write(&source, b"helper").unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());

        let helper = installed_runtime.join(DIAGNOSTICS_HELPER);
        fs::copy(&source, &helper).unwrap();
        assert_eq!(verify_msi_runtime_parity(&portable, &extracted).unwrap(), 5);
        fs::write(portable.join(PORTABLE_CONFIG_NAME), PORTABLE_CONFIG_DEFAULT).unwrap();
        assert_eq!(verify_msi_runtime_parity(&portable, &extracted).unwrap(), 5);
        let installed_marker = app.join(PORTABLE_CONFIG_NAME);
        fs::write(&installed_marker, PORTABLE_CONFIG_DEFAULT).unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());
        fs::remove_file(installed_marker).unwrap();

        fs::write(&helper, b"different helper").unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());

        fs::copy(&source, &helper).unwrap();
        assert_eq!(verify_msi_runtime_parity(&portable, &extracted).unwrap(), 5);
        let misplaced = misplaced_dir.join(DIAGNOSTICS_HELPER);
        fs::copy(&source, &misplaced).unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());
        fs::remove_file(&misplaced).unwrap();

        let signature = installed_runtime.join(diagnostics_pair::SIGNATURE_NAME);
        fs::remove_file(&signature).unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());
        fs::copy(
            portable_runtime.join(diagnostics_pair::SIGNATURE_NAME),
            &signature,
        )
        .unwrap();
        let bad_signature = misplaced_dir.join("DIAGNOSTICS-PAIR.TXT");
        fs::write(&bad_signature, []).unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());
        fs::remove_file(bad_signature).unwrap();

        let bad_crt = portable_runtime.join(PORTABLE_CRT_FILES[0].0);
        fs::write(&bad_crt, b"untrusted runtime").unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());
        fs::remove_file(bad_crt).unwrap();
        fs::write(installed_runtime.join(PORTABLE_CRT_FILES[0].0), []).unwrap();
        assert!(verify_msi_runtime_parity(&portable, &extracted).is_err());
    }

    #[test]
    fn leaks_and_forbidden_files_are_reported() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(
            dir.path().join("README-PORTABLE.md"),
            "see C:\\Users\\someone\\x",
        )
        .unwrap();
        fs::write(dir.path().join("exosnap.pdb"), "x").unwrap();
        fs::write(dir.path().join("Qt6Cored.dll"), "x").unwrap();
        fs::write(dir.path().join("exosnap_widgets_legacy.exe"), "x").unwrap();
        fs::write(dir.path().join("vc_redist.x64.exe"), "x").unwrap();
        fs::create_dir_all(dir.path().join("qml/QtTest")).unwrap();
        fs::create_dir_all(dir.path().join("plugins/qmltooling")).unwrap();
        fs::write(dir.path().join("plugins/qmltooling/qmldbg_tcp.dll"), "x").unwrap();
        fs::create_dir_all(dir.path().join("qml/QtQuick/Controls/Fusion")).unwrap();
        fs::create_dir_all(dir.path().join("qml/QtQuick/NativeStyle")).unwrap();
        fs::create_dir_all(dir.path().join("bin")).unwrap();
        fs::write(dir.path().join("bin/crashpad_handler.exe"), "x").unwrap();
        let errors = validate_tree(dir.path(), "0.10.0", &[r"C:\Users\".into()]).unwrap();
        for needle in [
            "leak",
            "development file",
            "debug Qt",
            "superseded",
            "qml/QtTest",
            "plugins/qmltooling",
            "qml/QtQuick/Controls/Fusion",
            "qml/QtQuick/NativeStyle",
            "forbidden directory in package: bin/",
            "vc_redist",
            "qmldbg_",
        ] {
            assert!(
                errors.iter().any(|e| e.contains(needle)),
                "{needle}: {errors:?}"
            );
        }
    }

    #[test]
    fn harvest_fragment_lists_nested_files_once() {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir_all(dir.path().join("plugins/platforms")).unwrap();
        fs::write(dir.path().join("exosnap.exe"), "x").unwrap();
        fs::write(dir.path().join("plugins/platforms/qwindows.dll"), "x").unwrap();
        let xml = harvest_fragment(dir.path()).unwrap();
        assert_eq!(xml.matches("<File ").count(), 2);
        assert!(xml.contains("<Directory Id=\"dir_plugins\" Name=\"plugins\">"));
        assert!(xml.contains("<Directory Id=\"dir_plugins_platforms\" Name=\"platforms\">"));
        assert!(xml.contains("<ComponentGroupRef Id=\"StagingFiles_plugins_platforms\" />"));
    }

    #[test]
    fn zip_has_one_top_level_directory() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("tree");
        fs::create_dir_all(root.join("sub")).unwrap();
        fs::write(root.join("a.txt"), "a").unwrap();
        fs::write(root.join("sub/b.txt"), "b").unwrap();
        let archive = dir.path().join("p.zip");
        write_zip(&root, "ExoSnap-0.10.0-windows-x64-portable", &archive).unwrap();
        let zip = zip::ZipArchive::new(fs::File::open(&archive).unwrap()).unwrap();
        let names: Vec<String> = zip.file_names().map(String::from).collect();
        assert!(
            names
                .iter()
                .all(|n| n.starts_with("ExoSnap-0.10.0-windows-x64-portable/")),
            "{names:?}"
        );
    }

    #[test]
    fn small_portable_entries_do_not_require_zip64() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("tree");
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("a.txt"), "a").unwrap();
        let archive = dir.path().join("p.zip");
        write_zip(&root, "ExoSnap-0.10.0-windows-x64-portable", &archive).unwrap();

        let bytes = fs::read(&archive).unwrap();
        assert_eq!(&bytes[..4], b"PK\x03\x04");
        assert_eq!(u16::from_le_bytes([bytes[4], bytes[5]]), 20);
    }

    fn packaging_source(relative: &str) -> String {
        fs::read_to_string(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../..")
                .join(relative),
        )
        .unwrap()
    }

    #[test]
    fn ownership_probe_requires_a_built_artifact_from_the_selected_tree() {
        let directory = tempfile::tempdir().unwrap();
        assert!(ownership_probe_path(directory.path()).is_err());
        let output = directory.path().join("packaging/msi");
        fs::create_dir_all(output.join("Release")).unwrap();
        let multi_config = output.join("Release/exosnap-msi-owner-probe.dll");
        fs::write(&multi_config, "fixture").unwrap();
        assert_eq!(
            ownership_probe_path(directory.path()).unwrap(),
            multi_config
        );
        let single_config = output.join("exosnap-msi-owner-probe.dll");
        fs::write(&single_config, "fixture").unwrap();
        assert_eq!(
            ownership_probe_path(directory.path()).unwrap(),
            single_config
        );
    }

    #[test]
    fn the_msi_identity_floor_and_off_by_default_options_are_pinned() {
        let package = packaging_source("packaging/msi/Package.wxs");
        assert!(
            package.contains(r#"UpgradeCode="8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D""#),
            "the permanent MSI UpgradeCode moved"
        );
        assert!(
            package.contains(r#"MinVersion="$(var.VCRedistMinVersion)""#),
            "the raw MSI must enforce the shared runtime floor"
        );
        assert!(
            package.contains(r#"Languages="1033""#),
            "the floor search must declare the runtime's 0409 version-resource language; a NULL Languages column does not match vcruntime140.dll"
        );
        assert!(package.contains(r#"Property Id="EXOSNAP_DESKTOP_SHORTCUT" Value="0""#));
        assert!(package.contains(r#"Property Id="EXOSNAP_REMOVE_USER_DATA" Value="0""#));
        assert!(package.contains(r#"Value="%LOCALAPPDATA%\ExoSnap""#));
        assert!(
            !package.contains("VCREDISTX64_INSTALLED"),
            "presence without a version floor is not a sufficient prerequisite check"
        );
    }

    #[test]
    fn the_setup_passes_the_direct_options_and_keeps_the_runtime_for_repair() {
        let setup = packaging_source("packaging/burn/Setup.wxs");
        assert!(setup.contains(r#"<MsiProperty Name="ARPSYSTEMCOMPONENT" Value="1" />"#));
        assert!(setup.contains(
            r#"<MsiProperty Name="EXOSNAP_DESKTOP_SHORTCUT" Value="[ExoSnapDesktopShortcut]" />"#
        ));
        assert!(setup.contains(
            r#"<MsiProperty Name="EXOSNAP_REMOVE_USER_DATA" Value="[ExoSnapRemoveUserData]" />"#
        ));
        assert!(setup.contains(r#"Name="ExoSnapDesktopShortcut" Type="numeric" Value="0""#));
        assert!(setup.contains(r#"Name="ExoSnapRemoveUserData" Type="numeric" Value="0""#));
        assert!(setup.contains(r#"Permanent="yes""#));
        assert!(
            !setup.contains(r#"Cache="remove""#),
            "a repair after the original Setup.exe is gone must still restore the runtime"
        );
        assert!(setup.contains(r#"<BootstrapperApplicationDll Id="ExoSnapNativeBA""#));
    }

    #[test]
    fn setup_payloads_preserve_nested_names_and_escape_xml_paths() {
        let root = Path::new(r"C:\runtime & ui");
        let file = root.join(r"qml\QtQuick\module.dll");
        let xml = setup_payload_xml(root, &[file]);
        assert!(xml.contains(r#"Name="qml\QtQuick\module.dll""#));
        assert!(xml.contains(r#"SourceFile="C:\runtime &amp; ui\qml\QtQuick\module.dll""#));
        assert!(xml.contains(r#"Compressed="yes""#));
    }

    #[test]
    fn setup_uses_the_prerequisite_safe_loader_and_offline_license() {
        let setup = packaging_source("packaging/burn/Setup.wxs");
        assert!(setup.contains(r#"SourceFile="$(var.BootstrapperPath)""#));
        assert!(setup.contains(r#"<PayloadGroupRef Id="ExoSnapSetupUiPayloads" />"#));
        assert!(!setup.contains("WixStandardBootstrapperApplication"));
        let cmake = packaging_source("apps/setup/CMakeLists.txt");
        assert!(cmake.contains("MSVC_RUNTIME_LIBRARY MultiThreaded"));
        assert!(cmake.contains("CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS"));
        assert!(cmake.contains("qt_add_resources(exosnap_setup_ui setup_license"));
        assert!(cmake.contains(r#"FILES "${PROJECT_SOURCE_DIR}/LICENSE""#));
    }

    #[test]
    fn launcher_uses_windows_dependencies_even_when_the_runtime_supplies_qt() {
        let root = tempfile::tempdir().unwrap();
        let launcher = root.path().join(LAUNCHER_NAME);
        let runtime = root.path().join(RUNTIME_DIR);
        fs::create_dir(&runtime).unwrap();
        fs::write(runtime.join("Qt6Core.dll"), pe::tests::synthetic_pe(&[])).unwrap();
        fs::write(
            &launcher,
            pe::tests::synthetic_pe(&["KERNEL32.dll", "Qt6Core.dll"]),
        )
        .unwrap();
        assert!(pe::audit_tree(root.path()).unwrap().unresolved.is_empty());
        assert!(validate_launcher_imports(&launcher).is_err());

        fs::write(
            &launcher,
            pe::tests::synthetic_pe(&["KERNEL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll"]),
        )
        .unwrap();
        validate_launcher_imports(&launcher).unwrap();
    }

    #[test]
    fn setup_runtime_cannot_depend_on_a_preinstalled_vc_runtime() {
        let root = tempfile::tempdir().unwrap();
        fs::write(
            root.path().join("exosnap-setup-ui.dll"),
            pe::tests::synthetic_pe(&["vcruntime140.dll"]),
        )
        .unwrap();
        assert!(validate_setup_runtime(root.path()).is_err());
        fs::create_dir(root.path().join("nested")).unwrap();
        fs::write(
            root.path().join("nested/vcruntime140.dll"),
            pe::tests::synthetic_pe(&[]),
        )
        .unwrap();
        assert!(validate_setup_runtime(root.path()).is_err());
        fs::rename(
            root.path().join("nested/vcruntime140.dll"),
            root.path().join("vcruntime140.dll"),
        )
        .unwrap();
        validate_setup_runtime(root.path()).unwrap();
        for name in [
            "msvcp140d.dll",
            "msvcp140d_atomic_wait.dll",
            "vcruntime140_1d.dll",
            "ucrtbased.dll",
        ] {
            assert!(is_debug_msvc(name));
        }
        assert!(!is_debug_msvc("msvcp140_atomic_wait.dll"));
    }

    #[test]
    fn setup_build_outputs_resolve_visual_studio_release_layout() {
        let root = tempfile::tempdir().unwrap();
        let dll = root
            .path()
            .join("apps/setup/runtime/Release/exosnap-setup-ui.dll");
        fs::create_dir_all(dll.parent().unwrap()).unwrap();
        fs::write(&dll, []).unwrap();
        assert_eq!(
            setup_build_file(root.path(), "runtime/exosnap-setup-ui.dll").unwrap(),
            dll
        );
        let loader = root.path().join("apps/setup/Release/exosnap-setup-ba.dll");
        fs::create_dir_all(loader.parent().unwrap()).unwrap();
        fs::write(&loader, []).unwrap();
        assert_eq!(
            setup_build_file(root.path(), "exosnap-setup-ba.dll").unwrap(),
            loader
        );
    }
}
