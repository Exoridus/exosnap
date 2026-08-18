#pragma once

// handoff.h -- the versioned update handoff document ExoSnap hands the updater.
//
// WHAT IT IS
// ----------
// A small JSON file naming exactly one update operation: which release, which
// manifest bytes prove it, which installation it applies to, and which
// transaction it belongs to. The application writes it; exosnap-updater.exe is
// started with --apply-handoff <path> and reads it.
//
// WHAT IT IS NOT
// --------------
// A trust anchor. The document is UNTRUSTED input: it is written into a
// user-writable directory, it carries no signature of its own, and no decision
// in the updater may rest on one of its fields alone. What it does is POINT at
// the release trust chain that already exists:
//
//     handoff.manifestPath           -> exact manifest bytes
//     handoff.manifestSignaturePath  -> detached ed25519 signature
//     pinned public key (compiled in)
//     manifest.packages[].sha256     -> package identity
//
// The updater re-reads those bytes, verifies the signature over them BEFORE
// parsing a single field, and only then compares the manifest's version against
// handoff.targetVersion. Tampering with the document therefore cannot install
// anything: point it at other manifest bytes and the signature check fails;
// change targetVersion and the version gate refuses; change installDir and the
// install-context check refuses.
//
// Signing the document itself would add a second signature scheme guarding a
// value that is already fully constrained by the first one -- which is why it
// deliberately has none.
//
// VERSIONING
// ----------
// `handoffVersion` is validated exactly. An unknown version is a hard reject,
// never a best-effort read: a document written by a future ExoSnap may mean
// something different by the same field names, and guessing is how an updater
// installs the wrong thing. Unknown ADDITIONAL keys are ignored, which is the
// forward-compatible half of the rule -- a future writer may add fields that an
// older reader does not need, but it may not change the meaning of the ones
// here without bumping the version.

#include <QString>
#include <cstdint>
#include <optional>

#include <update/update_types.h>

class QByteArray;

namespace exosnap::update_handoff {

// The one schema version this build writes and the only one it accepts.
inline constexpr int kHandoffVersion = 1;

// The document's file name inside a transaction directory, and the argv option
// that points the updater at it. Both are shared so the writer and the launcher
// cannot spell them differently.
inline constexpr const char* kHandoffFileName = "update-handoff.json";
inline constexpr const char* kApplyHandoffOption = "--apply-handoff";

// The manifest and signature file names inside a transaction directory. The
// application downloads the release assets under these names; the document then
// names the full paths, so a reader never has to reconstruct them.
inline constexpr const char* kManifestFileName = "update-manifest.json";
inline constexpr const char* kManifestSignatureFileName = "update-manifest.json.sig";

// ---------------------------------------------------------------------------
// The document
// ---------------------------------------------------------------------------

struct UpdateHandoff {
    int handoff_version = kHandoffVersion;

    // Correlation identity for the whole operation: minted by the application
    // when it prepares the handoff, carried into the updater's published state
    // and into its structured evidence. Opaque, non-secret, NOT a credential --
    // it authorises nothing and is not the automation run id (which names a
    // control session and is what a pipe name is built from).
    QString update_transaction_id;

    // The EXACT release tag the application offered the user, verbatim. The
    // updater compares the signed manifest's version string against this
    // byte-for-byte; SemVer normalisation is deliberately never applied,
    // because it collapses foreign prerelease labels onto one another.
    QString target_version;
    // The version running right now. Two jobs: the downgrade guard's input, and
    // the install-context proof -- <installDir>\exosnap.exe must actually report
    // this version, which is what binds the named directory to the claim.
    QString current_version;

    // Absolute paths to the release manifest and its detached signature, as
    // downloaded by the application. Untrusted like every other field: the
    // updater reads the bytes and re-verifies them.
    QString manifest_path;
    QString manifest_signature_path;

    exosnap::update::InstallMode install_mode = exosnap::update::InstallMode::Portable;
    QString install_dir;

    // The application process the updater must wait for before touching the
    // installation. 0 is not accepted in a handoff: something started this
    // updater, and a handoff that cannot name its parent cannot sequence the
    // swap against it.
    quint32 app_pid = 0;

    // ADR 0055: this operation reinstalls the IDENTICAL version on purpose. Adds
    // the updater's same-version gate; relaxes nothing.
    bool verify_reinstall = false;

    [[nodiscard]] bool operator==(const UpdateHandoff&) const = default;
};

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

// Why a handoff was refused. Reported as data, never as prose: a runner has to
// be able to assert on the reason, and the failure card renders from it.
enum class HandoffRejection : std::uint8_t {
    None = 0,
    FileUnreadable,     // the path does not exist, or could not be opened/read
    MalformedJson,      // not a JSON object
    UnsupportedVersion, // handoffVersion is absent or not kHandoffVersion
    MissingField,       // a required field is absent or empty
    InvalidField,       // a field is present but not a value this schema allows
};

[[nodiscard]] const char* HandoffRejectionName(HandoffRejection rejection) noexcept;

struct HandoffLoadResult {
    std::optional<UpdateHandoff> handoff;
    HandoffRejection rejection = HandoffRejection::None;
    // Which field, or which read failed. User-facing copy is built elsewhere;
    // this is evidence.
    QString detail;

    [[nodiscard]] bool ok() const noexcept {
        return handoff.has_value();
    }
};

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

// Indented UTF-8 JSON. Deliberately no signature, no manifest copy and no
// package bytes: everything large or trust-bearing is referenced, not embedded.
[[nodiscard]] QByteArray SerializeUpdateHandoff(const UpdateHandoff& handoff);

// Pure parse + schema validation. No filesystem access: whether the referenced
// files exist is a separate question, answered by the caller that is about to
// use them.
[[nodiscard]] HandoffLoadResult ParseUpdateHandoff(const QByteArray& bytes);

// Read `path` and parse it. FileUnreadable when the file is missing or cannot be
// read; otherwise exactly what ParseUpdateHandoff says.
[[nodiscard]] HandoffLoadResult LoadUpdateHandoff(const QString& path);

// Write the document so no reader can ever observe a partial one: the bytes go
// to a temporary sibling, are flushed, and are then renamed over the final name.
// Returns false with `error` filled on any failure (the destination is left
// untouched in that case).
[[nodiscard]] bool WriteUpdateHandoffAtomically(const QString& path, const UpdateHandoff& handoff, QString* error);

// A fresh, opaque transaction identity ("u-" + 16 lowercase hex characters).
// Not a secret and not unguessable-by-design; unique enough that two operations
// on one machine cannot be confused for each other.
[[nodiscard]] QString MakeUpdateTransactionId();

// ---------------------------------------------------------------------------
// Install-context validation
// ---------------------------------------------------------------------------

// Why the named installation was refused. This is the check the earlier
// --install-dir argument never had: a directory handed over by an untrusted
// document decides where a directory swap happens, so it has to be proven to be
// an ExoSnap installation of the version the document claims is running.
enum class InstallContextRejection : std::uint8_t {
    None = 0,
    PathNotAbsolute,   // a relative path cannot name an installation unambiguously
    DirectoryMissing,  // the path does not exist, or is not a directory
    ExecutableMissing, // no exosnap.exe in it -- this is not an ExoSnap installation
    VersionUnreadable, // exosnap.exe carries no readable ProductVersion string
    VersionMismatch,   // it reports a different version than the document claims
    RegistryMismatch,  // installed mode: not the directory Windows recorded
};

[[nodiscard]] const char* InstallContextRejectionName(InstallContextRejection rejection) noexcept;

// The measurements the decision is made from, so the rule itself is a pure
// function and can be exhausted by tests without an installation on disk.
struct InstallContextFacts {
    bool path_is_absolute = false;
    bool directory_exists = false;
    bool executable_exists = false;
    // The ProductVersion string of <install_dir>\exosnap.exe; empty when it
    // could not be read.
    QString executable_product_version;
    // What the document claims is running there.
    QString claimed_current_version;
    exosnap::update::InstallMode install_mode = exosnap::update::InstallMode::Portable;
    // The installation directory Windows Installer recorded, canonicalised;
    // empty when there is no such record.
    QString registry_install_dir;
    // The document's directory, canonicalised, for the registry comparison.
    QString install_dir;
};

// The rule. Order matters and is the reporting order: a missing directory is a
// better answer than "version unreadable" for the same input.
[[nodiscard]] InstallContextRejection ValidateInstallContext(const InstallContextFacts& facts);

// Measure the facts for `handoff` off the real filesystem and registry, then
// apply the rule. `detail` is filled with evidence on refusal.
[[nodiscard]] InstallContextRejection ValidateInstallContextOnDisk(const UpdateHandoff& handoff, QString* detail);

// Both referenced assets exist and are readable. Kept apart from the schema
// parse because a document can be perfectly well-formed and point at files that
// have since been removed -- a different failure with a different meaning.
// Returns true when both are present; fills `detail` with the missing one
// otherwise.
[[nodiscard]] bool HandoffAssetsPresent(const UpdateHandoff& handoff, QString* detail);

} // namespace exosnap::update_handoff
