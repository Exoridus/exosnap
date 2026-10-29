//! Size inventory of the validated and pruned shipping runtime.
//! The Qt Widgets/Labs total is a candidate footprint, not a proven saving.

use anyhow::{Context, Result, ensure};
use serde::Serialize;
use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;

use super::PackageResult;

const SCHEMA: &str = "exosnap.package-size/1";

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct FileSize {
    path: String,
    bytes: u64,
    category: &'static str,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct GroupSize {
    name: &'static str,
    bytes: u64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RuntimeSize {
    total_bytes: u64,
    file_count: usize,
    categories: Vec<GroupSize>,
    largest_files: Vec<FileSize>,
    qt_widgets_and_labs_gross_bytes: u64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ArtifactSize {
    name: &'static str,
    file: String,
    bytes: u64,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Report {
    schema: &'static str,
    version: String,
    runtime: RuntimeSize,
    artifacts: Vec<ArtifactSize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    setup_ui_runtime: Option<RuntimeSize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    vc_redistributable_bytes: Option<u64>,
}

fn category(path: &str) -> &'static str {
    let lower = path.replace('\\', "/").to_ascii_lowercase();
    let name = lower.rsplit('/').next().unwrap_or(&lower);
    if lower.starts_with("qml/qt/labs/platform/") {
        "Qt Labs Platform"
    } else if name == "qt6widgets.dll" {
        "Qt Widgets"
    } else if lower.starts_with("qml/") {
        "QML modules"
    } else if lower.starts_with("plugins/") {
        "Qt plugins"
    } else if name.starts_with("qt6quick") || name.starts_with("qt6shader") {
        "Qt Quick libraries"
    } else if name.starts_with("qt6") || name.starts_with("icu") {
        "Other Qt libraries"
    } else if ["avcodec", "avformat", "avutil", "swresample", "swscale", "ffmpeg"]
        .iter()
        .any(|prefix| name.starts_with(prefix))
    {
        "FFmpeg libraries"
    } else if name.starts_with("gameinput") || name.starts_with("xinput") {
        "Controller runtime"
    } else if name.starts_with("crashpad") || name.starts_with("sentry") {
        "Crash reporting"
    } else if lower.starts_with("licenses/")
        || [
            "license",
            "third_party_notices.md",
            "readme-portable.md",
            "known_limitations.md",
        ]
        .contains(&name)
    {
        "Legal and documentation"
    } else if name.ends_with(".exe") {
        "Executables"
    } else if name.ends_with(".dll") {
        "Other libraries"
    } else {
        "Other resources"
    }
}

fn scan(root: &Path) -> Result<RuntimeSize> {
    ensure!(root.is_dir(), "runtime directory missing: {}", root.display());
    let mut totals: BTreeMap<&'static str, u64> = BTreeMap::new();
    let mut files = Vec::new();
    let mut total_bytes = 0u64;
    let mut candidate = 0u64;
    for file in super::walk_files(root)? {
        ensure!(
            !file.symlink_metadata()?.file_type().is_symlink(),
            "symlink in runtime tree: {}",
            file.display()
        );
        let bytes = file.metadata()?.len();
        let path = super::relative(root, &file);
        let group = category(&path);
        *totals.entry(group).or_default() += bytes;
        total_bytes += bytes;
        if matches!(group, "Qt Widgets" | "Qt Labs Platform") {
            candidate += bytes;
        }
        files.push(FileSize {
            path,
            bytes,
            category: group,
        });
    }
    let file_count = files.len();
    ensure!(file_count > 0, "runtime directory is empty");
    files.sort_by(|a, b| b.bytes.cmp(&a.bytes).then_with(|| a.path.cmp(&b.path)));
    files.truncate(20);
    let mut categories: Vec<_> = totals
        .into_iter()
        .map(|(name, bytes)| GroupSize { name, bytes })
        .collect();
    categories.sort_by(|a, b| b.bytes.cmp(&a.bytes).then_with(|| a.name.cmp(b.name)));
    Ok(RuntimeSize {
        total_bytes,
        file_count,
        categories,
        largest_files: files,
        qt_widgets_and_labs_gross_bytes: candidate,
    })
}

fn artifact(name: &'static str, file: &Path) -> Result<ArtifactSize> {
    Ok(ArtifactSize {
        name,
        file: file
            .file_name()
            .context("package artifact lacks filename")?
            .to_string_lossy()
            .into_owned(),
        bytes: file
            .metadata()
            .with_context(|| format!("package artifact missing: {}", file.display()))?
            .len(),
    })
}

fn mib(bytes: u64) -> String {
    format!("{:.2} MiB", bytes as f64 / (1024.0 * 1024.0))
}

fn markdown(report: &Report) -> String {
    let mut out = String::new();
    let _ = writeln!(out, "## ExoSnap package footprint - {}\n", report.version);
    out.push_str("Sizes are measured from the **pruned shipping runtime**. Compressed packages and uncompressed files are separate measurements.\n\n");
    out.push_str("### Packaged downloads\n\n| Artifact | Bytes | Size |\n| --- | ---: | ---: |\n");
    for item in &report.artifacts {
        let _ = writeln!(out, "| {} | {} | {} |", item.name, item.bytes, mib(item.bytes));
    }
    let _ = writeln!(
        out,
        "\n### Uncompressed application runtime\n\n**{} files - {} bytes ({})**\n",
        report.runtime.file_count,
        report.runtime.total_bytes,
        mib(report.runtime.total_bytes)
    );
    out.push_str("| Category | Bytes | Size |\n| --- | ---: | ---: |\n");
    for item in &report.runtime.categories {
        let _ = writeln!(out, "| {} | {} | {} |", item.name, item.bytes, mib(item.bytes));
    }
    out.push_str("\n### Largest shipped files\n\n| File | Size |\n| --- | ---: |\n");
    for item in report.runtime.largest_files.iter().take(15) {
        let _ = writeln!(out, "| {} | {} |\n", item.path, mib(item.bytes));
    }
    let _ = writeln!(
        out,
        "\n### Qt Widgets / Qt Labs candidate footprint\n\n**{} uncompressed** for the grouped DLL and QML module. This is **not** a proven removal or compressed ZIP saving. Static imports and dynamically loaded Qt plugins must be verified before removal.\n",
        mib(report.runtime.qt_widgets_and_labs_gross_bytes)
    );
    if let Some(setup) = &report.setup_ui_runtime {
        let _ = writeln!(
            out,
            "Separate Setup UI runtime: **{} uncompressed** ({} files).",
            mib(setup.total_bytes),
            setup.file_count
        );
    }
    if let Some(bytes) = report.vc_redistributable_bytes {
        let _ = writeln!(out, "Visual C++ redistributable input: **{}**.", mib(bytes));
    }
    out
}

/// Write package-size evidence outside the immutable release candidate bundle.
pub(super) fn write(out: &Path, package: &PackageResult, vc_redist: Option<&Path>) -> Result<()> {
    let mut artifacts = vec![artifact("Portable ZIP", &package.portable)?];
    if let Some(msi) = &package.installer {
        artifacts.push(artifact("MSI", msi)?);
    }
    if let Some(setup) = &package.setup {
        artifacts.push(artifact("Offline Setup", setup)?);
    }
    let setup_dir = out.join("setup-runtime");
    let setup_ui_runtime = if setup_dir.is_dir() {
        Some(scan(&setup_dir)?)
    } else {
        None
    };
    let vc_redistributable_bytes = vc_redist
        .map(|p| {
            p.metadata()
                .map(|m| m.len())
                .with_context(|| format!("VC runtime missing: {}", p.display()))
        })
        .transpose()?;
    let report = Report {
        schema: SCHEMA,
        version: package.version.clone(),
        runtime: scan(&package.staging)?,
        artifacts,
        setup_ui_runtime,
        vc_redistributable_bytes,
    };
    fs::write(
        out.join("package-size.json"),
        serde_json::to_vec_pretty(&report)?,
    )?;
    fs::write(out.join("package-size.md"), markdown(&report))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn categories_separate_labs_and_widgets() {
        assert_eq!(category("Qt6Widgets.dll"), "Qt Widgets");
        assert_eq!(category("qml/Qt/labs/platform/plugin.dll"), "Qt Labs Platform");
        assert_eq!(category("qml/QtQuick/Controls/Basic/qmldir"), "QML modules");
        assert_eq!(category("Qt6Quick.dll"), "Qt Quick libraries");
    }

    #[test]
    fn report_counts_pruned_files_and_packages_separately() {
        let temp = tempfile::tempdir().unwrap();
        let stage = temp.path().join("staging");
        fs::create_dir_all(stage.join("qml/Qt/labs/platform")).unwrap();
        fs::write(stage.join("Qt6Widgets.dll"), [b'w'; 10]).unwrap();
        fs::write(stage.join("qml/Qt/labs/platform/plugin.dll"), [b'l'; 4]).unwrap();
        fs::write(stage.join("exosnap.exe"), [b'e'; 18]).unwrap();
        let zip = temp.path().join("portable.zip");
        fs::write(&zip, [b'z'; 7]).unwrap();
        let package = PackageResult {
            version: "0.10.1".into(),
            base_version: "0.10.1".into(),
            portable: zip,
            installer: None,
            setup: None,
            staging: stage,
            file_count: 3,
            dependency_audit: BTreeMap::new(),
            dependencies: Vec::new(),
        };
        write(temp.path(), &package, None).unwrap();
        let report: serde_json::Value =
            serde_json::from_slice(&fs::read(temp.path().join("package-size.json")).unwrap())
                .unwrap();
        assert_eq!(report["runtime"]["totalBytes"], 32);
        assert_eq!(report["runtime"]["qtWidgetsAndLabsGrossBytes"], 14);
        assert_eq!(report["artifacts"][0]["bytes"], 7);
        assert!(!report.to_string().contains(&temp.path().display().to_string()));
        let summary = fs::read_to_string(temp.path().join("package-size.md")).unwrap();
        assert!(summary.contains("not** a proven removal"));
    }

    #[test]
    fn refuses_missing_portable_artifact() {
        let temp = tempfile::tempdir().unwrap();
        assert!(artifact("Portable ZIP", &temp.path().join("missing.zip")).is_err());
    }
}
