// manifest_io.cpp -- ExoSnap update manifest parsing and verification.

#include <cstring>
#include <nlohmann/json.hpp>
#include <update/ed25519_verify.h>
#include <update/manifest_io.h>

namespace exosnap::update {

// ---------------------------------------------------------------------------
// Embedded public key
//
// Development builds use an all-zero placeholder (EXOSNAP_OFFICIAL_BUILD=OFF).
// Official release builds replace this with the real key injected at configure
// time from the CMake variable EXOSNAP_UPDATE_PUBLIC_KEY_BYTES (generated from
// the hex string in EXOSNAP_UPDATE_PUBLIC_KEY_HEX).
// ---------------------------------------------------------------------------
#ifndef EXOSNAP_UPDATE_PUBLIC_KEY_BYTES
// Development placeholder: y=2 does not encode a valid curve point, so
// ge_frombytes() rejects it and every signature verify returns false.
// Official release builds inject the real key via EXOSNAP_UPDATE_PUBLIC_KEY_BYTES.
#define EXOSNAP_UPDATE_PUBLIC_KEY_BYTES                                                                                \
    2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
#endif

const uint8_t kUpdatePublicKey[32] = {EXOSNAP_UPDATE_PUBLIC_KEY_BYTES};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::optional<std::array<uint8_t, 64>> ParseHex64(std::string_view hex) noexcept {
    if (hex.size() != 128)
        return std::nullopt;
    std::array<uint8_t, 64> out{};
    for (size_t i = 0; i < 64; ++i) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

// Extract the body for signature verification: the manifest JSON with the
// "signature" field removed, then the remaining object re-serialised.
static std::string ExtractBody(std::string_view raw_json) noexcept {
    try {
        auto j = nlohmann::json::parse(raw_json);
        j.erase("signature");
        return j.dump(); // compact, keys in insertion order
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// ParseManifest
// ---------------------------------------------------------------------------
ParseResult ParseManifest(std::string_view json) noexcept {
    try {
        auto j = nlohmann::json::parse(json);

        UpdateManifest m;

        // version
        auto version_sv = j.at("version").get<std::string>();
        auto ver = ParseSemVer(version_sv);
        if (!ver)
            return std::string{"invalid version: "} + version_sv;
        m.version = *ver;

        // minimum_accepted_version
        auto min_sv = j.at("minimum_accepted_version").get<std::string>();
        auto minver = ParseSemVer(min_sv);
        if (!minver)
            return std::string{"invalid minimum_accepted_version: "} + min_sv;
        m.minimum_accepted_version = *minver;

        // packages
        for (const auto& pkg : j.at("packages")) {
            PackageEntry e;
            auto kind_str = pkg.at("kind").get<std::string>();
            if (kind_str == "installer")
                e.kind = PackageKind::Installer;
            else if (kind_str == "portable")
                e.kind = PackageKind::Portable;
            else
                return std::string{"unknown package kind: "} + kind_str;
            e.url = pkg.at("url").get<std::string>();
            e.sha256_hex = pkg.at("sha256").get<std::string>();
            if (e.sha256_hex.size() != 64)
                return std::string{"sha256 must be 64 hex chars"};
            m.packages.push_back(std::move(e));
        }

        // signature
        auto sig_hex = j.at("signature").get<std::string>();
        auto sig = ParseHex64(sig_hex);
        if (!sig)
            return std::string{"invalid signature field (expected 128 hex chars)"};
        m.signature = *sig;

        return m;
    } catch (const nlohmann::json::exception& ex) {
        return std::string{"json parse error: "} + ex.what();
    } catch (...) {
        return std::string{"unknown parse error"};
    }
}

// ---------------------------------------------------------------------------
// VerifyManifestSignature
// ---------------------------------------------------------------------------
VerifyResult VerifyManifestSignature(const UpdateManifest& manifest, std::string_view raw_json) noexcept {
    std::string body = ExtractBody(raw_json);
    if (body.empty())
        return VerifyResult::ManifestParseError;

    const auto* msg = reinterpret_cast<const uint8_t*>(body.data());
    bool ok = ed25519_verify(manifest.signature.data(), msg, body.size(), kUpdatePublicKey);

    return ok ? VerifyResult::Ok : VerifyResult::ManifestSigInvalid;
}

// ---------------------------------------------------------------------------
// IsDowngrade
// ---------------------------------------------------------------------------
bool IsDowngrade(const UpdateManifest& manifest, const SemVer& current_version) noexcept {
    return manifest.version < current_version || manifest.version < manifest.minimum_accepted_version;
}

} // namespace exosnap::update
