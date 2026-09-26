//! Regenerates the detached-signature fixture pasted into
//! `libs/update/tests/test_update_signature.cpp`.
//!
//! The updater verifies a DETACHED ed25519 signature over the exact bytes of
//! update-manifest.json (the signature lives in a sibling `.sig` asset, never
//! inside the JSON). This mirrors what the release signing workflow does, so
//! the fixture is a genuine cross-implementation round trip: this signer uses
//! the same byte construction the release signer uses, and the C++ client
//! verifies it.
//!
//! The key pair is derived from a fixed, non-secret seed so the fixture is
//! fully deterministic and requires no secret. It is a TEST key, never the
//! production key. The manifest body is likewise fixed: this module
//! reproduces one pinned fixture, it does not build manifests in general.
//! Changing the seed or the manifest body changes every literal already
//! pasted in the C++ test, so do not change either without also repasting
//! there.

use ed25519_dalek::{Signer, SigningKey};

/// Fixed, non-secret test seed: 0x00, 0x01, ..., 0x1f.
const SEED: [u8; 32] = {
    let mut seed = [0u8; 32];
    let mut i = 0;
    while i < 32 {
        seed[i] = i as u8;
        i += 1;
    }
    seed
};

/// The exact bytes of update-manifest.json the fixture's signature covers.
/// LF line endings, matching a two-space-indent JSON pretty-printer over the
/// fixed manifest object below (version, minimum_accepted_version, and two
/// packages).
const MANIFEST_JSON: &str = "{\n  \"version\": \"1.2.3\",\n  \"minimum_accepted_version\": \"1.2.3\",\n  \"packages\": [\n    {\n      \"kind\": \"installer\",\n      \"url\": \"https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64.msi\",\n      \"sha256\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\"\n    },\n    {\n      \"kind\": \"portable\",\n      \"url\": \"https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64-portable.zip\",\n      \"sha256\": \"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"\n    }\n  ]\n}";

/// One detached-signature fixture: a public key, the exact manifest bytes it
/// was signed over, and the detached signature over those bytes.
pub struct ManifestFixture {
    pub public_key: [u8; 32],
    pub manifest_bytes: Vec<u8>,
    pub signature: [u8; 64],
}

/// Derives the fixed test key pair from `SEED` and signs `MANIFEST_JSON`.
/// Fully deterministic: repeated calls, and every language's implementation
/// of this same construction, produce byte-identical output.
pub fn generate() -> ManifestFixture {
    let signing_key = SigningKey::from_bytes(&SEED);
    let manifest_bytes = MANIFEST_JSON.as_bytes().to_vec();
    let signature = signing_key.sign(&manifest_bytes);
    ManifestFixture {
        public_key: signing_key.verifying_key().to_bytes(),
        manifest_bytes,
        signature: signature.to_bytes(),
    }
}

/// Renders a fixture the way the legacy Python fixture generator printed it,
/// ready to paste into the C++ test.
pub fn render(fixture: &ManifestFixture) -> String {
    let c_array = fixture
        .public_key
        .iter()
        .map(|b| format!("0x{b:02x}"))
        .collect::<Vec<_>>()
        .join(", ");
    let manifest_text = String::from_utf8_lossy(&fixture.manifest_bytes);
    format!(
        "Public key (hex): {}\nPublic key (C array):\n  {{{}}}\n\nSignature (hex): {}\n\nManifest bytes (LF):\n{}\n",
        hex::encode(fixture.public_key),
        c_array,
        hex::encode(fixture.signature),
        manifest_text,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    // The exact literals currently pasted in
    // libs/update/tests/test_update_signature.cpp. This test exists to catch
    // any drift between this generator and what is actually pasted there;
    // it must never be updated to match a changed generator, only the other
    // way around.
    const EXPECTED_PUBLIC_KEY: [u8; 32] = [
        0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe, 0x1d, 0x70, 0xdd, 0x18, 0xe7, 0x4b, 0xc0,
        0x99, 0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5, 0x0d, 0x5f, 0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55,
        0x31, 0xb8,
    ];

    const EXPECTED_SIGNATURE_HEX: &str = "728d0161a8f42472b52d2e6e6fb5d54b5e7ceb670b12b95c79451c25f854834\
420a8c1fd45f8a2143d519ddbd0dc16f99d0e3350a91b94e3c9772dab587e7206";

    const EXPECTED_MANIFEST: &str = "{\n  \"version\": \"1.2.3\",\n  \"minimum_accepted_version\": \"1.2.3\",\n  \"packages\": [\n    {\n      \"kind\": \"installer\",\n      \"url\": \"https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64.msi\",\n      \"sha256\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\"\n    },\n    {\n      \"kind\": \"portable\",\n      \"url\": \"https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64-portable.zip\",\n      \"sha256\": \"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"\n    }\n  ]\n}";

    #[test]
    fn matches_the_fixture_pasted_in_the_cpp_test() {
        let fixture = generate();
        assert_eq!(fixture.public_key, EXPECTED_PUBLIC_KEY);
        assert_eq!(fixture.manifest_bytes, EXPECTED_MANIFEST.as_bytes());
        assert_eq!(hex::encode(fixture.signature), EXPECTED_SIGNATURE_HEX);
    }

    #[test]
    fn render_matches_the_legacy_scripts_output_shape() {
        let fixture = generate();
        let rendered = render(&fixture);
        assert!(rendered.starts_with(
            "Public key (hex): 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8\n"
        ));
        assert!(rendered.contains("Public key (C array):\n  {0x03, 0xa1, 0x07"));
        assert!(rendered.contains(&format!("Signature (hex): {EXPECTED_SIGNATURE_HEX}\n")));
        assert!(rendered.ends_with(&format!("Manifest bytes (LF):\n{EXPECTED_MANIFEST}\n")));
    }
}
