#include <gtest/gtest.h>

#include <QDir>
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

    // Default hotkey[0] = "Alt+F9" (stored as empty string until first Save).
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

    store.Save(settings);
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

    store.Save(settings);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.window_geometry.x, 100);
    EXPECT_EQ(loaded.window_geometry.y, 200);
    EXPECT_EQ(loaded.window_geometry.width, 1200);
    EXPECT_EQ(loaded.window_geometry.height, 800);
    EXPECT_TRUE(loaded.window_geometry.maximized);
}

TEST(AppSettingsStoreTest, AppSettingsStore_Save_WritesSettingsVersion14) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings;
    store.Save(settings);

    QSettings raw_settings(settings_path, QSettings::IniFormat);
    // Version bumped to 14: ACCENT-PICKER-R1 adds accent_id.
    EXPECT_EQ(raw_settings.value(QStringLiteral("settings_version")).toInt(), 14);
}

// CRASH-WIRE-R1: auto_send_crash_reports round-trip + default tests
TEST(AppSettingsStoreTest, AppSettingsStore_DefaultAutoSendCrashReportsIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.auto_send_crash_reports);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AutoSendCrashReports_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.auto_send_crash_reports = true;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.auto_send_crash_reports);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingAutoSendCrashReportsKey_DefaultsFalse) {
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
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.auto_send_crash_reports);
}

// DIAGNOSTICS-OVERLAY-R1: show_diagnostics_overlay round-trip tests
TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_DiagnosticsOverlay_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_diagnostics_overlay = false;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_diagnostics_overlay);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_DiagnosticsOverlay_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_diagnostics_overlay = true;
    store.Save(settings);

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
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.show_notifications);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ShowNotifications_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_notifications = false;
    store.Save(settings);

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
    store.Save(settings); // triggers legacy removal

    QSettings raw(settings_path, QSettings::IniFormat);
    // Legacy groups must be absent after Save().
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("output")));
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("profiles")));
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("audio")));
    EXPECT_FALSE(raw.childGroups().contains(QStringLiteral("webcam")));
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
    // Should not throw or crash.
    EXPECT_NO_THROW(store.Save(settings));
}

// TRAY-CLOSE-TO-TRAY-R1: keep_running_in_tray + tray_close_notice_shown round-trip tests

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultKeepRunningInTrayIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.keep_running_in_tray);
}

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultTrayCloseNoticeShownIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.tray_close_notice_shown);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_KeepRunningInTray_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.keep_running_in_tray = true;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.keep_running_in_tray);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_KeepRunningInTray_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.keep_running_in_tray = false;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.keep_running_in_tray);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_TrayCloseNoticeShown_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.tray_close_notice_shown = true;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.tray_close_notice_shown);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_TrayCloseNoticeShown_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.tray_close_notice_shown = false;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.tray_close_notice_shown);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingTrayKeys_DefaultToFalse) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    // Write a file without the [tray] group at all.
    {
        QSettings s(settings_path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(settings_path);
    const PersistedAppSettings loaded = store.Load();
    // Tray keys absent: must default to false.
    EXPECT_FALSE(loaded.keep_running_in_tray);
    EXPECT_FALSE(loaded.tray_close_notice_shown);
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
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.show_quick_controls);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_ShowQuickControls_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.show_quick_controls = true;
    store.Save(settings);

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

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultCheckUpdatesOnStartIsTrue) {
    PersistedAppSettings settings;
    EXPECT_TRUE(settings.check_updates_on_start);
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_UpdateChannel_Preview) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.update_channel = QStringLiteral("Preview");
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.update_channel, QStringLiteral("Preview"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_CheckUpdatesOnStart_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.check_updates_on_start = false;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.check_updates_on_start);
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingUpdateKeys_DefaultToStableAndTrue) {
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
    // Update keys absent: must default to Stable / true.
    EXPECT_EQ(loaded.update_channel, QStringLiteral("Stable"));
    EXPECT_TRUE(loaded.check_updates_on_start);
}

// ---------------------------------------------------------------------------
// ACCENT-PICKER-R1 (0.5.0-B): accent_id round-trip + default tests
// ---------------------------------------------------------------------------

TEST(AppSettingsStoreTest, AppSettingsStore_DefaultAccentIdIsMint) {
    PersistedAppSettings settings;
    EXPECT_EQ(settings.accent_id, QStringLiteral("mint"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AccentId_Azure) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.accent_id = QStringLiteral("azure");
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.accent_id, QStringLiteral("azure"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AccentId_Violet) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.accent_id = QStringLiteral("violet");
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.accent_id, QStringLiteral("violet"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_SaveAndLoad_AccentId_Mint) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(TempSettingsPath(temp_dir));
    PersistedAppSettings settings;
    settings.accent_id = QStringLiteral("mint");
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.accent_id, QStringLiteral("mint"));
}

TEST(AppSettingsStoreTest, AppSettingsStore_MissingAccentId_DefaultsToMint) {
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
    // Accent key absent: must default to "mint".
    EXPECT_EQ(loaded.accent_id, QStringLiteral("mint"));
}

} // namespace exosnap
