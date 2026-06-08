#pragma once
#include <QKeySequence>
#include <QMainWindow>
#include <QStackedWidget>
#include <QString>
#include <array>
#include <cstdint>

#include "models/OutputSettingsModel.h"
#include "models/RecordingProfileRegistry.h"
#include "models/VideoSettingsModel.h"
#include "settings/AppSettingsStore.h"
#include <capability/capability_set.h>

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

namespace ui::dialogs {
class AboutOverlay;
class SourcePickerOverlay;
} // namespace ui::dialogs

class AdvancedPage;
class ConfigPage;
class DiagnosticsPage;
class HotkeysPage;
class LogsPage;
class RecordPage;
class WebcamPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void applyVisualScenario(const visual::VisualScenario& scenario);
#endif

  signals:
    void recordToggleRequested();
    void pauseToggleRequested();

  private slots:
    void onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text);
    void onHotkeyBindingChanged(int action_index, QKeySequence seq);
    void toggleFullScreen();

  private:
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    void applyRuntimeWindowIcon();
    void switchRecordingIcon(bool recording);
    bool effectiveMaximizedState() const;
    void applyRestoredGeometry();
    void saveWindowGeometry();

    void navigateToPage(int index);
    void setCurrentPage(int index);
    int navHighlightIndexFor(int index) const;
    void applyTitleBarStatus();
    void applyActiveProfileToPages();
    void refreshOutputProfileUi();
    void persistProfileState();
    void restoreHotkeyBindingsFromSettings();
    void refreshDiagnosticsData();
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void installVisualReadyMarker(const QString& scenario_id);
    void applyVisualSettingsScenario(const visual::VisualScenario& scenario);
    void applyVisualSourcePickerScenario(const visual::VisualScenario& scenario);
    void applyVisualDiagnosticsScenario();
#endif

    ui::chrome::OperationalTitleBar* title_bar_ = nullptr;
    ui::dialogs::AboutOverlay* about_overlay_ = nullptr;
    ui::dialogs::SourcePickerOverlay* source_picker_overlay_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    RecordPage* record_page_ = nullptr;
    ConfigPage* config_page_ = nullptr;
    DiagnosticsPage* diagnostics_page_ = nullptr;
    LogsPage* logs_page_ = nullptr;
    WebcamPage* webcam_page_ = nullptr;
    HotkeysPage* hotkeys_page_ = nullptr;
    AdvancedPage* advanced_page_ = nullptr;
    OutputSettingsModel output_settings_;
    VideoSettingsModel video_settings_;
    RecordingProfileRegistry profile_registry_;
    AppSettingsStore settings_store_;
    PersistedAppSettings persisted_settings_;
    static constexpr int kHotkeyIdStartStop = 1;
    static constexpr int kHotkeyIdPauseResume = 2;
    static constexpr int kHotkeyIdSplit = 3;
    static constexpr int kHotkeyIdMuteMic = 4;
    bool recording_active_ = false;
    bool runtime_window_icon_bound_ = false;
    bool resizable_style_applied_ = false;
    bool hotkeys_registered_ = false;
    bool win32_maximized_ = false;
    bool resize_cursor_shown_ = false;
    bool syncing_profile_ui_ = false;
    bool geometry_restored_ = false;
    bool pre_fullscreen_maximized_ = false;
    std::array<QKeySequence, 4> persisted_hotkeys_ = {
        QKeySequence(Qt::ALT | Qt::Key_F9),
        QKeySequence(),
        QKeySequence(),
        QKeySequence(),
    };
    capability::CapabilitySet runtime_caps_;
    bool runtime_caps_ready_ = false;
    QString record_status_label_ = QStringLiteral("READY");
};

} // namespace exosnap
