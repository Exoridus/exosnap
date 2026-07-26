#include "WebcamSetupPanel.h"

#include "CameraPreview.h"
#include "ComboBoxWheelFilter.h"
#include "ExoToggle.h"

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSize>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace exosnap::ui::widgets {

namespace {

QFrame* makeHairline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("frameRole", "sectionRuleLine");
    return line;
}

QString pct(int value) {
    return QString::number(value) + QStringLiteral("%");
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
WebcamChromaKeyColorMode colorModeFromString(const QString& mode) {
    if (mode.compare(QStringLiteral("blue"), Qt::CaseInsensitive) == 0)
        return WebcamChromaKeyColorMode::Blue;
    if (mode.compare(QStringLiteral("magenta"), Qt::CaseInsensitive) == 0)
        return WebcamChromaKeyColorMode::Magenta;
    if (mode.compare(QStringLiteral("custom"), Qt::CaseInsensitive) == 0)
        return WebcamChromaKeyColorMode::Custom;
    return WebcamChromaKeyColorMode::Green;
}
#endif

} // namespace

WebcamSetupPanel::WebcamSetupPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("webcamSetupPanel"));

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    // ── Left column: compact live preview ────────────────────────────────────
    camera_preview_ = new CameraPreview(this);
    // Override the standalone-page WIDTH only: compact 180-300px for the Settings
    // embed. Height is intentionally left Expanding (not a hardcoded Fixed value)
    // so the preview grows to match the right column's control stack. A previous
    // hardcoded setFixedHeight(175) + Qt::AlignTop matched the control column back
    // when it only held Enable/Camera/Resolution/Mirror; once Overlay opacity and
    // the Chroma key group were added the right column grew taller than 175px,
    // leaving a dead gap below the pinned-height preview.
    camera_preview_->setMinimumHeight(175);
    camera_preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    camera_preview_->setMaximumWidth(300);
    camera_preview_->setMinimumWidth(180);
    root->addWidget(camera_preview_, 0);

    // ── Right column: controls ────────────────────────────────────────────────
    auto* right_col = new QWidget(this);
    auto* right_layout = new QVBoxLayout(right_col);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(8);

    // Enable toggle row
    auto* enable_row = new QWidget(right_col);
    auto* er = new QHBoxLayout(enable_row);
    er->setContentsMargins(0, 0, 0, 0);
    er->setSpacing(10);
    auto* enable_label = new QLabel(QStringLiteral("Record webcam"), enable_row);
    enable_label->setProperty("labelRole", "settingsRowLabel");
    er->addWidget(enable_label, 1);
    enable_toggle_ = new ExoToggle(enable_row);
    enable_toggle_->setObjectName(QStringLiteral("webcamPanelEnableToggle"));
    enable_toggle_->setChecked(false);
    er->addWidget(enable_toggle_);
    right_layout->addWidget(enable_row);

    right_layout->addWidget(makeHairline(right_col));

    // Device row: combo + compact rescan button
    auto* device_label = new QLabel(QStringLiteral("Camera"), right_col);
    device_label->setProperty("labelRole", "settingsRowLabel");
    right_layout->addWidget(device_label);

    auto* device_row = new QWidget(right_col);
    auto* dr = new QHBoxLayout(device_row);
    dr->setContentsMargins(0, 0, 0, 0);
    dr->setSpacing(6);
    device_combo_ = new QComboBox(right_col);
    device_combo_->setObjectName(QStringLiteral("webcamPanelDeviceCombo"));
    device_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    dr->addWidget(device_combo_, 1);

    rescan_btn_ = new QPushButton(right_col); // #09: icon-only rescan button
    rescan_btn_->setObjectName(QStringLiteral("webcamPanelRescanBtn"));
    rescan_btn_->setProperty("role", "ghost");
    rescan_btn_->setToolTip(QStringLiteral("Rescan for cameras"));
    rescan_btn_->setFixedWidth(36);
    rescan_btn_->setCursor(Qt::PointingHandCursor);
    {
        // Themed lucide glyph in HT.mut — the previous currentColor SVG inherited the
        // lighter ghost-button text colour.
        const qreal dpr = rescan_btn_->devicePixelRatioF();
        rescan_btn_->setIcon(exosnap::ui::theme::lucideIcon(
            QStringLiteral("refresh-cw"), QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut), 14, dpr));
        rescan_btn_->setIconSize(QSize(14, 14));
    }
    dr->addWidget(rescan_btn_);
    right_layout->addWidget(device_row);

    // Resolution / FPS
    auto* res_label = new QLabel(QStringLiteral("Resolution / FPS"), right_col);
    res_label->setProperty("labelRole", "settingsRowLabel");
    right_layout->addWidget(res_label);

    resolution_combo_ = new QComboBox(right_col);
    resolution_combo_->setObjectName(QStringLiteral("webcamPanelResolutionCombo"));
    right_layout->addWidget(resolution_combo_);

    right_layout->addWidget(makeHairline(right_col));

    // Mirror toggle row (the only image-transform control here; placement is on Record).
    auto* mirror_row = new QWidget(right_col);
    auto* mr = new QHBoxLayout(mirror_row);
    mr->setContentsMargins(0, 0, 0, 0);
    mr->setSpacing(10);
    auto* mirror_label = new QLabel(QStringLiteral("Mirror image"), mirror_row);
    mirror_label->setProperty("labelRole", "settingsRowLabel");
    mr->addWidget(mirror_label, 1);
    mirror_toggle_ = new ExoToggle(mirror_row);
    mirror_toggle_->setObjectName(QStringLiteral("webcamPanelMirrorToggle"));
    mirror_toggle_->setChecked(false);
    mr->addWidget(mirror_toggle_);
    right_layout->addWidget(mirror_row);

    // Reusable "label + slider + percent" row builder (0–100 %), matching the
    // compact Settings row rhythm.
    auto addSliderRow = [&](const QString& label_text, const QString& object_name, int def, QSlider*& slider,
                            QLabel*& value_label, QWidget* row_parent) -> QWidget* {
        auto* row = new QWidget(row_parent);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(8);
        auto* lbl = new QLabel(label_text, row);
        lbl->setProperty("labelRole", "settingsRowLabel");
        rl->addWidget(lbl, 1);
        slider = new QSlider(Qt::Horizontal, row);
        slider->setObjectName(object_name);
        slider->setRange(0, 100);
        slider->setValue(def);
        slider->setFixedWidth(140);
        rl->addWidget(slider, 0);
        value_label = new QLabel(pct(def), row);
        value_label->setProperty("labelRole", "settingsRowLabel");
        value_label->setFixedWidth(38);
        value_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rl->addWidget(value_label, 0);
        return row;
    };

    // ── Overlay opacity (live-editable, same lock class as Mirror) ────────────
    right_layout->addWidget(addSliderRow(QStringLiteral("Overlay opacity"), QStringLiteral("webcamPanelOpacitySlider"),
                                         100, opacity_slider_, opacity_value_label_, right_col));

    right_layout->addWidget(makeHairline(right_col));

    // ── Chroma key: collapsed group; the header toggle expands/collapses it ────
    auto* chroma_header = new QWidget(right_col);
    auto* chr = new QHBoxLayout(chroma_header);
    chr->setContentsMargins(0, 0, 0, 0);
    chr->setSpacing(10);
    auto* chroma_label = new QLabel(QStringLiteral("Chroma key"), chroma_header);
    chroma_label->setProperty("labelRole", "settingsRowLabel");
    chr->addWidget(chroma_label, 1);
    chroma_toggle_ = new ExoToggle(chroma_header);
    chroma_toggle_->setObjectName(QStringLiteral("webcamPanelChromaToggle"));
    chroma_toggle_->setChecked(false);
    chr->addWidget(chroma_toggle_);
    right_layout->addWidget(chroma_header);

    // Collapsible body: hidden until the toggle enables chroma keying.
    chroma_body_ = new QWidget(right_col);
    chroma_body_->setObjectName(QStringLiteral("webcamPanelChromaBody"));
    auto* cb = new QVBoxLayout(chroma_body_);
    cb->setContentsMargins(0, 2, 0, 0);
    cb->setSpacing(8);

    // Key colour: swatch preview + preset chips + custom picker.
    auto* color_row = new QWidget(chroma_body_);
    auto* clr = new QHBoxLayout(color_row);
    clr->setContentsMargins(0, 0, 0, 0);
    clr->setSpacing(8);
    auto* color_label = new QLabel(QStringLiteral("Key color"), color_row);
    color_label->setProperty("labelRole", "settingsRowLabel");
    clr->addWidget(color_label, 1);

    chroma_swatch_ = new QPushButton(color_row);
    chroma_swatch_->setObjectName(QStringLiteral("webcamPanelChromaSwatch"));
    chroma_swatch_->setToolTip(QStringLiteral("Current key color"));
    chroma_swatch_->setFixedSize(22, 22);
    chroma_swatch_->setCursor(Qt::PointingHandCursor);
    chroma_swatch_->setFlat(true);
    clr->addWidget(chroma_swatch_, 0);

    auto makeChip = [&](const QString& text, const QString& object_name) -> QPushButton* {
        auto* btn = new QPushButton(text, color_row);
        btn->setObjectName(object_name);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("role", "ghost");
        clr->addWidget(btn, 0);
        return btn;
    };
    chroma_green_btn_ = makeChip(QStringLiteral("Green"), QStringLiteral("webcamPanelChromaGreenBtn"));
    chroma_blue_btn_ = makeChip(QStringLiteral("Blue"), QStringLiteral("webcamPanelChromaBlueBtn"));
    chroma_magenta_btn_ = makeChip(QStringLiteral("Magenta"), QStringLiteral("webcamPanelChromaMagentaBtn"));
    chroma_custom_btn_ = makeChip(QStringLiteral("Custom…"), QStringLiteral("webcamPanelChromaCustomBtn"));
    chroma_green_btn_->setChecked(true);

    cb->addWidget(color_row);
    cb->addWidget(addSliderRow(QStringLiteral("Tolerance"), QStringLiteral("webcamPanelChromaToleranceSlider"), 40,
                               tolerance_slider_, tolerance_value_label_, chroma_body_));
    cb->addWidget(addSliderRow(QStringLiteral("Softness"), QStringLiteral("webcamPanelChromaSoftnessSlider"), 15,
                               softness_slider_, softness_value_label_, chroma_body_));
    cb->addWidget(addSliderRow(QStringLiteral("Spill reduction"), QStringLiteral("webcamPanelChromaSpillSlider"), 30,
                               spill_slider_, spill_value_label_, chroma_body_));

    right_layout->addWidget(chroma_body_);
    chroma_body_->setVisible(false); // collapsed while disabled
    updateChromaSwatch();

    // v0.9 polish: the placement note moved into the Webcam card's info-i (kWebcamPlacement),
    // which also reclaims the trailing hairline + note row that padded the card height.
    right_layout->addStretch(1);
    root->addWidget(right_col, 1);

    auto* wheel_filter = new ComboBoxWheelFilter(this);
    wheel_filter->installOn(device_combo_);
    wheel_filter->installOn(resolution_combo_);

    // Wire signals
    connect(enable_toggle_, &ExoToggle::toggled, this, &WebcamSetupPanel::onEnableToggled);
    connect(device_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &WebcamSetupPanel::onDeviceChanged);
    connect(resolution_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &WebcamSetupPanel::onResolutionChanged);
    connect(mirror_toggle_, &ExoToggle::toggled, this, &WebcamSetupPanel::onMirrorToggled);
    connect(rescan_btn_, &QPushButton::clicked, this, &WebcamSetupPanel::onRescan);
    connect(opacity_slider_, &QSlider::valueChanged, this, &WebcamSetupPanel::onOpacityChanged);
    connect(chroma_toggle_, &ExoToggle::toggled, this, &WebcamSetupPanel::onChromaEnableToggled);
    connect(chroma_green_btn_, &QPushButton::clicked, this,
            [this]() { onColorModeChanged(WebcamChromaKeyColorMode::Green); });
    connect(chroma_blue_btn_, &QPushButton::clicked, this,
            [this]() { onColorModeChanged(WebcamChromaKeyColorMode::Blue); });
    connect(chroma_magenta_btn_, &QPushButton::clicked, this,
            [this]() { onColorModeChanged(WebcamChromaKeyColorMode::Magenta); });
    // The swatch and the "Custom…" chip open the same colour picker (the obvious
    // expectation for a clickable colour swatch).
    const auto pick_custom_color = [this]() {
        const auto& ck = current_settings_.chroma_key;
        const QColor initial = (ck.color_mode == WebcamChromaKeyColorMode::Custom)
                                   ? QColor(ck.custom_r, ck.custom_g, ck.custom_b)
                                   : QColor(ck.active_color().r, ck.active_color().g, ck.active_color().b);
        const QColor picked = QColorDialog::getColor(initial, this, QStringLiteral("Pick chroma key color"));
        if (!picked.isValid()) {
            // Restore the checked state to the still-active mode (the chip was
            // clicked, flipping its checked flag before the dialog was cancelled).
            updateChromaColorButtons();
            return;
        }
        current_settings_.chroma_key.custom_r = static_cast<uint8_t>(picked.red());
        current_settings_.chroma_key.custom_g = static_cast<uint8_t>(picked.green());
        current_settings_.chroma_key.custom_b = static_cast<uint8_t>(picked.blue());
        onColorModeChanged(WebcamChromaKeyColorMode::Custom);
    };
    connect(chroma_custom_btn_, &QPushButton::clicked, this, pick_custom_color);
    connect(chroma_swatch_, &QPushButton::clicked, this, pick_custom_color);
    connect(tolerance_slider_, &QSlider::valueChanged, this, &WebcamSetupPanel::onToleranceChanged);
    connect(softness_slider_, &QSlider::valueChanged, this, &WebcamSetupPanel::onSoftnessChanged);
    connect(spill_slider_, &QSlider::valueChanged, this, &WebcamSetupPanel::onSpillReductionChanged);

    // Watchdog: surface a non-technical hint if no frame arrives after 3 s.
    watchdog_ = new QTimer(this);
    watchdog_->setSingleShot(true);
    watchdog_->setInterval(3000);
    connect(watchdog_, &QTimer::timeout, this, [this]() {
        if (preview_frame_seen_ || !camera_preview_)
            return;
        camera_preview_->clearFrame();
        camera_preview_->setPlaceholderText(
            QStringLiteral("Camera preview unavailable.\nClose other apps that may be using this camera."));
    });

    refreshDevices();
}

WebcamSetupPanel::~WebcamSetupPanel() {
    // Do NOT call stopPreview() here: it emits previewActiveRequested, and relaying a
    // signal out of a destructor while Qt tears down the parent widget tree can re-enter
    // a half-destroyed receiver (observed as an access violation in ConfigPage teardown).
    // The capture consumer is released implicitly — Qt drops this object's connections on
    // teardown, and the shared capture is only ever destroyed at app shutdown. Just stop
    // the local watchdog timer.
    if (watchdog_)
        watchdog_->stop();
}

void WebcamSetupPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (mf_unavailable_)
        return; // S4: MF absent — no preview attempt
    startPreview();
}

void WebcamSetupPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    stopPreview();
}

void WebcamSetupPanel::applySettings(const WebcamSettings& settings) {
    const WebcamSettings s = SanitizeWebcamSettings(settings);
    suppress_signals_ = true;
    current_settings_ = s;

    enable_toggle_->setChecked(s.enabled);
    mirror_toggle_->setChecked(s.mirror);
    if (camera_preview_)
        camera_preview_->setMirror(s.mirror);

    // Resolved so an empty configured id pre-selects the first real camera instead
    // of leaving the "(no camera)" placeholder selected.
    const std::string want = ResolveWebcamDeviceId(s.device_id, devices_);
    for (int i = 0; i < device_combo_->count(); ++i) {
        if (device_combo_->itemData(i).toString().toStdString() == want) {
            device_combo_->setCurrentIndex(i);
            break;
        }
    }

    refreshFormats();

    for (int i = 0; i < resolution_combo_->count(); ++i) {
        const auto d = resolution_combo_->itemData(i).toList();
        if (d.size() == 2 && d[0].toInt() == s.width && d[1].toInt() == s.height) {
            resolution_combo_->setCurrentIndex(i);
            break;
        }
    }

    // Overlay opacity.
    if (opacity_slider_) {
        opacity_slider_->setValue(static_cast<int>(std::lround(s.opacity * 100.0f)));
        if (opacity_value_label_)
            opacity_value_label_->setText(pct(opacity_slider_->value()));
    }

    // Chroma-key group.
    if (chroma_toggle_)
        chroma_toggle_->setChecked(s.chroma_key.enabled);
    if (chroma_body_)
        chroma_body_->setVisible(s.chroma_key.enabled);
    updateChromaColorButtons();
    updateChromaSwatch();
    if (tolerance_slider_) {
        tolerance_slider_->setValue(static_cast<int>(std::lround(s.chroma_key.tolerance * 100.0f)));
        if (tolerance_value_label_)
            tolerance_value_label_->setText(pct(tolerance_slider_->value()));
    }
    if (softness_slider_) {
        softness_slider_->setValue(static_cast<int>(std::lround(s.chroma_key.softness * 100.0f)));
        if (softness_value_label_)
            softness_value_label_->setText(pct(softness_slider_->value()));
    }
    if (spill_slider_) {
        spill_slider_->setValue(static_cast<int>(std::lround(s.chroma_key.spill_reduction * 100.0f)));
        if (spill_value_label_)
            spill_value_label_->setText(pct(spill_slider_->value()));
    }

    suppress_signals_ = false;

    if (isVisible())
        startPreview();
}

void WebcamSetupPanel::setControlsLocked(bool locked) {
    if (mf_unavailable_)
        return; // S4: MF absent — controls are already permanently disabled
    device_combo_->setEnabled(!locked);
    resolution_combo_->setEnabled(!locked);
    rescan_btn_->setEnabled(!locked);
    enable_toggle_->setEnabled(true);
    mirror_toggle_->setEnabled(true);
    // Opacity and chroma-key parameters are pushed live during recording via
    // RecorderSession::UpdateWebcamOverlay, so they stay editable while locked —
    // the same class as Mirror (only device/resolution/rescan are restart-class).
    if (opacity_slider_)
        opacity_slider_->setEnabled(true);
    if (chroma_toggle_)
        chroma_toggle_->setEnabled(true);
    for (auto* btn : {chroma_swatch_, chroma_green_btn_, chroma_blue_btn_, chroma_magenta_btn_, chroma_custom_btn_}) {
        if (btn)
            btn->setEnabled(true);
    }
    for (auto* slider : {tolerance_slider_, softness_slider_, spill_slider_}) {
        if (slider)
            slider->setEnabled(true);
    }
}

// S4: Gate the entire panel when mfplat.dll is absent at runtime.
void WebcamSetupPanel::setMfUnavailable(bool unavailable) {
    if (!unavailable)
        return;
    mf_unavailable_ = true;
    stopPreview();

    // Disable all controls.
    if (enable_toggle_)
        enable_toggle_->setEnabled(false);
    if (device_combo_)
        device_combo_->setEnabled(false);
    if (resolution_combo_)
        resolution_combo_->setEnabled(false);
    if (rescan_btn_)
        rescan_btn_->setEnabled(false);
    if (mirror_toggle_)
        mirror_toggle_->setEnabled(false);
    if (opacity_slider_)
        opacity_slider_->setEnabled(false);
    if (chroma_toggle_)
        chroma_toggle_->setEnabled(false);
    for (auto* btn : {chroma_swatch_, chroma_green_btn_, chroma_blue_btn_, chroma_magenta_btn_, chroma_custom_btn_}) {
        if (btn)
            btn->setEnabled(false);
    }
    for (auto* slider : {tolerance_slider_, softness_slider_, spill_slider_}) {
        if (slider)
            slider->setEnabled(false);
    }

    // Show a placeholder in the preview area.
    if (camera_preview_) {
        camera_preview_->clearFrame();
        camera_preview_->setPlaceholderText(
            QStringLiteral("Webcam unavailable\nInstall the Windows Media Feature Pack to enable."));
    }
}

void WebcamSetupPanel::onEnableToggled(bool enabled) {
    current_settings_.enabled = enabled;
    // Preview is coupled to the enable state; start/stop it in step with the toggle.
    if (isVisible())
        startPreview();
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onMirrorToggled(bool mirror) {
    current_settings_.mirror = mirror;
    if (camera_preview_)
        camera_preview_->setMirror(mirror);
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onOpacityChanged(int value) {
    if (opacity_value_label_)
        opacity_value_label_->setText(pct(value));
    current_settings_.opacity = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onChromaEnableToggled(bool enabled) {
    current_settings_.chroma_key.enabled = enabled;
    // Expand the group when enabled, collapse it when disabled (the header toggle
    // is the disclosure control).
    if (chroma_body_)
        chroma_body_->setVisible(enabled);
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onColorModeChanged(WebcamChromaKeyColorMode mode) {
    current_settings_.chroma_key.color_mode = mode;
    updateChromaColorButtons();
    updateChromaSwatch();
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onToleranceChanged(int value) {
    if (tolerance_value_label_)
        tolerance_value_label_->setText(pct(value));
    current_settings_.chroma_key.tolerance = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onSoftnessChanged(int value) {
    if (softness_value_label_)
        softness_value_label_->setText(pct(value));
    current_settings_.chroma_key.softness = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::onSpillReductionChanged(int value) {
    if (spill_value_label_)
        spill_value_label_->setText(pct(value));
    current_settings_.chroma_key.spill_reduction = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamSetupPanel::updateChromaColorButtons() {
    const auto mode = current_settings_.chroma_key.color_mode;
    if (chroma_green_btn_)
        chroma_green_btn_->setChecked(mode == WebcamChromaKeyColorMode::Green);
    if (chroma_blue_btn_)
        chroma_blue_btn_->setChecked(mode == WebcamChromaKeyColorMode::Blue);
    if (chroma_magenta_btn_)
        chroma_magenta_btn_->setChecked(mode == WebcamChromaKeyColorMode::Magenta);
    if (chroma_custom_btn_)
        chroma_custom_btn_->setChecked(mode == WebcamChromaKeyColorMode::Custom);
}

void WebcamSetupPanel::updateChromaSwatch() {
    if (!chroma_swatch_)
        return;
    const auto ac = current_settings_.chroma_key.active_color();
    const QColor fill(ac.r, ac.g, ac.b);
    // Inline stylesheet on this one widget only (dynamic colour): never touches the
    // theme QSS, so it cannot trip the ${token} start-up crash.
    const QString border = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line);
    chroma_swatch_->setStyleSheet(QStringLiteral("QPushButton#webcamPanelChromaSwatch {"
                                                 " background-color: %1; border: 1px solid %2;"
                                                 " border-radius: 4px; }")
                                      .arg(fill.name(), border));
}

void WebcamSetupPanel::onDeviceChanged(int) {
    refreshFormats();
    if (!suppress_signals_) {
        const WebcamSettings s = SanitizeWebcamSettings(collectSettings());
        const bool capture_changed = s.device_id != current_settings_.device_id;
        current_settings_ = s;
        emit settingsChanged(current_settings_);
        if (isVisible() && capture_changed)
            startPreview();
    }
}

void WebcamSetupPanel::onResolutionChanged(int) {
    if (suppress_signals_)
        return;
    const WebcamSettings s = SanitizeWebcamSettings(collectSettings());
    const bool capture_changed = s.width != current_settings_.width || s.height != current_settings_.height;
    current_settings_ = s;
    emit settingsChanged(current_settings_);
    if (isVisible() && capture_changed)
        startPreview();
}

void WebcamSetupPanel::onRescan() {
    // Prefer the canonical notifier path when MainWindow has connected rescanRequested().
    // That routes through WebcamDeviceNotifier::rescan() → snapshotChanged →
    // onWebcamDevicesChanged, which deduplicates devices and fires the same handler as
    // native plug/unplug events.
    // If the signal has no receivers (e.g. in unit tests without a wired MainWindow)
    // fall back to a direct local refresh.
    if (receivers(SIGNAL(rescanRequested())) > 0) {
        emit rescanRequested();
    } else {
        refreshDevices();
        if (isVisible())
            startPreview();
    }
}

void WebcamSetupPanel::onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap) {
    const std::string configured_id = current_settings_.device_id;

    // Rebuild the device combo while preserving the configured stable ID.
    // Use QSignalBlocker on both combos for the entire rebuild to avoid
    // re-entrancy issues with suppress_signals_ being reset by nested calls.
    {
        const QSignalBlocker db(device_combo_);
        const QSignalBlocker resb(resolution_combo_);

        device_combo_->clear();
        device_combo_->addItem(QStringLiteral("(no camera)"), QString{});
        devices_.clear();

        for (const auto& dev : snap.devices) {
            device_combo_->addItem(QString::fromStdString(dev.name), QString::fromStdString(dev.id));
            devices_.push_back(dev);
        }
    }

    // Try to restore the configured device.
    bool found = false;
    for (int i = 0; i < device_combo_->count(); ++i) {
        if (device_combo_->itemData(i).toString().toStdString() == configured_id) {
            {
                const QSignalBlocker db(device_combo_);
                device_combo_->setCurrentIndex(i);
            }
            found = true;
            break;
        }
    }

    if (!found && !configured_id.empty()) {
        // Device absent: stop preview, show unavailable placeholder, keep stored id.
        {
            const QSignalBlocker db(device_combo_);
            device_combo_->setCurrentIndex(0);
        }
        stopPreview();
        if (camera_preview_) {
            camera_preview_->clearFrame();
            camera_preview_->setPlaceholderText(
                QStringLiteral("Camera unavailable. Reconnect and click \xe2\x86\xba."));
        }
        // Do NOT modify current_settings_.device_id.
    } else if (found) {
        // Device present: refresh formats and restore preview per visibility rules.
        suppress_signals_ = true;
        refreshFormats();
        // Restore format selection.
        for (int i = 0; i < resolution_combo_->count(); ++i) {
            const auto d = resolution_combo_->itemData(i).toList();
            if (d.size() == 2 && d[0].toInt() == current_settings_.width && d[1].toInt() == current_settings_.height) {
                const QSignalBlocker resb(resolution_combo_);
                resolution_combo_->setCurrentIndex(i);
                break;
            }
        }
        suppress_signals_ = false;
        if (isVisible())
            startPreview();
    } else {
        // No configured device at all: just refresh formats for whatever is selected.
        suppress_signals_ = true;
        refreshFormats();
        suppress_signals_ = false;
    }
}

void WebcamSetupPanel::onPreviewFrame(QImage frame) {
    preview_frame_seen_ = true;
    if (watchdog_)
        watchdog_->stop();
    if (camera_preview_)
        camera_preview_->setFrame(std::move(frame));
}

void WebcamSetupPanel::refreshDevices() {
    suppress_signals_ = true;
    device_combo_->clear();
    device_combo_->addItem(QStringLiteral("(no camera)"), QString{});
    devices_ = WebcamService::EnumerateDevices();
    for (const auto& d : devices_)
        device_combo_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
    const std::string resolved = ResolveWebcamDeviceId(current_settings_.device_id, devices_);
    if (!resolved.empty()) {
        const int idx = device_combo_->findData(QString::fromStdString(resolved));
        if (idx >= 0)
            device_combo_->setCurrentIndex(idx);
    }
    suppress_signals_ = false;
    refreshFormats();
}

void WebcamSetupPanel::refreshFormats() {
    // Preserve the caller's suppression state instead of hard-resetting it: when
    // called from applySettings() (already suppressing for the whole call), a
    // hardcoded "false" here used to re-enable emissions partway through
    // applySettings(), letting the tail of that function leak a spurious
    // settingsChanged (dropping opacity, see collectSettings()) before
    // applySettings() even returns.
    const bool was_suppressed = suppress_signals_;
    suppress_signals_ = true;
    resolution_combo_->clear();
    const QString dev_id = device_combo_->currentData().toString();
    if (!dev_id.isEmpty()) {
        formats_ = WebcamService::EnumerateFormats(dev_id.toStdString());
        for (const auto& f : formats_) {
            const QString label =
                QStringLiteral("%1×%2 @ %3 fps").arg(f.width).arg(f.height).arg(f.fps_num / (std::max)(1, f.fps_den));
            QVariantList res_data = {f.width, f.height};
            resolution_combo_->addItem(label, res_data);
        }
        // #08: camera present — enable resolution combo.
        resolution_combo_->setEnabled(true);
    } else {
        // #08: no camera selected — disable and show placeholder text.
        resolution_combo_->addItem(QStringLiteral("(no camera)"), QVariant());
        resolution_combo_->setEnabled(false);
    }
    suppress_signals_ = was_suppressed;
}

void WebcamSetupPanel::startPreview() {
    // Visual-test mode drives the preview deterministically; never request capture.
    if (visual_test_mode_)
        return;
    if (watchdog_)
        watchdog_->stop();
    preview_frame_seen_ = false;
    current_settings_ = SanitizeWebcamSettings(current_settings_);

    const QString dev_id = device_combo_->currentData().toString();
    const bool has_device = !dev_id.isEmpty();
    // Coupled to the enable state: request the shared capture only when enabled AND a
    // device exists — never merely from the panel becoming visible. The panel does not
    // open its own reader; MainWindow relays this request to the coordinator, which owns
    // the single shared capture, and pushes frames back via setPreviewFrame().
    if (!ShouldOpenWebcamPreview(current_settings_.enabled, has_device)) {
        emit previewActiveRequested(false);
        if (camera_preview_) {
            camera_preview_->clearFrame();
            camera_preview_->setPlaceholderText(!has_device
                                                    ? QStringLiteral("No camera found.\nConnect a camera and click ↺.")
                                                    : QStringLiteral("Turn on the webcam to preview."));
        }
        return;
    }

    if (camera_preview_) {
        camera_preview_->clearFrame();
        camera_preview_->setPlaceholderText(QStringLiteral("Camera preview"));
    }

    emit previewActiveRequested(true);
    if (watchdog_)
        watchdog_->start();
}

void WebcamSetupPanel::stopPreview() {
    if (watchdog_)
        watchdog_->stop();
    emit previewActiveRequested(false);
    preview_frame_seen_ = false;
    if (camera_preview_)
        camera_preview_->clearFrame();
}

void WebcamSetupPanel::setPreviewFrame(const QImage& frame) {
    // A frame from the shared capture. Ignore it unless this panel currently wants a
    // preview for a selected device (avoids painting a stray frame after the user turns
    // the webcam off or while no device is chosen). CameraPreview applies the mirror.
    if (visual_test_mode_ || !camera_preview_ || !isVisible())
        return;
    const bool has_device = !device_combo_->currentData().toString().isEmpty();
    if (!ShouldOpenWebcamPreview(current_settings_.enabled, has_device))
        return;
    onPreviewFrame(frame);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void WebcamSetupPanel::applyVisualState(bool available, bool mirror, bool chroma_enabled,
                                        const QString& chroma_color_mode) {
    visual_test_mode_ = true;
    stopPreview();
    suppress_signals_ = true;

    device_combo_->clear();
    resolution_combo_->clear();
    mirror_toggle_->setChecked(mirror);
    if (camera_preview_)
        camera_preview_->setMirror(mirror);

    // Chroma-key group: deterministic enable + key-colour mode so the
    // settings-webcam-chroma-* scenarios render visibly distinct (group open with
    // the selected key colour, or collapsed when disabled).
    current_settings_.chroma_key.enabled = chroma_enabled;
    if (!chroma_color_mode.isEmpty()) {
        const auto mode = colorModeFromString(chroma_color_mode);
        current_settings_.chroma_key.color_mode = mode;
        if (mode == WebcamChromaKeyColorMode::Custom) {
            // A distinctive non-preset colour so the "custom" render is visibly
            // different from the green/blue swatches.
            current_settings_.chroma_key.custom_r = 255;
            current_settings_.chroma_key.custom_g = 140;
            current_settings_.chroma_key.custom_b = 0;
        }
    }
    if (chroma_toggle_)
        chroma_toggle_->setChecked(chroma_enabled);
    if (chroma_body_)
        chroma_body_->setVisible(chroma_enabled);
    updateChromaColorButtons();
    updateChromaSwatch();

    if (available) {
        device_combo_->addItem(QStringLiteral("Visual Test Camera"), QStringLiteral("visual-test-camera"));
        resolution_combo_->addItem(QStringLiteral("1280×720 @ 30 fps"), QVariantList{1280, 720});
        enable_toggle_->setChecked(true);
        if (camera_preview_) {
            // Asymmetric left/right halves so the mirror flip is visibly verifiable.
            QImage frame(320, 200, QImage::Format_ARGB32);
            for (int y = 0; y < frame.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(frame.scanLine(y));
                for (int x = 0; x < frame.width(); ++x)
                    row[x] = (x < frame.width() / 2) ? qRgb(220, 90, 80) : qRgb(80, 140, 220);
            }
            for (int y = 10; y < 40; ++y) {
                auto* row = reinterpret_cast<QRgb*>(frame.scanLine(y));
                for (int x = 10; x < 40; ++x)
                    row[x] = qRgb(245, 245, 245);
            }
            camera_preview_->setFrame(frame);
            camera_preview_->setToolTip(QStringLiteral("Synthetic visual-test camera frame"));
        }
    } else {
        device_combo_->addItem(QStringLiteral("(no visual-test camera)"), QString());
        enable_toggle_->setChecked(false);
        if (camera_preview_) {
            camera_preview_->clearFrame();
            camera_preview_->setPlaceholderText(QStringLiteral("VISUAL TEST: Camera unavailable"));
            camera_preview_->setToolTip(QStringLiteral("Deterministic visual-test unavailable state"));
        }
    }

    suppress_signals_ = false;
}
#endif

WebcamSettings WebcamSetupPanel::collectSettings() const {
    WebcamSettings s;
    s.enabled = enable_toggle_->isChecked();
    s.device_id = device_combo_->currentData().toString().toStdString();

    const auto res = resolution_combo_->currentData().toList();
    s.width = (res.size() >= 2) ? res[0].toInt() : 1280;
    s.height = (res.size() >= 2) ? res[1].toInt() : 720;
    s.fps = 30;

    s.mirror = mirror_toggle_->isChecked();

    // Overlay placement + aspect-lock have no UI here (they live on the Record
    // preview) — pass them through from what was last applied.
    s.overlay = current_settings_.overlay;
    s.overlay_user_placed = current_settings_.overlay_user_placed;
    s.aspect_ratio_locked = current_settings_.aspect_ratio_locked;

    // Overlay opacity from the slider.
    s.opacity = opacity_slider_ ? opacity_slider_->value() / 100.0f : current_settings_.opacity;

    // Chroma-key: on/off + parameters from the controls; colour mode and custom
    // RGB are tracked in current_settings_ (set by the colour-mode buttons).
    s.chroma_key.enabled = chroma_toggle_ ? chroma_toggle_->isChecked() : current_settings_.chroma_key.enabled;
    s.chroma_key.color_mode = current_settings_.chroma_key.color_mode;
    s.chroma_key.custom_r = current_settings_.chroma_key.custom_r;
    s.chroma_key.custom_g = current_settings_.chroma_key.custom_g;
    s.chroma_key.custom_b = current_settings_.chroma_key.custom_b;
    s.chroma_key.tolerance =
        tolerance_slider_ ? tolerance_slider_->value() / 100.0f : current_settings_.chroma_key.tolerance;
    s.chroma_key.softness =
        softness_slider_ ? softness_slider_->value() / 100.0f : current_settings_.chroma_key.softness;
    s.chroma_key.spill_reduction =
        spill_slider_ ? spill_slider_->value() / 100.0f : current_settings_.chroma_key.spill_reduction;

    return SanitizeWebcamSettings(s);
}

} // namespace exosnap::ui::widgets
