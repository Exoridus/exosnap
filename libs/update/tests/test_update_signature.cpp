// test_update_signature.cpp -- ed25519 verify + manifest signature tests.
//
// Uses a test key pair generated from the RFC 8032 §6.1 test vectors so
// the tests are fully deterministic without requiring the CI secret.

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <update/ed25519_verify.h>
#include <update/manifest_io.h>
#include <update/update_types.h>

using namespace exosnap::update;

// ---------------------------------------------------------------------------
// RFC 8032 §6.1 Test Vector 1
// Private key (seed):  9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae3d55
// Public key:          d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
// Message:             (empty)
// Signature:
// e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b
// ---------------------------------------------------------------------------
static const uint8_t kTestPub[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
                                     0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
                                     0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
static const uint8_t kTestSig1[64] = {0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80,
                                      0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73,
                                      0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b,
                                      0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b, 0xd2, 0x5b, 0xf5, 0xf0,
                                      0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};

TEST(Ed25519, RFC8032Vector1EmptyMessage) {
    EXPECT_TRUE(ed25519_verify(kTestSig1, nullptr, 0, kTestPub));
}

TEST(Ed25519, TamperedSignatureFails) {
    uint8_t bad_sig[64];
    memcpy(bad_sig, kTestSig1, 64);
    bad_sig[0] ^= 0x01;
    EXPECT_FALSE(ed25519_verify(bad_sig, nullptr, 0, kTestPub));
}

TEST(Ed25519, WrongMessageFails) {
    const uint8_t msg[] = {0x01};
    EXPECT_FALSE(ed25519_verify(kTestSig1, msg, 1, kTestPub));
}

TEST(Ed25519, AllZeroSignatureRejected) {
    uint8_t zero_sig[64]{};
    EXPECT_FALSE(ed25519_verify(zero_sig, nullptr, 0, kTestPub));
}

// ---------------------------------------------------------------------------
// RFC 8032 §6.1 Test Vector 2
// Private key (seed):  4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4d0bd6f9
// Public key:          3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c
// Message:             72 (one byte, value 0x72)
// Signature:
// 92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00
// ---------------------------------------------------------------------------
static const uint8_t kTestPub2[32] = {0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a, 0x92, 0xb7, 0x0a,
                                      0xa7, 0x4d, 0x1b, 0x7e, 0xbc, 0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4,
                                      0x96, 0x8c, 0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
static const uint8_t kTestSig2[64] = {0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b, 0x5f,
                                      0x64, 0x25, 0x40, 0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f, 0xb3, 0x76,
                                      0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda, 0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99,
                                      0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c, 0x38, 0x7b, 0x2e, 0xae,
                                      0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00};
static const uint8_t kMsg2[] = {0x72};

TEST(Ed25519, RFC8032Vector2OneByte) {
    EXPECT_TRUE(ed25519_verify(kTestSig2, kMsg2, 1, kTestPub2));
}

// ---------------------------------------------------------------------------
// Cross-implementation round-trip for the DETACHED manifest signature.
//
// The fixture below is produced exactly the way the CI signer (sign-manifest.yml)
// produces a release: an ed25519 signature over the EXACT bytes of
// update-manifest.json. The signer and this fixture share the same test key pair
// (NOT the production key), derived from the fixed 32-byte seed 00 01 02 ... 1f.
// Public key: 03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8
//
// scripts/dev/gen-manifest-fixture.py regenerates the manifest text + signature.
// ---------------------------------------------------------------------------
static const uint8_t kFixturePubKey[32] = {0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe, 0x1d, 0x70, 0xdd,
                                           0x18, 0xe7, 0x4b, 0xc0, 0x99, 0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5,
                                           0x0d, 0x5f, 0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55, 0x31, 0xb8};

// The exact bytes of update-manifest.json the signer signed (LF line endings).
static const char* kFixtureManifestRaw = R"JSON({
  "version": "1.2.3",
  "minimum_accepted_version": "1.2.3",
  "packages": [
    {
      "kind": "installer",
      "url": "https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64.msi",
      "sha256": "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"
    },
    {
      "kind": "portable",
      "url": "https://github.com/Exoridus/exosnap/releases/download/v1.2.3/ExoSnap-1.2.3-windows-x64-portable.zip",
      "sha256": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
    }
  ]
})JSON";

// Detached signature over kFixtureManifestRaw (the .sig sidecar content).
static const char* kFixtureSignatureHex = "728d0161a8f42472b52d2e6e6fb5d54b5e7ceb670b12b95c79451c25f8548344"
                                          "20a8c1fd45f8a2143d519ddbd0dc16f99d0e3350a91b94e3c9772dab587e7206";

// The manifest bytes travel over the wire with LF endings; a CRLF checkout of
// this source file would embed CRLF in the literal, so normalise to LF here so
// the byte string matches what the signer signed regardless of git autocrlf.
static std::string FixtureManifest() {
    std::string s = kFixtureManifestRaw;
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

TEST(ManifestVerify, DetachedSignatureVerifies) {
    const std::string manifest = FixtureManifest();
    // The manifest must parse and its detached signature must verify against the key.
    auto result = ParseManifest(manifest);
    ASSERT_TRUE(std::holds_alternative<UpdateManifest>(result));
    EXPECT_EQ(VerifyManifestSignature(manifest, kFixtureSignatureHex, kFixturePubKey), VerifyResult::Ok);
}

TEST(ManifestVerify, TamperedManifestByteFailsVerification) {
    std::string manifest = FixtureManifest();
    manifest[manifest.find("1.2.3")] = '9'; // flip a version digit
    EXPECT_EQ(VerifyManifestSignature(manifest, kFixtureSignatureHex, kFixturePubKey),
              VerifyResult::ManifestSigInvalid);
}

TEST(ManifestVerify, WrongPublicKeyFailsVerification) {
    uint8_t other_key[32];
    memcpy(other_key, kFixturePubKey, 32);
    other_key[0] ^= 0x01;
    EXPECT_EQ(VerifyManifestSignature(FixtureManifest(), kFixtureSignatureHex, other_key),
              VerifyResult::ManifestSigInvalid);
}

TEST(ManifestVerify, MalformedSignatureHexFailsVerification) {
    EXPECT_EQ(VerifyManifestSignature(FixtureManifest(), "not-hex", kFixturePubKey), VerifyResult::ManifestSigInvalid);
    EXPECT_EQ(VerifyManifestSignature(FixtureManifest(), std::string(128, 'z'), kFixturePubKey),
              VerifyResult::ManifestSigInvalid);
}

// The default embedded key is the all-zero dev placeholder (an invalid curve
// point), so even a genuinely valid detached signature must be rejected.
TEST(ManifestVerify, DevBuildKeyRejectsValidSignature) {
    EXPECT_NE(VerifyManifestSignature(FixtureManifest(), kFixtureSignatureHex), VerifyResult::Ok);
}

TEST(ManifestVerify, DowngradeDetected) {
    SemVer current{1, 2, 0};
    UpdateManifest m;
    m.version = SemVer{1, 1, 0};
    m.minimum_accepted_version = SemVer{1, 0, 0};
    EXPECT_TRUE(IsDowngrade(m, current));
}

TEST(ManifestVerify, SameVersionIsNotDowngrade) {
    SemVer current{1, 2, 0};
    UpdateManifest m;
    m.version = SemVer{1, 2, 0};
    m.minimum_accepted_version = SemVer{1, 0, 0};
    EXPECT_FALSE(IsDowngrade(m, current));
}

TEST(ManifestVerify, BelowMinimumIsDowngrade) {
    SemVer current{0, 9, 0};
    UpdateManifest m;
    m.version = SemVer{0, 9, 0};
    m.minimum_accepted_version = SemVer{1, 0, 0};
    EXPECT_TRUE(IsDowngrade(m, current));
}
