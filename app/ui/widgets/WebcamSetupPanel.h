#pragma once

#include "../../models/WebcamSettings.h"
#include "../../services/WebcamDeviceNotifier.h"
#include "../../services/WebcamService.h"

#include <QHideEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>
#include <vector>

class QComboBox;
class QEvent;
class QPushButton;
class QSlider;
class QLabel;
class QTimer;

namespace exosnap::ui::widgets {

class CameraPreview;
class ExoToggle;

// Reusable inline webcam setup panel for the Settings Webcam card.
//
// Packages the live CameraPreview (full-width, on top), enable toggle, device
// combo, resolution combo, and a compact Rescan button floating on the
// preview's bottom-right corner into a single embeddable widget. The preview
// starts when the panel is shown and stops on hide. No overlay placement
// controls: those belong in the Record preview.
class WebcamSetupPanel : public QWidget {
    Q_OBJECT
  public:
    explicit WebcamSetupPanel(QWidget* parent = nullptr);
    ~WebcamSetupPanel() override;

    // Apply external settings without emitting settingsChanged.
    void applySettings(const WebcamSettings& settings);

    // Reactive device refresh: preserve the configured device_id/format across the
    // new snapshot.  If the configured device is absent → stop preview, show
    // unavailable placeholder, keep stored id.  If it returns → restore per
    // showEvent/visibility rules.  Never emits settingsChanged.
    void onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap);

    // Lock restart-class controls during recording (preview still runs if already started).
    // Enable and mirror remain live-editable.
    void setControlsLocked(bool locked);

    // S4: Call once when the capability probe reports mfplat.dll is absent.
    // Shows an inline notice and permanently disables all controls. No-op when
    // unavailable is false.
    void setMfUnavailable(bool unavailable);

    // The panel no longer opens its own camera reader; it is a pure viewer of the
    // single shared capture owned by the RecordingCoordinator. MainWindow pushes each
    // frame here (the panel applies its own mirror via CameraPreview), so the Settings
    // preview and the Record PiP share one reader — no device-lock fight, and it works
    // during recording. Frames for a different device than the one currently selected
    // are ignored to avoid briefly showing the wrong camera.
    void setPreviewFrame(const QImage& frame);

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // Deterministic visual-test state: suppresses real capture and injects a
    // synthetic frame (available) or an honest unavailable placeholder, plus the
    // mirror toggle/preview state and the chroma-key group (enabled + key colour
    // mode "green"/"blue"/"magenta"/"custom"). Never compiled into Release builds.
    void applyVisualState(bool available, bool mirror, bool chroma_enabled = false,
                          const QString& chroma_color_mode = QString());
#endif

    // One resolution_combo_ row, reduced to the fields the selection policy
    // below needs (widget-free so it is unit-testable without a live combo or
    // a real camera).
    struct ResolutionRow {
        int width = 0;
        int height = 0;
        int fps = 0;
    };

    // Selects which row of `rows` (in combo order) best matches a request of
    // (width, height, fps): an exact match on all three if one exists,
    // otherwise the first row matching just width/height -- a stored fps the
    // device no longer offers at that resolution (a different camera, or a
    // preset saved against a wider-format device) still resolves to *a* row
    // instead of leaving the combo on whatever it last had. Returns -1 when no
    // row matches even on width/height. Exposed static + public for unit
    // testing; findResolutionComboIndexFor() adapts this to the live combo.
    [[nodiscard]] static int FindResolutionRowIndex(const std::vector<ResolutionRow>& rows, int width, int height,
                                                    int fps) noexcept;

  signals:
    void settingsChanged(WebcamSettings settings);
    // Emitted when the user presses ↺ Rescan so MainWindow can route through
    // the canonical WebcamDeviceNotifier::rescan() path instead of calling
    // refreshDevices() directly.  MainWindow connects this before the panel is
    // shown for the first time.
    void rescanRequested();
    // Emitted when the panel wants the shared webcam capture to run (visible + enabled +
    // a device is selected) or to stop. MainWindow relays this to the coordinator as a
    // capture consumer, so the one reader stays alive while the Settings preview needs it.
    void previewActiveRequested(bool active);

  private slots:
    void onEnableToggled(bool enabled);
    void onDeviceChanged(int index);
    void onResolutionChanged(int index);
    void onMirrorToggled(bool mirror);
    void onOpacityChanged(int value);
    void onChromaEnableToggled(bool enabled);
    void onColorModeChanged(WebcamChromaKeyColorMode mode);
    void onToleranceChanged(int value);
    void onSoftnessChanged(int value);
    void onSpillReductionChanged(int value);
    void onRescan();
    void onPreviewFrame(QImage frame);

  private:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Catches camera_preview_'s own Resize/Move so rescan_btn_ stays pinned to
    // its bottom-right corner even if the preview is ever resized/moved
    // independently of the panel (e.g. a future heightForWidth or splitter
    // layout) — the panel's own resizeEvent alone only works today because the
    // preview happens to scale 1:1 with the panel.
    bool eventFilter(QObject* watched, QEvent* event) override;

    void refreshDevices();
    void refreshFormats();
    void startPreview();
    void stopPreview();
    // Refreshes the Key color button's icon swatch + hex text from the active
    // chroma-key color (current_settings_.chroma_key.active_color()).
    void updateKeyColorButton();
    // Repositions rescan_btn_ bottom-right over camera_preview_ (it is a
    // floating, non-layout child of the preview, not the device row).
    void positionRescanButton();
    WebcamSettings collectSettings() const;
    // Adapts resolution_combo_'s current items to FindResolutionRowIndex() (see
    // the public declaration above for the matching policy).
    int findResolutionComboIndexFor(int width, int height, int fps) const;
    // Re-reads resolution_combo_'s currently selected row into
    // current_settings_.width/height/fps. Needed after setCurrentIndex() is
    // called under signal suppression (applySettings() / onWebcamDevicesChanged()):
    // when FindResolutionRowIndex() falls back to a width/height-only match (the
    // exact fps is no longer offered), the combo now shows a different fps than
    // current_settings_ still holds, and nothing else re-syncs it while signals
    // stay suppressed.
    void resyncCurrentSettingsFromResolutionCombo();

    std::vector<WebcamDeviceInfo> devices_;
    std::vector<WebcamFormat> formats_;
    WebcamSettings current_settings_;
    bool suppress_signals_ = false;
    bool preview_frame_seen_ = false;
    bool visual_test_mode_ = false;
    bool mf_unavailable_ = false; // S4: set when mfplat.dll is absent at runtime

    CameraPreview* camera_preview_ = nullptr;
    ExoToggle* enable_toggle_ = nullptr;
    QComboBox* device_combo_ = nullptr;
    QComboBox* resolution_combo_ = nullptr;
    ExoToggle* mirror_toggle_ = nullptr;
    QPushButton* rescan_btn_ = nullptr;
    QTimer* watchdog_ = nullptr;

    // Overlay opacity (live-editable, same lock class as Mirror).
    QSlider* opacity_slider_ = nullptr;
    QLabel* opacity_value_label_ = nullptr;

    // Chroma-key group (collapsed while disabled; live-editable when shown).
    ExoToggle* chroma_toggle_ = nullptr;
    QWidget* chroma_body_ = nullptr;
    // Flat button showing the active key color as an icon swatch + hex text;
    // opens the QColorDialog custom-color picker. Replaces the former four
    // preset chip buttons (Green/Blue/Magenta/Custom).
    QPushButton* key_color_btn_ = nullptr;
    QSlider* tolerance_slider_ = nullptr;
    QLabel* tolerance_value_label_ = nullptr;
    QSlider* softness_slider_ = nullptr;
    QLabel* softness_value_label_ = nullptr;
    QSlider* spill_slider_ = nullptr;
    QLabel* spill_value_label_ = nullptr;
};

} // namespace exosnap::ui::widgets
