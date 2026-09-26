//! WinGet manifest validator: the canonical Codexo.ExoSnap manifest set
//! against the target ExoSnap version.
//!
//! Checked over packaging/winget/manifests/c/Codexo/ExoSnap/<version>/: all
//! three manifest files exist (version, installer, locale en-US);
//! `PackageIdentifier` is `Codexo.ExoSnap` and `PackageVersion` is identical
//! in all three; `ManifestVersion` is `1.10.0` in all three; the installer's
//! `InstallerSha256` is 64 uppercase hex characters; `ProductCode` and
//! `UpgradeCode` are well-formed braced GUIDs; the installer declares
//! `Microsoft.VCRedist.2015+.x64` under `Dependencies/PackageDependencies`
//! (a regression guard: ExoSnap links the dynamic MSVC runtime and does not
//! bundle it, so a clean machine without this dependency fails to start with
//! STATUS_DLL_NOT_FOUND); `InstallerUrl` matches the expected GitHub Release
//! MSI asset; `AppsAndFeaturesEntries[].DisplayVersion` and the locale
//! manifest's `ReleaseNotesUrl` both name the target version.
//!
//! WinGet keys a submission on the manifest directory name, so a directory
//! that disagrees with the target version is reported and nothing inside it
//! is inspected further: a release nobody built cannot also be internally
//! consistent in any way that matters.

use std::path::Path;
use std::sync::LazyLock;

use regex::Regex;

use super::ValidationReport;

fn get_yaml_value(
    report: &mut ValidationReport,
    text: &str,
    key: &str,
    file_label: &str,
) -> Option<String> {
    let pattern = format!(r"(?m)^\s*{}:\s*(\S+)\s*$", regex::escape(key));
    let re = Regex::new(&pattern).unwrap();
    match re.captures(text) {
        Some(m) => Some(m[1].trim_matches(|c| c == '\'' || c == '"').to_string()),
        None => {
            report
                .errors
                .push(format!("{file_label}: missing key '{key}'"));
            None
        }
    }
}

pub fn validate_winget(repo_root: &Path, version: &str) -> anyhow::Result<ValidationReport> {
    let manifest_root = repo_root.join("packaging/winget/manifests/c/Codexo/ExoSnap");
    if !manifest_root.is_dir() {
        anyhow::bail!(
            "WinGet manifest root not found: {}",
            manifest_root.display()
        );
    }

    let mut version_dirs: Vec<String> = std::fs::read_dir(&manifest_root)?
        .filter_map(|entry| {
            let entry = entry.ok()?;
            if entry.file_type().ok()?.is_dir() {
                Some(entry.file_name().to_string_lossy().to_string())
            } else {
                None
            }
        })
        .collect();
    version_dirs.sort();
    if version_dirs.len() != 1 {
        anyhow::bail!(
            "Expected exactly one version directory under {}, found: {}. A bump moves the existing directory (git mv), it does not add a second one.",
            manifest_root.display(),
            version_dirs.join(", ")
        );
    }

    let mut report = ValidationReport::default();
    if version_dirs[0] != version {
        report.errors.push(format!(
            "manifest directory is '{}' but the target version is '{version}'",
            version_dirs[0]
        ));
        return Ok(report);
    }

    let version_dir = manifest_root.join(version);
    let version_path = version_dir.join("Codexo.ExoSnap.yaml");
    let installer_path = version_dir.join("Codexo.ExoSnap.installer.yaml");
    let locale_path = version_dir.join("Codexo.ExoSnap.locale.en-US.yaml");

    let mut missing = Vec::new();
    for (path, label) in [
        (&version_path, "version manifest"),
        (&installer_path, "installer manifest"),
        (&locale_path, "locale en-US manifest"),
    ] {
        if !path.is_file() {
            missing.push(format!("Missing {label}: {}", path.display()));
        }
    }
    if !missing.is_empty() {
        anyhow::bail!(missing.join("; "));
    }

    let version_text = std::fs::read_to_string(&version_path)?;
    let installer_text = std::fs::read_to_string(&installer_path)?;
    let locale_text = std::fs::read_to_string(&locale_path)?;

    for (text, label) in [
        (&version_text, "version manifest"),
        (&installer_text, "installer manifest"),
        (&locale_text, "locale manifest"),
    ] {
        if let Some(id) = get_yaml_value(&mut report, text, "PackageIdentifier", label) {
            if id != "Codexo.ExoSnap" {
                report.errors.push(format!(
                    "{label}: PackageIdentifier '{id}' != 'Codexo.ExoSnap'"
                ));
            }
        }
    }

    let version_version = get_yaml_value(
        &mut report,
        &version_text,
        "PackageVersion",
        "version manifest",
    );
    let installer_version = get_yaml_value(
        &mut report,
        &installer_text,
        "PackageVersion",
        "installer manifest",
    );
    let locale_version = get_yaml_value(
        &mut report,
        &locale_text,
        "PackageVersion",
        "locale manifest",
    );
    if let (Some(v), Some(i)) = (&version_version, &installer_version) {
        if v != i {
            report.errors.push(format!(
                "PackageVersion mismatch: version manifest '{v}' != installer manifest '{i}'"
            ));
        }
    }
    if let (Some(v), Some(l)) = (&version_version, &locale_version) {
        if v != l {
            report.errors.push(format!(
                "PackageVersion mismatch: version manifest '{v}' != locale manifest '{l}'"
            ));
        }
    }
    if let Some(v) = &version_version {
        if v != version {
            report.errors.push(format!(
                "PackageVersion '{v}' does not match manifest directory name '{version}'"
            ));
        }
    }

    for (text, label) in [
        (&version_text, "version manifest"),
        (&installer_text, "installer manifest"),
        (&locale_text, "locale manifest"),
    ] {
        if let Some(mv) = get_yaml_value(&mut report, text, "ManifestVersion", label) {
            if mv != "1.10.0" {
                report
                    .errors
                    .push(format!("{label}: ManifestVersion '{mv}' != '1.10.0'"));
            }
        }
    }

    static HEX64_UPPER: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[0-9A-F]{64}$").unwrap());
    if let Some(sha) = get_yaml_value(
        &mut report,
        &installer_text,
        "InstallerSha256",
        "installer manifest",
    ) {
        if !HEX64_UPPER.is_match(&sha) {
            report.errors.push(format!(
                "installer manifest: InstallerSha256 '{sha}' is not 64 uppercase hex characters"
            ));
        }
    }

    static GUID: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(
            r"^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$",
        )
        .unwrap()
    });
    static PRODUCT_CODE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?m)^\s*ProductCode:\s*'([^']+)'").unwrap());
    let product_codes: Vec<&str> = PRODUCT_CODE
        .captures_iter(&installer_text)
        .map(|m| m.get(1).unwrap().as_str())
        .collect();
    if product_codes.is_empty() {
        report
            .errors
            .push("installer manifest: no ProductCode entries found".into());
    }
    for guid in &product_codes {
        if !GUID.is_match(guid) {
            report.errors.push(format!(
                "installer manifest: ProductCode '{guid}' is not a well-formed braced GUID"
            ));
        }
    }
    static UPGRADE_CODE: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?m)^\s*UpgradeCode:\s*'([^']+)'").unwrap());
    let upgrade_codes: Vec<&str> = UPGRADE_CODE
        .captures_iter(&installer_text)
        .map(|m| m.get(1).unwrap().as_str())
        .collect();
    if upgrade_codes.is_empty() {
        report
            .errors
            .push("installer manifest: no UpgradeCode entries found".into());
    }
    for guid in &upgrade_codes {
        if !GUID.is_match(guid) {
            report.errors.push(format!(
                "installer manifest: UpgradeCode '{guid}' is not a well-formed braced GUID"
            ));
        }
    }

    if !Regex::new(r"(?m)^\s*PackageDependencies:\s*$")
        .unwrap()
        .is_match(&installer_text)
    {
        report.errors.push(
            "installer manifest: missing Dependencies/PackageDependencies block (regression: removing this reintroduces STATUS_DLL_NOT_FOUND on clean machines)"
                .into(),
        );
    } else if !Regex::new(r"(?m)^\s*-\s*PackageIdentifier:\s*Microsoft\.VCRedist\.2015\+\.x64\s*$")
        .unwrap()
        .is_match(&installer_text)
    {
        report.errors.push(
            "installer manifest: Dependencies/PackageDependencies does not declare Microsoft.VCRedist.2015+.x64 (regression: removing this reintroduces STATUS_DLL_NOT_FOUND on clean machines)"
                .into(),
        );
    }

    if let Some(installer_url) = get_yaml_value(
        &mut report,
        &installer_text,
        "InstallerUrl",
        "installer manifest",
    ) {
        let expected = format!(
            "https://github.com/Exoridus/exosnap/releases/download/v{version}/ExoSnap-{version}-windows-x64.msi"
        );
        if installer_url != expected {
            report.errors.push(format!(
                "installer manifest: InstallerUrl '{installer_url}' != expected '{expected}'"
            ));
        }
    }

    if let Some(display_version) = get_yaml_value(
        &mut report,
        &installer_text,
        "DisplayVersion",
        "installer manifest",
    ) {
        if display_version != version {
            report.errors.push(format!(
                "installer manifest: DisplayVersion '{display_version}' != '{version}'"
            ));
        }
    }
    if let Some(release_notes_url) = get_yaml_value(
        &mut report,
        &locale_text,
        "ReleaseNotesUrl",
        "locale manifest",
    ) {
        let expected = format!("https://github.com/Exoridus/exosnap/releases/tag/v{version}");
        if release_notes_url != expected {
            report.errors.push(format!(
                "locale manifest: ReleaseNotesUrl '{release_notes_url}' != expected '{expected}'"
            ));
        }
    }

    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    const VERSION: &str = "0.10.0";

    const VERSION_MANIFEST: &str = concat!(
        "PackageIdentifier: Codexo.ExoSnap\n",
        "PackageVersion: 0.10.0\n",
        "DefaultLocale: en-US\n",
        "ManifestType: version\n",
        "ManifestVersion: 1.10.0\n",
    );

    const INSTALLER_MANIFEST: &str = concat!(
        "PackageIdentifier: Codexo.ExoSnap\n",
        "PackageVersion: 0.10.0\n",
        "InstallerType: wix\n",
        "Dependencies:\n",
        "  PackageDependencies:\n",
        "    - PackageIdentifier: Microsoft.VCRedist.2015+.x64\n",
        "Installers:\n",
        "  - Architecture: x64\n",
        "    InstallerUrl: https://github.com/Exoridus/exosnap/releases/download/v0.10.0/ExoSnap-0.10.0-windows-x64.msi\n",
        "    InstallerSha256: '0000000000000000000000000000000000000000000000000000000000000000'\n",
        "    ProductCode: '{00000000-0000-0000-0000-000000000000}'\n",
        "    AppsAndFeaturesEntries:\n",
        "      - DisplayName: ExoSnap\n",
        "        DisplayVersion: 0.10.0\n",
        "        ProductCode: '{00000000-0000-0000-0000-000000000000}'\n",
        "        UpgradeCode: '{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}'\n",
        "ManifestType: installer\n",
        "ManifestVersion: 1.10.0\n",
    );

    const LOCALE_MANIFEST: &str = concat!(
        "PackageIdentifier: Codexo.ExoSnap\n",
        "PackageVersion: 0.10.0\n",
        "PackageLocale: en-US\n",
        "ReleaseNotesUrl: https://github.com/Exoridus/exosnap/releases/tag/v0.10.0\n",
        "ManifestType: defaultLocale\n",
        "ManifestVersion: 1.10.0\n",
    );

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let manifest_dir = dir
            .path()
            .join("packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0");
        std::fs::create_dir_all(&manifest_dir).unwrap();
        std::fs::write(manifest_dir.join("Codexo.ExoSnap.yaml"), VERSION_MANIFEST).unwrap();
        std::fs::write(
            manifest_dir.join("Codexo.ExoSnap.installer.yaml"),
            INSTALLER_MANIFEST,
        )
        .unwrap();
        std::fs::write(
            manifest_dir.join("Codexo.ExoSnap.locale.en-US.yaml"),
            LOCALE_MANIFEST,
        )
        .unwrap();
        dir
    }

    #[test]
    fn a_consistent_manifest_set_passes() {
        let dir = fixture();
        let report = validate_winget(dir.path(), VERSION).unwrap();
        assert!(report.ok(), "{:?}", report.errors);
    }

    #[test]
    fn a_missing_manifest_root_is_a_hard_error() {
        let dir = tempfile::tempdir().unwrap();
        let error = validate_winget(dir.path(), VERSION).unwrap_err();
        assert!(error.to_string().contains("manifest root not found"));
    }

    #[test]
    fn a_missing_manifest_file_is_a_hard_error() {
        let dir = fixture();
        std::fs::remove_file(dir.path().join(
            "packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0/Codexo.ExoSnap.installer.yaml",
        ))
        .unwrap();
        let error = validate_winget(dir.path(), VERSION).unwrap_err();
        assert!(error.to_string().contains("installer manifest"));
    }

    #[test]
    fn a_second_version_directory_is_a_hard_error() {
        let dir = fixture();
        std::fs::create_dir_all(
            dir.path()
                .join("packaging/winget/manifests/c/Codexo/ExoSnap/0.9.0"),
        )
        .unwrap();
        let error = validate_winget(dir.path(), VERSION).unwrap_err();
        assert!(
            error.to_string().contains("found exactly one") || error.to_string().contains("found:")
        );
    }

    #[test]
    fn a_manifest_directory_disagreeing_with_the_target_version_is_reported() {
        let dir = fixture();
        let report = validate_winget(dir.path(), "0.9.0").unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("manifest directory is"))
        );
    }

    #[test]
    fn a_missing_vcredist_dependency_is_rejected() {
        let dir = fixture();
        let installer_path = dir.path().join(
            "packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0/Codexo.ExoSnap.installer.yaml",
        );
        std::fs::write(
            &installer_path,
            INSTALLER_MANIFEST.replace(
                "    - PackageIdentifier: Microsoft.VCRedist.2015+.x64\n",
                "",
            ),
        )
        .unwrap();
        let report = validate_winget(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("Microsoft.VCRedist.2015+.x64"))
        );
    }

    #[test]
    fn a_malformed_product_code_is_rejected() {
        let dir = fixture();
        let installer_path = dir.path().join(
            "packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0/Codexo.ExoSnap.installer.yaml",
        );
        std::fs::write(
            &installer_path,
            INSTALLER_MANIFEST.replace(
                "ProductCode: '{00000000-0000-0000-0000-000000000000}'",
                "ProductCode: 'not-a-guid'",
            ),
        )
        .unwrap();
        let report = validate_winget(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("not a well-formed braced GUID"))
        );
    }

    #[test]
    fn a_release_notes_url_naming_the_wrong_version_is_rejected() {
        let dir = fixture();
        let locale_path = dir.path().join(
            "packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0/Codexo.ExoSnap.locale.en-US.yaml",
        );
        std::fs::write(
            &locale_path,
            LOCALE_MANIFEST.replace("releases/tag/v0.10.0", "releases/tag/v0.9.0"),
        )
        .unwrap();
        let report = validate_winget(dir.path(), VERSION).unwrap();
        assert!(!report.ok());
        assert!(report.errors.iter().any(|e| e.contains("ReleaseNotesUrl")));
    }
}
