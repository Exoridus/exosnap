//! Detached authorization for the exact recorder and privileged helper bytes.

use anyhow::{Context, Result, ensure};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};
use std::fs::{self, File};
use std::io::Read;
use std::path::Path;

use crate::bundle::sha256_file;

pub const MANIFEST_NAME: &str = "diagnostics-pair.txt";
pub const SIGNATURE_NAME: &str = "diagnostics-pair.txt.sig";
const DOMAIN: &str = "EXOSNAP-DIAGNOSTICS-PAIR-V1\n";
const MANIFEST_LEN: usize = DOMAIN.len() + 2 * (64 + 1);

fn pair_manifest(recorder: &Path, helper: &Path) -> Result<String> {
    let (recorder_hash, _) = sha256_file(recorder)?;
    let (helper_hash, _) = sha256_file(helper)?;
    Ok(format!("{DOMAIN}{recorder_hash}\n{helper_hash}\n"))
}

pub fn sign_to_directory(
    out: &Path,
    recorder: &Path,
    helper: &Path,
    key: &SigningKey,
) -> Result<()> {
    let bytes = pair_manifest(recorder, helper)?;
    let sig = key.sign(bytes.as_bytes()).to_bytes();
    fs::create_dir_all(out)?;
    fs::write(out.join(MANIFEST_NAME), bytes)?;
    fs::write(out.join(SIGNATURE_NAME), sig)?;
    Ok(())
}

fn read_fixed<const N: usize>(path: &Path, kind: &str) -> Result<[u8; N]> {
    let mut bytes = Vec::with_capacity(N + 1);
    File::open(path)
        .with_context(|| format!("open {}", path.display()))?
        .take(N as u64 + 1)
        .read_to_end(&mut bytes)
        .with_context(|| format!("read {}", path.display()))?;
    bytes
        .try_into()
        .map_err(|_| anyhow::anyhow!("diagnostics pair {kind} is not {N} bytes"))
}

pub fn verify_in_directory(
    dir: &Path,
    recorder: &Path,
    helper: &Path,
    public: &VerifyingKey,
) -> Result<()> {
    let bytes = read_fixed::<MANIFEST_LEN>(&dir.join(MANIFEST_NAME), "manifest")?;
    let raw = read_fixed(&dir.join(SIGNATURE_NAME), "signature")?;
    public
        .verify_strict(&bytes, &Signature::from_bytes(&raw))
        .context("diagnostics pair signature does not verify")?;
    ensure!(
        bytes == pair_manifest(recorder, helper)?.as_bytes(),
        "diagnostics pair manifest does not match recorder and helper"
    );
    Ok(())
}
