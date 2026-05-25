#pragma once

#include "../models/WebcamSettings.h"
#include "../services/WebcamService.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

namespace exosnap {

namespace ui::widgets {
class ExoToggle;
class SectionRuleHeader;
} // namespace ui::widgets

class WebcamPage : public QWidget {
    Q_OBJECT
  public:
    explicit WebcamPage(QWidget* parent = nullptr);
    ~WebcamPage() override;

    void applySettings(const WebcamSettings& settings);

  signals:
    void settingsChanged(WebcamSettings settings);

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

    // Preview
    QLabel* preview_label_ = nullptr;

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
};

} // namespace exosnap
