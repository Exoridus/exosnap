//! Product version propagation across packaging surfaces.
//!
//! `project(exosnap VERSION x.y.z)` in the root CMakeLists.txt is the only
//! place the product version is declared, and the Chocolatey nuspec and
//! install script, the three WinGet manifests (and the directory they live
//! in), and the Scoop manifest each repeat it by hand. `check_packaging_version`
//! is the drift gate: every packaging literal must name the version the
//! source tree declares. `bump` is the other half, so agreeing is not a
//! matter of finding all of them by hand.
//!
//! `check_packaging_version` runs every validator regardless of whether an
//! earlier one disagreed, so a change that breaks two surfaces is reported as
//! two failures in one run rather than one round trip per surface.

use std::path::Path;
use std::sync::LazyLock;

use regex::{Captures, Regex};

use crate::git::Git;
use crate::packaging::{self, ChocolateyMode};

/// Every packaging surface `check_packaging_version` found disagreeing with
/// `target_version`.
#[derive(Debug)]
pub struct VersionDriftReport {
    pub target_version: String,
    pub failed: Vec<&'static str>,
}

impl VersionDriftReport {
    pub fn ok(&self) -> bool {
        self.failed.is_empty()
    }
}

/// Checks every packaging surface against `version`, or, when `None`, the
/// canonical CMake project version. The Chocolatey call is always
/// version-only: installer hashes and the Chocolatey moderation bar cannot be
/// true between a version bump and the release that produces the bytes they
/// describe, so requiring them here would fail the gate for the whole of
/// every release cycle.
pub fn check_packaging_version(
    repo_root: &Path,
    version: Option<&str>,
) -> anyhow::Result<VersionDriftReport> {
    let target_version = match version {
        Some(version) => version.to_string(),
        None => packaging::cmake_project_version(repo_root)?,
    };

    let mut failed = Vec::new();

    let chocolatey_mode = ChocolateyMode {
        version_only: true,
        manifest_path: None,
        require_manifest: false,
    };
    let chocolatey_ok = packaging::validate_chocolatey(repo_root, &target_version, chocolatey_mode)
        .is_ok_and(|report| report.ok());
    if !chocolatey_ok {
        failed.push("Chocolatey");
    }

    let winget_ok =
        packaging::validate_winget(repo_root, &target_version).is_ok_and(|report| report.ok());
    if !winget_ok {
        failed.push("WinGet");
    }

    let scoop_ok =
        packaging::validate_scoop(repo_root, &target_version).is_ok_and(|report| report.ok());
    if !scoop_ok {
        failed.push("Scoop");
    }

    Ok(VersionDriftReport {
        target_version,
        failed,
    })
}

/// Renders `check_packaging_version`'s verdict: every disagreeing surface, or
/// the all-clear line.
pub fn render_drift(report: &VersionDriftReport) -> String {
    if report.ok() {
        format!(
            "Packaging version gate PASSED: Chocolatey, WinGet and Scoop all name {}.\n",
            report.target_version
        )
    } else {
        format!(
            "Packaging version gate FAILED: {} do not agree with the CMake version {}. Bump every literal (docs/release-checklist.md section 8) or fix the source version.\n",
            report.failed.join(", "),
            report.target_version
        )
    }
}

const CHOCOLATEY_NUSPEC: &str = "packaging/chocolatey/exosnap.nuspec";
const CHOCOLATEY_INSTALL: &str = "packaging/chocolatey/tools/chocolateyinstall.ps1";
const SCOOP_MANIFEST: &str = "packaging/scoop/exosnap.json";

fn winget_manifest_dir(version: &str) -> String {
    format!("packaging/winget/manifests/c/Codexo/ExoSnap/{version}")
}

fn winget_version_manifest(version: &str) -> String {
    format!("{}/Codexo.ExoSnap.yaml", winget_manifest_dir(version))
}

fn winget_installer_manifest(version: &str) -> String {
    format!(
        "{}/Codexo.ExoSnap.installer.yaml",
        winget_manifest_dir(version)
    )
}

fn winget_locale_manifest(version: &str) -> String {
    format!(
        "{}/Codexo.ExoSnap.locale.en-US.yaml",
        winget_manifest_dir(version)
    )
}

/// Every file that repeats the product version by hand, relative to the
/// repository root, keyed by the version being bumped away from (the WinGet
/// manifests live inside the version-named directory).
fn version_files(current: &str) -> Vec<String> {
    vec![
        CHOCOLATEY_NUSPEC.to_string(),
        CHOCOLATEY_INSTALL.to_string(),
        SCOOP_MANIFEST.to_string(),
        winget_version_manifest(current),
        winget_installer_manifest(current),
        winget_locale_manifest(current),
    ]
}

/// A value only a release can produce, reset to its placeholder on every
/// bump so a bumped tree cannot be submitted describing the previous
/// release's bytes. `check_packaging_version` never examines these; the full
/// validators do, at submission time (docs/release-checklist.md section 8).
struct Placeholder {
    relative: String,
    pattern: Regex,
    value: String,
    label: &'static str,
}

fn placeholders(current: &str) -> Vec<Placeholder> {
    vec![
        Placeholder {
            relative: CHOCOLATEY_INSTALL.to_string(),
            pattern: Regex::new(r"(?m)^(\s*checksum64\s*=\s*')[0-9a-f]{64}(')").unwrap(),
            value: "0".repeat(64),
            label: "checksum64",
        },
        Placeholder {
            relative: SCOOP_MANIFEST.to_string(),
            pattern: Regex::new(r#"("hash":\s*")[0-9a-f]{64}(")"#).unwrap(),
            value: "0".repeat(64),
            label: "hash",
        },
        Placeholder {
            relative: winget_installer_manifest(current),
            pattern: Regex::new(r"(?m)^(\s*InstallerSha256:\s*')[0-9A-F]{64}(')").unwrap(),
            value: "0".repeat(64),
            label: "InstallerSha256",
        },
        Placeholder {
            relative: winget_installer_manifest(current),
            pattern: Regex::new(r"(?m)^(\s*ProductCode:\s*')\{[0-9A-Fa-f-]{36}\}(')").unwrap(),
            value: "{00000000-0000-0000-0000-000000000000}".to_string(),
            label: "ProductCode",
        },
    ]
}

/// Replaces every occurrence of `current` in `text` that is not itself part
/// of a longer dotted-number run, so `0.9.0` does not match inside `10.9.0`
/// or `0.9.01`, and counts how many were replaced.
fn replace_version_literal(text: &str, current: &str, new_version: &str) -> (String, usize) {
    let bytes = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut count = 0;
    let mut cursor = 0;
    while let Some(offset) = text[cursor..].find(current) {
        let start = cursor + offset;
        let end = start + current.len();
        let before_ok = start == 0 || !matches!(bytes[start - 1], b'0'..=b'9' | b'.');
        let after_ok = end == bytes.len() || !matches!(bytes[end], b'0'..=b'9' | b'.');
        out.push_str(&text[cursor..start]);
        if before_ok && after_ok {
            out.push_str(new_version);
            count += 1;
        } else {
            out.push_str(&text[start..end]);
        }
        cursor = end;
    }
    out.push_str(&text[cursor..]);
    (out, count)
}

static CMAKE_VERSION: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"project\(\s*exosnap\s+VERSION\s+(\d+\.\d+\.\d+)").unwrap());

static VERSION_SHAPE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^[0-9]+\.[0-9]+\.[0-9]+$").unwrap());

fn cmake_current_version(text: &str, cmake_path: &Path) -> anyhow::Result<String> {
    CMAKE_VERSION
        .captures(text)
        .map(|c| c[1].to_string())
        .ok_or_else(|| {
            anyhow::anyhow!(
                "could not parse project(exosnap VERSION x.y.z) from {}",
                cmake_path.display()
            )
        })
}

/// What a bump changed, or, when `already_current` is set, that it changed
/// nothing because the tree already declared `target_version`.
#[derive(Debug)]
pub struct BumpOutcome {
    pub current_version: String,
    pub target_version: String,
    pub already_current: bool,
    pub edits: Vec<String>,
    /// The self-verify `bump` runs at the end, over the bumped tree. `None`
    /// only for a no-op bump, which touches nothing to verify.
    pub drift: Option<VersionDriftReport>,
}

impl BumpOutcome {
    pub fn ok(&self) -> bool {
        self.already_current || self.drift.as_ref().is_some_and(VersionDriftReport::ok)
    }
}

/// Moves every packaging version literal from the CMake-declared current
/// version to `new_version`, resets the values only a release can produce to
/// their placeholders, and self-verifies the result with
/// `check_packaging_version`. Refuses a tree with uncommitted changes unless
/// `force`, so the bump is the whole diff and can be reviewed as one.
pub fn bump(repo_root: &Path, new_version: &str, force: bool) -> anyhow::Result<BumpOutcome> {
    anyhow::ensure!(
        VERSION_SHAPE.is_match(new_version),
        "version '{new_version}' is not x.y.z. A prerelease suffix is a release identity, not a product version."
    );

    let cmake_path = repo_root.join("CMakeLists.txt");
    let cmake_text = std::fs::read_to_string(&cmake_path)
        .map_err(|error| anyhow::anyhow!("could not read {}: {error}", cmake_path.display()))?;
    let current = cmake_current_version(&cmake_text, &cmake_path)?;

    if current == new_version {
        return Ok(BumpOutcome {
            current_version: current,
            target_version: new_version.to_string(),
            already_current: true,
            edits: Vec::new(),
            drift: None,
        });
    }

    if !force && repo_root.join(".git").exists() {
        let (_, status) =
            Git::new(repo_root).run(&["status", "--porcelain", "--untracked-files=no"]);
        anyhow::ensure!(
            status.trim().is_empty(),
            "the tree has uncommitted changes; a bump should be the whole diff. Commit or stash first, or pass --force."
        );
    }

    let mut edits = Vec::new();

    let updated_cmake = CMAKE_VERSION
        .replace(&cmake_text, |caps: &Captures| {
            caps[0].replace(&current, new_version)
        })
        .into_owned();
    std::fs::write(&cmake_path, &updated_cmake)
        .map_err(|error| anyhow::anyhow!("could not write {}: {error}", cmake_path.display()))?;
    edits.push(format!(
        "CMakeLists.txt: project(exosnap VERSION {new_version})"
    ));

    for relative in version_files(&current) {
        let path = repo_root.join(&relative);
        anyhow::ensure!(
            path.is_file(),
            "{relative} is not there; the file list for a bump is stale."
        );
        let text = std::fs::read_to_string(&path)
            .map_err(|error| anyhow::anyhow!("could not read {}: {error}", path.display()))?;
        let (mut updated, count) = replace_version_literal(&text, &current, new_version);
        anyhow::ensure!(
            count > 0,
            "{relative} names no {current}; the file list for a bump is stale."
        );
        for placeholder in placeholders(&current)
            .into_iter()
            .filter(|placeholder| placeholder.relative == relative)
        {
            anyhow::ensure!(
                placeholder.pattern.is_match(&updated),
                "{relative} has no {} to reset; the pattern for a bump is stale.",
                placeholder.label
            );
            updated = placeholder
                .pattern
                .replace_all(&updated, |caps: &Captures| {
                    format!("{}{}{}", &caps[1], placeholder.value, &caps[2])
                })
                .into_owned();
            edits.push(format!(
                "{relative}: {} reset to its placeholder",
                placeholder.label
            ));
        }
        std::fs::write(&path, &updated)
            .map_err(|error| anyhow::anyhow!("could not write {}: {error}", path.display()))?;
        edits.push(format!("{relative}: {count} literal(s)"));
    }

    let winget_root = repo_root.join("packaging/winget/manifests/c/Codexo/ExoSnap");
    let old_dir = winget_root.join(&current);
    let new_dir = winget_root.join(new_version);
    std::fs::rename(&old_dir, &new_dir).map_err(|error| {
        anyhow::anyhow!(
            "could not move {} to {}: {error}",
            old_dir.display(),
            new_dir.display()
        )
    })?;
    edits.push(format!(
        "packaging/winget/manifests/c/Codexo/ExoSnap/{current} -> {new_version}"
    ));

    let drift = check_packaging_version(repo_root, Some(new_version))?;

    Ok(BumpOutcome {
        current_version: current,
        target_version: new_version.to_string(),
        already_current: false,
        edits,
        drift: Some(drift),
    })
}

/// Renders a bump's outcome: what moved (or the no-op message), the release
/// fields left for the release that produces the bytes, and the self-verify
/// verdict.
pub fn render_bump(outcome: &BumpOutcome) -> String {
    if outcome.already_current {
        return format!(
            "The tree already declares {}; nothing to bump.\n",
            outcome.target_version
        );
    }

    let mut out = format!(
        "Bumping {} -> {}\n",
        outcome.current_version, outcome.target_version
    );
    for edit in &outcome.edits {
        out.push_str(&format!("  {edit}\n"));
    }
    out.push('\n');
    out.push_str(
        "Left for the release that produces the bytes (docs/release-checklist.md section 8):\n",
    );
    out.push_str(
        "  checksum64, InstallerSha256, ProductCode (from the freshly built MSI), hash, ReleaseDate.\n",
    );
    out.push('\n');
    if let Some(drift) = &outcome.drift {
        out.push_str(&render_drift(drift));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::packaging_fixture;

    const VERSION: &str = "0.10.0";
    const OTHER: &str = "0.10.1";

    fn edit_file(root: &Path, relative: &str, pattern: &str, replacement: &str) {
        let path = root.join(relative);
        let text = std::fs::read_to_string(&path).unwrap();
        let updated = Regex::new(pattern)
            .unwrap()
            .replace(&text, replacement)
            .into_owned();
        assert_ne!(
            updated, text,
            "mutation '{pattern}' matched nothing in {relative}"
        );
        std::fs::write(&path, updated).unwrap();
    }

    #[test]
    fn the_tracked_packaging_tree_passes_the_version_gate() {
        let dir = packaging_fixture(VERSION);
        let report = check_packaging_version(dir.path(), None).unwrap();
        assert!(report.ok(), "{:?}", report.failed);
        assert_eq!(report.target_version, VERSION);
    }

    #[test]
    fn a_missing_cmake_version_is_a_hard_error_when_none_is_given() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("CMakeLists.txt"), "project(other)\n").unwrap();
        assert!(check_packaging_version(dir.path(), None).is_err());
    }

    #[test]
    fn two_simultaneously_wrong_manifests_are_both_reported() {
        let dir = packaging_fixture(VERSION);
        edit_file(
            dir.path(),
            &winget_version_manifest(VERSION),
            &format!(r"(?m)^(PackageVersion:\s*){VERSION}\s*$"),
            &format!("${{1}}{OTHER}"),
        );
        edit_file(
            dir.path(),
            SCOOP_MANIFEST,
            &format!(r#"("version":\s*"){VERSION}(")"#),
            &format!("${{1}}{OTHER}${{2}}"),
        );
        let report = check_packaging_version(dir.path(), Some(VERSION)).unwrap();
        assert!(!report.ok());
        assert!(report.failed.contains(&"WinGet"), "{:?}", report.failed);
        assert!(report.failed.contains(&"Scoop"), "{:?}", report.failed);
        assert_eq!(report.failed.len(), 2, "{:?}", report.failed);
    }

    #[test]
    fn the_chocolatey_call_inside_the_aggregator_is_hardcoded_version_only() {
        // Blanking a moderation-required field (e.g. <owners>) fails the full
        // Chocolatey validator, but the aggregator's hardcoded -VersionOnly
        // mode never reaches that check.
        let dir = packaging_fixture(VERSION);
        edit_file(
            dir.path(),
            CHOCOLATEY_NUSPEC,
            r"<owners>[^<]*</owners>",
            "<owners></owners>",
        );

        let full = packaging::validate_chocolatey(
            dir.path(),
            VERSION,
            ChocolateyMode {
                version_only: false,
                manifest_path: None,
                require_manifest: false,
            },
        )
        .unwrap();
        assert!(
            !full.ok(),
            "the full validator must catch the blank <owners>"
        );

        let report = check_packaging_version(dir.path(), Some(VERSION)).unwrap();
        assert!(
            report.ok(),
            "the aggregator's version-only Chocolatey call must not see the moderation subset: {:?}",
            report.failed
        );
    }

    #[test]
    fn a_second_winget_version_directory_is_reported_as_winget() {
        let dir = packaging_fixture(VERSION);
        let manifests = dir
            .path()
            .join("packaging/winget/manifests/c/Codexo/ExoSnap");
        crate::test_support::copy_dir_all(&manifests.join(VERSION), &manifests.join(OTHER));

        let report = check_packaging_version(dir.path(), Some(VERSION)).unwrap();
        assert!(report.failed.contains(&"WinGet"), "{:?}", report.failed);
    }

    #[test]
    fn the_chocolatey_checksum_placeholder_does_not_fail_the_version_gate() {
        let dir = packaging_fixture(VERSION);
        let report = check_packaging_version(dir.path(), Some(VERSION)).unwrap();
        let choco_text = std::fs::read_to_string(dir.path().join(CHOCOLATEY_INSTALL)).unwrap();
        assert!(
            choco_text.contains(&"0".repeat(64)),
            "the fixture must carry the checksum64 placeholder"
        );
        assert!(
            !report.failed.contains(&"Chocolatey"),
            "{:?}",
            report.failed
        );
    }

    #[test]
    fn bump_rejects_a_prerelease_suffix() {
        let dir = packaging_fixture(VERSION);
        let error = bump(dir.path(), &format!("{OTHER}-rc1"), false).unwrap_err();
        assert!(error.to_string().contains("prerelease"));
        let cmake = std::fs::read_to_string(dir.path().join("CMakeLists.txt")).unwrap();
        assert!(
            cmake.contains(VERSION),
            "a refused bump must not have edited anything"
        );
    }

    #[test]
    fn bump_to_the_declared_version_is_a_noop() {
        let dir = packaging_fixture(VERSION);
        let before = std::fs::read_to_string(dir.path().join(SCOOP_MANIFEST)).unwrap();
        let outcome = bump(dir.path(), VERSION, false).unwrap();
        assert!(outcome.already_current);
        assert!(outcome.ok());
        let after = std::fs::read_to_string(dir.path().join(SCOOP_MANIFEST)).unwrap();
        assert_eq!(
            before, after,
            "a no-op bump must not reset an already-published hash"
        );
    }

    #[test]
    fn bump_refuses_a_dirty_tree_without_force() {
        let dir = packaging_fixture(VERSION);
        crate::test_support::init_committed_git_repo(dir.path());
        std::fs::write(dir.path().join(SCOOP_MANIFEST), "dirty").unwrap();
        let error = bump(dir.path(), OTHER, false).unwrap_err();
        assert!(error.to_string().contains("uncommitted changes"));
    }

    #[test]
    fn bump_force_bypasses_the_dirty_tree_refusal() {
        let dir = packaging_fixture(VERSION);
        crate::test_support::init_committed_git_repo(dir.path());
        edit_file(
            dir.path(),
            SCOOP_MANIFEST,
            "\"description\":",
            "\"description\" :",
        );
        let outcome = bump(dir.path(), OTHER, true).unwrap();
        assert!(!outcome.already_current);
    }

    #[test]
    fn bump_moves_every_literal_and_the_gate_agrees() {
        let dir = packaging_fixture(VERSION);
        let outcome = bump(dir.path(), OTHER, false).unwrap();
        assert!(
            outcome.ok(),
            "{:?}",
            outcome.drift.as_ref().map(|d| &d.failed)
        );

        let cmake = std::fs::read_to_string(dir.path().join("CMakeLists.txt")).unwrap();
        assert!(cmake.contains(&format!("VERSION {OTHER}")));

        let manifests = dir
            .path()
            .join("packaging/winget/manifests/c/Codexo/ExoSnap");
        assert!(manifests.join(OTHER).is_dir());
        assert!(!manifests.join(VERSION).exists());

        for relative in [CHOCOLATEY_NUSPEC, CHOCOLATEY_INSTALL, SCOOP_MANIFEST] {
            let text = std::fs::read_to_string(dir.path().join(relative)).unwrap();
            let (_, remaining) = replace_version_literal(&text, VERSION, "");
            assert_eq!(remaining, 0, "{relative} still names the old version");
        }
    }

    #[test]
    fn bump_resets_the_values_only_a_release_can_produce() {
        let dir = packaging_fixture(VERSION);
        bump(dir.path(), OTHER, false).unwrap();

        let choco = std::fs::read_to_string(dir.path().join(CHOCOLATEY_INSTALL)).unwrap();
        assert!(choco.contains(&format!("checksum64     = '{}'", "0".repeat(64))));

        let scoop = std::fs::read_to_string(dir.path().join(SCOOP_MANIFEST)).unwrap();
        assert!(scoop.contains(&format!("\"hash\": \"{}\"", "0".repeat(64))));

        let installer =
            std::fs::read_to_string(dir.path().join(winget_installer_manifest(OTHER))).unwrap();
        assert!(installer.contains(&format!("InstallerSha256: '{}'", "0".repeat(64))));
        assert!(
            !installer.contains("ProductCode: '{8988DAFC"),
            "every ProductCode must be reset, not only the installer's own"
        );
        assert_eq!(
            installer
                .matches("'{00000000-0000-0000-0000-000000000000}'")
                .count(),
            2,
            "both the installer and the AppsAndFeaturesEntries ProductCode must be reset"
        );
    }

    #[test]
    fn bump_fails_loudly_when_a_listed_file_no_longer_names_the_current_version() {
        let dir = packaging_fixture(VERSION);
        std::fs::write(dir.path().join(CHOCOLATEY_NUSPEC), "<package></package>").unwrap();
        let error = bump(dir.path(), OTHER, false).unwrap_err();
        assert!(error.to_string().contains("is stale"));
    }

    #[test]
    fn bump_fails_loudly_when_a_listed_file_is_missing() {
        let dir = packaging_fixture(VERSION);
        std::fs::remove_file(dir.path().join(CHOCOLATEY_NUSPEC)).unwrap();
        let error = bump(dir.path(), OTHER, false).unwrap_err();
        assert!(error.to_string().contains("is not there"));
    }
}
