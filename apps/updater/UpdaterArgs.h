#pragma once

// UpdaterArgs.h -- command-line arguments for the exosnap-updater process.
//
// Parsed from QCoreApplication::arguments(). No QtWidgets: this seam is
// unit-tested headless. QString/QStringList (Qt Core) only.

#include <QString>
#include <QStringList>
#include <optional>
#include <update/update_flow_state.h>
#include <update/update_types.h>

struct UpdaterArgs {
    // Manual when the command line carries no handoff context at all (a
    // double-click, or a user starting the updater to recover a broken install);
    // LegacyHandoff when ExoSnap launched it with the context arguments below.
    // NOT derivable after the fact: the staged handoff copy deliberately does not
    // live in the install directory, so "where is my exe" cannot answer it.
    exosnap::update::UpdaterMode mode = exosnap::update::UpdaterMode::Manual;
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    exosnap::update::InstallMode install_mode = exosnap::update::InstallMode::Portable;
    QString install_dir;     // required for portable in handoff mode; from --install-dir
    quint32 app_pid = 0;     // --app-pid; 0 = app not running
    QString current_version; // --current-version "0.8.1" (left pill + downgrade guard)
    // --target-version "0.9.0-rc5": the EXACT release the app offered the user.
    // When set, the updater installs that version or nothing at all -- the
    // manifest's version string must match it byte-for-byte, the same equality
    // the verification reinstall gate uses. Empty means "resolve the channel
    // yourself", which is the manual mode's normal operation.
    QString target_version;
    QString base_url;      // --base-url dev override ("" in official builds)
    QString preview_state; // --preview-state <download|progress|amber|red|green|reboot> (dev only)
    // ADR 0055 -- verification reinstall: the app was started with
    // --verify-update-reinstall and is asking for the IDENTICAL version to be
    // reinstalled through the full production path. Adds a hard gate (manifest
    // version string must equal --current-version exactly); relaxes nothing.
    bool verify_reinstall = false; // --verify-reinstall (boolean flag, no value)
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

// Parses updater arguments. Returns nullopt and writes a single error line to
// stderr when a required argument is missing or an argument value is malformed.
[[nodiscard]] std::optional<UpdaterArgs> ParseUpdaterArgs(const QStringList& argv);
