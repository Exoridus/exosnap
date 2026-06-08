#pragma once

#include "../../models/WebcamSettings.h"
#include "../../services/WebcamService.h"

#include <QHideEvent>
#include <QShowEvent>
#include <QWidget>
#include <vector>

class QComboBox;
class QPushButton;
class QTimer;

namespace exosnap::ui::widgets {

class CameraPreview;
class ExoToggle;

// Reusable inline webcam setup panel for the Settings Webcam card.
//
// Packages the live CameraPreview, enable toggle, device combo, resolution
// combo, and a compact Rescan button into a single embeddable widget. The
// preview starts when the panel is shown and stops on hide. No overlay
// placement controls: those belong in the Record preview.
class WebcamSetupPanel : public QWidget {
    Q_OBJECT
  public:
    explicit WebcamSetupPanel(QWidget* parent = nullptr);
    ~WebcamSetupPanel() override;

    // Apply external settings without emitting settingsChanged.
    void applySettings(const WebcamSettings& settings);

    // Lock all controls during recording (preview still runs if already started).
    void setControlsLocked(bool locked);

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // Deterministic visual-test state: suppresses real capture and injects a
    // synthetic frame (available) or an honest unavailable placeholder, plus the
    // mirror toggle/preview state. Never compiled into Release builds.
    void applyVisualState(bool available, bool mirror);
#endif

  signals:
    void settingsChanged(WebcamSettings settings);

  private slots:
    void onEnableToggled(bool enabled);
    void onDeviceChanged(int index);
    void onResolutionChanged(int index);
    void onMirrorToggled(bool mirror);
    void onRescan();
    void onPreviewFrame(QImage frame);

  private:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    void refreshDevices();
    void refreshFormats();
    void startPreview();
    void stopPreview();
    WebcamSettings collectSettings() const;

    WebcamService preview_service_;
    std::vector<WebcamDeviceInfo> devices_;
    std::vector<WebcamFormat> formats_;
    WebcamSettings current_settings_;
    bool suppress_signals_ = false;
    bool preview_frame_seen_ = false;
    bool visual_test_mode_ = false;

    CameraPreview* camera_preview_ = nullptr;
    ExoToggle* enable_toggle_ = nullptr;
    QComboBox* device_combo_ = nullptr;
    QComboBox* resolution_combo_ = nullptr;
    ExoToggle* mirror_toggle_ = nullptr;
    QPushButton* rescan_btn_ = nullptr;
    QTimer* watchdog_ = nullptr;
};

} // namespace exosnap::ui::widgets
