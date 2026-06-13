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

TEST(AppSettingsStoreTest, AppSettingsStore_Save_WritesSettingsVersion8) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString settings_path = TempSettingsPath(temp_dir);

    AppSettingsStore store(settings_path);
    PersistedAppSettings settings;
    store.Save(settings);

    QSettings raw_settings(settings_path, QSettings::IniFormat);
    // Version bumped to 8 in DIAGNOSTICS-OVERLAY-R1 (diagnostics overlay toggle added).
    EXPECT_EQ(raw_settings.value(QStringLiteral("settings_version")).toInt(), 8);
}

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

} // namespace exosnap
