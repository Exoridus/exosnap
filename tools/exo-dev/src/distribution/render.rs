//! Renders the prepared package surfaces from the resolved release.
//!
//! The tracked files under `packaging/` stay the canonical source; a prepared
//! tree is a copy with the release-dependent values filled from the verified
//! public assets. Every renderer refuses a template that does not carry the
//! placeholder it is about to fill, so a stale tree cannot silently produce a
//! package that describes the previous release.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, ensure};
use regex::Regex;

use super::msi::MsiIdentity;
use super::release::{DistributionRelease, is_sha256};

/// A placeholder in one tracked template and the verified value that replaces
/// it. `expected` is the placeholder shape in the canonical tree; a template
/// that no longer matches means the file was edited without updating the
/// renderer, and that is a failure, not a silent no-op.
struct Replacement {
    pattern: String,
    value: String,
}

fn replace_once(text: &str, replacement: &Replacement) -> Result<String> {
    ensure!(
        text.contains(&replacement.pattern),
        "the template no longer carries '{}'",
        replacement.pattern
    );
    let mut first = true;
    let mut out = String::with_capacity(text.len());
    let mut cursor = 0;
    while let Some(offset) = text[cursor..].find(&replacement.pattern) {
        let start = cursor + offset;
        out.push_str(&text[cursor..start]);
        if first {
            out.push_str(&replacement.value);
            first = false;
        } else {
            out.push_str(&replacement.pattern);
        }
        cursor = start + replacement.pattern.len();
    }
    out.push_str(&text[cursor..]);
    Ok(out)
}

fn copy_tree(from: &Path, to: &Path) -> Result<()> {
    ensure!(from.is_dir(), "{} is not a directory", from.display());
    fs::create_dir_all(to)?;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            copy_tree(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), &target)?;
        }
    }
    Ok(())
}

fn write(path: &Path, text: &str) -> Result<()> {
    fs::write(path, text).with_context(|| format!("could not write {}", path.display()))
}

/// Copies and fills `packaging/chocolatey`; returns the nuspec path.
pub fn render_chocolatey(
    repo_root: &Path,
    out_root: &Path,
    release: &DistributionRelease,
) -> Result<PathBuf> {
    let target = out_root.join("packaging/chocolatey");
    copy_tree(&repo_root.join("packaging/chocolatey"), &target)?;
    let install = target.join("tools/chocolateyinstall.ps1");
    let text = fs::read_to_string(&install)?;
    ensure!(
        crate::packaging::chocolatey::has_distribution_owner_switch(&text),
        "the Chocolatey template lost EXOSNAP_DISTRIBUTION_OWNER=chocolatey"
    );
    let text = replace_once(
        &text,
        &Replacement {
            pattern: format!("checksum64     = '{}'", "0".repeat(64)),
            value: format!("checksum64     = '{}'", release.msi.sha256),
        },
    )?;
    write(&install, &text)?;
    Ok(target.join("exosnap.nuspec"))
}

/// Copies and fills the versioned WinGet manifest directory; returns it.
pub fn render_winget(
    repo_root: &Path,
    out_root: &Path,
    release: &DistributionRelease,
    identity: &MsiIdentity,
) -> Result<PathBuf> {
    let relative = format!(
        "packaging/winget/manifests/c/Codexo/ExoSnap/{}",
        release.version
    );
    let source = repo_root.join(&relative);
    let target = out_root.join(&relative);
    copy_tree(&source, &target)?;
    let installer = target.join("Codexo.ExoSnap.installer.yaml");
    let text = fs::read_to_string(&installer)?;
    ensure!(
        crate::packaging::winget::has_distribution_owner_switch(&text),
        "the WinGet template lost EXOSNAP_DISTRIBUTION_OWNER=winget"
    );
    let placeholder = format!("'{}'", "0".repeat(64));
    ensure!(
        text.contains(&placeholder),
        "the WinGet template no longer carries the InstallerSha256 placeholder"
    );
    let text = text.replace(
        &placeholder,
        &format!("'{}'", release.msi.sha256.to_uppercase()),
    );
    let product = format!("'{}'", "{00000000-0000-0000-0000-000000000000}");
    ensure!(
        text.matches(&product).count() == 2,
        "the WinGet template must carry exactly two ProductCode placeholders"
    );
    let text = text.replace(&product, &format!("'{}'", identity.product_code));
    let upgrade = format!("'{}'", "{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}");
    ensure!(
        text.contains(&upgrade),
        "the WinGet template no longer carries the permanent UpgradeCode"
    );
    ensure!(
        identity
            .upgrade_code
            .eq_ignore_ascii_case("{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}"),
        "the MSI declares UpgradeCode '{}', not the permanent one the manifests publish",
        identity.upgrade_code
    );
    let text = replace_release_date(&text, &release.release_date())?;
    write(&installer, &text)?;
    Ok(target)
}

fn replace_release_date(text: &str, date: &str) -> Result<String> {
    let mut lines: Vec<String> = Vec::new();
    let mut seen = false;
    for line in text.lines() {
        if line.trim_start().starts_with("ReleaseDate:") {
            seen = true;
            let indent = &line[..line.len() - line.trim_start().len()];
            lines.push(format!("{indent}ReleaseDate: {date}"));
        } else {
            lines.push(line.to_string());
        }
    }
    ensure!(seen, "the WinGet template carries no ReleaseDate");
    let mut out = lines.join("\n");
    if text.ends_with('\n') {
        out.push('\n');
    }
    Ok(out)
}

/// Copies and fills `packaging/scoop/exosnap.json`; returns it. The bucket
/// copy drops `depends`, because `extras/vcredist2022` only resolves for a
/// user who has the Extras bucket; the standalone bucket documents the
/// requirement in its notes instead.
pub fn render_scoop(
    repo_root: &Path,
    out_root: &Path,
    release: &DistributionRelease,
) -> Result<PathBuf> {
    let target = out_root.join("packaging/scoop/exosnap.json");
    fs::create_dir_all(target.parent().unwrap())?;
    let text = fs::read_to_string(repo_root.join("packaging/scoop/exosnap.json"))?;
    ensure!(
        text.contains(&format!("\"hash\": \"{}\"", "0".repeat(64))),
        "the Scoop template no longer carries the hash placeholder"
    );
    ensure!(
        is_sha256(&release.portable.sha256),
        "the portable SHA-256 is malformed"
    );
    let text = text.replace(
        &format!("\"hash\": \"{}\"", "0".repeat(64)),
        &format!("\"hash\": \"{}\"", release.portable.sha256),
    );
    let depends = Regex::new(r#"(?m)^\s*"depends":\s*"[^"]*",\r?\n"#).unwrap();
    ensure!(
        depends.find_iter(&text).count() == 1,
        "the Scoop template must carry exactly one depends field to drop"
    );
    let text = depends.replace(&text, "").into_owned();
    write(&target, &text)?;
    Ok(target)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::packaging_fixture;

    fn release(version: &str) -> DistributionRelease {
        DistributionRelease {
            version: version.into(),
            tag: format!("v{version}"),
            source_commit: "a".repeat(40),
            release_id: 1,
            release_url: "https://example.invalid/release".into(),
            published_at: "2026-10-02T16:28:12Z".into(),
            msi: super::super::release::PackageAsset {
                filename: format!("ExoSnap-{version}-windows-x64.msi"),
                url: "https://example.invalid/msi".into(),
                sha256: "0123456789abcdef".repeat(4),
                size: 1,
            },
            portable: super::super::release::PackageAsset {
                filename: format!("ExoSnap-{version}-windows-x64-portable.zip"),
                url: "https://example.invalid/zip".into(),
                sha256: "fedcba9876543210".repeat(4),
                size: 1,
            },
        }
    }

    #[test]
    fn rendered_surfaces_carry_the_verified_values_and_pass_the_validators() {
        let dir = packaging_fixture("0.10.0");
        let out = dir.path().join("distribution");
        let model = release("0.10.0");
        let identity = MsiIdentity {
            product_code: "{11111111-2222-3333-4444-555555555555}".into(),
            upgrade_code: "{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}".into(),
        };
        let nuspec = render_chocolatey(dir.path(), &out, &model).unwrap();
        render_winget(dir.path(), &out, &model, &identity).unwrap();
        render_scoop(dir.path(), &out, &model).unwrap();

        let choco = crate::packaging::validate_chocolatey(
            &out,
            "0.10.0",
            crate::packaging::ChocolateyMode::default(),
        )
        .unwrap();
        assert!(choco.ok(), "{:?}", choco.errors);
        let winget = crate::packaging::validate_winget(&out, "0.10.0").unwrap();
        assert!(winget.ok(), "{:?}", winget.errors);
        let scoop = crate::packaging::validate_scoop(&out, "0.10.0").unwrap();
        assert!(scoop.ok(), "{:?}", scoop.errors);

        let install =
            fs::read_to_string(out.join("packaging/chocolatey/tools/chocolateyinstall.ps1"))
                .unwrap();
        assert!(install.contains(&format!(
            "checksum64     = '{}'",
            "0123456789abcdef".repeat(4)
        )));
        let installer = fs::read_to_string(out.join(
            "packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0/Codexo.ExoSnap.installer.yaml",
        ))
        .unwrap();
        assert_eq!(installer.matches(&"0123456789ABCDEF".repeat(4)).count(), 1);
        assert_eq!(
            installer
                .matches("{11111111-2222-3333-4444-555555555555}")
                .count(),
            2
        );
        assert!(installer.contains("ReleaseDate: 2026-10-02"));
        assert!(crate::packaging::winget::has_distribution_owner_switch(
            &installer
        ));
        assert!(crate::packaging::chocolatey::has_distribution_owner_switch(
            &install
        ));
        assert!(!installer.contains("ReleaseDate: 2026-09-07"));
        let tracked = fs::read_to_string(dir.path().join("packaging/scoop/exosnap.json")).unwrap();
        assert!(tracked.contains("\"depends\""));
        let scoop = fs::read_to_string(out.join("packaging/scoop/exosnap.json")).unwrap();
        assert!(scoop.contains(&format!("\"hash\": \"{}\"", "fedcba9876543210".repeat(4))));
        assert!(!scoop.contains(&"0".repeat(64)));
        assert!(
            !scoop.contains("\"depends\""),
            "the bucket copy drops the Extras-only dependency"
        );
        assert!(scoop.contains("checkver") && scoop.contains("autoupdate"));
        assert!(nuspec.is_file());
    }

    #[test]
    fn a_template_without_its_placeholder_is_a_failure_not_a_noop() {
        let dir = packaging_fixture("0.10.0");
        let install = dir
            .path()
            .join("packaging/chocolatey/tools/chocolateyinstall.ps1");
        let text = fs::read_to_string(&install).unwrap();
        fs::write(&install, text.replace(&"0".repeat(64), "already filled")).unwrap();
        let error = render_chocolatey(dir.path(), &dir.path().join("out"), &release("0.10.0"))
            .unwrap_err()
            .to_string();
        assert!(error.contains("no longer carries"), "{error}");
    }

    #[test]
    fn rendering_refuses_templates_that_lost_distribution_ownership() {
        let dir = packaging_fixture("0.10.0");
        let install = dir
            .path()
            .join("packaging/chocolatey/tools/chocolateyinstall.ps1");
        let text = fs::read_to_string(&install).unwrap();
        fs::write(
            &install,
            text.replace(" EXOSNAP_DISTRIBUTION_OWNER=chocolatey", ""),
        )
        .unwrap();
        assert!(
            render_chocolatey(dir.path(), &dir.path().join("out"), &release("0.10.0"))
                .unwrap_err()
                .to_string()
                .contains("EXOSNAP_DISTRIBUTION_OWNER")
        );
        let installer = dir.path().join(
            "packaging/winget/manifests/c/Codexo/ExoSnap/0.10.0/Codexo.ExoSnap.installer.yaml",
        );
        let text = fs::read_to_string(&installer).unwrap();
        fs::write(
            &installer,
            text.replace("  Custom: EXOSNAP_DISTRIBUTION_OWNER=winget\n", ""),
        )
        .unwrap();
        let identity = MsiIdentity {
            product_code: "{11111111-2222-3333-4444-555555555555}".into(),
            upgrade_code: "{8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D}".into(),
        };
        assert!(
            render_winget(
                dir.path(),
                &dir.path().join("out"),
                &release("0.10.0"),
                &identity
            )
            .unwrap_err()
            .to_string()
            .contains("EXOSNAP_DISTRIBUTION_OWNER")
        );
    }
}
