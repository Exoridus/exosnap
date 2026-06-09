#pragma once

#include "../models/WebcamSettings.h"
#include "../services/WebcamDeviceNotifier.h"
#include "../services/WebcamService.h"

#include <QHideEvent>
#include <QShowEvent>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace exosnap {

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace visual {
enum class VisualWebcamState;
}
#endif

namespace ui::widgets {
class CameraPreview;
class ExoToggle;
class SectionRuleHeader;
} // namespace ui::widgets

class WebcamPage : public QWidget {
    Q_OBJECT
  public:
    explicit WebcamPage(QWidget* parent = nullptr);
    ~WebcamPage() override;

    void applySettings(const WebcamSettings& settings);
    void setRecordingControlsLocked(bool locked);
    // Reactive device refresh: preserve the configured device_id across the new
    // snapshot.  Never emits settingsChanged.
    void onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap);
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void applyVisualState(visual::VisualWebcamState state);
#endif

  signals:
    void settingsChanged(WebcamSettings settings);
    void backToSettingsRequested();
    // Emitted when the user clicks the Rescan button so MainWindow can route the
    // request through the canonical WebcamDeviceNotifier::rescan() path.
    void rescanRequested();

  private slots:
    void onEnableToggled(bool enabled);
    void onDeviceChanged(int index);
    void onResolutionChanged(int index);
    void onChromaEnableToggled(bool enabled);
    void onToleranceChanged(int value);
    void onSoftnessChanged(int value);
    void onRefreshDevices();
    void onPreviewFrame(QImage frame);

  private:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    void refreshDevices();
    void refreshFormats();
    void applyCurrentSettings();
    void startPreview();
    void stopPreview();
    WebcamSettings collectSettings() const;

    // Webcam device enumeration + capture (owned by this page for preview)
    std::vector<WebcamDeviceInfo> devices_;
    std::vector<WebcamFormat> formats_;
    WebcamService preview_service_;

    // Controls
    ui::widgets::ExoToggle* enable_toggle_ = nullptr;
    QComboBox* device_combo_ = nullptr;
    QComboBox* resolution_combo_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;

    // Camera-only setup preview (no overlay / target compositing)
    ui::widgets::CameraPreview* camera_preview_ = nullptr;
    QTimer* preview_watchdog_ = nullptr;
    bool preview_frame_seen_ = false;

    // Overlay position (simple sliders 0–100% for MVP)
    QSlider* pos_x_slider_ = nullptr;
    QSlider* pos_y_slider_ = nullptr;
    QSlider* size_w_slider_ = nullptr;
    QSlider* size_h_slider_ = nullptr;
    QLabel* pos_x_label_ = nullptr;
    QLabel* pos_y_label_ = nullptr;
    QLabel* size_w_label_ = nullptr;
    QLabel* size_h_label_ = nullptr;
    QCheckBox* aspect_lock_check_ = nullptr;

    // Chroma key
    ui::widgets::ExoToggle* chroma_toggle_ = nullptr;
    QPushButton* chroma_color_btn_ = nullptr;
    QSlider* tolerance_slider_ = nullptr;
    QSlider* softness_slider_ = nullptr;
    QLabel* tolerance_label_ = nullptr;
    QLabel* softness_label_ = nullptr;

    WebcamSettings current_settings_;
    bool suppress_signals_ = false;
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    bool visual_test_mode_ = false;
#endif
};

} // namespace exosnap
