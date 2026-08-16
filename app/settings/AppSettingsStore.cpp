#include "AppSettingsStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "settings/ConfigPaths.h"
#include "ui/theme/ExoSnapThemes.h"

#include <string>

namespace exosnap {
namespace {

// Bump to 21: the single `theme_id` is replaced by an independent
// `appearance_id` + `accent_id` pair, migrated on load.
constexpr int kSettingsVersionCurrent = 21;

} // namespace

AppSettingsStore::AppSettingsStore() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        settings_path_ = QDir(config_dir).filePath(QStringLiteral("settings.ini"));
    } else {
        settings_path_ = QStringLiteral("settings.ini");
    }
}

AppSettingsStore::AppSettingsStore(QString settings_file_path) : settings_path_(std::move(settings_file_path)) {
}

PersistedAppSettings AppSettingsStore::Load() const {
    PersistedAppSettings persisted;

    if (settings_path_.isEmpty()) {
        return persisted;
    }

    // Captured before the read: QSettings::status() cannot tell "no file" from
    // "file read cleanly" — both are NoError — and the difference decides
    // whether writing the defaults back is legitimate or destructive.
    const bool file_existed = QFileInfo::exists(settings_path_);

    QSettings settings(settings_path_, QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("hotkeys"));
    for (int i = 0; i < static_cast<int>(persisted.hotkey_bindings.size()); ++i) {
        const QString key = QStringLiteral("binding_%1").arg(i);
        if (settings.contains(key)) {
            persisted.hotkey_bindings[static_cast<std::size_t>(i)] = settings.value(key).toString().trimmed();
        }
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("window"));
    persisted.window_geometry.x = settings.value(QStringLiteral("x"), -1).toInt();
    persisted.window_geometry.y = settings.value(QStringLiteral("y"), -1).toInt();
    persisted.window_geometry.width = settings.value(QStringLiteral("width"), -1).toInt();
    persisted.window_geometry.height = settings.value(QStringLiteral("height"), -1).toInt();
    persisted.window_geometry.maximized = settings.value(QStringLiteral("maximized"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("overlay"));
    persisted.show_recording_overlay = settings.value(QStringLiteral("show_recording_overlay"), true).toBool();
    // DIAGNOSTICS-OVERLAY-R1: diagnostics overlay toggle (default OFF).
    // Pre-1.0: no migration; missing key defaults to false.
    persisted.show_diagnostics_overlay = settings.value(QStringLiteral("show_diagnostics_overlay"), false).toBool();
    // NOTIFY-TOASTS-R1: notification toasts toggle (default ON).
    // Pre-1.0: no migration; missing key defaults to true.
    persisted.show_notifications = settings.value(QStringLiteral("show_notifications"), true).toBool();
    // Overlay content. Read as raw strings: validation belongs to
    // models/OverlayContentPolicy, which resolves an unknown token to the
    // shipped default rather than rejecting the file.
    persisted.recording_overlay_preset =
        settings.value(QStringLiteral("recording_overlay_preset"), QStringLiteral("minimal")).toString();
    persisted.recording_overlay_custom_elements =
        settings.value(QStringLiteral("recording_overlay_custom_elements"), QStringLiteral("elapsed")).toString();
    persisted.diagnostics_overlay_preset =
        settings.value(QStringLiteral("diagnostics_overlay_preset"), QStringLiteral("health")).toString();
    persisted.diagnostics_overlay_custom_elements =
        settings.value(QStringLiteral("diagnostics_overlay_custom_elements"), QStringLiteral("drop,drift,muted"))
            .toString();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("editor"));
    // "Open editor when finished" toggle (default ON).
    // Pre-1.0: no migration; missing key defaults to true.
    persisted.open_editor_when_finished = settings.value(QStringLiteral("open_editor_when_finished"), true).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("tray"));
    // TRAY-CLOSE-TO-TRAY-R1: close-to-tray opt-in (default OFF).
    persisted.keep_running_in_tray = settings.value(QStringLiteral("keep_running_in_tray"), false).toBool();
    // TRAY-CLOSE-TO-TRAY-R1: one-time close notice shown flag (default false).
    persisted.tray_close_notice_shown = settings.value(QStringLiteral("tray_close_notice_shown"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("presence"));
    // QUICK-PILL-R1: interactive quick-control pill toggle (default OFF).
    // Pre-1.0: no migration; missing key defaults to false.
    persisted.show_quick_controls = settings.value(QStringLiteral("show_quick_controls"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("crash"));
    // CRASH-POLICY-R2: migrate the old Boolean without turning a historical
    // false/missing value into a permanent refusal.
    const QString persisted_policy = settings.value(QStringLiteral("report_policy")).toString();
    if (persisted_policy == QStringLiteral("always_send")) {
        persisted.crash_report_policy = CrashReportPolicy::AlwaysSend;
    } else if (persisted_policy == QStringLiteral("never_send")) {
        persisted.crash_report_policy = CrashReportPolicy::NeverSend;
    } else if (persisted_policy == QStringLiteral("ask_every_time")) {
        persisted.crash_report_policy = CrashReportPolicy::AskEveryTime;
    } else if (settings.value(QStringLiteral("auto_send_crash_reports"), false).toBool()) {
        persisted.crash_report_policy = CrashReportPolicy::AlwaysSend;
    } else {
        persisted.crash_report_policy = CrashReportPolicy::AskEveryTime;
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("update"));
    // UPDATE-WIRE-R1: update channel (default "Stable") + auto-check-on-start.
    // ADR 0045: auto-check-on-start defaults to false (opt-in) so a first launch
    // never contacts api.github.com without explicit consent.
    // Pre-1.0: no migration; missing keys default to Stable / false.
    persisted.update_channel = settings.value(QStringLiteral("channel"), QStringLiteral("Stable")).toString();
    persisted.check_updates_on_start = settings.value(QStringLiteral("check_updates_on_start"), false).toBool();
    // Loop guard for the staged swap updater; empty when no swap is pending.
    persisted.applied_version = settings.value(QStringLiteral("applied_version"), QString()).toString();
    // WHATS-NEW: suppress the post-update overlay (default false = notices shown).
    persisted.whats_new_suppressed = settings.value(QStringLiteral("whats_new_suppressed"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("appearance"));
    // `appearance_id` + `accent_id` replace the single `theme_id`. A store
    // written by an older build carries only `theme_id`, so it is migrated to
    // the closest pair rather than dropped — otherwise every existing install
    // would silently snap back to Dark + Aqua on first launch after the update.
    //
    // The new keys win when present: once migrated, a later theme_id left in the
    // file (an older build run against the same store) must not overwrite a
    // choice the user has since made.
    const QString stored_appearance = settings.value(QStringLiteral("appearance_id"), QString()).toString();
    const QString stored_accent = settings.value(QStringLiteral("accent_id"), QString()).toString();
    if (stored_appearance.isEmpty() || stored_accent.isEmpty()) {
        const std::string legacy_theme_id =
            settings.value(QStringLiteral("theme_id"), QString()).toString().toStdString();
        persisted.appearance_id = stored_appearance.isEmpty()
                                      ? QString::fromUtf8(ui::theme::MigratedAppearanceId(legacy_theme_id))
                                      : stored_appearance;
        persisted.accent_id =
            stored_accent.isEmpty() ? QString::fromUtf8(ui::theme::MigratedAccentId(legacy_theme_id)) : stored_accent;
    } else {
        persisted.appearance_id = stored_appearance;
        persisted.accent_id = stored_accent;
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("settings_tiers"));
    // SETTINGS-TIERS-R1: expert mode toggle (default OFF).
    persisted.expert_mode_enabled = settings.value(QStringLiteral("expert_mode_enabled"), false).toBool();
    // SETTINGS-TIERS-R1: per-card expander expanded state (default collapsed).
    persisted.audio_separate_expander_expanded =
        settings.value(QStringLiteral("audio_separate_expander_expanded"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("diagnostics"));
    // ELEVATION-FOUNDATION-R1 (ADR 0033): present-diagnostics opt-in (default OFF).
    // Pre-1.0: no migration; missing key defaults to false.
    persisted.present_diagnostics_optin = settings.value(QStringLiteral("present_diagnostics_optin"), false).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("developer"));
    // SETTINGS-HONESTY-R1: developer log-level filter (default "Debug" = record
    // everything, review F1). Pre-1.0: no migration; missing key defaults to "Debug".
    persisted.developer_log_level = settings.value(QStringLiteral("log_level"), QStringLiteral("Debug")).toString();
    settings.endGroup();

    // A corrupt or otherwise unreadable settings.ini does not throw — QSettings
    // silently falls back to defaults for every key above. Surface that via the
    // status check instead of pretending the load was faithful.
    if (settings.status() != QSettings::NoError) {
        persisted.load_outcome = SettingsLoadOutcome::ReadFailed;
    } else {
        persisted.load_outcome = file_existed ? SettingsLoadOutcome::Loaded : SettingsLoadOutcome::DefaultsNoFile;
    }

    return persisted;
}

bool AppSettingsStore::BackupUnreadableFile(QString* out_backup_path) const {
    if (settings_path_.isEmpty() || !QFileInfo::exists(settings_path_)) {
        return false;
    }
    const QString backup_path = settings_path_ + QStringLiteral(".corrupt");
    // QFile::rename refuses an existing target on Windows, and an older backup
    // is worth less than the file that just failed to load, so it is replaced.
    if (QFileInfo::exists(backup_path) && !QFile::remove(backup_path)) {
        return false;
    }
    if (!QFile::rename(settings_path_, backup_path)) {
        return false;
    }
    if (out_backup_path != nullptr) {
        *out_backup_path = backup_path;
    }
    return true;
}

bool AppSettingsStore::Save(const PersistedAppSettings& settings_snapshot) const {
    if (settings_path_.isEmpty()) {
        return false;
    }

    const QFileInfo info(settings_path_);
    QDir().mkpath(info.absolutePath());

    QSettings settings(settings_path_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), kSettingsVersionCurrent);

    // Remove legacy groups from old builds so files are fully canonical after
    // one Save() call.  These keys are now owned by RecordingPresetStore.
    settings.remove(QStringLiteral("output"));
    settings.remove(QStringLiteral("video"));
    settings.remove(QStringLiteral("audio"));
    settings.remove(QStringLiteral("webcam"));
    settings.remove(QStringLiteral("profiles"));

    settings.beginGroup(QStringLiteral("hotkeys"));
    for (int i = 0; i < static_cast<int>(settings_snapshot.hotkey_bindings.size()); ++i) {
        settings.setValue(QStringLiteral("binding_%1").arg(i),
                          settings_snapshot.hotkey_bindings[static_cast<std::size_t>(i)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("window"));
    settings.setValue(QStringLiteral("x"), settings_snapshot.window_geometry.x);
    settings.setValue(QStringLiteral("y"), settings_snapshot.window_geometry.y);
    settings.setValue(QStringLiteral("width"), settings_snapshot.window_geometry.width);
    settings.setValue(QStringLiteral("height"), settings_snapshot.window_geometry.height);
    settings.setValue(QStringLiteral("maximized"), settings_snapshot.window_geometry.maximized);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("overlay"));
    settings.setValue(QStringLiteral("show_recording_overlay"), settings_snapshot.show_recording_overlay);
    // DIAGNOSTICS-OVERLAY-R1: diagnostics overlay toggle.
    settings.setValue(QStringLiteral("show_diagnostics_overlay"), settings_snapshot.show_diagnostics_overlay);
    // NOTIFY-TOASTS-R1: notification toasts toggle.
    settings.setValue(QStringLiteral("show_notifications"), settings_snapshot.show_notifications);
    settings.setValue(QStringLiteral("recording_overlay_preset"), settings_snapshot.recording_overlay_preset);
    settings.setValue(QStringLiteral("recording_overlay_custom_elements"),
                      settings_snapshot.recording_overlay_custom_elements);
    settings.setValue(QStringLiteral("diagnostics_overlay_preset"), settings_snapshot.diagnostics_overlay_preset);
    settings.setValue(QStringLiteral("diagnostics_overlay_custom_elements"),
                      settings_snapshot.diagnostics_overlay_custom_elements);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("editor"));
    settings.setValue(QStringLiteral("open_editor_when_finished"), settings_snapshot.open_editor_when_finished);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("tray"));
    // TRAY-CLOSE-TO-TRAY-R1: close-to-tray opt-in.
    settings.setValue(QStringLiteral("keep_running_in_tray"), settings_snapshot.keep_running_in_tray);
    // TRAY-CLOSE-TO-TRAY-R1: one-time close notice shown flag.
    settings.setValue(QStringLiteral("tray_close_notice_shown"), settings_snapshot.tray_close_notice_shown);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("presence"));
    // QUICK-PILL-R1: interactive quick-control pill toggle.
    settings.setValue(QStringLiteral("show_quick_controls"), settings_snapshot.show_quick_controls);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("crash"));
    QString crash_policy = QStringLiteral("ask_every_time");
    switch (settings_snapshot.crash_report_policy) {
    case CrashReportPolicy::AskEveryTime:
        break;
    case CrashReportPolicy::AlwaysSend:
        crash_policy = QStringLiteral("always_send");
        break;
    case CrashReportPolicy::NeverSend:
        crash_policy = QStringLiteral("never_send");
        break;
    }
    settings.setValue(QStringLiteral("report_policy"), crash_policy);
    settings.remove(QStringLiteral("auto_send_crash_reports"));
    settings.endGroup();

    settings.beginGroup(QStringLiteral("update"));
    // UPDATE-WIRE-R1: update channel + auto-check-on-start.
    settings.setValue(QStringLiteral("channel"), settings_snapshot.update_channel);
    settings.setValue(QStringLiteral("check_updates_on_start"), settings_snapshot.check_updates_on_start);
    settings.setValue(QStringLiteral("applied_version"), settings_snapshot.applied_version);
    // WHATS-NEW: suppress the post-update overlay.
    settings.setValue(QStringLiteral("whats_new_suppressed"), settings_snapshot.whats_new_suppressed);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("appearance"));
    settings.setValue(QStringLiteral("appearance_id"), settings_snapshot.appearance_id);
    settings.setValue(QStringLiteral("accent_id"), settings_snapshot.accent_id);
    // Dropped rather than left behind: a stale `theme_id` would be picked up
    // again by the migration path above if the new keys were ever cleared, and
    // it would then override a choice made since.
    settings.remove(QStringLiteral("theme_id"));
    settings.endGroup();

    settings.beginGroup(QStringLiteral("settings_tiers"));
    // SETTINGS-TIERS-R1: expert mode toggle.
    settings.setValue(QStringLiteral("expert_mode_enabled"), settings_snapshot.expert_mode_enabled);
    // SETTINGS-TIERS-R1: per-card expander expanded state.
    settings.setValue(QStringLiteral("audio_separate_expander_expanded"),
                      settings_snapshot.audio_separate_expander_expanded);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("diagnostics"));
    // ELEVATION-FOUNDATION-R1 (ADR 0033): present-diagnostics opt-in.
    settings.setValue(QStringLiteral("present_diagnostics_optin"), settings_snapshot.present_diagnostics_optin);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("developer"));
    // SETTINGS-HONESTY-R1: developer log-level filter.
    settings.setValue(QStringLiteral("log_level"), settings_snapshot.developer_log_level);
    settings.endGroup();

    // sync() is where the buffered keys actually reach the file; the status it
    // leaves behind is the only report of a full disk, a locked file or a
    // read-only directory. Judging the write before this point (or not at all)
    // is how a lost change used to look like a successful save.
    settings.sync();
    return settings.status() == QSettings::NoError;
}

const QString& AppSettingsStore::SettingsFilePath() const {
    return settings_path_;
}

} // namespace exosnap
