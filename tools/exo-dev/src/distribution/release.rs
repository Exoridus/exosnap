//! The typed final-release input every package channel prepares from.
//!
//! The model is resolved from the actual public GitHub Release and the
//! already-downloaded assets, never from the packaging files that merely
//! repeat a version. Every digest crosses three independent statements: the
//! asset digest the release API reports, the published `.sha256` sidecar, and
//! the bytes on disk. Any disagreement fails closed.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use anyhow::{Context as _, Result, ensure};
use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};

pub fn msi_name(version: &str) -> String {
    format!("ExoSnap-{version}-windows-x64.msi")
}

pub fn portable_name(version: &str) -> String {
    format!("ExoSnap-{version}-windows-x64-portable.zip")
}

pub fn portable_sidecar_name(version: &str) -> String {
    format!("ExoSnap-{version}-windows-x64-portable.sha256")
}

pub fn sha256_file(path: &Path) -> Result<(String, u64)> {
    let bytes = fs::read(path).with_context(|| format!("could not read {}", path.display()))?;
    let digest = Sha256::digest(&bytes);
    Ok((hex::encode(digest), bytes.len() as u64))
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

pub fn is_sha256(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_hexdigit())
}

/// One final package asset, located on the immutable public release.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PackageAsset {
    pub filename: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
}

/// The typed final-release input every channel prepares from.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DistributionRelease {
    pub version: String,
    pub tag: String,
    pub source_commit: String,
    pub release_id: u64,
    pub release_url: String,
    pub published_at: String,
    pub msi: PackageAsset,
    pub portable: PackageAsset,
}

static VERSION_SHAPE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^[0-9]+\.[0-9]+\.[0-9]+$").unwrap());

fn asset<'a>(assets: &'a [Value], name: &str) -> Result<&'a Value> {
    assets
        .iter()
        .find(|entry| entry["name"] == name)
        .with_context(|| format!("the release carries no asset named '{name}'"))
}

fn asset_digest(entry: &Value) -> Result<String> {
    let digest = entry["digest"]
        .as_str()
        .context("the release asset carries no digest")?;
    let hex = digest
        .strip_prefix("sha256:")
        .context("the release asset digest is not a SHA-256")?;
    ensure!(is_sha256(hex), "the release asset digest is malformed");
    Ok(hex.to_ascii_lowercase())
}

fn verify_asset(
    assets_dir: &Path,
    entry: &Value,
    filename: &str,
    sidecar: &str,
) -> Result<PackageAsset> {
    let url = entry["browser_download_url"]
        .as_str()
        .context("the release asset carries no download URL")?;
    let size = entry["size"]
        .as_u64()
        .context("the release asset carries no size")?;
    let api_digest = asset_digest(entry)?;
    let sidecar_text = fs::read_to_string(assets_dir.join(sidecar))
        .with_context(|| format!("the release sidecar '{sidecar}' was not downloaded"))?;
    ensure!(
        sidecar_text == format!("{api_digest}  {filename}\n"),
        "the published sidecar '{sidecar}' differs from the release API digest"
    );
    let path = assets_dir.join(filename);
    let (disk_digest, disk_size) = sha256_file(&path)?;
    ensure!(
        disk_digest == api_digest,
        "the downloaded '{filename}' hashes to {disk_digest}, the release declares {api_digest}"
    );
    ensure!(
        disk_size == size,
        "the downloaded '{filename}' is {disk_size} bytes, the release declares {size}"
    );
    Ok(PackageAsset {
        filename: filename.to_string(),
        url: url.to_string(),
        sha256: api_digest,
        size,
    })
}

impl DistributionRelease {
    /// Resolves the typed model from the release API document and the
    /// downloaded assets. The caller obtains both from the immutable public
    /// release; nothing here reads a packaging file.
    pub fn load(
        version: &str,
        source_commit: &str,
        release_json: &Path,
        assets_dir: &Path,
    ) -> Result<DistributionRelease> {
        ensure!(
            VERSION_SHAPE.is_match(version),
            "version '{version}' is not x.y.z"
        );
        ensure!(
            source_commit.len() == 40 && source_commit.bytes().all(|b| b.is_ascii_hexdigit()),
            "source commit '{source_commit}' is not a full commit"
        );
        let release: Value = serde_json::from_slice(
            &fs::read(release_json)
                .with_context(|| format!("could not read {}", release_json.display()))?,
        )
        .context("the release metadata is not JSON")?;
        let tag = format!("v{version}");
        ensure!(
            release["tag_name"] == tag,
            "the release metadata names '{}', not '{tag}'",
            release["tag_name"]
        );
        ensure!(
            release["draft"] == false && release["prerelease"] == false,
            "the release is a draft or a prerelease"
        );
        let release_id = release["id"]
            .as_u64()
            .filter(|id| *id > 0)
            .context("the release metadata carries no release ID")?;
        let release_url = release["html_url"]
            .as_str()
            .context("the release metadata carries no URL")?;
        let published_at = release["published_at"]
            .as_str()
            .context("the release metadata carries no publication time")?;

        let assets = release["assets"]
            .as_array()
            .context("the release metadata carries no assets")?;
        let msi_entry = asset(assets, &msi_name(version))?;
        let portable_entry = asset(assets, &portable_name(version))?;
        let msi = verify_asset(
            assets_dir,
            msi_entry,
            &msi_name(version),
            &format!("{}.sha256", msi_name(version)),
        )?;
        let portable = verify_asset(
            assets_dir,
            portable_entry,
            &portable_name(version),
            &portable_sidecar_name(version),
        )?;

        Ok(DistributionRelease {
            version: version.to_string(),
            tag,
            source_commit: source_commit.to_string(),
            release_id,
            release_url: release_url.to_string(),
            published_at: published_at.to_string(),
            msi,
            portable,
        })
    }

    pub fn msi_path(&self, assets_dir: &Path) -> PathBuf {
        assets_dir.join(&self.msi.filename)
    }

    pub fn portable_path(&self, assets_dir: &Path) -> PathBuf {
        assets_dir.join(&self.portable.filename)
    }

    /// The date WinGet's `ReleaseDate` records, from the immutable release.
    pub fn release_date(&self) -> String {
        self.published_at
            .split('T')
            .next()
            .unwrap_or(&self.published_at)
            .to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn fixture(dir: &Path) -> (PathBuf, PathBuf) {
        let assets = dir.join("assets");
        fs::create_dir_all(&assets).unwrap();
        let msi = b"msi bytes".to_vec();
        let portable = b"portable bytes".to_vec();
        let msi_sha = sha256_hex(&msi);
        let portable_sha = sha256_hex(&portable);
        fs::write(assets.join("ExoSnap-1.2.3-windows-x64.msi"), &msi).unwrap();
        fs::write(
            assets.join("ExoSnap-1.2.3-windows-x64-portable.zip"),
            &portable,
        )
        .unwrap();
        fs::write(
            assets.join("ExoSnap-1.2.3-windows-x64.msi.sha256"),
            format!("{msi_sha}  ExoSnap-1.2.3-windows-x64.msi\n"),
        )
        .unwrap();
        fs::write(
            assets.join("ExoSnap-1.2.3-windows-x64-portable.sha256"),
            format!("{portable_sha}  ExoSnap-1.2.3-windows-x64-portable.zip\n"),
        )
        .unwrap();
        let release = json!({
            "id": 7,
            "tag_name": "v1.2.3",
            "draft": false,
            "prerelease": false,
            "html_url": "https://github.com/Exoridus/exosnap/releases/tag/v1.2.3",
            "published_at": "2026-10-02T16:28:12Z",
            "assets": [
                {"name": "ExoSnap-1.2.3-windows-x64.msi", "size": msi.len(), "digest": format!("sha256:{msi_sha}"), "browser_download_url": "https://example.invalid/msi"},
                {"name": "ExoSnap-1.2.3-windows-x64.msi.sha256", "size": 1, "digest": format!("sha256:{}", sha256_hex(b"x")), "browser_download_url": "https://example.invalid/msi.sha256"},
                {"name": "ExoSnap-1.2.3-windows-x64-portable.zip", "size": portable.len(), "digest": format!("sha256:{portable_sha}"), "browser_download_url": "https://example.invalid/zip"},
                {"name": "ExoSnap-1.2.3-windows-x64-portable.sha256", "size": 1, "digest": format!("sha256:{}", sha256_hex(b"y")), "browser_download_url": "https://example.invalid/zip.sha256"},
            ]
        });
        let release_path = dir.join("release.json");
        fs::write(&release_path, serde_json::to_vec_pretty(&release).unwrap()).unwrap();
        (release_path, assets)
    }

    #[test]
    fn a_consistent_public_release_resolves_all_three_statements() {
        let dir = tempfile::tempdir().unwrap();
        let (release, assets) = fixture(dir.path());
        let model = DistributionRelease::load("1.2.3", &"a".repeat(40), &release, &assets).unwrap();
        assert_eq!(model.tag, "v1.2.3");
        assert_eq!(model.release_date(), "2026-10-02");
        assert!(is_sha256(&model.msi.sha256));
        assert_eq!(model.msi.size, 9);
    }

    #[test]
    fn a_release_bytes_sidecar_or_identity_mismatch_fails_closed() {
        let dir = tempfile::tempdir().unwrap();
        let (release, assets) = fixture(dir.path());
        let mutate = |name: &str, edit: &dyn Fn(&str) -> String| {
            let path = dir.path().join(name);
            let text = fs::read_to_string(&path).unwrap();
            fs::write(&path, edit(&text)).unwrap();
        };

        mutate("release.json", &|text| {
            text.replace("\"tag_name\": \"v1.2.3\"", "\"tag_name\": \"v9.9.9\"")
        });
        assert!(
            DistributionRelease::load("1.2.3", &"a".repeat(40), &release, &assets)
                .unwrap_err()
                .to_string()
                .contains("not 'v1.2.3'")
        );
        mutate("release.json", &|text| {
            text.replace("\"tag_name\": \"v9.9.9\"", "\"tag_name\": \"v1.2.3\"")
        });

        mutate("release.json", &|text| {
            text.replace("\"draft\": false", "\"draft\": true")
        });
        assert!(DistributionRelease::load("1.2.3", &"a".repeat(40), &release, &assets).is_err());
        mutate("release.json", &|text| {
            text.replace("\"draft\": true", "\"draft\": false")
        });

        fs::write(
            assets.join("ExoSnap-1.2.3-windows-x64.msi"),
            b"tampered msi bytes",
        )
        .unwrap();
        assert!(
            DistributionRelease::load("1.2.3", &"a".repeat(40), &release, &assets)
                .unwrap_err()
                .to_string()
                .contains("hashes to")
        );

        let (_, assets) = fixture(dir.path());
        fs::write(
            assets.join("ExoSnap-1.2.3-windows-x64.msi.sha256"),
            format!("{}  ExoSnap-1.2.3-windows-x64.msi\n", "0".repeat(64)),
        )
        .unwrap();
        assert!(
            DistributionRelease::load("1.2.3", &"a".repeat(40), &release, &assets)
                .unwrap_err()
                .to_string()
                .contains("sidecar")
        );
    }
}
