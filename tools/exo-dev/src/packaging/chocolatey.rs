//! Chocolatey package validator: packaging/chocolatey/ against the target
//! ExoSnap version, plus the mechanically checkable subset of the
//! Chocolatey community moderation requirements.
//!
//! Checked: exosnap.nuspec's `<version>`, `<iconUrl>` (exact jsDelivr `@v`
//! tag) and `<releaseNotes>` (exact GitHub Release tag URL);
//! chocolateyinstall.ps1's `url64bit` (exact GitHub Release MSI asset URL),
//! `checksum64` (64 lowercase hex characters, never a single-character
//! placeholder run, cross-checked against a release artifact manifest's
//! `msiSha256` when one is available) and `checksumType64` (`sha256`); no
//! other version-looking literal left in packaging/chocolatey/ (a historical
//! changelog bullet and the `vcredist140` dependency version are exempt);
//! every nuspec field the community feed requires or expects, HTTPS-only
//! metadata URLs, a CDN-served icon with a permitted extension, lowercase
//! space-separated tags without "chocolatey", description length and
//! Markdown heading spacing, every `<dependency>` pinning a version, no
//! leftover template placeholder or e-mail address; the automation scripts
//! opening with `$ErrorActionPreference = 'Stop'` and using none of
//! Write-Host, a `choco` command, a Chocolatey module import, a private
//! Chocolatey environment variable or internal variable, the deprecated
//! `Get-BinRoot`, `Get-WmiObject`, a raw `msiexec` call, or a plain-http URL;
//! an uninstall script exists at all; and nothing under tools/ besides a
//! `.ps1` file, with no source-control or OS index file.
//!
//! The checksum64 placeholder guard is unconditional: `-VersionOnly` skips
//! examining checksum64 at all, but never relaxes what counts as a
//! placeholder once it is examined. Between a version bump and the published
//! release the MSI does not exist yet, so the tree legitimately carries the
//! placeholder, but it must never reach `choco pack` or a submission.

use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use regex::Regex;
use serde_json::Value;

use super::{ValidationReport, bare_version_matches};

/// How much of the full Chocolatey validation runs, matching
/// `validate-chocolatey-package.ps1`'s own `-VersionOnly`, `-ManifestPath`
/// and `-RequireManifest` parameters.
#[derive(Clone, Debug, Default)]
pub struct ChocolateyMode {
    /// Skips the checksum64-vs-manifest cross-check entirely (both the "not
    /// examined" and the "skipped" paths). The packaging-version aggregator
    /// always passes `true`; the standalone CLI defaults it to `false`.
    pub version_only: bool,
    /// Explicit override; `None` means the script's own default of the
    /// local release build's own artifact manifest for that version.
    pub manifest_path: Option<PathBuf>,
    /// Promotes "no manifest found" from a skip to a hard error. Ignored
    /// when `version_only` is `true`.
    pub require_manifest: bool,
}

fn xml_element(text: &str, tag: &str) -> Option<String> {
    let pattern = format!(r"(?s)<{tag}>(.*?)</{tag}>", tag = regex::escape(tag));
    let re = Regex::new(&pattern).unwrap();
    let raw = re.captures(text)?[1].to_string();
    let trimmed = raw.trim();
    let unwrapped = trimmed
        .strip_prefix("<![CDATA[")
        .and_then(|rest| rest.strip_suffix("]]>"))
        .unwrap_or(trimmed);
    Some(unwrapped.trim().to_string())
}

/// `<dependency id="..." version="..." />` entries under `<dependencies>`.
fn dependencies(text: &str) -> Vec<(String, Option<String>)> {
    static DEPENDENCY: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"<dependency\s+([^>]*?)/>").unwrap());
    static ATTR: LazyLock<Regex> = LazyLock::new(|| Regex::new(r#"(\w+)="([^"]*)""#).unwrap());
    DEPENDENCY
        .captures_iter(text)
        .map(|m| {
            let attrs = &m[1];
            let mut id = String::new();
            let mut version = None;
            for a in ATTR.captures_iter(attrs) {
                match &a[1] {
                    "id" => id = a[2].to_string(),
                    "version" => version = Some(a[2].to_string()),
                    _ => {}
                }
            }
            (id, version)
        })
        .collect()
}

fn is_degenerate(checksum: &str) -> bool {
    checksum
        .chars()
        .collect::<std::collections::HashSet<_>>()
        .len()
        == 1
}

fn find_stale_version_references(
    report: &mut ValidationReport,
    text: &str,
    label: &str,
    target_version: &str,
    exclude_line_substrings: &[&str],
) {
    static CHANGELOG_BULLET: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"^\s*\*\s+\*\*\d+\.\d+\.\d+:\*\*").unwrap());
    for (idx, line) in text.lines().enumerate() {
        if CHANGELOG_BULLET.is_match(line) {
            continue;
        }
        if line.contains("vcredist140") {
            continue;
        }
        if exclude_line_substrings.iter().any(|sub| line.contains(sub)) {
            continue;
        }
        for (_, value) in bare_version_matches(line) {
            if value != target_version {
                report.errors.push(format!(
                    "{label}:{}: stale version reference '{value}' (target is '{target_version}') in: {}",
                    idx + 1,
                    line.trim()
                ));
            }
        }
    }
}

struct ScriptCheck {
    pattern: &'static str,
    message: &'static str,
}

static SCRIPT_CHECKS: &[ScriptCheck] = &[
    ScriptCheck {
        pattern: "Write-Host",
        message: "uses Write-Host; Chocolatey's own output helpers or Write-Warning are expected",
    },
    ScriptCheck {
        pattern: r"(?m)^\s*choco(latey)?(\.exe)?\s+(install|upgrade|uninstall|push|pack)\b",
        message: "calls a choco command from an automation script",
    },
    ScriptCheck {
        pattern: r"Import-Module\s+.*chocolatey",
        message: "imports a Chocolatey module; the helpers are already in scope",
    },
    ScriptCheck {
        pattern: r"\$env:(chocolateyPackageFolder|packageFolder|chocolateyToolsLocation|chocolateyBinRoot|chocolatey_bin_root|chocolateyChecksum(32|64)|chocolateyChecksumType(32|64)|downloadCacheAvailable)\b",
        message: "reads a private Chocolatey environment variable",
    },
    ScriptCheck {
        pattern: r"\$(nugetChocolateyPath|nugetPath|nugetExePath|nugetLibPath|chocInstallVariableName|nugetExe)\b",
        message: "uses an internal Chocolatey variable",
    },
    ScriptCheck {
        pattern: "Get-BinRoot",
        message: "uses the deprecated Get-BinRoot; use Get-ToolsLocation",
    },
    ScriptCheck {
        pattern: "Get-WmiObject",
        message: "uses Get-WmiObject to find installed software; use Get-UninstallRegistryKey",
    },
    ScriptCheck {
        pattern: r"(?m)^\s*[^#]*\bmsiexec\b",
        message: "shells out to msiexec; Install-/Uninstall-ChocolateyPackage does that correctly",
    },
    ScriptCheck {
        pattern: "http://",
        message: "contains a plain-http URL",
    },
];

pub fn validate_chocolatey(
    repo_root: &Path,
    version: &str,
    mode: ChocolateyMode,
) -> anyhow::Result<ValidationReport> {
    let choco_root = repo_root.join("packaging/chocolatey");
    let nuspec_path = choco_root.join("exosnap.nuspec");
    let tools_root = choco_root.join("tools");
    let install_path = tools_root.join("chocolateyinstall.ps1");
    let uninstall_path = tools_root.join("chocolateyuninstall.ps1");

    let mut missing = Vec::new();
    for (path, label) in [
        (&nuspec_path, "exosnap.nuspec"),
        (&install_path, "tools/chocolateyinstall.ps1"),
        (&uninstall_path, "tools/chocolateyuninstall.ps1"),
    ] {
        if !path.is_file() {
            missing.push(format!("Missing {label}: {}", path.display()));
        }
    }
    if !missing.is_empty() {
        anyhow::bail!(missing.join("; "));
    }

    let nuspec_text = std::fs::read_to_string(&nuspec_path)?;
    let install_text = std::fs::read_to_string(&install_path)?;
    let uninstall_text = std::fs::read_to_string(&uninstall_path)?;

    let mut report = ValidationReport::default();

    let nuspec_version = xml_element(&nuspec_text, "version").unwrap_or_default();
    if nuspec_version != version {
        report.errors.push(format!(
            "exosnap.nuspec: <version>{nuspec_version}</version> != target version '{version}'"
        ));
    }

    let expected_icon = format!(
        "https://cdn.jsdelivr.net/gh/Exoridus/exosnap@v{version}/app/assets/brand/exosnap-logo.svg"
    );
    let nuspec_icon = xml_element(&nuspec_text, "iconUrl").unwrap_or_default();
    if nuspec_icon != expected_icon {
        report.errors.push(format!(
            "exosnap.nuspec: <iconUrl>{nuspec_icon}</iconUrl> != expected '{expected_icon}'"
        ));
    }

    let expected_notes = format!("https://github.com/Exoridus/exosnap/releases/tag/v{version}");
    let nuspec_notes = xml_element(&nuspec_text, "releaseNotes").unwrap_or_default();
    if nuspec_notes != expected_notes {
        report.errors.push(format!(
            "exosnap.nuspec: <releaseNotes>{nuspec_notes}</releaseNotes> != expected '{expected_notes}'"
        ));
    }

    static URL64: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?m)^\s*url64bit\s*=\s*'([^']+)'").unwrap());
    match URL64.captures(&install_text) {
        None => report
            .errors
            .push("chocolateyinstall.ps1: could not find 'url64bit = ...'".into()),
        Some(m) => {
            let url64 = &m[1];
            let expected = format!(
                "https://github.com/Exoridus/exosnap/releases/download/v{version}/ExoSnap-{version}-windows-x64.msi"
            );
            if url64 != expected {
                report.errors.push(format!(
                    "chocolateyinstall.ps1: url64bit '{url64}' != expected '{expected}'"
                ));
            }
        }
    }

    static CHECKSUM64: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?m)^\s*checksum64\s*=\s*'([^']+)'").unwrap());
    static HEX64: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[0-9a-f]{64}$").unwrap());
    let mut checksum64: Option<String> = None;
    if mode.version_only {
        report
            .skips
            .push("checksum64 not examined (-VersionOnly).".into());
    } else {
        match CHECKSUM64.captures(&install_text) {
            None => report
                .errors
                .push("chocolateyinstall.ps1: could not find 'checksum64 = ...'".into()),
            Some(m) => {
                let value = m[1].to_string();
                if !HEX64.is_match(&value) {
                    report.errors.push(format!(
                        "chocolateyinstall.ps1: checksum64 '{value}' is not 64 lowercase hex characters"
                    ));
                } else if is_degenerate(&value) {
                    report.errors.push(format!(
                        "chocolateyinstall.ps1: checksum64 is the placeholder for an unpublished release ('{}' x 64), not a real hash. The package cannot be packed or submitted until the v{version} release exists. Take the lowercase value from ExoSnap-{version}-windows-x64.msi.sha256 (the sidecar published next to the MSI on the GitHub Release), or from the artifact manifest's msiSha256 for a local release build.",
                        &value[0..1]
                    ));
                }
                checksum64 = Some(value);
            }
        }

        static CHECKSUM_TYPE64: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"(?m)^\s*checksumType64\s*=\s*'([^']+)'").unwrap());
        match CHECKSUM_TYPE64.captures(&install_text) {
            None => report
                .errors
                .push("chocolateyinstall.ps1: could not find 'checksumType64 = ...'".into()),
            Some(m) => {
                if &m[1] != "sha256" {
                    report.errors.push(format!(
                        "chocolateyinstall.ps1: checksumType64 '{}' != 'sha256'",
                        &m[1]
                    ));
                }
            }
        }
    }

    let manifest_explicit = mode.manifest_path.is_some();
    let manifest_path = if mode.version_only {
        None
    } else {
        Some(mode.manifest_path.clone().unwrap_or_else(|| {
            repo_root.join(format!(
                ".workspace/release/{version}/artifact-manifest.json"
            ))
        }))
    };

    if mode.version_only {
        report
            .skips
            .push("checksum64-vs-manifest check skipped (-VersionOnly).".into());
    } else {
        let manifest_path = manifest_path.expect("computed above when not version_only");
        if !manifest_path.is_file() {
            if manifest_explicit {
                report.errors.push(format!(
                    "Manifest path specified but not found: {}",
                    manifest_path.display()
                ));
            } else if mode.require_manifest {
                report.errors.push(format!(
                    "No release artifact manifest at '{}' and -RequireManifest was set. Refusing to validate checksum64 without a real built-MSI hash to check it against.",
                    manifest_path.display()
                ));
            } else {
                report.skips.push(format!(
                    "No release artifact manifest at '{}': checksum64-vs-manifest check skipped.",
                    manifest_path.display()
                ));
            }
        } else {
            let manifest_text = std::fs::read_to_string(&manifest_path)?;
            let manifest: Value = serde_json::from_str(&manifest_text).map_err(|error| {
                anyhow::anyhow!("could not parse {}: {error}", manifest_path.display())
            })?;
            let manifest_version = manifest.get("version").and_then(Value::as_str);
            let manifest_sha = manifest.get("msiSha256").and_then(Value::as_str);
            if manifest_version.is_some_and(|v| v != version) {
                report.errors.push(format!(
                    "Artifact manifest '{}': version '{}' != target version '{version}'",
                    manifest_path.display(),
                    manifest_version.unwrap()
                ));
            } else if manifest_sha.is_none_or(str::is_empty) {
                if mode.require_manifest {
                    report.errors.push(format!(
                        "Artifact manifest '{}' has no msiSha256 (MSI build was skipped) and -RequireManifest was set.",
                        manifest_path.display()
                    ));
                } else {
                    report.skips.push(format!(
                        "Artifact manifest '{}' has no msiSha256 (MSI build was skipped): checksum64-vs-manifest check skipped.",
                        manifest_path.display()
                    ));
                }
            } else if let Some(checksum64) = &checksum64 {
                let manifest_sha = manifest_sha.unwrap().to_lowercase();
                if checksum64.to_lowercase() != manifest_sha {
                    report.errors.push(format!(
                        "chocolateyinstall.ps1: checksum64 '{checksum64}' != artifact manifest msiSha256 '{manifest_sha}' ({})",
                        manifest_path.display()
                    ));
                }
            }
        }
    }

    find_stale_version_references(
        &mut report,
        &nuspec_text,
        "exosnap.nuspec",
        version,
        &["<version>", "<iconUrl>", "<releaseNotes>"],
    );
    find_stale_version_references(
        &mut report,
        &install_text,
        "tools/chocolateyinstall.ps1",
        version,
        &["url64bit"],
    );
    find_stale_version_references(
        &mut report,
        &uninstall_text,
        "tools/chocolateyuninstall.ps1",
        version,
        &[],
    );

    if mode.version_only {
        return Ok(report);
    }

    // The moderation subset below describes a submission, which cannot happen
    // before the release exists. -VersionOnly returns before this point so the
    // version axis can be a pull-request check without dragging the rest of
    // the submission bar into it.
    static REQUIRED_FIELDS: &[&str] = &[
        "id",
        "version",
        "title",
        "authors",
        "owners",
        "summary",
        "description",
        "copyright",
        "projectUrl",
        "licenseUrl",
        "iconUrl",
        "releaseNotes",
        "tags",
        "docsUrl",
        "bugTrackerUrl",
        "packageSourceUrl",
        "projectSourceUrl",
    ];
    for field in REQUIRED_FIELDS {
        let value = xml_element(&nuspec_text, field).unwrap_or_default();
        if value.trim().is_empty() {
            report.errors.push(format!(
                "exosnap.nuspec: <{field}> is missing or empty (required or strongly expected by Chocolatey moderation)"
            ));
        }
    }

    let nuspec_id = xml_element(&nuspec_text, "id").unwrap_or_default();
    if !nuspec_id.is_empty() && nuspec_id != nuspec_id.to_lowercase() {
        report.errors.push(format!(
            "exosnap.nuspec: <id>{nuspec_id}</id> must be all lowercase"
        ));
    }

    let nuspec_title = xml_element(&nuspec_text, "title").unwrap_or_default();
    if !nuspec_title.is_empty() && !nuspec_id.is_empty() && nuspec_title == nuspec_id {
        report.errors.push(format!(
            "exosnap.nuspec: <title> is character-identical to <id> ('{nuspec_id}'); the id is lowercase, the title is title-cased"
        ));
    }

    let nuspec_copyright = xml_element(&nuspec_text, "copyright").unwrap_or_default();
    if !nuspec_copyright.is_empty() && nuspec_copyright.trim().chars().count() < 4 {
        report.errors.push(format!(
            "exosnap.nuspec: <copyright> '{nuspec_copyright}' is shorter than 4 characters"
        ));
    }

    let require_license_acceptance =
        xml_element(&nuspec_text, "requireLicenseAcceptance").unwrap_or_default();
    let license_url = xml_element(&nuspec_text, "licenseUrl").unwrap_or_default();
    if require_license_acceptance == "true" && license_url.is_empty() {
        report.errors.push(
            "exosnap.nuspec: <requireLicenseAcceptance>true</requireLicenseAcceptance> without a <licenseUrl>"
                .into(),
        );
    }

    for url_field in [
        "projectUrl",
        "licenseUrl",
        "iconUrl",
        "releaseNotes",
        "docsUrl",
        "bugTrackerUrl",
        "packageSourceUrl",
        "projectSourceUrl",
        "mailingListUrl",
    ] {
        if let Some(value) = xml_element(&nuspec_text, url_field) {
            if !value.is_empty() && !value.starts_with("https://") {
                report.errors.push(format!(
                    "exosnap.nuspec: <{url_field}> '{value}' is not an https:// URL"
                ));
            }
        }
    }

    if !nuspec_icon.is_empty() {
        if nuspec_icon.contains("raw.githubusercontent.com")
            || Regex::new(r"github\.com/.*/raw/")
                .unwrap()
                .is_match(&nuspec_icon)
        {
            report.errors.push(
                "exosnap.nuspec: <iconUrl> must go through a CDN (jsDelivr, Statically, Githack); raw GitHub links are rejected"
                    .into(),
            );
        }
        static IMAGE_EXT: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"(?i)\.(png|ico|gif|jpg|jpeg|bmp|webp|svg)$").unwrap());
        if !IMAGE_EXT.is_match(&nuspec_icon) {
            report.errors.push(format!(
                "exosnap.nuspec: <iconUrl> '{nuspec_icon}' does not end in a permitted image extension"
            ));
        }
    }

    if let Some(nuspec_tags) = xml_element(&nuspec_text, "tags") {
        if !nuspec_tags.is_empty() {
            if nuspec_tags.contains(',') {
                report.errors.push(
                    "exosnap.nuspec: <tags> must be space-separated, not comma-separated".into(),
                );
            }
            if nuspec_tags != nuspec_tags.to_lowercase() {
                report.errors.push(format!(
                    "exosnap.nuspec: <tags> must be lowercase: '{nuspec_tags}'"
                ));
            }
            if nuspec_tags.split_whitespace().any(|t| t == "chocolatey") {
                report
                    .errors
                    .push("exosnap.nuspec: <tags> must not contain 'chocolatey'".into());
            }
        }
    }

    if let Some(description) = xml_element(&nuspec_text, "description") {
        if !description.is_empty() {
            let len = description.trim().chars().count();
            if len < 30 {
                report.errors.push(format!(
                    "exosnap.nuspec: <description> is {len} characters, the minimum is 30"
                ));
            }
            if len > 4000 {
                report.errors.push(format!(
                    "exosnap.nuspec: <description> is {len} characters, the maximum is 4000"
                ));
            }
            static HEADING_NO_SPACE: LazyLock<Regex> =
                LazyLock::new(|| Regex::new(r"^\s*#{1,6}[^#\s]").unwrap());
            for (idx, line) in description.lines().enumerate() {
                if HEADING_NO_SPACE.is_match(line) {
                    report.errors.push(format!(
                        "exosnap.nuspec: <description> line {}: a Markdown heading with no space after the '#': {}",
                        idx + 1,
                        line.trim()
                    ));
                }
            }
        }
    }

    for (id, version_pin) in dependencies(&nuspec_text) {
        if version_pin.as_deref().unwrap_or("").trim().is_empty() {
            report.errors.push(format!(
                "exosnap.nuspec: <dependency id=\"{id}\"> has no version range"
            ));
        }
    }

    static EMAIL: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"[\w.%+-]+@[\w.-]+\.[A-Za-z]{2,}").unwrap());
    if let Some(m) = EMAIL.find(&nuspec_text) {
        report.errors.push(format!(
            "exosnap.nuspec: contains an e-mail address ('{}'); the feed does not accept one",
            m.as_str()
        ));
    }

    static PLACEHOLDERS: &[&str] = &[
        "__REPLACE",
        "PACKAGE_NAME",
        "space separated",
        "Software Name",
        "REPLACE_ME",
        "YOUR_",
        "The software LICENSE ACCEPTANCE",
    ];
    for placeholder in PLACEHOLDERS {
        if nuspec_text.contains(placeholder) {
            report.errors.push(format!(
                "exosnap.nuspec: leftover template placeholder '{placeholder}'"
            ));
        }
    }

    for (label, text) in [
        ("tools/chocolateyinstall.ps1", &install_text),
        ("tools/chocolateyuninstall.ps1", &uninstall_text),
    ] {
        static STOP_PREFERENCE: LazyLock<Regex> =
            LazyLock::new(|| Regex::new(r"(?m)^\s*\$ErrorActionPreference\s*=\s*'Stop'").unwrap());
        if !STOP_PREFERENCE.is_match(text) {
            report.errors.push(format!(
                "{label}: must start with $ErrorActionPreference = 'Stop'"
            ));
        }
        for check in SCRIPT_CHECKS {
            let re = Regex::new(check.pattern).unwrap();
            if re.is_match(text) {
                report.errors.push(format!("{label}: {}", check.message));
            }
        }
    }

    for entry in std::fs::read_dir(&tools_root)? {
        let entry = entry?;
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        let relative = format!("tools/{name}");
        if path.extension().and_then(|e| e.to_str()) != Some("ps1") {
            report.errors.push(format!(
                "{relative} is packaged but is not a PowerShell script. A package that ships binaries additionally needs tools/LICENSE.txt and tools/VERIFICATION.txt, and distribution rights for the binary."
            ));
        }
        static SYSTEM_FILE: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r"^\.git|^Thumbs\.db$|^\.DS_Store$|^desktop\.ini$").unwrap()
        });
        if SYSTEM_FILE.is_match(&name) {
            report.errors.push(format!(
                "{relative} is a source-control or operating-system index file and must not be packaged"
            ));
        }
    }

    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    const VERSION: &str = "0.10.0";

    const NUSPEC: &str = r#"<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://schemas.microsoft.com/packaging/2015/06/nuspec.xsd">
  <metadata>
    <id>exosnap</id>
    <version>0.10.0</version>
    <packageSourceUrl>https://github.com/Exoridus/exosnap/tree/main/packaging/chocolatey</packageSourceUrl>
    <owners>Codexo</owners>
    <title>ExoSnap</title>
    <authors>Codexo</authors>
    <projectUrl>https://github.com/Exoridus/exosnap</projectUrl>
    <iconUrl>https://cdn.jsdelivr.net/gh/Exoridus/exosnap@v0.10.0/app/assets/brand/exosnap-logo.svg</iconUrl>
    <copyright>Copyright 2026 Codexo</copyright>
    <licenseUrl>https://github.com/Exoridus/exosnap/blob/main/LICENSE</licenseUrl>
    <requireLicenseAcceptance>false</requireLicenseAcceptance>
    <projectSourceUrl>https://github.com/Exoridus/exosnap</projectSourceUrl>
    <docsUrl>https://github.com/Exoridus/exosnap/blob/main/README.md</docsUrl>
    <bugTrackerUrl>https://github.com/Exoridus/exosnap/issues</bugTrackerUrl>
    <tags>exosnap recorder screen-capture</tags>
    <summary>Windows-native screen, window and region recorder.</summary>
    <releaseNotes>https://github.com/Exoridus/exosnap/releases/tag/v0.10.0</releaseNotes>
    <description><![CDATA[
This description exists only to satisfy the thirty character minimum for the moderation check.

## Notes
]]></description>
    <dependencies>
      <dependency id="vcredist140" version="14.44.35112.1" />
    </dependencies>
  </metadata>
  <files>
    <file src="tools\**" target="tools" />
  </files>
</package>
"#;

    const INSTALL: &str = concat!(
        "$ErrorActionPreference = 'Stop'\n",
        "$packageArgs = @{\n",
        "  url64bit       = 'https://github.com/Exoridus/exosnap/releases/download/v0.10.0/ExoSnap-0.10.0-windows-x64.msi'\n",
        "  checksum64     = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'\n",
        "  checksumType64 = 'sha256'\n",
        "}\n",
        "Install-ChocolateyPackage @packageArgs\n",
    );

    const UNINSTALL: &str = concat!(
        "$ErrorActionPreference = 'Stop'\n",
        "Uninstall-ChocolateyPackage @packageArgs\n",
    );

    fn fixture() -> tempfile::TempDir {
        let dir = tempfile::tempdir().unwrap();
        let choco = dir.path().join("packaging/chocolatey");
        std::fs::create_dir_all(choco.join("tools")).unwrap();
        std::fs::write(choco.join("exosnap.nuspec"), NUSPEC).unwrap();
        std::fs::write(choco.join("tools/chocolateyinstall.ps1"), INSTALL).unwrap();
        std::fs::write(choco.join("tools/chocolateyuninstall.ps1"), UNINSTALL).unwrap();
        dir
    }

    #[test]
    fn a_consistent_package_passes_version_only() {
        let dir = fixture();
        let report = validate_chocolatey(
            dir.path(),
            VERSION,
            ChocolateyMode {
                version_only: true,
                ..Default::default()
            },
        )
        .unwrap();
        assert!(report.ok(), "{:?}", report.errors);
    }

    #[test]
    fn a_consistent_package_passes_full_validation() {
        let dir = fixture();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(report.ok(), "{:?}", report.errors);
    }

    #[test]
    fn a_missing_nuspec_is_a_hard_error() {
        let dir = fixture();
        std::fs::remove_file(dir.path().join("packaging/chocolatey/exosnap.nuspec")).unwrap();
        let error =
            validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap_err();
        assert!(error.to_string().contains("exosnap.nuspec"));
    }

    #[test]
    fn a_missing_uninstall_script_is_a_hard_error() {
        let dir = fixture();
        std::fs::remove_file(
            dir.path()
                .join("packaging/chocolatey/tools/chocolateyuninstall.ps1"),
        )
        .unwrap();
        let error =
            validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap_err();
        assert!(error.to_string().contains("chocolateyuninstall.ps1"));
    }

    #[test]
    fn a_nuspec_version_disagreeing_with_the_target_is_an_error() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/chocolatey/exosnap.nuspec"),
            NUSPEC.replace("<version>0.10.0</version>", "<version>0.9.0</version>"),
        )
        .unwrap();
        let report = validate_chocolatey(
            dir.path(),
            VERSION,
            ChocolateyMode {
                version_only: true,
                ..Default::default()
            },
        )
        .unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("<version>0.9.0</version>"))
        );
    }

    #[test]
    fn the_all_zero_placeholder_checksum_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path()
                .join("packaging/chocolatey/tools/chocolateyinstall.ps1"),
            INSTALL.replace(
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                &"0".repeat(64),
            ),
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("placeholder for an unpublished release"))
        );
    }

    #[test]
    fn version_only_never_examines_checksum64_at_all() {
        let dir = fixture();
        std::fs::write(
            dir.path()
                .join("packaging/chocolatey/tools/chocolateyinstall.ps1"),
            INSTALL.replace(
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                &"0".repeat(64),
            ),
        )
        .unwrap();
        let report = validate_chocolatey(
            dir.path(),
            VERSION,
            ChocolateyMode {
                version_only: true,
                ..Default::default()
            },
        )
        .unwrap();
        assert!(report.ok(), "{:?}", report.errors);
        assert!(report.skips.iter().any(|s| s.contains("not examined")));
    }

    #[test]
    fn write_host_in_an_automation_script_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path()
                .join("packaging/chocolatey/tools/chocolateyinstall.ps1"),
            format!("{INSTALL}\nWrite-Host 'installed'\n"),
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(report.errors.iter().any(|e| e.contains("uses Write-Host")));
    }

    #[test]
    fn a_comma_separated_tags_list_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/chocolatey/exosnap.nuspec"),
            NUSPEC.replace(
                "<tags>exosnap recorder screen-capture</tags>",
                "<tags>exosnap,recorder</tags>",
            ),
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(report.errors.iter().any(|e| e.contains("space-separated")));
    }

    #[test]
    fn a_dependency_without_a_version_pin_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/chocolatey/exosnap.nuspec"),
            NUSPEC.replace(
                r#"<dependency id="vcredist140" version="14.44.35112.1" />"#,
                r#"<dependency id="vcredist140" />"#,
            ),
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("has no version range"))
        );
    }

    #[test]
    fn a_stale_version_reference_in_the_description_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/chocolatey/exosnap.nuspec"),
            NUSPEC.replace("## Notes", "## Notes (carried over from 0.8.1)"),
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("stale version reference"))
        );
    }

    #[test]
    fn the_manifest_cross_check_is_a_hard_error_when_the_explicit_path_is_missing() {
        let dir = fixture();
        let manifest_path = dir.path().join("nope/artifact-manifest.json");
        let report = validate_chocolatey(
            dir.path(),
            VERSION,
            ChocolateyMode {
                manifest_path: Some(manifest_path),
                ..Default::default()
            },
        )
        .unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("Manifest path specified but not found"))
        );
    }

    #[test]
    fn require_manifest_without_a_manifest_is_an_error_not_a_silent_skip() {
        let dir = fixture();
        let report = validate_chocolatey(
            dir.path(),
            VERSION,
            ChocolateyMode {
                require_manifest: true,
                ..Default::default()
            },
        )
        .unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("-RequireManifest was set"))
        );
    }

    #[test]
    fn a_matching_manifest_checksum_passes() {
        let dir = fixture();
        let release_dir = dir.path().join(".workspace/release/0.10.0");
        std::fs::create_dir_all(&release_dir).unwrap();
        std::fs::write(
            release_dir.join("artifact-manifest.json"),
            r#"{"version":"0.10.0","msiSha256":"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"}"#,
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(report.ok(), "{:?}", report.errors);
    }

    #[test]
    fn a_mismatched_manifest_checksum_is_rejected() {
        let dir = fixture();
        let release_dir = dir.path().join(".workspace/release/0.10.0");
        std::fs::create_dir_all(&release_dir).unwrap();
        std::fs::write(
            release_dir.join("artifact-manifest.json"),
            r#"{"version":"0.10.0","msiSha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}"#,
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("!= artifact manifest msiSha256"))
        );
    }

    #[test]
    fn a_non_script_file_under_tools_is_rejected() {
        let dir = fixture();
        std::fs::write(
            dir.path().join("packaging/chocolatey/tools/exosnap.exe"),
            "not a binary",
        )
        .unwrap();
        let report = validate_chocolatey(dir.path(), VERSION, ChocolateyMode::default()).unwrap();
        assert!(!report.ok());
        assert!(
            report
                .errors
                .iter()
                .any(|e| e.contains("is not a PowerShell script"))
        );
    }
}
