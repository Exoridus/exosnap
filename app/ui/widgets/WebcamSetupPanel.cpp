#include "WebcamSetupPanel.h"

#include "CameraPreview.h"
#include "ComboBoxWheelFilter.h"
#include "ExoToggle.h"

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

namespace {

QFrame* makeHairline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("frameRole", "sectionRuleLine");
    return line;
}

QLabel* makeNote(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", "muted");
    label->setWordWrap(true);
    return label;
}

} // namespace

WebcamSetupPanel::WebcamSetupPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("webcamSetupPanel"));

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    // ── Left column: compact live preview ────────────────────────────────────
    camera_preview_ = new CameraPreview(this);
    // Override the standalone-page size: compact 280×175 (≈16:10) for Settings embed.
    camera_preview_->setFixedHeight(175);
    camera_preview_->setMaximumWidth(300);
    camera_preview_->setMinimumWidth(180);
    root->addWidget(camera_preview_, 0, Qt::AlignTop);

    // ── Right column: controls ────────────────────────────────────────────────
    auto* right_col = new QWidget(this);
    auto* right_layout = new QVBoxLayout(right_col);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(10);

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

    right_layout->addWidget(makeHairline(right_col));

    // Placement note (no controls here)
    right_layout->addWidget(
        makeNote(QStringLiteral("Position and size are configured in the Record preview."), right_col));

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

    // Preview frame callback: marshals to main thread via the Qt event loop.
    preview_service_.SetFrameCallback([guard = QPointer<WebcamSetupPanel>(this)](QImage img) {
        if (guard)
            guard->onPreviewFrame(std::move(img));
    });

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
    stopPreview();
}

void WebcamSetupPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
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

    for (int i = 0; i < device_combo_->count(); ++i) {
        if (device_combo_->itemData(i).toString().toStdString() == s.device_id) {
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

    suppress_signals_ = false;

    if (isVisible())
        startPreview();
}

void WebcamSetupPanel::setControlsLocked(bool locked) {
    device_combo_->setEnabled(!locked);
    resolution_combo_->setEnabled(!locked);
    rescan_btn_->setEnabled(!locked);
    enable_toggle_->setEnabled(true);
    mirror_toggle_->setEnabled(true);
}

void WebcamSetupPanel::onEnableToggled(bool enabled) {
    current_settings_.enabled = enabled;
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
    suppress_signals_ = false;
    refreshFormats();
}

void WebcamSetupPanel::refreshFormats() {
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
    suppress_signals_ = false;
}

void WebcamSetupPanel::startPreview() {
    // Visual-test mode drives the preview deterministically; never open a real device.
    if (visual_test_mode_)
        return;
    if (watchdog_)
        watchdog_->stop();
    preview_frame_seen_ = false;
    current_settings_ = SanitizeWebcamSettings(current_settings_);

    const QString dev_id = device_combo_->currentData().toString();
    if (dev_id.isEmpty()) {
        preview_service_.Stop();
        if (camera_preview_) {
            camera_preview_->clearFrame();
            camera_preview_->setPlaceholderText(QStringLiteral("No camera found.\nConnect a camera and click ↺."));
        }
        return;
    }

    const auto combo_data = resolution_combo_->currentData().toList();
    const int w = (combo_data.size() >= 2) ? combo_data[0].toInt() : current_settings_.width;
    const int h = (combo_data.size() >= 2) ? combo_data[1].toInt() : current_settings_.height;

    if (camera_preview_) {
        camera_preview_->clearFrame();
        camera_preview_->setPlaceholderText(QStringLiteral("Camera preview"));
    }

    preview_service_.Stop();
    preview_service_.Start(dev_id.toStdString(), w > 0 ? w : 1280, h > 0 ? h : 720, 30);
    if (watchdog_)
        watchdog_->start();
}

void WebcamSetupPanel::stopPreview() {
    if (watchdog_)
        watchdog_->stop();
    preview_service_.Stop();
    preview_frame_seen_ = false;
    if (camera_preview_)
        camera_preview_->clearFrame();
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void WebcamSetupPanel::applyVisualState(bool available, bool mirror) {
    visual_test_mode_ = true;
    stopPreview();
    suppress_signals_ = true;

    device_combo_->clear();
    resolution_combo_->clear();
    mirror_toggle_->setChecked(mirror);
    if (camera_preview_)
        camera_preview_->setMirror(mirror);

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

    // Preserve overlay and chroma from current (panel does not expose these controls).
    s.overlay = current_settings_.overlay;
    s.overlay_user_placed = current_settings_.overlay_user_placed;
    s.aspect_ratio_locked = current_settings_.aspect_ratio_locked;
    s.chroma_key = current_settings_.chroma_key;

    return SanitizeWebcamSettings(s);
}

} // namespace exosnap::ui::widgets
