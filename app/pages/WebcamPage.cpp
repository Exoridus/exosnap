#include "WebcamPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/CameraPreview.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/ExoCheckBox.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/SectionRuleHeader.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "../visual_tests/VisualScenario.h"
#endif

#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace exosnap {

namespace {

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    lbl->setProperty("labelRole", role);
    return lbl;
}

QFrame* makeDivider(QWidget* parent) {
    auto* div = new QFrame(parent);
    div->setFrameShape(QFrame::HLine);
    div->setProperty("frameRole", "sectionRuleLine");
    return div;
}

QString pct(int v) {
    return QString::number(v) + "%";
}

} // namespace

WebcamPage::WebcamPage(QWidget* parent) : QWidget(parent) {
    auto* page_layout = new QHBoxLayout(this);
    page_layout->setContentsMargins(0, 0, 0, 0);
    page_layout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    content->setMaximumWidth(760);
    auto* layout = new QVBoxLayout(content);
    const int pad = ui::theme::ExoSnapMetrics::kSpaceXl;
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // ---- Detail header: ‹ Settings / Webcam Setup ----
    {
        auto* header = new QWidget(content);
        header->setObjectName(QStringLiteral("detailPageHeader"));
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        auto* back_btn = new QPushButton(QString::fromUtf8("\xe2\x80\xb9 Settings"), header);
        back_btn->setProperty("role", "back");
        back_btn->setCursor(Qt::PointingHandCursor);
        auto* crumb = new QLabel(QStringLiteral("/ Webcam Setup"), header);
        crumb->setProperty("labelRole", "detailBreadcrumb");
        hl->addWidget(back_btn);
        hl->addWidget(crumb);
        hl->addStretch(1);
        layout->addWidget(header);
        connect(back_btn, &QPushButton::clicked, this, &WebcamPage::backToSettingsRequested);
    }

    // ---- Camera preview card ----
    {
        auto* card = new QFrame(content);
        card->setProperty("panelRole", "panel");
        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                               ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
        cl->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

        cl->addWidget(makeLabel(QStringLiteral("Camera preview"), "videoKvKey", card));

        camera_preview_ = new ui::widgets::CameraPreview(card);
        cl->addWidget(camera_preview_);

        auto* setup_note = makeLabel(
            QStringLiteral("Preview is for setup only. Enable \xe2\x80\x9cInclude webcam in recording\xe2\x80\x9d"
                           " to include it in recordings."),
            "subtitle", card);
        setup_note->setWordWrap(true);
        cl->addWidget(setup_note);

        auto* privacy_note =
            makeLabel(QStringLiteral("Preview activates the selected camera while this page is open."), "muted", card);
        privacy_note->setWordWrap(true);
        cl->addWidget(privacy_note);

        layout->addWidget(card);
    }

    // ---- Setup card ----
    {
        auto* card = new QFrame(content);
        card->setProperty("panelRole", "panel");
        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                               ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
        cl->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

        // Enable toggle row
        auto* toggle_row = new QWidget(card);
        auto* tr = new QHBoxLayout(toggle_row);
        tr->setContentsMargins(0, 0, 0, 0);
        tr->setSpacing(12);
        tr->addWidget(makeLabel(QStringLiteral("Include webcam in recording"), "videoKvKey", toggle_row), 1);
        enable_toggle_ = new ui::widgets::ExoToggle(toggle_row);
        enable_toggle_->setChecked(false);
        tr->addWidget(enable_toggle_);
        cl->addWidget(toggle_row);

        cl->addWidget(makeDivider(card));

        // Device
        cl->addWidget(makeLabel(QStringLiteral("Device"), "videoKvKey", card));
        auto* dev_row = new QWidget(card);
        auto* dr = new QHBoxLayout(dev_row);
        dr->setContentsMargins(0, 0, 0, 0);
        dr->setSpacing(8);
        device_combo_ = new QComboBox(card);
        device_combo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        device_combo_->setMinimumWidth(280);
        device_combo_->setMaximumWidth(480);
        refresh_btn_ = new QPushButton(QStringLiteral("Rescan"), card);
        refresh_btn_->setProperty("role", "utility");
        refresh_btn_->setToolTip(QStringLiteral("Rescan for connected cameras"));
        dr->addWidget(device_combo_, 1);
        dr->addWidget(refresh_btn_);
        cl->addWidget(dev_row);

        // Resolution / FPS
        cl->addWidget(makeLabel(QStringLiteral("Resolution / FPS"), "videoKvKey", card));
        resolution_combo_ = new QComboBox(card);
        resolution_combo_->setMinimumWidth(280);
        resolution_combo_->setMaximumWidth(380);
        cl->addWidget(resolution_combo_);

        layout->addWidget(card);
    }

    // ---- Overlay Placement (not in MVP — widgets created for data binding, not added to layout) ----
    {
        auto addSliderRow = [&](const QString& label, QSlider*& slider, QLabel*& valueLabel, int defVal) {
            auto* row = new QWidget(content);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(8);
            rl->addWidget(makeLabel(label, "videoKvKey", row), 1);
            slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(0, 100);
            slider->setValue(defVal);
            slider->setFixedWidth(160);
            valueLabel = makeLabel(pct(defVal), "videoKvKey", row);
            valueLabel->setFixedWidth(36);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rl->addWidget(slider);
            rl->addWidget(valueLabel);
            // not added to layout — overlay placement not available in MVP
            row->hide();
        };

        addSliderRow(QStringLiteral("X Position"), pos_x_slider_, pos_x_label_, 0);
        addSliderRow(QStringLiteral("Y Position"), pos_y_slider_, pos_y_label_, 0);
        addSliderRow(QStringLiteral("Width"), size_w_slider_, size_w_label_, 25);
        addSliderRow(QStringLiteral("Height"), size_h_slider_, size_h_label_, 25);

        aspect_lock_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Lock aspect ratio"), content);
        aspect_lock_check_->setChecked(true);
        aspect_lock_check_->hide();
        // not added to layout
    }

    // ---- Chroma Key card ----
    {
        auto* card = new QFrame(content);
        card->setProperty("panelRole", "panel");
        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                               ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
        cl->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

        // Toggle row
        auto* toggle_row = new QWidget(card);
        auto* tr = new QHBoxLayout(toggle_row);
        tr->setContentsMargins(0, 0, 0, 0);
        tr->setSpacing(12);
        tr->addWidget(makeLabel(QStringLiteral("Chroma Key"), "videoKvKey", toggle_row), 1);
        chroma_toggle_ = new ui::widgets::ExoToggle(toggle_row);
        chroma_toggle_->setChecked(false);
        tr->addWidget(chroma_toggle_);
        cl->addWidget(toggle_row);

        cl->addWidget(makeDivider(card));

        // Color mode buttons
        cl->addWidget(makeLabel(QStringLiteral("Color Mode"), "videoKvKey", card));
        auto* mode_row = new QWidget(card);
        auto* mr = new QHBoxLayout(mode_row);
        mr->setContentsMargins(0, 0, 0, 0);
        mr->setSpacing(6);
        chroma_green_btn_ = new QPushButton(QStringLiteral("Green"), mode_row);
        chroma_blue_btn_ = new QPushButton(QStringLiteral("Blue"), mode_row);
        chroma_magenta_btn_ = new QPushButton(QStringLiteral("Magenta"), mode_row);
        chroma_custom_btn_ = new QPushButton(QStringLiteral("Custom..."), mode_row);
        for (auto* btn : {chroma_green_btn_, chroma_blue_btn_, chroma_magenta_btn_, chroma_custom_btn_}) {
            btn->setCheckable(true);
            btn->setProperty("role", "chromaModeBtn");
        }
        chroma_green_btn_->setChecked(true);
        mr->addWidget(chroma_green_btn_);
        mr->addWidget(chroma_blue_btn_);
        mr->addWidget(chroma_magenta_btn_);
        mr->addWidget(chroma_custom_btn_);
        mr->addStretch(1);
        cl->addWidget(mode_row);

        // Sliders: Tolerance, Softness, Spill Reduction
        auto addChromaSlider = [&](const QString& label, QSlider*& slider, QLabel*& valueLabel, int def) {
            auto* r = new QWidget(card);
            auto* rl2 = new QHBoxLayout(r);
            rl2->setContentsMargins(0, 0, 0, 0);
            rl2->setSpacing(8);
            rl2->addWidget(makeLabel(label, "videoKvKey", r), 1);
            slider = new QSlider(Qt::Horizontal, r);
            slider->setRange(0, 100);
            slider->setValue(def);
            slider->setFixedWidth(160);
            valueLabel = makeLabel(pct(def), "videoKvKey", r);
            valueLabel->setFixedWidth(36);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rl2->addWidget(slider);
            rl2->addWidget(valueLabel);
            cl->addWidget(r);
        };
        addChromaSlider(QStringLiteral("Tolerance"), tolerance_slider_, tolerance_label_, 40);
        addChromaSlider(QStringLiteral("Softness"), softness_slider_, softness_label_, 15);
        addChromaSlider(QStringLiteral("Spill Reduction"), spill_slider_, spill_label_, 30);

        layout->addWidget(card);
    }

    layout->addStretch(1);

    auto* centering_host = new QWidget();
    auto* ch_layout = new QHBoxLayout(centering_host);
    ch_layout->setContentsMargins(0, 0, 0, 0);
    ch_layout->addStretch(1);
    ch_layout->addWidget(content, 0);
    ch_layout->addStretch(1);
    scroll->setWidget(centering_host);
    page_layout->addWidget(scroll, 1);

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(device_combo_);
    combo_wheel_filter->installOn(resolution_combo_);

    // ---- Wire signals ----
    connect(enable_toggle_, &ui::widgets::ExoToggle::toggled, this, &WebcamPage::onEnableToggled);
    connect(device_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &WebcamPage::onDeviceChanged);
    connect(resolution_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &WebcamPage::applyCurrentSettings);
    connect(refresh_btn_, &QPushButton::clicked, this, &WebcamPage::onRefreshDevices);
    connect(chroma_toggle_, &ui::widgets::ExoToggle::toggled, this, &WebcamPage::onChromaEnableToggled);
    connect(chroma_green_btn_, &QPushButton::clicked, this,
            [this]() { onColorModeChanged(WebcamChromaKeyColorMode::Green); });
    connect(chroma_blue_btn_, &QPushButton::clicked, this,
            [this]() { onColorModeChanged(WebcamChromaKeyColorMode::Blue); });
    connect(chroma_magenta_btn_, &QPushButton::clicked, this,
            [this]() { onColorModeChanged(WebcamChromaKeyColorMode::Magenta); });
    connect(chroma_custom_btn_, &QPushButton::clicked, this, [this]() {
        const auto& ck = current_settings_.chroma_key;
        const QColor initial = (ck.color_mode == WebcamChromaKeyColorMode::Custom)
                                   ? QColor(ck.custom_r, ck.custom_g, ck.custom_b)
                                   : QColor(ck.active_color().r, ck.active_color().g, ck.active_color().b);
        const QColor picked = QColorDialog::getColor(initial, this, QStringLiteral("Pick Chroma Key Color"));
        if (!picked.isValid())
            return;
        current_settings_.chroma_key.custom_r = static_cast<uint8_t>(picked.red());
        current_settings_.chroma_key.custom_g = static_cast<uint8_t>(picked.green());
        current_settings_.chroma_key.custom_b = static_cast<uint8_t>(picked.blue());
        onColorModeChanged(WebcamChromaKeyColorMode::Custom);
    });
    connect(tolerance_slider_, &QSlider::valueChanged, this, &WebcamPage::onToleranceChanged);
    connect(softness_slider_, &QSlider::valueChanged, this, &WebcamPage::onSoftnessChanged);
    connect(spill_slider_, &QSlider::valueChanged, this, &WebcamPage::onSpillReductionChanged);

    // Overlay sliders are wired for programmatic sync (e.g. applySettings). Their rows are
    // not in the main layout so the user cannot interact with them directly.
    auto wireSlider = [this](QSlider* slider, QLabel* lbl) {
        connect(slider, &QSlider::valueChanged, this, [this, slider, lbl](int v) {
            lbl->setText(pct(v));
            if (!suppress_signals_) {
                const WebcamSettings s = collectSettings();
                current_settings_ = s;
                emit settingsChanged(s);
            }
        });
    };
    wireSlider(pos_x_slider_, pos_x_label_);
    wireSlider(pos_y_slider_, pos_y_label_);
    wireSlider(size_w_slider_, size_w_label_);
    wireSlider(size_h_slider_, size_h_label_);
    connect(aspect_lock_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool locked) {
        if (!suppress_signals_) {
            current_settings_ = collectSettings();
            current_settings_.aspect_ratio_locked = locked;
            emit settingsChanged(current_settings_);
        }
    });

    // Live frames arrive on the main thread (WebcamService marshals via the
    // event loop). Guard with a QPointer so a frame in flight after the page is
    // destroyed is safely dropped.
    preview_service_.SetFrameCallback([guard = QPointer<WebcamPage>(this)](QImage img) {
        if (guard)
            guard->onPreviewFrame(std::move(img));
    });

    // If no frame arrives shortly after starting, surface a non-technical hint.
    preview_watchdog_ = new QTimer(this);
    preview_watchdog_->setSingleShot(true);
    preview_watchdog_->setInterval(3000);
    connect(preview_watchdog_, &QTimer::timeout, this, [this]() {
        if (preview_frame_seen_ || !camera_preview_)
            return;
        camera_preview_->clearFrame();
        camera_preview_->setPlaceholderText(
            QStringLiteral("Camera preview unavailable.\nClose other apps that may be using the camera, "
                           "then click Rescan."));
    });

    // Initial device list.
    refreshDevices();
}

WebcamPage::~WebcamPage() {
    stopPreview();
}

void WebcamPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    if (visual_test_mode_)
        return;
#endif
    // Setup preview runs whenever the page is open and a camera is available —
    // independent of the "Include webcam in recording" toggle.
    startPreview();
}

void WebcamPage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    if (visual_test_mode_)
        return;
#endif
    // Release the camera while the page is hidden to avoid stale/frozen device state.
    stopPreview();
}

void WebcamPage::applySettings(const WebcamSettings& settings) {
    const WebcamSettings sanitized_settings = SanitizeWebcamSettings(settings);
    suppress_signals_ = true;
    current_settings_ = sanitized_settings;

    enable_toggle_->setChecked(sanitized_settings.enabled);
    if (camera_preview_)
        camera_preview_->setMirror(sanitized_settings.mirror);

    // Find matching device.
    for (int i = 0; i < device_combo_->count(); ++i) {
        if (device_combo_->itemData(i).toString().toStdString() == sanitized_settings.device_id) {
            device_combo_->setCurrentIndex(i);
            break;
        }
    }

    refreshFormats();

    // Find matching resolution.
    for (int i = 0; i < resolution_combo_->count(); ++i) {
        const auto d = resolution_combo_->itemData(i).toList();
        if (d.size() == 2 && d[0].toInt() == sanitized_settings.width && d[1].toInt() == sanitized_settings.height) {
            resolution_combo_->setCurrentIndex(i);
            break;
        }
    }

    if (pos_x_slider_)
        pos_x_slider_->setValue(static_cast<int>(sanitized_settings.overlay.x_norm * 100));
    if (pos_y_slider_)
        pos_y_slider_->setValue(static_cast<int>(sanitized_settings.overlay.y_norm * 100));
    if (size_w_slider_)
        size_w_slider_->setValue(static_cast<int>(sanitized_settings.overlay.w_norm * 100));
    if (size_h_slider_)
        size_h_slider_->setValue(static_cast<int>(sanitized_settings.overlay.h_norm * 100));
    if (aspect_lock_check_)
        aspect_lock_check_->setChecked(sanitized_settings.aspect_ratio_locked);

    if (chroma_toggle_)
        chroma_toggle_->setChecked(sanitized_settings.chroma_key.enabled);
    {
        const auto mode = sanitized_settings.chroma_key.color_mode;
        if (chroma_green_btn_)
            chroma_green_btn_->setChecked(mode == WebcamChromaKeyColorMode::Green);
        if (chroma_blue_btn_)
            chroma_blue_btn_->setChecked(mode == WebcamChromaKeyColorMode::Blue);
        if (chroma_magenta_btn_)
            chroma_magenta_btn_->setChecked(mode == WebcamChromaKeyColorMode::Magenta);
        if (chroma_custom_btn_)
            chroma_custom_btn_->setChecked(mode == WebcamChromaKeyColorMode::Custom);
    }
    if (tolerance_slider_)
        tolerance_slider_->setValue(static_cast<int>(sanitized_settings.chroma_key.tolerance * 100));
    if (softness_slider_)
        softness_slider_->setValue(static_cast<int>(sanitized_settings.chroma_key.softness * 100));
    if (spill_slider_)
        spill_slider_->setValue(static_cast<int>(sanitized_settings.chroma_key.spill_reduction * 100));

    suppress_signals_ = false;

    // Refresh the setup preview only while the page is actually shown; otherwise
    // showEvent() will start it when the page becomes visible.
    if (isVisible())
        startPreview();
}

void WebcamPage::setRecordingControlsLocked(bool locked) {
    device_combo_->setEnabled(!locked);
    resolution_combo_->setEnabled(!locked);
    refresh_btn_->setEnabled(!locked);

    enable_toggle_->setEnabled(true);
    if (pos_x_slider_)
        pos_x_slider_->setEnabled(true);
    if (pos_y_slider_)
        pos_y_slider_->setEnabled(true);
    if (size_w_slider_)
        size_w_slider_->setEnabled(true);
    if (size_h_slider_)
        size_h_slider_->setEnabled(true);
    if (aspect_lock_check_)
        aspect_lock_check_->setEnabled(true);
    if (chroma_toggle_)
        chroma_toggle_->setEnabled(true);
    if (chroma_green_btn_)
        chroma_green_btn_->setEnabled(true);
    if (chroma_blue_btn_)
        chroma_blue_btn_->setEnabled(true);
    if (chroma_magenta_btn_)
        chroma_magenta_btn_->setEnabled(true);
    if (chroma_custom_btn_)
        chroma_custom_btn_->setEnabled(true);
    if (tolerance_slider_)
        tolerance_slider_->setEnabled(true);
    if (softness_slider_)
        softness_slider_->setEnabled(true);
    if (spill_slider_)
        spill_slider_->setEnabled(true);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void WebcamPage::applyVisualState(visual::VisualWebcamState state) {
    visual_test_mode_ = true;
    stopPreview();
    suppress_signals_ = true;

    device_combo_->clear();
    resolution_combo_->clear();
    if (state == visual::VisualWebcamState::Active) {
        device_combo_->addItem(QStringLiteral("Visual Test Camera"), QStringLiteral("visual-test-camera"));
        resolution_combo_->addItem(QStringLiteral("1280×720 @ 30 fps"), QVariantList{1280, 720});
        enable_toggle_->setChecked(true);
        if (camera_preview_) {
            QImage frame(1280, 720, QImage::Format_RGB32);
            frame.fill(QColor(36, 48, 58));
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

void WebcamPage::onEnableToggled(bool enabled) {
    // This toggle only controls whether the webcam is included in recordings.
    // The setup preview is independent and keeps running while the page is open.
    current_settings_.enabled = enabled;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onDeviceChanged(int) {
    refreshFormats();
    applyCurrentSettings();
}

void WebcamPage::onResolutionChanged(int) {
    applyCurrentSettings();
}

void WebcamPage::onRefreshDevices() {
    // Prefer the canonical notifier path when MainWindow has connected rescanRequested().
    if (receivers(SIGNAL(rescanRequested())) > 0) {
        emit rescanRequested();
    } else {
        refreshDevices();
        if (isVisible())
            startPreview();
    }
}

void WebcamPage::onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap) {
    const std::string configured_id = current_settings_.device_id;

    // Use QSignalBlocker on both combos for the entire rebuild so that no
    // currentIndexChanged fires during the update (avoids re-entrancy issues
    // with the suppress_signals_ flag when refresh functions reset it mid-call).
    {
        const QSignalBlocker db(device_combo_);
        const QSignalBlocker rb(resolution_combo_);

        device_combo_->clear();
        device_combo_->addItem("(no camera)", QString());
        devices_.clear();

        for (const auto& dev : snap.devices) {
            device_combo_->addItem(QString::fromStdString(dev.name), QString::fromStdString(dev.id));
            devices_.push_back(dev);
        }
    }

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
        // Device absent: stop preview, show unavailable state, keep stored id.
        {
            const QSignalBlocker db(device_combo_);
            device_combo_->setCurrentIndex(0);
        }
        stopPreview();
        if (camera_preview_) {
            camera_preview_->clearFrame();
            camera_preview_->setPlaceholderText(QStringLiteral("Camera unavailable.\nReconnect and click Rescan."));
        }
        // Do NOT modify current_settings_.device_id.
    } else if (found) {
        // Device returned: refresh formats and restore preview per visibility rules.
        suppress_signals_ = true;
        refreshFormats();
        // Restore resolution selection.
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
        suppress_signals_ = true;
        refreshFormats();
        suppress_signals_ = false;
    }
}

void WebcamPage::onChromaEnableToggled(bool enabled) {
    current_settings_.chroma_key.enabled = enabled;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onToleranceChanged(int value) {
    tolerance_label_->setText(pct(value));
    current_settings_.chroma_key.tolerance = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onSoftnessChanged(int value) {
    softness_label_->setText(pct(value));
    current_settings_.chroma_key.softness = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onColorModeChanged(WebcamChromaKeyColorMode mode) {
    current_settings_.chroma_key.color_mode = mode;
    if (chroma_green_btn_)
        chroma_green_btn_->setChecked(mode == WebcamChromaKeyColorMode::Green);
    if (chroma_blue_btn_)
        chroma_blue_btn_->setChecked(mode == WebcamChromaKeyColorMode::Blue);
    if (chroma_magenta_btn_)
        chroma_magenta_btn_->setChecked(mode == WebcamChromaKeyColorMode::Magenta);
    if (chroma_custom_btn_)
        chroma_custom_btn_->setChecked(mode == WebcamChromaKeyColorMode::Custom);
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onSpillReductionChanged(int value) {
    spill_label_->setText(pct(value));
    current_settings_.chroma_key.spill_reduction = value / 100.0f;
    if (!suppress_signals_)
        emit settingsChanged(collectSettings());
}

void WebcamPage::onPreviewFrame(QImage frame) {
    preview_frame_seen_ = true;
    if (preview_watchdog_)
        preview_watchdog_->stop();
    if (camera_preview_)
        camera_preview_->setFrame(std::move(frame));
}

void WebcamPage::refreshDevices() {
    suppress_signals_ = true;
    device_combo_->clear();
    device_combo_->addItem("(no camera)", QString());
    devices_ = WebcamService::EnumerateDevices();
    for (const auto& d : devices_)
        device_combo_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
    suppress_signals_ = false;
    refreshFormats();
}

void WebcamPage::refreshFormats() {
    suppress_signals_ = true;
    resolution_combo_->clear();
    const QString dev_id = device_combo_->currentData().toString();
    if (!dev_id.isEmpty()) {
        formats_ = WebcamService::EnumerateFormats(dev_id.toStdString());
        for (const auto& f : formats_) {
            const QString label =
                QString("%1×%2 @ %3 fps").arg(f.width).arg(f.height).arg(f.fps_num / (std::max)(1, f.fps_den));
            QVariantList res_data = {f.width, f.height};
            resolution_combo_->addItem(label, res_data);
        }
    }
    suppress_signals_ = false;
}

void WebcamPage::applyCurrentSettings() {
    if (suppress_signals_)
        return;
    const WebcamSettings new_settings = SanitizeWebcamSettings(collectSettings());
    const bool capture_changed =
        new_settings.device_id != current_settings_.device_id || new_settings.width != current_settings_.width ||
        new_settings.height != current_settings_.height || new_settings.fps != current_settings_.fps;
    current_settings_ = new_settings;
    emit settingsChanged(current_settings_);
    // Restart the setup preview when the capture device/format changes while the
    // page is visible — independent of the recording-inclusion toggle.
    if (isVisible() && capture_changed)
        startPreview();
}

void WebcamPage::startPreview() {
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    if (visual_test_mode_)
        return;
#endif
    current_settings_ = SanitizeWebcamSettings(current_settings_);
    if (preview_watchdog_)
        preview_watchdog_->stop();
    preview_frame_seen_ = false;

    const QString dev_id = device_combo_->currentData().toString();
    const bool has_device = !dev_id.isEmpty() || !devices_.empty();
    if (!has_device) {
        preview_service_.Stop();
        if (camera_preview_) {
            camera_preview_->clearFrame();
            camera_preview_->setPlaceholderText(QStringLiteral("No camera found. Connect a camera and click Rescan."));
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
    if (preview_watchdog_)
        preview_watchdog_->start();
}

void WebcamPage::stopPreview() {
    if (preview_watchdog_)
        preview_watchdog_->stop();
    preview_service_.Stop();
    preview_frame_seen_ = false;
    if (camera_preview_)
        camera_preview_->clearFrame();
}

WebcamSettings WebcamPage::collectSettings() const {
    WebcamSettings s;
    s.enabled = enable_toggle_->isChecked();
    s.device_id = device_combo_->currentData().toString().toStdString();

    const auto res = resolution_combo_->currentData().toList();
    s.width = (res.size() >= 2) ? res[0].toInt() : 1280;
    s.height = (res.size() >= 2) ? res[1].toInt() : 720;
    s.fps = 30;

    s.overlay.x_norm = pos_x_slider_ ? pos_x_slider_->value() / 100.0f : current_settings_.overlay.x_norm;
    s.overlay.y_norm = pos_y_slider_ ? pos_y_slider_->value() / 100.0f : current_settings_.overlay.y_norm;
    s.overlay.w_norm = size_w_slider_ ? size_w_slider_->value() / 100.0f : current_settings_.overlay.w_norm;
    s.overlay.h_norm = size_h_slider_ ? size_h_slider_->value() / 100.0f : current_settings_.overlay.h_norm;
    s.overlay_user_placed = current_settings_.overlay_user_placed;
    s.aspect_ratio_locked =
        aspect_lock_check_ ? aspect_lock_check_->isChecked() : current_settings_.aspect_ratio_locked;

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

} // namespace exosnap
