#pragma once

// UpdaterArgs.h -- how the exosnap-updater process learns what it is doing.
//
// TWO product modes, and the command line decides which one:
//
//   * App handoff -- `--apply-handoff <path>`. Everything about the operation
//     (the pinned release, the manifest bytes that prove it, the installation,
//     the parent pid, the transaction it belongs to) comes from ONE versioned
//     document, not from a dozen search arguments. The document is untrusted
//     input; see update_handoff/handoff.h.
//   * Manual -- no handoff. Someone started the executable themselves. It works
//     out its own context and resolves the channel itself, because nobody told
//     it anything.
//
// The search arguments this file used to parse (--install-dir, --app-pid,
// --current-version, --target-version, --verify-reinstall, --install-mode) are
// gone: they were a second, unversioned spelling of the handoff, and keeping
// both would have left two production paths into the same swap.
//
// No QtWidgets: this seam is unit-tested headless. QString/QStringList (Qt Core)
// only.

#include <QString>
#include <QStringList>
#include <optional>
#include <update/update_flow_state.h>
#include <update/update_types.h>
#include <update_handoff/handoff.h>

// What argv alone can say. Deliberately separate from UpdaterArgs: parsing the
// command line touches no file, so it stays pure and the IO failure modes of the
// handoff document have their own vocabulary.
struct UpdaterCommandLine {
    // Non-empty exactly when --apply-handoff was given; that presence IS the
    // mode, because a handoff is something only a launcher can hand over.
    QString handoff_path;
    // Manual-mode configuration. A person may reasonably pass these by hand, so
    // they deliberately do NOT arm a pipeline.
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    QString base_url;      // --base-url dev feed override ("" in official builds)
    QString preview_state; // --preview-state <download|progress|amber|red|green|reboot> (dev only)
};

// The resolved run context: argv plus, in handoff mode, the document's contents.
struct UpdaterArgs {
    exosnap::update::UpdaterMode mode = exosnap::update::UpdaterMode::Manual;
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    exosnap::update::InstallMode install_mode = exosnap::update::InstallMode::Portable;
    QString install_dir;
    quint32 app_pid = 0;     // 0 = no parent to wait for (manual mode)
    QString current_version; // the version running in install_dir
    // The EXACT release the app offered the user. In handoff mode this run
    // installs that version or nothing at all -- the signed manifest's version
    // string must match it byte-for-byte. Empty in manual mode, which resolves
    // the channel itself.
    QString target_version;
    // Correlation identity for the whole operation, minted by the application.
    // Empty in manual mode: a run nobody handed off is not part of anyone's
    // transaction.
    QString update_transaction_id;
    // The release trust anchor, as handed over: the exact manifest bytes and
    // their detached signature, already on disk. The updater re-verifies them
    // itself -- being handed a file is not being handed trust. Empty in manual
    // mode, which downloads its own.
    QString manifest_path;
    QString manifest_signature_path;
    QString base_url;      // manual-mode dev feed override
    QString preview_state; // dev-only render short-circuit
    // ADR 0055 -- verification reinstall: the app asked for the IDENTICAL
    // version to be reinstalled through the full production path. Adds a hard
    // gate; relaxes nothing.
    bool verify_reinstall = false;
};

// The canonical --preview-state values, in canon order. ONE list: the parser
// validates against it and main.cpp builds both its error message and its
// dispatch from it, so the two cannot drift the way they had (main knew
// "download" and "reboot"; the parser did not, and the parser's copy was dead
// code for the exe because the preview short-circuit runs before it).
[[nodiscard]] const QStringList& PreviewStateNames();
[[nodiscard]] bool IsKnownPreviewState(const QString& value);

// What the manual mode has to work out for itself, because no launcher told it.
struct ManualContext {
    exosnap::update::InstallMode install_mode = exosnap::update::InstallMode::Portable;
    QString install_dir;
};

// Pure derivation of the manual-mode context from what the machine reports.
// `registry_install_path` is ReadInstallPath()'s answer ("" when unset),
// `exe_dir` this process's own directory. Installed mode trusts the registry
// path and only falls back to `exe_dir` when the registry says nothing;
// portable mode uses `exe_dir`, which is the install directory exactly because
// a manual start is not a staged handoff copy.
[[nodiscard]] ManualContext ResolveManualContext(exosnap::update::InstallMode detected,
                                                 const QString& registry_install_path, const QString& exe_dir);

// Whether this run may contact the update feed at all. The official-build gate
// is an update-check POLICY (update_checker.h) and it applies to whoever does
// the checking; --base-url is the documented dev override and the only way a
// non-official build is allowed to look. ONE rule: the worker's check path and
// the automation channel's precondition both read this, so a client can never be
// told a check is available and then be refused by the engine.
[[nodiscard]] bool UpdateChecksEnabled(const UpdaterArgs& args);

// The version that is actually installed: the VERSIONINFO ProductVersion string
// of <install_dir>\exosnap.exe, empty when it cannot be read. Deliberately NOT
// this updater's own build version -- after a half-finished update the two
// differ, and the one the user cares about is the one on disk.
[[nodiscard]] QString ReadInstalledVersion(const QString& install_dir);

// Parses the command line. Returns nullopt and writes a single error line to
// stderr when an argument value is missing or malformed. Pure: it opens nothing.
[[nodiscard]] std::optional<UpdaterCommandLine> ParseUpdaterCommandLine(const QStringList& argv);

// The run context for a validated handoff document. Pure projection -- every
// field comes from the document or from argv, nothing is derived or guessed.
[[nodiscard]] UpdaterArgs ArgsFromHandoff(const exosnap::update_handoff::UpdateHandoff& handoff,
                                          const UpdaterCommandLine& command_line);

// The run context for a manual start, before FillManualContext measures the
// machine. Kept as a function so "manual mode carries no target, no transaction
// and no handed-over manifest" is stated once.
[[nodiscard]] UpdaterArgs ArgsForManualStart(const UpdaterCommandLine& command_line);
