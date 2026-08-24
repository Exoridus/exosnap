#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <string>

#include "settings/AppSettingsStore.h"

namespace exosnap {
namespace {

QString TempSettingsPath(const QTemporaryDir& temp_dir) {
    return QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));
}

} // namespace

// ---------------------------------------------------------------------------
// PersistedAppSettings round-trips (hotkeys + window geometry only)
// ---------------------------------------------------------------------------

TEST(AppSettingsStoreTest, AppSettingsStore_LoadMissingFile_ReturnsDefaults) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    const PersistedAppSettings loaded = store.Load();

    // Every hotkey ships unset; no action has a non-empty factory default.
    EXPECT_TRUE(loaded.hotkey_bindings[0].isEmpty());
    EXPECT_TRUE(loaded.hotkey_bindings[1].isEmpty());
    EXPECT_TRUE(loaded.hotkey_bindings[2].isEmpty());
    EXPECT_TRUE(loaded.hotkey_bindings[3].isEmpty());
    EXPECT_EQ(loaded.window_geometry.x, -1);
    EXPECT_EQ(loaded.window_geometry.y, -1);
    EXPECT_EQ(loaded.window_geometry.width, -1);
    EXPECT_EQ(loaded.window_geometry.height, -1);
    EXPECT_FALSE(loaded.window_geometry.maximized);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_HotkeyBindings) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.hotkey_bindings[0] = QStringLiteral("Ctrl+Alt+F10");
    settings.hotkey_bindings[1] = QStringLiteral("Ctrl+Shift+F11");
    settings.hotkey_bindings[2] = QStringLiteral("Alt+F8");
    settings.hotkey_bindings[3] = QStringLiteral("Ctrl+Alt+M");

    ASSERT_TRUE(store.Save(settings));
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.hotkey_bindings[0], QStringLiteral("Ctrl+Alt+F10"));
    EXPECT_EQ(loaded.hotkey_bindings[1], QStringLiteral("Ctrl+Shift+F11"));
    EXPECT_EQ(loaded.hotkey_bindings[2], QStringLiteral("Alt+F8"));
    EXPECT_EQ(loaded.hotkey_bindings[3], QStringLiteral("Ctrl+Alt+M"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_WindowGeometry) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.window_geometry.x = 100;
    settings.window_geometry.y = 200;
    settings.window_geometry.width = 1200;
    settings.window_geometry.height = 800;
    settings.window_geometry.maximized = true;

    ASSERT_TRUE(store.Save(settings));
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.window_geometry.x, 100);
    EXPECT_EQ(loaded.window_geometry.y, 200);
    EXPECT_EQ(loaded.window_geometry.width, 1200);
    EXPECT_EQ(loaded.window_geometry.height, 800);
    EXPECT_TRUE(loaded.window_geometry.maximized);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_PresentDiagnosticsOptIn) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));

    // Default is OFF.
    EXPECT_FALSE(store.Load().present_diagnostics_optin);

    PersistedAppSettings settings;
    settings.present_diagnostics_optin = true;
    ASSERT_TRUE(store.Save(settings));

    EXPECT_TRUE(store.Load().present_diagnostics_optin);
}

TEST(AppSettingsStoreTest, AppSettingsStore_Save_WritesSettingsVersion) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings;
    ASSERT_TRUE(store.Save(settings));

    QSettings raw_settings(settings_path, QSettings::IniFormat);
    // Version bumped to 21: appearance_id + accent_id replace theme_id.
    EXPECT_EQ(raw_settings.value(QStringLiteral("settings_version")).toInt(), 21);
}

TEST(AppSettingsStoreTest, CrashReportPolicy_DefaultsToAskEveryTime) {
    PersistedAppSettings settings;
    EXPECT_EQ(settings.crash_report_policy, CrashReportPolicy::AskEveryTime);
}

TEST(AppSettingsStoreTest, CrashReportPolicy_RoundTripsAllThreeStates) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    for (const CrashReportPolicy policy :
         {CrashReportPolicy::AskEveryTime, CrashReportPolicy::AlwaysSend, CrashReportPolicy::NeverSend}) {
        settings.crash_report_policy = policy;
        ASSERT_TRUE(store.Save(settings));
        EXPECT_EQ(store.Load().crash_report_policy, policy);
    }
}

TEST(AppSettingsStoreTest, CrashReportPolicy_MissingLegacyKeyMigratesToAskEveryTime) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the crash group at all.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    EXPECT_EQ(store.Load().crash_report_policy, CrashReportPolicy::AskEveryTime);
}

TEST(AppSettingsStoreTest, CrashReportPolicy_LegacyFalseMigratesToAskEveryTime) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    {
        QSettings settings(settings_path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("crash/auto_send_crash_reports"), false);
    }

    AppSettingsStore store(settings_path);
    EXPECT_EQ(store.Load().crash_report_policy, CrashReportPolicy::AskEveryTime);
}

TEST(AppSettingsStoreTest, CrashReportPolicy_LegacyTrueMigratesToAlwaysSend) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    {
        QSettings settings(settings_path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("crash/auto_send_crash_reports"), true);
    }

    AppSettingsStore store(settings_path);
    EXPECT_EQ(store.Load().crash_report_policy, CrashReportPolicy::AlwaysSend);
}

TEST(AppSettingsStoreTest, CrashReportPolicy_SaveRemovesLegacyBoolean) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    {
        QSettings settings(settings_path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("crash/auto_send_crash_reports"), true);
    }

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings;
    settings.crash_report_policy = CrashReportPolicy::NeverSend;
    ASSERT_TRUE(store.Save(settings));

    QSettings raw_settings(settings_path, QSettings::IniFormat);
    EXPECT_FALSE(raw_settings.contains(QStringLiteral("crash/auto_send_crash_reports")));
    EXPECT_EQ(raw_settings.value(QStringLiteral("crash/report_policy")).toString(), QStringLiteral("never_send"));
}

TEST(CrashReportDecisionTest, SendWithoutRememberIsOneShotAndDoesNotPersist) {
    const auto decision = ResolveCrashReportDecision(CrashReportAction::SendReport, false);
    EXPECT_TRUE(decision.send_current_report);
    EXPECT_FALSE(decision.persisted_policy.has_value());
    EXPECT_EQ(decision.consent_action, CrashConsentAction::SendPendingOnce);
}

TEST(CrashReportDecisionTest, DeclineWithoutRememberDoesNotPersist) {
    const auto decision = ResolveCrashReportDecision(CrashReportAction::DontSend, false);
    EXPECT_FALSE(decision.send_current_report);
    EXPECT_FALSE(decision.persisted_policy.has_value());
    EXPECT_EQ(decision.consent_action, CrashConsentAction::ResetToAsk);
}

TEST(CrashReportDecisionTest, SendWithRememberCommitsAlwaysSend) {
    const auto decision = ResolveCrashReportDecision(CrashReportAction::SendReport, true);
    ASSERT_TRUE(decision.persisted_policy.has_value());
    EXPECT_EQ(*decision.persisted_policy, CrashReportPolicy::AlwaysSend);
    EXPECT_EQ(decision.consent_action, CrashConsentAction::GrantPersistent);
}

TEST(CrashReportDecisionTest, DeclineWithRememberCommitsNeverSend) {
    const auto decision = ResolveCrashReportDecision(CrashReportAction::DontSend, true);
    ASSERT_TRUE(decision.persisted_policy.has_value());
    EXPECT_EQ(*decision.persisted_policy, CrashReportPolicy::NeverSend);
    EXPECT_EQ(decision.consent_action, CrashConsentAction::Revoke);
}

TEST(CrashReportDecisionTest, DismissNeverSendsOrPersistsEvenWithRememberDraft) {
    const auto decision = ResolveCrashReportDecision(CrashReportAction::Dismiss, true);
    EXPECT_FALSE(decision.send_current_report);
    EXPECT_FALSE(decision.persisted_policy.has_value());
    EXPECT_EQ(decision.consent_action, CrashConsentAction::None);
}

TEST(CrashReportPromptTest, ThreeStatePolicyResolvesDistinctStartupBehavior) {
    EXPECT_EQ(ResolveCrashPromptDisposition(CrashReportPolicy::AskEveryTime), CrashPromptDisposition::ShowPrompt);
    EXPECT_EQ(ResolveCrashPromptDisposition(CrashReportPolicy::AlwaysSend), CrashPromptDisposition::SuppressAndSend);
    EXPECT_EQ(ResolveCrashPromptDisposition(CrashReportPolicy::NeverSend), CrashPromptDisposition::SuppressWithoutSend);
}

// DIAGNOSTICS-OVERLAY-R1: show_diagnostics_overlay round-trip tests
TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_DiagnosticsOverlay_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_diagnostics_overlay = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_diagnostics_overlay);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_DiagnosticsOverlay_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_diagnostics_overlay = true;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_diagnostics_overlay);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingDiagnosticsOverlayKey_DefaultsFalse) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file that has the recording overlay key but NOT the diagnostics key.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Diagnostics overlay key absent: must default to false.
    EXPECT_FALSE(loaded.show_diagnostics_overlay);
    // Recording overlay key present: must still be true.
    EXPECT_TRUE(loaded.show_recording_overlay);
}

// NOTIFY-TOASTS-R1: show_notifications round-trip tests
TEST(AppSettingsStoreTest, AppSettingsStore_DefaultShowNotificationsIsTrue) {
    PersistedAppSettings settings;
    EXPECT_TRUE(settings.show_notifications);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ShowNotifications_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_notifications = true;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_notifications);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ShowNotifications_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_notifications = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_notifications);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingShowNotifications_DefaultsToTrue) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the show_notifications key in [overlay].
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_notifications);
}

// open_editor_when_finished round-trip tests. Was a debug-only ADR-0031
// roadmap-dummy toggle (no engine setting backed it, no Release row existed);
// now a real, persisted preference: default ON (recording completion opens
// the Edit overlay directly), OFF falls back to a notification toast
// offering Edit/Show-in-folder instead.
TEST(AppSettingsStoreTest, AppSettingsStore_DefaultOpenEditorWhenFinishedIsTrue) {
    PersistedAppSettings settings;
    EXPECT_TRUE(settings.open_editor_when_finished);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_OpenEditorWhenFinished_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.open_editor_when_finished = true;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.open_editor_when_finished);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_OpenEditorWhenFinished_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.open_editor_when_finished = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.open_editor_when_finished);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingOpenEditorWhenFinished_DefaultsToTrue) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the open_editor_when_finished key in [editor].
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.open_editor_when_finished);
}

TEST(AppSettingsStoreTest, AppSettingsStore_Save_RemovesLegacyGroups) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write legacy data that old builds would have left.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("output"));
        s.setValue(QStringLiteral("container"), QStringLiteral("mkv"));
        s.endGroup();
        s.beginGroup(QStringLiteral("profiles"));
        s.setValue(QStringLiteral("active_id"), QStringLiteral("builtin.mkv_h264_aac"));
        s.endGroup();
        s.beginGroup(QStringLiteral("audio"));
        s.setValue(QStringLiteral("source_row_count"), 3);
        s.endGroup();
        s.beginGroup(QStringLiteral("webcam"));
        s.setValue(QStringLiteral("enabled"), false);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings;
    ASSERT_TRUE(store.Save(settings)); // triggers legacy removal

    QSettings raw(settings_path, QSettings::IniFormat);
    // Legacy groups must be absent after Save().
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("output")));
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("profiles")));
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("audio")));
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("webcam")));
}

// ---------------------------------------------------------------------------
// QCR-201: load_outcome distinguishes the three load cases, because only one of
// them ("a settings file exists that we could not read") makes writing the
// built-in defaults back a destructive act.
// ---------------------------------------------------------------------------

TEST(AppSettingsStoreTest, AppSettingsStore_MissingFile_IsDefaultsNoFileNotAFailure) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    // First run: no file. Defaults are legitimate and persisting them is correct.
    EXPECT_EQ(store.Load().load_outcome, SettingsLoadOutcome::DefaultsNoFile);
}

TEST(AppSettingsStoreTest, AppSettingsStore_NormalLoad_IsLoaded) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    ASSERT_TRUE(store.Save(settings));
    EXPECT_EQ(store.Load().load_outcome, SettingsLoadOutcome::Loaded);
}

TEST(AppSettingsStoreTest, AppSettingsStore_UnreadableFile_IsReadFailed) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // A directory sitting at the settings path can never be parsed as an INI
    // file — QSettings reports this via status(), which is exactly the
    // corrupt/locked-file scenario load_outcome exists to surface.
    ASSERT_TRUE(QDir().mkpath(settings_path));

    AppSettingsStore store(settings_path);
    EXPECT_EQ(store.Load().load_outcome, SettingsLoadOutcome::ReadFailed);
}

TEST(AppSettingsStoreTest, AppSettingsStore_CorruptFileContent_IsReadFailed) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    {
        QFile file(settings_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        // Binary garbage with an unterminated group header: QSettings' INI
        // parser reports FormatError rather than silently yielding no keys.
        file.write(QByteArray("[hotkeys\n\x01\x02\x00\x03 = = =\n", 26));
    }

    AppSettingsStore store(settings_path);
    EXPECT_EQ(store.Load().load_outcome, SettingsLoadOutcome::ReadFailed);
}

// The QCR-201 invariant itself, checked against the bytes on disk: a store that
// failed to load must still be the store's own file after the caller decides not
// to write. The store cannot enforce that on its own (it does not know the
// caller's intent) — what it must provide is the fact the caller needs, plus a
// way to preserve the file when the caller does eventually write.
TEST(AppSettingsStoreTest, AppSettingsStore_BackupUnreadableFile_PreservesTheOriginalBytes) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    const QByteArray original("[hotkeys\nnot-parseable", 22);
    {
        QFile file(settings_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(original);
    }

    AppSettingsStore store(settings_path);
    ASSERT_EQ(store.Load().load_outcome, SettingsLoadOutcome::ReadFailed);

    QString backup_path;
    ASSERT_TRUE(store.BackupUnreadableFile(&backup_path));
    EXPECT_EQ(backup_path, settings_path + QStringLiteral(".corrupt"));
    EXPECT_FALSE(QFileInfo::exists(settings_path)) << "the unreadable file is moved aside, not copied";

    QFile backup(backup_path);
    ASSERT_TRUE(backup.open(QIODevice::ReadOnly));
    EXPECT_EQ(backup.readAll(), original);

    // The write that follows now lands on a clean path and cannot destroy anything.
    PersistedAppSettings settings;
    settings.appearance_id = QStringLiteral("light");
    ASSERT_TRUE(store.Save(settings));
    EXPECT_EQ(store.Load().appearance_id, QStringLiteral("light"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_BackupUnreadableFile_ReplacesAnEarlierBackup) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    const QString backup_path = settings_path + QStringLiteral(".corrupt");

    {
        QFile stale(backup_path);
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
        stale.write("stale");
    }
    {
        QFile file(settings_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("fresh");
    }

    AppSettingsStore store(settings_path);
    ASSERT_TRUE(store.BackupUnreadableFile());

    QFile backup(backup_path);
    ASSERT_TRUE(backup.open(QIODevice::ReadOnly));
    EXPECT_EQ(backup.readAll(), QByteArray("fresh")) << "one unreadable file is kept, not a growing pile";
}

TEST(AppSettingsStoreTest, AppSettingsStore_BackupUnreadableFile_NoFileIsNotABackup) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    EXPECT_FALSE(store.BackupUnreadableFile());
}

// ---------------------------------------------------------------------------
// ResolveSettingsWrite: the QCR-201 decision itself. Exhaustive over the three
// load outcomes x two intents x superseded/not, because the whole point of the
// item is that one of those twelve cells used to be a silent data loss.
// ---------------------------------------------------------------------------

TEST(SettingsWritePolicyTest, MissingFileAllowsEveryWrite) {
    for (const SettingsWriteIntent intent : {SettingsWriteIntent::Incidental, SettingsWriteIntent::UserEdit}) {
        EXPECT_EQ(ResolveSettingsWrite(SettingsLoadOutcome::DefaultsNoFile, /*superseded=*/false, intent),
                  SettingsWriteDecision::Write);
    }
}

TEST(SettingsWritePolicyTest, SuccessfulLoadAllowsEveryWrite) {
    for (const SettingsWriteIntent intent : {SettingsWriteIntent::Incidental, SettingsWriteIntent::UserEdit}) {
        EXPECT_EQ(ResolveSettingsWrite(SettingsLoadOutcome::Loaded, /*superseded=*/false, intent),
                  SettingsWriteDecision::Write);
    }
}

// The regression this item exists for: window geometry on move/close, a startup
// reconciliation, a one-time tray flag — none of them may replace a settings
// file the app could not read.
TEST(SettingsWritePolicyTest, ReadFailureRefusesIncidentalWrites) {
    EXPECT_EQ(
        ResolveSettingsWrite(SettingsLoadOutcome::ReadFailed, /*superseded=*/false, SettingsWriteIntent::Incidental),
        SettingsWriteDecision::Refuse);
}

TEST(SettingsWritePolicyTest, ReadFailureLetsAUserEditThroughButPreservesTheFileFirst) {
    EXPECT_EQ(
        ResolveSettingsWrite(SettingsLoadOutcome::ReadFailed, /*superseded=*/false, SettingsWriteIntent::UserEdit),
        SettingsWriteDecision::PreserveThenWrite);
}

TEST(SettingsWritePolicyTest, OnceSupersededTheFileIsOrdinaryAgain) {
    // The unreadable file has already been moved aside and rewritten, so there
    // is nothing left to protect — including for the incidental writes that
    // were refused a moment earlier.
    for (const SettingsWriteIntent intent : {SettingsWriteIntent::Incidental, SettingsWriteIntent::UserEdit}) {
        EXPECT_EQ(ResolveSettingsWrite(SettingsLoadOutcome::ReadFailed, /*superseded=*/true, intent),
                  SettingsWriteDecision::Write);
    }
}

// QCR-201, second half: Save() reports its own failure instead of returning void
// and letting a lost change look like a successful write.
TEST(AppSettingsStoreTest, AppSettingsStore_SaveToAnUnwritablePath_ReportsFailure) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    // A directory occupying the settings path: mkpath of the parent succeeds,
    // the INI write cannot.
    const QString settings_path = TempSettingsPath(temp_dir);
    ASSERT_TRUE(QDir().mkpath(settings_path));

    AppSettingsStore store(settings_path);
    const PersistedAppSettings settings;
    EXPECT_FALSE(store.Save(settings));
}

TEST(AppSettingsStoreTest, AppSettingsStore_EmptyPath_LoadReturnsDefaults) {
    const QString empty_path;
    AppSettingsStore store{empty_path};
    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.hotkey_bindings[1].isEmpty());
    EXPECT_EQ(loaded.window_geometry.x, -1);
}

TEST(AppSettingsStoreTest, AppSettingsStore_EmptyPath_SaveIsNoOp) {
    const QString empty_path;
    AppSettingsStore store{empty_path};
    PersistedAppSettings settings;
    settings.hotkey_bindings[0] = QStringLiteral("Alt+F9");
    // Nothing to write to, and the caller is told so rather than left believing
    // the settings were persisted.
    EXPECT_FALSE(store.Save(settings));
}

// Window presence: minimize_to_tray + hide_window_from_capture round-trip tests

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultMinimizeToTrayIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.minimize_to_tray);
}

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultHideWindowFromCaptureIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.hide_window_from_capture);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_MinimizeToTray_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.minimize_to_tray = true;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.minimize_to_tray);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_MinimizeToTray_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.minimize_to_tray = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.minimize_to_tray);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_HideWindowFromCapture_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.hide_window_from_capture = true;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.hide_window_from_capture);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_HideWindowFromCapture_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.hide_window_from_capture = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.hide_window_from_capture);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingWindowKeys_DefaultToFalse) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the [window] group at all.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Both settings hide the window in their own way, so an absent key must
    // never read as ON.
    EXPECT_FALSE(loaded.minimize_to_tray);
    EXPECT_FALSE(loaded.hide_window_from_capture);
}

// QUICK-PILL-R1: show_quick_controls round-trip tests

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultShowQuickControlsIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.show_quick_controls);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ShowQuickControls_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_quick_controls = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_quick_controls);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ShowQuickControls_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_quick_controls = true;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_quick_controls);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingShowQuickControls_DefaultsFalse) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the [presence] group.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Quick-controls key absent: must default to false.
    EXPECT_FALSE(loaded.show_quick_controls);
}

// UPDATE-WIRE-R1: update_channel + check_updates_on_start round-trip tests

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultUpdateChannelIsStable) {
    PersistedAppSettings settings;
    EXPECT_EQ(settings.update_channel, QStringLiteral("Stable"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultCheckUpdatesOnStartIsFalse) {
    // ADR 0045: the update check must be opt-in — a first launch must not
    // contact api.github.com before the user has explicitly turned this on.
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.check_updates_on_start);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_UpdateChannel_Preview) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.update_channel = QStringLiteral("Preview");
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.update_channel, QStringLiteral("Preview"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_CheckUpdatesOnStart_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.check_updates_on_start = false;
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.check_updates_on_start);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingUpdateKeys_DefaultToStableAndFalse) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the [update] group.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Update keys absent: must default to Stable / false (ADR 0045 — opt-in).
    EXPECT_EQ(loaded.update_channel, QStringLiteral("Stable"));
    EXPECT_FALSE(loaded.check_updates_on_start);
}

// ---------------------------------------------------------------------------
// Appearance + accent: round-trip, defaults, and the legacy theme_id migration
// ---------------------------------------------------------------------------

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultAppearanceIsDarkAqua) {
    PersistedAppSettings settings;
    EXPECT_EQ(settings.appearance_id, QStringLiteral("dark"));
    EXPECT_EQ(settings.accent_id, QStringLiteral("aqua"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AppearanceAndAccent) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.appearance_id = QStringLiteral("light");
    settings.accent_id = QStringLiteral("violet");
    ASSERT_TRUE(store.Save(settings));

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.appearance_id, QStringLiteral("light"));
    EXPECT_EQ(loaded.accent_id, QStringLiteral("violet"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingAppearanceGroup_DefaultsToDarkAqua) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the [appearance] group.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.appearance_id, QStringLiteral("dark"));
    EXPECT_EQ(loaded.accent_id, QStringLiteral("aqua"));
}

// A store written by a pre-0.9 build carries only `theme_id`. Reading it must
// produce the closest pair, not the default — otherwise every existing install
// silently loses its colour on the first launch after the update.
TEST(AppSettingsStoreTest, AppSettingsStore_LegacyThemeIdMigratesToAppearanceAndAccent) {
    struct Row {
        const char* legacy;
        const char* appearance;
        const char* accent;
    };
    for (const Row& row : {Row{"dark-default", "dark", "aqua"}, Row{"dark-indigo", "dark", "violet"},
                           Row{"light-paper", "light", "sky"}, Row{"light-slate", "light", "violet"}}) {
        QTemporaryDir temp_dir;
        ASSERT_TRUE(temp_dir.isValid());
        const QString settings_path = TempSettingsPath(temp_dir);
        {
            QSettings s(settings_path, QSettings::IniFormat);
            s.beginGroup(QStringLiteral("appearance"));
            s.setValue(QStringLiteral("theme_id"), QString::fromUtf8(row.legacy));
            s.endGroup();
            s.sync();
        }

        AppSettingsStore store(settings_path);
        const PersistedAppSettings loaded = store.Load();
        EXPECT_EQ(loaded.appearance_id, QString::fromUtf8(row.appearance)) << row.legacy;
        EXPECT_EQ(loaded.accent_id, QString::fromUtf8(row.accent)) << row.legacy;
    }
}

TEST(AppSettingsStoreTest, AppSettingsStore_UnknownLegacyThemeIdMigratesToTheDefault) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("appearance"));
        s.setValue(QStringLiteral("theme_id"), QStringLiteral("dark-teal-that-never-shipped"));
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Never blank, never unstyled: an unreadable preference resolves to the
    // shipped default rather than to an empty id.
    EXPECT_EQ(loaded.appearance_id, QStringLiteral("dark"));
    EXPECT_EQ(loaded.accent_id, QStringLiteral("aqua"));
}

// The new keys win once they exist. An older build run against the same store
// would rewrite `theme_id`; letting that override a choice made since would
// undo the user's selection every time they switched builds.
TEST(AppSettingsStoreTest, AppSettingsStore_StoredAppearanceOutranksALingeringThemeId) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("appearance"));
        s.setValue(QStringLiteral("appearance_id"), QStringLiteral("light"));
        s.setValue(QStringLiteral("accent_id"), QStringLiteral("magenta"));
        s.setValue(QStringLiteral("theme_id"), QStringLiteral("dark-indigo"));
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.appearance_id, QStringLiteral("light"));
    EXPECT_EQ(loaded.accent_id, QStringLiteral("magenta"));
}

// Saving drops the legacy key, so the migration path above can never fire again
// against a value the user has since replaced.
TEST(AppSettingsStoreTest, AppSettingsStore_SaveRemovesTheLegacyThemeIdKey) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("appearance"));
        s.setValue(QStringLiteral("theme_id"), QStringLiteral("light-slate"));
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings = store.Load();
    ASSERT_EQ(settings.accent_id, QStringLiteral("violet"));
    ASSERT_TRUE(store.Save(settings));

    QSettings s(settings_path, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("appearance"));
    EXPECT_FALSE(s.contains(QStringLiteral("theme_id")));
    s.endGroup();
}

} // namespace exosnap
