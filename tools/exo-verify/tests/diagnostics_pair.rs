use base64::Engine as _;
use ed25519_dalek::{Signer, SigningKey};
use std::fs;
use std::path::PathBuf;
use std::process::{Command, Output};

const PUBLIC_KEY: &str = "ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c";
const WRONG_PUBLIC_KEY: &str = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const MANIFEST: &[u8] = b"EXOSNAP-DIAGNOSTICS-PAIR-V1\n\
93384247058b5e037a16c08536d5a3b3c20453cda6571c7e016942f9f93b274f\n\
e81d3b0e9d82feaaf5f6e55bdff24731d7eee08632ffa63801e6397290c5d20a\n";
const SIGNATURE_HEX: &str = concat!(
    "ac0d966ddcf7abb82d12ec2993b0cd3dc69a539238a990300e1d59948e6a40f150",
    "31f9f5bf16dca31bfbcb8f5b6b2da05ead213e27af42030eef02fcb82db00e"
);

struct Fixture {
    dir: tempfile::TempDir,
    recorder: PathBuf,
    helper: PathBuf,
}

impl Fixture {
    fn new() -> Self {
        let dir = tempfile::tempdir().unwrap();
        let recorder = dir.path().join("exosnap.exe");
        let helper = dir.path().join("exosnap-helper.exe");
        fs::write(&recorder, b"recorder").unwrap();
        fs::write(&helper, b"helper").unwrap();
        Self {
            dir,
            recorder,
            helper,
        }
    }

    fn output(&self, name: &str) -> PathBuf {
        self.dir.path().join("pair").join(name)
    }

    fn command(&self, action: &str, dir_flag: &str, public_key: &str) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_exo-verify"));
        command
            .args(["manifest", action, "--recorder"])
            .arg(&self.recorder)
            .arg("--helper")
            .arg(&self.helper)
            .arg(dir_flag)
            .arg(self.dir.path().join("pair"))
            .args(["--public-key-hex", public_key]);
        command
    }

    fn sign(&self, public_key: &str) -> Output {
        self.command("sign-diagnostics-pair", "--out", public_key)
            .env(
                "EXOSNAP_UPDATE_SIGNING_KEY",
                base64::engine::general_purpose::STANDARD.encode([7; 32]),
            )
            .output()
            .unwrap()
    }

    fn verify(&self, public_key: &str) -> Output {
        self.command("verify-diagnostics-pair", "--pair-dir", public_key)
            .env_remove("EXOSNAP_UPDATE_SIGNING_KEY")
            .output()
            .unwrap()
    }
}

fn assert_success(output: Output) {
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

fn assert_rejected(output: Output, expected: &str) {
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(!output.status.success(), "unexpected success");
    assert!(stderr.contains(expected), "{stderr}");
}

#[test]
fn signs_exact_pair_and_verifies_with_only_public_key() {
    let fixture = Fixture::new();
    assert_success(fixture.sign(PUBLIC_KEY));
    assert_eq!(
        fs::read(fixture.output("diagnostics-pair.txt")).unwrap(),
        MANIFEST
    );
    assert_eq!(
        fs::read(fixture.output("diagnostics-pair.txt.sig")).unwrap(),
        hex::decode(SIGNATURE_HEX).unwrap()
    );
    assert_success(fixture.verify(PUBLIC_KEY));
}

#[test]
fn rejects_wrong_key_missing_signature_and_changed_helper() {
    let fixture = Fixture::new();
    let signature = fixture.output("diagnostics-pair.txt.sig");
    assert_rejected(fixture.sign(WRONG_PUBLIC_KEY), "not the private half");
    assert!(!signature.exists());
    assert_success(fixture.sign(PUBLIC_KEY));
    assert_rejected(
        fixture.verify(WRONG_PUBLIC_KEY),
        "signature does not verify",
    );
    fs::write(&fixture.helper, b"mismatch").unwrap();
    assert_rejected(
        fixture.verify(PUBLIC_KEY),
        "does not match recorder and helper",
    );
    fs::remove_file(signature).unwrap();
    assert_rejected(fixture.verify(PUBLIC_KEY), "diagnostics-pair.txt.sig");
}

#[test]
fn rejects_changed_bytes_and_signed_wrong_role_order_domain_or_length() {
    let fixture = Fixture::new();
    assert_success(fixture.sign(PUBLIC_KEY));
    let manifest = fixture.output("diagnostics-pair.txt");
    let signature = fixture.output("diagnostics-pair.txt.sig");
    let mut changed = MANIFEST.to_vec();
    changed[28] = b'0';
    fs::write(&manifest, changed).unwrap();
    assert_rejected(fixture.verify(PUBLIC_KEY), "signature does not verify");

    let key = SigningKey::from_bytes(&[7; 32]);
    let header_len = b"EXOSNAP-DIAGNOSTICS-PAIR-V1\n".len();
    let swapped = [
        &MANIFEST[..header_len],
        &MANIFEST[header_len + 65..],
        &MANIFEST[header_len..header_len + 65],
    ]
    .concat();
    let mut wrong_domain = MANIFEST.to_vec();
    wrong_domain[0] = b'Z';
    for invalid in [&swapped, &wrong_domain] {
        fs::write(&manifest, invalid).unwrap();
        fs::write(&signature, key.sign(invalid).to_bytes()).unwrap();
        assert_rejected(
            fixture.verify(PUBLIC_KEY),
            "does not match recorder and helper",
        );
    }

    let too_long = [MANIFEST, b"\n"].concat();
    fs::write(&manifest, &too_long).unwrap();
    fs::write(&signature, key.sign(&too_long).to_bytes()).unwrap();
    assert_rejected(fixture.verify(PUBLIC_KEY), "manifest is not 158 bytes");
    fs::write(&manifest, MANIFEST).unwrap();
    fs::write(&signature, SIGNATURE_HEX).unwrap();
    assert_rejected(fixture.verify(PUBLIC_KEY), "signature is not 64 bytes");
}
