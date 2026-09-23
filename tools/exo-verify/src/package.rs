//! Packaging: the CMake install tree becomes the portable ZIP and the MSI.
//!
//! The install tree is the only authority for package contents; nothing is
//! hand-copied. Both packages are produced from the same pruned staging tree,
//! so they cannot disagree about which runtime files ship. Validation here
//! refuses to write a package that would be wrong on every machine; whether
//! the package works on a machine is judged by the release lanes.

use anyhow::{Context, Result, bail, ensure};
use serde::Serialize;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use crate::pe;

pub const PLATFORM: &str = "windows-x64";

#[derive(clap::Args)]
pub struct PackageArgs {
    /// Configured and built CMake Release tree.
    #[arg(long)]
    pub build_dir: PathBuf,
    /// Output directory (must be empty or absent).
    #[arg(long)]
    pub out: PathBuf,
    /// The version compiled into the binaries. Defaults to `<project version>-dev`.
    #[arg(long)]
    pub version: Option<String>,
    #[arg(long, default_value = ".")]
    pub repo_root: PathBuf,
    /// Skip the MSI (requires the WiX CLI otherwise).
    #[arg(long)]
    pub skip_msi: bool,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PackageResult {
    pub version: String,
    pub base_version: String,
    pub portable: PathBuf,
    pub installer: Option<PathBuf>,
    pub staging: PathBuf,
    pub file_count: usize,
    pub dependency_audit: BTreeMap<String, usize>,
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
const REQUIRED_FILES: &[&str] = &[
    "exosnap.exe",
    "exosnap-updater.exe",
    "qt.conf",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6Svg.dll",
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
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "KNOWN_LIMITATIONS.md",
    "README-PORTABLE.md",
    // A QML module directory without its qmldir cannot be resolved by the engine.
    "qml/QtQuick/qmldir",
    "qml/QtQuick/Controls/qmldir",
    "qml/QtQml/qmldir",
];

const REQUIRED_DIRS: &[&str] = &[
    "plugins/platforms",
    "licenses",
    "qml/QtQuick",
    "qml/QtQuick/Controls",
    "qml/QtQuick/Dialogs",
    "qml/QtQuick/Shapes",
    "qml/QtQml",
    // The countdown overlay's MultiEffect; a missing module fails inside a
    // capture-excluded overlay, where nobody can observe it.
    "qml/QtQuick/Effects",
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
const FORBIDDEN_EXECUTABLES: &[&str] = &["exosnap_quick_spike.exe", "exosnap_widgets_legacy.exe"];

const FORBIDDEN_DIRS: &[&str] = &[
    ".git",
    ".github",
    ".workspace",
    ".claude",
    "include",
    "lib",
    "src",
    "tests",
    "CMakeFiles",
    "Testing",
    "qml/QtTest",
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

/// Structural validation of a runtime tree. Returns every problem found; an
/// empty list means the tree is shippable as far as its contents can tell.
pub fn validate_tree(root: &Path, version: &str, leak_patterns: &[String]) -> Result<Vec<String>> {
    let mut errors = Vec::new();
    for f in REQUIRED_FILES {
        if !root.join(f).is_file() {
            errors.push(format!("missing required file: {f}"));
        }
    }
    for d in REQUIRED_DIRS {
        if !root.join(d).is_dir() {
            errors.push(format!("missing required directory: {d}"));
        }
    }
    for lic in REQUIRED_LICENSES {
        if !root.join("licenses").join(lic).is_file() {
            errors.push(format!("missing third-party license: licenses/{lic}"));
        }
    }
    if root.join("crashpad_handler.exe").is_file() {
        for lic in CRASH_CAPTURE_LICENSES {
            if !root.join("licenses").join(lic).is_file() {
                errors.push(format!(
                    "crash capture ships but its license is missing: licenses/{lic}"
                ));
            }
        }
    }
    for d in FORBIDDEN_DIRS {
        if root.join(d).is_dir() {
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
        if name == "qt6quicktest.dll" || name == "qt6test.dll" {
            errors.push(format!("Qt test framework in package: {rel}"));
        }
        if is_debug_qt(&name) {
            errors.push(format!("debug Qt DLL in package: {rel}"));
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
    for exe in ["exosnap.exe", "exosnap-updater.exe"] {
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
    if let Ok(limits) = fs::read_to_string(root.join("KNOWN_LIMITATIONS.md"))
        && !limits.contains(base)
    {
        errors.push(format!("KNOWN_LIMITATIONS.md does not name version {base}"));
    }
    Ok(errors)
}

/// Qt debug builds append `d` to the module name (`Qt6Cored.dll`); no Qt 6
/// release module name ends in `d`.
fn is_debug_qt(lower_name: &str) -> bool {
    lower_name.starts_with("qt6") && lower_name.ends_with("d.dll")
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
        .compression_method(zip::CompressionMethod::Deflated)
        .large_file(true);
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

/// Every executable and DLL of the staging tree must be inside the MSI.
pub fn msi_missing_binaries(staging: &Path, extracted: &Path) -> Result<Vec<String>> {
    let shipped: BTreeSet<String> = walk_files(extracted)?
        .iter()
        .filter_map(|p| {
            p.file_name()
                .map(|n| n.to_string_lossy().to_ascii_lowercase())
        })
        .collect();
    Ok(walk_files(staging)?
        .iter()
        .filter(|p| {
            p.extension()
                .and_then(|e| e.to_str())
                .is_some_and(|e| matches!(e.to_ascii_lowercase().as_str(), "exe" | "dll"))
        })
        .filter(|p| {
            !shipped.contains(
                &p.file_name()
                    .unwrap()
                    .to_string_lossy()
                    .to_ascii_lowercase(),
            )
        })
        .map(|p| relative(staging, p))
        .collect())
}

pub fn run(args: &PackageArgs) -> Result<PackageResult> {
    let repo_root = external_path(&fs::canonicalize(&args.repo_root)?);
    let base = project_version(&repo_root)?;
    let version = args
        .version
        .clone()
        .unwrap_or_else(|| format!("{base}-dev"));
    let re = regex::Regex::new(r"^([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?$").unwrap();
    let caps = re
        .captures(&version)
        .with_context(|| format!("'{version}' is not X.Y.Z or X.Y.Z-label"))?;
    ensure!(
        &caps[1] == base,
        "version '{version}' has base '{}', but CMakeLists.txt declares {base}",
        &caps[1]
    );
    if args.out.exists() {
        ensure!(
            fs::read_dir(&args.out)?.next().is_none(),
            "{} is not empty",
            args.out.display()
        );
    }
    fs::create_dir_all(&args.out)?;
    let out = external_path(&fs::canonicalize(&args.out)?);
    let top = portable_dir_name(&version);
    let staging = out.join("staging").join(&top);

    println!("==> cmake --install into {}", staging.display());
    run_checked(
        Command::new("cmake")
            .arg("--install")
            .arg(&args.build_dir)
            .args(["--config", "Release", "--prefix"])
            .arg(&staging),
        "cmake --install",
        Duration::from_secs(900),
    )?;
    // Vendored static dependencies install headers and import libraries the
    // product never loads; they carry no switch to turn that off.
    for dev in ["lib", "include"] {
        remove_if_exists(&staging.join(dev))?;
    }
    // Software OpenGL, translations and network bearer/TLS plugins are not used.
    for unused in [
        "opengl32sw.dll",
        "translations",
        "plugins/bearer",
        "plugins/tls",
    ] {
        remove_if_exists(&staging.join(unused))?;
    }

    let mut leaks = vec![
        r"C:\Users\".to_string(),
        ".workspace".into(),
        ".claude".into(),
        repo_root.display().to_string(),
    ];
    if let Ok(o) = Command::new("git")
        .args(["config", "user.email"])
        .current_dir(&repo_root)
        .output()
    {
        leaks.push(String::from_utf8_lossy(&o.stdout).trim().to_string());
    }
    println!("==> validating the install tree");
    let mut errors = validate_tree(&staging, &version, &leaks)?;
    println!("==> auditing static runtime dependencies");
    let audit = pe::audit_tree(&staging)?;
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

    let portable = out.join(format!("{top}.zip"));
    println!("==> {}", portable.display());
    write_zip(&staging, &top, &portable)?;

    let installer = if args.skip_msi {
        None
    } else {
        let wix = crate::tools::require("wix")?;
        let msi = out.join(format!("ExoSnap-{version}-{PLATFORM}.msi"));
        let fragment = out.join("_harvest.wxs");
        fs::write(&fragment, harvest_fragment(&staging)?)?;
        println!("==> {}", msi.display());
        // The Windows Installer ProductVersion is numeric; the full identity
        // lives in the file name and the executables' ProductVersion string.
        run_checked(
            Command::new(wix)
                .args(["build", "-arch", "x64", "-o"])
                .arg(&msi)
                .arg("-d")
                .arg(format!("ProductVersion={base}"))
                .arg(repo_root.join("packaging/msi/Package.wxs"))
                .arg(&fragment),
            "wix build",
            Duration::from_secs(900),
        )?;
        let extracted = out.join("msi-extract");
        msi_extract(&msi, &extracted)?;
        let missing = msi_missing_binaries(&staging, &extracted)?;
        fs::remove_dir_all(&extracted).ok();
        ensure!(
            missing.is_empty(),
            "the MSI lacks staged binaries: {}",
            missing.join(", ")
        );
        Some(msi)
    };

    let file_count = walk_files(&staging)?.len();
    let result = PackageResult {
        version,
        base_version: base,
        portable,
        installer,
        staging,
        file_count,
        dependency_audit: BTreeMap::from([
            ("binaries".into(), audit.binaries),
            ("shipped".into(), audit.shipped),
            ("windows".into(), audit.system),
            ("msvcRuntime".into(), audit.msvc),
        ]),
    };
    fs::write(
        out.join("package.json"),
        serde_json::to_vec_pretty(&result)?,
    )?;
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(!is_debug_qt("qt6quickshapes.dll"));
        assert!(!is_debug_qt("qt6core.dll"));
    }

    #[test]
    fn an_empty_tree_reports_every_missing_piece() {
        let dir = tempfile::tempdir().unwrap();
        let errors = validate_tree(dir.path(), "0.10.0", &[]).unwrap();
        assert!(errors.iter().any(|e| e.contains("exosnap.exe")));
        assert!(errors.iter().any(|e| e.contains("qml/QtQuick/Effects")));
        assert!(errors.iter().any(|e| e.contains("licenses/ffmpeg.txt")));
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
        fs::create_dir_all(dir.path().join("qml/QtTest")).unwrap();
        let errors = validate_tree(dir.path(), "0.10.0", &[r"C:\Users\".into()]).unwrap();
        for needle in [
            "leak",
            "development file",
            "debug Qt",
            "superseded",
            "qml/QtTest",
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
}
