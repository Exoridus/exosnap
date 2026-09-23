//! The immutable release candidate: package bytes plus the inventory that names them.
//!
//! `bundle.json` lists every file with its SHA-256 and size, so hashing that one
//! document binds every byte of the candidate. The SHA-256 of `bundle.json` as
//! written is the bundle identity (`bundleSha256`) that every lane result,
//! release plan, decision file and report carries.

use anyhow::{Context, Result, anyhow, bail, ensure};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};

pub const BUNDLE_SCHEMA: &str = "exosnap.release-bundle/1";
pub const INVENTORY_NAME: &str = "bundle.json";

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum FileRole {
    Installer,
    Portable,
    Runtime,
    Metadata,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BundleFile {
    pub path: String,
    pub role: FileRole,
    pub sha256: String,
    pub size: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Inventory {
    pub schema: String,
    pub product: String,
    pub product_version: String,
    pub source_commit: String,
    pub candidate_id: String,
    pub created_at: String,
    #[serde(default)]
    pub toolchain: BTreeMap<String, String>,
    #[serde(default)]
    pub inputs: BTreeMap<String, String>,
    pub files: Vec<BundleFile>,
}

impl Inventory {
    pub fn file(&self, role: FileRole) -> Option<&BundleFile> {
        self.files.iter().find(|f| f.role == role)
    }

    pub fn packages(&self) -> BTreeMap<String, String> {
        self.files
            .iter()
            .filter(|f| matches!(f.role, FileRole::Installer | FileRole::Portable))
            .map(|f| (file_name(&f.path).to_string(), f.sha256.clone()))
            .collect()
    }
}

/// A verified bundle on disk.
#[derive(Debug, Clone)]
pub struct Bundle {
    pub root: PathBuf,
    pub sha256: String,
    pub inventory: Inventory,
}

impl Bundle {
    pub fn path_of(&self, file: &BundleFile) -> PathBuf {
        #[cfg(windows)]
        {
            self.root.join(file.path.replace('/', "\\"))
        }
        #[cfg(not(windows))]
        {
            self.root.join(&file.path)
        }
    }

    pub fn require(&self, role: FileRole) -> Result<PathBuf> {
        let file = self
            .inventory
            .file(role)
            .ok_or_else(|| anyhow!("the bundle carries no {role:?} file"))?;
        Ok(self.path_of(file))
    }
}

pub struct CreateRequest {
    pub out_dir: PathBuf,
    pub product_version: String,
    pub source_commit: String,
    pub candidate_id: String,
    pub installer: PathBuf,
    pub portable: PathBuf,
    pub runtimes: Vec<PathBuf>,
    pub metadata: Vec<PathBuf>,
    pub toolchain: BTreeMap<String, String>,
    pub inputs: BTreeMap<String, String>,
}

pub fn sha256_file(path: &Path) -> Result<(String, u64)> {
    let mut file = fs::File::open(path).with_context(|| format!("open {}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 20];
    let mut size = 0u64;
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        size += read as u64;
    }
    Ok((hex::encode(hasher.finalize()), size))
}

pub fn sha256_bytes(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

pub fn is_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

fn file_name(path: &str) -> &str {
    path.rsplit('/').next().unwrap_or(path)
}

/// Package file names the updater and the release page depend on.
pub fn installer_name(version: &str) -> String {
    format!("ExoSnap-{version}-windows-x64.msi")
}

pub fn portable_name(version: &str) -> String {
    format!("ExoSnap-{version}-windows-x64-portable.zip")
}

pub fn is_final_version(version: &str) -> bool {
    let parts: Vec<&str> = version.split('.').collect();
    parts.len() == 3
        && parts.iter().all(|p| {
            !p.is_empty()
                && p.bytes().all(|b| b.is_ascii_digit())
                && (p.len() == 1 || !p.starts_with('0'))
        })
}

pub fn create(request: &CreateRequest) -> Result<Bundle> {
    ensure!(
        is_final_version(&request.product_version),
        "'{}' is not a final x.y.z version; a candidate carries the version it will be published as",
        request.product_version
    );
    ensure!(
        request.source_commit.len() == 40
            && request.source_commit.bytes().all(|b| b.is_ascii_hexdigit()),
        "source commit must be a full 40-character SHA"
    );
    if request.out_dir.exists() {
        ensure!(
            fs::read_dir(&request.out_dir)?.next().is_none(),
            "{} is not empty; a bundle is never assembled over existing files",
            request.out_dir.display()
        );
    }
    fs::create_dir_all(&request.out_dir)?;

    let expected_installer = installer_name(&request.product_version);
    let expected_portable = portable_name(&request.product_version);
    check_name(&request.installer, &expected_installer)?;
    check_name(&request.portable, &expected_portable)?;

    let mut files = Vec::new();
    let mut place = |source: &Path, dir: &str, role: FileRole| -> Result<()> {
        let name = source
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or_else(|| anyhow!("{} has no usable file name", source.display()))?;
        let relative = format!("{dir}/{name}");
        ensure!(
            !files.iter().any(|f: &BundleFile| f.path == relative),
            "{relative} is supplied twice"
        );
        let target = request.out_dir.join(dir).join(name);
        fs::create_dir_all(target.parent().unwrap())?;
        fs::copy(source, &target).with_context(|| format!("copy {}", source.display()))?;
        let (sha256, size) = sha256_file(&target)?;
        files.push(BundleFile {
            path: relative,
            role,
            sha256,
            size,
        });
        Ok(())
    };
    place(&request.installer, "packages", FileRole::Installer)?;
    place(&request.portable, "packages", FileRole::Portable)?;
    for runtime in &request.runtimes {
        place(runtime, "runtimes", FileRole::Runtime)?;
    }
    for metadata in &request.metadata {
        place(metadata, "metadata", FileRole::Metadata)?;
    }
    files.sort_by(|a, b| a.path.cmp(&b.path));

    let inventory = Inventory {
        schema: BUNDLE_SCHEMA.to_string(),
        product: "ExoSnap".to_string(),
        product_version: request.product_version.clone(),
        source_commit: request.source_commit.to_ascii_lowercase(),
        candidate_id: request.candidate_id.clone(),
        created_at: crate::model::now_rfc3339(),
        toolchain: request.toolchain.clone(),
        inputs: request.inputs.clone(),
        files,
    };
    let mut bytes = serde_json::to_vec_pretty(&inventory)?;
    bytes.push(b'\n');
    fs::write(request.out_dir.join(INVENTORY_NAME), &bytes)?;
    open(&request.out_dir)
}

fn check_name(path: &Path, expected: &str) -> Result<()> {
    let actual = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default();
    ensure!(
        actual == expected,
        "{} must be named {expected}; the updater and release page locate packages by that name",
        path.display()
    );
    Ok(())
}

/// Opens a bundle directory and proves that every listed byte is present and
/// unchanged, and that nothing unlisted rides along.
pub fn open(root: &Path) -> Result<Bundle> {
    let inventory_path = root.join(INVENTORY_NAME);
    let bytes = fs::read(&inventory_path)
        .with_context(|| format!("{} is not a bundle: no {INVENTORY_NAME}", root.display()))?;
    let inventory: Inventory = serde_json::from_slice(&bytes)
        .with_context(|| format!("parse {}", inventory_path.display()))?;
    ensure!(
        inventory.schema == BUNDLE_SCHEMA,
        "unsupported bundle schema '{}'",
        inventory.schema
    );
    ensure!(
        is_final_version(&inventory.product_version),
        "bundle version is not x.y.z"
    );
    ensure!(
        inventory.file(FileRole::Installer).is_some()
            && inventory.file(FileRole::Portable).is_some(),
        "a bundle must carry both the installer and the portable package"
    );

    let mut listed = BTreeMap::new();
    for file in &inventory.files {
        safe_relative(&file.path)?;
        ensure!(
            is_sha256(&file.sha256),
            "{} carries a malformed SHA-256",
            file.path
        );
        let (sha256, size) = sha256_file(&root.join(&file.path))?;
        if sha256 != file.sha256 || size != file.size {
            bail!(
                "{} does not match the inventory (inventory {} / {} bytes, disk {} / {} bytes)",
                file.path,
                file.sha256,
                file.size,
                sha256,
                size
            );
        }
        listed.insert(file.path.clone(), ());
    }
    for path in walk(root)? {
        if path != INVENTORY_NAME && !listed.contains_key(&path) {
            bail!("{path} is in the bundle directory but not in its inventory");
        }
    }
    Ok(Bundle {
        root: root.to_path_buf(),
        sha256: sha256_bytes(&bytes),
        inventory,
    })
}

fn safe_relative(path: &str) -> Result<()> {
    let p = Path::new(path);
    ensure!(
        !path.is_empty()
            && !path.contains('\\')
            && p.components().all(|c| matches!(c, Component::Normal(_))),
        "unsafe bundle path '{path}'"
    );
    Ok(())
}

fn walk(root: &Path) -> Result<Vec<String>> {
    let mut out = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for entry in fs::read_dir(&dir)? {
            let entry = entry?;
            let path = entry.path();
            if entry.file_type()?.is_dir() {
                stack.push(path);
            } else {
                let relative = path
                    .strip_prefix(root)?
                    .to_string_lossy()
                    .replace('\\', "/");
                out.push(relative);
            }
        }
    }
    out.sort();
    Ok(out)
}

/// Writes the bundle as one stored (uncompressed) archive for transport.
/// Packages are already compressed; storing keeps packing fast and exact.
pub fn pack(bundle: &Bundle, archive: &Path) -> Result<()> {
    let file = fs::File::create(archive)?;
    let mut zip = zip::ZipWriter::new(file);
    let options = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Stored)
        .large_file(true);
    let mut names = vec![INVENTORY_NAME.to_string()];
    names.extend(bundle.inventory.files.iter().map(|f| f.path.clone()));
    for name in names {
        zip.start_file(name.as_str(), options)?;
        let mut source = fs::File::open(bundle.root.join(&name))?;
        std::io::copy(&mut source, &mut zip)?;
    }
    zip.finish()?.flush()?;
    Ok(())
}

pub fn unpack(archive: &Path, out_dir: &Path) -> Result<Bundle> {
    if out_dir.exists() {
        ensure!(
            fs::read_dir(out_dir)?.next().is_none(),
            "{} is not empty",
            out_dir.display()
        );
    }
    fs::create_dir_all(out_dir)?;
    let mut zip = zip::ZipArchive::new(fs::File::open(archive)?)?;
    for index in 0..zip.len() {
        let mut entry = zip.by_index(index)?;
        let name = entry.name().to_string();
        safe_relative(&name)?;
        let target = out_dir.join(&name);
        fs::create_dir_all(target.parent().unwrap())?;
        let mut out = fs::File::create(&target)?;
        std::io::copy(&mut entry, &mut out)?;
    }
    open(out_dir)
}

/// Opens either a bundle directory or a packed bundle archive. An archive is
/// unpacked next to itself, into `<archive>.d`, once.
pub fn open_any(path: &Path) -> Result<Bundle> {
    if path.is_dir() {
        return open(path);
    }
    let target = PathBuf::from(format!("{}.d", path.display()));
    if target.join(INVENTORY_NAME).is_file() {
        return open(&target);
    }
    unpack(path, &target)
}

#[cfg(test)]
pub mod tests {
    use super::*;

    pub fn fixture(dir: &Path, version: &str) -> Bundle {
        let src = dir.join("src");
        fs::create_dir_all(&src).unwrap();
        let installer = src.join(installer_name(version));
        let portable = src.join(portable_name(version));
        fs::write(&installer, b"msi bytes").unwrap();
        fs::write(&portable, b"zip bytes").unwrap();
        create(&CreateRequest {
            out_dir: dir.join("bundle"),
            product_version: version.to_string(),
            source_commit: "0123456789abcdef0123456789abcdef01234567".to_string(),
            candidate_id: format!("{version}-test"),
            installer,
            portable,
            runtimes: vec![],
            metadata: vec![],
            toolchain: BTreeMap::new(),
            inputs: BTreeMap::new(),
        })
        .unwrap()
    }

    #[test]
    fn identity_is_stable_across_reopen_and_pack_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = fixture(dir.path(), "0.10.0");
        assert!(is_sha256(&bundle.sha256));
        assert_eq!(open(&bundle.root).unwrap().sha256, bundle.sha256);
        let archive = dir.path().join("b.zip");
        pack(&bundle, &archive).unwrap();
        let unpacked = unpack(&archive, &dir.path().join("out")).unwrap();
        assert_eq!(unpacked.sha256, bundle.sha256);
    }

    #[test]
    fn a_changed_package_byte_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = fixture(dir.path(), "0.10.0");
        fs::write(
            bundle.root.join("packages").join(installer_name("0.10.0")),
            b"msi byteZ",
        )
        .unwrap();
        let error = open(&bundle.root).unwrap_err().to_string();
        assert!(error.contains("does not match the inventory"), "{error}");
    }

    #[cfg(windows)]
    #[test]
    fn package_paths_use_native_separators_for_windows_installers() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = fixture(dir.path(), "0.10.0");
        let installer = bundle.require(FileRole::Installer).unwrap();
        assert!(!installer.to_string_lossy().contains('/'));
    }

    #[test]
    fn an_unlisted_file_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = fixture(dir.path(), "0.10.0");
        fs::write(bundle.root.join("packages").join("extra.dll"), b"x").unwrap();
        assert!(
            open(&bundle.root)
                .unwrap_err()
                .to_string()
                .contains("not in its inventory")
        );
    }

    #[test]
    fn prerelease_versions_and_misnamed_packages_are_refused() {
        assert!(!is_final_version("0.10.0-rc1"));
        assert!(!is_final_version("0.10"));
        assert!(!is_final_version("01.2.3"));
        assert!(is_final_version("0.10.0"));
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("x.msi");
        fs::write(&src, b"a").unwrap();
        let request = CreateRequest {
            out_dir: dir.path().join("b"),
            product_version: "0.10.0".into(),
            source_commit: "0123456789abcdef0123456789abcdef01234567".into(),
            candidate_id: "c".into(),
            installer: src.clone(),
            portable: src,
            runtimes: vec![],
            metadata: vec![],
            toolchain: BTreeMap::new(),
            inputs: BTreeMap::new(),
        };
        assert!(
            create(&request)
                .unwrap_err()
                .to_string()
                .contains("must be named")
        );
    }
}
