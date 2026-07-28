#pragma once

// UpdaterArgs.h -- command-line arguments for the exosnap-updater process.
//
// Parsed from QCoreApplication::arguments(). No QtWidgets: this seam is
// unit-tested headless. QString/QStringList (Qt Core) only.

#include <QString>
#include <QStringList>
#include <optional>
#include <update/update_types.h>

struct UpdaterArgs {
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    exosnap::update::InstallMode install_mode = exosnap::update::InstallMode::Portable;
    QString install_dir;     // required for portable; from --install-dir
    quint32 app_pid = 0;     // --app-pid; 0 = app not running
    QString current_version; // --current-version "0.8.1" (left pill + downgrade guard)
    QString base_url;        // --base-url dev override ("" in official builds)
    QString preview_state;   // --preview-state <progress|amber|red|green> (dev only)
    // ADR 0055 -- verification reinstall: the app was started with
    // --verify-update-reinstall and is asking for the IDENTICAL version to be
    // reinstalled through the full production path. Adds a hard gate (manifest
    // version string must equal --current-version exactly); relaxes nothing.
    bool verify_reinstall = false; // --verify-reinstall (boolean flag, no value)
};

// Parses updater arguments. Returns nullopt and writes a single error line to
// stderr when a required argument is missing or an argument value is malformed.
[[nodiscard]] std::optional<UpdaterArgs> ParseUpdaterArgs(const QStringList& argv);
