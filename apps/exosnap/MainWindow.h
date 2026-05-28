#pragma once
#include <QKeySequence>
#include <QListWidget>
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

class QLabel;
class QShowEvent;
class QTimer;

namespace exosnap {

namespace ui::chrome {
class OperationalTitleBar;
class GlobalRecordingBar;
} // namespace ui::chrome

class ConfigPage;
class DiagnosticsPage;
class HotkeysPage;
class OutputPage;
class RecordPage;
class WebcamPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);

  signals:
    void recordToggleRequested();
    void pauseToggleRequested();

  private slots:
    void onNavRowChanged(int row);
    void onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text);
    void onRecordChromeRuntimeMetricsChanged(const QString& elapsed_text, const QString& bitrate_text,
                                             const QString& drop_text, const QString& size_text);
    void onGlobalRecordingBarPrimaryActionRequested();
    void onGlobalRecordingBarPauseActionRequested();
    void pollIdleRuntimeMetrics();
    void onHotkeyBindingChanged(int action_index, QKeySequence seq);

  private:
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& event_type, void* message, qintptr* result) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    void applyRuntimeWindowIcon();
    bool effectiveMaximizedState() const;

    void setCurrentPage(int index);
    void updatePageHeader(int index);
    QString buildGlobalRecordingBarProfileSummary() const;
    QString buildGlobalRecordingBarTargetSummary() const;
    QString buildOutputPageMeta() const;
    QString buildOutputSummary() const;
    void refreshGlobalRecordingBarContext();
    void applyActiveProfileToPages();
    void refreshOutputProfileUi();
    void persistProfileState();
    void restoreHotkeyBindingsFromSettings();
    void refreshDiagnosticsData();

    ui::chrome::OperationalTitleBar* title_bar_ = nullptr;
    ui::chrome::GlobalRecordingBar* global_recording_bar_ = nullptr;
    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    RecordPage* record_page_ = nullptr;
    OutputPage* output_page_ = nullptr;
    ConfigPage* config_page_ = nullptr;
    DiagnosticsPage* diagnostics_page_ = nullptr;
    WebcamPage* webcam_page_ = nullptr;
    HotkeysPage* hotkeys_page_ = nullptr;
    OutputSettingsModel output_settings_;
    VideoSettingsModel video_settings_;
    RecordingProfileRegistry profile_registry_;
    AppSettingsStore settings_store_;
    PersistedAppSettings persisted_settings_;
    QLabel* page_kicker_label_ = nullptr;
    QLabel* page_title_label_ = nullptr;
    QLabel* page_subtitle_label_ = nullptr;
    QLabel* page_meta_label_ = nullptr;
    QLabel* sidebar_status_value_label_ = nullptr;
    QTimer* idle_metrics_timer_ = nullptr;
    static constexpr int kHotkeyIdStartStop = 1;
    static constexpr int kHotkeyIdPauseResume = 2;
    static constexpr int kHotkeyIdSplit = 3;
    static constexpr int kHotkeyIdMuteMic = 4;
    bool recording_active_ = false;
    bool runtime_window_icon_bound_ = false;
    bool resizable_style_applied_ = false;
    bool hotkeys_registered_ = false;
    bool win32_maximized_ = false;
    bool syncing_profile_ui_ = false;
    std::array<QKeySequence, 4> persisted_hotkeys_ = {
        QKeySequence(Qt::ALT | Qt::Key_F9),
        QKeySequence(),
        QKeySequence(),
        QKeySequence(),
    };
    capability::CapabilitySet runtime_caps_;
    bool runtime_caps_ready_ = false;
    QString recording_context_text_;
    QString record_status_label_ = QStringLiteral("READY");
    std::uint64_t last_cpu_idle_ticks_ = 0;
    std::uint64_t last_cpu_kernel_ticks_ = 0;
    std::uint64_t last_cpu_user_ticks_ = 0;
    bool cpu_baseline_ready_ = false;
};

} // namespace exosnap
