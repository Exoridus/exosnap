#pragma once
#include <QKeySequence>
#include <QMainWindow>
#include <QRect>
#include <QStackedWidget>
#include <QString>
#include <array>
#include <cstdint>
#include <memory>

#include "models/OutputSettingsModel.h"
#include "models/RecordingPreset.h"
#include "models/RecordingPresetRegistry.h"
#include "models/VideoSettingsModel.h"
#include "notifications/NotificationManager.h"
#include "services/AudioDeviceNotifier.h"
#include "services/DisplayDeviceNotifier.h"
#include "services/GlobalHotkeyService.h"
#include "services/RecoveryService.h"
#include "services/WebcamDeviceNotifier.h"
#include "settings/AppSettingsStore.h"
#include "settings/RecordingPresetStore.h"
#include "settings/RecoveryManifestStore.h"
#include "ui/tray/TrayPresence.h"
#include <capability/audio_ui_state.h>
#include <capability/capability_set.h>
#include <crash_capture/crash_capture.h>

#include <optional>
#include <string>

class QShowEvent;

namespace exosnap {

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace visual {
struct VisualScenario;
}
#endif

namespace ui::chrome {
class OperationalTitleBar;
} // namespace ui::chrome

namespace ui::tray {
class TrayPresence;
} // namespace ui::tray

namespace ui::dialogs {
class AboutOverlay;
class CrashReportOverlay;
class RecoveryOverlay;
class SourcePickerOverlay;
} // namespace ui::dialogs

namespace ui::overlay {
class CountdownOverlayWindow;
class DiagnosticsOverlayWindow;
class NotificationToastWindow;
class QuickControlPillWindow;
class RecordingOverlayWindow;
} // namespace ui::overlay

class AdvancedPage;
class ConfigPage;
class DiagnosticsPage;
class HotkeysPage;
class LogsPage;
class RecordPage;
class UpdateService;
class WebcamPage;

namespace update {
struct UpdateCheckResult;
} // namespace update

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // CRASH-WIRE-R1: true when the crash dialog's "Restart ExoSnap" was chosen.
    // main() reads this after app.exec() to relaunch a detached instance.
    [[nodiscard]] bool relaunchRequested() const noexcept {
        return relaunch_requested_;
    }

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void applyVisualScenario(const visual::VisualScenario& scenario);
#endif

  signals:
    void recordToggleRequested();
    void pauseToggleRequested();
    void captureFrameRequested();
    void addMarkerRequested();
    void splitRecordingRequested();

  private slots:
    void onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text);
    void onHotkeyServiceBindingChanged(exosnap::HotkeyAction action, QKeySequence seq);
    void toggleFullScreen();

    // Reactive device-change forwarding handlers.
    void onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap, exosnap::DiscoveryReason reason);
    void onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap, exosnap::DiscoveryReason reason);
    void onDisplaysChanged(const exosnap::DisplaySnapshot& snap, exosnap::DiscoveryReason reason);

    void onTrayActivateWindow();

  private:
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    void applyRuntimeWindowIcon();
    void switchRecordingIcon(bool recording, bool paused);
    bool effectiveMaximizedState() const;
    void applyRestoredGeometry();
    void saveWindowGeometry();

    void navigateToPage(int index);
    void setCurrentPage(int index);
    int navHighlightIndexFor(int index) const;
    void applyTitleBarStatus();
    void refreshPresetUi();
    void initHotkeyService();
    void refreshDiagnosticsData();

    // ---- Preset system (Stage 2) ----

    // Assemble the current live config from all sources.
    [[nodiscard]] RecordingPresetConfig captureLiveConfig() const;

    // Apply a preset config atomically to all pages.
    // Sets applying_preset_ = true for the duration.
    void applyPresetConfig(const RecordingPresetConfig& cfg);

    // Preset operation handlers (wired to ConfigPage signals).
    void onPresetSelected(const QString& id);
    void onSavePreset();
    void onSavePresetAs(const QString& name);
    void onNewPreset();
    void onDuplicatePreset();
    void onRenamePreset(const QString& name);
    void onDeletePreset();
    void onResetChanges();
    void onResetToDefaults();
    void onSetDefaultPreset();

    // Persist the full preset store state.
    void persistPresetState();

    void saveWindowGeometryToSettings();

    // Startup recovery: scan the manifest; open the overlay when candidates exist.
    void checkAndShowRecoveryOverlay();

    // CRASH-WIRE-R1 (ADR 0017): next-launch crash dialog. Shown when the previous
    // session did not mark a clean exit. Deferred behind the recovery overlay so
    // the user is never double-prompted.
    void checkAndShowCrashReportOverlay();
    void openCrashReportOverlay();
    // Build the live session context (version + output context) for the sidecar.
    [[nodiscard]] crash_capture::SessionContext currentSessionContext() const;
    // Push the current context to the crash engine + session sidecar. Cheap.
    void refreshCrashSessionContext();

    // RECORDING-OVERLAY-R1: update the recording overlay visibility/state.
    void updateRecordingOverlay();
    // DIAGNOSTICS-OVERLAY-R1: update the diagnostics overlay visibility/state.
    void updateDiagnosticsOverlay();
    // COUNTDOWN-OVERLAY-R1: update the countdown overlay.
    void onCountdownStateChanged(bool active, int remaining_seconds, int duration_seconds);

    // NOTIFY-TOASTS-R1: instantiate the manager + toast window; called from constructor.
    void initNotificationToasts();
    // Gate toasts on the show_notifications setting.
    void updateNotificationToastsEnabled();

    // UPDATE-WIRE-R1 (ADR 0012): trigger a guarded update check. No-op (and shows
    // the paused banner) while recording / remuxing.
    void triggerUpdateCheck();
    // Handle the async result: build the UI model + state, and enqueue a toast when
    // an update is available and notifications are enabled.
    void onUpdateCheckComplete(const update::UpdateCheckResult& result);
    // QUICK-PILL-R1: update the quick-control pill visibility/state.
    void updateQuickControlPill();

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void installVisualReadyMarker(const QString& scenario_id);
    void applyVisualSettingsScenario(const visual::VisualScenario& scenario);
    void applyVisualSourcePickerScenario(const visual::VisualScenario& scenario);
    void applyVisualDiagnosticsScenario(const visual::VisualScenario& scenario);
    // Apply device-discovery visual state (audio mic list, webcam availability).
    // Guarded; non-persistent: no writes to AppSettingsStore or RecordingPresetStore.
    void applyVisualDeviceDiscoveryScenario(const visual::VisualScenario& scenario);
    void applyVisualHotkeysScenario(const visual::VisualScenario& scenario);
#endif

    ui::chrome::OperationalTitleBar* title_bar_ = nullptr;
    ui::tray::TrayPresence* tray_presence_ = nullptr;
    ui::dialogs::AboutOverlay* about_overlay_ = nullptr;
    ui::dialogs::RecoveryOverlay* recovery_overlay_ = nullptr;
    ui::dialogs::SourcePickerOverlay* source_picker_overlay_ = nullptr;
    ui::dialogs::CrashReportOverlay* crash_overlay_ = nullptr;
    ui::overlay::CountdownOverlayWindow* countdown_overlay_ = nullptr;
    ui::overlay::RecordingOverlayWindow* recording_overlay_ = nullptr;
    ui::overlay::DiagnosticsOverlayWindow* diagnostics_overlay_ = nullptr;
    // QUICK-PILL-R1: interactive capture-excluded quick-control pill (no parent; top-level).
    ui::overlay::QuickControlPillWindow* quick_control_pill_ = nullptr;
    // NOTIFY-TOASTS-R1: manager (owned by this) + toast window (top-level, no parent).
    notifications::NotificationManager* notification_manager_ = nullptr;
    ui::overlay::NotificationToastWindow* notification_toast_window_ = nullptr;
    // UPDATE-WIRE-R1 (ADR 0012): Qt bridge to the update engine (owned by this).
    UpdateService* update_service_ = nullptr;
    // Last update check's releases-page URL (for the panel's "Open releases" / notes link).
    QString last_update_releases_url_;
    // Last known monitor rect from RecordPage for overlay positioning.
    QRect recording_monitor_rect_;
    QStackedWidget* stack_ = nullptr;
    RecordPage* record_page_ = nullptr;
    ConfigPage* config_page_ = nullptr;
    DiagnosticsPage* diagnostics_page_ = nullptr;
    LogsPage* logs_page_ = nullptr;
    WebcamPage* webcam_page_ = nullptr;
    HotkeysPage* hotkeys_page_ = nullptr;
    AdvancedPage* advanced_page_ = nullptr;

    // Device notifiers (owned; started after capability probe; stopped first in ~MainWindow).
    AudioDeviceNotifier audio_notifier_;
    WebcamDeviceNotifier webcam_notifier_;
    DisplayDeviceNotifier display_notifier_;

    // Live mirrors for the currently active configuration.
    OutputSettingsModel output_settings_;
    VideoSettingsModel video_settings_;
    capability::AudioUiState live_audio_;
    WebcamSettings live_webcam_;

    // Preset system (replaces legacy profile_registry_).
    RecordingPresetRegistry preset_registry_;
    RecordingPresetStore preset_store_;

    // Reduced AppSettingsStore: hotkeys + window geometry only.
    AppSettingsStore settings_store_;
    PersistedAppSettings persisted_settings_;

    // Recovery manifest + service (owned by MainWindow; coordinator gets a pointer).
    RecoveryManifestStore recovery_manifest_store_;
    RecoveryService recovery_service_;

    // Rebindable global hotkey service. Owns binding model + Win32 registration.
    GlobalHotkeyService* hotkey_service_ = nullptr;
    std::unique_ptr<IHotkeyRegistrar> win32_hotkey_registrar_;

    bool recording_active_ = false;
    // ADR-0014: true while the MP4 remux job is running after the engine stopped.
    bool remuxing_active_ = false;
    // TRAY-CLOSE-TO-TRAY-R1: set to true when the user explicitly quits via the
    // tray menu "Quit" action so closeEvent bypasses the hide-to-tray logic.
    bool force_quit_ = false;
    bool runtime_window_icon_bound_ = false;
    bool resizable_style_applied_ = false;
    bool hotkeys_registered_ = false;
    bool win32_maximized_ = false;
    bool resize_cursor_shown_ = false;
    bool syncing_preset_ui_ = false;
    bool applying_preset_ = false;
    bool geometry_restored_ = false;
    bool pre_fullscreen_maximized_ = false;
    capability::CapabilitySet runtime_caps_;
    bool runtime_caps_ready_ = false;
    QString record_status_label_ = QStringLiteral("READY");

    // CRASH-WIRE-R1 (ADR 0017): crash-capture session lifecycle.
    std::string crash_dir_;
    std::optional<crash_capture::SessionContext> pending_crash_;
    bool relaunch_requested_ = false;
};

} // namespace exosnap
