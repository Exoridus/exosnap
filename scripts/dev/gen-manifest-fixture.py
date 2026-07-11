#!/usr/bin/env python3
"""Regenerate the detached-signature test fixture used by test_update_signature.cpp.

The updater verifies a DETACHED ed25519 signature over the exact bytes of
update-manifest.json (the signature lives in a sibling `.sig` asset, never inside
the JSON). This mirrors exactly what .github/workflows/sign-manifest.yml does, so
the fixture is a genuine cross-implementation round-trip: the Python signer here
uses the same byte construction the CI signer uses, and the C++ client verifies it.

The key pair is derived from a fixed, non-secret seed so the fixture is fully
deterministic and requires no CI secret. It is a TEST key, NOT the production key.

Run:  python scripts/dev/gen-manifest-fixture.py
Then paste the printed public key, manifest text, and signature into
libs/update/tests/test_update_signature.cpp.

Requires: pip install pynacl
"""

import json

import nacl.signing

# Deterministic TEST key pair (never the production key): seed = 00 01 02 ... 1f.
SEED = bytes(range(32))


def main() -> None:
    sk = nacl.signing.SigningKey(SEED)
    pub = bytes(sk.verify_key)

    manifest = {
        "version": "1.2.3",
        "minimum_accepted_version": "1.2.3",
        "packages": [
            {
                "kind": "installer",
                "url": "https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64.msi",
                "sha256": "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899",
            },
            {
                "kind": "portable",
                "url": "https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64-portable.zip",
                "sha256": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
            },
        ],
    }

    # Same byte construction as sign-manifest.yml: serialise once, sign the exact
    # bytes (LF line endings), keep the signature detached.
    manifest_bytes = json.dumps(manifest, indent=2).encode("utf-8")
    sig_hex = sk.sign(manifest_bytes).signature.hex()

    print("Public key (hex):", pub.hex())
    print("Public key (C array):")
    print("  {" + ", ".join(f"0x{b:02x}" for b in pub) + "}")
    print()
    print("Signature (hex):", sig_hex)
    print()
    print("Manifest bytes (LF):")
    print(manifest_bytes.decode("utf-8"))


if __name__ == "__main__":
    main()
