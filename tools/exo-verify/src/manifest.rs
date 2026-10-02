//! The signed update manifest.
//!
//! The client verifies a detached Ed25519 signature over the exact manifest
//! bytes it received, with the public key embedded at build time, before it
//! reads any field. The manifest therefore has no canonical form: whatever
//! bytes are signed here are the bytes that ship.
//!
//! The private seed is read from an environment variable and exists only in
//! process memory. It is never logged, echoed or written to disk.

use anyhow::{Context, Result, anyhow, bail, ensure};
use base64::Engine as _;
use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use std::path::Path;

use crate::bundle::{Bundle, FileRole, is_final_version, sha256_file};

pub const MANIFEST_NAME: &str = "update-manifest.json";
pub const SIGNATURE_NAME: &str = "update-manifest.json.sig";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Package {
    pub kind: String,
    pub url: String,
    pub sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Manifest {
    pub version: String,
    pub minimum_accepted_version: String,
    pub packages: Vec<Package>,
}

/// Loads the signing seed from `var` (base64 of the 32-byte Ed25519 seed).
pub fn signing_key_from_env(var: &str) -> Result<SigningKey> {
    let encoded = std::env::var(var).map_err(|_| anyhow!("{var} is not set"))?;
    let seed = base64::engine::general_purpose::STANDARD
        .decode(encoded.trim())
        .map_err(|_| anyhow!("{var} is not valid base64"))?;
    let seed: [u8; 32] = seed
        .try_into()
        .map_err(|_| anyhow!("{var} does not decode to a 32-byte Ed25519 seed"))?;
    Ok(SigningKey::from_bytes(&seed))
}

pub fn public_key_from_hex(hex_key: &str) -> Result<VerifyingKey> {
    let bytes: [u8; 32] = hex::decode(hex_key.trim())
        .context("public key is not hex")?
        .try_into()
        .map_err(|_| anyhow!("public key is not 32 bytes"))?;
    VerifyingKey::from_bytes(&bytes).context("public key is not a valid Ed25519 point")
}

/// Fails unless `key` is the private half of the key embedded in the product.
/// A mismatched pair would sign manifests every client rejects.
pub fn check_key_pair(key: &SigningKey, embedded_public_hex: &str) -> Result<()> {
    let embedded = public_key_from_hex(embedded_public_hex)?;
    ensure!(
        key.verifying_key() == embedded,
        "the signing key is not the private half of the embedded public key (derived {}..., embedded {}...)",
        &hex::encode(key.verifying_key().as_bytes())[..16],
        &hex::encode(embedded.as_bytes())[..16]
    );
    Ok(())
}

/// Builds the manifest for the bundle's exact package bytes. `installer_url`
/// and `portable_url` are where those bytes will be served.
pub fn for_bundle(bundle: &Bundle, installer_url: &str, portable_url: &str) -> Result<Manifest> {
    let version = &bundle.inventory.product_version;
    ensure!(
        is_final_version(version),
        "refusing to describe non-final version '{version}'"
    );
    for url in [installer_url, portable_url] {
        ensure!(
            url.starts_with("https://"),
            "package URL '{url}' is not https"
        );
    }
    let sha = |role| -> Result<String> {
        Ok(bundle
            .inventory
            .file(role)
            .ok_or_else(|| anyhow!("bundle lacks {role:?}"))?
            .sha256
            .clone())
    };
    Ok(Manifest {
        version: version.clone(),
        minimum_accepted_version: version.clone(),
        packages: vec![
            Package {
                kind: "installer".into(),
                url: installer_url.into(),
                sha256: sha(FileRole::Installer)?,
            },
            Package {
                kind: "portable".into(),
                url: portable_url.into(),
                sha256: sha(FileRole::Portable)?,
            },
        ],
    })
}

pub fn serialize(manifest: &Manifest) -> Result<Vec<u8>> {
    Ok(serde_json::to_vec_pretty(manifest)?)
}

pub fn sign(bytes: &[u8], key: &SigningKey) -> String {
    hex::encode(key.sign(bytes).to_bytes())
}

pub fn verify(bytes: &[u8], signature_hex: &str, public: &VerifyingKey) -> Result<()> {
    let raw: [u8; 64] = hex::decode(signature_hex.trim())
        .context("signature is not hex")?
        .try_into()
        .map_err(|_| anyhow!("signature is not 64 bytes"))?;
    public
        .verify(bytes, &Signature::from_bytes(&raw))
        .map_err(|_| anyhow!("the signature does not verify over the manifest bytes"))
}

/// Proves that a signed manifest describes exactly the given package files:
/// the signature verifies with the embedded key, the version is the one
/// expected, and every listed hash is the hash of the file on disk.
pub fn verify_against_files(
    manifest_bytes: &[u8],
    signature_hex: &str,
    public: &VerifyingKey,
    expected_version: &str,
    installer: &Path,
    portable: &Path,
) -> Result<Manifest> {
    verify(manifest_bytes, signature_hex, public)?;
    let manifest: Manifest =
        serde_json::from_slice(manifest_bytes).context("parse signed manifest")?;
    ensure!(
        manifest.version == expected_version
            && manifest.minimum_accepted_version == expected_version,
        "manifest announces {} (minimum {}), expected {expected_version}",
        manifest.version,
        manifest.minimum_accepted_version
    );
    for (kind, path) in [("installer", installer), ("portable", portable)] {
        let package = manifest
            .packages
            .iter()
            .find(|p| p.kind == kind)
            .ok_or_else(|| anyhow!("manifest lists no {kind} package"))?;
        let (actual, _) = sha256_file(path)?;
        if !package.sha256.eq_ignore_ascii_case(&actual) {
            bail!(
                "manifest {kind} SHA-256 {} does not match {} ({actual})",
                package.sha256,
                path.display()
            );
        }
    }
    Ok(manifest)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key() -> SigningKey {
        SigningKey::from_bytes(&[7u8; 32])
    }

    #[test]
    fn signature_round_trips_and_detects_tampering() {
        let bytes = br#"{"version":"0.10.0"}"#;
        let sig = sign(bytes, &key());
        verify(bytes, &sig, &key().verifying_key()).unwrap();
        assert!(verify(br#"{"version":"0.10.1"}"#, &sig, &key().verifying_key()).is_err());
    }

    #[test]
    fn mismatched_key_pair_is_refused() {
        let other = SigningKey::from_bytes(&[8u8; 32]);
        let embedded = hex::encode(other.verifying_key().as_bytes());
        assert!(check_key_pair(&key(), &embedded).is_err());
        check_key_pair(&key(), &hex::encode(key().verifying_key().as_bytes())).unwrap();
    }

    #[test]
    fn manifest_hashes_must_match_the_bytes() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = crate::bundle::tests::fixture(dir.path(), "0.10.0");
        let manifest = for_bundle(&bundle, "https://h/i.msi", "https://h/p.zip").unwrap();
        let bytes = serialize(&manifest).unwrap();
        let sig = sign(&bytes, &key());
        let installer = bundle.require(FileRole::Installer).unwrap();
        let portable = bundle.require(FileRole::Portable).unwrap();
        verify_against_files(
            &bytes,
            &sig,
            &key().verifying_key(),
            "0.10.0",
            &installer,
            &portable,
        )
        .unwrap();

        let broken = dir.path().join("broken.msi");
        std::fs::write(&broken, b"not the qualified bytes").unwrap();
        let error = verify_against_files(
            &bytes,
            &sig,
            &key().verifying_key(),
            "0.10.0",
            &broken,
            &portable,
        )
        .unwrap_err()
        .to_string();
        assert!(error.contains("does not match"), "{error}");
        assert!(
            verify_against_files(
                &bytes,
                &sig,
                &key().verifying_key(),
                "0.10.1",
                &installer,
                &portable
            )
            .is_err()
        );
    }

    #[test]
    fn plain_http_package_urls_are_refused() {
        let dir = tempfile::tempdir().unwrap();
        let bundle = crate::bundle::tests::fixture(dir.path(), "0.10.0");
        assert!(for_bundle(&bundle, "http://h/i.msi", "https://h/p.zip").is_err());
    }
}
